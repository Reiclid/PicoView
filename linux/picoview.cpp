// PicoView for Wayland - stage one.
//
// This is not a port of the Windows program. It is a second front end over the
// same core: the parts that decide what something looks like (src/core) are
// shared, and everything below them is written twice, once against Win32 and
// Direct2D and once against Wayland. That split is the whole point, and this
// file is the proof that the core really is free of Windows.
//
// What stage one does: opens a window on any wl_compositor with xdg-shell,
// shows a picture, walks the folder it is in, zooms, pans, goes full screen,
// and draws the same generated artwork for a music file that the Windows build
// draws - from the same code, so the same track gets the same picture.
//
// What it deliberately does not do yet, and why:
//
//   - No shaping. FreeType draws the glyphs and fontconfig finds the font, but
//     the string is walked a character at a time. Latin and Cyrillic do not
//     mind; the day an Arabic file name turns up, HarfBuzz goes in text.cpp.
//   - No GPU. Everything is composited on the processor into a wl_shm buffer.
//     A still picture does not need a Vulkan context, and not having one keeps
//     this to a single dependency-light binary. Stage two is where that
//     changes, and the scaler here is already threaded so the shape of the
//     work does not have to.
//   - No video and no sound. That is libmpv, and it is its own stage.
//
// Build:  meson setup build && meson compile -C build
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

// stb reads the file itself here; on Windows the program already has the bytes
// in hand, which is why that build passes memory instead.
#define STB_IMAGE_IMPLEMENTATION
#include "../src/third_party/stb_image.h"

#include "../src/core/coverplan.h"
#include "paint.h"
#include "text.h"
#include "video.h"

#include <poll.h>

// ------------------------------------------------------------------ state
namespace {

struct Image {
    std::vector<uint8_t> px;          // premultiplied BGRA, top row first
    int w = 0, h = 0;
    bool ok = false;
};

struct Buffer {
    wl_buffer* buf = nullptr;
    uint8_t*   data = nullptr;
    size_t     size = 0;
    int        w = 0, h = 0;
    bool       busy = false;
};

struct App {
    // ---- wayland
    wl_display*    display = nullptr;
    wl_registry*   registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm*        shm = nullptr;
    wl_seat*       seat = nullptr;
    wl_keyboard*   keyboard = nullptr;
    wl_pointer*    pointer = nullptr;
    xdg_wm_base*   wmBase = nullptr;
    zxdg_decoration_manager_v1* decoMgr = nullptr;

    wl_surface*    surface = nullptr;
    xdg_surface*   xsurface = nullptr;
    xdg_toplevel*  toplevel = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;

    xkb_context*   xkb = nullptr;
    xkb_keymap*    keymap = nullptr;
    xkb_state*     xkbState = nullptr;
    bool           ctrlDown = false;
    bool           shiftDown = false;

    Buffer         buffers[2];
    int            width = 1100, height = 700;
    bool           configured = false;
    bool           running = true;
    bool           dirty = true;
    bool           fullscreen = false;

    // ---- interface
    pv::Font*      fBody = nullptr;
    pv::Font*      fSmall = nullptr;
    bool           pointerIn = false;    // the bar is there when the pointer is
    int            hot = 0, pressed = 0;

    // ---- pointer
    double         mouseX = 0, mouseY = 0;
    bool           dragging = false;
    double         dragFromX = 0, dragFromY = 0;
    float          dragPanX = 0, dragPanY = 0;

    // ---- content
    std::vector<std::string> files;
    int            index = 0;
    std::string    folder;
    Image          image;
    pv::Video      video;
    bool           seekDrag = false;
    float          zoom = 1.f;
    bool           fitted = true;       // zoom follows the window until touched
    float          panX = 0, panY = 0;
};

App g;

// ------------------------------------------------------------------ files
bool endsWithLower(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[s.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

bool isPicture(const std::string& s) {
    static const char* ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga",
                                 ".psd", ".hdr", ".pnm", ".ppm", ".pgm", ".jfif", nullptr };
    for (int i = 0; ext[i]; ++i)
        if (endsWithLower(s, ext[i])) return true;
    return false;
}

bool isMusic(const std::string& s) { return pv::isAudioName(s); }
bool isFilm(const std::string& s) { return pv::isVideoName(s); }

std::string dirOf(const std::string& path) {
    size_t at = path.find_last_of('/');
    return at == std::string::npos ? std::string(".") : path.substr(0, at);
}
std::string nameOf(const std::string& path) {
    size_t at = path.find_last_of('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}

// Everything in the folder this program is willing to show, in name order, so
// the arrow keys walk it the same way the Windows build does.
void scanFolder(const std::string& dir, const std::string& select) {
    g.files.clear();
    g.folder = dir;
    DIR* d = opendir(dir.c_str());
    if (d) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string name = e->d_name;
            if (!isPicture(name) && !isMusic(name) && !isFilm(name)) continue;
            std::string full = dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            g.files.push_back(full);
        }
        closedir(d);
    }
    std::sort(g.files.begin(), g.files.end());
    g.index = 0;
    for (size_t i = 0; i < g.files.size(); ++i)
        if (g.files[i] == select) { g.index = (int)i; break; }
}

// ------------------------------------------------------------------ images
Image loadPicture(const std::string& path) {
    Image im;
    int w = 0, h = 0, comp = 0;
    stbi_uc* p = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!p) return im;
    im.w = w;
    im.h = h;
    im.px.resize((size_t)w * h * 4);
    // RGBA straight, to BGRA premultiplied: that is what wl_shm's ARGB8888
    // wants and what the core rasteriser already produces.
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        uint8_t r = p[i * 4 + 0], gg = p[i * 4 + 1], b = p[i * 4 + 2], a = p[i * 4 + 3];
        im.px[i * 4 + 0] = (uint8_t)(b * a / 255);
        im.px[i * 4 + 1] = (uint8_t)(gg * a / 255);
        im.px[i * 4 + 2] = (uint8_t)(r * a / 255);
        im.px[i * 4 + 3] = a;
    }
    stbi_image_free(p);
    im.ok = true;
    return im;
}

