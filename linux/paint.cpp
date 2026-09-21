#include "paint.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace pv {
namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Source over, on premultiplied pixels. `a` is the source alpha already folded
// into the colour.
inline void over(uint8_t* d, float sb, float sg, float sr, float sa) {
    float inv = 1.f - sa;
    d[0] = (uint8_t)(sb * 255.f + d[0] * inv + 0.5f);
    d[1] = (uint8_t)(sg * 255.f + d[1] * inv + 0.5f);
    d[2] = (uint8_t)(sr * 255.f + d[2] * inv + 0.5f);
    d[3] = (uint8_t)(sa * 255.f + d[3] * inv + 0.5f);
}

// Signed distance to a rounded rectangle. Negative inside, and the value is in
// pixels, which is what makes one-pixel antialiasing fall out of it.
inline float roundDist(float px, float py, Rect r, float rad) {
    rad = std::min(rad, std::min(r.w, r.h) * 0.5f);
    float dx = fabsf(px - (r.x + r.w * 0.5f)) - (r.w * 0.5f - rad);
    float dy = fabsf(py - (r.y + r.h * 0.5f)) - (r.h * 0.5f - rad);
    float ax = dx > 0 ? dx : 0, ay = dy > 0 ? dy : 0;
    float outside = sqrtf(ax * ax + ay * ay);
    float inside = std::min(std::max(dx, dy), 0.f);
    return outside + inside - rad;
}

}  // namespace

void Canvas::clip(int x0, int y0, int x1, int y1) {
    cx0 = clampi(x0, 0, w);
    cy0 = clampi(y0, 0, h);
    cx1 = clampi(x1, cx0, w);
    cy1 = clampi(y1, cy0, h);
}

void clear(Canvas& c, Color col) {
    uint32_t v = ((uint32_t)(clampf(col.a, 0, 1) * 255.f + 0.5f) << 24) |
                 ((uint32_t)(clampf(col.r * col.a, 0, 1) * 255.f + 0.5f) << 16) |
                 ((uint32_t)(clampf(col.g * col.a, 0, 1) * 255.f + 0.5f) << 8) |
                 ((uint32_t)(clampf(col.b * col.a, 0, 1) * 255.f + 0.5f));
    for (int y = c.cy0; y < c.cy1; ++y) {
        uint32_t* row = (uint32_t*)(c.px + (size_t)y * c.stride);
        for (int x = c.cx0; x < c.cx1; ++x) row[x] = v;
    }
}

void fillRect(Canvas& c, Rect r, Color col) {
    if (col.a <= 0.001f) return;
    int x0 = clampi((int)floorf(r.x), c.cx0, c.cx1);
    int y0 = clampi((int)floorf(r.y), c.cy0, c.cy1);
    int x1 = clampi((int)ceilf(r.right()), c.cx0, c.cx1);
    int y1 = clampi((int)ceilf(r.bottom()), c.cy0, c.cy1);
    float sa = clampf(col.a, 0, 1);
    float sb = col.b * sa, sg = col.g * sa, sr = col.r * sa;
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = c.px + (size_t)y * c.stride;
        for (int x = x0; x < x1; ++x) over(row + x * 4, sb, sg, sr, sa);
    }
}

void fillRound(Canvas& c, Rect r, float radius, Color col) {
    if (col.a <= 0.001f || r.w <= 0 || r.h <= 0) return;
    if (radius <= 0.01f) { fillRect(c, r, col); return; }

    int x0 = clampi((int)floorf(r.x) - 1, c.cx0, c.cx1);
    int y0 = clampi((int)floorf(r.y) - 1, c.cy0, c.cy1);
    int x1 = clampi((int)ceilf(r.right()) + 1, c.cx0, c.cx1);
    int y1 = clampi((int)ceilf(r.bottom()) + 1, c.cy0, c.cy1);

    // The straight middle needs no distance at all, so it is filled flat and
    // only the bands holding the corners are measured.
    float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    int midY0 = clampi((int)ceilf(r.y + rad) + 1, y0, y1);
    int midY1 = clampi((int)floorf(r.bottom() - rad) - 1, midY0, y1);

    float sa = clampf(col.a, 0, 1);
    auto band = [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            uint8_t* row = c.px + (size_t)y * c.stride;
            for (int x = x0; x < x1; ++x) {
                float d = roundDist(x + 0.5f, y + 0.5f, r, rad);
                float cov = clampf(0.5f - d, 0.f, 1.f);
                if (cov <= 0.002f) continue;
                float a = sa * cov;
                over(row + x * 4, col.b * a, col.g * a, col.r * a, a);
            }
        }
    };
    band(y0, midY0);
    if (midY1 > midY0)
        fillRect(c, Rect{ r.x, (float)midY0, r.w, (float)(midY1 - midY0) }, col);
    band(midY1, y1);
}

void strokeRound(Canvas& c, Rect r, float radius, float width, Color col) {
    if (col.a <= 0.001f || width <= 0) return;
    int x0 = clampi((int)floorf(r.x) - 1, c.cx0, c.cx1);
    int y0 = clampi((int)floorf(r.y) - 1, c.cy0, c.cy1);
    int x1 = clampi((int)ceilf(r.right()) + 1, c.cx0, c.cx1);
    int y1 = clampi((int)ceilf(r.bottom()) + 1, c.cy0, c.cy1);
    float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    float half = width * 0.5f;
    float sa = clampf(col.a, 0, 1);

    for (int y = y0; y < y1; ++y) {
        uint8_t* row = c.px + (size_t)y * c.stride;
        for (int x = x0; x < x1; ++x) {
            // The outline is the band where the distance is near zero.
            float d = fabsf(roundDist(x + 0.5f, y + 0.5f, r, rad) + half) - half;
            float cov = clampf(0.5f - d, 0.f, 1.f);
            if (cov <= 0.002f) continue;
            float a = sa * cov;
            over(row + x * 4, col.b * a, col.g * a, col.r * a, a);
        }
    }
}

