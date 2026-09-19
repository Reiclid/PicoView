// Small helpers, theme palette, folder listing, portable settings.
#include "pg.h"
#include <cstdarg>
#include <cstdio>

// ------------------------------------------------------------------ basics
double nowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

wstring lowerOf(wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

wstring extOf(const wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t sl = path.find_last_of(L"\\/");
    if (dot == wstring::npos || (sl != wstring::npos && dot < sl)) return L"";
    return lowerOf(path.substr(dot));
}

wstring fileNameOf(const wstring& path) {
    size_t sl = path.find_last_of(L"\\/");
    return sl == wstring::npos ? path : path.substr(sl + 1);
}

wstring stemOf(const wstring& path) {
    wstring n = fileNameOf(path);
    size_t dot = n.find_last_of(L'.');
    return dot == wstring::npos ? n : n.substr(0, dot);
}

wstring dirOf(const wstring& path) {
    size_t sl = path.find_last_of(L"\\/");
    if (sl == wstring::npos) return L"";
    if (sl == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, sl);
}

wstring joinPath(const wstring& dir, const wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

wstring humanSize(uint64_t b) {
    const wchar_t* u[] = { T(L"Б"), T(L"КБ"), T(L"МБ"), T(L"ГБ"), T(L"ТБ") };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    swprintf(buf, 64, (i == 0) ? L"%.0f %s" : (v < 10 ? L"%.2f %s" : L"%.1f %s"), v, u[i]);
    return buf;
}

wstring humanTime(const FILETIME& ft) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return L"";
    FILETIME lf; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lf) || !FileTimeToSystemTime(&lf, &st)) return L"";
    wchar_t d[64] = {}, t[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return wstring(d) + L" " + t;
}

wstring exePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

wstring exeDir() { return dirOf(exePath()); }

bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void pgLog(const char* fmt, ...) {
    static int on = -1;
    if (on < 0) { char b[8] = {}; on = (GetEnvironmentVariableA("PG_DEBUG", b, 8) && b[0] == '1') ? 1 : 0; }
    if (!on) return;
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wstring path = joinPath(exeDir(), L"picoview.log");
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, msg, (DWORD)strlen(msg), &w, nullptr);
    static const char kEol[2] = { 0x0D, 0x0A };
    WriteFile(h, kEol, 2, &w, nullptr);
    CloseHandle(h);
}

// ------------------------------------------------------------------ resume store
static wstring resumeKey(const wstring& path) {
    // FNV-1a over the lower-cased path.
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : lowerOf(path)) {
        h ^= (uint64_t)c;
        h *= 1099511628211ull;
    }
    wchar_t buf[24];
    swprintf(buf, 24, L"K%016llX", (unsigned long long)h);
    return buf;
}

void ResumeStore::put(const wstring& path, double seconds) {
    wstring k = resumeKey(path);
    if (!map_.count(k)) order_.push_back(k);
    map_[k] = seconds;
    while (order_.size() > 120) {
        map_.erase(order_.front());
        order_.pop_front();
    }
}

bool ResumeStore::get(const wstring& path, double& seconds) const {
    auto it = map_.find(resumeKey(path));
    if (it == map_.end()) return false;
    seconds = it->second;
    return true;
}

void ResumeStore::forget(const wstring& path) {
    wstring k = resumeKey(path);
    map_.erase(k);
    for (auto it = order_.begin(); it != order_.end(); ++it)
        if (*it == k) { order_.erase(it); break; }
}

void ResumeStore::load(const wstring& iniFile) {
    map_.clear();
    order_.clear();
    std::vector<wchar_t> buf(16384, 0);
    DWORD n = GetPrivateProfileSectionW(L"Resume", buf.data(), (DWORD)buf.size(), iniFile.c_str());
    if (!n) return;
    const wchar_t* p = buf.data();
    while (*p) {
        wstring entry = p;
        size_t eq = entry.find(L'=');
        if (eq != wstring::npos && eq > 0) {
            wstring k = entry.substr(0, eq);
            double v = _wtof(entry.c_str() + eq + 1);
            if (v > 0) { map_[k] = v; order_.push_back(k); }
        }
        p += entry.size() + 1;
    }
}

