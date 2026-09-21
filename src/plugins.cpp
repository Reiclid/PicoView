// Loading other people's code into this process.
//
// The rule that shapes everything here is that a machine with no plugins must
// not pay for the feature. The whole subsystem is one test of whether a
// `plugins` folder exists; if it does not, nothing else in this file ever
// runs and startup is exactly what it was before.
//
// The second rule is that a plugin must not be able to take the viewer down
// with it. There is no sandbox - a DLL in this process can do anything the
// process can - but every call across the boundary is guarded, a plugin that
// hands back nonsense is dropped rather than believed, and one that faults is
// switched off for the rest of the session instead of being asked again.
#include "app.h"
#include "../include/picoview_plugin.h"

#include <shlobj.h>

// ------------------------------------------------------------- the boundary
// Everything that calls into a plugin lives in these little functions, because
// a function using __try may not also need C++ unwinding. Raw pointers only.
namespace {

void hostLog(const wchar_t* m);
void hostToast(const wchar_t* m);
void* hostAlloc(size_t n) { return n ? calloc(1, n) : nullptr; }
void  hostFree(void* p) { free(p); }

const PvHost kHost = {
    (uint32_t)sizeof(PvHost), PV_ABI_VERSION, hostLog, hostToast, hostAlloc, hostFree
};

int callInit(PvPluginInit fn, PvPlugin* api) {
    __try { return fn(&kHost, api); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int callDecode(const PvPlugin* api, const wchar_t* path, PvImage* out) {
    __try { return api->decode(path, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int callEnhance(const PvPlugin* api, const PvImage* in, int scale, PvImage* out) {
    __try { return api->enhance(in, scale, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool callFree(const PvPlugin* api, PvImage* img) {
    if (!api->free_image) return true;
    __try { api->free_image(img); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void callShutdown(const PvPlugin* api) {
    if (!api->shutdown) return;
    __try { api->shutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
size_t safeLen(const wchar_t* s, size_t cap) {
    if (!s) return 0;
    __try {
        size_t n = 0;
        while (n < cap && s[n]) ++n;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// The plugin's rows, into ours. A plugin that lied about its height would walk
// off the end of its own buffer, which is its fault but our crash.
bool safeRows(uint8_t* dst, const uint8_t* src, int rows, int dstStride, int srcStride) {
    __try {
        for (int y = 0; y < rows; ++y)
            memcpy(dst + (size_t)y * dstStride, src + (size_t)y * srcStride, (size_t)dstStride);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Loaded {
    PluginInfo info;
    HMODULE    mod = nullptr;
    PvPlugin   api{};
    bool       broken = false;      // it faulted once; never called again
};

std::vector<Loaded>  g_plugins;
std::vector<wstring> g_exts;
std::mutex           g_call;        // plugins are not assumed to be re-entrant
bool                 g_scanned = false;
bool                 g_anyEnhance = false;
DWORD                g_uiThread = 0;
wstring              g_userDir;

// pgLog formats with the narrow runtime, and %ls there goes through the ANSI
// code page: one Cyrillic letter in a plugin's name and the whole line comes
// out empty. Everything wide is converted here instead.
std::string u8(const wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void hostLog(const wchar_t* m) {
    if (m) pgLog("plugin: %s", u8(m).c_str());
}
void hostToast(const wchar_t* m) {
    // Only from the thread that owns the window; a decoding worker saying this
    // would be writing to the interface from underneath it.
    if (m && g_app && GetCurrentThreadId() == g_uiThread) g_app->showToast(m);
    else hostLog(m);
}

wstring safeStr(const wchar_t* s, size_t cap = 400) {
    size_t n = safeLen(s, cap);
    return n ? wstring(s, n) : wstring();
}

wstring appDataPlugins() {
    if (!g_userDir.empty()) return g_userDir;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        g_userDir = joinPath(joinPath(p, L"PicoView"), L"plugins");
        CoTaskMemFree(p);
    }
    return g_userDir;
}

void splitExts(const wstring& list, std::vector<wstring>& out) {
    wstring cur;
    for (size_t i = 0; i <= list.size(); ++i) {
        wchar_t c = (i < list.size()) ? list[i] : 0;
        if (c == 0 || c == L',' || c == L';' || c == L' ') {
            if (!cur.empty()) {
                cur = lowerOf(cur);
                if (cur[0] != L'.') cur = L"." + cur;
                out.push_back(cur);
                cur.clear();
            }
        } else cur += c;
    }
}

void rebuildTables() {
    g_exts.clear();
    g_anyEnhance = false;
    for (const auto& p : g_plugins) {
        if (!p.info.loaded) continue;
        if (p.info.caps & PV_CAP_DECODE) splitExts(p.info.extensions, g_exts);
        if (p.info.caps & PV_CAP_ENHANCE) g_anyEnhance = true;
    }
    std::sort(g_exts.begin(), g_exts.end());
    g_exts.erase(std::unique(g_exts.begin(), g_exts.end()), g_exts.end());
}

bool isOff(const wstring& disabled, const wstring& file) {
    if (disabled.empty()) return false;
    return (L"," + lowerOf(disabled) + L",").find(L"," + lowerOf(file) + L",") != wstring::npos;
}

void loadOne(const wstring& dir, const wstring& file, const wstring& disabled) {
    for (const auto& p : g_plugins)
        if (_wcsicmp(p.info.file.c_str(), file.c_str()) == 0) return;   // same name, first wins

    Loaded L;
    L.info.file = file;
    L.info.dir = dir;
    L.info.path = joinPath(dir, file);
    L.info.enabled = !isOff(disabled, file);
    if (!L.info.enabled) { g_plugins.push_back(L); return; }

    L.mod = LoadLibraryExW(L.info.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!L.mod) {
        pgLog("plugin %s: LoadLibrary failed, err=%lu", u8(file).c_str(), GetLastError());
        L.info.error = T(L"Не вдалося завантажити DLL");
        g_plugins.push_back(L);
        return;
    }
    auto init = (PvPluginInit)GetProcAddress(L.mod, "PicoViewPluginInit");
    if (!init) {
        pgLog("plugin %s: no PicoViewPluginInit export", u8(file).c_str());
        L.info.error = T(L"Це не додаток PicoView");
        FreeLibrary(L.mod);
        g_plugins.push_back(L);
        return;
    }

    PvPlugin api{};
    api.size = (uint32_t)sizeof(api);
    int rc = callInit(init, &api);
    if (rc != PV_OK || !api.caps) {
        pgLog("plugin %s: init returned %d caps=%u", u8(file).c_str(), rc, api.caps);
        L.info.error = T(L"Додаток відмовився запускатися");
        FreeLibrary(L.mod);
        g_plugins.push_back(L);
        return;
    }

    L.api = api;
    L.info.loaded = true;
    L.info.caps = api.caps;
    L.info.name = safeStr(api.name);
    if (L.info.name.empty()) L.info.name = file;
    L.info.version = safeStr(api.version, 32);
    L.info.author = safeStr(api.author, 120);
    L.info.description = safeStr(api.description, 400);
    if ((api.caps & PV_CAP_DECODE) && api.decode) L.info.extensions = safeStr(api.extensions, 1024);
    if (!api.enhance) L.info.caps &= ~(uint32_t)PV_CAP_ENHANCE;
    if (!api.decode) L.info.caps &= ~(uint32_t)PV_CAP_DECODE;

    pgLog("plugin loaded '%s' v%s caps=%u ext=%s", u8(L.info.name).c_str(),
          u8(L.info.version).c_str(), L.info.caps, u8(L.info.extensions).c_str());
    g_plugins.push_back(L);
}

void scanDir(const wstring& dir, const wstring& disabled) {
    if (dir.empty()) return;
    DWORD at = GetFileAttributesW(dir.c_str());
    pgLog("plugin scan %s attr=%lx", u8(dir).c_str(), at);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*.dll").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        loadOne(dir, fd.cFileName, disabled);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool copyOut(const PvImage& img, PixelBuf& out) {
    if (img.format != PV_BGRA8 || !img.pixels) return false;
    if (img.width <= 0 || img.height <= 0) return false;
    if ((int64_t)img.width * (int64_t)img.height > 400ll * 1000 * 1000) return false;
    int stride = img.stride > 0 ? img.stride : img.width * 4;
    if (stride < img.width * 4) return false;

    out.w = img.width;
    out.h = img.height;
    out.px.assign((size_t)out.w * out.h * 4, 0);
    if (!safeRows(out.px.data(), (const uint8_t*)img.pixels, out.h, out.w * 4, stride)) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------ host api
void pluginsInit(const wstring& disabled) {
    if (g_scanned) return;
    g_scanned = true;
    g_uiThread = GetCurrentThreadId();
    scanDir(joinPath(exeDir(), L"plugins"), disabled);
    scanDir(appDataPlugins(), disabled);
    rebuildTables();
}

const std::vector<wstring>& pluginExtensions() { return g_exts; }
bool pluginsEnhanceAvailable() { return g_anyEnhance; }
wstring pluginsFolder() { return joinPath(exeDir(), L"plugins"); }
wstring pluginsUserFolder() { return appDataPlugins(); }

std::vector<PluginInfo> pluginsAll() {
    std::lock_guard<std::mutex> lk(g_call);
    std::vector<PluginInfo> v;
    v.reserve(g_plugins.size());
    for (const auto& p : g_plugins) v.push_back(p.info);
    return v;
}

// Switching one off unloads it there and then; switching one on loads it where
// it stands, so a DLL dropped into the folder while the viewer is open is one
// click away from working.
void pluginsSetEnabled(const wstring& file, bool on) {
    std::lock_guard<std::mutex> lk(g_call);
    for (size_t i = 0; i < g_plugins.size(); ++i) {
        if (_wcsicmp(g_plugins[i].info.file.c_str(), file.c_str()) != 0) continue;
        wstring dir = g_plugins[i].info.dir;
        if (g_plugins[i].mod) {
            callShutdown(&g_plugins[i].api);
            FreeLibrary(g_plugins[i].mod);
        }
        g_plugins.erase(g_plugins.begin() + i);
        loadOne(dir, file, on ? wstring() : file);
        rebuildTables();
        return;
    }
}

bool pluginDecode(const wstring& path, PixelBuf& out, bool& hasAlpha) {
    if (g_plugins.empty()) return false;
    wstring ext = lowerOf(extOf(path));
    if (ext.empty()) return false;

    std::lock_guard<std::mutex> lk(g_call);
    for (auto& p : g_plugins) {
        if (!p.info.loaded || p.broken || !p.api.decode) continue;
        if (!(p.info.caps & PV_CAP_DECODE)) continue;
        if (lowerOf(p.info.extensions).find(ext) == wstring::npos) continue;

        PvImage img{};
        int rc = callDecode(&p.api, path.c_str(), &img);
        if (rc == -1) {
            p.broken = true;
            pgLog("plugin %s faulted while decoding; switched off", u8(p.info.name).c_str());
            continue;
        }
        if (rc != PV_OK) continue;

        bool ok = copyOut(img, out);
        if (!callFree(&p.api, &img)) p.broken = true;
        if (ok) { hasAlpha = true; return true; }
    }
    return false;
}

bool pluginEnhance(const uint8_t* bgra, int w, int h, int stride, int scale,
                   PixelBuf& out, wstring& usedName) {
    if (!bgra || w <= 0 || h <= 0 || !g_anyEnhance) return false;

    PvImage in{};
    in.format = PV_BGRA8;
    in.width = w;
    in.height = h;
    in.stride = stride;
    in.pixels = (void*)bgra;

    std::lock_guard<std::mutex> lk(g_call);
    for (auto& p : g_plugins) {
        if (!p.info.loaded || p.broken || !p.api.enhance) continue;
        if (!(p.info.caps & PV_CAP_ENHANCE)) continue;

        PvImage img{};
        int rc = callEnhance(&p.api, &in, scale, &img);
        if (rc == -1) {
            p.broken = true;
            pgLog("plugin %s faulted while enhancing; switched off", u8(p.info.name).c_str());
            continue;
        }
        if (rc != PV_OK) continue;

        bool ok = copyOut(img, out);
        if (!callFree(&p.api, &img)) p.broken = true;
        if (ok) { usedName = p.info.name; return true; }
    }
    return false;
}
