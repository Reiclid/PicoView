// Video playback through the Media Foundation Media Engine.
//
// The Media Engine handles demuxing, decoding, audio and clocking for every
// codec the system has, and hands us frames straight into a D3D11 texture that
// Direct2D can draw - so video shares the same GPU path as the images and needs
// no extra libraries.
#include "pg.h"

#include <mfapi.h>
#include <mfmediaengine.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d10.h>          // ID3D10Multithread
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

// ------------------------------------------------------------------ notify
namespace {

class EngineNotify : public IMFMediaEngineNotify {
public:
    explicit EngineNotify(HWND hwnd) : hwnd_(hwnd) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // Called on a Media Foundation thread - just wake the UI thread.
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        (void)p2;
        HWND h = hwnd_;
        if (h) PostMessageW(h, WM_PG_VIDEO, (WPARAM)ev, (LPARAM)p1);
        return S_OK;
    }

    void detach() { hwnd_ = nullptr; }

private:
    ~EngineNotify() = default;
    LONG ref_ = 1;
    volatile HWND hwnd_ = nullptr;
};

} // namespace

// ------------------------------------------------------------------ impl
struct VideoPlayer::Impl {
    ComPtr<IMFMediaEngine>       engine;
    ComPtr<IMFMediaEngineEx>     engineEx;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr;
    ComPtr<ID3D11Device>         d3d;
    ComPtr<ID3D11Texture2D>      tex;
    ComPtr<ID2D1Bitmap1>         bmp;
    EngineNotify*                notify = nullptr;

    HWND    hwnd = nullptr;
    UINT    resetToken = 0;
    bool    mfStarted = false;
    bool    opened = false;
    bool    metaKnown = false;
    bool    autoPlayPending = false;
    float   wantVolume = 1.f;
    bool    wantMuted = false;
    bool    quiet = false;
    int     w = 0, h = 0;
    double  lastPts = -1.0;
    wstring path;
    wstring error;
};

VideoPlayer::~VideoPlayer() { shutdown(); }

bool VideoPlayer::init(ID3D11Device* dev, HWND notifyHwnd) {
    if (p_) return true;
    if (!dev) return false;

    auto* p = new Impl();
    p->d3d = dev;
    p->hwnd = notifyHwnd;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { delete p; return false; }
    p->mfStarted = true;

    // The engine decodes on its own threads against our device.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(MFCreateDXGIDeviceManager(&p->resetToken, &p->dxgiMgr)) ||
        FAILED(p->dxgiMgr->ResetDevice(dev, p->resetToken))) {
        MFShutdown();
        delete p;
        return false;
    }

    p_ = p;
    return true;
}

void VideoPlayer::shutdown() {
    if (!p_) return;
    close();
    if (p_->notify) { p_->notify->detach(); p_->notify->Release(); p_->notify = nullptr; }
    p_->dxgiMgr.Reset();
    p_->d3d.Reset();
    if (p_->mfStarted) MFShutdown();
    delete p_;
    p_ = nullptr;
}

void VideoPlayer::close() {
    if (!p_) return;
    if (p_->engine) {
        p_->engine->Pause();
        p_->engine->Shutdown();
    }
    p_->engineEx.Reset();
    p_->engine.Reset();
    p_->bmp.Reset();
    p_->tex.Reset();
    if (p_->notify) { p_->notify->detach(); p_->notify->Release(); p_->notify = nullptr; }
    p_->opened = false;
    p_->metaKnown = false;
    // wantVolume / wantMuted deliberately survive close(), so the next file
    // starts at the same level.
    p_->autoPlayPending = false;
    p_->w = p_->h = 0;
    p_->lastPts = -1.0;
    p_->path.clear();
    p_->error.clear();
}

bool VideoPlayer::open(const wstring& path, bool autoPlay) {
    if (!p_) return false;
    close();

    p_->notify = new EngineNotify(p_->hwnd);

    ComPtr<IMFMediaEngineClassFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        p_->error = T(L"Media Foundation недоступна");
        return false;
    }

    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 4))) { p_->error = L"MFCreateAttributes"; return false; }
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, static_cast<IMFMediaEngineNotify*>(p_->notify));
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, p_->dxgiMgr.Get());
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    if (FAILED(factory->CreateInstance(0, attr.Get(), &p_->engine))) {
        p_->error = T(L"Не вдалося створити медіарушій");
        return false;
    }
    p_->engine.As(&p_->engineEx);

    BSTR url = SysAllocString(path.c_str());
    HRESULT hr = p_->engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) { p_->error = T(L"Не вдалося відкрити відео"); return false; }

    p_->path = path;
    p_->opened = true;
    p_->autoPlayPending = autoPlay;
    p_->quiet = true;                 // silence the first sample after loading
    p_->engine->SetVolume(0.0);
    return true;
}