// A music file has no picture, so it gets the one the core makes up for it -
// byte for byte what the Windows build shows for the same track.
Image loadCover(const std::string& path) {
    Image im;
    std::string name = nameOf(path);
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    pv::CoverPlan plan;
    pv::coverPlanFromSeed(pv::coverSeedBytes(name.data(), name.size()), plan);

    const int side = 512;
    im.w = im.h = side;
    im.px.assign((size_t)side * side * 4, 0);
    pv::coverRasterBGRA(plan, side, im.px.data(), side * 4);
    im.ok = true;
    return im;
}

void openAt(int index) {
    if (g.files.empty()) return;
    if (index < 0) index = (int)g.files.size() - 1;
    if (index >= (int)g.files.size()) index = 0;
    g.index = index;
    const std::string& path = g.files[index];

    g.video.close();
    g.image = Image();
    if (isFilm(path) || isMusic(path)) {
        // Both go to mpv: a music file is a video with nothing to look at, and
        // the picture it gets instead is the one the shared core makes up.
        if (!g.video.open(path))
            fprintf(stderr, "picoview: %s\n", g.video.error().c_str());
        if (isMusic(path)) g.image = loadCover(path);
    } else {
        g.image = loadPicture(path);
        if (!g.image.ok)
            fprintf(stderr, "picoview: cannot read %s\n", path.c_str());
    }
    g.fitted = true;
    g.panX = g.panY = 0;
    g.dirty = true;

    std::string title = "PicoView - " + nameOf(path);
    if (g.toplevel) xdg_toplevel_set_title(g.toplevel, title.c_str());
}

// ------------------------------------------------------------------ drawing
float fitZoomFor(int w, int h) {
    if (!g.image.ok || g.image.w <= 0 || g.image.h <= 0) return 1.f;
    float z = std::min((float)w / g.image.w, (float)h / g.image.h);
    return std::min(z, 1.f);         // never blow a small picture up to fit
}
float fitZoom() { return fitZoomFor(g.width, g.height); }

void clampPan() {
    float z = g.fitted ? fitZoom() : g.zoom;
    float dw = g.image.w * z, dh = g.image.h * z;
    float slackX = std::max(0.f, (dw - g.width) * 0.5f);
    float slackY = std::max(0.f, (dh - g.height) * 0.5f);
    g.panX = std::max(-slackX, std::min(slackX, g.panX));
    g.panY = std::max(-slackY, std::min(slackY, g.panY));
}

// ------------------------------------------------------------------ the bar
// Immediate mode, like the Windows build: the layout is computed twice, once
// to draw and once to hit test, and there is no widget tree to keep in step
// with anything.
enum {
    B_NONE = 0, B_PREV, B_NEXT, B_ZOUT, B_ZIN, B_FIT, B_ACTUAL, B_FULL, B_PLAY
};

struct Btn {
    pv::Rect r;
    int      id = B_NONE;
    bool     on = false;             // drawn as held down
};

// Colours, straight out of the Windows build's dark theme so the two look
// like the same program.
const pv::Color kCanvas = pv::rgba(27, 27, 29);
const pv::Color kBar = pv::rgba(38, 38, 41, 0.94f);
const pv::Color kBarEdge = pv::rgba(255, 255, 255, 0.09f);
const pv::Color kText = pv::rgba(240, 240, 243);
const pv::Color kTextDim = pv::rgba(240, 240, 243, 0.62f);
const pv::Color kHover = pv::rgba(255, 255, 255, 0.10f);
const pv::Color kHeld = pv::rgba(255, 255, 255, 0.18f);
const pv::Color kAccent = pv::rgba(96, 165, 250);

