// Writing images back out: format conversion and compression.
//
// Everything here runs on one background thread. Reaching a requested file size
// means encoding the picture several times and measuring, so the work is far too
// slow for the UI thread - and the user is usually dragging a slider, which is
// why preview requests are coalesced down to the newest one.
#include "pg.h"

IWICImagingFactory2* pgWic();          // the factory decode.cpp already created

// ------------------------------------------------------------------ formats
static std::vector<EncFormat> g_encFormats;
static std::once_flag         g_encOnce;

// Common first, the way the file-type panel is ordered. Anything not listed is
// dropped: .dds is a GPU texture container and has no business in a photo
// compressor, and codecs that enumerate but fail to create are useless.
static int formatRank(const wstring& ext) {
    static const wchar_t* order[] = { L".jpg", L".png", L".heic", L".tif", L".jxr", L".bmp", L".gif" };
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); ++i)
        if (ext == order[i]) return i;
    return -1;
}

static wstring prettyName(const wstring& ext) {
    if (ext == L".jpg")  return L"JPEG";
    if (ext == L".png")  return L"PNG";
    if (ext == L".heic") return L"HEIC";
    if (ext == L".tif")  return L"TIFF";
    if (ext == L".jxr")  return L"JPEG XR";
    if (ext == L".bmp")  return L"BMP";
    if (ext == L".gif")  return L"GIF";
    return ext.substr(1);
}

// The extension we want to offer for a codec, out of everything it claims.
static wstring preferredExt(const wstring& list) {
    std::vector<wstring> exts;
    wstring cur;
    for (wchar_t c : list + L",") {
        if (c == L',' || c == L';' || c == L' ') { if (!cur.empty()) exts.push_back(lowerOf(cur)); cur.clear(); }
        else cur += c;
    }
    int best = 99;
    wstring bestExt;
    for (auto& e : exts) {
        int r = formatRank(e);
        if (r >= 0 && r < best) { best = r; bestExt = e; }
    }
    return bestExt;
}

static void probeFormats() {
    IWICImagingFactory2* wic = pgWic();
    if (!wic) return;

    ComPtr<IEnumUnknown> en;
    if (FAILED(wic->CreateComponentEnumerator(WICEncoder, WICComponentEnumerateDefault, &en))) return;

    ComPtr<IUnknown> unk;
    ULONG fetched = 0;
    while (en->Next(1, &unk, &fetched) == S_OK && fetched) {
        ComPtr<IWICBitmapCodecInfo> info;
        if (SUCCEEDED(unk.As(&info))) {
            UINT need = 0;
            info->GetFileExtensions(0, nullptr, &need);
            wstring exts;
            if (need > 1 && need < 4096) {
                std::vector<wchar_t> buf(need + 1, 0);
                if (SUCCEEDED(info->GetFileExtensions(need, buf.data(), &need))) exts = buf.data();
            }
            wstring ext = preferredExt(exts);
            GUID container{};
            if (!ext.empty() && SUCCEEDED(info->GetContainerFormat(&container))) {
                // Only keep it if a frame can really be created: HEIF enumerates
                // without its extension installed, and JPEG XL fails outright.
                EncFormat f;
                f.ext = ext;
                f.name = prettyName(ext);
                f.container = container;
                f.alpha = (ext == L".png" || ext == L".tif" || ext == L".heic" ||
                           ext == L".jxr" || ext == L".gif");

                ComPtr<IWICBitmapEncoder> enc;
                ComPtr<IStream> mem;
                if (SUCCEEDED(wic->CreateEncoder(container, nullptr, &enc)) &&
                    SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) &&
                    SUCCEEDED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache))) {
                    ComPtr<IWICBitmapFrameEncode> frame;
                    ComPtr<IPropertyBag2> bag;
                    if (SUCCEEDED(enc->CreateNewFrame(&frame, &bag)) && bag) {
                        PROPBAG2 p{};
                        p.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                        VARIANT v{};
                        HRESULT err = S_OK;
                        f.quality = SUCCEEDED(bag->Read(1, &p, nullptr, &v, &err));
                        VariantClear(&v);
                        g_encFormats.push_back(std::move(f));
                    }
                }
            }
        }
        unk.Reset();
    }

    std::sort(g_encFormats.begin(), g_encFormats.end(),
              [](const EncFormat& a, const EncFormat& b) {
                  return formatRank(a.ext) < formatRank(b.ext);
              });
}

const std::vector<EncFormat>& encodeFormats() {
    std::call_once(g_encOnce, probeFormats);
    return g_encFormats;
}