// Called from the UI thread for every event the engine posts.
unsigned VideoPlayer::onEvent(unsigned ev, uintptr_t param) {
    if (!p_ || !p_->engine) return 0;
    switch (ev) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
            DWORD w = 0, h = 0;
            if (SUCCEEDED(p_->engine->GetNativeVideoSize(&w, &h)) && w && h) {
                p_->w = (int)w; p_->h = (int)h;
                p_->metaKnown = true;
                p_->tex.Reset();
                p_->bmp.Reset();
                return PGV_REPAINT | PGV_SIZED;
            }
            return PGV_REPAINT;
        }
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            // The engine only honours volume once a source is actually loaded.
            p_->engine->SetMuted(p_->wantMuted ? TRUE : FALSE);
            if (!p_->quiet) p_->engine->SetVolume(p_->wantVolume);
            if (p_->autoPlayPending) { p_->autoPlayPending = false; p_->engine->Play(); }
            return p_->quiet ? (PGV_REPAINT | PGV_QUIET) : PGV_REPAINT;
        case MF_MEDIA_ENGINE_EVENT_ERROR: {
            switch ((MF_MEDIA_ENGINE_ERR)param) {
                case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
                    p_->error = T(L"Формат або кодек не підтримується системою"); break;
                case MF_MEDIA_ENGINE_ERR_DECODE:
                    p_->error = T(L"Помилка декодування"); break;
                case MF_MEDIA_ENGINE_ERR_NETWORK:
                    p_->error = T(L"Помилка читання файлу"); break;
                case MF_MEDIA_ENGINE_ERR_ABORTED:
                    p_->error = T(L"Відтворення перервано"); break;
                default:
                    p_->error = T(L"Не вдалося відтворити"); break;
            }
            return PGV_REPAINT | PGV_ERROR;
        }
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            return PGV_REPAINT | PGV_ENDED;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            return p_->quiet ? (PGV_REPAINT | PGV_QUIET) : PGV_REPAINT;
        case MF_MEDIA_ENGINE_EVENT_PLAY:
        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE:
        case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            return PGV_REPAINT;
        default:
            return 0;
    }
}

bool VideoPlayer::isOpen() const { return p_ && p_->opened; }
bool VideoPlayer::hasFrame() const { return p_ && p_->bmp; }
bool VideoPlayer::metaKnown() const { return p_ && p_->metaKnown; }
int  VideoPlayer::width() const { return p_ ? p_->w : 0; }
int  VideoPlayer::height() const { return p_ ? p_->h : 0; }
wstring VideoPlayer::error() const { return p_ ? p_->error : wstring(); }
wstring VideoPlayer::path() const { return p_ ? p_->path : wstring(); }

bool VideoPlayer::playing() const {
    return p_ && p_->engine && !p_->engine->IsPaused() && !p_->engine->IsEnded();
}
bool VideoPlayer::ended() const { return p_ && p_->engine && p_->engine->IsEnded(); }

double VideoPlayer::duration() const {
    if (!p_ || !p_->engine) return 0;
    double d = p_->engine->GetDuration();
    return (d > 0 && d < 1e9) ? d : 0;
}
double VideoPlayer::position() const {
    if (!p_ || !p_->engine) return 0;
    double t = p_->engine->GetCurrentTime();
    return t > 0 ? t : 0;
}

void VideoPlayer::play() { if (p_ && p_->engine) { if (p_->engine->IsEnded()) p_->engine->SetCurrentTime(0); p_->engine->Play(); } }
void VideoPlayer::pause() { if (p_ && p_->engine) p_->engine->Pause(); }
void VideoPlayer::togglePlay() { playing() ? pause() : play(); }

void VideoPlayer::seek(double sec) {
    if (!p_ || !p_->engine) return;
    double d = duration();
    if (d > 0) sec = clampf((float)sec, 0.f, (float)std::max(0.0, d - 0.05));
    else if (sec < 0) sec = 0;
    beginQuiet();
    p_->engine->SetCurrentTime(sec);
}