float fitZoomFor(int w, int h);

std::vector<Btn> layoutBar(int W, int H, pv::Rect& barOut, pv::Rect& pctOut) {
    const float bs = 40, gap = 4, pad = 10, pct = 62, sep = 10;

    // Laid out from zero first and centred afterwards. Working out the width
    // with a formula and then laying the buttons out separately is how the
    // last button ended up outside the bar: two descriptions of one thing.
    std::vector<Btn> v;
    float cx = 0;
    auto slot = [&](int id) {
        v.push_back(Btn{ pv::Rect{ cx, 0, bs, bs }, id, false });
        cx += bs + gap;
    };
    slot(B_PREV);
    slot(B_NEXT);
    if (g.video.isOpen()) {
        cx += sep;
        slot(B_PLAY);
    }
    cx += sep;
    slot(B_ZOUT);
    pctOut = pv::Rect{ cx, 0, pct, bs };
    cx += pct + gap;
    slot(B_ZIN);
    cx += sep;
    slot(B_FIT);
    slot(B_ACTUAL);
    cx += sep;
    slot(B_FULL);

    float content = cx - gap;                 // the last slot leaves one behind
    float width = content + pad * 2;
    float x = (W - width) * 0.5f;
    float y = H - bs - pad * 2 - 14;
    barOut = pv::Rect{ x, y, width, bs + pad * 2 };

    float ox = x + pad, oy = y + pad;
    for (Btn& b : v) { b.r.x += ox; b.r.y += oy; }
    pctOut.x += ox;
    pctOut.y += oy;
    return v;
}

// The icons are drawn rather than typed: an icon font that is on every Linux
// machine does not exist, and four triangles are cheaper than shipping one.
void drawIcon(pv::Canvas& c, int id, pv::Rect r, pv::Color col) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float k = r.h * 0.26f;                  // the icons all live in this box
    switch (id) {
        case B_PREV:
            pv::fillTri(c, cx + k * 0.5f, cy - k, cx + k * 0.5f, cy + k, cx - k * 0.7f, cy, col);
            break;
        case B_NEXT:
            pv::fillTri(c, cx - k * 0.5f, cy - k, cx - k * 0.5f, cy + k, cx + k * 0.7f, cy, col);
            break;
        case B_ZOUT:
            pv::fillRound(c, pv::Rect{ cx - k, cy - 1.f, k * 2, 2.f }, 1.f, col);
            break;
        case B_ZIN:
            pv::fillRound(c, pv::Rect{ cx - k, cy - 1.f, k * 2, 2.f }, 1.f, col);
            pv::fillRound(c, pv::Rect{ cx - 1.f, cy - k, 2.f, k * 2 }, 1.f, col);
            break;
        case B_FIT:
            pv::strokeRound(c, pv::Rect{ cx - k, cy - k * 0.8f, k * 2, k * 1.6f }, 2.f, 1.6f, col);
            break;
        case B_ACTUAL: {
            // a square with a square in it: "one pixel is one pixel"
            pv::strokeRound(c, pv::Rect{ cx - k, cy - k, k * 2, k * 2 }, 2.f, 1.6f, col);
            pv::fillRound(c, pv::Rect{ cx - k * 0.3f, cy - k * 0.3f, k * 0.6f, k * 0.6f }, 1.f, col);
            break;
        }
        case B_PLAY: {
            if (g.video.paused()) {
                pv::fillTri(c, cx - k * 0.55f, cy - k, cx - k * 0.55f, cy + k,
                            cx + k * 0.8f, cy, col);
            } else {
                float bw = k * 0.42f;
                pv::fillRound(c, pv::Rect{ cx - k * 0.62f, cy - k, bw, k * 2 }, 1.f, col);
                pv::fillRound(c, pv::Rect{ cx + k * 0.2f, cy - k, bw, k * 2 }, 1.f, col);
            }
            break;
        }
        case B_FULL: {
            float t = 1.8f, len = k * 0.85f;
            for (int i = 0; i < 4; ++i) {
                float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
                float ox = cx + sx * k, oy = cy + sy * k;
                pv::fillRound(c, pv::Rect{ sx > 0 ? ox - len : ox, oy - t * 0.5f, len, t }, 1.f, col);
                pv::fillRound(c, pv::Rect{ ox - t * 0.5f, sy > 0 ? oy - len : oy, t, len }, 1.f, col);
            }
            break;
        }
        default: break;
    }
}

