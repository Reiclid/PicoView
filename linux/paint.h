// A small painter over a block of pixels.
//
// The Windows build has Direct2D for this. There is no Direct2D here, and the
// answer is not to drag in a two-dimensional engine the size of the program:
// an interface made of rounded rectangles, lines and text needs perhaps three
// hundred lines of arithmetic. This is those three hundred lines.
//
// Everything works in premultiplied BGRA, which is what wl_shm's ARGB8888 is
// on a little-endian machine, so nothing is converted on the way to the screen.
// Edges are antialiased by coverage from a distance field rather than by
// supersampling - one sqrt per pixel near an edge, nothing anywhere else.
#pragma once

#include <cstdint>

namespace pv {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;    // straight alpha, 0..1
};

inline Color rgba(int r, int g, int b, float a = 1.f) {
    return Color{ r / 255.f, g / 255.f, b / 255.f, a };
}
inline Color fade(Color c, float k) { c.a *= k; return c; }

struct Canvas {
    uint8_t* px = nullptr;
    int w = 0, h = 0, stride = 0;
    // Nothing is drawn outside this. Saves every caller from clamping.
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;

    void reset() { cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }
    void clip(int x0, int y0, int x1, int y1);
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

inline Rect inset(Rect r, float d) { return Rect{ r.x + d, r.y + d, r.w - d * 2, r.h - d * 2 }; }

void clear(Canvas& c, Color col);
void fillRect(Canvas& c, Rect r, Color col);
void fillRound(Canvas& c, Rect r, float radius, Color col);
void strokeRound(Canvas& c, Rect r, float radius, float width, Color col);
// A soft edge under a panel. Not a real gaussian - a few falling steps, which
// at these sizes nobody can tell from one.
void dropShadow(Canvas& c, Rect r, float radius, float spread, float strength);

// Small solid triangles, which is what the arrows on the command bar are.
// Sampled four times a pixel rather than measured: at icon size that is both
// cheaper and better looking than any distance field.
void fillTri(Canvas& c, float x0, float y0, float x1, float y1,
             float x2, float y2, Color col);

struct Bitmap {
    const uint8_t* px = nullptr;         // premultiplied BGRA
    int w = 0, h = 0, stride = 0;
};

// Bilinear, and split across the cores when the job is big enough to be worth
// the threads. This is the one operation that has to keep up with a window
// being dragged.
void blit(Canvas& c, const Bitmap& src, Rect dst, bool smooth = true);

}  // namespace pv
