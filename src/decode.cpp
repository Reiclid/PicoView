// Decoding pipeline.
//
// Speed strategy, in order of preference:
//   1. Preview  - embedded EXIF/container thumbnail, or the Explorer thumbnail
//                 cache. Lands in single-digit milliseconds, shown instantly.
//   2. Full     - WIC decode, scaled down *during* decode when the image is
//                 larger than the target. For JPEG this hits the DCT scaler
//                 (1/2, 1/4, 1/8) and is several times faster than a full
//                 decode followed by a resize.
//   3. Fallback - stb_image / QOI for anything WIC has no codec for.
#include "pg.h"
#include <propvarutil.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_PSD
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_ONLY_HDR
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#include "third_party/stb_image.h"

static ComPtr<IWICImagingFactory2> g_wic;
static std::vector<wstring>        g_exts;
static std::unordered_set<wstring> g_extSet;

// ------------------------------------------------------------------ utils
static void premulRGBAtoBGRA(const uint8_t* src, uint8_t* dst, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t r = src[0], g = src[1], b = src[2], a = src[3];
        if (a == 255) { dst[0] = (uint8_t)b; dst[1] = (uint8_t)g; dst[2] = (uint8_t)r; }
        else {
            dst[0] = (uint8_t)((b * a + 127) / 255);
            dst[1] = (uint8_t)((g * a + 127) / 255);
            dst[2] = (uint8_t)((r * a + 127) / 255);
        }
        dst[3] = (uint8_t)a;
        src += 4; dst += 4;
    }
}

static bool readWholeFile(const wstring& path, std::vector<uint8_t>& out, uint64_t& fileSize, FILETIME& mtime) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION fi{};
    GetFileInformationByHandle(h, &fi);
    uint64_t size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    fileSize = size; mtime = fi.ftLastWriteTime;
    if (size == 0 || size > (512ull << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)size);
    size_t off = 0;
    while (off < out.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - off, 32u << 20);
        DWORD got = 0;
        if (!ReadFile(h, out.data() + off, chunk, &got, nullptr) || got == 0) break;
        off += got;
    }
    CloseHandle(h);
    if (off != out.size()) { out.clear(); return false; }
    return true;
}

// ------------------------------------------------------------------ exif
static wstring pvString(IWICMetadataQueryReader* r, const wchar_t* q) {
    PROPVARIANT v; PropVariantInit(&v);
    wstring res;
    if (SUCCEEDED(r->GetMetadataByName(q, &v))) {
        if (v.vt == VT_LPSTR && v.pszVal) {
            int n = MultiByteToWideChar(CP_ACP, 0, v.pszVal, -1, nullptr, 0);
            if (n > 1) { res.resize(n - 1); MultiByteToWideChar(CP_ACP, 0, v.pszVal, -1, &res[0], n); }
        } else if (v.vt == VT_LPWSTR && v.pwszVal) {
            res = v.pwszVal;
        }
    }
    PropVariantClear(&v);
    while (!res.empty() && (res.back() == L' ' || res.back() == L'\0')) res.pop_back();
    return res;
}

static bool pvRational(IWICMetadataQueryReader* r, const wchar_t* q, double& out) {
    PROPVARIANT v; PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(r->GetMetadataByName(q, &v))) {
        if (v.vt == VT_UI8) {
            uint32_t num = (uint32_t)(v.uhVal.QuadPart & 0xFFFFFFFFull);
            uint32_t den = (uint32_t)(v.uhVal.QuadPart >> 32);
            if (den) { out = (double)num / den; ok = true; }
        } else if (v.vt == VT_I8) {
            int32_t num = (int32_t)(uint32_t)(v.hVal.QuadPart & 0xFFFFFFFFull);
            int32_t den = (int32_t)(uint32_t)(v.hVal.QuadPart >> 32);
            if (den) { out = (double)num / den; ok = true; }
        } else if (v.vt == VT_R8) { out = v.dblVal; ok = true; }
        else if (v.vt == VT_R4) { out = v.fltVal; ok = true; }
    }
    PropVariantClear(&v);
    return ok;
}

static bool pvUInt(IWICMetadataQueryReader* r, const wchar_t* q, unsigned& out) {
    PROPVARIANT v; PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(r->GetMetadataByName(q, &v))) {
        if (v.vt == VT_UI2) { out = v.uiVal; ok = true; }
        else if (v.vt == VT_UI4) { out = v.ulVal; ok = true; }
        else if (v.vt == VT_UI8) { out = (unsigned)v.uhVal.QuadPart; ok = true; }
        else if (v.vt == (VT_UI2 | VT_VECTOR) && v.caui.cElems > 0) { out = v.caui.pElems[0]; ok = true; }
    }
    PropVariantClear(&v);
    return ok;
}