int encodeFormatFor(const wstring& ext) {
    const auto& fs = encodeFormats();
    wstring e = lowerOf(ext);
    if (e == L".jpeg" || e == L".jfif" || e == L".jpe") e = L".jpg";
    if (e == L".tiff") e = L".tif";
    if (e == L".heif" || e == L".hif") e = L".heic";
    if (e == L".wdp" || e == L".hdp") e = L".jxr";
    for (size_t i = 0; i < fs.size(); ++i) if (fs[i].ext == e) return (int)i;
    return -1;
}

// ------------------------------------------------------------------ pixels
// PixelBuf is premultiplied BGRA. A format without an alpha channel would
// otherwise get dark fringes wherever the picture was transparent, so flatten
// it onto white first - which for premultiplied data is just c + (255 - a).
static void flattenOntoWhite(PixelBuf& buf) {
    uint8_t* p = buf.px.data();
    size_t n = (size_t)buf.w * buf.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        int inv = 255 - p[3];
        if (inv) {
            p[0] = (uint8_t)std::min(255, p[0] + inv);
            p[1] = (uint8_t)std::min(255, p[1] + inv);
            p[2] = (uint8_t)std::min(255, p[2] + inv);
        }
        p[3] = 255;
    }
}

static bool scalePixels(const PixelBuf& src, int w, int h, PixelBuf& out) {
    IWICImagingFactory2* wic = pgWic();
    if (!wic || !src.valid() || w < 1 || h < 1) return false;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory((UINT)src.w, (UINT)src.h, GUID_WICPixelFormat32bppPBGRA,
                                           (UINT)src.stride(), (UINT)src.px.size(),
                                           const_cast<BYTE*>(src.px.data()), &bmp)))
        return false;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bmp.Get(), (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)))
        return false;
    out.w = w; out.h = h;
    out.px.resize((size_t)w * h * 4);
    return SUCCEEDED(scaler->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
}

// True only if some pixel is actually see-through. A format that *can* store
// alpha still should not, when the picture has none: a PNG of an opaque photo
// would otherwise spend a quarter of its bytes on a channel of 255s.
static bool hasRealAlpha(const PixelBuf& buf) {
    const uint8_t* p = buf.px.data();
    size_t n = (size_t)buf.w * buf.h;
    for (size_t i = 0; i < n; ++i, p += 4) if (p[3] != 255) return true;
    return false;
}

static bool encodeToBytes(const PixelBuf& src, const EncFormat& f, bool withAlpha, float quality,
                          int outW, int outH, std::vector<uint8_t>& out) {
    IWICImagingFactory2* wic = pgWic();
    if (!wic || !src.valid() || outW < 1 || outH < 1) return false;

    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory((UINT)src.w, (UINT)src.h, GUID_WICPixelFormat32bppPBGRA,
                                           (UINT)src.stride(), (UINT)src.px.size(),
                                           const_cast<BYTE*>(src.px.data()), &bmp)))
        return false;

    ComPtr<IWICBitmapSource> source = bmp;
    if (outW != src.w || outH != src.h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source.Get(), (UINT)outW, (UINT)outH,
                                      WICBitmapInterpolationModeFant)))
            return false;
        source = scaler;
    }

    ComPtr<IStream> mem;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) return false;

    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(f.container, nullptr, &enc)) ||
        FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache)))
        return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(enc->CreateNewFrame(&frame, &bag))) return false;

    if (bag) {
        if (f.quality) {
            PROPBAG2 p{};
            p.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v{};
            v.vt = VT_R4;
            v.fltVal = clampf(quality, 0.02f, 1.f);
            bag->Write(1, &p, &v);
        } else if (f.ext == L".tif") {
            // Lossless, but ZIP is a great deal smaller than the default.
            PROPBAG2 p{};
            p.pstrName = const_cast<LPOLESTR>(L"TiffCompressionMethod");
            VARIANT v{};
            v.vt = VT_UI1;
            v.bVal = WICTiffCompressionZIP;
            bag->Write(1, &p, &v);
        }
    }
    if (FAILED(frame->Initialize(bag.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)outW, (UINT)outH))) return false;
    frame->SetResolution(96.0, 96.0);

    // Ask for what we want, then convert to whatever the encoder agreed to.
    WICPixelFormatGUID actual = withAlpha ? GUID_WICPixelFormat32bppBGRA
                                          : GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&actual);

    ComPtr<IWICPalette> palette;
    bool indexed = (actual == GUID_WICPixelFormat8bppIndexed ||
                    actual == GUID_WICPixelFormat4bppIndexed ||
                    actual == GUID_WICPixelFormat2bppIndexed ||
                    actual == GUID_WICPixelFormat1bppIndexed);
    if (indexed) {
        UINT colors = (actual == GUID_WICPixelFormat8bppIndexed) ? 256
                    : (actual == GUID_WICPixelFormat4bppIndexed) ? 16
                    : (actual == GUID_WICPixelFormat2bppIndexed) ? 4 : 2;
        if (FAILED(wic->CreatePalette(&palette)) ||
            FAILED(palette->InitializeFromBitmap(source.Get(), colors, withAlpha)))
            palette.Reset();
        if (palette) frame->SetPalette(palette.Get());
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(source.Get(), actual, WICBitmapDitherTypeErrorDiffusion,
                                palette.Get(), 0.0,
                                palette ? WICBitmapPaletteTypeCustom : WICBitmapPaletteTypeMedianCut)))
        return false;

    if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

    // Pull the bytes back out of the memory stream.
    HGLOBAL h = nullptr;
    if (FAILED(GetHGlobalFromStream(mem.Get(), &h)) || !h) return false;
    STATSTG st{};
    if (FAILED(mem->Stat(&st, STATFLAG_NONAME))) return false;
    size_t n = (size_t)st.cbSize.QuadPart;
    void* base = GlobalLock(h);
    if (!base) return false;
    out.assign((const uint8_t*)base, (const uint8_t*)base + n);
    GlobalUnlock(h);
    return !out.empty();
}