void drawBar(pv::Canvas& c, int W, int H) {
    if (!g.pointerIn) return;

    pv::Rect bar, pct;
    std::vector<Btn> btns = layoutBar(W, H, bar, pct);

    pv::dropShadow(c, bar, bar.h * 0.5f, 10.f, 0.55f);
    pv::fillRound(c, bar, bar.h * 0.5f, kBar);
    pv::strokeRound(c, bar, bar.h * 0.5f, 1.f, kBarEdge);

    for (const Btn& b : btns) {
        bool held = (g.pressed == b.id);
        bool hot = (g.hot == b.id);
        if (held)     pv::fillRound(c, b.r, b.r.h * 0.5f, kHeld);
        else if (hot) pv::fillRound(c, b.r, b.r.h * 0.5f, kHover);
        bool lit = (b.id == B_FIT && g.fitted) ||
                   (b.id == B_FULL && g.fullscreen) ||
                   (b.id == B_ACTUAL && !g.fitted && fabsf(g.zoom - 1.f) < 0.001f);
        drawIcon(c, b.id, b.r, lit ? kAccent : kText);
    }

    char buf[32];
    float z = g.fitted ? fitZoomFor(W, H) : g.zoom;
    snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(z * 100.f));
    pv::drawTextIn(c, g.fBody, buf, pct, kTextDim, pv::Align::Centre);
}

// Where the seek bar lives, so drawing and clicking agree about it.
pv::Rect seekRect(int W, int H) {
    pv::Rect bar, pct;
    layoutBar(W, H, bar, pct);
    return pv::Rect{ bar.x + 16, bar.y - 22, bar.w - 32, 14 };
}

std::string clockOf(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    int t = (int)(seconds + 0.0001);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, (t % 3600) / 60, t % 60);
    else           snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    return b;
}

void drawSeek(pv::Canvas& c, int W, int H) {
    if (!g.pointerIn || !g.video.isOpen()) return;
    double dur = g.video.duration();
    if (!(dur > 0)) return;

    pv::Rect r = seekRect(W, H);
    pv::Rect track{ r.x, r.y + r.h * 0.5f - 2.f, r.w, 4.f };

    pv::dropShadow(c, pv::Rect{ r.x - 10, r.y - 6, r.w + 20, r.h + 12 }, 10.f, 8.f, 0.4f);
    pv::fillRound(c, pv::Rect{ r.x - 10, r.y - 6, r.w + 20, r.h + 12 }, 10.f, kBar);

    float t = (float)(g.video.position() / dur);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    pv::fillRound(c, track, 2.f, pv::rgba(255, 255, 255, 0.22f));
    pv::fillRound(c, pv::Rect{ track.x, track.y, track.w * t, track.h }, 2.f, kAccent);
    pv::fillRound(c, pv::Rect{ track.x + track.w * t - 5.f, r.y + r.h * 0.5f - 5.f, 10.f, 10.f },
                  5.f, kText);

    std::string now = clockOf(g.video.position()) + "  /  " + clockOf(dur);
    pv::Rect tb{ r.x, r.y - 20, r.w, 18 };
    pv::drawTextIn(c, g.fSmall, now.c_str(), tb, kTextDim, pv::Align::Centre);
}

void drawCaption(pv::Canvas& c, int W) {
    if (!g.pointerIn || g.files.empty()) return;

    std::string name = nameOf(g.files[g.index]);
    char meta[96];
    if (g.image.ok)
        snprintf(meta, sizeof(meta), "%d x %d   %d/%d", g.image.w, g.image.h,
                 g.index + 1, (int)g.files.size());
    else
        snprintf(meta, sizeof(meta), "%d/%d", g.index + 1, (int)g.files.size());

    float nameW = pv::textWidth(g.fBody, name.c_str());
    float metaW = pv::textWidth(g.fSmall, meta);
    float gap = 16, pad = 14, h = 34;
    float w = std::min((float)W - 40.f, pad * 2 + nameW + gap + metaW);
    pv::Rect pill{ (W - w) * 0.5f, 12, w, h };

    pv::dropShadow(c, pill, h * 0.5f, 8.f, 0.5f);
    pv::fillRound(c, pill, h * 0.5f, kBar);
    pv::strokeRound(c, pill, h * 0.5f, 1.f, kBarEdge);

    pv::Rect nb{ pill.x + pad, pill.y, nameW, h };
    pv::drawTextIn(c, g.fBody, name.c_str(), nb, kText);
    pv::Rect mb{ pill.right() - pad - metaW, pill.y, metaW, h };
    pv::drawTextIn(c, g.fSmall, meta, mb, kTextDim);
}

// ------------------------------------------------------------------ frame
void paint(Buffer& b) {
    pv::Canvas c;
    c.px = b.data;
    c.w = b.w;
    c.h = b.h;
    c.stride = b.w * 4;
    c.reset();

    pv::clear(c, kCanvas);

    // A film fills the window, the way a player does, and mpv puts the bars on
    // for us. Zoom and pan belong to pictures.
    if (g.video.hasVideo()) {
        g.video.render(b.data, b.w, b.h, b.w * 4);
    } else if (g.image.ok) {
        float z = g.fitted ? fitZoomFor(b.w, b.h) : g.zoom;
        float dw = std::max(1.f, g.image.w * z);
        float dh = std::max(1.f, g.image.h * z);
        pv::Bitmap src{ g.image.px.data(), g.image.w, g.image.h, g.image.w * 4 };
        pv::blit(c, src,
                 pv::Rect{ (b.w - dw) * 0.5f + g.panX, (b.h - dh) * 0.5f + g.panY, dw, dh },
                 z < 0.999f || z > 1.001f);
    }

    drawCaption(c, b.w);
    drawSeek(c, b.w, b.h);
    drawBar(c, b.w, b.h);
}