static wstring fmtNum(double v, int decimals) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, v);
    wstring s = buf;
    if (s.find(L'.') != wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s;
}

static void readExif(IWICBitmapFrameDecode* frame, ExifInfo& e) {
    ComPtr<IWICMetadataQueryReader> r;
    if (FAILED(frame->GetMetadataQueryReader(&r)) || !r) return;

    wstring make = pvString(r.Get(), L"/app1/ifd/{ushort=271}");
    wstring model = pvString(r.Get(), L"/app1/ifd/{ushort=272}");
    if (model.empty()) model = pvString(r.Get(), L"/ifd/{ushort=272}");
    if (!model.empty()) {
        // "NIKON CORPORATION" + "NIKON D750" -> "NIKON D750"
        if (!make.empty() && model.rfind(make, 0) != 0) {
            wstring m1 = make.substr(0, make.find(L' '));
            if (model.rfind(m1, 0) != 0) model = make + L" " + model;
        }
        e.camera = model; e.any = true;
    } else if (!make.empty()) { e.camera = make; e.any = true; }

    e.lens = pvString(r.Get(), L"/app1/ifd/exif/{ushort=42036}");
    if (!e.lens.empty()) e.any = true;

    double d = 0;
    if (pvRational(r.Get(), L"/app1/ifd/exif/{ushort=33434}", d) && d > 0) {
        if (d >= 1.0) e.exposure = fmtNum(d, 1) + L" s";
        else          e.exposure = L"1/" + fmtNum(1.0 / d, 0) + L" s";
        e.any = true;
    }
    if (pvRational(r.Get(), L"/app1/ifd/exif/{ushort=33437}", d) && d > 0) { e.aperture = L"f/" + fmtNum(d, 1); e.any = true; }
    if (pvRational(r.Get(), L"/app1/ifd/exif/{ushort=37386}", d) && d > 0) { e.focal = fmtNum(d, 0) + L" mm"; e.any = true; }
    unsigned u = 0;
    if (pvUInt(r.Get(), L"/app1/ifd/exif/{ushort=34855}", u) && u) { e.iso = L"ISO " + std::to_wstring(u); e.any = true; }

    e.taken = pvString(r.Get(), L"/app1/ifd/exif/{ushort=36867}");
    if (e.taken.empty()) e.taken = pvString(r.Get(), L"/app1/ifd/{ushort=306}");
    if (e.taken.size() >= 19) {
        // 2024:05:11 13:02:44 -> 2024-05-11 13:02
        e.taken[4] = L'-'; e.taken[7] = L'-';
        e.taken = e.taken.substr(0, 16);
        e.any = true;
    }
    e.software = pvString(r.Get(), L"/app1/ifd/{ushort=305}");
}