void ResumeStore::save(const wstring& iniFile) const {
    wstring block;
    for (const auto& k : order_) {
        auto it = map_.find(k);
        if (it == map_.end()) continue;
        wchar_t v[32];
        swprintf(v, 32, L"%.1f", it->second);
        block += k + L"=" + v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    WritePrivateProfileSectionW(L"Resume", block.c_str(), iniFile.c_str());
}

// ------------------------------------------------------------------ associations
static bool regSetString(const wstring& sub, const wchar_t* name, const wstring& val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                            (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static const wchar_t* kAppKey = L"Software\\Classes\\Applications\\PicoView.exe";
static const wchar_t* kCapKey = L"Software\\PicoView\\Capabilities";

// ProgID for one extension, e.g. ".jpg" -> "PicoView.jpg".
static wstring progIdFor(const wstring& ext) {
    return L"PicoView" + ext;
}

bool registerAssociations(const std::vector<wstring>& exts) {
    wstring exe = exePath();
    if (exe.empty()) return false;
    wstring cmd = L"\"" + exe + L"\" \"%1\"";
    wstring icon = L"\"" + exe + L"\",0";

    // 1. The "Open with" entry. This part always works, no matter what the
    //    shell decides about defaults.
    regSetString(kAppKey, L"FriendlyAppName", L"PicoView");
    regSetString(wstring(kAppKey) + L"\\shell\\open\\command", nullptr, cmd);
    regSetString(wstring(kAppKey) + L"\\DefaultIcon", nullptr, icon);

    RegDeleteTreeW(HKEY_CURRENT_USER, (wstring(kAppKey) + L"\\SupportedTypes").c_str());
    for (const auto& e : exts) {
        regSetString(wstring(kAppKey) + L"\\SupportedTypes", e.c_str(), L"");
        HKEY k = nullptr;
        wstring ow = L"Software\\Classes\\" + e + L"\\OpenWithList\\PicoView.exe";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, ow.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
            == ERROR_SUCCESS) RegCloseKey(k);
    }

    // 2. A ProgID per extension plus a Capabilities block under
    //    RegisteredApplications. This is the only registration Windows 11
    //    accepts: it cannot be made the default silently, but it is what puts
    //    PicoView in Settings -> Default apps as a real entry, where one click
    //    per type (or "Set default") hands the type over.
    RegDeleteTreeW(HKEY_CURRENT_USER, (wstring(kCapKey) + L"\\FileAssociations").c_str());
    regSetString(kCapKey, L"ApplicationName", L"PicoView");
    regSetString(kCapKey, L"ApplicationDescription",
                 T(L"Швидкий переглядач зображень і відео"));
    regSetString(kCapKey, L"ApplicationIcon", icon);

    for (const auto& e : exts) {
        wstring prog = progIdFor(e);
        wstring pk = L"Software\\Classes\\" + prog;
        wstring label = e.size() > 1 ? e.substr(1) : e;
        for (auto& c : label) if (c >= L'a' && c <= L'z') c -= 32;   // extensions are ASCII
        regSetString(pk, nullptr, label + L" \u2014 PicoView");
        regSetString(pk, L"FriendlyTypeName", label + L" \u2014 PicoView");
        regSetString(pk + L"\\DefaultIcon", nullptr, icon);
        regSetString(pk + L"\\shell\\open", L"FriendlyAppName", L"PicoView");
        regSetString(pk + L"\\shell\\open\\command", nullptr, cmd);
        regSetString(wstring(kCapKey) + L"\\FileAssociations", e.c_str(), prog);
    }
    regSetString(L"Software\\RegisteredApplications", L"PicoView", kCapKey);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

void unregisterAssociations(const std::vector<wstring>& allExts) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\PicoView");
    for (const auto& e : allExts) {
        wstring ow = L"Software\\Classes\\" + e + L"\\OpenWithList\\PicoView.exe";
        RegDeleteTreeW(HKEY_CURRENT_USER, ow.c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progIdFor(e)).c_str());
    }
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_WRITE, &k)
        == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"PicoView");
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Opens the Windows page where the hand-over actually happens. `ext` lands on
// the picker for that one type; empty lands on PicoView's own page.
void openDefaultAppsPage(const wstring& ext) {
    wstring uri = L"ms-settings:defaultapps";
    if (!ext.empty()) uri += L"?ftprint=" + ext;
    else              uri += L"?registeredAppUser=PicoView";
    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------------ theme
static D2D1_COLOR_F rgba(uint32_t hex, float a = 1.0f) {
    return D2D1::ColorF(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a);
}

bool systemUsesDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

COLORREF systemAccentColor() {
    // AccentPalette: 8 RGBA entries, dark3..light3. Index 5 (Light2) is what
    // Windows 11 uses for accents on dark surfaces, index 3 is the base.
    BYTE pal[32] = {};
    DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS && sz >= 32) {
        int i = systemUsesDarkMode() ? 20 : 12;
        return RGB(pal[i], pal[i + 1], pal[i + 2]);
    }
    DWORD col = 0; BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque)))
        return RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
    return systemUsesDarkMode() ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);
}

