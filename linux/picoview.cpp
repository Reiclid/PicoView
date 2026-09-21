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
//   - No text. Text means HarfBuzz, FreeType and fontconfig, and none of them
//     belongs in the first version of anything.
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

bool isMusic(const std::string& s) {
    static const char* ext[] = { ".mp3", ".wav", ".flac", ".m4a", ".ogg", ".opus",
                                 ".wma", ".aac", nullptr };
    for (int i = 0; ext[i]; ++i)
        if (endsWithLower(s, ext[i])) return true;
    return false;
}

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
            if (!isPicture(name) && !isMusic(name)) continue;
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
    g.image = isMusic(path) ? loadCover(path) : loadPicture(path);
    if (!g.image.ok)
        fprintf(stderr, "picoview: cannot read %s\n", path.c_str());
    g.fitted = true;
    g.panX = g.panY = 0;
    g.dirty = true;

    std::string title = "PicoView - " + nameOf(path);
    if (g.toplevel) xdg_toplevel_set_title(g.toplevel, title.c_str());
}

// ------------------------------------------------------------------ drawing
float fitZoom() {
    if (!g.image.ok || g.image.w <= 0 || g.image.h <= 0) return 1.f;
    float sx = (float)g.width / (float)g.image.w;
    float sy = (float)g.height / (float)g.image.h;
    float z = std::min(sx, sy);
    return std::min(z, 1.f);         // never blow a small picture up to fit
}

void clampPan() {
    float z = g.fitted ? fitZoom() : g.zoom;
    float dw = g.image.w * z, dh = g.image.h * z;
    float slackX = std::max(0.f, (dw - g.width) * 0.5f);
    float slackY = std::max(0.f, (dh - g.height) * 0.5f);
    g.panX = std::max(-slackX, std::min(slackX, g.panX));
    g.panY = std::max(-slackY, std::min(slackY, g.panY));
}

// Bilinear, split across the cores. A twenty-four megapixel photo scaled into
// a window is the one place this build would feel slow, so it is the one place
// that is threaded.
void blitRows(uint8_t* dst, int dstStride, int y0, int y1,
              int dx, int dy, int dw, int dh) {
    const Image& im = g.image;
    const float invW = (float)im.w / (float)dw;
    const float invH = (float)im.h / (float)dh;

    for (int y = y0; y < y1; ++y) {
        uint8_t* row = dst + (size_t)y * dstStride;
        float sy = ((float)(y - dy) + 0.5f) * invH - 0.5f;
        int   iy = (int)floorf(sy);
        float fy = sy - iy;
        int   y1i = std::min(std::max(iy, 0), im.h - 1);
        int   y2i = std::min(std::max(iy + 1, 0), im.h - 1);
        const uint8_t* r1 = im.px.data() + (size_t)y1i * im.w * 4;
        const uint8_t* r2 = im.px.data() + (size_t)y2i * im.w * 4;

        int xa = std::max(0, dx), xb = std::min(g.width, dx + dw);
        for (int x = xa; x < xb; ++x) {
            float sx = ((float)(x - dx) + 0.5f) * invW - 0.5f;
            int   ix = (int)floorf(sx);
            float fx = sx - ix;
            int   x1i = std::min(std::max(ix, 0), im.w - 1);
            int   x2i = std::min(std::max(ix + 1, 0), im.w - 1);
            uint8_t* o = row + x * 4;
            for (int c = 0; c < 4; ++c) {
                float a = r1[x1i * 4 + c] + (r1[x2i * 4 + c] - r1[x1i * 4 + c]) * fx;
                float b = r2[x1i * 4 + c] + (r2[x2i * 4 + c] - r2[x1i * 4 + c]) * fx;
                float v = a + (b - a) * fy;
                o[c] = (uint8_t)std::min(255.f, std::max(0.f, v + 0.5f));
            }
        }
    }
}

void paint(Buffer& b) {
    // The backdrop, dark like the other build's.
    for (int y = 0; y < b.h; ++y) {
        uint32_t* row = (uint32_t*)(b.data + (size_t)y * b.w * 4);
        for (int x = 0; x < b.w; ++x) row[x] = 0xFF1B1B1D;
    }
    if (!g.image.ok) return;

    float z = g.fitted ? fitZoom() : g.zoom;
    int dw = std::max(1, (int)lroundf(g.image.w * z));
    int dh = std::max(1, (int)lroundf(g.image.h * z));
    int dx = (int)lroundf((b.w - dw) * 0.5f + g.panX);
    int dy = (int)lroundf((b.h - dh) * 0.5f + g.panY);

    int y0 = std::max(0, dy), y1 = std::min(b.h, dy + dh);
    if (y1 <= y0) return;

    unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
    if ((y1 - y0) < 64) n = 1;
    std::vector<std::thread> pool;
    int span = (y1 - y0 + (int)n - 1) / (int)n;
    for (unsigned i = 0; i < n; ++i) {
        int a = y0 + (int)i * span;
        int c = std::min(y1, a + span);
        if (a >= c) break;
        pool.emplace_back(blitRows, b.data, b.w * 4, a, c, dx, dy, dw, dh);
    }
    for (auto& t : pool) t.join();
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
        case XKB_KEY_Right:
        case XKB_KEY_Next:
        case XKB_KEY_space:
            openAt(g.index + 1);
            break;
        case XKB_KEY_Left:
        case XKB_KEY_Prior:
            openAt(g.index - 1);
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
}
void ptLeave(void*, wl_pointer*, uint32_t, wl_surface*) { g.dragging = false; }
void ptMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    g.mouseX = wl_fixed_to_double(x);
    g.mouseY = wl_fixed_to_double(y);
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

    while (g.running && wl_display_dispatch(g.display) != -1) {
        if (g.dirty) redraw();
    }

    for (auto& b : g.buffers) {
        if (b.buf) wl_buffer_destroy(b.buf);
        if (b.data) munmap(b.data, b.size);
    }
    if (g.xkbState) xkb_state_unref(g.xkbState);
    if (g.keymap) xkb_keymap_unref(g.keymap);
    if (g.xkb) xkb_context_unref(g.xkb);
    wl_display_disconnect(g.display);
    return 0;
}