void VideoPlayer::beginQuiet() {
    if (!p_ || !p_->engine) return;
    p_->quiet = true;
    p_->engine->SetVolume(0.0);
}

void VideoPlayer::endQuiet() {
    if (!p_ || !p_->engine) return;
    p_->quiet = false;
    p_->engine->SetVolume(p_->wantVolume);
}
void VideoPlayer::seekBy(double delta) { seek(position() + delta); }

void VideoPlayer::setVolume(float v) {
    if (!p_) return;
    p_->wantVolume = clampf(v, 0.f, 1.f);
    if (p_->engine && !p_->quiet) p_->engine->SetVolume(p_->wantVolume);
}
float VideoPlayer::volume() const { return p_ ? p_->wantVolume : 1.f; }

void VideoPlayer::setMuted(bool m) {
    if (!p_) return;
    p_->wantMuted = m;
    if (p_->engine) p_->engine->SetMuted(m ? TRUE : FALSE);
}
bool VideoPlayer::muted() const { return p_ && p_->wantMuted; }

void VideoPlayer::setRate(double r) { if (p_ && p_->engine) p_->engine->SetPlaybackRate(r); }
double VideoPlayer::rate() const { return p_ && p_->engine ? p_->engine->GetPlaybackRate() : 1.0; }

// Pull the current frame if the engine has a new one. Returns the bitmap to
// draw (which stays valid until the next successful call).
ID2D1Bitmap1* VideoPlayer::frame(ID2D1DeviceContext* dc) {
    if (!p_ || !p_->engine || !p_->metaKnown || !dc) return p_ ? p_->bmp.Get() : nullptr;

    if (!p_->tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)p_->w;
        td.Height = (UINT)p_->h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(p_->d3d->CreateTexture2D(&td, nullptr, &p_->tex))) return nullptr;

        ComPtr<IDXGISurface> surf;
        if (FAILED(p_->tex.As(&surf))) { p_->tex.Reset(); return nullptr; }

        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &p_->bmp))) {
            p_->tex.Reset();
            return nullptr;
        }
    }

    LONGLONG pts = 0;
    if (p_->engine->OnVideoStreamTick(&pts) == S_OK) {
        RECT dst{ 0, 0, p_->w, p_->h };
        MFARGB border{ 0, 0, 0, 255 };
        p_->engine->TransferVideoFrame(p_->tex.Get(), nullptr, &dst, &border);
        p_->lastPts = (double)pts / 1e7;
    }
    return p_->bmp.Get();
}

// ------------------------------------------------------------------ scrub preview
// A second, independent decode path used only for the thumbnail that follows the
// cursor along the seek bar. Its own thread and its own source reader, so it can
// never stall playback; only the newest request matters.
struct VideoPreview::Impl {
    std::thread             th;
    std::mutex              m;
    std::condition_variable cv;
    bool     quit = false;
    bool     haveReq = false;
    double   reqTime = 0;
    wstring  path;
    int      maxW = 240;

    std::mutex om;
    PixelBuf  out;
    double    outTime = -1;
    bool      outFresh = false;
    std::atomic<bool> ready{ false };
};

namespace {

// RGB32 frames arrive bottom-up unless the stride says otherwise; this box
// filter flips and shrinks in one pass.
void shrinkRGB32(const BYTE* src, LONG stride, int sw, int sh, int dw, int dh, PixelBuf& out) {
    out.w = dw; out.h = dh;
    out.px.assign((size_t)dw * dh * 4, 0);
    bool bottomUp = stride < 0;
    LONG absStride = bottomUp ? -stride : stride;
    const BYTE* base = bottomUp ? src + (size_t)(sh - 1) * absStride : src;
    ptrdiff_t step = bottomUp ? -(ptrdiff_t)absStride : (ptrdiff_t)absStride;

    for (int y = 0; y < dh; ++y) {
        int sy0 = y * sh / dh, sy1 = std::max(sy0 + 1, (y + 1) * sh / dh);
        uint8_t* orow = out.px.data() + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            int sx0 = x * sw / dw, sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const BYTE* row = base + (ptrdiff_t)sy * step;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const BYTE* px = row + (size_t)sx * 4;
                    b += px[0]; g += px[1]; r += px[2];
                    ++n;
                }
            }
            if (!n) n = 1;
            orow[x * 4 + 0] = (uint8_t)(b / n);
            orow[x * 4 + 1] = (uint8_t)(g / n);
            orow[x * 4 + 2] = (uint8_t)(r / n);
            orow[x * 4 + 3] = 255;
        }
    }
}

} // namespace

