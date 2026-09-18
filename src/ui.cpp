// Layout, painting and hit-testing. Immediate-mode: every widget both draws
// itself and reports its own interaction inside the same pass, which keeps the
// hit-test order identical to the paint order.
#include "app.h"

// --------------------------------------------------------------- glyphs
namespace ico {
    static const wchar_t* Photo = L"";
    static const wchar_t* ChevL = L"";
    static const wchar_t* ChevR = L"";
    static const wchar_t* ZoomIn = L"";
    static const wchar_t* ZoomOut = L"";
    static const wchar_t* FitPage = L"";
    static const wchar_t* Rotate = L"";   // mirrored for the other direction
    static const wchar_t* Play = L"";
    static const wchar_t* Pause = L"";
    static const wchar_t* FullScr = L"";
    static const wchar_t* BackWin = L"";
    static const wchar_t* Info = L"";
    static const wchar_t* Delete = L"";
    static const wchar_t* Copy = L"";
    static const wchar_t* OpenFile = L"";
    static const wchar_t* Folder = L"";
    static const wchar_t* Grid = L"";
    static const wchar_t* Film = L"";
    static const wchar_t* Sort = L"";
    static const wchar_t* Sun = L"";
    static const wchar_t* Moon = L"";
    static const wchar_t* Help = L"";
    static const wchar_t* Min = L"";
    static const wchar_t* Max = L"";
    static const wchar_t* Restore = L"";
    static const wchar_t* Close = L"";
    static const wchar_t* Reveal = L"";
    static const wchar_t* Wallpaper = L"";
    static const wchar_t* Up = L"";
    static const wchar_t* Down = L"";
    static const wchar_t* PrevTrack = L"";
    static const wchar_t* NextTrack = L"";
    static const wchar_t* Back = L"";
    static const wchar_t* Fwd = L"";
    static const wchar_t* Volume = L"";
    static const wchar_t* Vol0 = L"";
    static const wchar_t* Vol1 = L"";
    static const wchar_t* Vol2 = L"";
    static const wchar_t* Vol3 = L"";
    static const wchar_t* Speed = L"";
    static const wchar_t* Flip = L"";   // turned 90 deg for the vertical one
    static const wchar_t* Mute = L"";
    static const wchar_t* Video = L"";
    static const wchar_t* More = L"";
    static const wchar_t* Zoom100 = L"";
    static const wchar_t* AutoOff = L"";
    static const wchar_t* AutoFit = L"";
    static const wchar_t* AutoWin = L"";
    static const wchar_t* Settings = L"";
    static const wchar_t* NewWindow = L"";
    static const wchar_t* Check = L"";
    static const wchar_t* Pin = L"";
    static const wchar_t* Unpin = L"";
    static const wchar_t* Compress = L"";
}

// --------------------------------------------------------------- frame-local
static bool   g_overUi = false;       // pointer is over an interactive surface
static int    g_hotPrev = 0;
static double g_hotSince = 0;
static wstring g_tip;
static D2D1_RECT_F g_tipAnchor{};
// An open flyout covers whatever is beneath it; those widgets must not light up.
static bool   g_blockOn = false;
static D2D1_RECT_F g_block{};

// Widgets announce the pointer they want; the last (topmost) one drawn wins.
static inline void useCursor(App& a, LPCWSTR id) { a.wantCursor = id; }

// How much of the way to its target a value moves this frame. `perSec` is the
// fraction still left after one second, so the motion looks the same at 60 and
// at 165 Hz instead of racing on a fast display.
static inline float easeK(App& a, float perSec) {
    return 1.f - powf(perSec, clampf((float)a.frameDt, 0.f, 0.1f));
}
static inline bool easeTo(App& a, float& v, float target, float perSec, float eps = 0.004f) {
    if (fabsf(v - target) <= eps) { v = target; return false; }
    v += (target - v) * easeK(a, perSec);
    a.requestAnim();
    return true;
}

// Every clickable surface eases its hover and press state rather than snapping,
// so the whole UI reacts in one visual language.
struct WidgetAnim { float hover = 0.f, press = 0.f; };
static std::unordered_map<int, WidgetAnim> g_wanim;

static void animState(App& a, int id, bool hovered, bool held, float& hv, float& pr) {
    WidgetAnim& w = g_wanim[id];
    easeTo(a, w.hover, hovered ? 1.f : 0.f, 1e-7f);
    easeTo(a, w.press, held ? 1.f : 0.f, 1e-14f);
    hv = w.hover; pr = w.press;
}

// Glyph transforms, so one icon can serve a mirrored or turned pair and the set
// stays visually consistent.
enum { GX_NONE = 0, GX_MIRROR = 1, GX_ROT90 = 2 };

static D2D1_MATRIX_3X2_F glyphXform(int xf, float press, D2D1_RECT_F r) {
    D2D1_POINT_2F c{ (r.left + r.right) * .5f, (r.top + r.bottom) * .5f };
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
    if (xf == GX_MIRROR)     m = D2D1::Matrix3x2F::Scale(-1.f, 1.f, c);
    else if (xf == GX_ROT90) m = D2D1::Matrix3x2F::Rotation(90.f, c);
    if (press > 0.002f) m = m * D2D1::Matrix3x2F::Scale(1.f - press * 0.09f, 1.f - press * 0.09f, c);
    return m;
}

static inline bool inRect(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
static inline D2D1_RECT_F rectOf(float x, float y, float w, float h) { return { x, y, x + w, y + h }; }
static inline D2D1_RECT_F inflate(D2D1_RECT_F r, float d) { return { r.left - d, r.top - d, r.right + d, r.bottom + d }; }
static inline float rw(const D2D1_RECT_F& r) { return r.right - r.left; }
static inline float rh(const D2D1_RECT_F& r) { return r.bottom - r.top; }

static D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
static D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

static wstring fitText(Gfx& g, const wstring& s, IDWriteTextFormat* f, float maxW) {
    if (s.empty() || maxW <= 0) return L"";
    if (g.measure(s, f).width <= maxW) return s;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (g.measure(s.substr(0, mid) + L"…", f).width <= maxW) lo = mid; else hi = mid - 1;
    }
    return s.substr(0, lo) + L"…";
}

static wstring fmtMP(int w, int h) {
    double mp = (double)w * h / 1e6;
    wchar_t b[32];
    swprintf(b, 32, mp < 10 ? L"%.1f" : L"%.0f", mp);
    return b;
}

// Widget identity has to be unique per call site. The same command shows up in
// several places (grid lives in both the title bar and the command bar); if they
// shared an id they would cancel each other's press/release tracking.
enum UiId {
    UI_TB_GRID = 1000, UI_TB_VIEWER, UI_TB_OPEN, UI_TB_HELP, UI_TB_THEME,
    UI_TB_MIN, UI_TB_MAX, UI_TB_CLOSE,
    UI_EMPTY_OPEN = 1100, UI_EMPTY_FOLDER,
    UI_INFO_CLOSE = 1200, UI_INFO_REVEAL, UI_INFO_WALL,
    UI_HELP_CLOSE = 1300,
    UI_GH_SORTDIR = 1400, UI_GH_SORT, UI_GH_SMALLER, UI_GH_BIGGER,
    UI_BAR_BASE = 1500,
    UI_V_PREV = 1700, UI_V_BACK, UI_V_PLAY, UI_V_FWD, UI_V_NEXT, UI_V_GRID,
    UI_V_MUTE, UI_V_VOL, UI_V_VOLV, UI_V_SPEED, UI_V_FULL, UI_V_INFO, UI_V_DEL, UI_V_SEEK,
    UI_V_MORE, UI_BAR_MORE,
    UI_MENU_BASE = 1800,
    UI_SET_BASE = 2000,
    UI_ASSOC_BASE = 2400,
    UI_COMP_BASE = 2600
};

// --------------------------------------------------------------- widgets
struct BtnStyle {
    bool  toggled = false;
    bool  enabled = true;
    bool  danger = false;
    bool  accentFill = false;
    float radius = 6.f;
};

// Shared by every pill-shaped control: resolves the interaction, eases the
// hover and press amounts and paints the blended background.
static bool btnCore(App& a, int id, D2D1_RECT_F r, const BtnStyle& st, float opacity,
                    D2D1_COLOR_F& bgOut, D2D1_COLOR_F& fgOut, float& pressOut) {
    Gfx& g = a.gfx;
    bool blocked = g_blockOn && inRect(g_block, a.in.mouse);
    bool hovered = st.enabled && !blocked && inRect(r, a.in.mouse) && a.in.hasMouse;
    if (hovered) { g_overUi = true; a.hot = id; useCursor(a, IDC_HAND); }
    bool held = hovered && a.in.down && a.active == id;
    bool clicked = false;
    if (hovered && a.in.pressed) a.active = id;
    if (a.in.released && a.active == id) { if (hovered) clicked = true; a.active = 0; }

    float hv = 0.f, pr = 0.f;
    animState(a, id, hovered, held, hv, pr);
    pressOut = pr;

    const D2D1_COLOR_F clear = D2D1::ColorF(0, 0, 0, 0);
    D2D1_COLOR_F rest = clear, hover = clear, press = clear;
    D2D1_COLOR_F fg = st.enabled ? a.th.text : a.th.textMute;
    if (st.accentFill) {
        rest = a.th.accent; hover = a.th.accentHover; press = mix(a.th.accent, a.th.canvas, .18f);
        fg = a.th.accentText;
    } else if (st.danger) {
        hover = a.th.dangerHover; press = a.th.danger;
        fg = mix(fg, D2D1::ColorF(1, 1, 1, 1), hv);
    } else if (st.toggled) {
        rest = alpha(a.th.accent, 0.20f); hover = alpha(a.th.accent, 0.30f); press = alpha(a.th.accent, 0.38f);
        fg = a.th.accent;
    } else {
        hover = a.th.cardHover; press = a.th.cardPress;
    }
    bgOut = mix(mix(rest, hover, hv), press, pr);
    fgOut = fg;

    // A pressed control sinks a little, the way Windows 11 buttons do.
    D2D1_RECT_F fill = inflate(r, -pr * g.s(1.5f));
    g.roundRect(fill, g.s(st.radius), alpha(bgOut, opacity));
    if (st.toggled && !st.accentFill) {
        float w = (r.right - r.left) * 0.34f * (1.f - pr * 0.25f), cx = (r.left + r.right) * .5f;
        g.roundRect({ cx - w / 2, r.bottom - g.s(3.f), cx + w / 2, r.bottom - g.s(1.f) },
                    g.s(1.f), alpha(a.th.accent, opacity));
    }
    return clicked;
}

static bool button(App& a, int uid, int cmd, D2D1_RECT_F r, const wchar_t* glyph, const wchar_t* tip,
                   BtnStyle st = {}, float opacity = 1.f, int xform = GX_NONE) {
    const int id = uid;
    Gfx& g = a.gfx;
    D2D1_COLOR_F bg, fg;
    float pr = 0.f;
    bool clicked = btnCore(a, id, r, st, opacity, bg, fg, pr);

    if (glyph && *glyph) {
        bool xf = (xform != GX_NONE) || pr > 0.002f;
        if (xf) g.dc->SetTransform(glyphXform(xform, pr, r));
        g.text(glyph, g.fIcon.Get(), r, alpha(fg, opacity), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (xf) g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    if (a.hot == id && tip && *tip) { g_tip = tip; g_tipAnchor = r; }
    if (clicked && st.enabled && cmd != CMD_NONE) a.pendingCmd = cmd;
    return clicked && st.enabled;
}

static bool textButton(App& a, int uid, int cmd, D2D1_RECT_F r, const wstring& label, const wchar_t* tip,
                       BtnStyle st = {}, float opacity = 1.f) {
    const int id = uid;
    Gfx& g = a.gfx;
    D2D1_COLOR_F bg, fg;
    float pr = 0.f;
    bool clicked = btnCore(a, id, r, st, opacity, bg, fg, pr);

    if (pr > 0.002f) g.dc->SetTransform(glyphXform(GX_NONE, pr, r));
    g.text(label, g.fBody.Get(), r, alpha(fg, opacity), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (pr > 0.002f) g.dc->SetTransform(D2D1::Matrix3x2F::Identity());

    if (a.hot == id && tip && *tip) { g_tip = tip; g_tipAnchor = r; }
    if (clicked && st.enabled && cmd != CMD_NONE) a.pendingCmd = cmd;
    return clicked && st.enabled;
}

// Horizontal slider. `value` is 0..1; returns true while the user drags it and
// writes the live position into `out`.
static bool slider(App& a, int uid, D2D1_RECT_F r, float value, float& out,
                   float opacity, float trackH, bool showKnob = true) {
    Gfx& g = a.gfx;
    D2D1_RECT_F hit = { r.left - g.s(4.f), r.top - g.s(8.f), r.right + g.s(4.f), r.bottom + g.s(8.f) };
    bool blocked = g_blockOn && inRect(g_block, a.in.mouse);
    bool hovered = !blocked && inRect(hit, a.in.mouse) && a.in.hasMouse;
    if (hovered) { g_overUi = true; a.hot = uid; useCursor(a, IDC_HAND); }
    if (hovered && a.in.pressed) a.active = uid;
    bool dragging = (a.active == uid) && a.in.down;
    if (dragging) useCursor(a, IDC_HAND);
    if (a.active == uid && a.in.released) { dragging = false; a.active = 0; }

    float v = clampf(value, 0.f, 1.f);
    if (dragging || (a.active == uid && a.in.released))
        v = clampf((a.in.mouse.x - r.left) / std::max(1.f, rw(r)), 0.f, 1.f);
    out = v;

    float hv = 0.f, pr = 0.f;
    animState(a, uid, hovered, dragging, hv, pr);
    float th = g.s(trackH) * (1.f + 0.6f * std::max(hv, pr));
    float cy = (r.top + r.bottom) * .5f;
    D2D1_RECT_F track = { r.left, cy - th * .5f, r.right, cy + th * .5f };
    g.roundRect(track, th * .5f, alpha(a.th.text, opacity * 0.22f));

    D2D1_RECT_F fill = { r.left, track.top, r.left + rw(r) * v, track.bottom };
    if (fill.right > fill.left) g.roundRect(fill, th * .5f, alpha(a.th.accent, opacity));

    float kg = std::max(hv, pr);
    if (showKnob && kg > 0.01f) {
        float kr = g.s(6.f) * kg;
        g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fill.right, cy), kr, kr),
                          g.solid(alpha(a.th.accent, opacity)));
    }
    return dragging;
}

// Vertical twin of `slider`, full at the top. Used by the volume flyout.
static bool vslider(App& a, int uid, D2D1_RECT_F r, float value, float& out,
                    float opacity, float trackW, bool live = true) {
    Gfx& g = a.gfx;
    D2D1_RECT_F hit = { r.left - g.s(10.f), r.top - g.s(6.f), r.right + g.s(10.f), r.bottom + g.s(6.f) };
    bool hovered = live && inRect(hit, a.in.mouse) && a.in.hasMouse;
    if (hovered) { g_overUi = true; a.hot = uid; useCursor(a, IDC_HAND); }
    if (hovered && a.in.pressed) a.active = uid;
    bool dragging = (a.active == uid) && a.in.down;
    if (dragging) useCursor(a, IDC_HAND);
    if (a.active == uid && a.in.released) { dragging = false; a.active = 0; }

    float v = clampf(value, 0.f, 1.f);
    if (dragging) v = 1.f - clampf((a.in.mouse.y - r.top) / std::max(1.f, rh(r)), 0.f, 1.f);
    out = v;

    float hv = 0.f, pr = 0.f;
    animState(a, uid, hovered, dragging, hv, pr);
    float tw = g.s(trackW) * (1.f + 0.6f * std::max(hv, pr));
    float cx = (r.left + r.right) * .5f;
    D2D1_RECT_F track = { cx - tw * .5f, r.top, cx + tw * .5f, r.bottom };
    g.roundRect(track, tw * .5f, alpha(a.th.text, opacity * 0.22f));

    float fy = r.bottom - rh(r) * v;
    if (r.bottom > fy)
        g.roundRect({ track.left, fy, track.right, r.bottom }, tw * .5f, alpha(a.th.accent, opacity));

    float kg = std::max(hv, pr);
    if (kg > 0.01f)
        g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, fy), g.s(6.f) * kg, g.s(6.f) * kg),
                          g.solid(alpha(a.th.accent, opacity)));
    return dragging;
}

// Barely-there lift. Two very faint rings instead of a visible halo - a heavy
// drop shadow reads as a smudge on light photos.
static void shadowPill(Gfx& g, D2D1_RECT_F r, float radius, const D2D1_COLOR_F& sh, float opacity) {
    for (int i = 2; i >= 1; --i) {
        float d = g.s((float)i * 2.f);
        g.roundRect(inflate(r, d), radius + d, alpha(sh, 0.030f * opacity));
    }
}

// --------------------------------------------------------------- flyout menu
struct MenuEntry {
    int            cmd = CMD_NONE;
    const wchar_t* glyph = nullptr;
    wstring        label;
    wstring        hint;
    bool           toggled = false;
    bool           enabled = true;
    bool           separator = false;
    int            xform = 0;      // GX_* - lets one glyph serve a mirrored pair
};

