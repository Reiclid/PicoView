// Window, input, application logic.
#include "app.h"

App* g_app = nullptr;

static const wchar_t* kClass = L"PicoViewWindow";
enum { TIMER_ANIM = 1, TIMER_SLIDESHOW = 2, TIMER_UNQUIET = 3, TIMER_BARHIDE = 4 };

// DWM bits that are not in every SDK header.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#define PG_DWMWCP_ROUND       2
#define PG_DWMSBT_MAINWINDOW  2
#define PG_DWMSBT_NONE        1
#define PG_DWMSBT_ACRYLIC     3
#define PG_DWMWCP_DONOTROUND  1
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#define PG_DWMWA_COLOR_NONE    0xFFFFFFFE
#define PG_DWMWA_COLOR_DEFAULT 0xFFFFFFFF

// Undocumented but long-stable: the only way a plain Win32 window gets a blur
// of whatever sits *behind* it (DWM's own backdrops only sample the wallpaper).
// Absent on an OS that ever drops it, in which case we fall back to acrylic.
namespace {
struct PG_ACCENT_POLICY { int state; int flags; unsigned gradient; int animId; };
struct PG_WCA_DATA { int attrib; void* data; size_t size; };
using PFN_SetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, PG_WCA_DATA*);

PFN_SetWindowCompositionAttribute swca() {
    static PFN_SetWindowCompositionAttribute fn = [] {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        return u ? (PFN_SetWindowCompositionAttribute)
                   GetProcAddress(u, "SetWindowCompositionAttribute") : nullptr;
    }();
    return fn;
}
} // namespace

// =================================================================== helpers
void App::invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
void App::requestAnim() { animating = true; }

void App::showToast(const wstring& t, double seconds) {
    toast.text = t;
    toast.until = nowSec() + seconds;
    invalidate();
}

wstring App::currentPath() const {
    if (index >= 0 && index < (int)folder.count()) return folder.pathAt(index);
    return pendingPath;
}

std::shared_ptr<Picture> App::current() {
    wstring p = currentPath();
    if (p.empty()) return nullptr;
    auto it = pics.find(p);
    return it == pics.end() ? nullptr : it->second;
}

// ------------------------------------------------------------------ caches
static size_t picBytes(const std::shared_ptr<Picture>& p) {
    return p && p->bmp ? (size_t)p->w * p->h * 4 : 0;
}

void App::trimCaches() {
    const size_t kPicBudget = 768ull << 20;
    size_t total = 0;
    for (auto& kv : pics) total += picBytes(kv.second);

    wstring keep[5];
    int n = 0;
    for (int d = -2; d <= 2; ++d) {
        int i = index + d;
        if (i >= 0 && i < (int)folder.count()) keep[n++] = folder.pathAt(i);
    }

    while (total > kPicBudget && picLru.size() > 1) {
        wstring victim;
        for (auto it = picLru.begin(); it != picLru.end(); ++it) {
            bool protectedEntry = false;
            for (int i = 0; i < n; ++i) if (keep[i] == *it) protectedEntry = true;
            if (!protectedEntry) { victim = *it; picLru.erase(it); break; }
        }
        if (victim.empty()) break;
        auto it = pics.find(victim);
        if (it != pics.end()) { total -= picBytes(it->second); pics.erase(it); }
    }

    while (thumbLru.size() > thumbBudget) {
        wstring v = thumbLru.front();
        thumbLru.pop_front();
        thumbs.erase(v);
    }
}

static void touchLru(std::deque<wstring>& lru, const wstring& key) {
    for (auto it = lru.begin(); it != lru.end(); ++it)
        if (*it == key) { lru.erase(it); break; }
    lru.push_back(key);
}

std::shared_ptr<Thumb> App::thumbFor(const wstring& path, int px, bool request) {
    auto it = thumbs.find(path);
    if (it != thumbs.end()) {
        touchLru(thumbLru, path);
        return it->second;
    }
    if (!request) return nullptr;

    auto t = std::make_shared<Thumb>();
    t->requested = true;
    thumbs[path] = t;
    thumbLru.push_back(path);

    DecodeJob j;
    j.path = path;
    j.kind = JobKind::Thumb;
    j.targetW = j.targetH = clampi(px, 96, 512);
    j.generation = loader.generation();
    j.priority = 40;
    loader.submit(std::move(j));
    return t;
}

// ------------------------------------------------------------------ requests
void App::requestPicture(const wstring& path, bool exif, int prio) {
    if (path.empty()) return;
    auto it = pics.find(path);
    if (it != pics.end() && it->second->bmp && !it->second->preview) {
        if (exif && !it->second->exifRead) { /* fall through to re-request metadata */ }
        else { touchLru(picLru, path); return; }
    }

    // How many pixels the canvas can actually show. Falling back to the work
    // area matters for the very first image, which is requested before the
    // window has been laid out.
    int tw = (int)(R.canvas.right - R.canvas.left);
    int th = (int)(R.canvas.bottom - R.canvas.top);
    if (tw < 64 || th < 64) {
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            tw = wa.right - wa.left;
            th = wa.bottom - wa.top;
        } else { tw = 1920; th = 1080; }
    }
    tw = std::max(tw, 320);
    th = std::max(th, 240);

    DecodeJob j;
    j.path = path;
    j.kind = JobKind::Full;
    j.targetW = tw;
    j.targetH = th;
    j.wantExif = exif;
    j.generation = loader.generation();
    j.priority = prio;
    loader.submit(std::move(j));
}

void App::preload() {
    if (folder.count() < 2) return;
    const int order[] = { 1, -1, 2, -2 };
    for (int d : order) {
        int i = index + d;
        if (cfg.loopFolder) i = ((i % (int)folder.count()) + (int)folder.count()) % (int)folder.count();
        if (i < 0 || i >= (int)folder.count() || i == index) continue;
        wstring p = folder.pathAt(i);
        if (isVideoPath(p)) continue;              // videos stream, nothing to preload
        auto it = pics.find(p);
        if (it != pics.end() && it->second->bmp) continue;
        requestPicture(p, false, 20 + abs(d));
    }
}

// ------------------------------------------------------------------ view math
bool App::sourceSize(int& w, int& h) {
    if (videoMode) { w = video.width(); h = video.height(); return w > 0 && h > 0; }
    auto pic = current();
    if (!pic || pic->srcW <= 0) return false;
    w = pic->srcW; h = pic->srcH;
    return true;
}