VideoPreview::~VideoPreview() { close(); }

void VideoPreview::close() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->quit = true;
    }
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    delete p_;
    p_ = nullptr;
}

bool    VideoPreview::ready() const { return p_ && p_->ready.load(); }
wstring VideoPreview::path() const { return p_ ? p_->path : wstring(); }

void VideoPreview::open(const wstring& path, int maxWidth) {
    close();
    auto* p = new Impl();
    p->path = path;
    p->maxW = clampi(maxWidth, 80, 480);
    p_ = p;

    p->th = std::thread([p] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);

        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFAttributes> attr;
        int sw = 0, sh = 0;
        LONG defStride = 0;

        if (SUCCEEDED(MFCreateAttributes(&attr, 2))) {
            attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            attr->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
        }
        if (SUCCEEDED(MFCreateSourceReaderFromURL(p->path.c_str(), attr.Get(), &reader))) {
            reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
            reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

            // Ask the reader's own video processor for a thumbnail-sized frame.
            // Converting and scaling 1080p to RGB32 on the CPU for every scrub
            // step is what made a long file feel frozen; letting the pipeline
            // hand back a small frame costs a fraction of that.
            UINT32 nw = 0, nh = 0;
            ComPtr<IMFMediaType> nat;
            if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nat)))
                MFGetAttributeSize(nat.Get(), MF_MT_FRAME_SIZE, &nw, &nh);

            for (int attempt = 0; attempt < 2 && sw == 0; ++attempt) {
                ComPtr<IMFMediaType> want;
                if (FAILED(MFCreateMediaType(&want))) break;
                want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                if (attempt == 0 && nw > 0 && nh > 0 && (int)nw > p->maxW) {
                    UINT32 dw = (UINT32)(p->maxW & ~1);
                    UINT32 dh = (UINT32)(((UINT64)nh * dw / nw) & ~1u);
                    if (dh < 2) dh = 2;
                    MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, dw, dh);
                } else if (attempt == 0) {
                    continue;                      // nothing to shrink, use the plain path
                }
                if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                       nullptr, want.Get())))
                    continue;
                ComPtr<IMFMediaType> cur;
                if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
                    continue;
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                sw = (int)w; sh = (int)h;
                UINT32 st = 0;
                if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)))
                    defStride = (LONG)(INT32)st;
                if (defStride == 0) defStride = (LONG)w * 4;
            }
        }
        if (sw > 0 && sh > 0) p->ready.store(true);

        for (;;) {
            double t = 0;
            {
                std::unique_lock<std::mutex> lk(p->m);
                p->cv.wait(lk, [p] { return p->quit || p->haveReq; });
                if (p->quit) break;
                t = p->reqTime;
                p->haveReq = false;
            }
            if (!reader || sw <= 0) continue;
            double t0 = nowSec();
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = (LONGLONG)(t * 1e7);
            reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);

            // A seek only lands on a key frame, so walk forward to the moment the
            // cursor is actually on - bounded, so a long GOP cannot stall this.
            // Walking forward from the key frame is what costs the time, so it
            // gets a budget: past it we show the frame we have. A scrub preview
            // that is a second early beats one that never appears.
            const double walkBudget = 0.10;
            ComPtr<IMFSample> chosen;
            LONGLONG chosenTs = 0;
            bool quit = false;
            for (int guard = 0; guard < 72; ++guard) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                              nullptr, &flags, &ts, &sample)))
                    break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                if (sample) { chosen = sample; chosenTs = ts; }

                // A newer request takes over the walk, but whatever has already
                // been decoded is still a usable frame near the right moment.
                // Dropping it was why the card froze while the pointer swept the
                // track: every request was superseded before it produced
                // anything, so nothing was ever published.
                bool newer = false;
                {
                    std::lock_guard<std::mutex> lk(p->m);
                    quit = p->quit;
                    newer = p->haveReq;
                }
                if (quit || newer) break;
                if ((double)ts / 1e7 >= t - 0.03) break;
                if (chosen && nowSec() - t0 > walkBudget) break;
            }

            if (!quit && chosen) {
                LONGLONG ts = chosenTs;
                ComPtr<IMFSample> sample = chosen;
                ComPtr<IMFMediaBuffer> buf;
                if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) continue;

                int dw = std::min(p->maxW, sw);
                int dh = std::max(1, sh * dw / std::max(1, sw));
                PixelBuf frame;

                ComPtr<IMF2DBuffer> b2d;
                BYTE* data = nullptr;
                LONG stride = defStride;
                bool locked2d = false, locked = false;
                if (SUCCEEDED(buf.As(&b2d)) && SUCCEEDED(b2d->Lock2D(&data, &stride))) locked2d = true;
                else if (SUCCEEDED(buf->Lock(&data, nullptr, nullptr))) { stride = defStride; locked = true; }

                if (data) shrinkRGB32(data, stride, sw, sh, dw, dh, frame);
                if (locked2d) b2d->Unlock2D();
                else if (locked) buf->Unlock();

                if (frame.valid()) {
                    std::lock_guard<std::mutex> lk(p->om);
                    p->out = std::move(frame);
                    p->outTime = (double)ts / 1e7;
                    p->outFresh = true;
                }
            }
        }

        reader.Reset();
        attr.Reset();
        MFShutdown();
        CoUninitialize();
    });
}

