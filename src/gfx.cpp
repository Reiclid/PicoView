// Direct3D 11 + DirectComposition + Direct2D surface.
//
// The swap chain is composition-based with premultiplied alpha, so whatever we
// leave transparent shows the DWM Mica backdrop underneath - that is what makes
// the chrome look like a native Windows 11 app. If DirectComposition is not
// available we fall back to a plain opaque HWND swap chain.
#include "pg.h"

static const D2D1_COLOR_F kClear = { 0.f, 0.f, 0.f, 0.f };

// Creating the D3D device pulls in the graphics driver and is by far the most
// expensive part of start-up, so it is kicked off on its own thread before the
// window even exists and collected later.
static std::thread                 g_warmThread;
static ComPtr<ID3D11Device>        g_warmDev;
static ComPtr<ID3D11DeviceContext> g_warmCtx;

static HRESULT createD3D(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    // Video support is required by the Media Engine, and the device must stay
    // thread-safe because MF decodes on its own threads.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3
    };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        // Some drivers refuse the video flag; fall back without it.
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    return hr;
}

void Gfx::warmStart() {
    if (g_warmThread.joinable()) return;
    g_warmThread = std::thread([] {
        double t = nowSec();
        createD3D(g_warmDev, g_warmCtx);
        pgLog("  gfx: warm D3D device ready in %.1f ms", (nowSec() - t) * 1000);
    });
}

bool Gfx::init(HWND h) {
    double t0 = nowSec();
    hwnd = h;
    dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;

    RECT rc{}; GetClientRect(h, &rc);
    width = std::max<int>(1, rc.right - rc.left);
    height = std::max<int>(1, rc.bottom - rc.top);

    HRESULT hr = E_FAIL;
    if (g_warmThread.joinable()) {
        g_warmThread.join();
        if (g_warmDev) { d3d = g_warmDev; d3dCtx = g_warmCtx; hr = S_OK; }
        g_warmDev.Reset(); g_warmCtx.Reset();
    }
    if (FAILED(hr)) hr = createD3D(d3d, d3dCtx);
    if (FAILED(hr)) return false;
    pgLog("  gfx: D3D11CreateDevice %.1f ms", (nowSec() - t0) * 1000);

    ComPtr<IDXGIDevice1> dxgiDev;
    if (FAILED(d3d.As(&dxgiDev))) return false;
    dxgiDev->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;

    if (SUCCEEDED(factory->CreateSwapChainForComposition(d3d.Get(), &sd, nullptr, &swap))) {
        if (SUCCEEDED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp))) &&
            SUCCEEDED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget)) &&
            SUCCEEDED(dcomp->CreateVisual(&dvisual)) &&
            SUCCEEDED(dvisual->SetContent(swap.Get())) &&
            SUCCEEDED(dtarget->SetRoot(dvisual.Get()))) {
            dcomp->Commit();
            composed = true;
        } else {
            swap.Reset(); dcomp.Reset(); dtarget.Reset(); dvisual.Reset();
        }
    }
    if (!composed) {
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(d3d.Get(), hwnd, &sd, nullptr, nullptr, &swap)))
            return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    pgLog("  gfx: swapchain+dcomp %.1f ms", (nowSec() - t0) * 1000);

    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3),
                                 &opts, (void**)d2dFactory.GetAddressOf())))
        return false;
    if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice))) return false;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    dc->SetDpi(96.f, 96.f);        // 1 unit == 1 physical pixel
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    // ClearType is only valid over opaque pixels. This target is transparent
    // wherever the Mica backdrop shows through, so grayscale AA it is.
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    pgLog("  gfx: d2d device %.1f ms", (nowSec() - t0) * 1000);
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                   (IUnknown**)dw.GetAddressOf())))
        return false;
    pgLog("  gfx: dwrite %.1f ms", (nowSec() - t0) * 1000);

    setDpi(dpi);
    pgLog("  gfx: text formats %.1f ms", (nowSec() - t0) * 1000);
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush))) return false;
    return resize(width, height);
}

void Gfx::setDpi(UINT newDpi) {
    dpi = newDpi ? newDpi : 96;
    if (!dw) return;

    auto mk = [&](const wchar_t* family, float sizeDip, DWRITE_FONT_WEIGHT w,
                  ComPtr<IDWriteTextFormat>& out) {
        out.Reset();
        if (FAILED(dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, s(sizeDip), L"", &out)))
            dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, s(sizeDip), L"", &out);
        if (out) {
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            out->SetTrimming(nullptr, nullptr);
        }
    };

    const wchar_t* ui = L"Segoe UI Variable Text";
    const wchar_t* disp = L"Segoe UI Variable Display";
    mk(ui, 12.f, DWRITE_FONT_WEIGHT_NORMAL, fCaption);
    mk(ui, 14.f, DWRITE_FONT_WEIGHT_NORMAL, fBody);
    mk(ui, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fBodyStrong);
    mk(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, fSmall);
    mk(disp, 21.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fTitle);
    mk(L"Segoe Fluent Icons", 15.f, DWRITE_FONT_WEIGHT_NORMAL, fIcon);
    mk(L"Segoe Fluent Icons", 10.5f, DWRITE_FONT_WEIGHT_NORMAL, fIconSmall);
    mk(L"Segoe Fluent Icons", 26.f, DWRITE_FONT_WEIGHT_NORMAL, fIconBig);

    if (fIcon)      fIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (fIconSmall) fIconSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (fIconBig) fIconBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