// ------------------------------------------------------------------ buffers
void bufferRelease(void* data, wl_buffer*) {
    ((Buffer*)data)->busy = false;
}
const wl_buffer_listener kBufferListener = { bufferRelease };

bool makeBuffer(Buffer& b, int w, int h) {
    if (b.buf) {
        wl_buffer_destroy(b.buf);
        b.buf = nullptr;
    }
    if (b.data) {
        munmap(b.data, b.size);
        b.data = nullptr;
    }
    b.size = (size_t)w * h * 4;
    b.w = w;
    b.h = h;

    int fd = memfd_create("picoview", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)b.size) < 0) { close(fd); return false; }
    b.data = (uint8_t*)mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b.data == MAP_FAILED) { b.data = nullptr; close(fd); return false; }

    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, (int32_t)b.size);
    b.buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b.buf) return false;
    wl_buffer_add_listener(b.buf, &kBufferListener, &b);
    b.busy = false;
    return true;
}

void redraw() {
    if (!g.configured || !g.surface) return;

    Buffer* b = nullptr;
    for (auto& cand : g.buffers)
        if (!cand.busy) { b = &cand; break; }
    if (!b) return;                      // both still held; the next frame will do

    if (b->w != g.width || b->h != g.height || !b->data) {
        if (!makeBuffer(*b, g.width, g.height)) return;
    }
    clampPan();
    paint(*b);

    // A way to see exactly what was composited, rather than measuring a
    // screenshot with a ruler: PV_DUMP=/tmp/x.ppm writes the first frame out.
    if (const char* dump = getenv("PV_DUMP")) {
        static bool once = false;
        // Not the very first frame: that one is painted in answer to the
        // compositor's configure, before anything has been opened.
        if (!once && g.image.ok) {
            once = true;
            if (FILE* f = fopen(dump, "wb")) {
                fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
                for (int y = 0; y < b->h; ++y) {
                    const uint8_t* row = b->data + (size_t)y * b->w * 4;
                    for (int x = 0; x < b->w; ++x) {
                        fputc(row[x * 4 + 2], f);   // R
                        fputc(row[x * 4 + 1], f);   // G
                        fputc(row[x * 4 + 0], f);   // B
                    }
                }
                fclose(f);
                fprintf(stderr, "picoview: wrote %s (%dx%d)\n", dump, b->w, b->h);
            }
        }
    }

    wl_surface_attach(g.surface, b->buf, 0, 0);
    wl_surface_damage_buffer(g.surface, 0, 0, b->w, b->h);
    wl_surface_commit(g.surface);
    b->busy = true;
    g.dirty = false;
}

// ------------------------------------------------------------------ input
// Which button is under a point, or B_NONE. The same layout the drawing used,
// computed again rather than remembered - it is a dozen rectangles.
int hitBar(double x, double y) {
    if (!g.pointerIn) return B_NONE;
    pv::Rect bar, pct;
    std::vector<Btn> btns = layoutBar(g.width, g.height, bar, pct);
    for (const Btn& b : btns)
        if (b.r.contains((float)x, (float)y)) return b.id;
    return B_NONE;
}

// True anywhere on the bar or the seek strip, not just on a button: a drag
// started there should not pan the picture underneath it.
bool overBar(double x, double y) {
    if (!g.pointerIn) return false;
    pv::Rect bar, pct;
    layoutBar(g.width, g.height, bar, pct);
    if (bar.contains((float)x, (float)y)) return true;
    if (g.video.isOpen() && g.video.duration() > 0) {
        pv::Rect r = seekRect(g.width, g.height);
        if (pv::Rect{ r.x - 10, r.y - 8, r.w + 20, r.h + 16 }.contains((float)x, (float)y))
            return true;
    }
    return false;
}

// Dragging the handle scrubs; a click anywhere on the track jumps there.
bool seekFromPoint(double x, double y, bool starting) {
    if (!g.video.isOpen()) return false;
    double dur = g.video.duration();
    if (!(dur > 0)) return false;
    pv::Rect r = seekRect(g.width, g.height);
    if (starting &&
        !pv::Rect{ r.x - 10, r.y - 8, r.w + 20, r.h + 16 }.contains((float)x, (float)y))
        return false;
    float t = (float)((x - r.x) / std::max(1.f, r.w));
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    g.video.seekTo(dur * t);
    g.dirty = true;
    return true;
}