// Windows 11 style flyout, anchored to a button on the bottom bar so it opens
// upwards. Picking an item posts its command and closes.
static void popupMenu(App& a, int uidBase, D2D1_RECT_F anchor,
                      const std::vector<MenuEntry>& items, bool& open) {
    if (!open) return;
    Gfx& g = a.gfx;

    float ih = g.s(34.f), sepH = g.s(9.f), pad = g.s(4.f);
    float w = g.s(190.f);
    for (const auto& it : items) {
        if (it.separator) continue;
        float need = g.s(58.f) + g.measure(it.label, g.fBody.Get()).width;
        if (!it.hint.empty()) need += g.s(18.f) + g.measure(it.hint, g.fCaption.Get()).width;
        w = std::max(w, need);
    }
    w = std::min(w, std::max(g.s(190.f), (float)g.width - g.s(24.f)));

    float wantH = pad * 2;
    for (const auto& it : items) wantH += it.separator ? sepH : ih;

    // Never taller than the window: what does not fit scrolls.
    float maxH = (float)g.height - g.s(16.f);
    float h = std::min(wantH, maxH);
    a.menuScrollMax = std::max(0.f, wantH - h);
    a.menuScroll = clampf(a.menuScroll, 0.f, a.menuScrollMax);

    float x = clampf(anchor.right - w, g.s(8.f), std::max(g.s(8.f), (float)g.width - w - g.s(8.f)));
    float y = anchor.top - h - g.s(8.f);
    if (y < g.s(8.f)) y = std::min(anchor.bottom + g.s(8.f), (float)g.height - h - g.s(8.f));
    y = clampf(y, g.s(8.f), std::max(g.s(8.f), (float)g.height - h - g.s(8.f)));
    D2D1_RECT_F m = rectOf(x, y, w, h);
    a.moreMenuBounds = m;

    // Windows 11 flyouts grow into place instead of appearing whole.
    easeTo(a, a.menuAnim, 1.f, 2e-10f, 0.002f);
    float op = clampf(a.menuAnim * 1.35f, 0.f, 1.f);
    bool animating = a.menuAnim < 0.999f;
    if (animating) {
        float dy = (1.f - a.menuAnim) * g.s(10.f) * (y < anchor.top ? 1.f : -1.f);
        g.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                               D2D1::IdentityMatrix(), op), nullptr);
        g.dc->SetTransform(D2D1::Matrix3x2F::Translation(0.f, dy));
    }

    shadowPill(g, m, g.s(8.f), a.th.shadow, 1.f);
    g.roundRect(m, g.s(8.f), alpha(a.th.bar, 0.99f));
    g.roundRectStroke(m, g.s(8.f), a.th.barStroke, g.s(1.f));
    if (inRect(inflate(m, g.s(6.f)), a.in.mouse) && a.in.hasMouse) g_overUi = true;

    g.dc->PushAxisAlignedClip(m, D2D1_ANTIALIAS_MODE_ALIASED);
    float iy = m.top + pad - a.menuScroll;
    int idx = 0;
    for (const auto& it : items) {
        if (it.separator) {
            g.dc->FillRectangle(rectOf(m.left + g.s(12.f), iy + sepH * .5f, w - g.s(24.f), g.s(1.f)),
                                g.solid(a.th.stroke));
            iy += sepH; ++idx;
            continue;
        }
        D2D1_RECT_F ir = rectOf(m.left + pad, iy, w - pad * 2, ih);
        if (ir.bottom < m.top || ir.top > m.bottom) { iy += ih; ++idx; continue; }
        bool hovered = it.enabled && inRect(ir, a.in.mouse) && a.in.hasMouse;
        if (hovered) {
            g_overUi = true; a.hot = uidBase + idx; useCursor(a, IDC_HAND);
            g.roundRect(ir, g.s(5.f), a.th.cardHover);
        }
        if (it.toggled)
            g.roundRect(rectOf(ir.left + g.s(3.f), (ir.top + ir.bottom) * .5f - g.s(8.f), g.s(3.f), g.s(16.f)),
                        g.s(1.5f), a.th.accent);

        D2D1_COLOR_F fg = it.enabled ? a.th.text : a.th.textMute;
        if (it.glyph) {
            IDWriteTextFormat* gf = (it.glyph[0] >= 0xE000) ? g.fIcon.Get() : g.fBody.Get();
            D2D1_RECT_F gr = rectOf(ir.left + g.s(8.f), ir.top, g.s(26.f), ih);
            if (it.xform != GX_NONE) g.dc->SetTransform(glyphXform(it.xform, 0.f, gr));
            g.text(it.glyph, gf, gr, it.toggled ? a.th.accent : fg, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (it.xform != GX_NONE) g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        g.text(it.label, g.fBody.Get(), rectOf(ir.left + g.s(42.f), ir.top, rw(ir) - g.s(50.f), ih), fg);
        if (!it.hint.empty())
            g.text(it.hint, g.fCaption.Get(), rectOf(ir.left, ir.top, rw(ir) - g.s(12.f), ih),
                   a.th.textMute, DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (hovered && a.in.released) { a.pendingCmd = it.cmd; open = false; }
        iy += ih; ++idx;
    }
    g.dc->PopAxisAlignedClip();

    if (a.menuScrollMax > 1.f) {
        float trackH = h - g.s(10.f);
        float thumbH = std::max(g.s(24.f), trackH * (h / std::max(1.f, wantH)));
        float t = a.menuScroll / a.menuScrollMax;
        g.roundRect(rectOf(m.right - g.s(7.f), m.top + g.s(5.f) + t * (trackH - thumbH),
                           g.s(3.f), thumbH), g.s(1.5f), alpha(a.th.text, 0.30f));
    }

    if (animating) {
        g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
        g.dc->PopLayer();
    }

    if (a.in.pressed && !inRect(m, a.in.mouse) && !inRect(anchor, a.in.mouse)) open = false;
}

static const wchar_t* autoSizeGlyph(int mode) {
    return mode == 0 ? ico::AutoOff : mode == 1 ? ico::AutoFit : ico::AutoWin;
}
static const wchar_t* autoSizeTip(int mode) {
    return mode == 0 ? T(L"Автопідгонка: вимкнена  (A)")
         : mode == 1 ? T(L"Автопідгонка: масштаб під вікно  (A)")
                     : T(L"Автопідгонка: вікно під зображення  (A)");
}
static const wchar_t* autoSizeLabel(int mode) {
    return mode == 0 ? T(L"Автопідгонка: вимкнена")
         : mode == 1 ? T(L"Автопідгонка: під вікно")
                     : T(L"Автопідгонка: вікно під фото");
}

// Everything that is not worth a permanent slot on the bar.
static std::vector<MenuEntry> buildActionMenu(App& a, bool forVideo) {
    bool has = forVideo ? a.video.isOpen() : (a.current() && a.current()->bmp && !a.current()->failed);
    std::vector<MenuEntry> v;
    auto add = [&](int cmd, const wchar_t* glyph, const wchar_t* label, const wchar_t* hint,
                   bool toggled = false, bool enabled = true, int xform = GX_NONE) {
        MenuEntry e; e.cmd = cmd; e.glyph = glyph; e.label = label; e.hint = hint ? hint : L"";
        e.toggled = toggled; e.enabled = enabled; e.xform = xform;
        v.push_back(std::move(e));
    };
    auto sep = [&] { MenuEntry e; e.separator = true; v.push_back(std::move(e)); };

    add(CMD_FIT, ico::FitPage, T(L"Вписати у вікно"), L"0", a.fitMode == Fit::Window, has);
    add(CMD_ACTUAL, ico::Zoom100, T(L"Масштаб 100%"), L"1", a.fitMode == Fit::Actual, has);
    add(CMD_AUTOSIZE, autoSizeGlyph(a.cfg.autoSize), autoSizeLabel(a.cfg.autoSize), L"A");
    sep();
    add(CMD_ROT_L, ico::Rotate, T(L"Обернути ліворуч"), L"Shift+R", false, has, GX_MIRROR);
    add(CMD_ROT_R, ico::Rotate, T(L"Обернути праворуч"), L"R", false, has);
    add(CMD_FLIP_H, ico::Flip, T(L"Дзеркально по горизонталі"), L"H", a.flipH, has);
    add(CMD_FLIP_V, ico::Flip, T(L"Дзеркально по вертикалі"), L"V", a.flipV, has, GX_ROT90);
    sep();
    if (forVideo) {
        wchar_t sp[24];
        swprintf(sp, 24, T(L"Швидкість %.2gx"), a.video.rate());
        add(CMD_SPEED, ico::Speed, sp, L"S", fabs(a.video.rate() - 1.0) > 0.01, a.video.isOpen());
        add(CMD_MUTE, a.video.muted() ? ico::Mute : ico::Volume,
            a.video.muted() ? T(L"Увімкнути звук") : T(L"Вимкнути звук"), L"M", a.video.muted());
        sep();
    }
    add(CMD_GRID, ico::Grid, T(L"Сітка папки"), L"G");
    add(CMD_FILMSTRIP, ico::Film, T(L"Стрічка кадрів"), L"T", a.cfg.filmstrip, a.folder.count() > 1);
    add(CMD_INFO, ico::Info, T(L"Відомості"), L"I", a.cfg.infoPanel);
    sep();
    if (!forVideo) add(CMD_COMPRESS, ico::Compress, T(L"Стиснути та конвертувати"), L"Ctrl+E", false, has);
    if (!forVideo) add(CMD_COPY, ico::Copy, T(L"Копіювати"), L"Ctrl+C", false, has);
    add(CMD_REVEAL, ico::Reveal, T(L"Показати в провіднику"), nullptr);
    add(CMD_NEWWINDOW, ico::NewWindow, T(L"Відкрити нове вікно"), L"Ctrl+N");
    add(CMD_PIN, a.cfg.alwaysOnTop ? ico::Unpin : ico::Pin, T(L"Поверх усіх вікон"), L"P",
        a.cfg.alwaysOnTop);
    if (!forVideo) add(CMD_SETWALLPAPER, ico::Wallpaper, T(L"Зробити шпалерами"), nullptr, false, has);
    add(CMD_DELETE, ico::Delete, T(L"Перемістити в кошик"), L"Del");
    sep();
    add(CMD_SETTINGS, ico::Settings, T(L"Налаштування"), L"Ctrl+,");
    return v;
}

// --------------------------------------------------------------- layout
void uiLayout(App& a) {
    Gfx& g = a.gfx;
    a.R.client = rectOf(0, 0, (float)g.width, (float)g.height);

    float tb = a.fullscreen ? 0.f : g.s(40.f);
    a.R.titlebar = rectOf(0, 0, (float)g.width, tb);
    float contentTop = a.titleOverlay() ? 0.f : tb;

    float btnW = g.s(46.f);
    a.R.btnClose = rectOf(g.width - btnW, 0, btnW, tb);
    a.R.btnMax = rectOf(g.width - btnW * 2, 0, btnW, tb);
    a.R.btnMin = rectOf(g.width - btnW * 3, 0, btnW, tb);
    a.R.caption = rectOf(0, 0, g.width - btnW * 3 - g.s(96.f), tb);

    float compW = 0.f;
    if (a.compAnim > 0.002f)
        compW = std::min(g.s(340.f), (float)g.width * 0.6f) * a.compAnim;
    float infoW = (a.cfg.infoPanel && !a.fullscreen && compW <= 0.f && g.width > g.s(760.f))
                      ? g.s(320.f) : 0.f;
    // The panel is a solid surface with its own header, so it always starts
    // below the title bar - even where the title bar floats over the picture,
    // or its heading would collide with the window buttons.
    float compTop = a.fullscreen ? 0.f : tb;
    a.R.comp = rectOf(g.width - compW, compTop, compW, g.height - compTop);
    a.R.info = rectOf(g.width - infoW, contentTop, infoW, g.height - contentTop);
    a.R.content = rectOf(0, contentTop, g.width - infoW - compW, g.height - contentTop);

    if (a.view == View::Grid) {
        float hh = g.s(56.f);
        a.R.gridHeader = rectOf(a.R.content.left, a.R.content.top, rw(a.R.content), hh);
        a.R.grid = rectOf(a.R.content.left, a.R.content.top + hh, rw(a.R.content), rh(a.R.content) - hh);
        a.R.canvas = a.R.grid;
        a.R.filmstrip = rectOf(0, 0, 0, 0);
    } else {
        // The filmstrip is an overlay: it fades in with the command bar and never
        // takes space away from the picture.
        float fs = (a.cfg.filmstrip && a.folder.count() > 1 && rh(a.R.content) > g.s(300.f))
                   ? g.s(88.f) : 0.f;
        a.R.canvas = a.R.content;
        a.R.filmstrip = rectOf(a.R.content.left, a.R.content.bottom - fs, rw(a.R.content), fs);
        a.R.gridHeader = rectOf(0, 0, 0, 0);
        a.R.grid = rectOf(0, 0, 0, 0);
    }
    a.needRelayout = false;
}

int uiHitTestCaption(App& a, POINT pt) {
    if (a.fullscreen) return HTCLIENT;
    // A hidden overlay strip must not swallow clicks meant for the picture.
    if (a.titleOverlay() && a.barAlpha <= 0.25f) return HTCLIENT;
    D2D1_POINT_2F p{ (float)pt.x, (float)pt.y };
    if (inRect(a.R.btnMax, p)) return HTMAXBUTTON;
    if (inRect(a.R.btnMin, p) || inRect(a.R.btnClose, p)) return HTCLIENT;
    if (inRect(a.R.caption, p)) {
        // Both clusters of the title bar hold real buttons; only the strip
        // between them drags the window.
        if (p.x < a.gfx.s(84.f)) return HTCLIENT;
        if (p.x >= a.R.btnMin.left - a.gfx.s(76.f)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

// --------------------------------------------------------------- titlebar
static void drawTitlebar(App& a) {
    Gfx& g = a.gfx;
    if (a.fullscreen) return;
    D2D1_RECT_F r = a.R.titlebar;

    float top = 1.f;
    if (a.titleOverlay()) {
        top = a.barAlpha;
        if (top <= 0.01f) return;
        // Square off the bottom so the strip meets the rounded window corners.
        D2D1_COLOR_F bg = alpha(a.th.bar, top * 0.94f);
        g.roundRect(r, g.s(8.f), bg);
        g.dc->FillRectangle(rectOf(r.left, r.bottom - g.s(8.f), rw(r), g.s(8.f)), g.solid(bg));
        g.dc->FillRectangle(rectOf(r.left, r.bottom - g.s(1.f), rw(r), g.s(1.f)),
                            g.solid(alpha(a.th.barStroke, top)));
    }

    float pad = g.s(10.f);
    float bs = g.s(32.f);
    float x = pad;
    float cy = (r.top + r.bottom) * .5f;

    // Left cluster: back to grid / open.
    if (a.view == View::Viewer) {
        button(a, UI_TB_GRID, CMD_GRID, rectOf(x, cy - bs / 2, bs, bs), ico::Grid, T(L"Сітка папки  (G)"),
               {}, top);
        x += bs + g.s(2.f);
    } else {
        button(a, UI_TB_VIEWER, CMD_VIEWER, rectOf(x, cy - bs / 2, bs, bs), ico::Photo, T(L"Переглядач  (Esc)"),
               { false, a.folder.count() > 0 }, top);
        x += bs + g.s(2.f);
    }
    button(a, UI_TB_OPEN, CMD_OPEN, rectOf(x, cy - bs / 2, bs, bs), ico::OpenFile, T(L"Відкрити файл  (Ctrl+O)"),
           {}, top);
    x += bs + g.s(10.f);

    // Title text.
    wstring title, sub;
    auto pic = a.current();
    if (a.view == View::Grid) {
        title = a.folder.dir().empty() ? L"PicoView" : fileNameOf(a.folder.dir());
        sub = a.folder.count() ? (std::to_wstring(a.folder.count()) + T(L" зображень")) : L"";
    } else if (a.videoMode) {
        title = fileNameOf(a.currentPath());
        if (a.video.width() > 0)
            sub = std::to_wstring(a.video.width()) + L" × " + std::to_wstring(a.video.height());
        double d = a.video.duration();
        if (d > 0) sub += (sub.empty() ? L"" : L"  ·  ") + formatTime(d);
        if (a.folder.count() > 1 && a.index >= 0)
            sub += L"  ·  " + std::to_wstring(a.index + 1) + L"/" + std::to_wstring(a.folder.count());
    } else if (a.index >= 0 && a.index < (int)a.folder.count()) {
        title = a.folder.at(a.index).name;
        if (pic && !pic->failed && pic->srcW > 0) {
            sub = std::to_wstring(pic->srcW) + L" × " + std::to_wstring(pic->srcH);
            if (pic->fileSize) sub += L"  ·  " + humanSize(pic->fileSize);
            if (a.folder.count() > 1) sub += L"  ·  " + std::to_wstring(a.index + 1) + L"/" + std::to_wstring(a.folder.count());
        }
    } else {
        title = L"PicoView";
        sub = T(L"Швидкий переглядач зображень");
    }

    float availR = a.R.btnMin.left - g.s(84.f);
    float availW = std::max(g.s(40.f), availR - x);
    float tW = std::min(availW, g.measure(title, g.fBodyStrong.Get()).width);
    wstring t = fitText(g, title, g.fBodyStrong.Get(), availW);
    g.text(t, g.fBodyStrong.Get(), rectOf(x, r.top, availW, rh(r)), alpha(a.th.text, top));

    if (!sub.empty()) {
        float sx = x + tW + g.s(12.f);
        float sw = availR - sx;
        if (sw > g.s(60.f))
            g.text(fitText(g, sub, g.fCaption.Get(), sw), g.fCaption.Get(),
                   rectOf(sx, r.top, sw, rh(r)), alpha(a.th.textDim, top));
    }

    // Right cluster before the window buttons.
    // Only the two controls worth a permanent slot; the theme switch and the
    // shortcut sheet live in Settings.
    float rx = a.R.btnMin.left - g.s(4.f) - bs;
    button(a, UI_SET_BASE + 200, CMD_SETTINGS, rectOf(rx, cy - bs / 2, bs, bs), ico::Settings,
           T(L"Налаштування  (Ctrl+,)"), { a.settingsOpen }, top);
    rx -= bs + g.s(2.f);
    button(a, UI_SET_BASE + 201, CMD_PIN, rectOf(rx, cy - bs / 2, bs, bs),
           a.cfg.alwaysOnTop ? ico::Unpin : ico::Pin,
           a.cfg.alwaysOnTop ? T(L"Відкріпити  (P)") : T(L"Поверх усіх вікон  (P)"),
           { a.cfg.alwaysOnTop }, top);

    // Window buttons.
    bool zoomed = IsZoomed(a.hwnd) != 0;
    button(a, UI_TB_MIN, CMD_MINIMIZE, a.R.btnMin, ico::Min, nullptr, { false, true, false, false, 0.f }, top);
    button(a, UI_TB_MAX, CMD_MAXIMIZE, a.R.btnMax, zoomed ? ico::Restore : ico::Max, nullptr,
           { false, true, false, false, 0.f }, top);
    button(a, UI_TB_CLOSE, CMD_CLOSE, a.R.btnClose, ico::Close, nullptr,
           { false, true, true, false, 0.f }, top);
}

// --------------------------------------------------------------- viewer
static void drawCheckerboard(Gfx& g, D2D1_RECT_F r, const Theme& th) {
    float c = g.s(12.f);
    bool lightCanvas = (th.canvas.r + th.canvas.g + th.canvas.b) / 3.f > 0.5f;
    D2D1_COLOR_F a1 = lightCanvas ? D2D1::ColorF(0.85f, 0.85f, 0.86f, 1) : D2D1::ColorF(0.16f, 0.16f, 0.17f, 1);
    D2D1_COLOR_F a2 = lightCanvas ? D2D1::ColorF(0.92f, 0.92f, 0.93f, 1) : D2D1::ColorF(0.21f, 0.21f, 0.22f, 1);
    g.dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    g.dc->Clear(a1);
    int cols = (int)(rw(r) / c) + 2, rows = (int)(rh(r) / c) + 2;
    for (int y = 0; y < rows; ++y)
        for (int x = (y & 1); x < cols; x += 2)
            g.dc->FillRectangle(rectOf(r.left + x * c, r.top + y * c, c, c), g.solid(a2));
    g.dc->PopAxisAlignedClip();
}

// Fluent progress ring: one arc that sweeps around a faint track, its length
// breathing as it turns. Replaces the old ring of blinking dots.
static void drawSpinner(Gfx& g, D2D1_POINT_2F c, float radius, const D2D1_COLOR_F& col, double t) {
    float th = std::max(g.s(1.6f), radius * 0.20f);
    g.dc->DrawEllipse(D2D1::Ellipse(c, radius, radius), g.solid(alpha(col, 0.16f)), th);

    double spin = t * 1.05;
    float sweep = (float)(105.0 + 80.0 * (0.5 - 0.5 * cos(spin * 2.6)));
    float start = (float)fmod(spin * 260.0, 360.0) - sweep * .5f;

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(g.d2dFactory->CreatePathGeometry(&path))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;
    auto at = [&](float deg) {
        float rad = deg * 3.14159265f / 180.f;
        return D2D1::Point2F(c.x + cosf(rad) * radius, c.y + sinf(rad) * radius);
    };
    sink->BeginFigure(at(start), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(at(start + sweep), D2D1::SizeF(radius, radius), 0.f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweep > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    static ComPtr<ID2D1StrokeStyle> round;
    if (!round) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
        sp.startCap = sp.endCap = D2D1_CAP_STYLE_ROUND;
        g.d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &round);
    }
    g.dc->DrawGeometry(path.Get(), g.solid(alpha(col, 0.95f)), th, round.Get());
}

static void drawEmptyState(App& a, D2D1_RECT_F r) {
    Gfx& g = a.gfx;
    float w = std::min(g.s(460.f), rw(r) - g.s(48.f));
    float h = g.s(260.f);
    D2D1_RECT_F card = rectOf((r.left + r.right - w) / 2, (r.top + r.bottom - h) / 2, w, h);

    g.roundRect(card, g.s(12.f), a.th.dark ? alpha(D2D1::ColorF(1, 1, 1, 1), 0.045f) : alpha(D2D1::ColorF(1, 1, 1, 1), 0.55f));
    g.roundRectStroke(card, g.s(12.f), a.th.stroke, g.s(1.f));

    float y = card.top + g.s(34.f);
    g.text(ico::Photo, g.fIconBig.Get(), rectOf(card.left, y, w, g.s(40.f)), a.th.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += g.s(52.f);
    g.text(T(L"Перетягніть зображення сюди"), g.fTitle.Get(), rectOf(card.left, y, w, g.s(30.f)),
           a.th.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += g.s(36.f);
    wstring fmt = T(L"Підтримується ") + std::to_wstring(decodeExtensions().size()) + T(L" форматів");
    g.text(fmt, g.fCaption.Get(), rectOf(card.left, y, w, g.s(20.f)), a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);

    y += g.s(40.f);
    float bw = g.s(150.f), bh = g.s(34.f);
    textButton(a, UI_EMPTY_OPEN, CMD_OPEN, rectOf((card.left + card.right - bw) / 2, y, bw, bh), T(L"Відкрити файл"), nullptr,
               { false, true, false, true, 5.f });
    y += bh + g.s(8.f);
    textButton(a, UI_EMPTY_FOLDER, CMD_OPEN_FOLDER, rectOf((card.left + card.right - bw) / 2, y, bw, bh), T(L"Відкрити папку"), nullptr,
               { false, true, false, false, 5.f });
}

static void drawVideoFrame(App& a, D2D1_RECT_F cv);

static void drawVideoFrame(App& a, D2D1_RECT_F cv) {
    Gfx& g = a.gfx;
    ID2D1Bitmap1* bmp = a.video.metaKnown() ? a.video.frame(g.dc.Get()) : nullptr;

    if (!bmp) {
        wstring err = a.video.error();
        if (!err.empty()) {
            float w = std::min(g.s(440.f), rw(cv) - g.s(48.f));
            D2D1_RECT_F card = rectOf((cv.left + cv.right - w) / 2,
                                      (cv.top + cv.bottom) / 2 - g.s(54.f), w, g.s(108.f));
            g.roundRect(card, g.s(10.f), alpha(a.th.danger, 0.16f));
            g.roundRectStroke(card, g.s(10.f), alpha(a.th.danger, 0.55f), g.s(1.f));
            g.text(fileNameOf(a.currentPath()), g.fBodyStrong.Get(),
                   rectOf(card.left + g.s(16.f), card.top + g.s(20.f), w - g.s(32.f), g.s(24.f)),
                   D2D1::ColorF(1, 1, 1, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
            g.text(err, g.fCaption.Get(),
                   rectOf(card.left + g.s(16.f), card.top + g.s(50.f), w - g.s(32.f), g.s(40.f)),
                   D2D1::ColorF(1, 1, 1, 0.78f), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            drawSpinner(g, D2D1::Point2F((cv.left + cv.right) / 2, (cv.top + cv.bottom) / 2),
                        g.s(15.f), a.th.textDim, nowSec());
            a.requestAnim();
        }
        return;
    }

    float srcW = (float)std::max(1, a.video.width());
    float srcH = (float)std::max(1, a.video.height());
    float cx = (cv.left + cv.right) * .5f + a.panX;
    float cy = (cv.top + cv.bottom) * .5f + a.panY;

    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-srcW * .5f, -srcH * .5f);
    m = m * D2D1::Matrix3x2F::Scale(a.zoom, a.zoom);
    if (a.rot) m = m * D2D1::Matrix3x2F::Rotation(a.rot * 90.f);
    if (a.flipH) m = m * D2D1::Matrix3x2F::Scale(-1.f, 1.f);
    if (a.flipV) m = m * D2D1::Matrix3x2F::Scale(1.f, -1.f);
    m = m * D2D1::Matrix3x2F::Translation(cx, cy);

    g.dc->SetTransform(m);
    g.dc->DrawBitmap(bmp, rectOf(0, 0, srcW, srcH), clampf(a.fadeIn, 0.f, 1.f),
                     D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());

    // Skipping: a brief badge on the side you jumped towards.
    double left = a.seekFlashUntil - nowSec();
    if (left > 0 && a.seekFlashDir != 0) {
        float t = clampf((float)(left / 0.75), 0.f, 1.f);      // 1 -> 0
        float fade = t < 0.35f ? t / 0.35f : 1.f;
        float grow = 1.f + (1.f - t) * 0.18f;
        float rad = clampf(std::min(rw(cv), rh(cv)) * 0.075f, g.s(44.f), g.s(96.f)) * grow;
        float side = a.seekFlashDir < 0 ? 0.24f : 0.76f;
        D2D1_POINT_2F c{ cv.left + rw(cv) * side, (cv.top + cv.bottom) * .5f };

        g.dc->FillEllipse(D2D1::Ellipse(c, rad, rad), g.solid(D2D1::ColorF(0, 0, 0, 0.36f * fade)));
        // Two chevrons from the icon font read as a fast-forward arrow.
        g.text(a.seekFlashDir < 0 ? L"" : L"", g.fIconBig.Get(),
               rectOf(c.x - rad, c.y - rad - g.s(14.f), rad * 2, rad * 2),
               D2D1::ColorF(1, 1, 1, 0.95f * fade), DWRITE_TEXT_ALIGNMENT_CENTER);
        wchar_t lbl[32];
        swprintf(lbl, 32, T(L"%.0f с"), a.seekFlashAmount);
        g.text(lbl, g.fBodyStrong.Get(),
               rectOf(c.x - rad, c.y + g.s(10.f), rad * 2, g.s(26.f)),
               D2D1::ColorF(1, 1, 1, 0.92f * fade), DWRITE_TEXT_ALIGNMENT_CENTER);
        a.requestAnim();
    }

    // Paused: a soft play badge in the middle, like every other player.
    if (!a.video.playing() && !a.seekDragging) {
        float rad = g.s(34.f);
        D2D1_POINT_2F c{ (cv.left + cv.right) * .5f, (cv.top + cv.bottom) * .5f };
        g.dc->FillEllipse(D2D1::Ellipse(c, rad, rad), g.solid(D2D1::ColorF(0, 0, 0, 0.42f)));
        g.dc->FillEllipse(D2D1::Ellipse(c, rad, rad), g.solid(alpha(a.th.text, 0.10f)));
        g.text(a.video.ended() ? L"" : ico::Play, g.fIconBig.Get(),
               rectOf(c.x - rad, c.y - rad, rad * 2, rad * 2),
               D2D1::ColorF(1, 1, 1, 0.92f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void drawImage(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F cv = a.R.canvas;
    if (rw(cv) <= 0 || rh(cv) <= 0) return;

    g.dc->PushAxisAlignedClip(cv, D2D1_ANTIALIAS_MODE_ALIASED);
    g.dc->FillRectangle(cv, g.solid(a.th.canvas));

    if (a.videoMode) {
        drawVideoFrame(a, cv);
        g.dc->PopAxisAlignedClip();
        return;
    }

    auto pic = a.current();
    if (!pic || (!pic->bmp && !pic->failed)) {
        g.dc->PopAxisAlignedClip();
        if (a.folder.count() == 0 && a.index < 0) { drawEmptyState(a, cv); return; }
        D2D1_POINT_2F c{ (cv.left + cv.right) / 2, (cv.top + cv.bottom) / 2 };
        drawSpinner(g, c, g.s(15.f), a.th.textDim, nowSec());
        a.requestAnim();
        return;
    }

    if (pic->failed) {
        g.dc->PopAxisAlignedClip();
        float w = std::min(g.s(420.f), rw(cv) - g.s(48.f));
        D2D1_RECT_F card = rectOf((cv.left + cv.right - w) / 2, (cv.top + cv.bottom) / 2 - g.s(54.f), w, g.s(108.f));
        g.roundRect(card, g.s(10.f), alpha(a.th.danger, 0.16f));
        g.roundRectStroke(card, g.s(10.f), alpha(a.th.danger, 0.55f), g.s(1.f));
        g.text(fileNameOf(pic->path), g.fBodyStrong.Get(),
               rectOf(card.left + g.s(16.f), card.top + g.s(20.f), w - g.s(32.f), g.s(24.f)),
               D2D1::ColorF(1, 1, 1, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
        g.text(pic->error.empty() ? T(L"Не вдалося відкрити") : pic->error, g.fCaption.Get(),
               rectOf(card.left + g.s(16.f), card.top + g.s(50.f), w - g.s(32.f), g.s(40.f)),
               D2D1::ColorF(1, 1, 1, 0.75f), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    // Build the transform: image pixels -> screen pixels.
    float srcW = (float)std::max(1, pic->srcW), srcH = (float)std::max(1, pic->srcH);
    float cx = (cv.left + cv.right) * .5f + a.panX;
    float cy = (cv.top + cv.bottom) * .5f + a.panY;

    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-srcW * .5f, -srcH * .5f);
    m = m * D2D1::Matrix3x2F::Scale(a.zoom, a.zoom);
    if (a.rot) m = m * D2D1::Matrix3x2F::Rotation(a.rot * 90.f);
    // Mirroring is applied last so it always matches the screen axes, whatever
    // rotation is active.
    if (a.flipH) m = m * D2D1::Matrix3x2F::Scale(-1.f, 1.f);
    if (a.flipV) m = m * D2D1::Matrix3x2F::Scale(1.f, -1.f);
    m = m * D2D1::Matrix3x2F::Translation(cx, cy);

    if (pic->hasAlpha) {
        D2D1_RECT_F ir = a.imageRect();
        D2D1_RECT_F clip{ std::max(ir.left, cv.left), std::max(ir.top, cv.top),
                          std::min(ir.right, cv.right), std::min(ir.bottom, cv.bottom) };
        if (clip.right > clip.left && clip.bottom > clip.top) drawCheckerboard(g, clip, a.th);
    }

    // With the compressor open the canvas *is* the preview: the result is drawn
    // over exactly the area the original would cover, so the comparison is like
    // for like even when the output has fewer pixels.
    ID2D1Bitmap1* shown = pic->bmp.Get();
    if (a.compOpen && a.compBmp && !a.compCompare) shown = a.compBmp.Get();
    float shownW = shown->GetSize().width;

    float effective = a.zoom * (shownW / srcW);      // bitmap texel -> screen pixel
    D2D1_INTERPOLATION_MODE mode;
    if (!a.cfg.smoothing && effective >= 1.f)      mode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    else if (effective >= 4.f)                     mode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    else if (effective < 0.98f)                    mode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    else                                           mode = D2D1_INTERPOLATION_MODE_LINEAR;

    g.dc->SetTransform(m);
    float op = clampf(a.fadeIn, 0.f, 1.f);
    g.dc->DrawBitmap(shown, rectOf(0, 0, srcW, srcH), op, mode, nullptr, nullptr);
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g.dc->PopAxisAlignedClip();

    if (pic->preview || pic->upgrading) {
        drawSpinner(g, D2D1::Point2F(cv.right - g.s(30.f), cv.top + g.s(30.f)), g.s(8.f), a.th.textDim, nowSec());
        a.requestAnim();
    }
}

// --------------------------------------------------------------- command bar
struct BarItem {
    int          cmd;
    const wchar_t* glyph;
    const wchar_t* tip;
    wstring      label;      // used when glyph == nullptr
    bool         toggled = false;
    bool         enabled = true;
    bool         danger = false;
    int          xform = 0;  // GX_* applied to the glyph
    int          drop = 0;   // higher = dropped first when space runs out
    float        width = 40.f;
    bool         separator = false;
    int          uid = 0;           // assigned from the item's index
};

static void drawCommandBar(App& a) {
    Gfx& g = a.gfx;
    if (a.view != View::Viewer) return;
    float op = a.barAlpha;
    if (op <= 0.01f) return;

    auto pic = a.current();
    bool has = pic && pic->bmp && !pic->failed;
    bool multi = a.folder.count() > 1;

    std::vector<BarItem> items;
    auto sep = [&](int drop) { BarItem b{}; b.separator = true; b.drop = drop; b.width = 9.f; items.push_back(b); };

    items.push_back({ CMD_GRID, ico::Grid, T(L"Сітка папки  (G)"), L"", false, true, false, false, 6 });
    sep(6);
    items.push_back({ CMD_PREV, ico::ChevL, T(L"Попереднє  (←)"), L"", false, multi, false, false, 0 });
    items.push_back({ CMD_NEXT, ico::ChevR, T(L"Наступне  (→)"), L"", false, multi, false, false, 0 });
    sep(3);
    items.push_back({ CMD_ZOOM_OUT, ico::ZoomOut, T(L"Зменшити  (−)"), L"", false, has, false, false, 2 });

    wchar_t zbuf[24];
    swprintf(zbuf, 24, L"%d%%", (int)lround(a.zoom * 100.f));
    BarItem z{}; z.cmd = CMD_FIT; z.glyph = nullptr; z.label = zbuf; z.tip = T(L"Вписати / 100%  (0 / 1)");
    z.enabled = has; z.drop = 2; z.width = 58.f;
    items.push_back(z);

    items.push_back({ CMD_ZOOM_IN, ico::ZoomIn, T(L"Збільшити  (+)"), L"", false, has, false, false, 2 });
    items.push_back({ CMD_FIT, ico::FitPage, T(L"Вписати у вікно  (0)"), L"", false, has, false, false, 4 });
    items.push_back({ CMD_AUTOSIZE, autoSizeGlyph(a.cfg.autoSize), autoSizeTip(a.cfg.autoSize), L"",
                      a.cfg.autoSize != 0, true, false, false, 3 });
    sep(5);
    items.push_back({ CMD_ROT_L, ico::Rotate, T(L"Обернути ліворуч  (Shift+R)"), L"", false, has, false, GX_MIRROR, 5 });
    items.push_back({ CMD_ROT_R, ico::Rotate, T(L"Обернути праворуч  (R)"), L"", false, has, false, GX_NONE, 5 });

    BarItem fh{}; fh.cmd = CMD_FLIP_H; fh.glyph = ico::Flip;
    fh.tip = T(L"Дзеркально по горизонталі  (H)"); fh.toggled = a.flipH; fh.enabled = has;
    fh.drop = 6; fh.width = 38.f;
    items.push_back(fh);

    BarItem fv{}; fv.cmd = CMD_FLIP_V; fv.glyph = ico::Flip; fv.xform = GX_ROT90;
    fv.tip = T(L"Дзеркально по вертикалі  (V)"); fv.toggled = a.flipV; fv.enabled = has;
    fv.drop = 6; fv.width = 38.f;
    items.push_back(fv);
    sep(7);
    items.push_back({ CMD_SLIDESHOW, a.slideshow ? ico::Pause : ico::Play,
                      a.slideshow ? T(L"Зупинити показ  (Space)") : T(L"Слайдшоу  (Space)"), L"", a.slideshow, multi, false, false, 7 });
    items.push_back({ CMD_FULLSCREEN, a.fullscreen ? ico::BackWin : ico::FullScr,
                      a.fullscreen ? T(L"Вийти з повного екрана  (F11)") : T(L"На весь екран  (F11)"), L"", a.fullscreen, true, false, false, 1 });
    items.push_back({ CMD_FILMSTRIP, ico::Film, T(L"Стрічка кадрів  (T)"), L"", a.cfg.filmstrip, multi, false, false, 8 });
    sep(9);
    items.push_back({ CMD_INFO, ico::Info, T(L"Відомості  (I)"), L"", a.cfg.infoPanel, has, false, false, 9 });
    items.push_back({ CMD_COPY, ico::Copy, T(L"Копіювати  (Ctrl+C)"), L"", false, has, false, false, 10 });
    items.push_back({ CMD_DELETE, ico::Delete, T(L"У кошик  (Del)"), L"", false, has, true, false, 10 });
    sep(11);
    items.push_back({ CMD_MORE, ico::More, T(L"Більше"), L"", a.moreMenuOpen, true, false, false, 0 });

    for (size_t i = 0; i < items.size(); ++i) items[i].uid = UI_BAR_BASE + (int)i;

    // Drop optional groups until the bar fits the canvas.
    float pad = g.s(8.f);
    float maxW = rw(a.R.canvas) - g.s(32.f);
    int dropLevel = 11;
    std::vector<BarItem> shown;
    float total = 0;
    for (;;) {
        shown.clear(); total = pad * 2;
        for (auto& it : items) if (it.drop < dropLevel) { shown.push_back(it); total += g.s(it.width) + (it.separator ? 0 : g.s(5.f)); }
        if (total <= maxW || dropLevel <= 1) break;
        --dropLevel;
    }
    if (shown.empty()) return;

    float h = g.s(48.f);
    float x0 = (a.R.canvas.left + a.R.canvas.right - total) * .5f;
    float y0 = a.R.canvas.bottom - rh(a.R.filmstrip) - h - g.s(16.f);
    D2D1_RECT_F bar = rectOf(x0, y0, total, h);
    a.R.commandBar = bar;

    if (inRect(inflate(bar, g.s(28.f)), a.in.mouse) && a.in.hasMouse) g_overUi = true;

    shadowPill(g, bar, g.s(10.f), a.th.shadow, op);
    g.roundRect(bar, g.s(10.f), alpha(a.th.bar, op));
    g.roundRectStroke(bar, g.s(10.f), alpha(a.th.barStroke, op), g.s(1.f));

    float x = bar.left + pad;
    float cy = (bar.top + bar.bottom) * .5f;
    for (auto& it : shown) {
        float w = g.s(it.width);
        if (it.separator) {
            float sx = x + w * .5f;
            g.dc->FillRectangle(rectOf(sx - g.s(0.5f), cy - g.s(12.f), g.s(1.f), g.s(24.f)),
                                g.solid(alpha(a.th.barStroke, op * 1.6f)));
            x += w;
            continue;
        }
        D2D1_RECT_F r = rectOf(x, cy - g.s(17.f), w, g.s(34.f));
        if (it.cmd == CMD_MORE) a.moreMenuAnchor = r;
        BtnStyle st{ it.toggled, it.enabled, it.danger, false, 6.f };
        if (it.glyph) button(a, it.uid, it.cmd, r, it.glyph, it.tip, st, op, it.xform);
        else          textButton(a, it.uid, it.cmd, r, it.label, it.tip, st, op);
        x += w + g.s(5.f);
    }
}

// The speaker icon follows the level, like the Windows volume flyout.
static const wchar_t* volumeGlyph(const App& a) {
    if (a.video.muted()) return ico::Mute;
    float v = a.video.volume();
    if (v <= 0.005f) return ico::Vol0;
    if (v < 0.34f)   return ico::Vol1;
    if (v < 0.67f)   return ico::Vol2;
    return ico::Vol3;
}

// --------------------------------------------------------------- video bar
static void drawVideoBar(App& a) {
    Gfx& g = a.gfx;
    float op = a.barAlpha;
    if (op <= 0.01f) return;

    bool multi = a.folder.count() > 1;
    bool live = a.video.isOpen() && a.video.metaKnown();
    double dur = a.video.duration();
    double pos = a.seekDragging ? a.seekPreview : a.video.position();

    float pillW = std::min(rw(a.R.canvas) - g.s(24.f), g.s(920.f));
    if (pillW < g.s(240.f)) return;
    float pillH = g.s(80.f);
    D2D1_RECT_F bar = rectOf((a.R.canvas.left + a.R.canvas.right - pillW) * .5f,
                             a.R.canvas.bottom - rh(a.R.filmstrip) - pillH - g.s(16.f), pillW, pillH);
    a.R.commandBar = bar;
    if (inRect(inflate(bar, g.s(24.f)), a.in.mouse) && a.in.hasMouse) g_overUi = true;

    shadowPill(g, bar, g.s(12.f), a.th.shadow, op);
    g.roundRect(bar, g.s(12.f), alpha(a.th.bar, op));
    g.roundRectStroke(bar, g.s(12.f), alpha(a.th.barStroke, op), g.s(1.f));

    // ---- seek track
    float pad = g.s(14.f);
    D2D1_RECT_F seek = rectOf(bar.left + pad, bar.top + g.s(14.f), pillW - pad * 2, g.s(10.f));
    float frac = (dur > 0) ? (float)(pos / dur) : 0.f;
    float outFrac = frac;
    bool dragging = slider(a, UI_V_SEEK, seek, frac, outFrac, op, 4.f);
    if (live && dur > 0) {
        if (dragging) { a.seekDragging = true; a.seekPreview = outFrac * dur; }
        else if (a.seekDragging) { a.seekDragging = false; a.videoSeekTo(outFrac * dur); }
    }

    // ---- thumbnail of the frame under the cursor
    D2D1_RECT_F seekHot = { seek.left - g.s(4.f), seek.top - g.s(10.f),
                            seek.right + g.s(4.f), seek.bottom + g.s(10.f) };
    bool overSeek = a.in.hasMouse && (inRect(seekHot, a.in.mouse) || dragging);
    if (live && dur > 0 && overSeek && op > 0.4f) {
        wstring path = a.currentPath();
        if (a.preview.path() != path) {
            a.preview.open(path, 320);
            a.previewWant = a.previewAt = -1;
            a.previewPending = false;
            a.previewBmp.Reset();
            a.previewX = -1.f;
            a.previewFade = 0.f;
        }
        double tHover = clampf((a.in.mouse.x - seek.left) / std::max(1.f, rw(seek)), 0.f, 1.f) * dur;
        // Ask again once the cursor has moved a few pixels along the track. The
        // old test was a slice of the running time, so on a long clip the card
        // simply refused to update until you swept a long way across it.
        double perPx = dur / std::max(1.0, (double)rw(seek));
        if (a.previewWant < 0 || fabs(tHover - a.previewWant) > std::max(0.05, perPx * 3.0)) {
            a.previewWant = tHover;
            a.previewPending = true;
            a.preview.request(tHover);
        }

        PixelBuf got;
        double gotAt = 0;
        if (a.preview.poll(got, gotAt) && got.valid()) {
            a.previewBmp = g.upload(got);
            a.previewW = got.w;
            a.previewH = got.h;
            a.previewAt = gotAt;
            a.previewPending = false;
        }
        // The decoder lives on its own thread and cannot repaint us, so while
        // the pointer is on the track we simply keep drawing: a frame that
        // lands is shown at once instead of waiting for the next mouse move.
        a.requestAnim();

        float tw = g.s(184.f);
        float ar = 9.f / 16.f;
        if (a.previewBmp && a.previewW > 0) ar = (float)a.previewH / (float)a.previewW;
        else if (a.video.width() > 0)       ar = (float)a.video.height() / (float)a.video.width();
        float th = tw * clampf(ar, 0.25f, 2.2f);

        float padc = g.s(5.f), capH = g.s(20.f);
        float cw2 = tw + padc * 2, ch2 = th + capH + padc * 2;
        float wantX = clampf(a.in.mouse.x - cw2 * .5f,
                             a.R.canvas.left + g.s(8.f),
                             std::max(a.R.canvas.left + g.s(8.f), a.R.canvas.right - cw2 - g.s(8.f)));
        if (a.previewX < 0.f) a.previewX = wantX;
        easeTo(a, a.previewX, wantX, 1e-12f, 0.3f);      // glides with the cursor
        easeTo(a, a.previewFade, 1.f, 1e-9f, 0.003f);

        float cxp = a.previewX;
        float cyp = bar.top - ch2 - g.s(10.f);   // clear of the bar, not just the track
        float cop = op * a.previewFade;
        D2D1_RECT_F card = rectOf(cxp, cyp, cw2, ch2);

        shadowPill(g, card, g.s(8.f), a.th.shadow, cop);
        g.roundRect(card, g.s(8.f), alpha(a.th.bar, cop));
        g.roundRectStroke(card, g.s(8.f), alpha(a.th.barStroke, cop), g.s(1.f));
        D2D1_RECT_F ir = rectOf(cxp + padc, cyp + padc, tw, th);
        float irr = g.s(5.f);
        if (a.previewBmp) {
            g.pushRoundClip(ir, irr);
            g.dc->DrawBitmap(a.previewBmp.Get(), ir, cop, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            g.popRoundClip();
            // Seeking a big file can stall on the disk for a moment. Say so with
            // a quiet ring instead of letting the card look frozen.
            bool stale = a.previewAt < 0 || fabs(a.previewAt - tHover) > perPx * 6.0;
            if (a.previewPending && stale) {
                D2D1_POINT_2F sc{ ir.right - g.s(15.f), ir.bottom - g.s(15.f) };
                g.dc->FillEllipse(D2D1::Ellipse(sc, g.s(11.f), g.s(11.f)),
                                  g.solid(D2D1::ColorF(0, 0, 0, 0.45f * cop)));
                drawSpinner(g, sc, g.s(7.f), D2D1::ColorF(1, 1, 1, cop), nowSec());
            }
        } else {
            g.roundRect(ir, irr, alpha(a.th.card, cop));
            drawSpinner(g, D2D1::Point2F((ir.left + ir.right) * .5f, (ir.top + ir.bottom) * .5f),
                        g.s(11.f), alpha(a.th.textDim, cop), nowSec());
        }
        g.roundRectStroke(ir, irr, alpha(a.th.barStroke, cop), g.s(1.f));
        g.text(formatTime(tHover), g.fCaption.Get(),
               rectOf(cxp, cyp + padc + th, cw2, capH), alpha(a.th.text, cop),
               DWRITE_TEXT_ALIGNMENT_CENTER);

        // a tick showing exactly where that frame sits on the track
        float markX = clampf(a.in.mouse.x, seek.left, seek.right);
        g.roundRect(rectOf(markX - g.s(1.f), seek.top + g.s(1.f), g.s(2.f), rh(seek) - g.s(2.f)),
                    g.s(1.f), alpha(a.th.text, cop * 0.75f));
    } else if (!overSeek) {
        a.previewWant = -1;
        a.previewPending = false;
        a.previewFade = 0.f;
        a.previewX = -1.f;
    }

    // ---- controls row
    float rowY = bar.top + g.s(36.f);
    float bs = g.s(34.f);
    float cyRow = rowY + bs * .5f;

    wstring times = formatTime(pos) + L"  /  " + formatTime(dur);
    float timeW = g.measure(times, g.fCaption.Get()).width + g.s(8.f);
    g.text(times, g.fCaption.Get(), rectOf(bar.left + pad, rowY, timeW, bs),
           alpha(a.th.textDim, op));

    // centre transport
    const float tgap = g.s(9.f);
    const float playD = g.s(44.f);
    float grp = bs * 4 + playD + tgap * 4;
    float x = (bar.left + bar.right - grp) * .5f;
    button(a, UI_V_PREV, CMD_PREV, rectOf(x, cyRow - bs / 2, bs, bs), ico::PrevTrack,
           T(L"Попередній файл  (PgUp)"), { false, multi }, op);
    x += bs + tgap;
    button(a, UI_V_BACK, CMD_SEEK_BACK, rectOf(x, cyRow - bs / 2, bs, bs), ico::Back,
           T(L"−5 секунд  (←)"), { false, live }, op);
    x += bs + tgap;
    button(a, UI_V_PLAY, CMD_PLAYPAUSE, rectOf(x, cyRow - playD / 2, playD, playD),
           a.video.playing() ? ico::Pause : ico::Play,
           a.video.playing() ? T(L"Пауза  (Space)") : T(L"Відтворити  (Space)"),
           { false, live, false, true, 22.f }, op);
    x += playD + tgap;
    button(a, UI_V_FWD, CMD_SEEK_FWD, rectOf(x, cyRow - bs / 2, bs, bs), ico::Fwd,
           T(L"+5 секунд  (→)"), { false, live }, op);
    x += bs + tgap;
    button(a, UI_V_NEXT, CMD_NEXT, rectOf(x, cyRow - bs / 2, bs, bs), ico::NextTrack,
           T(L"Наступний файл  (PgDn)"), { false, multi }, op);

    // Right-hand cluster stays lean: everything else lives behind the "more"
    // flyout so the player bar does not turn into a wall of icons. The speaker
    // is the one control that is never dropped - when the inline slider no
    // longer fits it grows a vertical one on hover instead.
    const float gap = g.s(6.f);
    const float wSpeed = g.s(40.f);
    const float wPct = g.s(38.f), wVolTrack = g.s(58.f);
    const float wVolInline = wPct + g.s(4.f) + wVolTrack;

    float roomRight = bar.right - pad - (x + bs + g.s(20.f));

    bool onFull = true, onSpeed = true, onVolInline = true;
    auto total = [&] {
        float t = bs + gap + bs;                    // "more" + speaker
        if (onFull)      t += bs + gap;
        if (onSpeed)     t += wSpeed + gap;
        if (onVolInline) t += wVolInline + gap;
        return t;
    };
    if (total() > roomRight) onVolInline = false;
    if (total() > roomRight) onSpeed = false;
    if (total() > roomRight) onFull = false;

    float rx = bar.right - pad;                     // right edge of the next slot
    auto slot = [&](float w) {
        rx -= w;
        D2D1_RECT_F r = rectOf(rx, cyRow - bs / 2, w, bs);
        rx -= gap;
        return r;
    };
    float vol = a.video.muted() ? 0.f : a.video.volume();

    a.moreMenuAnchor = slot(bs);
    button(a, UI_V_MORE, CMD_MORE, a.moreMenuAnchor, ico::More,
           T(L"Більше"), { a.moreMenuOpen }, op);

    if (onFull)
        button(a, UI_V_FULL, CMD_FULLSCREEN, slot(bs),
               a.fullscreen ? ico::BackWin : ico::FullScr, T(L"На весь екран  (F11)"),
               { a.fullscreen }, op);

    if (onSpeed) {
        wchar_t sp[16];
        swprintf(sp, 16, L"%.2gx", a.video.rate());
        textButton(a, UI_V_SPEED, CMD_SPEED, slot(wSpeed),
                   sp, T(L"Швидкість  (S / Ctrl+Shift+колесо)"),
                   { fabs(a.video.rate() - 1.0) > 0.01, live }, op);
    }

    if (onVolInline) {
        g.text(std::to_wstring(clampi((int)lround(vol * 100.f), 0, 100)) + L"%", g.fCaption.Get(),
               slot(wPct), alpha(a.th.textDim, op), DWRITE_TEXT_ALIGNMENT_TRAILING);
        rx += gap - g.s(4.f);                       // percent hugs its slider
        D2D1_RECT_F sr = slot(wVolTrack);
        float outVol = vol;
        if (slider(a, UI_V_VOL, rectOf(sr.left, cyRow - g.s(7.f), rw(sr), g.s(14.f)),
                   vol, outVol, op, 4.f)) {
            a.video.setVolume(outVol);
            a.video.setMuted(false);
            a.cfg.volume = clampi((int)lround(outVol * 100.f), 0, 100);
            a.cfg.muted = false;
        }
    }

    D2D1_RECT_F volBtn = slot(bs);
    button(a, UI_V_MUTE, CMD_MUTE, volBtn, volumeGlyph(a),
           a.volPopup ? nullptr
                      : (a.video.muted() ? T(L"Увімкнути звук  (M)") : T(L"Вимкнути звук  (M)")),
           { a.video.muted() }, op);

    // ---- vertical volume flyout (narrow bar)
    if (!onVolInline) {
        bool overBtn = a.in.hasMouse && inRect(inflate(volBtn, g.s(4.f)), a.in.mouse);
        bool overPop = a.volPopup && a.in.hasMouse &&
                       inRect(inflate(a.volPopupRect, g.s(10.f)), a.in.mouse);
        bool holding = (a.active == UI_V_VOLV);
        if (overBtn || overPop || holding) { a.volPopup = true; a.volPopupUntil = nowSec() + 0.35; }
        else if (a.volPopup) {
            if (nowSec() > a.volPopupUntil) a.volPopup = false;
            else a.requestAnim();                   // wait out the grace period
        }
        if (op <= 0.05f) a.volPopup = false;
    } else if (a.volPopup && nowSec() > a.volPopupUntil) {
        a.volPopup = false;
    }

    easeTo(a, a.volAnim, (a.volPopup && !onVolInline) ? 1.f : 0.f, 4e-10f, 0.003f);
    if (a.volAnim > 0.003f) {
        float pw = g.s(46.f), ph = g.s(152.f);
        D2D1_RECT_F pop = rectOf((volBtn.left + volBtn.right) * .5f - pw * .5f,
                                 volBtn.top - ph - g.s(8.f), pw, ph);
        a.volPopupRect = pop;
        if (a.volPopup) g_overUi = true;

        // Grows out of the speaker button rather than appearing whole.
        float grow = 0.88f + 0.12f * a.volAnim;
        float vop = op * clampf(a.volAnim * 1.3f, 0.f, 1.f);
        bool anim = a.volAnim < 0.999f;
        if (anim)
            g.dc->SetTransform(D2D1::Matrix3x2F::Scale(
                grow, grow, D2D1::Point2F((pop.left + pop.right) * .5f, pop.bottom)));

        shadowPill(g, pop, g.s(10.f), a.th.shadow, vop);
        g.roundRect(pop, g.s(10.f), alpha(a.th.bar, vop));
        g.roundRectStroke(pop, g.s(10.f), alpha(a.th.barStroke, vop), g.s(1.f));

        float capH = g.s(22.f);
        g.text(std::to_wstring(clampi((int)lround(vol * 100.f), 0, 100)) + L"%", g.fCaption.Get(),
               rectOf(pop.left, pop.bottom - capH - g.s(5.f), pw, capH), alpha(a.th.textDim, vop),
               DWRITE_TEXT_ALIGNMENT_CENTER);

        D2D1_RECT_F track = rectOf((pop.left + pop.right) * .5f - g.s(7.f), pop.top + g.s(13.f),
                                   g.s(14.f), ph - capH - g.s(26.f));
        float outVol = vol;
        // On the way out it is only a picture; clicks there belong to the canvas.
        if (vslider(a, UI_V_VOLV, track, vol, outVol, vop, 4.f, a.volPopup)) {
            a.video.setVolume(outVol);
            a.video.setMuted(false);
            a.cfg.volume = clampi((int)lround(outVol * 100.f), 0, 100);
            a.cfg.muted = false;
        }
        if (anim) g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

// --------------------------------------------------------------- filmstrip
static void drawFilmstrip(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F r = a.R.filmstrip;
    if (rh(r) <= 1 || a.folder.count() == 0) return;
    float op = a.barAlpha;
    if (op <= 0.01f) return;
    bool interactive = op > 0.25f;

    g.roundRect(r, 0, alpha(a.th.bar, op * 0.94f));
    g.dc->FillRectangle(rectOf(r.left, r.top, rw(r), g.s(1.f)), g.solid(alpha(a.th.barStroke, op)));

    float th = rh(r) - g.s(24.f);
    float tw = th * 1.34f;
    float gap = g.s(6.f);
    float step = tw + gap;
    float total = step * a.folder.count() + gap;

    // Keep the current image visible.
    if (a.index >= 0) {
        float want = a.index * step + gap - (rw(r) - tw) * .5f;
        a.filmScrollTarget = clampf(want, 0.f, std::max(0.f, total - rw(r)));
    }
    easeTo(a, a.filmScroll, a.filmScrollTarget, 3e-8f, 0.5f);

    g.dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = r.top + g.s(12.f);
    int first = std::max(0, (int)((a.filmScroll - gap) / step) - 1);
    int last = std::min((int)a.folder.count() - 1, (int)((a.filmScroll + rw(r)) / step) + 1);
    a.filmHover = -1;

    for (int i = first; i <= last; ++i) {
        float x = r.left + gap + i * step - a.filmScroll;
        D2D1_RECT_F cell = rectOf(x, y, tw, th);
        if (cell.right < r.left || cell.left > r.right) continue;

        bool hovered = interactive && inRect(cell, a.in.mouse) && inRect(r, a.in.mouse) && a.in.hasMouse;
        if (hovered) { a.filmHover = i; g_overUi = true; useCursor(a, IDC_HAND); }
        bool cur = (i == a.index);

        g.roundRect(cell, g.s(4.f), cur ? alpha(a.th.accent, 0.22f) : (hovered ? a.th.cardHover : a.th.card));

        wstring p = a.folder.pathAt(i);
        auto tb = a.thumbFor(p, (int)(th * 1.35f), true);
        if (tb && tb->bmp) {
            float bw = (float)tb->w, bh = (float)tb->h;
            float k = std::min((tw - g.s(6.f)) / bw, (th - g.s(6.f)) / bh);
            float dw = bw * k, dh = bh * k;
            D2D1_RECT_F dst = rectOf(cell.left + (tw - dw) / 2, cell.top + (th - dh) / 2, dw, dh);
            float fade = clampf((float)((nowSec() - tb->arrivedAt) * 6.0), 0.f, 1.f);
            if (fade < 1.f) a.requestAnim();
            g.dc->DrawBitmap(tb->bmp.Get(), dst, fade * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else if (tb && tb->failed) {
            g.text(ico::Photo, g.fIcon.Get(), cell, alpha(a.th.textMute, op), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (isVideoPath(p)) {
            float bw = g.s(20.f), bh = g.s(14.f);
            D2D1_RECT_F badge = rectOf(cell.left + g.s(5.f), cell.bottom - bh - g.s(5.f), bw, bh);
            g.roundRect(badge, g.s(3.f), D2D1::ColorF(0, 0, 0, 0.60f * op));
            g.text(ico::Play, g.fIconSmall.Get(), badge, D2D1::ColorF(1, 1, 1, 0.92f * op),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (cur) g.roundRectStroke(cell, g.s(4.f), alpha(a.th.accent, op), g.s(2.f));
        else if (hovered) g.roundRectStroke(cell, g.s(4.f), alpha(a.th.strokeStrong, op), g.s(1.f));
    }
    g.dc->PopAxisAlignedClip();

    if (interactive && a.filmHover >= 0 && a.in.released && a.active == 0 && a.filmHover != a.index) {
        a.goTo(a.filmHover, true);
    }
}

// --------------------------------------------------------------- grid
static void drawGridHeader(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F r = a.R.gridHeader;
    if (rh(r) <= 1) return;

    float pad = g.s(16.f);
    float cy = (r.top + r.bottom) * .5f;
    wstring dir = a.folder.dir().empty() ? T(L"Папку не вибрано") : a.folder.dir();
    g.text(ico::Folder, g.fIcon.Get(), rectOf(r.left + pad, r.top, g.s(20.f), rh(r)), a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);

    float tx = r.left + pad + g.s(26.f);
    float availW = rw(r) - g.s(300.f);
    g.text(fitText(g, dir, g.fBody.Get(), availW), g.fBody.Get(), rectOf(tx, r.top, availW, rh(r)), a.th.textDim);

    float bs = g.s(32.f);
    float x = r.right - pad - bs;
    button(a, UI_GH_SORTDIR, CMD_SORT_DIR, rectOf(x, cy - bs / 2, bs, bs), a.cfg.sortDesc ? ico::Down : ico::Up,
           a.cfg.sortDesc ? T(L"За спаданням") : T(L"За зростанням"));
    x -= bs + g.s(6.f);

    const wchar_t* sortNames[] = { T(L"Ім'я"), T(L"Дата"), T(L"Розмір"), T(L"Тип") };
    float sw = g.s(104.f);
    x -= sw - bs;
    D2D1_RECT_F sortBtn = rectOf(x, cy - bs / 2, sw, bs);
    if (textButton(a, UI_GH_SORT, CMD_NONE, sortBtn, sortNames[clampi(a.cfg.sortBy, 0, 3)], T(L"Сортування"),
                   { false, true, false, false, 5.f })) {
        a.sortMenuOpen = !a.sortMenuOpen;
        a.sortMenuRect = sortBtn;
    }
    g.text(ico::Sort, g.fIcon.Get(), rectOf(sortBtn.left + g.s(6.f), sortBtn.top, g.s(20.f), rh(sortBtn)),
           a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Thumbnail size stepper.
    x -= g.s(6.f) + bs;
    button(a, UI_GH_SMALLER, CMD_ZOOM_OUT, rectOf(x, cy - bs / 2, bs, bs), ico::ZoomOut, T(L"Менші мініатюри"));
    x -= bs + g.s(2.f);
    button(a, UI_GH_BIGGER, CMD_ZOOM_IN, rectOf(x, cy - bs / 2, bs, bs), ico::ZoomIn, T(L"Більші мініатюри"));

    g.dc->FillRectangle(rectOf(r.left, r.bottom - g.s(1.f), rw(r), g.s(1.f)), g.solid(a.th.stroke));
}

static void drawSortMenu(App& a) {
    if (!a.sortMenuOpen) return;
    Gfx& g = a.gfx;
    const wchar_t* names[] = { T(L"За іменем"), T(L"За датою"), T(L"За розміром"), T(L"За типом") };
    float w = g.s(180.f), ih = g.s(34.f), pad = g.s(4.f);
    float h = ih * 4 + pad * 2;
    D2D1_RECT_F m = rectOf(std::min(a.sortMenuRect.left, (float)g.width - w - g.s(12.f)),
                           a.sortMenuRect.bottom + g.s(4.f), w, h);
    shadowPill(g, m, g.s(8.f), a.th.shadow, 1.f);
    g.roundRect(m, g.s(8.f), alpha(a.th.bar, 0.98f));
    g.roundRectStroke(m, g.s(8.f), a.th.barStroke, g.s(1.f));
    if (inRect(inflate(m, g.s(8.f)), a.in.mouse)) g_overUi = true;

    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F ir = rectOf(m.left + pad, m.top + pad + i * ih, w - pad * 2, ih);
        bool hovered = inRect(ir, a.in.mouse) && a.in.hasMouse;
        if (hovered) { useCursor(a, IDC_HAND); g.roundRect(ir, g.s(5.f), a.th.cardHover); }
        if (a.cfg.sortBy == i)
            g.dc->FillRoundedRectangle(D2D1::RoundedRect(rectOf(ir.left + g.s(3.f), (ir.top + ir.bottom) / 2 - g.s(8.f), g.s(3.f), g.s(16.f)), g.s(1.5f), g.s(1.5f)), g.solid(a.th.accent));
        g.text(names[i], g.fBody.Get(), rectOf(ir.left + g.s(14.f), ir.top, rw(ir) - g.s(18.f), ih), a.th.text);
        if (hovered && a.in.released) { a.pendingCmd = CMD_SORT_NAME + i; a.sortMenuOpen = false; }
    }
    if (a.in.pressed && !inRect(m, a.in.mouse) && !inRect(a.sortMenuRect, a.in.mouse)) a.sortMenuOpen = false;
}

static void drawGrid(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F r = a.R.grid;
    if (rh(r) <= 1) return;

    if (a.folder.count() == 0) { drawEmptyState(a, r); return; }

    float pad = g.s(16.f);
    float cell = g.s((float)a.cfg.thumbSize);
    float capH = g.s(24.f);
    float gap = g.s(10.f);
    float availW = rw(r) - pad * 2;
    int cols = std::max(1, (int)((availW + gap) / (cell + gap)));
    float cw = (availW - gap * (cols - 1)) / cols;
    float chh = cw + capH;
    int rows = ((int)a.folder.count() + cols - 1) / cols;
    float contentH = rows * (chh + gap) + pad * 2 - gap;

    a.gridCols = cols;
    a.gridRowH = chh + gap;

    float maxScroll = std::max(0.f, contentH - rh(r));
    a.gridScrollTarget = clampf(a.gridScrollTarget, 0.f, maxScroll);
    if (fabsf(a.gridScroll - a.gridScrollTarget) > 0.4f) {
        a.gridScroll += (a.gridScrollTarget - a.gridScroll) * easeK(a, 1e-8f);
        a.requestAnim();
    } else a.gridScroll = a.gridScrollTarget;

    g.dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);

    int firstRow = std::max(0, (int)((a.gridScroll - pad) / (chh + gap)) - 1);
    int lastRow = std::min(rows - 1, (int)((a.gridScroll + rh(r) - pad) / (chh + gap)) + 1);
    a.gridHover = -1;
    int clickedIndex = -1;

    for (int row = firstRow; row <= lastRow; ++row) {
        for (int col = 0; col < cols; ++col) {
            int i = row * cols + col;
            if (i >= (int)a.folder.count()) break;
            float x = r.left + pad + col * (cw + gap);
            float y = r.top + pad + row * (chh + gap) - a.gridScroll;
            D2D1_RECT_F cellR = rectOf(x, y, cw, chh);
            D2D1_RECT_F imgR = rectOf(x, y, cw, cw);

            bool hovered = inRect(cellR, a.in.mouse) && inRect(r, a.in.mouse) && a.in.hasMouse;
            if (hovered) { a.gridHover = i; g_overUi = true; useCursor(a, IDC_HAND); }
            bool cur = (i == a.index);

            g.roundRect(imgR, g.s(6.f), hovered ? a.th.cardHover : a.th.card);

            wstring p = a.folder.pathAt(i);
            bool visible = true;
            auto tb = a.thumbFor(p, clampi((int)(cw * 1.1f), 96, 512), visible);
            if (tb && tb->bmp) {
                float bw = (float)tb->w, bh = (float)tb->h;
                float inset = g.s(5.f);
                float k = std::min((cw - inset * 2) / bw, (cw - inset * 2) / bh);
                float dw = bw * k, dh = bh * k;
                D2D1_RECT_F dst = rectOf(x + (cw - dw) / 2, y + (cw - dh) / 2, dw, dh);
                float fade = clampf((float)((nowSec() - tb->arrivedAt) * 5.0), 0.f, 1.f);
                if (fade < 1.f) a.requestAnim();
                g.dc->DrawBitmap(tb->bmp.Get(), dst, fade, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else if (tb && tb->failed) {
                g.text(ico::Photo, g.fIconBig.Get(), imgR, a.th.textMute, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                drawSpinner(g, D2D1::Point2F((imgR.left + imgR.right) / 2, (imgR.top + imgR.bottom) / 2),
                            g.s(9.f), a.th.textMute, nowSec());
                a.requestAnim();
            }

            if (isVideoPath(p)) {
                float bw = g.s(26.f), bh = g.s(18.f);
                D2D1_RECT_F badge = rectOf(imgR.left + g.s(8.f), imgR.bottom - bh - g.s(8.f), bw, bh);
                g.roundRect(badge, g.s(4.f), D2D1::ColorF(0, 0, 0, 0.62f));
                g.text(ico::Play, g.fIconSmall.Get(), badge, D2D1::ColorF(1, 1, 1, 0.95f),
                       DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            if (cur) g.roundRectStroke(imgR, g.s(6.f), a.th.accent, g.s(2.f));
            else if (hovered) g.roundRectStroke(imgR, g.s(6.f), a.th.strokeStrong, g.s(1.f));

            wstring name = fitText(g, a.folder.at(i).name, g.fCaption.Get(), cw - g.s(8.f));
            g.text(name, g.fCaption.Get(), rectOf(x, y + cw, cw, capH),
                   cur ? a.th.text : a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);

            // A click that belongs to the open sort menu must not fall through
            // and open a picture.
            if (hovered && a.in.released && a.active == 0 && !a.sortMenuOpen) clickedIndex = i;
        }
    }
    g.dc->PopAxisAlignedClip();

    // Slim scrollbar.
    if (maxScroll > 1.f) {
        float trackH = rh(r) - g.s(8.f);
        float thumbH = std::max(g.s(28.f), trackH * (rh(r) / contentH));
        float t = a.gridScroll / maxScroll;
        float sx = r.right - g.s(9.f);
        bool barNear = a.in.hasMouse && a.in.mouse.x > r.right - g.s(40.f);
        float aw = barNear ? g.s(6.f) : g.s(3.f);
        D2D1_RECT_F thumb = rectOf(sx + (g.s(6.f) - aw) / 2, r.top + g.s(4.f) + t * (trackH - thumbH), aw, thumbH);
        g.roundRect(thumb, aw / 2, alpha(a.th.text, barNear ? 0.42f : 0.22f));
    }

    if (clickedIndex >= 0) {
        a.goTo(clickedIndex, true);
        a.setView(View::Viewer);
    }
}

// --------------------------------------------------------------- info panel
static void infoRow(App& a, float& y, const wstring& label, const wstring& value) {
    if (value.empty()) return;
    Gfx& g = a.gfx;
    D2D1_RECT_F p = a.R.info;
    float pad = g.s(18.f);
    g.text(label, g.fSmall.Get(), rectOf(p.left + pad, y, rw(p) - pad * 2, g.s(16.f)), a.th.textMute);
    y += g.s(17.f);
    wstring v = fitText(g, value, g.fBody.Get(), rw(p) - pad * 2);
    g.text(v, g.fBody.Get(), rectOf(p.left + pad, y, rw(p) - pad * 2, g.s(20.f)), a.th.text);
    y += g.s(26.f);
}

static void drawInfoPanel(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F p = a.R.info;
    if (rw(p) <= 1) return;
    if (inRect(p, a.in.mouse) && a.in.hasMouse) g_overUi = true;

    g.dc->FillRectangle(p, g.solid(a.th.dark ? D2D1::ColorF(0.f, 0.f, 0.f, 0.22f) : D2D1::ColorF(1.f, 1.f, 1.f, 0.42f)));
    g.dc->FillRectangle(rectOf(p.left, p.top, g.s(1.f), rh(p)), g.solid(a.th.stroke));

    float pad = g.s(18.f);
    float y = p.top + g.s(18.f);
    g.text(T(L"Відомості"), g.fBodyStrong.Get(), rectOf(p.left + pad, y, rw(p) - pad * 2 - g.s(34.f), g.s(24.f)), a.th.text);
    button(a, UI_INFO_CLOSE, CMD_INFO, rectOf(p.right - pad - g.s(28.f), y - g.s(2.f), g.s(28.f), g.s(28.f)), ico::Close, T(L"Закрити"));
    y += g.s(36.f);

    if (a.videoMode) {
        wstring path = a.currentPath();
        infoRow(a, y, T(L"Файл"), fileNameOf(path));
        if (a.video.width() > 0)
            infoRow(a, y, T(L"Розмір у пікселях"),
                    std::to_wstring(a.video.width()) + L" × " + std::to_wstring(a.video.height()));
        double dur = a.video.duration();
        if (dur > 0) infoRow(a, y, T(L"Тривалість"), formatTime(dur));

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            infoRow(a, y, T(L"Обсяг"), humanSize(((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow));
            infoRow(a, y, T(L"Змінено"), humanTime(fad.ftLastWriteTime));
        }
        { wstring e = extOf(path); if (!e.empty() && e[0] == L'.') e.erase(0, 1);
          for (auto& c : e) c = (wchar_t)towupper(c);
          infoRow(a, y, T(L"Тип"), e); }

        // Pulled from the shell property store, same source Explorer uses.
        if (a.mediaPropsPath != path) {
            a.mediaPropsPath = path;
            readMediaProps(path, a.mediaProps);
        }
        if (!a.mediaProps.empty()) {
            y += g.s(6.f);
            g.dc->FillRectangle(rectOf(p.left + pad, y, rw(p) - pad * 2, g.s(1.f)), g.solid(a.th.stroke));
            y += g.s(16.f);
            for (const auto& kv : a.mediaProps) infoRow(a, y, kv.first, kv.second);
        }

        y += g.s(10.f);
        float bwv = rw(p) - pad * 2, bhv = g.s(32.f);
        textButton(a, UI_INFO_REVEAL, CMD_REVEAL, rectOf(p.left + pad, y, bwv, bhv),
                   T(L"Показати в провіднику"), nullptr, { false, true, false, false, 5.f });
        return;
    }

    auto pic = a.current();
    if (!pic) { g.text(T(L"Немає даних"), g.fBody.Get(), rectOf(p.left + pad, y, rw(p) - pad * 2, g.s(20.f)), a.th.textDim); return; }

    infoRow(a, y, T(L"Файл"), fileNameOf(pic->path));
    if (pic->srcW > 0)
        infoRow(a, y, T(L"Розмір у пікселях"), std::to_wstring(pic->srcW) + L" × " + std::to_wstring(pic->srcH) +
                L"  (" + fmtMP(pic->srcW, pic->srcH) + T(L" Мп)"));
    infoRow(a, y, T(L"Обсяг"), humanSize(pic->fileSize));
    { wstring e = extOf(pic->path); if (!e.empty() && e[0] == L'.') e.erase(0, 1);
      for (auto& c : e) c = (wchar_t)towupper(c);
      infoRow(a, y, T(L"Тип"), e); }
    infoRow(a, y, T(L"Змінено"), humanTime(pic->mtime));
    if (pic->frameCount > 1) infoRow(a, y, T(L"Кадрів"), std::to_wstring(pic->frameCount));

    if (pic->exif.any) {
        y += g.s(6.f);
        g.dc->FillRectangle(rectOf(p.left + pad, y, rw(p) - pad * 2, g.s(1.f)), g.solid(a.th.stroke));
        y += g.s(16.f);
        infoRow(a, y, T(L"Камера"), pic->exif.camera);
        infoRow(a, y, T(L"Об'єктив"), pic->exif.lens);
        infoRow(a, y, T(L"Знято"), pic->exif.taken);
        wstring shot;
        for (const wstring* s : { &pic->exif.exposure, &pic->exif.aperture, &pic->exif.iso, &pic->exif.focal })
            if (!s->empty()) { if (!shot.empty()) shot += L"   "; shot += *s; }
        infoRow(a, y, T(L"Експозиція"), shot);
        infoRow(a, y, T(L"Софт"), pic->exif.software);
    }

    y += g.s(10.f);
    float bw = rw(p) - pad * 2, bh = g.s(32.f);
    textButton(a, UI_INFO_REVEAL, CMD_REVEAL, rectOf(p.left + pad, y, bw, bh), T(L"Показати в провіднику"), nullptr, { false, true, false, false, 5.f });
    y += bh + g.s(6.f);
    textButton(a, UI_INFO_WALL, CMD_SETWALLPAPER, rectOf(p.left + pad, y, bw, bh), T(L"Зробити шпалерами"), nullptr, { false, true, false, false, 5.f });

    if (pic->decodeMs > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, T(L"декодовано за %.1f мс"), pic->decodeMs);
        g.text(buf, g.fSmall.Get(), rectOf(p.left + pad, p.bottom - g.s(28.f), rw(p) - pad * 2, g.s(18.f)), a.th.textMute);
    }
}

// --------------------------------------------------------------- settings sheet
static bool toggleSwitch(App& a, int uid, D2D1_RECT_F r, bool value) {
    Gfx& g = a.gfx;
    float w = g.s(42.f), h = g.s(22.f);
    D2D1_RECT_F sw = rectOf(r.right - w, (r.top + r.bottom) * .5f - h * .5f, w, h);
    D2D1_RECT_F hit = inflate(sw, g.s(6.f));

    bool hovered = inRect(hit, a.in.mouse) && a.in.hasMouse;
    if (hovered) { g_overUi = true; a.hot = uid; useCursor(a, IDC_HAND); }
    if (hovered && a.in.pressed) a.active = uid;
    bool clicked = false;
    if (a.in.released && a.active == uid) { if (hovered) clicked = true; a.active = 0; }

    D2D1_COLOR_F track = value ? (hovered ? a.th.accentHover : a.th.accent)
                               : (hovered ? a.th.cardHover : a.th.card);
    g.roundRect(sw, h * .5f, track);
    if (!value) g.roundRectStroke(sw, h * .5f, a.th.strokeStrong, g.s(1.f));

    float kr = g.s(value ? 7.f : 6.f);
    float kx = value ? sw.right - h * .5f : sw.left + h * .5f;
    g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, (sw.top + sw.bottom) * .5f), kr, kr),
                      g.solid(value ? a.th.accentText : a.th.text));
    return clicked;
}

// Windows 11 style segmented control; returns the newly picked index or -1.
static int segmented(App& a, int uidBase, D2D1_RECT_F r,
                     const wchar_t* const* opts, int n, int value) {
    Gfx& g = a.gfx;
    float h = g.s(30.f);

    // Equal slices leave short labels swimming and clip long ones, so each
    // segment gets the width its own text needs.
    std::vector<float> widths((size_t)n), offs((size_t)n);
    float inner = g.s(11.f), total = 0.f;
    for (int i = 0; i < n; ++i) {
        widths[i] = std::max(g.s(44.f), g.measure(opts[i], g.fCaption.Get()).width + inner * 2);
        total += widths[i];
    }
    float maxW = rw(r) + g.s(90.f);
    if (total > maxW) {
        float k = maxW / total;
        total = 0.f;
        for (int i = 0; i < n; ++i) { widths[i] *= k; total += widths[i]; }
    }
    for (int i = 0, acc = 0; i < n; ++i) { offs[i] = (float)acc; acc += (int)widths[i]; }
    total = offs[n - 1] + widths[n - 1];

    D2D1_RECT_F box = rectOf(r.right - total, (r.top + r.bottom) * .5f - h * .5f, total, h);
    g.roundRect(box, g.s(6.f), a.th.card);
    g.roundRectStroke(box, g.s(6.f), a.th.stroke, g.s(1.f));

    int picked = -1;
    for (int i = 0, xo = 0; i < n; ++i) {
        float iw = widths[i];
        D2D1_RECT_F ir = rectOf(box.left + offs[i], box.top, iw, h);
        (void)xo;
        bool hovered = inRect(ir, a.in.mouse) && a.in.hasMouse;
        if (hovered) { g_overUi = true; a.hot = uidBase + i; useCursor(a, IDC_HAND); }
        if (hovered && a.in.pressed) a.active = uidBase + i;
        if (a.in.released && a.active == uidBase + i) { if (hovered) picked = i; a.active = 0; }

        if (i == value) g.roundRect(inflate(ir, -g.s(2.f)), g.s(4.f), alpha(a.th.accent, 0.28f));
        else if (hovered) g.roundRect(inflate(ir, -g.s(2.f)), g.s(4.f), a.th.cardHover);
        g.text(opts[i], g.fCaption.Get(), ir, i == value ? a.th.accent : a.th.textDim,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return picked;
}

struct SetRow {
    App& a;
    D2D1_RECT_F card;
    float y;
    float pad;
    float ctrlW;
};

static void setSection(SetRow& s, const wchar_t* title) {
    Gfx& g = s.a.gfx;
    s.y += g.s(10.f);
    g.text(title, g.fBodyStrong.Get(),
           rectOf(s.card.left + s.pad, s.y, rw(s.card) - s.pad * 2, g.s(24.f)), s.a.th.accent);
    s.y += g.s(30.f);
}

static D2D1_RECT_F setLabel(SetRow& s, const wchar_t* label, const wchar_t* hint, float rowH) {
    Gfx& g = s.a.gfx;
    D2D1_RECT_F row = rectOf(s.card.left + s.pad, s.y, rw(s.card) - s.pad * 2, rowH);
    float lw = rw(row) - s.ctrlW - g.s(16.f);
    if (hint && *hint) {
        g.text(label, g.fBody.Get(), rectOf(row.left, row.top + g.s(6.f), lw, g.s(20.f)), s.a.th.text);
        g.text(hint, g.fSmall.Get(), rectOf(row.left, row.top + g.s(25.f), lw, g.s(18.f)), s.a.th.textMute);
    } else {
        g.text(label, g.fBody.Get(), rectOf(row.left, row.top, lw, rowH), s.a.th.text);
    }
    s.y += rowH + g.s(4.f);
    return rectOf(row.right - s.ctrlW, row.top, s.ctrlW, rowH);
}

static const wchar_t* const kPopularExts[] = {
    L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp", L".tif", L".tiff",
    L".heic", L".heif", L".avif", L".ico", L".jfif", L".jxl", L".psd", L".svg",
    L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v", L".wmv", L".mpg", L".ts"
};

static bool isPopularExt(const wstring& e) {
    for (const wchar_t* p : kPopularExts) if (e == p) return true;
    return false;
}

// One chip per extension. Returns the height consumed.
static float assocGrid(App& a, D2D1_RECT_F area, float& y, const std::vector<size_t>& idx,
                       D2D1_RECT_F clipRect) {
    Gfx& g = a.gfx;
    if (idx.empty()) return 0;
    float chipH = g.s(30.f), gapc = g.s(6.f);
    float availW = rw(area);
    int cols = std::max(1, (int)((availW + gapc) / (g.s(84.f) + gapc)));
    float cw = (availW - gapc * (cols - 1)) / cols;

    for (size_t n = 0; n < idx.size(); ++n) {
        size_t i = idx[n];
        int col = (int)n % cols, row = (int)n / cols;
        D2D1_RECT_F chip = rectOf(area.left + col * (cw + gapc), y + row * (chipH + gapc), cw, chipH);
        if (chip.bottom >= clipRect.top - chipH && chip.top <= clipRect.bottom + chipH) {
            bool on = a.assocSel.count(a.assocAll[i]) > 0;
            int uid = UI_ASSOC_BASE + (int)i;
            bool hovered = inRect(chip, a.in.mouse) && inRect(clipRect, a.in.mouse) && a.in.hasMouse;
            if (hovered) { g_overUi = true; a.hot = uid; useCursor(a, IDC_HAND); }
            if (hovered && a.in.pressed) a.active = uid;
            if (a.in.released && a.active == uid) {
                if (hovered) {
                    if (on) a.assocSel.erase(a.assocAll[i]);
                    else a.assocSel.insert(a.assocAll[i]);
                    a.settingsDirty = true;
                }
                a.active = 0;
            }

            g.roundRect(chip, g.s(5.f), on ? alpha(a.th.accent, hovered ? 0.34f : 0.22f)
                                           : (hovered ? a.th.cardHover : a.th.card));
            if (on) g.roundRectStroke(chip, g.s(5.f), alpha(a.th.accent, 0.65f), g.s(1.f));
            if (on)
                g.text(ico::Check, g.fIconSmall.Get(), rectOf(chip.left + g.s(5.f), chip.top, g.s(16.f), chipH),
                       a.th.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            wstring lbl = a.assocAll[i];
            if (!lbl.empty() && lbl[0] == L'.') lbl.erase(0, 1);
            g.text(lbl, g.fCaption.Get(), rectOf(chip.left + g.s(21.f), chip.top, cw - g.s(25.f), chipH),
                   on ? a.th.text : a.th.textDim);
        }
    }
    int rows = ((int)idx.size() + cols - 1) / cols;
    float h = rows * (chipH + gapc);
    y += h;
    return h;
}

// --------------------------------------------------------------- compressor
static wstring compSizeText(double bytes) {
    wchar_t b[32];
    if (bytes >= 1024.0 * 1024.0)      swprintf(b, 32, L"%.2f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024.0)          swprintf(b, 32, L"%.0f KB", bytes / 1024.0);
    else                               swprintf(b, 32, L"%.0f B", bytes);
    return b;
}

// A panel docked to the right edge rather than a sheet over the picture: the
// canvas keeps showing the photo, and what it shows is the compressed result.
static void drawCompressor(App& a) {
    Gfx& g = a.gfx;
    D2D1_RECT_F panel = a.R.comp;
    if (rw(panel) < g.s(8.f)) return;

    const auto& formats = encodeFormats();
    if (formats.empty()) { a.compOpen = false; return; }
    const EncFormat& fmt = formats[clampi(a.compFormat, 0, (int)formats.size() - 1)];

    g.dc->FillRectangle(panel, g.solid(a.th.chrome));
    g.dc->FillRectangle(rectOf(panel.left, panel.top, g.s(1.f), rh(panel)), g.solid(a.th.stroke));
    if (inRect(panel, a.in.mouse) && a.in.hasMouse) g_overUi = true;

    float pad = g.s(16.f);
    D2D1_RECT_F inner = rectOf(panel.left + pad, panel.top, rw(panel) - pad * 2 - g.s(6.f), rh(panel));
    if (rw(inner) < g.s(80.f)) return;          // still sliding in

    // ---------------- header
    float hdr = g.s(48.f);
    g.text(T(L"Стиснути та конвертувати"), g.fBodyStrong.Get(),
           rectOf(inner.left, panel.top, rw(inner) - g.s(34.f), hdr), a.th.text);
    if (button(a, UI_COMP_BASE, CMD_NONE,
               rectOf(inner.right - g.s(30.f), panel.top + g.s(9.f), g.s(30.f), g.s(30.f)),
               ico::Close, T(L"Закрити  (Esc)"))) {
        a.openCompressor(false);
        return;
    }
    g.dc->FillRectangle(rectOf(panel.left + g.s(1.f), panel.top + hdr, rw(panel), g.s(1.f)),
                        g.solid(a.th.stroke));

    D2D1_RECT_F col = rectOf(inner.left, panel.top + hdr + g.s(10.f), rw(inner),
                             rh(panel) - hdr - g.s(10.f));
    if (rh(col) < g.s(40.f)) return;

    g.dc->PushAxisAlignedClip(rectOf(panel.left, col.top, rw(panel), rh(col)),
                              D2D1_ANTIALIAS_MODE_ALIASED);
    float y = col.top - a.compScroll;
    float rowH = g.s(22.f);
    auto label = [&](const wchar_t* t) {
        g.text(t, g.fCaption.Get(), rectOf(col.left, y, rw(col), rowH), a.th.textDim);
        y += rowH;
    };

    // ---------------- format
    label(T(L"Формат"));
    {
        float gap = g.s(6.f);
        int perRow = clampi((int)((rw(col) + gap) / (g.s(72.f) + gap)), 1, 4);
        float cw = (rw(col) - gap * (perRow - 1)) / perRow, ch = g.s(30.f);
        for (size_t i = 0; i < formats.size(); ++i) {
            int cx = (int)i % perRow, cy = (int)i / perRow;
            D2D1_RECT_F r = rectOf(col.left + cx * (cw + gap), y + cy * (ch + gap), cw, ch);
            bool sel = (int)i == a.compFormat;
            if (textButton(a, UI_COMP_BASE + 10 + (int)i, CMD_NONE, r, formats[i].name, nullptr,
                           { sel, true, false, false, 5.f }) && !sel) {
                a.compFormat = (int)i;
                a.compressRequest(true);
            }
        }
        y += ((formats.size() + perRow - 1) / perRow) * (ch + gap) + g.s(8.f);
    }

    // ---------------- mode
    label(T(L"Режим"));
    {
        const wchar_t* modes[] = { T(L"Якість"), T(L"Відсоток"), T(L"Розмір файлу") };
        int cur = (int)a.compMode;
        float gap = g.s(6.f);
        float cw = (rw(col) - gap * 2) / 3.f, ch = g.s(30.f);
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F r = rectOf(col.left + i * (cw + gap), y, cw, ch);
            if (textButton(a, UI_COMP_BASE + 30 + i, CMD_NONE, r, modes[i], nullptr,
                           { i == cur, true, false, false, 5.f }) && i != cur) {
                a.compMode = (CompressMode)i;
                a.compressRequest(true);
            }
        }
        y += ch + g.s(12.f);
    }

    auto sliderRow = [&](int uid, const wchar_t* name, const wstring& value, float v, float& out) {
        g.text(name, g.fCaption.Get(), rectOf(col.left, y, rw(col) - g.s(80.f), rowH), a.th.textDim);
        g.text(value, g.fCaption.Get(), rectOf(col.right - g.s(80.f), y, g.s(80.f), rowH),
               a.th.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += rowH;
        bool dragging = slider(a, uid, rectOf(col.left, y + g.s(2.f), rw(col), g.s(14.f)), v, out, 1.f, 4.f);
        y += g.s(26.f);
        return dragging;
    };

    if (a.compMode == CompressMode::Quality) {
        if (!fmt.quality) {
            g.text(T(L"Формат без втрат — якість не регулюється"), g.fSmall.Get(),
                   rectOf(col.left, y, rw(col), rowH * 2), a.th.textMute);
            y += rowH + g.s(8.f);
        } else {
            float v = a.compQuality, out = v;
            wchar_t b[16]; swprintf(b, 16, L"%d", (int)lround(a.compQuality * 100.f));
            if (sliderRow(UI_COMP_BASE + 40, T(L"Якість"), b, v, out)) {
                a.compQuality = clampf(out, 0.05f, 1.f);
                a.compressRequest();
            }
        }
    } else if (a.compMode == CompressMode::Percent) {
        float v = a.compPercent, out = v;
        wchar_t b[16]; swprintf(b, 16, L"%d%%", (int)lround(a.compPercent * 100.f));
        if (sliderRow(UI_COMP_BASE + 41, T(L"Частка від оригіналу"), b, v, out)) {
            a.compPercent = clampf(out, 0.02f, 1.f);
            a.compressRequest();
        }
    } else {
        // 0.02 .. 25 MB on a curve, so the small end where it matters is usable.
        const double maxMB = 25.0;
        float v = (float)pow(clampf((float)(a.compTargetMB / maxMB), 0.f, 1.f), 1.0 / 2.2), out = v;
        wchar_t b[24]; swprintf(b, 24, L"%.2f MB", a.compTargetMB);
        if (sliderRow(UI_COMP_BASE + 42, T(L"Цільовий розмір"), b, v, out)) {
            a.compTargetMB = std::max(0.02, pow((double)out, 2.2) * maxMB);
            a.compressRequest();
        }
    }

    {
        float v = a.compScale, out = v;
        wchar_t b[24];
        if (a.compHasResult) swprintf(b, 24, L"%d×%d", a.compRes.outW, a.compRes.outH);
        else                 swprintf(b, 24, L"%d%%", (int)lround(a.compScale * 100.f));
        if (sliderRow(UI_COMP_BASE + 43, T(L"Роздільність"), b, v, out)) {
            a.compScale = clampf(out, 0.05f, 1.f);
            a.compressRequest();
        }
    }

    if (a.compMode != CompressMode::Quality) {
        D2D1_RECT_F tr = rectOf(col.left, y, rw(col), g.s(30.f));
        g.text(T(L"Зменшувати за потреби"), g.fCaption.Get(),
               rectOf(tr.left, tr.top, rw(tr) - g.s(52.f), rh(tr)), a.th.textDim);
        if (toggleSwitch(a, UI_COMP_BASE + 50, tr, a.compDownscale)) {
            a.compDownscale = !a.compDownscale;
            a.compressRequest(true);
        }
        y += g.s(36.f);
    }

    // ---------------- numbers
    y += g.s(2.f);
    g.dc->FillRectangle(rectOf(col.left, y, rw(col), g.s(1.f)), g.solid(a.th.stroke));
    y += g.s(10.f);

    float statOp = a.compPending ? 0.45f : 1.f;
    auto statRow = [&](const wchar_t* k, const wstring& v, const D2D1_COLOR_F& c) {
        g.text(k, g.fCaption.Get(), rectOf(col.left, y, rw(col) * .45f, rowH),
               alpha(a.th.textMute, statOp));
        g.text(v, g.fBodyStrong.Get(), rectOf(col.left + rw(col) * .3f, y, rw(col) * .7f, rowH),
               alpha(c, statOp), DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += rowH + g.s(2.f);
    };

    if (a.compHasResult) {
        const CompressResult& r = a.compRes;
        wchar_t buf[64];
        swprintf(buf, 64, L"%s · %d×%d", compSizeText((double)r.srcBytes).c_str(), r.srcW, r.srcH);
        statRow(T(L"Було"), buf, a.th.textDim);
        swprintf(buf, 64, L"%s · %d×%d", compSizeText((double)r.outBytes).c_str(), r.outW, r.outH);
        statRow(T(L"Стане"), buf, a.th.text);

        if (r.srcBytes > 0) {
            double d = 100.0 * (1.0 - (double)r.outBytes / (double)r.srcBytes);
            swprintf(buf, 64, d >= 0 ? L"−%.0f%%" : L"+%.0f%%", fabs(d));
            statRow(T(L"Різниця"), buf, d >= 0 ? a.th.accent : a.th.danger);
        }
        if (fmt.quality && a.compMode != CompressMode::Quality) {
            swprintf(buf, 64, L"%d", (int)lround(r.usedQuality * 100.f));
            statRow(T(L"Підібрана якість"), buf, a.th.textDim);
        }
        if (r.missedTarget) {
            g.text(T(L"Менше зробити не вдалося"), g.fSmall.Get(),
                   rectOf(col.left, y, rw(col), rowH * 2), a.th.danger);
            y += rowH + g.s(4.f);
        }
    } else if (!a.compStatus.empty()) {
        g.text(a.compStatus, g.fSmall.Get(), rectOf(col.left, y, rw(col), rowH * 2), a.th.danger);
        y += rowH;
    }

    // Hold this to put the untouched picture back on the canvas for a moment.
    {
        float bh2 = g.s(30.f);
        D2D1_RECT_F cmp = rectOf(col.left, y, rw(col), bh2);
        auto pic = a.current();
        bool can = pic && pic->bmp && a.compBmp;
        bool over = can && inRect(cmp, a.in.mouse) && a.in.hasMouse;
        if (over) { g_overUi = true; useCursor(a, IDC_HAND); a.hot = UI_COMP_BASE + 2; }
        bool holding = over && a.in.down;
        if (holding != a.compCompare) { a.compCompare = holding; a.invalidate(); }
        g.roundRect(cmp, g.s(5.f), holding ? alpha(a.th.accent, 0.25f)
                                           : (over ? a.th.cardHover : a.th.card));
        g.text(holding ? T(L"Оригінал") : T(L"Затисніть для оригіналу"), g.fCaption.Get(),
               cmp, holding ? a.th.accent : a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += bh2 + g.s(10.f);
    }

    // ---------------- actions
    {
        float bh2 = g.s(34.f);
        bool ready = a.compHasResult && !a.compPending;
        float half = (rw(col) - g.s(8.f)) * .5f;
        textButton(a, UI_COMP_BASE + 60, CMD_COMP_SAVE, rectOf(col.left, y, half, bh2),
                   T(L"Зберегти"), nullptr, { false, ready, false, true, 5.f });
        textButton(a, UI_COMP_BASE + 61, CMD_COMP_SAVEAS, rectOf(col.left + half + g.s(8.f), y, half, bh2),
                   T(L"Зберегти як…"), nullptr, { false, ready, false, false, 5.f });
        y += bh2 + g.s(8.f);

        bool batching = a.compBatchTotal > 1 && a.compBatchDone < a.compBatchTotal;
        if (batching) {
            D2D1_RECT_F pr2 = rectOf(col.left, y + g.s(14.f), rw(col) - g.s(92.f), g.s(6.f));
            g.roundRect(pr2, g.s(3.f), alpha(a.th.text, 0.2f));
            float frac = a.compBatchTotal ? (float)a.compBatchDone / a.compBatchTotal : 0.f;
            g.roundRect(rectOf(pr2.left, pr2.top, rw(pr2) * frac, rh(pr2)), g.s(3.f), a.th.accent);
            g.text(std::to_wstring(a.compBatchDone) + L" / " + std::to_wstring(a.compBatchTotal),
                   g.fCaption.Get(), rectOf(col.left, y, rw(col) - g.s(92.f), bh2),
                   a.th.textDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
            textButton(a, UI_COMP_BASE + 63, CMD_COMP_CANCEL,
                       rectOf(col.right - g.s(86.f), y, g.s(86.f), bh2),
                       T(L"Скасувати"), nullptr, { false, true, true, false, 5.f });
            a.requestAnim();
        } else {
            size_t n = 0;
            for (size_t i = 0; i < a.folder.count(); ++i)
                if (!isVideoPath(a.folder.at((int)i).name)) ++n;
            textButton(a, UI_COMP_BASE + 62, CMD_COMP_BATCH, rectOf(col.left, y, rw(col), bh2),
                       T(L"Уся папка…") + (n ? (L"  (" + std::to_wstring(n) + L")") : L""),
                       T(L"Обробити всі зображення папки в іншу теку"),
                       { false, n > 0 && ready, false, false, 5.f });
        }
        y += bh2 + g.s(12.f);
    }

    g.dc->PopAxisAlignedClip();

    float contentH = (y + a.compScroll) - col.top;
    a.compScrollMax = std::max(0.f, contentH - rh(col));
    a.compScroll = clampf(a.compScroll, 0.f, a.compScrollMax);
    if (a.compScrollMax > 1.f) {
        float trackH = rh(col) - g.s(8.f);
        float thumbH = std::max(g.s(28.f), trackH * (rh(col) / std::max(1.f, contentH)));
        float t = a.compScroll / a.compScrollMax;
        g.roundRect(rectOf(panel.right - g.s(6.f), col.top + g.s(4.f) + t * (trackH - thumbH),
                           g.s(3.f), thumbH), g.s(1.5f), alpha(a.th.text, 0.30f));
    }

    // The canvas is the preview, so the wait belongs on the canvas.
    if (a.compPending) {
        drawSpinner(g, D2D1::Point2F((a.R.canvas.left + a.R.canvas.right) * .5f,
                                     a.R.canvas.top + g.s(44.f)),
                    g.s(13.f), a.th.textDim, nowSec());
        a.requestAnim();
    }
}

static void drawSettings(App& a) {
    if (!a.settingsOpen) return;
    Gfx& g = a.gfx;
    g_overUi = true;
    a.loadAssociations();

    g.dc->FillRectangle(a.R.client, g.solid(D2D1::ColorF(0, 0, 0, a.th.dark ? 0.58f : 0.38f)));

    // The sheet shrinks with the window: below ~600 dip the side rail turns into
    // a row of icon tabs so the content keeps a usable width.
    float w = std::min(g.s(780.f), (float)g.width - g.s(20.f));
    float h = std::min((float)g.height - g.s(20.f), g.s(620.f));
    w = std::max(w, std::min((float)g.width - g.s(8.f), g.s(300.f)));
    h = std::max(h, std::min((float)g.height - g.s(8.f), g.s(220.f)));
    D2D1_RECT_F card = rectOf((g.width - w) * .5f, (g.height - h) * .5f, w, h);
    shadowPill(g, card, g.s(12.f), a.th.shadow, 1.f);
    g.roundRect(card, g.s(12.f), alpha(a.th.bar, 0.995f));
    g.roundRectStroke(card, g.s(12.f), a.th.barStroke, g.s(1.f));

    bool narrow = w < g.s(600.f);
    bool tiny = h < g.s(360.f);
    float hdr = tiny ? g.s(44.f) : g.s(56.f);
    float pad = narrow ? g.s(12.f) : g.s(20.f);

    g.text(T(L"Налаштування"), tiny ? g.fBodyStrong.Get() : g.fTitle.Get(),
           rectOf(card.left + pad, card.top + g.s(tiny ? 8.f : 12.f),
                  w - pad * 2 - g.s(40.f), g.s(32.f)), a.th.text);
    // Closing is handled here rather than through a toggle command, so a single
    // click can never re-open the sheet on the same frame.
    if (button(a, UI_SET_BASE, CMD_NONE,
               rectOf(card.right - pad - g.s(32.f), card.top + g.s(tiny ? 6.f : 12.f), g.s(32.f), g.s(32.f)),
               ico::Close, T(L"Закрити"))) {
        a.settingsOpen = false;
        a.cfg.save();
        a.settingsDirty = false;
        a.invalidate();
        return;
    }
    g.dc->FillRectangle(rectOf(card.left, card.top + hdr, w, g.s(1.f)), g.solid(a.th.stroke));

    // ---------------- navigation rail
    const wchar_t* navIcons[] = { L"\uE790", L"\uE71E", L"\uE714", L"\uE737", L"\uE8B7" };
    const wchar_t* navNames[] = { T(L"Вигляд"), T(L"Перегляд"), T(L"Відео"), T(L"Вікна"), T(L"Формати") };
    const int navCount = 5;

    float navTop = card.top + hdr + g.s(1.f);
    float railW = narrow ? 0.f : g.s(186.f);
    float tabsH = narrow ? g.s(46.f) : 0.f;

    auto navItem = [&](int i, D2D1_RECT_F ir, bool iconOnly) {
        bool hovered = inRect(ir, a.in.mouse) && a.in.hasMouse;
        int uid = UI_SET_BASE + 300 + i;
        if (hovered) { g_overUi = true; a.hot = uid; useCursor(a, IDC_HAND); }
        if (hovered && a.in.pressed) a.active = uid;
        if (a.in.released && a.active == uid) {
            if (hovered && a.settingsTab != i) { a.settingsTab = i; a.settingsScroll = 0; }
            a.active = 0;
        }
        bool sel = (a.settingsTab == i);
        if (sel) g.roundRect(ir, g.s(6.f), alpha(a.th.accent, 0.18f));
        else if (hovered) g.roundRect(ir, g.s(6.f), a.th.cardHover);

        if (iconOnly) {
            g.text(navIcons[i], g.fIcon.Get(), ir, sel ? a.th.accent : a.th.textDim,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            if (sel)
                g.roundRect(rectOf((ir.left + ir.right) * .5f - g.s(9.f), ir.bottom - g.s(3.f),
                                   g.s(18.f), g.s(2.5f)), g.s(1.2f), a.th.accent);
            if (hovered) { g_tip = navNames[i]; g_tipAnchor = ir; }
        } else {
            if (sel)
                g.roundRect(rectOf(ir.left + g.s(2.f), (ir.top + ir.bottom) * .5f - g.s(9.f),
                                   g.s(3.f), g.s(18.f)), g.s(1.5f), a.th.accent);
            g.text(navIcons[i], g.fIcon.Get(), rectOf(ir.left + g.s(12.f), ir.top, g.s(24.f), rh(ir)),
                   sel ? a.th.accent : a.th.textDim, DWRITE_TEXT_ALIGNMENT_CENTER);
            g.text(navNames[i], g.fBody.Get(), rectOf(ir.left + g.s(44.f), ir.top, rw(ir) - g.s(50.f), rh(ir)),
                   sel ? a.th.text : a.th.textDim);
        }
    };

    if (narrow) {
        float tw = std::min(g.s(64.f), (w - pad * 2) / navCount);
        float tx = (card.left + card.right - tw * navCount) * .5f;
        for (int i = 0; i < navCount; ++i)
            navItem(i, rectOf(tx + i * tw, navTop + g.s(4.f), tw - g.s(3.f), g.s(38.f)), true);
        g.dc->FillRectangle(rectOf(card.left, navTop + tabsH - g.s(1.f), w, g.s(1.f)), g.solid(a.th.stroke));
    } else {
        D2D1_RECT_F rail = rectOf(card.left, navTop, railW, h - hdr - g.s(1.f));
        g.dc->FillRectangle(rectOf(rail.right - g.s(1.f), rail.top, g.s(1.f), rh(rail)), g.solid(a.th.stroke));
        float ny = rail.top + g.s(10.f);
        for (int i = 0; i < navCount; ++i) {
            navItem(i, rectOf(rail.left + g.s(8.f), ny, railW - g.s(16.f), g.s(38.f)), false);
            ny += g.s(42.f);
        }
    }

    // ---------------- content
    D2D1_RECT_F body = rectOf(card.left + railW, navTop + tabsH,
                              w - railW, h - hdr - g.s(1.f) - tabsH);
    D2D1_RECT_F inner = rectOf(body.left + pad, body.top, rw(body) - pad * 2 - g.s(6.f), rh(body));
    g.dc->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);

    SetRow s{ a, rectOf(inner.left - pad, body.top, rw(inner) + pad * 2, rh(body)),
              body.top + g.s(6.f) - a.settingsScroll, pad, narrow ? g.s(170.f) : g.s(230.f) };
    float startY = s.y;
    float rowH = g.s(40.f), rowH2 = g.s(48.f);
    auto touched = [&] { a.settingsDirty = true; };

    switch (a.settingsTab) {
    case 0: {   // ------------------------------------------------ appearance
        // Language names stay in their own language, never translated.
        const wchar_t* langs[] = { L"Українська", L"English", L"Русский" };
        int lp = segmented(a, UI_SET_BASE + 5, setLabel(s, T(L"Мова"), nullptr, rowH),
                           langs, 3, clampi(a.cfg.lang, 0, 2));
        if (lp >= 0) {
            a.cfg.lang = lp;
            g_lang.store(lp, std::memory_order_relaxed);
            touched();
        }

        const wchar_t* themes[] = { T(L"Система"), T(L"Темна"), T(L"Світла") };
        int p = segmented(a, UI_SET_BASE + 10, setLabel(s, T(L"Тема"), nullptr, rowH), themes, 3, a.cfg.themeMode);
        if (p >= 0) { a.cfg.themeMode = p; a.applyTheme(); touched(); }

        const wchar_t* backs[] = { T(L"Немає"), L"Mica", L"Acrylic", T(L"Розмиття") };
        p = segmented(a, UI_SET_BASE + 20,
                      setLabel(s, T(L"Задник вікна"), T(L"Що видно за фотографією"), rowH2), backs, 4, a.cfg.backdrop);
        if (p >= 0) { a.cfg.backdrop = p; a.applyTheme(); touched(); }

        D2D1_RECT_F cr = setLabel(s, T(L"Затемнення задника"), nullptr, rowH);
        float v = a.cfg.canvasDim / 100.f, outv = v;
        if (slider(a, UI_SET_BASE + 30, rectOf(cr.left, cr.top + rowH * .5f - g.s(7.f), rw(cr) - g.s(52.f), g.s(14.f)),
                   v, outv, 1.f, 4.f)) {
            a.cfg.canvasDim = clampi((int)lround(outv * 100.f), 0, 100);
            a.applyTheme(); touched();
        }
        g.text(std::to_wstring(a.cfg.canvasDim) + L"%", g.fCaption.Get(),
               rectOf(cr.right - g.s(46.f), cr.top, g.s(46.f), rowH), a.th.textDim, DWRITE_TEXT_ALIGNMENT_TRAILING);

        // Delay before the title strip and the command bar fade out. The last
        // step past 10 s means "leave them up".
        D2D1_RECT_F hr = setLabel(s, T(L"Приховувати панелі"),
                                  T(L"Час бездіяльності до зникання"), rowH2);
        const int kNever = 21;          // 0..20 -> 0.0..10.0 s, 21 -> never
        int hidx = (a.cfg.barHideMs < 0) ? kNever
                                         : clampi(a.cfg.barHideMs / 500, 0, kNever - 1);
        float hv = (float)hidx / (float)kNever, houtv = hv;
        if (slider(a, UI_SET_BASE + 35,
                   rectOf(hr.left, hr.top + rowH2 * .5f - g.s(7.f), rw(hr) - g.s(72.f), g.s(14.f)),
                   hv, houtv, 1.f, 4.f)) {
            int ni = clampi((int)lround(houtv * kNever), 0, kNever);
            a.cfg.barHideMs = (ni == kNever) ? -1 : ni * 500;
            a.requestAnim(); touched();
        }
        {
            wchar_t hb[32];
            if (a.cfg.barHideMs < 0)       wcscpy(hb, T(L"Не ховати"));
            else if (a.cfg.barHideMs == 0) wcscpy(hb, T(L"Одразу"));
            else swprintf(hb, 32, T(L"%.1f с"), a.cfg.barHideMs / 1000.0);
            g.text(hb, g.fCaption.Get(), rectOf(hr.right - g.s(66.f), hr.top, g.s(66.f), rowH2),
                   a.th.textDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        if (toggleSwitch(a, UI_SET_BASE + 40,
                         setLabel(s, T(L"Стрічка кадрів"), T(L"Накладається знизу, не зменшує фото"), rowH2),
                         a.cfg.filmstrip)) {
            a.cfg.filmstrip = !a.cfg.filmstrip; a.needRelayout = true; touched();
        }
        break;
    }
    case 1: {   // ------------------------------------------------ viewing
        const wchar_t* autos[] = { T(L"Вимкнено"), T(L"Під вікно"), T(L"Вікно під фото") };
        int p = segmented(a, UI_SET_BASE + 50, setLabel(s, T(L"Автопідгонка"), T(L"Клавіша A"), rowH2), autos, 3, a.cfg.autoSize);
        if (p >= 0) {
            a.cfg.autoSize = p;
            a.fitMode = (p == 0) ? Fit::Actual : Fit::Window;
            a.autoSizeWindow();
            a.applyFit(true);
            touched();
        }

        const wchar_t* wheels[] = { T(L"Масштаб"), T(L"Гортання") };
        p = segmented(a, UI_SET_BASE + 60, setLabel(s, T(L"Колесо миші"), nullptr, rowH), wheels, 2, a.cfg.wheelMode);
        if (p >= 0) { a.cfg.wheelMode = p; touched(); }

        if (toggleSwitch(a, UI_SET_BASE + 70,
                         setLabel(s, T(L"Запам'ятовувати масштаб"), T(L"Для кожного файлу окремо"), rowH2),
                         a.cfg.rememberZoom)) { a.cfg.rememberZoom = !a.cfg.rememberZoom; touched(); }
        if (toggleSwitch(a, UI_SET_BASE + 80, setLabel(s, T(L"Згладжування при збільшенні"), nullptr, rowH), a.cfg.smoothing))
        { a.cfg.smoothing = !a.cfg.smoothing; touched(); }
        if (toggleSwitch(a, UI_SET_BASE + 90, setLabel(s, T(L"Гортати папку по колу"), nullptr, rowH), a.cfg.loopFolder))
        { a.cfg.loopFolder = !a.cfg.loopFolder; touched(); }

        D2D1_RECT_F hr = setLabel(s, T(L"Гарячі клавіші"), T(L"Клавіша F1"), rowH2);
        textButton(a, UI_SET_BASE + 105, CMD_HELP,
                   rectOf(hr.right - g.s(150.f), hr.top + g.s(8.f), g.s(150.f), g.s(32.f)),
                   T(L"Показати"), nullptr, { false, true, false, false, 5.f });

        D2D1_RECT_F cr = setLabel(s, T(L"Інтервал слайдшоу"), nullptr, rowH);
        float v = clampf((a.cfg.slideshowMs - 500) / 14500.f, 0.f, 1.f), outv = v;
        if (slider(a, UI_SET_BASE + 100, rectOf(cr.left, cr.top + rowH * .5f - g.s(7.f), rw(cr) - g.s(52.f), g.s(14.f)),
                   v, outv, 1.f, 4.f)) {
            a.cfg.slideshowMs = clampi((int)lround(500 + outv * 14500.f), 500, 15000); touched();
        }
        wchar_t sb[32];
        swprintf(sb, 32, T(L"%.1f с"), a.cfg.slideshowMs / 1000.0);
        g.text(sb, g.fCaption.Get(), rectOf(cr.right - g.s(46.f), cr.top, g.s(46.f), rowH),
               a.th.textDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
    }
    case 2: {   // ------------------------------------------------ video
        if (toggleSwitch(a, UI_SET_BASE + 110, setLabel(s, T(L"Починати відтворення одразу"), nullptr, rowH), a.cfg.autoPlay))
        { a.cfg.autoPlay = !a.cfg.autoPlay; touched(); }

        if (toggleSwitch(a, UI_SET_BASE + 115,
                         setLabel(s, T(L"Продовжувати з місця зупинки"), T(L"Окремо для кожного відео"), rowH2),
                         a.cfg.resumeVideo)) { a.cfg.resumeVideo = !a.cfg.resumeVideo; touched(); }

        D2D1_RECT_F cr = setLabel(s, T(L"Гучність"), nullptr, rowH);
        float v = a.cfg.volume / 100.f, outv = v;
        if (slider(a, UI_SET_BASE + 120, rectOf(cr.left, cr.top + rowH * .5f - g.s(7.f), rw(cr) - g.s(52.f), g.s(14.f)),
                   v, outv, 1.f, 4.f)) {
            a.cfg.volume = clampi((int)lround(outv * 100.f), 0, 100);
            if (a.videoMode) { a.video.setVolume(a.cfg.volume / 100.f); a.video.setMuted(false); a.cfg.muted = false; }
            touched();
        }
        g.text(std::to_wstring(a.cfg.volume) + L"%", g.fCaption.Get(),
               rectOf(cr.right - g.s(46.f), cr.top, g.s(46.f), rowH), a.th.textDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
    }
    case 3: {   // ------------------------------------------------ windows
        if (toggleSwitch(a, UI_SET_BASE + 130,
                         setLabel(s, T(L"Кожен файл у власному вікні"),
                                  T(L"Інакше файли відкриваються в цьому ж вікні (швидше)"), rowH2),
                         !a.cfg.singleInstance)) {
            a.cfg.singleInstance = !a.cfg.singleInstance;
            a.cfg.save();               // new processes read this immediately
            a.settingsDirty = false;
        }

        if (toggleSwitch(a, UI_SET_BASE + 135,
                         setLabel(s, T(L"Поверх усіх вікон"), T(L"Клавіша P"), rowH2), a.cfg.alwaysOnTop)) {
            a.cfg.alwaysOnTop = !a.cfg.alwaysOnTop;
            a.applyTopmost();
            touched();
        }

        D2D1_RECT_F cr = setLabel(s, T(L"Відкрити ще одне вікно"), L"Ctrl+N", rowH2);
        textButton(a, UI_SET_BASE + 140, CMD_NEWWINDOW,
                   rectOf(cr.right - g.s(150.f), cr.top + g.s(8.f), g.s(150.f), g.s(32.f)),
                   T(L"Нове вікно"), nullptr, { false, true, false, false, 5.f });
        break;
    }
    default: {  // ------------------------------------------------ file types
        // The label formats do not wrap, so the note is three short lines.
        const wchar_t* note[] = {
            T(L"Позначте формати й натисніть «Застосувати»."),
            T(L"Windows 11 не дозволяє програмі самій стати стандартною."),
            T(L"Далі натисніть «Зробити стандартною» і підтвердіть вибір у Windows."),
        };
        for (int i = 0; i < 3; ++i) {
            g.text(note[i], g.fSmall.Get(), rectOf(inner.left, s.y, rw(inner), g.s(17.f)),
                   i == 0 ? a.th.textDim : a.th.textMute);
            s.y += g.s(17.f);
        }
        s.y += g.s(12.f);

        std::vector<size_t> popular, rest;
        for (size_t i = 0; i < a.assocAll.size(); ++i)
            (isPopularExt(a.assocAll[i]) ? popular : rest).push_back(i);

        g.text(T(L"Найпоширеніші"), g.fBodyStrong.Get(),
               rectOf(inner.left, s.y, rw(inner) - g.s(150.f), g.s(22.f)), a.th.accent);
        textButton(a, UI_SET_BASE + 180, CMD_ASSOC_POPULAR,
                   rectOf(inner.right - g.s(150.f), s.y - g.s(4.f), g.s(150.f), g.s(28.f)),
                   T(L"Позначити всі"), nullptr, { false, true, false, false, 5.f });
        s.y += g.s(30.f);
        assocGrid(a, inner, s.y, popular, body);
        s.y += g.s(18.f);

        g.text(T(L"Решта"), g.fBodyStrong.Get(), rectOf(inner.left, s.y, rw(inner), g.s(22.f)), a.th.accent);
        s.y += g.s(28.f);
        assocGrid(a, inner, s.y, rest, body);
        s.y += g.s(16.f);

        float bw = g.s(150.f), bh = g.s(34.f);
        float bx = inner.left;
        textButton(a, UI_SET_BASE + 150, CMD_ASSOC_APPLY, rectOf(bx, s.y, bw, bh),
                   T(L"Застосувати"), nullptr, { false, true, false, true, 5.f });
        bx += bw + g.s(8.f);
        textButton(a, UI_SET_BASE + 160, CMD_ASSOC_CLEAR, rectOf(bx, s.y, g.s(110.f), bh),
                   T(L"Зняти все"), nullptr, { false, true, false, false, 5.f });
        bx += g.s(118.f);
        textButton(a, UI_SET_BASE + 170, CMD_ASSOC_WINDOWS, rectOf(bx, s.y, g.s(200.f), bh),
                   T(L"Зробити стандартною"), nullptr, { false, true, false, false, 5.f });
        s.y += bh + g.s(14.f);
        break;
    }
    }

    g.dc->PopAxisAlignedClip();

    float contentH = s.y - startY;
    a.settingsScrollMax = std::max(0.f, contentH - rh(body) + g.s(16.f));
    a.settingsScroll = clampf(a.settingsScroll, 0.f, a.settingsScrollMax);

    if (a.settingsScrollMax > 1.f) {
        float trackH = rh(body) - g.s(12.f);
        float thumbH = std::max(g.s(30.f), trackH * (rh(body) / std::max(1.f, contentH)));
        float t = a.settingsScroll / a.settingsScrollMax;
        g.roundRect(rectOf(card.right - g.s(9.f), body.top + g.s(6.f) + t * (trackH - thumbH),
                           g.s(4.f), thumbH), g.s(2.f), alpha(a.th.text, 0.28f));
    }

    // Persist once the interaction ends, so a dragged slider does not thrash the file.
    if (a.settingsDirty && a.in.released) { a.cfg.save(); a.settingsDirty = false; }

    if (a.in.pressed && !inRect(card, a.in.mouse)) {
        a.settingsOpen = false;
        if (a.settingsDirty) { a.cfg.save(); a.settingsDirty = false; }
    }
}

// --------------------------------------------------------------- overlays
static void drawToast(App& a) {
    if (a.toast.text.empty()) return;
    double left = a.toast.until - nowSec();
    if (left <= 0) { a.toast.text.clear(); return; }
    Gfx& g = a.gfx;
    float op = clampf((float)left * 3.f, 0.f, 1.f);
    float w = g.measure(a.toast.text, g.fBody.Get()).width + g.s(36.f);
    float h = g.s(38.f);
    D2D1_RECT_F r = rectOf((a.R.content.left + a.R.content.right - w) / 2,
                           a.R.canvas.bottom - rh(a.R.filmstrip) - h - g.s(86.f), w, h);
    shadowPill(g, r, h / 2, a.th.shadow, op);
    g.roundRect(r, h / 2, alpha(a.th.bar, op * 0.98f));
    g.roundRectStroke(r, h / 2, alpha(a.th.barStroke, op), g.s(1.f));
    g.text(a.toast.text, g.fBody.Get(), r, alpha(a.th.text, op), DWRITE_TEXT_ALIGNMENT_CENTER);
    a.requestAnim();
}

static void drawTooltip(App& a) {
    Gfx& g = a.gfx;
    if (a.hot != g_hotPrev) { g_hotPrev = a.hot; g_hotSince = nowSec(); }
    if (g_tip.empty() || a.hot == 0) return;
    if (nowSec() - g_hotSince < 0.45) { a.requestAnim(); return; }

    float w = g.measure(g_tip, g.fCaption.Get()).width + g.s(20.f);
    float h = g.s(28.f);
    float x = clampf((g_tipAnchor.left + g_tipAnchor.right) / 2 - w / 2, g.s(6.f), g.width - w - g.s(6.f));
    float y = g_tipAnchor.bottom + g.s(6.f);
    if (y + h > g.height - g.s(6.f)) y = g_tipAnchor.top - h - g.s(6.f);
    D2D1_RECT_F r = rectOf(x, y, w, h);
    shadowPill(g, r, g.s(5.f), a.th.shadow, 1.f);
    g.roundRect(r, g.s(5.f), alpha(a.th.bar, 0.99f));
    g.roundRectStroke(r, g.s(5.f), a.th.barStroke, g.s(1.f));
    g.text(g_tip, g.fCaption.Get(), r, a.th.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

struct HelpRow { const wchar_t* keys; const wchar_t* what; };

static void drawHelp(App& a) {
    if (!a.showHelp) return;
    Gfx& g = a.gfx;
    const HelpRow videoRows[] = {
        { T(L"Space  /  клік"),    T(L"Відтворити / пауза") },
        { L"←  →",              T(L"Перемотати −5 / +5 секунд") },
        { L"Shift+←  →",        T(L"Перемотати −30 / +30 секунд") },
        { L"↑  ↓",              T(L"Гучність") },
        { L"M",                 T(L"Вимкнути / увімкнути звук") },
        { L"S",                 T(L"Швидкість відтворення") },
        { L"A",                 T(L"Автопідгонка вікна та масштабу") },
        { L"PgUp  PgDn",        T(L"Попередній / наступний файл") },
        { T(L"Подвійний клік"),    T(L"Повний екран") },
        { L"F11  /  F",         T(L"Повний екран") },
        { L"G  /  Esc",         T(L"Сітка папки / вихід") },
        { L"I",                 T(L"Панель відомостей") },
        { L"T",                 T(L"Стрічка кадрів") },
        { L"Delete",            T(L"Перемістити в кошик") },
        { L"F1",                T(L"Ця довідка") },
    };
    const HelpRow rows[] = {
        { L"← →  /  PgUp PgDn", T(L"Попереднє / наступне зображення") },
        { L"Home  End",         T(L"Перше / останнє") },
        { T(L"+  −  /  Ctrl+Колесо"), T(L"Збільшити / зменшити") },
        { L"0  /  1",           T(L"Вписати у вікно / масштаб 100%") },
        { T(L"Колесо"),            T(L"Масштаб (або гортання — див. W)") },
        { L"W",                 T(L"Змінити дію колеса") },
        { L"A",                 T(L"Автопідгонка: вимк / під вікно / вікно під фото") },
        { T(L"Перетягування"),     T(L"Панорамування") },
        { T(L"Подвійний клік"),    T(L"Вписати / 100%") },
        { L"F11  /  F",         T(L"Повний екран") },
        { L"G  /  Esc",         T(L"Сітка папки / вихід") },
        { L"Space",             T(L"Слайдшоу") },
        { L"R  /  Shift+R",     T(L"Обернути праворуч / ліворуч") },
        { L"H  /  V",           T(L"Дзеркально по горизонталі / вертикалі") },
        { L"I",                 T(L"Панель відомостей") },
        { L"T",                 T(L"Стрічка кадрів") },
        { L"Ctrl+C",            T(L"Копіювати зображення") },
        { L"Ctrl+O",            T(L"Відкрити файл") },
        { L"Ctrl+N",            T(L"Відкрити нове вікно") },
        { L"Ctrl+,",            T(L"Налаштування") },
        { L"P",                 T(L"Закріпити поверх усіх вікон") },
        { L"Delete",            T(L"Перемістити в кошик") },
        { L"P",                 T(L"Закріпити поверх усіх вікон") },
        { L"Ctrl+N  /  Ctrl+,", T(L"Нове вікно / налаштування") },
        { L"F1",                T(L"Ця довідка") },
    };
    const HelpRow* list = a.videoMode ? videoRows : rows;
    const int N = a.videoMode ? (int)(sizeof(videoRows) / sizeof(videoRows[0]))
                              : (int)(sizeof(rows) / sizeof(rows[0]));

    g.dc->FillRectangle(a.R.client, g.solid(D2D1::ColorF(0, 0, 0, a.th.dark ? 0.55f : 0.35f)));
    float w = std::min(g.s(520.f), (float)g.width - g.s(48.f));
    float rowH = g.s(25.f);
    float h = std::min((float)g.height - g.s(48.f), rowH * N + g.s(86.f));
    D2D1_RECT_F card = rectOf((g.width - w) / 2, (g.height - h) / 2, w, h);
    shadowPill(g, card, g.s(12.f), a.th.shadow, 1.f);
    g.roundRect(card, g.s(12.f), alpha(a.th.bar, 0.99f));
    g.roundRectStroke(card, g.s(12.f), a.th.barStroke, g.s(1.f));
    g_overUi = true;

    float pad = g.s(24.f);
    g.text(a.videoMode ? T(L"Клавіші відео") : T(L"Гарячі клавіші"), g.fTitle.Get(), rectOf(card.left + pad, card.top + g.s(18.f), w - pad * 2, g.s(30.f)), a.th.text);
    button(a, UI_HELP_CLOSE, CMD_HELP, rectOf(card.right - pad - g.s(30.f), card.top + g.s(18.f), g.s(30.f), g.s(30.f)), ico::Close, nullptr);

    float y = card.top + g.s(58.f);
    float keyW = g.s(190.f);
    for (int i = 0; i < N && y + rowH <= card.bottom - g.s(6.f); ++i, y += rowH) {
        g.text(list[i].keys, g.fCaption.Get(), rectOf(card.left + pad, y, keyW, rowH), a.th.accent);
        g.text(list[i].what, g.fCaption.Get(), rectOf(card.left + pad + keyW, y, w - pad * 2 - keyW, rowH), a.th.textDim);
    }

    if (a.in.pressed && !inRect(card, a.in.mouse)) a.showHelp = false;
}

// --------------------------------------------------------------- canvas input
static void canvasInput(App& a) {
    if (a.view != View::Viewer) return;
    if (!inRect(a.R.canvas, a.in.mouse) || !a.in.hasMouse) return;
    if (g_overUi && !a.dragging) return;

    if (a.videoMode) {
        useCursor(a, IDC_HAND);
        // Click toggles playback, double click goes full screen.
        if (a.in.doubleClick) { a.pendingCmd = CMD_FULLSCREEN; return; }
        if (a.in.released && a.active == 0 && a.video.isOpen()) a.pendingCmd = CMD_PLAYPAUSE;
        return;
    }

    auto pic = a.current();
    {
        int sw = 0, sh = 0;
        if (a.sourceSize(sw, sh)) {
            float iw = sw * a.zoom, ih = sh * a.zoom;
            if (a.rot & 1) std::swap(iw, ih);
            if (a.dragging || iw > rw(a.R.canvas) + 1.f || ih > rh(a.R.canvas) + 1.f)
                useCursor(a, IDC_SIZEALL);
        }
    }
    if (a.in.pressed && pic && pic->bmp) {
        a.dragging = true;
        a.dragOrigin = POINT{ (LONG)a.in.mouse.x, (LONG)a.in.mouse.y };
        a.dragPanX = a.panX; a.dragPanY = a.panY;
    }
    if (a.dragging && a.in.down) {
        a.panX = a.dragPanX + (a.in.mouse.x - a.dragOrigin.x);
        a.panY = a.dragPanY + (a.in.mouse.y - a.dragOrigin.y);
        a.panTargetX = a.panX; a.panTargetY = a.panY;
        a.fitMode = Fit::Free;
        a.clampPan();
    }
    if (a.in.released) a.dragging = false;

    if (a.in.doubleClick && pic && pic->bmp) {
        a.pendingCmd = (a.fitMode == Fit::Window) ? CMD_ACTUAL : CMD_FIT;
    }
}

// --------------------------------------------------------------- frame
void uiFrame(App& a) {
    Gfx& g = a.gfx;
    if (a.needRelayout) uiLayout(a);
    g_overUi = false;
    g_tip.clear();
    a.hot = 0;
    a.wantCursor = IDC_ARROW;
    // Widgets under an open flyout must not react; the bounds come from the
    // previous frame, which is where the menu already was.
    g_blockOn = a.moreMenuOpen && rw(a.moreMenuBounds) > 1.f;
    g_block = a.moreMenuBounds;

    // Overlay surfaces claim the pointer before the canvas gets it.
    if (!a.fullscreen && inRect(a.R.titlebar, a.in.mouse) && a.in.hasMouse &&
        (!a.titleOverlay() || a.barAlpha > 0.25f)) g_overUi = true;
    if (rh(a.R.filmstrip) > 1 && a.barAlpha > 0.25f &&
        inRect(a.R.filmstrip, a.in.mouse) && a.in.hasMouse) g_overUi = true;
    if (rw(a.R.info) > 1 && inRect(a.R.info, a.in.mouse) && a.in.hasMouse) g_overUi = true;
    if (rw(a.R.comp) > 1 && inRect(a.R.comp, a.in.mouse) && a.in.hasMouse) g_overUi = true;
    if (a.view == View::Viewer && a.barAlpha > 0.05f && rw(a.R.commandBar) > 1 &&
        inRect(inflate(a.R.commandBar, g.s(10.f)), a.in.mouse) && a.in.hasMouse) g_overUi = true;
    if (a.moreMenuOpen || a.settingsOpen) g_overUi = true;
    if (a.view == View::Grid) g_overUi = true;

    if (a.view == View::Viewer) {
        drawImage(a);
        canvasInput(a);
        drawFilmstrip(a);
        if (a.videoMode) drawVideoBar(a); else drawCommandBar(a);
    } else {
        drawGrid(a);
        drawGridHeader(a);
        drawSortMenu(a);
    }

    drawInfoPanel(a);
    drawCompressor(a);
    drawTitlebar(a);
    if (a.moreMenuOpen && a.view == View::Viewer) {
        auto entries = buildActionMenu(a, a.videoMode);
        popupMenu(a, UI_MENU_BASE, a.moreMenuAnchor, entries, a.moreMenuOpen);
    }
    drawSettings(a);
    drawHelp(a);
    // Last, so a confirmation of something done inside Settings is not hidden
    // behind the very sheet that triggered it.
    drawToast(a);
    drawTooltip(a);

    a.in.pressed = a.in.released = a.in.doubleClick = false;
    a.in.wheel = 0;
}