D2D1_RECT_F App::imageRect() {
    D2D1_RECT_F cv = R.canvas;
    int sw = 0, sh = 0;
    if (!sourceSize(sw, sh)) return cv;
    float w = (float)sw * zoom, h = (float)sh * zoom;
    if (rot & 1) std::swap(w, h);
    float cx = (cv.left + cv.right) * .5f + panX;
    float cy = (cv.top + cv.bottom) * .5f + panY;
    return { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
}

static float fitZoomFor(App& a) {
    int sw = 0, sh = 0;
    if (!a.sourceSize(sw, sh)) return 1.f;
    float w = (float)sw, h = (float)sh;
    if (a.rot & 1) std::swap(w, h);
    float inset = (a.cfg.autoSize == 2 || a.fullscreen) ? 0.f : a.gfx.s(24.f);
    float cw = std::max(1.f, a.R.canvas.right - a.R.canvas.left - inset);
    float ch = std::max(1.f, a.R.canvas.bottom - a.R.canvas.top - inset);
    float k = std::min(cw / w, ch / h);
    // Video fills the frame, and so does "window follows the picture" - there the
    // frame *is* the picture, so any gap or 100% cap would be wrong. A photo in
    // the other modes is never blown up past 100%: that only makes its pixels
    // bigger, not better.
    return (a.videoMode || a.cfg.autoSize == 2) ? k : std::min(1.f, k);
}

void App::clampPan() {
    int sw = 0, sh = 0;
    if (!sourceSize(sw, sh)) { panX = panY = panTargetX = panTargetY = 0; return; }
    float w = (float)sw * zoom, h = (float)sh * zoom;
    if (rot & 1) std::swap(w, h);
    float cw = R.canvas.right - R.canvas.left, ch = R.canvas.bottom - R.canvas.top;

    float mx = std::max(0.f, (w - cw) * .5f);
    float my = std::max(0.f, (h - ch) * .5f);
    panX = clampf(panX, -mx, mx);
    panY = clampf(panY, -my, my);
    panTargetX = clampf(panTargetX, -mx, mx);
    panTargetY = clampf(panTargetY, -my, my);
}

void App::applyFit(bool animate) {
    float z = (fitMode == Fit::Actual) ? 1.f : fitZoomFor(*this);
    int sw = 0, sh = 0; sourceSize(sw, sh);
    pgLog("applyFit z=%.3f canvas=%.0fx%.0f src=%dx%d", z,
          R.canvas.right - R.canvas.left, R.canvas.bottom - R.canvas.top, sw, sh);
    zoomTarget = z;
    panTargetX = panTargetY = 0;
    if (!animate) { zoom = z; panX = panY = 0; }
    clampPan();
    invalidate();
}

void App::rememberView() {
    if (!cfg.rememberZoom) return;
    wstring p = currentPath();
    int sw = 0, sh = 0;
    if (p.empty() || !sourceSize(sw, sh)) return;

    ViewState v;
    v.zoom = zoomTarget; v.panX = panTargetX; v.panY = panTargetY;
    v.rot = rot; v.flipH = flipH; v.flipV = flipV; v.fit = (int)fitMode;

    if (!viewStates.count(p)) viewStateLru.push_back(p);
    viewStates[p] = v;
    while (viewStateLru.size() > 300) {
        viewStates.erase(viewStateLru.front());
        viewStateLru.pop_front();
    }
}

void App::takePendingView() {
    if (!hasPendingView) return;
    hasPendingView = false;
    rot = pendingView.rot;
    flipH = pendingView.flipH;
    flipV = pendingView.flipV;
    fitMode = (Fit)pendingView.fit;
    zoom = zoomTarget = pendingView.zoom;
    panX = panTargetX = pendingView.panX;
    panY = panTargetY = pendingView.panY;
    clampPan();
    invalidate();
}

void App::autoSizeWindow() {
    if (cfg.autoSize != 2 || fullscreen || !hwnd) return;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) return;

    int sw = 0, sh = 0;
    if (!sourceSize(sw, sh)) return;
    if (rot & 1) std::swap(sw, sh);

    UINT dpi = gfx.dpi ? gfx.dpi : 96;
    int titleH = titleOverlay() ? 0 : (int)lround(40.0 * dpi / 96.0);
    int infoW = cfg.infoPanel ? (int)lround(320.0 * dpi / 96.0) : 0;
    int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

    // Never grow past the work area; shrink the picture instead.
    double maxCanvasW = (mi.rcWork.right - mi.rcWork.left) - fx * 2 - infoW;
    double maxCanvasH = (mi.rcWork.bottom - mi.rcWork.top) - fy - titleH;
    double k = std::min(1.0, std::min(maxCanvasW / sw, maxCanvasH / sh));
    if (!(k > 0.01)) return;

    int winW = (int)lround(sw * k) + infoW + fx * 2;
    int winH = (int)lround(sh * k) + titleH + fy;
    winW = clampi(winW, 420, mi.rcWork.right - mi.rcWork.left);
    winH = clampi(winH, 320, mi.rcWork.bottom - mi.rcWork.top);

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    int x = clampi(cx - winW / 2, mi.rcWork.left, mi.rcWork.right - winW);
    int y = clampi(cy - winH / 2, mi.rcWork.top, mi.rcWork.bottom - winH);

    if (wr.left == x && wr.top == y && (wr.right - wr.left) == winW && (wr.bottom - wr.top) == winH)
        return;
    SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::setZoom(float z, D2D1_POINT_2F anchor, bool animate) {
    int sw = 0, sh = 0;
    if (!sourceSize(sw, sh)) return;
    float z0 = zoomTarget;
    float z1 = clampf(z, 0.02f, 64.f);
    if (fabsf(z1 - z0) < 0.0001f) return;

    // Keep the point under the cursor fixed.
    float cx = (R.canvas.left + R.canvas.right) * .5f;
    float cy = (R.canvas.top + R.canvas.bottom) * .5f;
    float ox = anchor.x - (cx + panTargetX);
    float oy = anchor.y - (cy + panTargetY);
    float k = z1 / z0;
    panTargetX -= ox * (k - 1.f);
    panTargetY -= oy * (k - 1.f);

    zoomTarget = z1;
    fitMode = Fit::Free;
    if (!animate) { zoom = z1; panX = panTargetX; panY = panTargetY; }
    clampPan();
    requestAnim();
    invalidate();
}

// Ask for a sharper decode when the user magnifies past what we decoded.
static void maybeUpgrade(App& a) {
    auto pic = a.current();
    if (!pic || !pic->bmp || pic->full || pic->upgrading || pic->preview) return;
    float needed = a.zoomTarget * pic->srcW;
    if (needed <= pic->w * 1.12f) return;

    pic->upgrading = true;
    DecodeJob j;
    j.path = pic->path;
    j.kind = JobKind::Full;
    j.targetW = j.targetH = 0;           // native resolution
    j.generation = a.loader.generation();
    j.priority = 5;
    a.loader.submit(std::move(j));
}

// ------------------------------------------------------------------ video
void App::openVideo(const wstring& path) {
    videoMode = true;
    rot = 0; flipH = flipV = false;
    fitMode = (cfg.autoSize == 0) ? Fit::Actual : Fit::Window;
    slideshow = false;
    fadeIn = 0.f;

    // Can be called before Direct3D exists (first file on the command line);
    // wWinMain re-opens once the device is up.
    if (!gfx.d3d) return;
    if (!videoInit) videoInit = video.init(gfx.d3d.Get(), hwnd);
    if (!videoInit) { showToast(T(L"Media Foundation недоступна")); return; }

    if (!video.open(path, cfg.autoPlay)) {
        showToast(video.error().empty() ? T(L"Не вдалося відкрити відео") : video.error());
        return;
    }
    video.setVolume(cfg.volume / 100.f);
    video.setMuted(cfg.muted);

    pendingResume = -1;
    double sec = 0;
    if (cfg.resumeVideo && resume.get(path, sec) && sec > 8.0) pendingResume = sec;

    applyFit(false);
    invalidate();
}

// A seek always re-arms the un-mute timer: if the engine somehow never reports
// SEEKED we would otherwise stay silent.
void App::videoSeekTo(double seconds) {
    if (!videoMode || !video.isOpen()) return;
    video.seek(seconds);
    SetTimer(hwnd, TIMER_UNQUIET, 220, nullptr);
    invalidate();
}

void App::videoSeekBy(double delta) {
    if (!videoMode || !video.isOpen()) return;
    video.seekBy(delta);
    SetTimer(hwnd, TIMER_UNQUIET, 220, nullptr);
    seekFlashDir = delta < 0 ? -1 : 1;
    seekFlashAmount = fabs(delta);
    seekFlashUntil = nowSec() + 0.75;
    requestAnim();
    invalidate();
}

void App::leaveVideo() {
    if (!videoMode) return;
    storeResume();
    preview.close();
    previewBmp.Reset();
    previewW = previewH = 0;
    previewAt = previewWant = -1;
    cfg.volume = clampi((int)lround(video.volume() * 100.f), 0, 100);
    cfg.muted = video.muted();
    video.close();
    videoMode = false;
    seekDragging = false;
}

// ------------------------------------------------------------------ navigation
void App::goTo(int newIndex, bool resetView) {
    if (folder.count() == 0) return;
    int n = (int)folder.count();
    if (cfg.loopFolder) newIndex = ((newIndex % n) + n) % n;
    newIndex = clampi(newIndex, 0, n - 1);
    if (newIndex == index && current() && current()->bmp) return;

    rememberView();
    index = newIndex;
    moreMenuOpen = false;
    loader.bumpGeneration();
    if (resetView) { rot = 0; flipH = flipV = false;
                     fitMode = (cfg.autoSize == 0) ? Fit::Actual : Fit::Window; }

    wstring p = folder.pathAt(index);

    hasPendingView = false;
    if (cfg.rememberZoom) {
        auto vs = viewStates.find(p);
        if (vs != viewStates.end()) { pendingView = vs->second; hasPendingView = true; }
    }

    if (isVideoPath(p)) {
        leaveVideo();
        openVideo(p);
        preload();
        trimCaches();
        invalidate();
        return;
    }
    leaveVideo();

    auto it = pics.find(p);
    if (it == pics.end()) {
        auto pic = std::make_shared<Picture>();
        pic->path = p;
        pics[p] = pic;
        picLru.push_back(p);
        // Instant glimpse while the real decode runs.
        DecodeJob prev;
        prev.path = p; prev.kind = JobKind::Preview;
        prev.generation = loader.generation(); prev.priority = 0;
        loader.submit(std::move(prev));
        requestPicture(p, true, 1);
        fadeIn = 0.f;
    } else {
        touchLru(picLru, p);
        if (!it->second->bmp) requestPicture(p, true, 1);
        fadeIn = 1.f;
    }

    applyFit(false);
    {
        auto pic = current();
        if (pic && pic->bmp && pic->srcW > 0) takePendingView();
    }
    preload();
    trimCaches();
    slideshowNext = nowSec() + cfg.slideshowMs / 1000.0;
    invalidate();
}

void App::step(int delta) {
    if (folder.count() == 0) return;
    int n = (int)folder.count();
    int next = index + delta;
    if (!cfg.loopFolder && (next < 0 || next >= n)) {
        showToast(delta > 0 ? T(L"Останнє зображення") : T(L"Перше зображення"), 1.0);
        return;
    }
    goTo(next, true);
}

void App::scanFolderNow(const wstring& dir, const wstring& select) {
    folder.scan(dir, (SortBy)cfg.sortBy, cfg.sortDesc);
    folderScanned = true;
    cfg.lastFolder = dir;
    int i = select.empty() ? 0 : folder.indexOf(fileNameOf(select));
    if (i < 0) i = 0;
    index = folder.count() ? i : -1;
    needRelayout = true;
    invalidate();
}

void App::openPath(const wstring& path) {
    if (path.empty()) return;
    if (dirExists(path)) {
        scanFolderNow(path, L"");
        if (folder.count()) goTo(0, true); else setView(View::Grid);
        return;
    }
    if (!fileExists(path)) { showToast(T(L"Файл не знайдено")); return; }

    // Drop the old index first: until the folder is rescanned, currentPath()
    // must resolve to the file we were actually asked to open, otherwise the
    // decode result gets matched against the previously shown picture.
    rememberView();
    index = -1;
    pendingPath = path;

    hasPendingView = false;
    if (cfg.rememberZoom) {
        auto vs = viewStates.find(path);
        if (vs != viewStates.end()) { pendingView = vs->second; hasPendingView = true; }
    }

    if (isVideoPath(path)) {
        leaveVideo();
        openVideo(path);
        setView(View::Viewer);
        needRelayout = true;
        PostMessageW(hwnd, WM_PG_SCANNED, 0, 0);
        return;
    }
    leaveVideo();
    auto pic = std::make_shared<Picture>();
    pic->path = path;
    if (!pics.count(path)) { pics[path] = pic; picLru.push_back(path); }

    loader.bumpGeneration();
    DecodeJob prev;
    prev.path = path; prev.kind = JobKind::Preview;
    prev.generation = loader.generation(); prev.priority = 0;
    loader.submit(std::move(prev));
    requestPicture(path, true, 1);

    rot = 0; flipH = flipV = false; fadeIn = 0.f;
    fitMode = (cfg.autoSize == 0) ? Fit::Actual : Fit::Window;
    setView(View::Viewer);
    needRelayout = true;

    // The folder listing can wait until after the first frame.
    PostMessageW(hwnd, WM_PG_SCANNED, 0, 0);
}

void App::setView(View v) {
    if (view == v) return;
    moreMenuOpen = false;
    view = v;
    cfg.viewMode = (v == View::Grid) ? 1 : 0;
    needRelayout = true;
    if (v == View::Grid && index >= 0 && gridCols > 0) {
        // Centre the current image in the grid.
        float target = (index / std::max(1, gridCols)) * gridRowH - (R.grid.bottom - R.grid.top) * .4f;
        gridScrollTarget = std::max(0.f, target);
        gridScroll = gridScrollTarget;
    }
    if (v == View::Viewer) applyFit(false);
    invalidate();
}

// ------------------------------------------------------------------ chrome
void App::applyTheme() {
    bool dark = (cfg.themeMode == 1) ? true : (cfg.themeMode == 2) ? false : systemUsesDarkMode();
    // With blur-behind the tint comes from the accent policy, so the canvas
    // itself must stay clear. Fullscreen has nothing worth showing behind it.
    blurActive = !fullscreen && cfg.backdrop == 3 && swca() != nullptr;
    float canvasAlpha = fullscreen ? 1.f
                      : blurActive ? 0.f
                      : clampf(cfg.canvasDim / 100.f, 0.f, 1.f);
    th.update(dark, systemAccentColor(), canvasAlpha);
    pgLog("applyTheme mode=%d -> dark=%d text=(%.2f,%.2f,%.2f a=%.2f)", cfg.themeMode, (int)dark,
          th.text.r, th.text.g, th.text.b, th.text.a);
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    applyBackdrop();
    invalidate();
}

void App::loadAssociations() {
    if (assocLoaded) return;
    assocLoaded = true;

    std::unordered_set<wstring> seen;
    for (const auto& e : decodeExtensions()) if (seen.insert(e).second) assocAll.push_back(e);
    for (const auto& e : videoExtensions()) if (seen.insert(e).second) assocAll.push_back(e);
    std::sort(assocAll.begin(), assocAll.end());

    if (cfg.associations.empty()) {
        for (const wchar_t* e : { L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp",
                                  L".tif", L".tiff", L".heic", L".avif", L".ico",
                                  L".mp4", L".mkv", L".mov", L".avi", L".webm" })
            if (seen.count(e)) assocSel.insert(e);
    } else {
        wstring cur;
        for (wchar_t c : cfg.associations) {
            if (c == L',' || c == L';') { if (!cur.empty()) assocSel.insert(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) assocSel.insert(cur);
    }
}

void App::applyTopmost() {
    if (!hwnd) return;
    SetWindowPos(hwnd, cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::storeResume() {
    if (!cfg.resumeVideo || !videoMode || !video.isOpen()) return;
    wstring p = video.path();
    double pos = video.position(), dur = video.duration();
    if (p.empty() || dur <= 0) return;
    // Only worth remembering for something long enough to come back to, and
    // only from somewhere that is neither the very start nor the very end.
    if (dur >= 30.0 && pos > 8.0 && pos < dur - 8.0) resume.put(p, pos);
    else resume.forget(p);
}

void App::applyBackdrop() {
    // Rounded corners and the accent border are what leave a hairline frame
    // around a full-screen window, so both are turned off there.
    int corner = fullscreen ? PG_DWMWCP_DONOTROUND : PG_DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = fullscreen ? PG_DWMWA_COLOR_NONE : PG_DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));

    // The two mechanisms fight each other, so only one is ever enabled.
    int b = (fullscreen || blurActive) ? PG_DWMSBT_NONE
          : (cfg.backdrop == 0 ? PG_DWMSBT_NONE
          :  cfg.backdrop == 1 ? PG_DWMSBT_MAINWINDOW : PG_DWMSBT_ACRYLIC);
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &b, sizeof(b));

    if (auto fn = swca()) {
        PG_ACCENT_POLICY ap{};
        if (blurActive) {
            ap.state = 4;                       // ACCENT_ENABLE_ACRYLICBLURBEHIND
            ap.flags = 2;                       // tint the whole client area
            unsigned a = (unsigned)clampi(cfg.canvasDim * 255 / 100, 0, 255);
            unsigned c = th.dark ? 0x16u : 0xE6u;   // neutral grey, 0xAABBGGRR
            ap.gradient = (a << 24) | (c << 16) | (c << 8) | c;
        } else {
            ap.state = 0;                       // ACCENT_DISABLED
        }
        PG_WCA_DATA d{ 19 /* WCA_ACCENT_POLICY */, &ap, sizeof(ap) };
        fn(hwnd, &d);
    }
}

void App::setFullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        prevPlacement.length = sizeof(prevPlacement);
        GetWindowPlacement(hwnd, &prevPlacement);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    needRelayout = true;
    applyTheme();
    applyTopmost();
    applyFit(false);
    invalidate();
}

// ------------------------------------------------------------------ actions
void App::openFileDialog() {
    wchar_t buf[MAX_PATH * 4] = {};
    wstring filter = decodeFilterString();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!cfg.lastFolder.empty()) ofn.lpstrInitialDir = cfg.lastFolder.c_str();
    if (GetOpenFileNameW(&ofn)) openPath(buf);
}

