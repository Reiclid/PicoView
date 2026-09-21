/* Hover over a file in Explorer, see it.
 *
 * This is the second kind of PicoView plugin: not a decoder and not a filter,
 * but a service - something that runs quietly in the background while it is
 * switched on. PicoView starts it on its interface thread and never speaks to
 * it again until it is switched off.
 *
 * How it knows what you are pointing at, cheapest test first:
 *
 *   1. Where is the cursor, and has it stopped? GetCursorPos, free.
 *   2. Is the window under it an Explorer window? GetClassName, free.
 *   3. Which item is under it? UI Automation's ElementFromPoint. This one
 *      costs tens of milliseconds, so it only runs once the pointer has been
 *      still for a moment over Explorer, never on every poll.
 *   4. Which file is that? Explorer's list gives a display name, which is not
 *      a path and may not even have the extension on it. The folder comes from
 *      the shell's own view of that window; the extension, when Windows is
 *      hiding it, from one FindFirstFile against "name.*".
 *   5. Is there a real thumbnail for it? If not, nothing is shown - a card
 *      with a generic icon in it would be worse than no card.
 *
 * The card itself is a layered window drawn by hand into a DIB: rounded, dark,
 * never takes focus, and goes away the moment the pointer moves off the item.
 *
 * The plugin interface is C. This file is C++ only because COM in C is a wall
 * of vtable macros, and none of that crosses the boundary.
 */
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <uiautomation.h>
#include <string>
#include <math.h>

#include "../../include/picoview_plugin.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "msimg32.lib")

static const PvHost* g_host = nullptr;
static HANDLE  g_thread = nullptr;
static HANDLE  g_quit = nullptr;
static HWND    g_popup = nullptr;
static bool    g_shown = false;
static std::wstring g_showing;          // path the card is currently showing

static void note(const wchar_t* m) {
    if (g_host && g_host->log) g_host->log(m);
}

/* ------------------------------------------------------------------ COM ptr */
template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

/* ------------------------------------------------------------- the card */
/* Everything is painted into one 32-bit DIB and handed to the compositor in a
   single call. No WM_PAINT, no flicker, and per-pixel alpha for the corners. */

static const int kPad = 10;
static const int kCaption = 20;
static const int kMaxThumb = 320;

static float roundedCoverage(float x, float y, float w, float h, float r) {
    /* Distance to a rounded rectangle, turned into one pixel of coverage.
       Cheaper and steadier than any of the ways GDI would do this. */
    float dx = fabsf(x - w * 0.5f) - (w * 0.5f - r);
    float dy = fabsf(y - h * 0.5f) - (h * 0.5f - r);
    float ax = dx > 0 ? dx : 0, ay = dy > 0 ? dy : 0;
    float outside = sqrtf(ax * ax + ay * ay) - r;
    float inside = (dx > dy ? dx : dy) - r;
    float d = (dx > 0 && dy > 0) ? outside : inside;
    float c = 0.5f - d;                      /* one pixel of antialiasing */
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

static void paintCard(HDC dc, HBITMAP dib, uint8_t* bits, int W, int H,
                      HBITMAP thumb, int sw, int sh, int tw, int th,
                      const wchar_t* caption) {
    HGDIOBJ old = SelectObject(dc, dib);

    /* Card body. Dark enough to sit over anything, with a hairline edge. */
    RECT all = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 34));
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    RECT edge = { 0, 0, W, H };
    HBRUSH line = CreateSolidBrush(RGB(64, 64, 68));
    FrameRect(dc, &edge, line);
    DeleteObject(line);

    /* The thumbnail, composited properly: Explorer hands back premultiplied
       alpha for anything with transparency, and BitBlt would ignore it. The
       source rectangle is the whole bitmap - Explorer is free to return one
       bigger than was asked for, and blitting it at its own size would show a
       corner of the picture instead of the picture. */
    if (thumb) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ so = SelectObject(src, thumb);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, kPad, kPad, tw, th, src, 0, 0, sw, sh, bf);
        SelectObject(src, so);
        DeleteDC(src);
    }

    if (caption && *caption) {
        HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(226, 226, 230));
        RECT tr = { kPad, H - kCaption - 4, W - kPad, H - 4 };
        DrawTextW(dc, caption, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                                        DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(f);
    }

    /* GDI leaves the alpha channel as it found it, and text drawing scribbles
       over it, so the whole surface gets its alpha decided here - opaque
       inside the rounded shape, feathered at the corners. */
    GdiFlush();
    for (int y = 0; y < H; ++y) {
        uint8_t* row = bits + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
            float c = roundedCoverage(x + 0.5f, y + 0.5f, (float)W, (float)H, 8.0f);
            uint8_t a = (uint8_t)(c * 255.0f + 0.5f);
            uint8_t* px = row + x * 4;
            /* The layered window wants premultiplied colour. */
            px[0] = (uint8_t)(px[0] * a / 255);
            px[1] = (uint8_t)(px[1] * a / 255);
            px[2] = (uint8_t)(px[2] * a / 255);
            px[3] = a;
        }
    }
    SelectObject(dc, old);
}