static WICBitmapTransformOptions orientationTransform(IWICBitmapFrameDecode* frame, const wstring& ext) {
    // Only formats where WIC is known *not* to apply orientation itself.
    if (ext != L".jpg" && ext != L".jpeg" && ext != L".jpe" && ext != L".jfif" &&
        ext != L".tif" && ext != L".tiff")
        return WICBitmapTransformRotate0;
    ComPtr<IWICMetadataQueryReader> r;
    if (FAILED(frame->GetMetadataQueryReader(&r)) || !r) return WICBitmapTransformRotate0;
    unsigned o = 1;
    if (!pvUInt(r.Get(), L"/app1/ifd/{ushort=274}", o)) {
        if (!pvUInt(r.Get(), L"/ifd/{ushort=274}", o)) return WICBitmapTransformRotate0;
    }
    switch (o) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

// ------------------------------------------------------------------ WIC -> PixelBuf
static bool copyToBuf(IWICBitmapSource* src, PixelBuf& out) {
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return false;
    if ((uint64_t)w * h > 512ull * 1024 * 1024) return false;   // sanity cap

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return false;

    out.w = (int)w; out.h = (int)h;
    out.px.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
}

// ------------------------------------------------------------------ fallbacks
static bool decodeQOI(const uint8_t* d, size_t n, PixelBuf& out, bool& hasAlpha) {
    if (n < 22 || memcmp(d, "qoif", 4) != 0) return false;
    auto be32 = [&](size_t o) { return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3]; };
    uint32_t w = be32(4), h = be32(8);
    uint8_t ch = d[12];
    if (!w || !h || (uint64_t)w * h > 256ull * 1024 * 1024 || (ch != 3 && ch != 4)) return false;
    hasAlpha = (ch == 4);

    out.w = (int)w; out.h = (int)h;
    out.px.resize((size_t)w * h * 4);
    uint8_t idx[64][4] = {};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t p = 14, np = (size_t)w * h;
    int run = 0;
    for (size_t i = 0; i < np; ++i) {
        if (run > 0) { --run; }
        else if (p < n) {
            uint8_t op = d[p++];
            if (op == 0xFE && p + 2 < n) { r = d[p]; g = d[p + 1]; b = d[p + 2]; p += 3; }
            else if (op == 0xFF && p + 3 < n) { r = d[p]; g = d[p + 1]; b = d[p + 2]; a = d[p + 3]; p += 4; }
            else if ((op & 0xC0) == 0x00) { uint8_t k = op & 0x3F; r = idx[k][0]; g = idx[k][1]; b = idx[k][2]; a = idx[k][3]; }
            else if ((op & 0xC0) == 0x40) { r += ((op >> 4) & 3) - 2; g += ((op >> 2) & 3) - 2; b += (op & 3) - 2; }
            else if ((op & 0xC0) == 0x80) {
                if (p >= n) break;
                uint8_t b2 = d[p++];
                int dg = (int)(op & 0x3F) - 32;
                r += (uint8_t)(dg - 8 + ((b2 >> 4) & 0x0F));
                g += (uint8_t)dg;
                b += (uint8_t)(dg - 8 + (b2 & 0x0F));
            } else { run = (op & 0x3F); }
            idx[(r * 3 + g * 5 + b * 7 + a * 11) % 64][0] = r;
            idx[(r * 3 + g * 5 + b * 7 + a * 11) % 64][1] = g;
            idx[(r * 3 + g * 5 + b * 7 + a * 11) % 64][2] = b;
            idx[(r * 3 + g * 5 + b * 7 + a * 11) % 64][3] = a;
        }
        uint8_t* o = &out.px[i * 4];
        if (a == 255) { o[0] = b; o[1] = g; o[2] = r; o[3] = 255; }
        else { o[0] = (uint8_t)((b * a + 127) / 255); o[1] = (uint8_t)((g * a + 127) / 255); o[2] = (uint8_t)((r * a + 127) / 255); o[3] = a; }
    }
    return true;
}

static bool decodeFallback(const std::vector<uint8_t>& data, PixelBuf& out, bool& hasAlpha) {
    if (data.empty()) return false;
    if (decodeQOI(data.data(), data.size(), out, hasAlpha)) return true;

    int w = 0, h = 0, comp = 0;
    stbi_uc* p = stbi_load_from_memory(data.data(), (int)std::min<size_t>(data.size(), INT_MAX), &w, &h, &comp, 4);
    if (!p) return false;
    hasAlpha = (comp == 2 || comp == 4);
    out.w = w; out.h = h;
    out.px.resize((size_t)w * h * 4);
    premulRGBAtoBGRA(p, out.px.data(), (size_t)w * h);
    stbi_image_free(p);
    return true;
}