void App::openFolderDialog() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(hwnd))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        openPath(p);
        CoTaskMemFree(p);
    }
}

void App::revealInExplorer() {
    wstring p = currentPath();
    if (p.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.c_str());
    if (!pidl) return;
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

void App::setAsWallpaper() {
    wstring p = currentPath();
    if (p.empty()) return;
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)p.c_str(),
                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        showToast(T(L"Встановлено як шпалери"));
    else
        showToast(T(L"Не вдалося встановити шпалери"));
}

void App::copyToClipboard() {
    wstring p = currentPath();
    if (p.empty()) return;

    DecodeJob j;
    j.path = p; j.kind = JobKind::Full; j.generation = loader.generation();
    DecodeResult r;
    std::atomic<uint64_t> live{ j.generation };
    runDecodeJob(j, r, live);
    if (!r.ok || !r.img.valid()) { showToast(T(L"Не вдалося скопіювати")); return; }

    size_t rowBytes = (size_t)r.img.w * 4;
    size_t total = sizeof(BITMAPINFOHEADER) + rowBytes * r.img.h;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!hg) return;
    auto* bih = (BITMAPINFOHEADER*)GlobalLock(hg);
    ZeroMemory(bih, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = r.img.w;
    bih->biHeight = r.img.h;                 // bottom-up
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)(rowBytes * r.img.h);

    auto* dst = (uint8_t*)(bih + 1);
    for (int y = 0; y < r.img.h; ++y) {
        const uint8_t* src = r.img.px.data() + (size_t)(r.img.h - 1 - y) * rowBytes;
        uint8_t* out = dst + (size_t)y * rowBytes;
        for (int x = 0; x < r.img.w; ++x) {
            uint32_t a = src[x * 4 + 3];
            if (a == 0) { out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = 255; out[x * 4 + 3] = 0; }
            else if (a == 255) { memcpy(out + x * 4, src + x * 4, 4); }
            else {
                out[x * 4 + 0] = (uint8_t)std::min<uint32_t>(255, src[x * 4 + 0] * 255 / a);
                out[x * 4 + 1] = (uint8_t)std::min<uint32_t>(255, src[x * 4 + 1] * 255 / a);
                out[x * 4 + 2] = (uint8_t)std::min<uint32_t>(255, src[x * 4 + 2] * 255 / a);
                out[x * 4 + 3] = (uint8_t)a;
            }
        }
    }
    GlobalUnlock(hg);

    // Also offer the file itself, so pasting into Explorer or mail works.
    size_t dropSize = sizeof(DROPFILES) + (p.size() + 2) * sizeof(wchar_t);
    HGLOBAL hd = GlobalAlloc(GMEM_MOVEABLE, dropSize);
    if (hd) {
        auto* df = (DROPFILES*)GlobalLock(hd);
        ZeroMemory(df, dropSize);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy((uint8_t*)df + sizeof(DROPFILES), p.c_str(), p.size() * sizeof(wchar_t));
        GlobalUnlock(hd);
    }

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_DIB, hg);
        if (hd) SetClipboardData(CF_HDROP, hd);
        CloseClipboard();
        showToast(T(L"Скопійовано в буфер обміну"));
    } else {
        GlobalFree(hg);
        if (hd) GlobalFree(hd);
    }
}

