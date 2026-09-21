/* A worked example of a PicoView plugin - and a useful one.
 *
 * It does both of the things the interface allows, so that anyone writing a
 * plugin has a complete, compiling answer to "how do I ...":
 *
 *   decode   .pcx - a format neither WIC nor the built-in fallbacks read, so
 *                   adding this really does teach the viewer something new.
 *   enhance        - Catmull-Rom enlargement with a light unsharp mask, done
 *                   separably so a 12 megapixel photo doubles in well under a
 *                   second. Nothing here needs a GPU; a plugin wrapping DLSS,
 *                   NIS or an ONNX model would sit in exactly this function
 *                   and hand back the same PvImage.
 *
 * Build it with build.bat. It has no dependencies beyond the C runtime, which
 * is linked statically, so the DLL is a single file to copy.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../include/picoview_plugin.h"

static const PvHost* g_host = NULL;

/* ------------------------------------------------------------------ helpers */
static void* pv_alloc(size_t n) {
    return (g_host && g_host->alloc) ? g_host->alloc(n) : calloc(1, n);
}
static void pv_free(void* p) {
    if (g_host && g_host->release) g_host->release(p);
    else free(p);
}
static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* PicoView works in premultiplied alpha, so no channel may exceed alpha. A
   filter that overshoots and leaves colour above alpha shows up as a bright
   fringe, which is the classic way a resampler betrays itself. */
static void fix_premul(unsigned char* p) {
    if (p[0] > p[3]) p[0] = p[3];
    if (p[1] > p[3]) p[1] = p[3];
    if (p[2] > p[3]) p[2] = p[3];
}

/* ------------------------------------------------------------------ .pcx */
static int pcx_decode(const wchar_t* path, PvImage* out) {
    HANDLE h;
    DWORD  size, got;
    unsigned char* raw;
    int w, h_, planes, bpp, bpl, y, x, p, ok = PV_ERR_CORRUPT;
    unsigned char* row = NULL;
    unsigned char* dst = NULL;
    const unsigned char* pal = NULL;
    size_t pos;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return PV_ERR_FORMAT;
    size = GetFileSize(h, NULL);
    if (size < 129 || size > 64u * 1024 * 1024) { CloseHandle(h); return PV_ERR_FORMAT; }
    raw = (unsigned char*)malloc(size);
    if (!raw) { CloseHandle(h); return PV_ERR_MEMORY; }
    if (!ReadFile(h, raw, size, &got, NULL) || got != size) {
        CloseHandle(h); free(raw); return PV_ERR_CORRUPT;
    }
    CloseHandle(h);

    if (raw[0] != 0x0A || raw[2] != 1) { free(raw); return PV_ERR_FORMAT; }
    bpp = raw[3];
    w = (raw[9] << 8 | raw[8]) - (raw[5] << 8 | raw[4]) + 1;
    h_ = (raw[11] << 8 | raw[10]) - (raw[7] << 8 | raw[6]) + 1;
    planes = raw[65];
    bpl = raw[67] << 8 | raw[66];
    if (w <= 0 || h_ <= 0 || w > 32768 || h_ > 32768 || bpl <= 0) { free(raw); return PV_ERR_CORRUPT; }
    if (bpp != 8 || (planes != 1 && planes != 3)) { free(raw); return PV_ERR_UNSUPPORTED; }

    /* A 256 colour image keeps its palette in the last 769 bytes, behind a
       0x0C marker. Without one, the sixteen colour header palette is used. */
    if (planes == 1 && size > 769 && raw[size - 769] == 0x0C) pal = raw + size - 768;

    row = (unsigned char*)malloc((size_t)bpl * planes);
    dst = (unsigned char*)pv_alloc((size_t)w * h_ * 4);
    if (!row || !dst) { free(raw); free(row); pv_free(dst); return PV_ERR_MEMORY; }

    pos = 128;
    for (y = 0; y < h_; ++y) {
        int need = bpl * planes, at = 0;
        while (at < need) {
            unsigned char b, run = 1;
            if (pos >= size) { memset(row + at, 0, (size_t)(need - at)); at = need; break; }
            b = raw[pos++];
            if ((b & 0xC0) == 0xC0) {
                run = b & 0x3F;
                if (pos >= size) break;
                b = raw[pos++];
            }
            while (run-- && at < need) row[at++] = b;
        }
        for (x = 0; x < w; ++x) {
            unsigned char* o = dst + ((size_t)y * w + x) * 4;
            if (planes == 3) {
                o[2] = row[x];                  /* R */
                o[1] = row[bpl + x];            /* G */
                o[0] = row[bpl * 2 + x];        /* B */
            } else if (pal) {
                const unsigned char* c = pal + (size_t)row[x] * 3;
                o[2] = c[0]; o[1] = c[1]; o[0] = c[2];
            } else {
                const unsigned char* c = raw + 16 + (size_t)(row[x] & 15) * 3;
                o[2] = c[0]; o[1] = c[1]; o[0] = c[2];
            }
            o[3] = 255;                         /* PCX has no alpha */
        }
    }
    (void)p;
    free(row);
    free(raw);

    out->format = PV_BGRA8;
    out->width = w;
    out->height = h_;
    out->stride = w * 4;
    out->pixels = dst;
    out->owner = NULL;
    ok = PV_OK;
    return ok;
}

/* ------------------------------------------------------------------ enhance */
/* Catmull-Rom, the interpolating cubic. Unlike a plain bicubic it passes
   through its samples, so an enlargement stays as sharp as the original
   instead of turning soft. */