// Decode the bytes we just produced, so the preview shows the real artefacts
// rather than the untouched original.
static bool decodeBytes(const std::vector<uint8_t>& bytes, int maxEdge, PixelBuf& out) {
    IWICImagingFactory2* wic = pgWic();
    if (!wic || bytes.empty()) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())))
        return false;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;

    ComPtr<IWICBitmapSource> src = frame;
    if (maxEdge > 0 && (int)std::max(w, h) > maxEdge) {
        double k = (double)maxEdge / std::max(w, h);
        UINT dw = (UINT)std::max(1.0, w * k), dh = (UINT)std::max(1.0, h * k);
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), dw, dh, WICBitmapInterpolationModeFant))) {
            src = scaler;
            w = dw; h = dh;
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return false;

    out.w = (int)w; out.h = (int)h;
    out.px.resize((size_t)w * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
}

// ------------------------------------------------------------------ worker
struct Compressor::Impl {
    std::thread             th;
    std::mutex              m;
    std::condition_variable cv;
    bool                    quit = false;
    bool                    havePreview = false;
    CompressJob             preview;
    std::deque<CompressJob> saves;
    std::atomic<bool>       working{ false };
    std::atomic<bool>       cancel{ false };
    std::atomic<bool>       runningPreview{ false };

    std::mutex                 om;
    std::vector<CompressResult> out;
    HWND                        notify = nullptr;
};

Compressor::~Compressor() { stop(); }

static uint64_t fileSizeOf(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static bool writeAll(const wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Decode the source at full resolution through the normal path, so every format
// the viewer opens can also be converted - including the ones that go through
// the non-WIC fallbacks.
static bool loadSource(const wstring& path, PixelBuf& px, int& srcW, int& srcH, wstring& err) {
    static const std::atomic<uint64_t> live{ 1 };
    DecodeJob job;
    job.path = path;
    job.kind = JobKind::Full;
    job.generation = 1;
    DecodeResult r;
    runDecodeJob(job, r, live);
    if (!r.ok || !r.img.valid()) { err = r.error.empty() ? T(L"Не вдалося прочитати файл") : r.error; return false; }
    px = std::move(r.img);
    srcW = r.srcW > 0 ? r.srcW : px.w;
    srcH = r.srcH > 0 ? r.srcH : px.h;
    return true;
}

static void runJob(Compressor::Impl* p, const CompressJob& job, CompressResult& res) {
    const bool isPreview = job.outPath.empty();
    auto aborted = [&] { return isPreview && p->cancel.load(); };
    double t0 = nowSec();
    res.id = job.id;
    res.path = job.path;
    res.outPath = job.outPath;
    res.batchIndex = job.batchIndex;
    res.batchTotal = job.batchTotal;
    res.srcBytes = fileSizeOf(job.path);

    const auto& formats = encodeFormats();
    if (formats.empty()) { res.error = T(L"Немає доступних кодувальників"); return; }
    const EncFormat& f = formats[clampi(job.format, 0, (int)formats.size() - 1)];

    PixelBuf src;
    int nativeW = 0, nativeH = 0;
    if (!loadSource(job.path, src, nativeW, nativeH, res.error)) return;
    res.srcW = src.w;
    res.srcH = src.h;
    const bool useAlpha = f.alpha && hasRealAlpha(src);
    if (!useAlpha) flattenOntoWhite(src);

    auto sized = [&](float scale, int& w, int& h) {
        w = std::max(1, (int)lround(src.w * (double)clampf(scale, 0.02f, 1.f)));
        h = std::max(1, (int)lround(src.h * (double)clampf(scale, 0.02f, 1.f)));
    };

    std::vector<uint8_t> bytes;
    float useQ = clampf(job.quality, 0.02f, 1.f);
    float useScale = clampf(job.scale, 0.02f, 1.f);
    int w = 0, h = 0;
    sized(useScale, w, h);

    if (job.mode == CompressMode::Quality) {
        if (!encodeToBytes(src, f, useAlpha, useQ, w, h, bytes)) { res.error = T(L"Не вдалося закодувати"); return; }
    } else {
        double target = (job.mode == CompressMode::Percent)
                            ? res.srcBytes * clampf((float)job.target, 0.01f, 1.f)
                            : job.target;
        if (target < 1024) target = 1024;

        // Quality first: it keeps every pixel.
        //
        // Searching at the real resolution would mean up to a dozen encodes of a
        // 24 MP picture - seconds of waiting while the panel shows stale numbers.
        // JPEG size tracks pixel count closely enough that the bracket can be
        // found on a small proxy in a few milliseconds per step, and only the
        // last couple of probes have to run at full size to be exact.
        PixelBuf proxy;
        double proxyPx = 0;
        bool hopeless = false;
        const double fullPx = (double)w * h;
        if (fullPx > 1.6e6) {
            double k = sqrt(1.6e6 / fullPx);
            int pw = std::max(16, (int)lround(w * k)), ph = std::max(16, (int)lround(h * k));
            if (scalePixels(src, pw, ph, proxy)) proxyPx = (double)pw * ph;
        }

        if (f.quality) {
            float lo = 0.03f, hi = 0.98f;
            if (proxy.valid() && !aborted()) {
                double pTarget = target * (proxyPx / fullPx);
                float plo = lo, phi = hi, pbest = -1.f;
                for (int i = 0; i < 9 && !aborted(); ++i) {
                    float mid = (plo + phi) * .5f;
                    std::vector<uint8_t> trial;
                    if (!encodeToBytes(proxy, f, useAlpha, mid, proxy.w, proxy.h, trial)) break;
                    if ((double)trial.size() <= pTarget) { pbest = mid; plo = mid; }
                    else phi = mid;
                    if (phi - plo < 0.01f) break;
                }
                if (pbest > 0.f) {
                    lo = clampf(pbest - 0.14f, 0.03f, 0.97f);
                    hi = clampf(pbest + 0.14f, lo + 0.02f, 0.98f);
                } else {
                    // Even the floor overshot: no quality setting will reach the
                    // target, so do not spend half a dozen full-size encodes
                    // proving it - go straight to shrinking.
                    hopeless = true;
                }
            }

            std::vector<uint8_t> best;
            float bestQ = lo;
            if (!hopeless) {
                for (int i = 0; i < 5 && !aborted(); ++i) {
                    float mid = (lo + hi) * .5f;
                    std::vector<uint8_t> trial;
                    if (!encodeToBytes(src, f, useAlpha, mid, w, h, trial)) break;
                    if ((double)trial.size() <= target) { best.swap(trial); bestQ = mid; lo = mid; }
                    else hi = mid;
                    if (hi - lo < 0.015f) break;
                }
                // The bracket from the proxy can sit entirely above the target;
                // fall back to the floor rather than reporting a miss that is
                // not real.
                if (best.empty() && lo > 0.04f && !aborted()) {
                    std::vector<uint8_t> trial;
                    if (encodeToBytes(src, f, useAlpha, 0.03f, w, h, trial) && (double)trial.size() <= target) {
                        best.swap(trial);
                        bestQ = 0.03f;
                    }
                }
            }
            if (!best.empty()) { bytes.swap(best); useQ = bestQ; }
        }

        if (bytes.empty() || bytes.size() > target) {
            if (job.allowDownscale) {
                float q = f.quality ? 0.72f : 1.f;
                float slo = 0.05f, shi = useScale;

                // At a fixed quality the file grows roughly with the pixel count,
                // so one cheap encode of the proxy says about where to look, and
                // the bracket can start narrow instead of at the whole range.
                if (proxy.valid() && !aborted()) {
                    std::vector<uint8_t> t;
                    if (encodeToBytes(proxy, f, useAlpha, q, proxy.w, proxy.h, t) && !t.empty()) {
                        double perPx = (double)t.size() / proxyPx;
                        double wantPx = target / std::max(1e-9, perPx);
                        float seed = (float)sqrt(clampf((float)(wantPx / fullPx), 0.0025f, 1.f));
                        slo = clampf(seed * 0.55f, 0.05f, useScale);
                        shi = clampf(seed * 1.6f, slo + 0.02f, useScale);
                    }
                }

                std::vector<uint8_t> best;
                float bestS = slo;
                for (int pass = 0; pass < 2 && best.empty() && !aborted(); ++pass) {
                    float a0 = slo, b0 = shi;
                    for (int i = 0; i < 5 && !aborted(); ++i) {
                        float mid = (a0 + b0) * .5f;
                        int tw, th;
                        sized(mid, tw, th);
                        std::vector<uint8_t> trial;
                        if (!encodeToBytes(src, f, useAlpha, q, tw, th, trial)) break;
                        if (trial.size() <= target) { best.swap(trial); bestS = mid; a0 = mid; }
                        else b0 = mid;
                        if (b0 - a0 < 0.01f) break;
                    }
                    // The seeded bracket was too optimistic: look below it.
                    shi = slo;
                    slo = 0.05f;
                    if (shi <= 0.06f) break;
                }
                if (!best.empty()) {
                    bytes.swap(best);
                    useScale = bestS;
                    useQ = q;
                    sized(useScale, w, h);
                }
            }
            if (bytes.empty()) {
                // Nothing fit: show the smallest we could make and say so.
                float q = f.quality ? 0.03f : 1.f;
                if (!encodeToBytes(src, f, useAlpha, q, w, h, bytes)) { res.error = T(L"Не вдалося закодувати"); return; }
                useQ = q;
            }
            res.missedTarget = bytes.size() > target;
        }
    }

    res.outBytes = bytes.size();
    res.outW = w;
    res.outH = h;
    res.usedQuality = useQ;
    res.usedScale = useScale;

    if (!job.outPath.empty()) {
        res.saved = writeAll(job.outPath, bytes);
        if (!res.saved) res.error = T(L"Не вдалося записати файл");
    }
    if (job.previewMax > 0 && !aborted())
        decodeBytes(bytes, job.previewMax, res.preview);

    res.ok = res.error.empty();
    res.ms = (nowSec() - t0) * 1000.0;
}

void Compressor::start(HWND notify) {
    if (p_) return;
    auto* p = new Impl();
    p->notify = notify;
    p_ = p;

    p->th = std::thread([p] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            CompressJob job;
            {
                std::unique_lock<std::mutex> lk(p->m);
                p->cv.wait(lk, [p] { return p->quit || p->havePreview || !p->saves.empty(); });
                if (p->quit) break;
                // Saving is what the user is waiting on; previews are just the
                // slider catching up.
                if (!p->saves.empty()) { job = std::move(p->saves.front()); p->saves.pop_front(); }
                else { job = p->preview; p->havePreview = false; }
            }
            p->working.store(true);
            p->runningPreview.store(job.outPath.empty());
            p->cancel.store(false);

            CompressResult res;
            runJob(p, job, res);

            {
                std::lock_guard<std::mutex> lk(p->om);
                p->out.push_back(std::move(res));
            }
            p->working.store(false);
            if (p->notify) PostMessageW(p->notify, WM_PG_COMPRESS, 0, 0);
        }
        CoUninitialize();
    });
}

void Compressor::stop() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->quit = true;
    }
    p_->cancel.store(true);
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    delete p_;
    p_ = nullptr;
}

void Compressor::request(CompressJob job) {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->preview = std::move(job);
        p_->havePreview = true;
    }
    // Let a preview that is already running give up early; a save is never
    // interrupted, because its output has to be exactly what was asked for.
    if (p_->runningPreview.load()) p_->cancel.store(true);
    p_->cv.notify_one();
}

void Compressor::save(CompressJob job) {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->saves.push_back(std::move(job));
    }
    p_->cv.notify_one();
}

void Compressor::cancelBatch() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->saves.clear();
    }
    p_->cancel.store(true);
}

bool Compressor::pop(CompressResult& out) {
    if (!p_) return false;
    std::lock_guard<std::mutex> lk(p_->om);
    if (p_->out.empty()) return false;
    out = std::move(p_->out.front());
    p_->out.erase(p_->out.begin());
    return true;
}

bool Compressor::busy() const {
    if (!p_) return false;
    if (p_->working.load()) return true;
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->havePreview || !p_->saves.empty();
}