void zoomAt(float factor, double cx, double cy) {
    float before = g.fitted ? fitZoom() : g.zoom;
    float after = std::min(64.f, std::max(0.02f, before * factor));
    // Keep whatever is under the pointer under the pointer.
    double ox = cx - (g.width * 0.5 + g.panX);
    double oy = cy - (g.height * 0.5 + g.panY);
    g.panX -= (float)(ox * (after / before - 1.0));
    g.panY -= (float)(oy * (after / before - 1.0));
    g.zoom = after;
    g.fitted = false;
    g.dirty = true;
}

void doAction(int id) {
    switch (id) {
        case B_PREV: openAt(g.index - 1); break;
        case B_NEXT: openAt(g.index + 1); break;
        case B_ZOUT: zoomAt(1.f / 1.25f, g.width * 0.5, g.height * 0.5); break;
        case B_ZIN:  zoomAt(1.25f, g.width * 0.5, g.height * 0.5); break;
        case B_FIT:
            g.fitted = true;
            g.panX = g.panY = 0;
            g.dirty = true;
            break;
        case B_ACTUAL:
            g.zoom = 1.f;
            g.fitted = false;
            g.panX = g.panY = 0;
            g.dirty = true;
            break;
        case B_PLAY:
            g.video.togglePause();
            g.dirty = true;
            break;
        case B_FULL:
            g.fullscreen = !g.fullscreen;
            if (g.fullscreen) xdg_toplevel_set_fullscreen(g.toplevel, nullptr);
            else              xdg_toplevel_unset_fullscreen(g.toplevel);
            break;
        default: break;
    }
}

void onKey(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_q:
        case XKB_KEY_Escape:
            if (g.fullscreen && sym == XKB_KEY_Escape) {
                xdg_toplevel_unset_fullscreen(g.toplevel);
                g.fullscreen = false;
            } else {
                g.running = false;
            }
            break;
        case XKB_KEY_space:
            if (g.video.isOpen()) { g.video.togglePause(); g.dirty = true; }
            else openAt(g.index + 1);
            break;
        case XKB_KEY_Right:
        case XKB_KEY_Next:
            // On a film the arrows seek, the way they do on Windows.
            if (g.video.isOpen() && g.video.duration() > 0) {
                g.video.seekBy(g.shiftDown ? 30.0 : 5.0);
                g.dirty = true;
            } else openAt(g.index + 1);
            break;
        case XKB_KEY_Left:
        case XKB_KEY_Prior:
            if (g.video.isOpen() && g.video.duration() > 0) {
                g.video.seekBy(g.shiftDown ? -30.0 : -5.0);
                g.dirty = true;
            } else openAt(g.index - 1);
            break;
        case XKB_KEY_Home: openAt(0); break;
        case XKB_KEY_End:  openAt((int)g.files.size() - 1); break;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add:
            zoomAt(1.25f, g.width * 0.5, g.height * 0.5);
            break;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract:
            zoomAt(1.f / 1.25f, g.width * 0.5, g.height * 0.5);
            break;
        case XKB_KEY_0:
            g.fitted = true;
            g.panX = g.panY = 0;
            g.dirty = true;
            break;
        case XKB_KEY_1:
            g.zoom = 1.f;
            g.fitted = false;
            g.panX = g.panY = 0;
            g.dirty = true;
            break;
        case XKB_KEY_f:
        case XKB_KEY_F11:
            g.fullscreen = !g.fullscreen;
            if (g.fullscreen) xdg_toplevel_set_fullscreen(g.toplevel, nullptr);
            else              xdg_toplevel_unset_fullscreen(g.toplevel);
            break;
        default: break;
    }
}