void App::deleteCurrent() {
    wstring p = currentPath();
    if (p.empty() || !fileExists(p)) return;

    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return;
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOFX_RECYCLEONDELETE);
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item)))) return;
    op->DeleteItem(item.Get(), nullptr);
    if (FAILED(op->PerformOperations())) { showToast(T(L"Не вдалося видалити")); return; }

    pics.erase(p);
    thumbs.erase(p);
    int old = index;
    if (old >= 0 && old < (int)folder.count()) {
        folder.removeAt(old);
        if (folder.count() == 0) { index = -1; pendingPath.clear(); }
        else goTo(clampi(old, 0, (int)folder.count() - 1), true);
    }
    showToast(T(L"Переміщено в кошик"));
    needRelayout = true;
    invalidate();
}

// ----------------------------------------------------------- audio / speed
static const double kRates[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
static const int    kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));

// `wrap` cycles (the button), otherwise the ends hold (wheel and keyboard).
static void videoStepRate(App& a, int dir, bool wrap) {
    if (!a.videoMode) return;
    double cur = a.video.rate();
    int at = 2;
    for (int i = 0; i < kRateCount; ++i) if (fabs(kRates[i] - cur) < 0.01) { at = i; break; }
    int next = wrap ? (at + dir + kRateCount) % kRateCount
                    : clampi(at + dir, 0, kRateCount - 1);
    a.video.setRate(kRates[next]);
    wchar_t b[32];
    swprintf(b, 32, T(L"Швидкість %.2gx"), kRates[next]);
    a.showToast(b, 1.2);
    a.invalidate();
}

static void videoNudgeVolume(App& a, float delta) {
    if (!a.videoMode) return;
    float v = clampf((a.video.muted() ? 0.f : a.video.volume()) + delta, 0.f, 1.f);
    a.video.setVolume(v);
    a.video.setMuted(false);
    a.cfg.volume = clampi((int)lround(v * 100.f), 0, 100);
    a.cfg.muted = false;
    a.showToast(T(L"Гучність ") + std::to_wstring(a.cfg.volume) + L"%", 0.9);
    // Show the flyout too, so the level is visible while it changes.
    a.volPopup = true;
    a.volPopupUntil = nowSec() + 1.1;
    a.invalidate();
}