static void cubic_weights(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
    w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

static int enhance(const PvImage* in, int scale, PvImage* out) {
    int iw, ih, ow, oh, x, y, c, k;
    const unsigned char* src;
    unsigned char* tmp = NULL;
    unsigned char* dst = NULL;
    float w4[4];

    if (!in || !out || in->format != PV_BGRA8 || !in->pixels) return PV_ERR_FORMAT;
    if (scale != 1 && scale != 2 && scale != 4) return PV_ERR_UNSUPPORTED;
    iw = in->width;
    ih = in->height;
    if (iw < 2 || ih < 2) return PV_ERR_UNSUPPORTED;
    ow = iw * scale;
    oh = ih * scale;
    if ((long long)ow * oh > 80ll * 1000 * 1000) return PV_ERR_UNSUPPORTED;

    src = (const unsigned char*)in->pixels;
    /* Separable: across first into a tall strip, then down. Sixteen taps
       become eight, which is the difference between a pause and a wait. */
    tmp = (unsigned char*)malloc((size_t)ow * ih * 4);
    dst = (unsigned char*)pv_alloc((size_t)ow * oh * 4);
    if (!tmp || !dst) { free(tmp); pv_free(dst); return PV_ERR_MEMORY; }

    for (y = 0; y < ih; ++y) {
        const unsigned char* sr = src + (size_t)y * in->stride;
        unsigned char* dr = tmp + (size_t)y * ow * 4;
        for (x = 0; x < ow; ++x) {
            float sx = (x + 0.5f) / (float)scale - 0.5f;
            int   x0 = (int)floorf(sx);
            cubic_weights(sx - x0, w4);
            for (c = 0; c < 4; ++c) {
                float acc = 0;
                for (k = 0; k < 4; ++k) {
                    int xi = x0 - 1 + k;
                    if (xi < 0) xi = 0;
                    if (xi >= iw) xi = iw - 1;
                    acc += w4[k] * sr[xi * 4 + c];
                }
                dr[x * 4 + c] = (unsigned char)clamp255((int)(acc + 0.5f));
            }
            fix_premul(dr + x * 4);
        }
    }

    for (y = 0; y < oh; ++y) {
        float sy = (y + 0.5f) / (float)scale - 0.5f;
        int   y0 = (int)floorf(sy);
        unsigned char* dr = dst + (size_t)y * ow * 4;
        cubic_weights(sy - y0, w4);
        for (x = 0; x < ow; ++x) {
            for (c = 0; c < 4; ++c) {
                float acc = 0;
                for (k = 0; k < 4; ++k) {
                    int yi = y0 - 1 + k;
                    if (yi < 0) yi = 0;
                    if (yi >= ih) yi = ih - 1;
                    acc += w4[k] * tmp[((size_t)yi * ow + x) * 4 + c];
                }
                dr[x * 4 + c] = (unsigned char)clamp255((int)(acc + 0.5f));
            }
            fix_premul(dr + x * 4);
        }
    }
    free(tmp);

    /* A gentle unsharp mask over the result. Enlargement always costs some
       acutance and this puts a little of it back without ringing. */
    {
        unsigned char* copy = (unsigned char*)malloc((size_t)ow * oh * 4);
        if (copy) {
            memcpy(copy, dst, (size_t)ow * oh * 4);
            for (y = 1; y < oh - 1; ++y) {
                for (x = 1; x < ow - 1; ++x) {
                    unsigned char* o = dst + ((size_t)y * ow + x) * 4;
                    const unsigned char* p0 = copy + ((size_t)y * ow + x) * 4;
                    for (c = 0; c < 3; ++c) {
                        int blur = (copy[((size_t)(y - 1) * ow + x) * 4 + c] +
                                    copy[((size_t)(y + 1) * ow + x) * 4 + c] +
                                    copy[((size_t)y * ow + x - 1) * 4 + c] +
                                    copy[((size_t)y * ow + x + 1) * 4 + c] + 2) / 4;
                        o[c] = (unsigned char)clamp255(p0[c] + ((p0[c] - blur) * 45) / 100);
                    }
                    fix_premul(o);
                }
            }
            free(copy);
        }
    }

    out->format = PV_BGRA8;
    out->width = ow;
    out->height = oh;
    out->stride = ow * 4;
    out->pixels = dst;
    out->owner = NULL;
    return PV_OK;
}

static void free_image(PvImage* img) {
    if (!img || !img->pixels) return;
    pv_free(img->pixels);
    img->pixels = NULL;
}

static void shutdown_(void) { g_host = NULL; }

/* ------------------------------------------------------------------ entry */
__declspec(dllexport) int PicoViewPluginInit(const PvHost* host, PvPlugin* out) {
    if (!host || !out) return PV_ERR_FORMAT;
    if (host->abi != PV_ABI_VERSION) return PV_ERR_UNSUPPORTED;
    g_host = host;

    out->size = (uint32_t)sizeof(PvPlugin);
    out->caps = PV_CAP_DECODE | PV_CAP_ENHANCE;
    out->name = L"Приклад: PCX і збільшення";
    out->version = L"1.0";
    out->author = L"PicoView";
    out->description = L"Збільшує вдвічі й підвищує різкість";
    out->extensions = L".pcx";
    out->decode = pcx_decode;
    out->enhance = enhance;
    out->free_image = free_image;
    out->shutdown = shutdown_;
    return PV_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
