#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

namespace pv {
namespace {

FT_Library g_ft = nullptr;
bool       g_ftTried = false;

FT_Library freetype() {
    if (!g_ftTried) {
        g_ftTried = true;
        if (FT_Init_FreeType(&g_ft) != 0) g_ft = nullptr;
    }
    return g_ft;
}

// One rendered character, kept because a file name redraws sixty times a
// second and rasterising the same letter each time would be absurd.
struct Glyph {
    std::vector<uint8_t> a;          // coverage, w*h
    int w = 0, h = 0;
    int left = 0, top = 0;           // where it sits relative to the pen
    float advance = 0;
};

// The next code point, and how many bytes it took. Invalid input advances one
// byte and yields U+FFFD, so a mangled name cannot hang the drawing.
uint32_t utf8Next(const char* s, int& i) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t c = p[i];
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && (p[i + 1] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
        i += 2;
        return v;
    }
    if ((c & 0xF0) == 0xE0 && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
        i += 3;
        return v;
    }
    if ((c & 0xF8) == 0xF0 && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80 &&
        (p[i + 3] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) |
                     ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu);
        i += 4;
        return v;
    }
    ++i;
    return 0xFFFD;
}

std::string findFont(const char* family, bool bold) {
    static bool inited = false;
    if (!inited) { inited = true; FcInit(); }

    std::string path;
    FcPattern* pat = FcNameParse((const FcChar8*)(family && *family ? family : "sans-serif"));
    if (!pat) return path;
    if (bold) FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res;
    if (FcPattern* got = FcFontMatch(nullptr, pat, &res)) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(got, FC_FILE, 0, &file) == FcResultMatch && file)
            path = (const char*)file;
        FcPatternDestroy(got);
    }
    FcPatternDestroy(pat);
    return path;
}

}  // namespace

struct Font {
    FT_Face face = nullptr;
    float   size = 0;
    float   height = 0, asc = 0;
    std::map<uint32_t, Glyph> cache;

    const Glyph* glyph(uint32_t cp) {
        auto it = cache.find(cp);
        if (it != cache.end()) return &it->second;
        if (!face) return nullptr;
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0)
            return nullptr;

        Glyph g;
        FT_GlyphSlot s = face->glyph;
        g.w = (int)s->bitmap.width;
        g.h = (int)s->bitmap.rows;
        g.left = s->bitmap_left;
        g.top = s->bitmap_top;
        g.advance = (float)(s->advance.x) / 64.f;
        g.a.resize((size_t)g.w * g.h);
        for (int y = 0; y < g.h; ++y)
            memcpy(&g.a[(size_t)y * g.w], s->bitmap.buffer + (size_t)y * s->bitmap.pitch,
                   (size_t)g.w);
        return &cache.emplace(cp, std::move(g)).first->second;
    }
};

Font* fontOpen(const char* family, float pixelSize, bool bold) {
    Font* f = new Font();
    f->size = pixelSize;
    FT_Library ft = freetype();
    if (!ft) return f;

    std::string path = findFont(family, bold);
    if (path.empty() || FT_New_Face(ft, path.c_str(), 0, &f->face) != 0) {
        f->face = nullptr;
        return f;
    }
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)(pixelSize + 0.5f));
    f->height = (float)f->face->size->metrics.height / 64.f;
    f->asc = (float)f->face->size->metrics.ascender / 64.f;
    return f;
}

void fontClose(Font* f) {
    if (!f) return;
    if (f->face) FT_Done_Face(f->face);
    delete f;
}

float lineHeight(Font* f) { return f && f->face ? f->height : 0.f; }
float ascent(Font* f) { return f && f->face ? f->asc : 0.f; }

float textWidth(Font* f, const char* s) {
    if (!f || !f->face || !s) return 0.f;
    float x = 0;
    for (int i = 0; s[i];) {
        uint32_t cp = utf8Next(s, i);
        if (const Glyph* g = f->glyph(cp)) x += g->advance;
    }
    return x;
}

void drawText(Canvas& c, Font* f, const char* s, float x, float y, Color col) {
    if (!f || !f->face || !s || col.a <= 0.002f) return;
    float pen = x;
    float sa = std::max(0.f, std::min(1.f, col.a));

    for (int i = 0; s[i];) {
        uint32_t cp = utf8Next(s, i);
        const Glyph* g = f->glyph(cp);
        if (!g) continue;
        int gx = (int)lroundf(pen) + g->left;
        int gy = (int)lroundf(y) - g->top;

        int x0 = std::max(gx, c.cx0), x1 = std::min(gx + g->w, c.cx1);
        int y0 = std::max(gy, c.cy0), y1 = std::min(gy + g->h, c.cy1);
        for (int py = y0; py < y1; ++py) {
            uint8_t* row = c.px + (size_t)py * c.stride;
            const uint8_t* src = &g->a[(size_t)(py - gy) * g->w];
            for (int px = x0; px < x1; ++px) {
                float cov = src[px - gx] / 255.f;
                if (cov <= 0.004f) continue;
                float a = sa * cov;
                uint8_t* d = row + px * 4;
                float inv = 1.f - a;
                d[0] = (uint8_t)(col.b * a * 255.f + d[0] * inv + 0.5f);
                d[1] = (uint8_t)(col.g * a * 255.f + d[1] * inv + 0.5f);
                d[2] = (uint8_t)(col.r * a * 255.f + d[2] * inv + 0.5f);
                d[3] = (uint8_t)(a * 255.f + d[3] * inv + 0.5f);
            }
        }
        pen += g->advance;
    }
}

void drawTextIn(Canvas& c, Font* f, const char* s, Rect box, Color col, Align al) {
    if (!f || !f->face || !s) return;

    std::string text = s;
    float w = textWidth(f, text.c_str());
    if (w > box.w && box.w > 0) {
        // Cut from the end and say so, rather than letting it run past the edge.
        const char* dots = "...";
        float dw = textWidth(f, dots);
        while (!text.empty() && textWidth(f, text.c_str()) + dw > box.w) {
            // Step back one whole character, not one byte.
            size_t n = text.size();
            do { --n; } while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80);
            text.resize(n);
        }
        text += dots;
        w = textWidth(f, text.c_str());
    }

    float x = box.x;
    if (al == Align::Centre) x = box.x + (box.w - w) * 0.5f;
    else if (al == Align::Right) x = box.right() - w;
    float y = box.y + (box.h + f->asc - (f->height - f->asc)) * 0.5f;
    drawText(c, f, text.c_str(), x, y, col);
}

}  // namespace pv