void dropShadow(Canvas& c, Rect r, float radius, float spread, float strength) {
    if (strength <= 0.002f || spread <= 0) return;
    // Concentric rounded rectangles of falling alpha. Six of them is enough
    // that the banding is below what a dark edge shows.
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        float t = (float)i / steps;
        float grow = spread * t;
        float a = strength * (1.f - t) * (1.f - t) * 0.5f;
        fillRound(c, Rect{ r.x - grow, r.y - grow + spread * 0.25f,
                           r.w + grow * 2, r.h + grow * 2 },
                  radius + grow, Color{ 0, 0, 0, a });
    }
}

void fillTri(Canvas& c, float x0, float y0, float x1, float y1,
             float x2, float y2, Color col) {
    if (col.a <= 0.002f) return;
    int xa = clampi((int)floorf(std::min({ x0, x1, x2 })), c.cx0, c.cx1);
    int xb = clampi((int)ceilf(std::max({ x0, x1, x2 })) + 1, c.cx0, c.cx1);
    int ya = clampi((int)floorf(std::min({ y0, y1, y2 })), c.cy0, c.cy1);
    int yb = clampi((int)ceilf(std::max({ y0, y1, y2 })) + 1, c.cy0, c.cy1);

    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-6f) return;
    float inv = 1.f / d;
    float sa = clampf(col.a, 0, 1);

    for (int y = ya; y < yb; ++y) {
        uint8_t* row = c.px + (size_t)y * c.stride;
        for (int x = xa; x < xb; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    float px = x + 0.25f + sx * 0.5f, py = y + 0.25f + sy * 0.5f;
                    float l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) * inv;
                    float l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) * inv;
                    float l2 = 1.f - l0 - l1;
                    if (l0 >= 0 && l1 >= 0 && l2 >= 0) ++hits;
                }
            if (!hits) continue;
            float a = sa * hits * 0.25f;
            over(row + x * 4, col.b * a, col.g * a, col.r * a, a);
        }
    }
}

// --------------------------------------------------------------------- blit
namespace {

void blitRows(Canvas* c, const Bitmap* s, Rect d, int y0, int y1, bool smooth) {
    const float invW = (float)s->w / d.w;
    const float invH = (float)s->h / d.h;
    int xa = clampi((int)floorf(d.x), c->cx0, c->cx1);
    int xb = clampi((int)ceilf(d.right()), c->cx0, c->cx1);

    for (int y = y0; y < y1; ++y) {
        uint8_t* row = c->px + (size_t)y * c->stride;
        float sy = ((float)y - d.y + 0.5f) * invH - 0.5f;
        int   iy = (int)floorf(sy);
        float fy = smooth ? sy - iy : 0.f;
        int   y1i = clampi(iy, 0, s->h - 1);
        int   y2i = clampi(iy + 1, 0, s->h - 1);
        const uint8_t* r1 = s->px + (size_t)y1i * s->stride;
        const uint8_t* r2 = s->px + (size_t)y2i * s->stride;

        for (int x = xa; x < xb; ++x) {
            float sx = ((float)x - d.x + 0.5f) * invW - 0.5f;
            int   ix = (int)floorf(sx);
            float fx = smooth ? sx - ix : 0.f;
            int   x1i = clampi(ix, 0, s->w - 1);
            int   x2i = clampi(ix + 1, 0, s->w - 1);
            uint8_t* o = row + x * 4;
            float v[4];
            for (int k = 0; k < 4; ++k) {
                float top = r1[x1i * 4 + k] + (r1[x2i * 4 + k] - r1[x1i * 4 + k]) * fx;
                float bot = r2[x1i * 4 + k] + (r2[x2i * 4 + k] - r2[x1i * 4 + k]) * fx;
                v[k] = clampf(top + (bot - top) * fy, 0.f, 255.f);
            }
            // Over, not copy: a PNG with holes in it has to show the backdrop
            // through them rather than punch them out.
            float sa = v[3] / 255.f;
            if (sa >= 0.998f) {
                o[0] = (uint8_t)(v[0] + 0.5f); o[1] = (uint8_t)(v[1] + 0.5f);
                o[2] = (uint8_t)(v[2] + 0.5f); o[3] = 255;
            } else {
                float inv = 1.f - sa;
                for (int k = 0; k < 4; ++k)
                    o[k] = (uint8_t)(v[k] + o[k] * inv + 0.5f);
            }
        }
    }
}

}  // namespace

void blit(Canvas& c, const Bitmap& src, Rect dst, bool smooth) {
    if (!src.px || src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
    int y0 = clampi((int)floorf(dst.y), c.cy0, c.cy1);
    int y1 = clampi((int)ceilf(dst.bottom()), c.cy0, c.cy1);
    if (y1 <= y0) return;

    unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
    if ((y1 - y0) < 64) n = 1;
    if (n == 1) { blitRows(&c, &src, dst, y0, y1, smooth); return; }

    std::vector<std::thread> pool;
    int span = (y1 - y0 + (int)n - 1) / (int)n;
    for (unsigned i = 0; i < n; ++i) {
        int a = y0 + (int)i * span;
        int b = std::min(y1, a + span);
        if (a >= b) break;
        pool.emplace_back(blitRows, &c, &src, dst, a, b, smooth);
    }
    for (auto& t : pool) t.join();
}

}  // namespace pv