void Theme::update(bool isDark, COLORREF sysAccent, float canvasAlpha) {
    dark = isDark;
    float ar = GetRValue(sysAccent) / 255.f, ag = GetGValue(sysAccent) / 255.f, ab = GetBValue(sysAccent) / 255.f;

    // Some accent colours are far too dark (or too light) to read against our
    // surfaces, so nudge them until they carry enough contrast either way.
    auto lum = [&] { return 0.2126f * ar + 0.7152f * ag + 0.0722f * ab; };
    auto toward = [&](float target, float t) {
        ar += (target - ar) * t; ag += (target - ag) * t; ab += (target - ab) * t;
    };
    for (int i = 0; i < 8 && isDark && lum() < 0.52f; ++i) toward(1.f, 0.22f);
    for (int i = 0; i < 8 && !isDark && lum() > 0.46f; ++i) toward(0.f, 0.22f);

    accent = D2D1::ColorF(ar, ag, ab, 1.f);
    accentHover = D2D1::ColorF(clampf(ar * 1.12f, 0, 1), clampf(ag * 1.12f, 0, 1), clampf(ab * 1.12f, 0, 1), 1.f);

    if (dark) {
        accentText = rgba(0x000000);
        text = rgba(0xFFFFFF, 0.93f);
        textDim = rgba(0xFFFFFF, 0.64f);
        textMute = rgba(0xFFFFFF, 0.40f);
        layer = rgba(0x202020, 0.72f);
        card = rgba(0xFFFFFF, 0.061f);
        cardHover = rgba(0xFFFFFF, 0.094f);
        cardPress = rgba(0xFFFFFF, 0.036f);
        stroke = rgba(0xFFFFFF, 0.070f);
        strokeStrong = rgba(0xFFFFFF, 0.14f);
        focus = rgba(0xFFFFFF, 0.90f);
        canvas = rgba(0x0A0A0A, canvasAlpha);
        chrome = rgba(0x1F1F1F, 0.0f);          // transparent -> Mica shows through
        bar = rgba(0x2B2B2B, 0.86f);
        barStroke = rgba(0xFFFFFF, 0.09f);
        shadow = rgba(0x000000, 0.55f);
        danger = rgba(0xC42B1C);
        dangerHover = rgba(0xD13438);
    } else {
        accentText = rgba(0xFFFFFF);
        text = rgba(0x191919, 0.96f);
        textDim = rgba(0x000000, 0.61f);
        textMute = rgba(0x000000, 0.40f);
        layer = rgba(0xFFFFFF, 0.70f);
        card = rgba(0xFFFFFF, 0.70f);
        cardHover = rgba(0xFFFFFF, 0.90f);
        cardPress = rgba(0xF9F9F9, 0.55f);
        stroke = rgba(0x000000, 0.065f);
        strokeStrong = rgba(0x000000, 0.16f);
        focus = rgba(0x000000, 0.90f);
        canvas = rgba(0x101010, canvasAlpha * 0.72f);   // photos read better on a dark canvas
        chrome = rgba(0xF3F3F3, 0.0f);
        bar = rgba(0xF9F9F9, 0.90f);
        barStroke = rgba(0x000000, 0.08f);
        shadow = rgba(0x000000, 0.28f);
        danger = rgba(0xC42B1C);
        dangerHover = rgba(0xD13438);
    }
}

// ------------------------------------------------------------------ folder
static SortBy  g_sortBy = SortBy::Name;
static bool    g_sortDesc = false;