void VideoPreview::request(double seconds) {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->reqTime = std::max(0.0, seconds);
        p_->haveReq = true;
    }
    p_->cv.notify_one();
}

bool VideoPreview::poll(PixelBuf& out, double& atSeconds) {
    if (!p_) return false;
    std::lock_guard<std::mutex> lk(p_->om);
    if (!p_->outFresh) return false;
    out = std::move(p_->out);
    atSeconds = p_->outTime;
    p_->outFresh = false;
    return true;
}

// ------------------------------------------------------------------ metadata
// Everything Explorer shows in its Details pane is already indexed by the shell
// property system, so we ask that instead of demuxing the file ourselves.
// Media Foundation stores the codec as a FourCC packed into a GUID, and the
// shell's own display strings are English, so the values are decoded here.
static wstring fourccOf(unsigned v) {
    char c[5] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF),
                  (char)((v >> 16) & 0xFF), (char)((v >> 24) & 0xFF), 0 };
    for (int i = 0; i < 4; ++i) if (c[i] < 32 || c[i] > 126) return L"";
    wstring out;
    for (int i = 0; i < 4; ++i) out += (wchar_t)towupper((wint_t)c[i]);
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static wstring videoCodecName(unsigned data1) {
    wstring f = fourccOf(data1);
    if (f == L"H264" || f == L"AVC1" || f == L"X264") return L"H.264 / AVC";
    if (f == L"HEVC" || f == L"HVC1" || f == L"HEV1" || f == L"H265") return L"H.265 / HEVC";
    if (f == L"AV01") return L"AV1";
    if (f == L"VP90") return L"VP9";
    if (f == L"VP80") return L"VP8";
    if (f == L"MP4V" || f == L"MPG4" || f == L"DIVX" || f == L"XVID" || f == L"DX50") return L"MPEG-4";
    if (f == L"MPG2" || f == L"MP2V") return L"MPEG-2";
    if (f == L"WMV1" || f == L"WMV2" || f == L"WMV3" || f == L"WVC1") return L"Windows Media Video";
    if (f == L"MJPG") return L"Motion JPEG";
    return f;
}

static wstring audioCodecName(unsigned data1) {
    switch (data1 & 0xFFFFu) {
        case 0x0001: return L"PCM";
        case 0x0003: return L"PCM (float)";
        case 0x0055: return L"MP3";
        case 0x0161: return L"WMA";
        case 0x0162: return L"WMA Pro";
        case 0x1610: return L"AAC";
        case 0x00FF: return L"AAC";
        case 0x2000: return L"AC-3";
        case 0x2001: return L"DTS";
        case 0xF1AC: return L"FLAC";
        case 0x704F: return L"Opus";
        case 0x674F: case 0x6750: case 0x6751: return L"Vorbis";
        default: return L"";
    }
}

static wstring fmtBitrate(unsigned bps) {
    wchar_t b[40];
    if (bps >= 1000000) { swprintf(b, 40, T(L"%.1f Мбіт/с"), bps / 1e6); return b; }
    if (bps >= 1000) { swprintf(b, 40, T(L"%u кбіт/с"), bps / 1000); return b; }
    return std::to_wstring(bps) + T(L" біт/с");
}