static void hideCard() {
    if (g_popup && g_shown) {
        ShowWindow(g_popup, SW_HIDE);
        g_shown = false;
    }
    g_showing.clear();
}

static void showCard(const std::wstring& path, HBITMAP thumb, int sw, int sh,
                     int tw, int th, POINT at) {
    int W = tw + kPad * 2;
    int H = th + kPad * 2 + kCaption;
    if (W < 140) W = 140;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;                 /* top down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }

    const wchar_t* name = PathFindFileNameW(path.c_str());
    paintCard(mem, dib, (uint8_t*)bits, W, H, thumb, sw, sh, tw, th, name);

    /* Beside the pointer, never under it, and always on the monitor it is on. */
    /* Clear of Windows' own tooltip, which comes up in the same corner. */
    POINT pos = { at.x + 26, at.y + 30 };
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        if (pos.x + W > mi.rcWork.right)  pos.x = at.x - W - 18;
        if (pos.y + H > mi.rcWork.bottom) pos.y = mi.rcWork.bottom - H - 4;
        if (pos.x < mi.rcWork.left) pos.x = mi.rcWork.left + 4;
        if (pos.y < mi.rcWork.top)  pos.y = mi.rcWork.top + 4;
    }

    HGDIOBJ old = SelectObject(mem, dib);
    POINT src = { 0, 0 };
    SIZE  size = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_popup, screen, &pos, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);

    if (!g_shown) {
        ShowWindow(g_popup, SW_SHOWNOACTIVATE);
        g_shown = true;
    }
    g_showing = path;

    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

/* --------------------------------------------------------------- thumbnail */
static HBITMAP thumbnailFor(const std::wstring& path, int& w, int& h) {
    Ref<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f)
        return nullptr;

    SIZE want = { kMaxThumb, kMaxThumb };
    HBITMAP bmp = nullptr;
    /* THUMBNAILONLY is the whole point: a file with no real thumbnail gets no
       card, rather than a card with a generic icon in it. */
    if (FAILED(f->GetImage(want, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp)) || !bmp)
        return nullptr;

    BITMAP bm = {};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(bmp);
        return nullptr;
    }
    w = bm.bmWidth;
    h = bm.bmHeight;
    return bmp;
}

/* ------------------------------------------------------- what is under it */
static bool isExplorer(HWND top) {
    wchar_t cls[64] = {};
    GetClassNameW(top, cls, 63);
    return wcscmp(cls, L"CabinetWClass") == 0 || wcscmp(cls, L"ExploreWClass") == 0;
}

/* The folder that window is showing, straight from the shell rather than by
   reading the address bar. */
static bool folderOfWindow(HWND top, std::wstring& out) {
    Ref<IShellWindows> sw;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw))) || !sw)
        return false;

    long count = 0;
    if (FAILED(sw->get_Count(&count))) return false;
    for (long i = 0; i < count; ++i) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        Ref<IDispatch> disp;
        if (FAILED(sw->Item(v, &disp)) || !disp) continue;

        Ref<IWebBrowserApp> app;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(&app))) || !app) continue;
        SHANDLE_PTR hw = 0;
        if (FAILED(app->get_HWND(&hw)) || (HWND)hw != top) continue;

        Ref<IServiceProvider> sp;
        if (FAILED(app->QueryInterface(IID_PPV_ARGS(&sp))) || !sp) return false;
        Ref<IShellBrowser> sb;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) || !sb) return false;
        Ref<IShellView> sv;
        if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) return false;
        Ref<IFolderView> fv;
        if (FAILED(sv->QueryInterface(IID_PPV_ARGS(&fv))) || !fv) return false;
        Ref<IPersistFolder2> pf;
        if (FAILED(fv->GetFolder(IID_PPV_ARGS(&pf))) || !pf) return false;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(pf->GetCurFolder(&pidl)) || !pidl) return false;
        wchar_t buf[MAX_PATH] = {};
        bool ok = SHGetPathFromIDListW(pidl, buf) != 0;
        CoTaskMemFree(pidl);
        if (ok) out = buf;
        return ok;
    }
    return false;
}

/* The display name of the list item under the pointer. This is the expensive
   step, so it runs only once the pointer has settled. */
static bool itemNameAt(IUIAutomation* ua, POINT pt, std::wstring& out) {
    if (!ua) return false;
    Ref<IUIAutomationElement> el;
    if (FAILED(ua->ElementFromPoint(pt, &el)) || !el) return false;

    /* The point may land on a label inside the item rather than the item, so
       walk up a little looking for the row itself. */
    Ref<IUIAutomationTreeWalker> walker;
    ua->get_ControlViewWalker(&walker);
    IUIAutomationElement* cur = el.p;
    cur->AddRef();
    for (int depth = 0; depth < 4 && cur; ++depth) {
        CONTROLTYPEID type = 0;
        if (SUCCEEDED(cur->get_CurrentControlType(&type)) &&
            (type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId)) {
            BSTR name = nullptr;
            bool ok = SUCCEEDED(cur->get_CurrentName(&name)) && name && *name;
            if (ok) out = name;
            if (name) SysFreeString(name);
            cur->Release();
            return ok;
        }
        if (!walker) break;
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
        cur->Release();
        cur = parent;
    }
    if (cur) cur->Release();
    return false;
}