static bool entryLess(const FileEntry& a, const FileEntry& b) {
    int c = 0;
    switch (g_sortBy) {
        case SortBy::Date:
            c = CompareFileTime(&a.mtime, &b.mtime);
            break;
        case SortBy::Size:
            c = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            break;
        case SortBy::Type: {
            wstring ea = extOf(a.name), eb = extOf(b.name);
            c = ea.compare(eb) < 0 ? -1 : (ea == eb ? 0 : 1);
            break;
        }
        default: break;
    }
    if (c == 0) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
    return g_sortDesc ? (c > 0) : (c < 0);
}

void ImageFolder::scan(const wstring& dir, SortBy by, bool desc) {
    dir_ = dir;
    files_.clear();
    if (dir.empty()) return;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    files_.reserve(512);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        wstring ext = extOf(fd.cFileName);
        if (!decodeIsSupported(ext) && !isMediaExt(ext)) continue;
        FileEntry e;
        e.name = fd.cFileName;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        files_.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    resort(by, desc);
}

void ImageFolder::resort(SortBy by, bool desc) {
    g_sortBy = by; g_sortDesc = desc;
    std::sort(files_.begin(), files_.end(), entryLess);
}

int ImageFolder::indexOf(const wstring& fileName) const {
    for (size_t i = 0; i < files_.size(); ++i)
        if (_wcsicmp(files_[i].name.c_str(), fileName.c_str()) == 0) return (int)i;
    return -1;
}

void ImageFolder::removeAt(size_t i) {
    if (i < files_.size()) files_.erase(files_.begin() + i);
}

// ------------------------------------------------------------------ settings
wstring Settings::file() const {
    // Portable first: keep the ini next to the exe when that folder is writable.
    wstring local = joinPath(exeDir(), L"PicoView.ini");
    HANDLE h = CreateFileW(local.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return local; }

    PWSTR appData = nullptr;
    wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) {
        dir = joinPath(appData, L"PicoView");
        CoTaskMemFree(appData);
        CreateDirectoryW(dir.c_str(), nullptr);
        return joinPath(dir, L"PicoView.ini");
    }
    return local;
}

static int iniInt(const wstring& f, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"PicoView", key, def, f.c_str());
}

// A UTF-8 BOM in front of "[PicoView]" makes the profile API miss the section
// and quietly hand back every default - which is easy to hit, because Notepad
// and PowerShell both write one. Take it off before reading.
static void stripBom(const wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    unsigned char bom[3] = {};
    DWORD got = 0;
    if (ReadFile(h, bom, 3, &got, nullptr) && got == 3 &&
        bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 3 && size.QuadPart < (1 << 22)) {
            std::vector<char> rest((size_t)size.QuadPart - 3);
            DWORD read = 0;
            if (ReadFile(h, rest.data(), (DWORD)rest.size(), &read, nullptr) && read == rest.size()) {
                SetFilePointer(h, 0, nullptr, FILE_BEGIN);
                DWORD wrote = 0;
                WriteFile(h, rest.data(), read, &wrote, nullptr);
                SetEndOfFile(h);
            }
        }
    }
    CloseHandle(h);
}