// ------------------------------------------------------------------ shell thumbnails
static bool shellThumb(const wstring& path, int size, bool cachedOnly, PixelBuf& out, bool& hasAlpha) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f) return false;

    SIZE sz{ size, size };
    DWORD flags = SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK;
    if (cachedOnly) flags |= SIIGBF_INCACHEONLY;
    HBITMAP hbm = nullptr;
    if (FAILED(f->GetImage(sz, flags, &hbm)) || !hbm) return false;

    BITMAP bm{};
    if (!GetObject(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) { DeleteObject(hbm); return false; }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.w = bm.bmWidth; out.h = bm.bmHeight;
    out.px.resize((size_t)out.w * out.h * 4);
    HDC dc = GetDC(nullptr);
    int rows = GetDIBits(dc, hbm, 0, bm.bmHeight, out.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hbm);
    if (rows != bm.bmHeight) { out.clear(); return false; }

    // Some providers hand back zeroed alpha; treat a fully transparent image as opaque.
    bool anyAlpha = false;
    for (size_t i = 3; i < out.px.size(); i += 4) if (out.px[i]) { anyAlpha = true; break; }
    hasAlpha = anyAlpha;
    if (!anyAlpha) for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    else {
        // GetDIBits gives straight alpha; Direct2D wants premultiplied.
        for (size_t i = 0; i < out.px.size(); i += 4) {
            uint32_t a = out.px[i + 3];
            if (a != 255) {
                out.px[i + 0] = (uint8_t)((out.px[i + 0] * a + 127) / 255);
                out.px[i + 1] = (uint8_t)((out.px[i + 1] * a + 127) / 255);
                out.px[i + 2] = (uint8_t)((out.px[i + 2] * a + 127) / 255);
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------ main decode
static bool wicDecode(const wstring& path, const std::vector<uint8_t>& data,
                      int targetW, int targetH, bool wantExif, DecodeResult& out) {
    if (!g_wic) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(g_wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), (DWORD)data.size()))) return false;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;

    UINT frames = 1;
    dec->GetFrameCount(&frames);
    out.frameCount = (int)std::max(1u, frames);

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;

    WICPixelFormatGUID pf{};
    if (SUCCEEDED(frame->GetPixelFormat(&pf))) {
        ComPtr<IWICComponentInfo> ci;
        ComPtr<IWICPixelFormatInfo2> pfi;
        BOOL transparent = FALSE;
        if (SUCCEEDED(g_wic->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci.As(&pfi)) &&
            SUCCEEDED(pfi->SupportsTransparency(&transparent)))
            out.hasAlpha = transparent != FALSE;
    }

    WICBitmapTransformOptions rot = orientationTransform(frame.Get(), extOf(path));
    bool swapped = (rot & WICBitmapTransformRotate90) || (rot & WICBitmapTransformRotate270);

    out.srcW = swapped ? (int)h : (int)w;
    out.srcH = swapped ? (int)w : (int)h;

    // Decide the decode resolution. Never upscale during decode.
    UINT dw = w, dh = h;
    if (targetW > 0 && targetH > 0) {
        int tw = swapped ? targetH : targetW;
        int th = swapped ? targetW : targetH;
        double k = std::min((double)tw / w, (double)th / h);
        if (k < 1.0) {
            dw = (UINT)std::max(1.0, std::floor(w * k + 0.5));
            dh = (UINT)std::max(1.0, std::floor(h * k + 0.5));
        }
    }
    out.isFullRes = (dw == w && dh == h);

    ComPtr<IWICBitmapSource> src;
    if (!out.isFullRes) {
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(g_wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), dw, dh, WICBitmapInterpolationModeFant)))
            src = scaler;
    }
    if (!src) { src = frame; out.isFullRes = true; }

    if (rot != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> fr;
        if (SUCCEEDED(g_wic->CreateBitmapFlipRotator(&fr)) &&
            SUCCEEDED(fr->Initialize(src.Get(), rot)))
            src = fr;
    }

    if (!copyToBuf(src.Get(), out.img)) return false;
    if (wantExif) readExif(frame.Get(), out.exif);
    return true;
}

// ------------------------------------------------------------------ entry points
void decodeInit() {
    CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));

    auto add = [&](wstring e) {
        e = lowerOf(e);
        if (e.empty()) return;
        if (e[0] != L'.') e = L"." + e;
        if (g_extSet.insert(e).second) g_exts.push_back(e);
    };

    // Baseline: always present on Windows.
    for (const wchar_t* e : { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif",
                              L".tif", L".tiff", L".ico", L".dib", L".wdp", L".jxr", L".hdp",
                              L".dds", L".heic", L".heif", L".avif", L".webp", L".jxl",
                              L".tga", L".psd", L".hdr", L".pic", L".pnm", L".ppm", L".pgm",
                              L".pbm", L".qoi", L".cur", L".apng" })
        add(e);

    // Whatever WIC codecs are actually installed (RAW packs, HEIF, AVIF, WebP...).
    if (g_wic) {
        ComPtr<IEnumUnknown> en;
        if (SUCCEEDED(g_wic->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en))) {
            ComPtr<IUnknown> unk;
            ULONG fetched = 0;
            while (en->Next(1, &unk, &fetched) == S_OK && fetched) {
                ComPtr<IWICBitmapCodecInfo> info;
                if (SUCCEEDED(unk.As(&info))) {
                    UINT need = 0;
                    info->GetFileExtensions(0, nullptr, &need);
                    if (need > 1 && need < 4096) {
                        std::vector<wchar_t> buf(need + 1, 0);
                        if (SUCCEEDED(info->GetFileExtensions(need, buf.data(), &need))) {
                            wstring all = buf.data(), cur;
                            for (wchar_t c : all) {
                                if (c == L',' || c == L';' || c == L' ') { if (!cur.empty()) add(cur); cur.clear(); }
                                else cur += c;
                            }
                            if (!cur.empty()) add(cur);
                        }
                    }
                }
                unk.Reset();
            }
        }
    }
    std::sort(g_exts.begin(), g_exts.end());
}