void kbKeymap(void*, wl_keyboard*, uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    if (g.keymap) xkb_keymap_unref(g.keymap);
    if (g.xkbState) xkb_state_unref(g.xkbState);
    g.keymap = xkb_keymap_new_from_string(g.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    g.xkbState = g.keymap ? xkb_state_new(g.keymap) : nullptr;
}
void kbEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void kbLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void kbKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !g.xkbState) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkbState, key + 8);
    onKey(sym);
}
void kbModifiers(void*, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat,
                 uint32_t locked, uint32_t group) {
    if (!g.xkbState) return;
    xkb_state_update_mask(g.xkbState, dep, lat, locked, 0, 0, group);
    g.ctrlDown = xkb_state_mod_name_is_active(g.xkbState, XKB_MOD_NAME_CTRL,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;
    g.shiftDown = xkb_state_mod_name_is_active(g.xkbState, XKB_MOD_NAME_SHIFT,
                                               XKB_STATE_MODS_EFFECTIVE) > 0;
}
void kbRepeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboard = {
    kbKeymap, kbEnter, kbLeave, kbKey, kbModifiers, kbRepeat
};

void ptEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    g.mouseX = wl_fixed_to_double(x);
    g.mouseY = wl_fixed_to_double(y);
    // The bar and the caption are there while the pointer is, and gone when it
    // is not. No timer, no fade, nothing to keep in step.
    g.pointerIn = true;
    g.hot = hitBar(g.mouseX, g.mouseY);
    g.dirty = true;
}
void ptLeave(void*, wl_pointer*, uint32_t, wl_surface*) {
    g.dragging = false;
    g.pointerIn = false;
    g.hot = g.pressed = B_NONE;
    g.dirty = true;
}
void ptMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    g.mouseX = wl_fixed_to_double(x);
    g.mouseY = wl_fixed_to_double(y);
    if (!g.pointerIn) { g.pointerIn = true; g.dirty = true; }
    int was = g.hot;
    g.hot = hitBar(g.mouseX, g.mouseY);
    if (g.hot != was) g.dirty = true;
    if (g.seekDrag) { seekFromPoint(g.mouseX, g.mouseY, false); return; }
    if (g.dragging) {
        g.panX = g.dragPanX + (float)(g.mouseX - g.dragFromX);
        g.panY = g.dragPanY + (float)(g.mouseY - g.dragFromY);
        g.fitted = false;
        if (g.zoom <= 0.f) g.zoom = fitZoom();
        g.dirty = true;
    }
}
void ptButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    if (button != BTN_LEFT) return;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int id = hitBar(g.mouseX, g.mouseY);
        if (id != B_NONE) { g.pressed = id; g.dirty = true; return; }
        if (seekFromPoint(g.mouseX, g.mouseY, true)) { g.seekDrag = true; return; }
        if (overBar(g.mouseX, g.mouseY)) return;   // the bar swallows the drag
    } else if (g.seekDrag) {
        g.seekDrag = false;
        return;
    } else if (g.pressed != B_NONE) {
        // Acting on release, and only if the pointer is still on the button,
        // is what lets a mis-aimed press be taken back.
        int id = hitBar(g.mouseX, g.mouseY);
        int was = g.pressed;
        g.pressed = B_NONE;
        g.dirty = true;
        if (id == was) doAction(was);
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        g.dragging = true;
        g.dragFromX = g.mouseX;
        g.dragFromY = g.mouseY;
        g.dragPanX = g.panX;
        g.dragPanY = g.panY;
        if (g.fitted) { g.zoom = fitZoom(); }
    } else {
        g.dragging = false;
    }
}
void ptAxis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    double v = wl_fixed_to_double(value);
    if (g.ctrlDown) zoomAt(v < 0 ? 1.15f : 1.f / 1.15f, g.mouseX, g.mouseY);
    else            openAt(g.index + (v < 0 ? -1 : 1));
}
void ptFrame(void*, wl_pointer*) {}
void ptAxisSource(void*, wl_pointer*, uint32_t) {}
void ptAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void ptAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
// Filled in by assignment rather than as an aggregate: wl_pointer_listener has
// grown twice since this was written, and a list of initialisers would warn on
// a new wayland-client and fail to compile on an old one.
wl_pointer_listener makePointerListener() {
    wl_pointer_listener l{};
    l.enter = ptEnter;
    l.leave = ptLeave;
    l.motion = ptMotion;
    l.button = ptButton;
    l.axis = ptAxis;
    l.frame = ptFrame;
    l.axis_source = ptAxisSource;
    l.axis_stop = ptAxisStop;
    l.axis_discrete = ptAxisDiscrete;
    return l;
}
const wl_pointer_listener kPointer = makePointerListener();

void seatCaps(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &kKeyboard, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &kPointer, nullptr);
    }
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeat = { seatCaps, seatName };

// ------------------------------------------------------------------ shell
void wmPing(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener kWmBase = { wmPing };

void surfaceConfigure(void*, xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    g.configured = true;
    redraw();
}
const xdg_surface_listener kXdgSurface = { surfaceConfigure };

void toplevelConfigure(void*, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
    if (w > 0 && h > 0 && (w != g.width || h != g.height)) {
        g.width = w;
        g.height = h;
        g.dirty = true;
    }
    g.fullscreen = false;
    uint32_t* st;
    for (st = (uint32_t*)states->data;
         (const char*)st < (const char*)states->data + states->size; ++st)
        if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) g.fullscreen = true;
}
void toplevelClose(void*, xdg_toplevel*) { g.running = false; }
void toplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevelCaps(void*, xdg_toplevel*, wl_array*) {}
xdg_toplevel_listener makeToplevelListener() {
    xdg_toplevel_listener l{};
    l.configure = toplevelConfigure;
    l.close = toplevelClose;
    l.configure_bounds = toplevelBounds;
    l.wm_capabilities = toplevelCaps;
    return l;
}
const xdg_toplevel_listener kToplevel = makeToplevelListener();