/* Display name plus folder, into a real path. Windows hides known extensions
   by default, so the direct join is tried first and a single wildcard lookup
   covers the rest. */
static bool resolvePath(const std::wstring& folder, const std::wstring& name, std::wstring& out) {
    if (folder.empty() || name.empty()) return false;
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos)
        return false;

    std::wstring direct = folder;
    if (!direct.empty() && direct.back() != L'\\') direct += L'\\';
    direct += name;

    DWORD at = GetFileAttributesW(direct.c_str());
    if (at != INVALID_FILE_ATTRIBUTES) {
        if (at & FILE_ATTRIBUTE_DIRECTORY) return false;
        out = direct;
        return true;
    }

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((direct + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out = folder;
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out += fd.cFileName;
        ok = true;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* ------------------------------------------------------------------ worker */
static LRESULT CALLBACK popupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI worker(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = popupProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PicoViewHoverPeek";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    g_popup = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        L"PicoViewHoverPeek", L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_popup) {
        note(L"hoverpeek: could not create the window");
        CoUninitialize();
        return 1;
    }

    Ref<IUIAutomation> ua;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&ua))) || !ua)
        note(L"hoverpeek: UI Automation is unavailable");

    POINT last = { -30000, -30000 };
    DWORD stillSince = 0;
    bool  asked = false;                 /* the resting point has been looked up */

    for (;;) {
        DWORD wait = MsgWaitForMultipleObjects(1, &g_quit, FALSE, 60, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        POINT pt;
        if (!GetCursorPos(&pt)) continue;

        /* Any click, and any keystroke that matters, takes the card away. */
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            hideCard();
            asked = true;                /* do not pop straight back up */
            continue;
        }

        int dx = pt.x - last.x, dy = pt.y - last.y;
        if (dx * dx + dy * dy > 16) {    /* moved more than a few pixels */
            last = pt;
            stillSince = GetTickCount();
            asked = false;
            if (g_shown) {
                /* Still on the same row? Cheap enough to re-ask only when the
                   pointer left the item's rectangle, which we approximate by
                   letting the next resting point decide. */
                HWND top = GetAncestor(WindowFromPoint(pt), GA_ROOT);
                if (!isExplorer(top)) hideCard();
            }
            continue;
        }
        if (asked) continue;
        if (GetTickCount() - stillSince < 320) continue;   /* let it settle */
        asked = true;

        HWND top = GetAncestor(WindowFromPoint(pt), GA_ROOT);
        if (!isExplorer(top)) { hideCard(); continue; }

        std::wstring name;
        if (!itemNameAt(ua.p, pt, name)) { hideCard(); continue; }

        std::wstring folder;
        if (!folderOfWindow(top, folder)) { hideCard(); continue; }

        std::wstring path;
        if (!resolvePath(folder, name, path)) { hideCard(); continue; }
        if (path == g_showing) continue;                   /* already up */

        int sw = 0, sh = 0;
        HBITMAP thumb = thumbnailFor(path, sw, sh);
        if (!thumb) { hideCard(); continue; }
        int tw = sw, th = sh;
        if (tw > kMaxThumb) { th = th * kMaxThumb / tw; tw = kMaxThumb; }
        if (th > kMaxThumb) { tw = tw * kMaxThumb / th; th = kMaxThumb; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        showCard(path, thumb, sw, sh, tw, th, pt);
        DeleteObject(thumb);
    }

    hideCard();
    if (g_popup) {
        DestroyWindow(g_popup);
        g_popup = nullptr;
    }
    UnregisterClassW(L"PicoViewHoverPeek", wc.hInstance);
    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------ plugin */
static int service_start(void) {
    if (g_thread) return PV_OK;
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_quit) return PV_ERR_MEMORY;
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_quit);
        g_quit = nullptr;
        return PV_ERR_MEMORY;
    }
    note(L"hoverpeek: watching Explorer");
    return PV_OK;
}

static void service_stop(void) {
    if (!g_thread) return;
    SetEvent(g_quit);
    WaitForSingleObject(g_thread, 4000);
    CloseHandle(g_thread);
    CloseHandle(g_quit);
    g_thread = nullptr;
    g_quit = nullptr;
    note(L"hoverpeek: stopped");
}

static void shutdown_(void) {
    service_stop();
    g_host = nullptr;
}

extern "C" __declspec(dllexport) int PicoViewPluginInit(const PvHost* host, PvPlugin* out) {
    if (!host || !out) return PV_ERR_FORMAT;
    if (host->abi != PV_ABI_VERSION) return PV_ERR_UNSUPPORTED;
    g_host = host;

    out->size = (uint32_t)sizeof(PvPlugin);
    out->caps = PV_CAP_SERVICE;
    out->name = L"Предперегляд у Провіднику";
    out->version = L"1.0";
    out->author = L"PicoView";
    out->description = L"Наведіть на файл у Провіднику - зʼявиться картка з ним";
    out->service_start = service_start;
    out->service_stop = service_stop;
    out->shutdown = shutdown_;
    return PV_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