bool decodeIsSupported(const wstring& ext) {
    return !ext.empty() && g_extSet.count(lowerOf(ext)) > 0;
}

const std::vector<wstring>& decodeExtensions() { return g_exts; }

wstring decodeFilterString() {
    wstring all;
    for (const auto& e : g_exts) { if (!all.empty()) all += L";"; all += L"*" + e; }
    wstring f = T(L"Зображення|") + all + T(L"|Усі файли|*.*|");
    for (auto& c : f) if (c == L'|') c = L'\0';
    return f;
}

void runDecodeJob(const DecodeJob& job, DecodeResult& out, const std::atomic<uint64_t>& liveGen) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);

    out.path = job.path;
    out.kind = job.kind;
    out.generation = job.generation;
    out.ok = false;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(job.path.c_str(), GetFileExInfoStandard, &fad)) {
        out.fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        out.mtime = fad.ftLastWriteTime;
    }

    if (job.kind == JobKind::Preview) {
        // Cheapest possible glimpse: container thumbnail, no full read.
        if (g_wic) {
            ComPtr<IWICBitmapDecoder> dec;
            if (SUCCEEDED(g_wic->CreateDecoderFromFilename(job.path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &dec))) {
                ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(dec->GetFrame(0, &frame))) {
                    UINT w = 0, h = 0;
                    if (SUCCEEDED(frame->GetSize(&w, &h))) { out.srcW = (int)w; out.srcH = (int)h; }
                    ComPtr<IWICBitmapSource> th;
                    if (SUCCEEDED(frame->GetThumbnail(&th)) && th) {
                        WICBitmapTransformOptions rot = orientationTransform(frame.Get(), extOf(job.path));
                        if (rot != WICBitmapTransformRotate0) {
                            ComPtr<IWICBitmapFlipRotator> fr;
                            if (SUCCEEDED(g_wic->CreateBitmapFlipRotator(&fr)) && SUCCEEDED(fr->Initialize(th.Get(), rot)))
                                th = fr;
                            if ((rot & WICBitmapTransformRotate90) || (rot & WICBitmapTransformRotate270))
                                std::swap(out.srcW, out.srcH);
                        }
                        if (copyToBuf(th.Get(), out.img)) out.ok = true;
                    }
                }
            }
        }
        if (!out.ok && shellThumb(job.path, 256, true, out.img, out.hasAlpha)) out.ok = true;
        if (!out.ok) out.error = L"no-preview";
    } else if (job.kind == JobKind::Thumb) {
        int want = job.targetW > 0 ? job.targetW : 256;
        // The Explorer cache already knows nearly every format through shell
        // extensions, and it answers in microseconds when warm.
        if (shellThumb(job.path, want, false, out.img, out.hasAlpha)) out.ok = true;
        if (!out.ok) {
            std::vector<uint8_t> data;
            uint64_t sz = 0; FILETIME mt{};
            if (readWholeFile(job.path, data, sz, mt)) {
                DecodeResult tmp;
                tmp.path = job.path;
                if (wicDecode(job.path, data, want, want, false, tmp) && tmp.img.valid()) {
                    out.img = std::move(tmp.img); out.srcW = tmp.srcW; out.srcH = tmp.srcH; out.ok = true;
                } else if (decodeFallback(data, out.img, out.hasAlpha)) {
                    out.srcW = out.img.w; out.srcH = out.img.h; out.ok = true;
                }
            }
        }
        if (!out.ok) out.error = L"no-thumb";
    } else {
        std::vector<uint8_t> data;
        uint64_t sz = 0; FILETIME mt{};
        if (!readWholeFile(job.path, data, sz, mt)) {
            out.error = T(L"Не вдалося прочитати файл");
        } else if (liveGen.load(std::memory_order_acquire) != job.generation) {
            out.error = L"cancelled";
        } else {
            out.fileSize = sz; out.mtime = mt;
            if (wicDecode(job.path, data, job.targetW, job.targetH, job.wantExif, out) && out.img.valid()) {
                out.ok = true;
            } else if (decodeFallback(data, out.img, out.hasAlpha)) {
                out.srcW = out.img.w; out.srcH = out.img.h; out.isFullRes = true; out.ok = true;
            } else {
                out.error = T(L"Формат не підтримується або файл пошкоджено");
            }
        }
    }

    QueryPerformanceCounter(&t1);
    out.decodeMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}