void Settings::load() {
    wstring f = file();
    if (!fileExists(f)) return;
    stripBom(f);
    themeMode = clampi(iniInt(f, L"ThemeMode", themeMode), 0, 2);
    viewMode = clampi(iniInt(f, L"ViewMode", viewMode), 0, 1);
    thumbSize = clampi(iniInt(f, L"ThumbSize", thumbSize), 96, 420);
    filmstrip = iniInt(f, L"Filmstrip", filmstrip) != 0;
    infoPanel = iniInt(f, L"InfoPanel", infoPanel) != 0;
    sortBy = clampi(iniInt(f, L"SortBy", sortBy), 0, 3);
    sortDesc = iniInt(f, L"SortDesc", sortDesc) != 0;
    wheelMode = clampi(iniInt(f, L"WheelMode", wheelMode), 0, 1);
    slideshowMs = clampi(iniInt(f, L"SlideshowMs", slideshowMs), 500, 60000);
    loopFolder = iniInt(f, L"LoopFolder", loopFolder) != 0;
    smoothing = iniInt(f, L"Smoothing", smoothing) != 0;
    singleInstance = iniInt(f, L"SingleInstance", singleInstance) != 0;
    barHideMs = clampi(iniInt(f, L"BarHideMs", barHideMs), -1, 10000);
    canvasDim = clampi(iniInt(f, L"CanvasDim", canvasDim), 0, 100);
    backdrop = clampi(iniInt(f, L"Backdrop", backdrop), 0, 3);
    volume = clampi(iniInt(f, L"Volume", volume), 0, 100);
    muted = iniInt(f, L"Muted", muted) != 0;
    autoPlay = iniInt(f, L"AutoPlay", autoPlay) != 0;
    autoSize = clampi(iniInt(f, L"AutoSize", autoSize), 0, 2);
    autoSizeMax = clampi(iniInt(f, L"AutoSizeMax", autoSizeMax), 30, 100);
    lang = iniInt(f, L"Lang", -1);
    if (lang < 0 || lang > 2) {
        WORD p = PRIMARYLANGID(GetUserDefaultUILanguage());
        lang = (p == LANG_UKRAINIAN) ? LANG_UK : (p == LANG_RUSSIAN) ? LANG_RU : LANG_EN;
    }
    rememberZoom = iniInt(f, L"RememberZoom", rememberZoom) != 0;
    alwaysOnTop = iniInt(f, L"AlwaysOnTop", alwaysOnTop) != 0;
    resumeVideo = iniInt(f, L"ResumeVideo", resumeVideo) != 0;
    { wchar_t ab[4096] = {};
      GetPrivateProfileStringW(L"PicoView", L"Associations", L"", ab, 4096, f.c_str());
      associations = ab; }
    fullscreen = iniInt(f, L"Fullscreen", fullscreen) != 0;
    lastZoom = clampi(iniInt(f, L"LastZoom", 0), 0, 6400) / 100.f;
    lastFit = clampi(iniInt(f, L"LastFit", 0), 0, 2);
    lastRot = clampi(iniInt(f, L"LastRot", 0), 0, 3);
    lastFlip = clampi(iniInt(f, L"LastFlip", 0), 0, 3);
    maximized = iniInt(f, L"Maximized", maximized) != 0;
    placement.left = iniInt(f, L"WinX", 0);
    placement.top = iniInt(f, L"WinY", 0);
    placement.right = iniInt(f, L"WinW", 0);
    placement.bottom = iniInt(f, L"WinH", 0);

    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"PicoView", L"LastFolder", L"", buf, MAX_PATH * 2, f.c_str());
    lastFolder = buf;
    GetPrivateProfileStringW(L"PicoView", L"LastFile", L"", buf, MAX_PATH * 2, f.c_str());
    lastFile = buf;
}

void Settings::save() const {
    wstring f = file();
    auto put = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(L"PicoView", k, std::to_wstring(v).c_str(), f.c_str());
    };
    put(L"ThemeMode", themeMode);
    put(L"ViewMode", viewMode);
    put(L"ThumbSize", thumbSize);
    put(L"Filmstrip", filmstrip);
    put(L"InfoPanel", infoPanel);
    put(L"SortBy", sortBy);
    put(L"SortDesc", sortDesc);
    put(L"WheelMode", wheelMode);
    put(L"SlideshowMs", slideshowMs);
    put(L"LoopFolder", loopFolder);
    put(L"Smoothing", smoothing);
    put(L"SingleInstance", singleInstance);
    put(L"BarHideMs", barHideMs);
    put(L"CanvasDim", canvasDim);
    put(L"Backdrop", backdrop);
    put(L"Volume", volume);
    put(L"Muted", muted);
    put(L"AutoPlay", autoPlay);
    put(L"AutoSize", autoSize);
    put(L"AutoSizeMax", autoSizeMax);
    put(L"Lang", lang);
    put(L"RememberZoom", rememberZoom);
    put(L"AlwaysOnTop", alwaysOnTop);
    put(L"ResumeVideo", resumeVideo);
    put(L"Fullscreen", fullscreen);
    put(L"LastZoom", (int)lround(lastZoom * 100.f));
    put(L"LastFit", lastFit);
    put(L"LastRot", lastRot);
    put(L"LastFlip", lastFlip);
    put(L"Maximized", maximized);
    put(L"WinX", placement.left);
    put(L"WinY", placement.top);
    put(L"WinW", placement.right);
    put(L"WinH", placement.bottom);
    WritePrivateProfileStringW(L"PicoView", L"LastFolder", lastFolder.c_str(), f.c_str());
    WritePrivateProfileStringW(L"PicoView", L"LastFile", lastFile.c_str(), f.c_str());
    WritePrivateProfileStringW(L"PicoView", L"Associations", associations.c_str(), f.c_str());
}