void registryAdd(void*, wl_registry* reg, uint32_t id, const char* iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name)) {
        g.compositor = (wl_compositor*)wl_registry_bind(reg, id, &wl_compositor_interface,
                                                        std::min(ver, 4u));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        g.shm = (wl_shm*)wl_registry_bind(reg, id, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g.wmBase = (xdg_wm_base*)wl_registry_bind(reg, id, &xdg_wm_base_interface,
                                                  std::min(ver, 3u));
        xdg_wm_base_add_listener(g.wmBase, &kWmBase, nullptr);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        g.seat = (wl_seat*)wl_registry_bind(reg, id, &wl_seat_interface, std::min(ver, 5u));
        wl_seat_add_listener(g.seat, &kSeat, nullptr);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        g.decoMgr = (zxdg_decoration_manager_v1*)wl_registry_bind(
            reg, id, &zxdg_decoration_manager_v1_interface, 1);
    }
}
void registryRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry = { registryAdd, registryRemove };

}  // namespace

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    std::string arg = (argc > 1) ? argv[1] : std::string();
    if (arg == "--help" || arg == "-h") {
        printf("picoview [file|folder]\n"
               "  arrows / page up-down   previous, next\n"
               "  ctrl + wheel            zoom      wheel   previous, next\n"
               "  + -                     zoom      0 / 1   fit / actual size\n"
               "  f, F11                  full screen\n"
               "  q, Esc                  quit\n");
        return 0;
    }

    if (!arg.empty()) {
        char real[4096];
        if (realpath(arg.c_str(), real)) arg = real;
        struct stat st;
        if (stat(arg.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) scanFolder(arg, std::string());
        else scanFolder(dirOf(arg), arg);
    } else {
        char cwd[4096];
        scanFolder(getcwd(cwd, sizeof(cwd)) ? cwd : ".", std::string());
    }
    if (g.files.empty()) {
        fprintf(stderr, "picoview: nothing to show here\n");
        return 1;
    }

    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        fprintf(stderr, "picoview: no Wayland display (is WAYLAND_DISPLAY set?)\n");
        return 1;
    }
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &kRegistry, nullptr);
    wl_display_roundtrip(g.display);

    if (!g.compositor || !g.shm || !g.wmBase) {
        fprintf(stderr, "picoview: the compositor is missing wl_shm or xdg-shell\n");
        return 1;
    }

    g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // Whatever this machine calls its sans-serif. Asking for a specific family
    // would mean shipping one, and shipping a font to draw a file name is not
    // a trade worth making.
    g.fBody = pv::fontOpen(nullptr, 15.f, false);
    g.fSmall = pv::fontOpen(nullptr, 12.f, false);
    g.surface = wl_compositor_create_surface(g.compositor);
    g.xsurface = xdg_wm_base_get_xdg_surface(g.wmBase, g.surface);
    xdg_surface_add_listener(g.xsurface, &kXdgSurface, nullptr);
    g.toplevel = xdg_surface_get_toplevel(g.xsurface);
    xdg_toplevel_add_listener(g.toplevel, &kToplevel, nullptr);
    xdg_toplevel_set_app_id(g.toplevel, "picoview");
    xdg_toplevel_set_title(g.toplevel, "PicoView");

    // Ask the compositor to draw the frame. Hyprland and every other wlroots
    // compositor will; the ones that will not simply give a bare surface,
    // which is no worse than what this stage could draw for itself.
    if (g.decoMgr) {
        g.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(g.decoMgr, g.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(g.deco,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(g.surface);
    wl_display_roundtrip(g.display);

    openAt(g.index);
    redraw();

    // The loop has to answer to two things now: Wayland on its socket, and mpv
    // on another thread. Wayland's prepare/read dance exists exactly so the
    // second one cannot slip an event in between the check and the read.
    while (g.running) {
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        wl_display_flush(g.display);

        bool playing = g.video.isOpen() && !g.video.paused();
        int timeout = (g.dirty || playing) ? 8 : (g.video.isOpen() ? 200 : -1);

        pollfd pfd{ wl_display_get_fd(g.display), POLLIN, 0 };
        int n = poll(&pfd, 1, timeout);
        if (n > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(g.display) == -1) break;
        } else {
            wl_display_cancel_read(g.display);
        }
        if (wl_display_dispatch_pending(g.display) == -1) break;

        g.video.pump();
        if (g.dirty || g.video.wants()) redraw();
    }
    g.video.close();

    for (auto& b : g.buffers) {
        if (b.buf) wl_buffer_destroy(b.buf);
        if (b.data) munmap(b.data, b.size);
    }
    pv::fontClose(g.fBody);
    pv::fontClose(g.fSmall);
    if (g.xkbState) xkb_state_unref(g.xkbState);
    if (g.keymap) xkb_keymap_unref(g.keymap);
    if (g.xkb) xkb_context_unref(g.xkb);
    wl_display_disconnect(g.display);
    return 0;
}
