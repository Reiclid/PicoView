// Artwork for a track that has none.
//
// A song with no picture in it still has a name, and that is enough to make one
// from: the name hashes into a set of glowing lobes - a hot core with petals of
// neighbouring hues leaning out of it - which are then added together over
// black, the way light mixes rather than paint. The same song always comes back
// the same.
//
// The plan lives here because two renderers draw it: the viewer paints it with
// Direct2D so it can move with the music, and the thumbnail pipeline rasterises
// the resting pose once, on a worker thread, so a folder of songs costs nothing
// per frame.
#include "pg.h"

// A hash is all the randomness we want: stable, and cheap enough to redo.
static uint64_t hashOf(const wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) { h ^= (uint64_t)c; h *= 1099511628211ull; }
    return h ? h : 1;
}

namespace {
struct Rnd {
    uint64_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 32); }
    float f() { return (float)(next() & 0xFFFFFF) / (float)0x1000000; }
    float f(float a, float b) { return a + (b - a) * f(); }
    int   i(int n) { return n > 0 ? (int)(next() % (uint32_t)n) : 0; }
};
} // namespace

// Hue-Saturation-Lightness, because a palette of neighbouring hues is the whole
// idea and RGB cannot express that.
void coverHsl(float h, float sat, float l, float& r, float& g, float& b) {
    h = fmodf(h, 1.f);
    if (h < 0) h += 1.f;
    auto ch = [&](float n) {
        float k = fmodf(n + h * 12.f, 12.f);
        return l - sat * std::min(l, 1.f - l) *
               std::max(-1.f, std::min(std::min(k - 3.f, 9.f - k), 1.f));
    };
    r = ch(0.f);
    g = ch(8.f);
    b = ch(4.f);
}

void coverPlan(const wstring& path, CoverPlan& out) {
    out = CoverPlan();
    Rnd r{ hashOf(lowerOf(fileNameOf(path))) };
    r.next(); r.next();

    // A family of neighbouring hues rather than a scatter: the colours have to
    // read as one glowing thing, the way a flame goes red to orange to yellow.
    float h0 = r.f();
    float step = r.f(0.06f, 0.15f) * (r.i(2) ? 1.f : -1.f);
    int n = 5 + r.i(3);
    out.rot = r.f(-0.22f, 0.22f);
    out.spread = r.f(0.13f, 0.24f);

    for (int i = 0; i < n; ++i) {
        CoverLobe b;
        // The first one is the core: without something hot in the middle the
        // lobes read as separate stains rather than one light.
        bool core = (i == 0);
        float ang = r.f(0.f, 6.283f);
        float orb = core ? out.spread * 0.16f : out.spread * r.f(0.55f, 1.f);
        b.x = 0.5f + cosf(ang) * orb;
        b.y = 0.5f + sinf(ang) * orb;
        b.r = core ? r.f(0.40f, 0.52f) : r.f(0.34f, 0.58f);
        b.dx = r.f(-0.04f, 0.04f);
        b.dy = r.f(-0.04f, 0.04f);
        b.speed = r.f(0.15f, 0.42f);
        b.phase = ang;
        b.weight = core ? 1.f : r.f(0.55f, 1.f);
        // Petals point away from the middle, which is what turns a pile of
        // circles into something with a silhouette.
        b.aspect = core ? r.f(1.f, 1.15f) : r.f(1.25f, 2.1f);
        b.lean = ang;
        b.alpha = core ? 1.f : 0.95f;
        coverHsl(h0 + step * i + r.f(-0.02f, 0.02f), r.f(0.92f, 1.f), r.f(0.50f, 0.60f),
                 b.cr, b.cg, b.cb);
        out.lobes.push_back(b);
    }
}

// The falloff from the middle of a lobe to nothing, as the gradient stops the
// Direct2D version uses. Both renderers read from this so they agree.
float coverFalloff(float t) {
    static const float pos[5] = { 0.f, 0.20f, 0.46f, 0.74f, 1.f };
    static const float val[5] = { 1.f, 0.72f, 0.28f, 0.06f, 0.f };
    if (t <= 0) return 1.f;
    if (t >= 1) return 0.f;
    for (int i = 1; i < 5; ++i) {
        if (t <= pos[i]) {
            float f = (t - pos[i - 1]) / (pos[i] - pos[i - 1]);
            return val[i - 1] + (val[i] - val[i - 1]) * f;
        }
    }
    return 0.f;
}

// The resting pose, rasterised. Premultiplied BGRA, opaque, square.
void coverRaster(const wstring& path, int size, PixelBuf& out) {
    size = clampi(size, 32, 1024);
    CoverPlan plan;
    coverPlan(path, plan);

    out.w = out.h = size;
    out.px.assign((size_t)size * size * 4, 0);

    const float side = (float)size;
    const float vignetteFrom = 0.72f;      // matches the drawn version

    struct Prep { float cx, cy, ax, ay, cs, sn, cr, cg, cb, a; };
    std::vector<Prep> pre;
    pre.reserve(plan.lobes.size());
    for (const auto& b : plan.lobes) {
        Prep p;
        p.cx = b.x * side;
        p.cy = b.y * side;
        p.ax = std::max(1.f, b.r * side * b.aspect);
        p.ay = std::max(1.f, b.r * side);
        p.cs = cosf(-b.lean);
        p.sn = sinf(-b.lean);
        p.cr = b.cr; p.cg = b.cg; p.cb = b.cb;
        p.a = b.alpha * 0.82f;             // the drawn one also sits below full
        pre.push_back(p);
    }

    const float mid = side * .5f;
    const float vr = side * 0.78f;
    for (int y = 0; y < size; ++y) {
        uint8_t* row = out.px.data() + (size_t)y * size * 4;
        for (int x = 0; x < size; ++x) {
            float px = (float)x + .5f, py = (float)y + .5f;
            float r = 0.015f, g = 0.015f, b = 0.02f;
            for (const auto& p : pre) {
                float dx = px - p.cx, dy = py - p.cy;
                float rx = dx * p.cs - dy * p.sn;
                float ry = dx * p.sn + dy * p.cs;
                float t = sqrtf((rx * rx) / (p.ax * p.ax) + (ry * ry) / (p.ay * p.ay));
                if (t >= 1.f) continue;
                float w = coverFalloff(t) * p.a;
                r += p.cr * w;
                g += p.cg * w;
                b += p.cb * w;
            }
            // Corners stay black, so the light looks like it comes from inside.
            float d = sqrtf((px - mid) * (px - mid) + (py - mid) * (py - mid)) / vr;
            if (d > vignetteFrom) {
                float k = clampf(1.f - (d - vignetteFrom) / (1.f - vignetteFrom) * 0.92f, 0.f, 1.f);
                r *= k; g *= k; b *= k;
            }
            row[x * 4 + 0] = (uint8_t)(clampf(b, 0.f, 1.f) * 255.f + .5f);
            row[x * 4 + 1] = (uint8_t)(clampf(g, 0.f, 1.f) * 255.f + .5f);
            row[x * 4 + 2] = (uint8_t)(clampf(r, 0.f, 1.f) * 255.f + .5f);
            row[x * 4 + 3] = 255;
        }
    }
}