// ------------------------------------------------------------------ commands
void uiOnCommand(App& a, int cmd) {
    auto pic = a.current();
    bool has = pic && pic->bmp && !pic->failed;
    D2D1_POINT_2F centre{ (a.R.canvas.left + a.R.canvas.right) * .5f,
                          (a.R.canvas.top + a.R.canvas.bottom) * .5f };

    switch (cmd) {
        case CMD_PREV:  a.step(-1); break;
        case CMD_NEXT:  a.step(+1); break;
        case CMD_FIRST: a.goTo(0, true); break;
        case CMD_LAST:  if (a.folder.count()) a.goTo((int)a.folder.count() - 1, true); break;

        case CMD_ZOOM_IN:
            if (a.view == View::Grid) { a.cfg.thumbSize = clampi(a.cfg.thumbSize + 28, 96, 420); a.invalidate(); }
            else if (has) { a.setZoom(a.zoomTarget * 1.25f, centre, true); maybeUpgrade(a); }
            break;
        case CMD_ZOOM_OUT:
            if (a.view == View::Grid) { a.cfg.thumbSize = clampi(a.cfg.thumbSize - 28, 96, 420); a.invalidate(); }
            else if (has) a.setZoom(a.zoomTarget / 1.25f, centre, true);
            break;
        case CMD_FIT:
            if (a.fitMode == Fit::Window && has) { a.fitMode = Fit::Actual; a.applyFit(true); maybeUpgrade(a); }
            else { a.fitMode = Fit::Window; a.applyFit(true); }
            break;
        case CMD_ACTUAL: a.fitMode = Fit::Actual; a.applyFit(true); maybeUpgrade(a); break;

        case CMD_ROT_L: a.rot = (a.rot + 3) % 4; a.applyFit(false); a.autoSizeWindow(); break;
        case CMD_ROT_R: a.rot = (a.rot + 1) % 4; a.applyFit(false); a.autoSizeWindow(); break;

        case CMD_AUTOSIZE: {
            a.cfg.autoSize = (a.cfg.autoSize + 1) % 3;
            const wchar_t* names[] = {
                T(L"Автопідгонка вимкнена  ·  100%"),
                T(L"Масштаб під вікно"),
                T(L"Вікно під зображення")
            };
            a.showToast(names[a.cfg.autoSize], 1.6);
            a.needRelayout = true;
            a.lastMouseMove = nowSec();
            a.fitMode = (a.cfg.autoSize == 0) ? Fit::Actual : Fit::Window;
            a.autoSizeWindow();
            a.applyFit(true);
            a.invalidate();
            break;
        }
        case CMD_FLIP_H: a.flipH = !a.flipH; a.invalidate(); break;
        case CMD_FLIP_V: a.flipV = !a.flipV; a.invalidate(); break;

        case CMD_FULLSCREEN: a.setFullscreen(!a.fullscreen); break;

        case CMD_PLAYPAUSE:
            if (a.videoMode) { a.video.togglePlay(); a.invalidate(); }
            break;
        case CMD_MUTE:
            if (a.videoMode) {
                a.video.setMuted(!a.video.muted());
                a.cfg.muted = a.video.muted();
                a.invalidate();
            }
            break;
        case CMD_SEEK_BACK: if (a.videoMode) { a.videoSeekBy(-5.0); } break;
        case CMD_SEEK_FWD:  if (a.videoMode) { a.videoSeekBy(+5.0); } break;
        case CMD_SPEED:
            if (a.videoMode) videoStepRate(a, +1, true);
            break;
        case CMD_SLIDESHOW:
            a.slideshow = !a.slideshow;
            a.slideshowNext = nowSec() + a.cfg.slideshowMs / 1000.0;
            a.showToast(a.slideshow ? T(L"Слайдшоу увімкнено") : T(L"Слайдшоу вимкнено"));
            break;

        case CMD_GRID:
            if (!a.folderScanned && !a.currentPath().empty())
                a.scanFolderNow(dirOf(a.currentPath()), a.currentPath());
            a.setView(View::Grid);
            break;
        case CMD_VIEWER: a.setView(View::Viewer); break;
        case CMD_TOGGLE_VIEW: uiOnCommand(a, a.view == View::Grid ? CMD_VIEWER : CMD_GRID); break;

        case CMD_INFO:      a.cfg.infoPanel = !a.cfg.infoPanel; a.needRelayout = true;
                            a.autoSizeWindow(); a.invalidate(); break;
        case CMD_FILMSTRIP: a.cfg.filmstrip = !a.cfg.filmstrip; a.needRelayout = true; a.invalidate(); break;

        case CMD_DELETE: a.deleteCurrent(); break;
        case CMD_COPY:   a.copyToClipboard(); break;
        case CMD_OPEN:   a.openFileDialog(); break;
        case CMD_OPEN_FOLDER: a.openFolderDialog(); break;
        case CMD_REVEAL: a.revealInExplorer(); break;
        case CMD_SETWALLPAPER: a.setAsWallpaper(); break;

        case CMD_THEME:
            a.cfg.themeMode = a.th.dark ? 2 : 1;
            a.applyTheme();
            break;

        case CMD_MINIMIZE: ShowWindow(a.hwnd, SW_MINIMIZE); break;
        case CMD_MAXIMIZE: ShowWindow(a.hwnd, IsZoomed(a.hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case CMD_CLOSE:    PostMessageW(a.hwnd, WM_CLOSE, 0, 0); break;

        case CMD_SORT_NAME: case CMD_SORT_DATE: case CMD_SORT_SIZE: case CMD_SORT_TYPE: {
            a.cfg.sortBy = cmd - CMD_SORT_NAME;
            wstring keep = a.currentPath();
            a.folder.resort((SortBy)a.cfg.sortBy, a.cfg.sortDesc);
            a.index = keep.empty() ? a.index : a.folder.indexOf(fileNameOf(keep));
            a.invalidate();
            break;
        }
        case CMD_SORT_DIR: {
            a.cfg.sortDesc = !a.cfg.sortDesc;
            wstring keep = a.currentPath();
            a.folder.resort((SortBy)a.cfg.sortBy, a.cfg.sortDesc);
            a.index = keep.empty() ? a.index : a.folder.indexOf(fileNameOf(keep));
            a.invalidate();
            break;
        }

        case CMD_MORE:
            a.moreMenuOpen = !a.moreMenuOpen;
            a.menuScroll = 0;
            a.menuAnim = 0.f;              // replay the grow-in each time
            a.requestAnim();
            a.invalidate();
            break;

        case CMD_PIN:
            a.cfg.alwaysOnTop = !a.cfg.alwaysOnTop;
            a.applyTopmost();
            a.cfg.save();
            a.showToast(a.cfg.alwaysOnTop ? T(L"Вікно закріплено поверх інших")
                                          : T(L"Закріплення знято"), 1.6);
            break;

        case CMD_SETTINGS:
            a.loadAssociations();
            a.settingsOpen = !a.settingsOpen;
            a.settingsScroll = 0;
            a.moreMenuOpen = false;
            a.invalidate();
            break;

        case CMD_NEWWINDOW: {
            wstring args = L"--new";
            wstring p = a.currentPath();
            if (!p.empty()) args += L" \"" + p + L"\"";
            ShellExecuteW(nullptr, L"open", exePath().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }

        case CMD_ASSOC_APPLY: {
            a.loadAssociations();
            std::vector<wstring> list(a.assocSel.begin(), a.assocSel.end());
            std::sort(list.begin(), list.end());
            wstring joined;
            for (const auto& e : list) { if (!joined.empty()) joined += L","; joined += e; }
            a.cfg.associations = joined;
            unregisterAssociations(a.assocAll);
            if (!list.empty() && registerAssociations(list)) {
                a.assocApplied = true;
                a.showToast(T(L"Зареєстровано форматів: ") + std::to_wstring(list.size()) +
                            T(L". Підтвердіть у Windows"), 3.2);
            } else if (list.empty()) {
                a.assocApplied = false;
                a.showToast(T(L"Реєстрацію прибрано"), 2.0);
            } else {
                a.showToast(T(L"Не вдалося записати в реєстр"), 2.5);
            }
            a.settingsDirty = false;
            a.cfg.save();
            break;
        }

        case CMD_ASSOC_POPULAR: {
            a.loadAssociations();
            for (const wchar_t* e : { L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp",
                                      L".tif", L".tiff", L".heic", L".heif", L".avif", L".ico",
                                      L".jfif", L".jxl", L".psd", L".svg",
                                      L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v",
                                      L".wmv", L".mpg", L".ts" }) {
                for (const auto& have : a.assocAll)
                    if (have == e) { a.assocSel.insert(have); break; }
            }
            a.settingsDirty = true;
            a.invalidate();
            break;
        }

        case CMD_ASSOC_CLEAR:
            a.loadAssociations();
            a.assocSel.clear();
            a.invalidate();
            break;

        case CMD_ASSOC_WINDOWS:
            // Nothing is registered yet on a first run, so write it first -
            // otherwise the Windows page has no PicoView entry to land on.
            if (!a.cfg.associations.empty() || a.assocApplied) openDefaultAppsPage();
            else {
                uiOnCommand(a, CMD_ASSOC_APPLY);
                openDefaultAppsPage();
            }
            break;

        case CMD_HELP: a.showHelp = !a.showHelp; a.invalidate(); break;
        case CMD_ESCAPE:
            if (a.settingsOpen) a.settingsOpen = false;
            else if (a.moreMenuOpen || a.sortMenuOpen) { a.moreMenuOpen = a.sortMenuOpen = false; }
            else if (a.showHelp) a.showHelp = false;
            else if (a.fullscreen) a.setFullscreen(false);
            else if (a.view == View::Grid && a.folder.count()) a.setView(View::Viewer);
            else if (a.slideshow) { a.slideshow = false; }
            else PostMessageW(a.hwnd, WM_CLOSE, 0, 0);
            a.invalidate();
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ results
static void drainResults(App& a) {
    DecodeResult r;
    bool changed = false;
    while (a.loader.pop(r)) {
        if (r.kind == JobKind::Thumb) {
            auto it = a.thumbs.find(r.path);
            if (it == a.thumbs.end()) continue;
            if (r.ok && r.img.valid()) {
                it->second->bmp = a.gfx.upload(r.img);
                it->second->w = r.img.w;
                it->second->h = r.img.h;
                it->second->arrivedAt = nowSec();
            } else it->second->failed = true;
            changed = true;
            continue;
        }

        auto it = a.pics.find(r.path);
        if (it == a.pics.end()) continue;
        auto& pic = it->second;

        if (!r.ok) {
            if (r.kind == JobKind::Preview) continue;        // no thumbnail is fine
            if (r.error == L"cancelled") continue;
            pic->upgrading = false;
            if (!pic->bmp) { pic->failed = true; pic->error = r.error; changed = true; }
            continue;
        }

        // A preview must never replace real pixels.
        if (r.kind == JobKind::Preview && pic->bmp && !pic->preview) continue;
        if (r.kind == JobKind::Preview && pic->bmp && pic->preview && pic->w >= r.img.w) continue;
        if (r.kind == JobKind::Full && pic->bmp && !pic->preview && r.img.w < pic->w) { pic->upgrading = false; continue; }

        auto bmp = a.gfx.upload(r.img);
        if (!bmp) continue;
        pic->bmp = bmp;
        pic->w = r.img.w;
        pic->h = r.img.h;
        pic->hasAlpha = r.hasAlpha;
        pic->preview = (r.kind == JobKind::Preview);
        pic->full = r.isFullRes && r.kind != JobKind::Preview;
        pic->failed = false;
        pic->upgrading = false;
        if (r.srcW > 0) { pic->srcW = r.srcW; pic->srcH = r.srcH; }
        else if (!pic->srcW) { pic->srcW = r.img.w; pic->srcH = r.img.h; }
        if (r.fileSize) pic->fileSize = r.fileSize;
        if (r.mtime.dwLowDateTime || r.mtime.dwHighDateTime) pic->mtime = r.mtime;
        if (r.frameCount > 1) pic->frameCount = r.frameCount;
        if (r.exif.any) { pic->exif = r.exif; pic->exifRead = true; }
        if (r.kind == JobKind::Full) pic->decodeMs = r.decodeMs;

        pgLog("decoded %ls kind=%d %dx%d (src %dx%d) in %.1f ms",
              fileNameOf(r.path).c_str(), (int)r.kind, r.img.w, r.img.h, r.srcW, r.srcH, r.decodeMs);
        if (a.currentPath() == r.path) {
            if (a.fitMode != Fit::Free) a.applyFit(false);
            a.takePendingView();
            a.autoSizeWindow();
            if (!a.firstImageShown) {
                a.firstImageShown = true;
                a.firstImageMs = (nowSec() - a.startupAt) * 1000.0;
                pgLog("FIRST PIXELS after %.1f ms (kind=%d decode=%.1f ms %dx%d)",
                      a.firstImageMs, (int)r.kind, r.decodeMs, r.img.w, r.img.h);
            }
            if (a.fadeIn < 1.f) a.requestAnim();
        }
        changed = true;
    }
    if (changed) { a.trimCaches(); a.invalidate(); }
}

// ------------------------------------------------------------------ render
static void stepAnimations(App& a, double dt) {
    float k = 1.f - powf(0.00008f, (float)clampf((float)dt, 0.f, 0.1f));

    if (fabsf(a.zoom - a.zoomTarget) > std::max(0.0004f, a.zoomTarget * 0.0015f)) {
        a.zoom += (a.zoomTarget - a.zoom) * k;
        a.requestAnim();
    } else a.zoom = a.zoomTarget;

    if (fabsf(a.panX - a.panTargetX) > 0.4f || fabsf(a.panY - a.panTargetY) > 0.4f) {
        a.panX += (a.panTargetX - a.panX) * k;
        a.panY += (a.panTargetY - a.panY) * k;
        a.requestAnim();
    } else { a.panX = a.panTargetX; a.panY = a.panTargetY; }

    if (a.fadeIn < 1.f) { a.fadeIn = clampf(a.fadeIn + (float)dt * 5.0f, 0.f, 1.f); a.requestAnim(); }

    // Playback needs a steady stream of repaints to pull frames.
    if (a.videoMode && a.video.playing()) a.requestAnim();

    double idle = nowSec() - a.lastMouseMove;
    bool keepBar = a.cfg.barHideMs < 0;           // the overlays never fade
    // At zero the bars follow the pointer and go the moment it stops, but they
    // still need a floor so they stay up while it is actually moving.
    double hideAfter = std::max(a.cfg.barHideMs / 1000.0, 0.12);
    bool wantBar = (a.view == View::Viewer || a.titleOverlay()) &&
                   (keepBar || idle < hideAfter || a.hot != 0 || a.in.down || a.seekDragging ||
                    a.settingsOpen || a.moreMenuOpen || a.volPopup || a.showHelp ||
                    (a.videoMode && !a.video.playing()));
    float target = wantBar ? 1.f : 0.f;
    if (fabsf(a.barAlpha - target) > 0.004f) {
        a.barAlpha += (target - a.barAlpha) * (float)(1.0 - pow(0.0005, clampf((float)dt, 0.f, 0.1f)));
        a.requestAnim();
    } else {
        a.barAlpha = target;
        // Nothing else is animating once the bar is up, so the frame loop would
        // stop and the fade-out would never start. Wake up exactly when the idle
        // period runs out instead of painting continuously.
        if (target == 1.f && a.hwnd && !keepBar) {
            double left = hideAfter - idle;
            if (left > 0) SetTimer(a.hwnd, TIMER_BARHIDE, (UINT)(left * 1000.0) + 40, nullptr);
        }
    }
}

static void render(App& a) {
    if (!a.gfx.dc) return;
    double t = nowSec();
    double dt = a.lastFrame > 0 ? std::min(0.1, t - a.lastFrame) : 0.016;
    a.lastFrame = t;
    a.frameDt = dt;          // ui.cpp eases everything against this

    D2D1_RECT_F prevCanvas = a.R.canvas;
    if (a.needRelayout) uiLayout(a);
    bool canvasMoved = prevCanvas.left != a.R.canvas.left || prevCanvas.top != a.R.canvas.top ||
                       prevCanvas.right != a.R.canvas.right || prevCanvas.bottom != a.R.canvas.bottom;
    if (canvasMoved) {
        if (a.fitMode != Fit::Free) a.applyFit(false);
        else a.clampPan();          // hand-set zoom survives a panel opening
    }

    a.animating = false;
    stepAnimations(a, dt);
    a.clampPan();

    if (!a.gfx.begin()) return;
    uiFrame(a);
    a.gfx.end();

    // Apply straight away: WM_SETCURSOR arrives before the move that changed
    // what is under the pointer, so waiting for it would lag one frame behind.
    if (a.in.hasMouse && a.wantCursor != a.shownCursor) {
        a.shownCursor = a.wantCursor;
        SetCursor(LoadCursorW(nullptr, a.wantCursor));
    }

    if (a.pendingCmd != CMD_NONE) {
        int cmd = a.pendingCmd;
        a.pendingCmd = CMD_NONE;
        uiOnCommand(a, cmd);
    }

    if (a.animating) SetTimer(a.hwnd, TIMER_ANIM, 8, nullptr);
    else KillTimer(a.hwnd, TIMER_ANIM);
}

// ------------------------------------------------------------------ input
static void onKey(App& a, WPARAM key) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    a.lastMouseMove = nowSec();

    if (ctrl) {
        switch (key) {
            case 'C': uiOnCommand(a, CMD_COPY); return;
            case 'O': uiOnCommand(a, CMD_OPEN); return;
            case 'D': uiOnCommand(a, CMD_OPEN_FOLDER); return;
            case 'N': uiOnCommand(a, CMD_NEWWINDOW); return;
            case VK_OEM_COMMA: uiOnCommand(a, CMD_SETTINGS); return;
            case VK_OEM_PLUS: case VK_ADD: uiOnCommand(a, CMD_ZOOM_IN); return;
            case VK_OEM_MINUS: case VK_SUBTRACT: uiOnCommand(a, CMD_ZOOM_OUT); return;
            case '0': uiOnCommand(a, CMD_FIT); return;
        }
        return;
    }

    if (a.videoMode && a.view == View::Viewer && a.video.isOpen()) {
        switch (key) {
            case VK_SPACE: uiOnCommand(a, CMD_PLAYPAUSE); a.invalidate(); return;
            case VK_LEFT:  a.videoSeekBy(shift ? -30.0 : -5.0); return;
            case VK_RIGHT: a.videoSeekBy(shift ? +30.0 : +5.0); return;
            case VK_UP:
            case VK_DOWN:
                videoNudgeVolume(a, key == VK_UP ? 0.05f : -0.05f);
                return;
            case 'M': uiOnCommand(a, CMD_MUTE);
                      a.showToast(a.video.muted() ? T(L"Звук вимкнено") : T(L"Звук увімкнено"), 0.9);
                      return;
            case 'S': uiOnCommand(a, CMD_SPEED); return;
            case VK_PRIOR: a.step(-1); return;
            case VK_NEXT:  a.step(+1); return;
            default: break;
        }
    }

    switch (key) {
        case VK_LEFT: case VK_PRIOR: case VK_UP:
            if (a.view == View::Grid && key == VK_UP) { a.gridScrollTarget -= a.gridRowH; a.requestAnim(); a.invalidate(); }
            else if (a.view == View::Grid) a.goTo(a.index - (key == VK_PRIOR ? a.gridCols * 3 : 1), true);
            else uiOnCommand(a, CMD_PREV);
            break;
        case VK_RIGHT: case VK_NEXT: case VK_DOWN:
            if (a.view == View::Grid && key == VK_DOWN) { a.gridScrollTarget += a.gridRowH; a.requestAnim(); a.invalidate(); }
            else if (a.view == View::Grid) a.goTo(a.index + (key == VK_NEXT ? a.gridCols * 3 : 1), true);
            else uiOnCommand(a, CMD_NEXT);
            break;
        case VK_HOME: uiOnCommand(a, CMD_FIRST); break;
        case VK_END:  uiOnCommand(a, CMD_LAST); break;
        case VK_ADD: case VK_OEM_PLUS: uiOnCommand(a, CMD_ZOOM_IN); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: uiOnCommand(a, CMD_ZOOM_OUT); break;
        case '0': a.fitMode = Fit::Window; a.applyFit(true); break;
        case '1': uiOnCommand(a, CMD_ACTUAL); break;
        case 'F': case VK_F11: uiOnCommand(a, CMD_FULLSCREEN); break;
        case 'G': uiOnCommand(a, a.view == View::Grid ? CMD_VIEWER : CMD_GRID); break;
        case 'I': uiOnCommand(a, CMD_INFO); break;
        case 'T': uiOnCommand(a, CMD_FILMSTRIP); break;
        case 'R': uiOnCommand(a, shift ? CMD_ROT_L : CMD_ROT_R); break;
        case 'H': uiOnCommand(a, CMD_FLIP_H); break;
        case 'V': uiOnCommand(a, CMD_FLIP_V); break;
        case 'A': uiOnCommand(a, CMD_AUTOSIZE); break;
        case 'P': uiOnCommand(a, CMD_PIN); break;
        case 'W': a.cfg.wheelMode = 1 - a.cfg.wheelMode;
                  a.showToast(a.cfg.wheelMode ? T(L"Колесо: гортання") : T(L"Колесо: масштаб")); break;
        case VK_DELETE: uiOnCommand(a, CMD_DELETE); break;
        case VK_SPACE:
            if (a.view == View::Viewer) uiOnCommand(a, CMD_SLIDESHOW);
            break;
        case VK_RETURN:
            if (a.view == View::Grid) a.setView(View::Viewer);
            break;
        case VK_F1: uiOnCommand(a, CMD_HELP); break;
        case VK_ESCAPE: uiOnCommand(a, CMD_ESCAPE); break;
        default: break;
    }
    a.invalidate();
}

static void onWheel(App& a, int delta, POINT ptClient) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    a.lastMouseMove = nowSec();

    if (a.settingsOpen) {
        a.settingsScroll = clampf(a.settingsScroll - delta * a.gfx.s(0.9f), 0.f, a.settingsScrollMax);
        a.invalidate();
        return;
    }
    if (a.moreMenuOpen && a.menuScrollMax > 0.f) {
        a.menuScroll = clampf(a.menuScroll - delta * a.gfx.s(0.6f), 0.f, a.menuScrollMax);
        a.invalidate();
        return;
    }

    // Ctrl + wheel rides the volume, Ctrl+Shift + wheel the playback speed.
    // There is nothing to zoom in a video, so the modifier is free here.
    if (a.videoMode && a.view == View::Viewer && ctrl) {
        if (shift) videoStepRate(a, delta > 0 ? +1 : -1, false);
        else       videoNudgeVolume(a, delta > 0 ? 0.05f : -0.05f);
        return;
    }

    if (a.view == View::Grid) {
        if (ctrl) a.cfg.thumbSize = clampi(a.cfg.thumbSize + (delta > 0 ? 24 : -24), 96, 420);
        else a.gridScrollTarget -= delta * a.gfx.s(1.1f);
        a.requestAnim(); a.invalidate();
        return;
    }
    if (a.R.filmstrip.bottom > a.R.filmstrip.top &&
        ptClient.y >= a.R.filmstrip.top && ptClient.y <= a.R.filmstrip.bottom) {
        a.step(delta > 0 ? -1 : 1);
        return;
    }

    bool zoomIt = ctrl || (a.cfg.wheelMode == 0);
    if (zoomIt) {
        auto pic = a.current();
        if (!pic || !pic->bmp) return;
        float f = powf(1.0016f, (float)delta);
        a.setZoom(a.zoomTarget * f, D2D1::Point2F((float)ptClient.x, (float)ptClient.y), true);
        maybeUpgrade(a);
    } else {
        a.step(delta > 0 ? -1 : 1);
    }
}

// ------------------------------------------------------------------ wndproc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* pa = g_app;
    if (!pa) return DefWindowProcW(hwnd, msg, wp, lp);
    App& a = *pa;

    switch (msg) {
        case WM_NCCALCSIZE: {
            // Never hand this to DefWindowProc: doing so re-enables the standard
            // DWM caption and we end up with two title bars.
            if (a.fullscreen) return 0;
            // wParam FALSE hands us a bare RECT*, TRUE hands us NCCALCSIZE_PARAMS.
            // The very first message a window gets is the FALSE one, so both must
            // be handled or the standard caption survives.
            RECT* rc = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            UINT dpi = GetDpiForWindow(hwnd);
            int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            rc->left += fx;
            rc->right -= fx;
            rc->bottom -= fy;
            // Leaving `top` alone drops the caption while keeping the resize grip.
            if (IsZoomed(hwnd)) rc->top += fy;
            pgLog("NCCALCSIZE dpi=%u fx=%d fy=%d -> %ld,%ld,%ld,%ld", dpi, fx, fy,
                  rc->left, rc->top, rc->right, rc->bottom);
            return 0;
        }

        case WM_NCHITTEST: {
            if (a.fullscreen) return HTCLIENT;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT wr{}; GetWindowRect(hwnd, &wr);
            int bw = (int)(8 * a.gfx.dpi / 96);
            bool zoomed = IsZoomed(hwnd) != 0;
            if (!zoomed) {
                bool l = pt.x < wr.left + bw, r = pt.x >= wr.right - bw;
                bool t = pt.y < wr.top + bw, b = pt.y >= wr.bottom - bw;
                if (t && l) return HTTOPLEFT;   if (t && r) return HTTOPRIGHT;
                if (b && l) return HTBOTTOMLEFT; if (b && r) return HTBOTTOMRIGHT;
                if (l) return HTLEFT; if (r) return HTRIGHT;
                if (t) return HTTOP;  if (b) return HTBOTTOM;
            }
            POINT cp = pt; ScreenToClient(hwnd, &cp);
            return uiHitTestCaption(a, cp);
        }

        // Keep the custom caption buttons live while Windows owns the pointer.
        case WM_NCMOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            a.in.mouse = D2D1::Point2F((float)pt.x, (float)pt.y);
            a.in.hasMouse = true;
            a.lastMouseMove = nowSec();
            a.invalidate();
            if (wp == HTMAXBUTTON) return 0;
            break;
        }
        case WM_NCMOUSELEAVE:
            a.in.hasMouse = false;
            a.invalidate();
            break;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) { a.in.pressed = true; a.in.down = true; a.invalidate(); return 0; }
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) { a.in.released = true; a.in.down = false; a.invalidate(); return 0; }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, a.wantCursor));
                a.shownCursor = a.wantCursor;
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            a.in.mouse = D2D1::Point2F((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            a.in.hasMouse = true;
            a.lastMouseMove = nowSec();
            a.invalidate();
            return 0;
        }
        case WM_MOUSELEAVE:
            a.in.hasMouse = false;
            a.in.down = false;
            a.dragging = false;
            a.invalidate();
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            a.in.mouse = D2D1::Point2F((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            a.in.down = true; a.in.pressed = true; a.in.hasMouse = true;
            a.lastMouseMove = nowSec();
            a.invalidate();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            a.in.mouse = D2D1::Point2F((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            a.in.down = false; a.in.released = true;
            a.invalidate();
            return 0;
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            a.in.mouse = D2D1::Point2F((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            a.in.doubleClick = true; a.in.hasMouse = true;
            a.in.down = true; a.in.pressed = true;   // also a normal press for widgets
            a.lastMouseMove = nowSec();
            a.invalidate();
            return 0;
        case WM_MBUTTONUP:
            uiOnCommand(a, CMD_FULLSCREEN);
            return 0;
        case WM_XBUTTONUP:
            uiOnCommand(a, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? CMD_PREV : CMD_NEXT);
            return 0;
        case WM_RBUTTONUP:
            uiOnCommand(a, CMD_TOGGLE_VIEW);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            onWheel(a, GET_WHEEL_DELTA_WPARAM(wp), pt);
            return 0;
        }

        case WM_KEYDOWN: onKey(a, wp); return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN) { uiOnCommand(a, CMD_FULLSCREEN); return 0; }
            break;

        case WM_DROPFILES: {
            HDROP hd = (HDROP)wp;
            wchar_t buf[MAX_PATH * 2] = {};
            if (DragQueryFileW(hd, 0, buf, MAX_PATH * 2)) {
                SetForegroundWindow(hwnd);
                a.openPath(buf);
            }
            DragFinish(hd);
            return 0;
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && a.gfx.dc) {
                a.gfx.resize(LOWORD(lp), HIWORD(lp));
                a.needRelayout = true;
                uiLayout(a);
                if (a.fitMode != Fit::Free) a.applyFit(false);
                a.invalidate();
            }
            return 0;

        case WM_DPICHANGED: {
            a.gfx.setDpi(HIWORD(wp));
            RECT* nr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            a.needRelayout = true;
            a.invalidate();
            return 0;
        }

        case WM_SIZING: {
            // "Window follows the picture" also means the frame cannot be
            // dragged into a different aspect ratio.
            if (a.cfg.autoSize != 2 || a.fullscreen || IsZoomed(hwnd)) break;
            int sw = 0, sh = 0;
            if (!a.sourceSize(sw, sh)) break;
            if (a.rot & 1) std::swap(sw, sh);

            RECT* r = (RECT*)lp;
            UINT dpi = a.gfx.dpi ? a.gfx.dpi : 96;
            int titleH = a.titleOverlay() ? 0 : (int)lround(40.0 * dpi / 96.0);
            int infoW = a.cfg.infoPanel ? (int)lround(320.0 * dpi / 96.0) : 0;
            int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            double ar = (double)sw / std::max(1, sh);
            int canvasW = (r->right - r->left) - fx * 2 - infoW;
            int canvasH = (r->bottom - r->top) - fy - titleH;
            if (canvasW < 80 || canvasH < 60) break;

            bool vertEdge = (wp == WMSZ_TOP || wp == WMSZ_BOTTOM);
            if (vertEdge) canvasW = (int)lround(canvasH * ar);
            else          canvasH = (int)lround(canvasW / ar);

            int newW = canvasW + infoW + fx * 2;
            int newH = canvasH + titleH + fy;

            if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - newW;
            else r->right = r->left + newW;
            if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - newH;
            else r->bottom = r->top + newH;
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = (LONG)(420 * a.gfx.dpi / 96);
            mmi->ptMinTrackSize.y = (LONG)(320 * a.gfx.dpi / 96);
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (lp && _wcsicmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) a.applyTheme();
            break;

        case WM_COPYDATA: {
            auto* cds = (COPYDATASTRUCT*)lp;
            if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                wstring path((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
                while (!path.empty() && path.back() == 0) path.pop_back();
                if (!path.empty()) {
                    a.startupAt = nowSec();
                    a.firstImageShown = false;
                    a.folderScanned = false;
                    a.openPath(path);
                    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                }
            }
            return TRUE;
        }

        case WM_PG_VIDEO: {
            unsigned flags = a.video.onEvent((unsigned)wp, (uintptr_t)lp);
            if (!flags) return 0;
            if ((flags & PGV_SIZED) && a.pendingResume > 0) {
                double dur = a.video.duration();
                if (dur <= 0 || a.pendingResume < dur - 5.0) {
                    a.video.seek(a.pendingResume);
                    a.showToast(T(L"Продовжено з ") + formatTime(a.pendingResume), 2.2);
                }
                a.pendingResume = -1;
            }
            if (flags & PGV_SIZED) {
                a.needRelayout = true;
                if (a.fitMode != Fit::Free) a.applyFit(false);
                a.takePendingView();
                a.autoSizeWindow();
            }
            if (flags & PGV_QUIET) SetTimer(hwnd, TIMER_UNQUIET, 110, nullptr);
            if (flags & PGV_ERROR) a.showToast(a.video.error(), 3.0);
            if ((flags & PGV_ENDED) && a.slideshow && a.folder.count() > 1) a.step(1);
            a.invalidate();
            return 0;
        }

        case WM_PG_DECODED:
            drainResults(a);
            return 0;

        case WM_PG_SCANNED: {
            if (!a.folderScanned && !a.pendingPath.empty()) {
                wstring keep = a.pendingPath;
                a.folder.scan(dirOf(keep), (SortBy)a.cfg.sortBy, a.cfg.sortDesc);
                a.folderScanned = true;
                a.cfg.lastFolder = dirOf(keep);
                int i = a.folder.indexOf(fileNameOf(keep));
                a.index = i >= 0 ? i : -1;
                if (a.index < 0 && a.folder.count()) a.index = 0;
                a.needRelayout = true;
                a.preload();
                a.invalidate();
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_ANIM) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wp == TIMER_BARHIDE) {
                KillTimer(hwnd, TIMER_BARHIDE);
                a.invalidate();
                return 0;
            }
            if (wp == TIMER_UNQUIET) {
                KillTimer(hwnd, TIMER_UNQUIET);
                a.video.endQuiet();
                return 0;
            }
            if (wp == TIMER_SLIDESHOW) {
                if (a.slideshow && a.view == View::Viewer && nowSec() >= a.slideshowNext) {
                    auto pic = a.current();
                    if (pic && pic->bmp && !pic->preview) a.step(1);
                }
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            render(a);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE: {
            if (!a.fullscreen && !IsIconic(hwnd)) {
                WINDOWPLACEMENT wpl{ sizeof(wpl) };
                GetWindowPlacement(hwnd, &wpl);
                a.cfg.maximized = (wpl.showCmd == SW_SHOWMAXIMIZED);
                a.cfg.placement.left = wpl.rcNormalPosition.left;
                a.cfg.placement.top = wpl.rcNormalPosition.top;
                a.cfg.placement.right = wpl.rcNormalPosition.right - wpl.rcNormalPosition.left;
                a.cfg.placement.bottom = wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top;
            }
            a.storeResume();
            a.rememberView();
            a.cfg.lastFile = a.currentPath();
            {
                auto vs = a.viewStates.find(a.cfg.lastFile);
                if (vs != a.viewStates.end()) {
                    a.cfg.lastZoom = vs->second.zoom;
                    a.cfg.lastFit = vs->second.fit;
                    a.cfg.lastRot = vs->second.rot;
                    a.cfg.lastFlip = (vs->second.flipH ? 1 : 0) | (vs->second.flipV ? 2 : 0);
                }
            }
            a.cfg.fullscreen = a.fullscreen;
            a.leaveVideo();
            a.cfg.save();
            a.resume.save(a.cfg.file());
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ entry
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    App app;
    g_app = &app;
    app.inst = hInst;
    app.startupAt = nowSec();
    app.lastMouseMove = app.startupAt;

    pgLog("t+%.1f entry", (nowSec() - app.startupAt) * 1000);
    app.cfg.load();
    langInit();
    g_lang.store(clampi(app.cfg.lang, 0, 2), std::memory_order_relaxed);
    app.resume.load(app.cfg.file());

    wstring openArg;
    bool forceNewWindow = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (!argv[i]) continue;
                if (argv[i][0] == L'-' || argv[i][0] == L'/') {
                    if (_wcsicmp(argv[i] + 1, L"new") == 0 || _wcsicmp(argv[i] + 1, L"-new") == 0 ||
                        _wcsicmp(argv[i] + 1, L"n") == 0)
                        forceNewWindow = true;
                } else if (openArg.empty()) openArg = argv[i];
            }
            LocalFree(argv);
        }
    }

    // Re-using a running instance turns a ~300 ms cold start into a ~30 ms hand-off.
    // This runs before any GPU or codec work so the forwarding process exits at once.
    // Hold Shift while opening (or pass --new) to get a separate window anyway.
    HANDLE instanceMutex = nullptr;
    if (app.cfg.singleInstance && !forceNewWindow && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        instanceMutex = CreateMutexW(nullptr, TRUE, L"PicoView.SingleInstance.v1");
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowW(kClass, nullptr);
            if (other) {
                if (!openArg.empty()) {
                    wchar_t full[MAX_PATH * 2] = {};
                    if (!GetFullPathNameW(openArg.c_str(), MAX_PATH * 2, full, nullptr))
                        wcsncpy_s(full, openArg.c_str(), _TRUNCATE);
                    COPYDATASTRUCT cds{};
                    cds.dwData = 1;
                    cds.cbData = (DWORD)((wcslen(full) + 1) * sizeof(wchar_t));
                    cds.lpData = full;
                    SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
                }
                if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
                CloseHandle(instanceMutex);
                CoUninitialize();
                return 0;   // nothing heavyweight was started, so this is instant
            }
        }
    }

    Gfx::warmStart();
    decodeInit();
    app.view = app.cfg.viewMode ? View::Grid : View::Viewer;
    pgLog("t+%.1f decodeInit (%zu ext)", (nowSec() - app.startupAt) * 1000, decodeExtensions().size());

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = nullptr;            // the app picks the cursor per widget
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 820;
    if (app.cfg.placement.right > 300 && app.cfg.placement.bottom > 200) {
        RECT test{ app.cfg.placement.left, app.cfg.placement.top,
                   app.cfg.placement.left + app.cfg.placement.right,
                   app.cfg.placement.top + app.cfg.placement.bottom };
        if (MonitorFromRect(&test, MONITOR_DEFAULTTONULL)) {
            x = test.left; y = test.top;
            w = app.cfg.placement.right; h = app.cfg.placement.bottom;
        }
    }

    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES,
                                kClass, L"PicoView", WS_OVERLAPPEDWINDOW,
                                x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    app.hwnd = hwnd;
    pgLog("t+%.1f window created", (nowSec() - app.startupAt) * 1000);

    // Windows 11 chrome.
    int corner = PG_DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = PG_DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    app.applyTheme();

    // Seed geometry from the freshly created window so the very first decode
    // already targets the right number of pixels.
    {
        RECT cr{};
        GetClientRect(hwnd, &cr);
        UINT d = GetDpiForWindow(hwnd);
        app.gfx.dpi = d ? d : 96;
        app.gfx.width = std::max<int>(1, cr.right - cr.left);
        app.gfx.height = std::max<int>(1, cr.bottom - cr.top);
        uiLayout(app);
    }

    // Start decoding before the GPU stack is up: the two overlap.
    app.loader.start(hwnd);
    if (!openArg.empty()) app.openPath(openArg);
    else if (!app.cfg.lastFile.empty() && fileExists(app.cfg.lastFile)) {
        if (app.cfg.rememberZoom && app.cfg.lastZoom > 0.01f) {
            ViewState v;
            v.zoom = app.cfg.lastZoom;
            v.fit = app.cfg.lastFit;
            v.rot = app.cfg.lastRot;
            v.flipH = (app.cfg.lastFlip & 1) != 0;
            v.flipV = (app.cfg.lastFlip & 2) != 0;
            app.viewStates[app.cfg.lastFile] = v;
            app.viewStateLru.push_back(app.cfg.lastFile);
        }
        app.openPath(app.cfg.lastFile);
    }
    else if (app.view == View::Grid && !app.cfg.lastFolder.empty() && dirExists(app.cfg.lastFolder))
        app.scanFolderNow(app.cfg.lastFolder, L"");

    pgLog("t+%.1f decode submitted", (nowSec() - app.startupAt) * 1000);
    if (!app.gfx.init(hwnd)) {
        MessageBoxW(nullptr, T(L"Не вдалося ініціалізувати Direct3D / Direct2D."), L"PicoView", MB_ICONERROR);
        return 2;
    }
    pgLog("t+%.1f gfx ready (composed=%d)", (nowSec() - app.startupAt) * 1000, (int)app.gfx.composed);
    if (app.videoMode && !app.video.isOpen() && !app.pendingPath.empty())
        app.openVideo(app.pendingPath);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    app.applyTopmost();
    app.needRelayout = true;
    uiLayout(app);
    app.applyFit(false);

    {
        RECT wr{}, cr{}; POINT o{ 0, 0 };
        GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr); ClientToScreen(hwnd, &o);
        pgLog("win=%ld,%ld,%ld,%ld client=%ldx%ld origin=%ld,%ld dpi=%u composed=%d",
              wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, o.x, o.y,
              app.gfx.dpi, (int)app.gfx.composed);
        pgLog("canvas=%.0f,%.0f,%.0f,%.0f gfx=%dx%d", app.R.canvas.left, app.R.canvas.top,
              app.R.canvas.right, app.R.canvas.bottom, app.gfx.width, app.gfx.height);
    }
    ShowWindow(hwnd, app.cfg.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    if (app.cfg.fullscreen) app.setFullscreen(true);
    UpdateWindow(hwnd);
    pgLog("t+%.1f window shown", (nowSec() - app.startupAt) * 1000);
    SetTimer(hwnd, TIMER_SLIDESHOW, 200, nullptr);
    DragAcceptFiles(hwnd, TRUE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app.loader.stop();
    app.preview.close();
    app.video.shutdown();
    app.pics.clear();
    app.thumbs.clear();
    app.gfx.shutdown();
    if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
    CoUninitialize();
    return 0;
}