bool Gfx::resize(int w, int h) {
    if (!swap || !dc) return false;
    w = std::max(1, w); h = std::max(1, h);
    width = w; height = h;

    dc->SetTarget(nullptr);
    target.Reset();

    HRESULT hr = swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
    if (FAILED(hr)) return false;

    ComPtr<IDXGISurface2> surf;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc->CreateBitmapFromDxgiSurface(surf.Get(), &props, &target))) return false;
    dc->SetTarget(target.Get());
    return true;
}

bool Gfx::begin() {
    if (!dc || !target) return false;
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(composed ? kClear : D2D1::ColorF(0.06f, 0.06f, 0.06f, 1.f));
    return true;
}

void Gfx::end() {
    if (!dc) return;
    HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { shutdown(); init(hwnd); return; }
    if (swap) swap->Present(1, 0);
    if (composed && dcomp) dcomp->Commit();
}

void Gfx::shutdown() {
    if (g_warmThread.joinable()) g_warmThread.join();
    g_warmDev.Reset(); g_warmCtx.Reset();
    if (dc) dc->SetTarget(nullptr);
    target.Reset(); brush.Reset();
    fCaption.Reset(); fBody.Reset(); fBodyStrong.Reset(); fSmall.Reset();
    fTitle.Reset(); fIcon.Reset(); fIconBig.Reset(); fIconSmall.Reset();
    dw.Reset(); dc.Reset(); d2dDevice.Reset(); d2dFactory.Reset();
    dvisual.Reset(); dtarget.Reset(); dcomp.Reset();
    swap.Reset(); d3dCtx.Reset(); d3d.Reset();
    composed = false;
}

// Halve a premultiplied BGRA buffer in place-ish; used only when an image is
// larger than what the GPU will accept as a texture.
static void halve(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst, int& dw, int& dh) {
    dw = std::max(1, sw / 2); dh = std::max(1, sh / 2);
    dst.resize((size_t)dw * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const uint8_t* r0 = src + (size_t)(y * 2) * sw * 4;
        const uint8_t* r1 = src + (size_t)std::min(y * 2 + 1, sh - 1) * sw * 4;
        uint8_t* o = dst.data() + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            int x0 = x * 2, x1 = std::min(x * 2 + 1, sw - 1);
            for (int c = 0; c < 4; ++c)
                o[x * 4 + c] = (uint8_t)((r0[x0 * 4 + c] + r0[x1 * 4 + c] + r1[x0 * 4 + c] + r1[x1 * 4 + c] + 2) / 4);
        }
    }
}

ComPtr<ID2D1Bitmap1> Gfx::upload(const PixelBuf& buf) {
    ComPtr<ID2D1Bitmap1> bmp;
    if (!dc || !buf.valid()) return bmp;

    UINT maxDim = dc->GetMaximumBitmapSize();
    if (maxDim == 0) maxDim = 4096;

    const uint8_t* px = buf.px.data();
    int w = buf.w, h = buf.h;
    std::vector<uint8_t> scratch;
    while ((UINT)w > maxDim || (UINT)h > maxDim) {
        std::vector<uint8_t> next;
        int nw = 0, nh = 0;
        halve(px, w, h, next, nw, nh);
        scratch = std::move(next);
        px = scratch.data(); w = nw; h = nh;
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU((UINT)w, (UINT)h), px, (UINT)w * 4, &props, &bmp)))
        bmp.Reset();
    return bmp;
}

void Gfx::text(const wstring& str, IDWriteTextFormat* f, D2D1_RECT_F r, const D2D1_COLOR_F& c,
               DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa) {
    if (!f || str.empty()) return;
    f->SetTextAlignment(ta);
    f->SetParagraphAlignment(pa);
    dc->DrawTextW(str.c_str(), (UINT32)str.size(), f, r, solid(c),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

D2D1_SIZE_F Gfx::measure(const wstring& str, IDWriteTextFormat* f, float maxW) {
    D2D1_SIZE_F out{ 0, 0 };
    if (!f || str.empty() || !dw) return out;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(str.c_str(), (UINT32)str.size(), f, maxW, 10000.f, &layout)))
        return out;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    out.width = m.widthIncludingTrailingWhitespace;
    out.height = m.height;
    return out;
}

D2D1_SIZE_F Gfx::textWrap(const wstring& str, IDWriteTextFormat* f, D2D1_RECT_F r,
                          const D2D1_COLOR_F& c) {
    D2D1_SIZE_F out{ 0, 0 };
    if (!f || str.empty() || !dw) return out;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    out = measure(str, f, r.right - r.left);
    dc->DrawTextW(str.c_str(), (UINT32)str.size(), f, r, solid(c),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return out;
}

void Gfx::roundRect(D2D1_RECT_F r, float radius, const D2D1_COLOR_F& fill) {
    if (fill.a <= 0.001f) return;
    if (radius <= 0.5f) { dc->FillRectangle(r, solid(fill)); return; }
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), solid(fill));
}

// Rounded clip for bitmaps: a layer masked by a rounded rectangle.
void Gfx::pushRoundClip(D2D1_RECT_F r, float radius) {
    ComPtr<ID2D1RoundedRectangleGeometry> geom;
    if (FAILED(d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &geom))) {
        dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        return;
    }
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geom.Get()), nullptr);
}

void Gfx::popRoundClip() { dc->PopLayer(); }

void Gfx::roundRectStroke(D2D1_RECT_F r, float radius, const D2D1_COLOR_F& c, float w) {
    if (c.a <= 0.001f || w <= 0.f) return;
    float i = w * 0.5f;
    D2D1_RECT_F rr{ r.left + i, r.top + i, r.right - i, r.bottom - i };
    if (radius <= 0.5f) dc->DrawRectangle(rr, solid(c), w);
    else dc->DrawRoundedRectangle(D2D1::RoundedRect(rr, std::max(0.f, radius - i), std::max(0.f, radius - i)), solid(c), w);
}