// Everything Explorer shows in its Details pane is already indexed by the shell
// property system, so we ask that instead of demuxing the file ourselves.
void readMediaProps(const wstring& path, std::vector<std::pair<wstring, wstring>>& out) {
    out.clear();
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return;

    auto u32 = [&](REFPROPERTYKEY key, unsigned& v) {
        PROPVARIANT pv; PropVariantInit(&pv);
        bool ok = false;
        if (SUCCEEDED(item->GetProperty(key, &pv))) {
            if (pv.vt == VT_UI4) { v = pv.ulVal; ok = true; }
            else if (pv.vt == VT_UI8) { v = (unsigned)pv.uhVal.QuadPart; ok = true; }
            else if (pv.vt == VT_I4) { v = (unsigned)pv.lVal; ok = true; }
        }
        PropVariantClear(&pv);
        return ok;
    };
    // Both the video and audio "format" keys carry a GUID whose first field is
    // the FourCC (video) or the WAVE tag (audio).
    auto guidData1 = [&](REFPROPERTYKEY key, unsigned& v) {
        PROPVARIANT pv; PropVariantInit(&pv);
        bool ok = false;
        if (SUCCEEDED(item->GetProperty(key, &pv))) {
            if (pv.vt == VT_CLSID && pv.puuid) { v = pv.puuid->Data1; ok = true; }
            else if (pv.vt == VT_LPWSTR && pv.pwszVal && wcslen(pv.pwszVal) > 9 && pv.pwszVal[0] == L'{') {
                v = (unsigned)wcstoul(wstring(pv.pwszVal + 1, 8).c_str(), nullptr, 16);
                ok = true;
            }
        }
        PropVariantClear(&pv);
        return ok;
    };

    unsigned v = 0;
    if (guidData1(PKEY_Video_Compression, v)) {
        wstring name = videoCodecName(v);
        if (!name.empty()) out.emplace_back(T(L"Відеокодек"), name);
    }
    if (u32(PKEY_Video_FrameRate, v) && v) {
        wchar_t b[40];
        double fps = v / 1000.0;
        swprintf(b, 40, (fabs(fps - floor(fps + 0.5)) < 0.01) ? T(L"%.0f кадр/с") : T(L"%.2f кадр/с"), fps);
        out.emplace_back(T(L"Частота кадрів"), b);
    }
    if (u32(PKEY_Video_EncodingBitrate, v) && v)
        out.emplace_back(T(L"Бітрейт відео"), fmtBitrate(v));

    if (guidData1(PKEY_Audio_Format, v)) {
        wstring name = audioCodecName(v);
        if (!name.empty()) out.emplace_back(T(L"Аудіокодек"), name);
    }
    if (u32(PKEY_Audio_ChannelCount, v) && v) {
        wstring ch = std::to_wstring(v);
        if (v == 1) ch += T(L"  (моно)");
        else if (v == 2) ch += T(L"  (стерео)");
        else if (v == 6) ch += L"  (5.1)";
        else if (v == 8) ch += L"  (7.1)";
        out.emplace_back(T(L"Канали"), ch);
    }
    if (u32(PKEY_Audio_SampleRate, v) && v) {
        wchar_t b[40];
        swprintf(b, 40, T(L"%.1f кГц"), v / 1000.0);
        out.emplace_back(T(L"Частота"), b);
    }
    if (u32(PKEY_Audio_EncodingBitrate, v) && v)
        out.emplace_back(T(L"Бітрейт аудіо"), fmtBitrate(v));
}

// ------------------------------------------------------------------ helpers
const wchar_t* kVideoExts[] = {
    L".mp4", L".m4v", L".mov", L".mkv", L".avi", L".wmv", L".webm", L".mpg",
    L".mpeg", L".m2ts", L".mts", L".ts", L".3gp", L".3g2", L".asf", L".ogv",
    L".vob", L".divx", L".f4v", L".m2v"
};

const std::vector<wstring>& videoExtensions() {
    static std::vector<wstring> v(std::begin(kVideoExts), std::end(kVideoExts));
    return v;
}

bool isVideoExt(const wstring& ext) {
    if (ext.empty()) return false;
    wstring e = lowerOf(ext);
    for (const auto& v : videoExtensions()) if (v == e) return true;
    return false;
}

bool isVideoPath(const wstring& path) { return isVideoExt(extOf(path)); }

wstring formatTime(double seconds) {
    if (!(seconds >= 0) || seconds > 359999) return L"--:--";
    int t = (int)(seconds + 0.0001);
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    wchar_t buf[32];
    if (h > 0) swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    else       swprintf(buf, 32, L"%d:%02d", m, s);
    return buf;
}
