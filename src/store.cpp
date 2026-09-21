// The plugin catalogue: what can be installed, and installing it.
//
// The shape of this is deliberate. The program ships as one file and knows
// nothing about any plugin. When someone opens the plugins page, and only
// then, it fetches a small JSON list from the project's own repository and
// shows what is on offer. Pressing "install" downloads one DLL into the user's
// own folder, checks it against the hash in the list, and loads it there and
// then - no restart for anything except the list of file formats, which is
// settled once at startup by design.
//
// Downloading code and running it is exactly as serious as it sounds. Three
// things keep it honest: the catalogue is fetched over HTTPS from one fixed
// address that is compiled in, every file is refused unless its SHA-256
// matches what the catalogue said, and nothing is ever fetched without the
// user pressing the button.
//
// Nothing in this file runs unless the plugins page is opened. winhttp and
// bcrypt are both delay loaded, so a session that never opens it never even
// maps them.
#include "app.h"

#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>

// ------------------------------------------------------------------ json
// Just enough JSON for a catalogue: objects, arrays, strings, numbers, the
// three literals. No comments, no escapes beyond the common ones. A hundred
// lines beats a dependency for a file this program controls both ends of.
namespace {

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool         b = false;
    double       num = 0;
    std::string  str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* find(const char* key) const {
        if (kind != Obj) return nullptr;
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string s(const char* key) const {
        const Json* j = find(key);
        return (j && j->kind == Str) ? j->str : std::string();
    }
    double n(const char* key) const {
        const Json* j = find(key);
        return (j && j->kind == Num) ? j->num : 0.0;
    }
};

struct JsonReader {
    const char* p;
    const char* end;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end - p) < n || memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    bool value(Json& out) {
        if (++depth > 24) return false;              // a catalogue is not a tree
        ws();
        if (p >= end) return false;
        bool ok = false;
        switch (*p) {
            case '{': ok = object(out); break;
            case '[': ok = array(out); break;
            case '"': out.kind = Json::Str; ok = string(out.str); break;
            case 't': out.kind = Json::Bool; out.b = true;  ok = lit("true"); break;
            case 'f': out.kind = Json::Bool; out.b = false; ok = lit("false"); break;
            case 'n': out.kind = Json::Null; ok = lit("null"); break;
            default:  ok = number(out); break;
        }
        --depth;
        return ok;
    }

    bool string(std::string& s) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (end - p < 5) return false;
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            char c = p[i];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                            else return false;
                        }
                        p += 4;
                        // Straight to UTF-8; surrogate pairs are not expected in
                        // a catalogue and are written as real characters anyway.
                        if (cp < 0x80) s += (char)cp;
                        else if (cp < 0x800) {
                            s += (char)(0xC0 | (cp >> 6));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else {
                            s += (char)(0xE0 | (cp >> 12));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: s += *p; break;
                }
                ++p;
            } else {
                s += *p++;
            }
            if (s.size() > 1u << 20) return false;
        }
        if (p >= end) return false;
        ++p;
        return true;
    }

    bool number(Json& out) {
        const char* s = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '-' || *p == '+')) { ++p; any = true; }
        if (!any) return false;
        out.kind = Json::Num;
        out.num = atof(std::string(s, p - s).c_str());
        return true;
    }

    bool array(Json& out) {
        out.kind = Json::Arr;
        ++p;                                          // [
        ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            Json v;
            if (!value(v)) return false;
            out.arr.push_back(std::move(v));
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return false;
        }
    }

    bool object(Json& out) {
        out.kind = Json::Obj;
        ++p;                                          // {
        ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p >= end || *p != ':') return false;
            ++p;
            Json v;
            if (!value(v)) return false;
            out.obj.push_back({ std::move(key), std::move(v) });
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return false;
        }
    }
};

bool jsonParse(const std::vector<uint8_t>& bytes, Json& out) {
    if (bytes.empty()) return false;
    size_t at = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) at = 3;
    JsonReader r{ (const char*)bytes.data() + at, (const char*)bytes.data() + bytes.size() };
    return r.value(out);
}

wstring wide(const std::string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)std::max(0, n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ------------------------------------------------------------------ http
// One GET, following redirects, with a ceiling on the size so a wrong URL
// cannot fill the disk. Progress is reported so the button can show it.
bool httpGet(const wstring& url, std::vector<uint8_t>& out,
             std::atomic<int>* gotBytes, std::atomic<int>* totalBytes,
             const std::atomic<bool>* cancel, wstring& err) {
    out.clear();
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;      uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;       uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = T(L"Хибна адреса"); return false; }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { err = T(L"Лише HTTPS"); return false; }

    HINTERNET ses = WinHttpOpen(L"PicoView/1.3", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = T(L"Немає доступу до мережі"); return false; }
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 30000);

    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                if (code != 200) {
                    wchar_t b[64];
                    swprintf(b, 64, T(L"Сервер відповів %lu"), code);
                    err = b;
                } else {
                    DWORD clen = 0;
                    len = sizeof(clen);
                    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &clen, &len,
                                            WINHTTP_NO_HEADER_INDEX) && totalBytes)
                        totalBytes->store((int)clen);

                    const size_t kMax = 64u * 1024 * 1024;
                    ok = true;
                    for (;;) {
                        if (cancel && cancel->load()) { ok = false; err = T(L"Скасовано"); break; }
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                        if (!avail) break;
                        size_t at = out.size();
                        if (at + avail > kMax) { ok = false; err = T(L"Файл завеликий"); break; }
                        out.resize(at + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, out.data() + at, avail, &got)) { ok = false; break; }
                        out.resize(at + got);
                        if (gotBytes) gotBytes->store((int)out.size());
                    }
                    if (!ok && err.empty()) err = T(L"Обрив завантаження");
                }
            } else {
                err = T(L"Не вдалося зʼєднатися");
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    } else {
        err = T(L"Не вдалося зʼєднатися");
    }
    WinHttpCloseHandle(ses);
    return ok && !out.empty();
}

// ------------------------------------------------------------------ hash
wstring sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return wstring();
    uint8_t digest[32] = {};
    NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return wstring();

    wstring hex;
    hex.reserve(64);
    for (uint8_t b : digest) {
        const wchar_t* d = L"0123456789abcdef";
        hex += d[b >> 4];
        hex += d[b & 15];
    }
    return hex;
}

// ------------------------------------------------------------------ state
std::mutex              g_m;
std::vector<StoreItem>  g_items;
std::atomic<int>        g_state{ 0 };     // 0 idle, 1 fetching, 2 ready, 3 failed
wstring                 g_error;
std::atomic<bool>       g_busy{ false };  // a download is in flight

}  // namespace

// The one address this program will fetch from. Compiled in on purpose: a
// catalogue URL that could be pointed elsewhere by a setting would be a
// standing invitation.
static const wchar_t* kCatalogUrl =
    L"https://raw.githubusercontent.com/Reiclid/PicoView/main/plugins/catalog.json";

int  pluginStoreState() { return g_state.load(); }
bool pluginStoreBusy() { return g_busy.load(); }

wstring pluginStoreError() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_error;
}

std::vector<StoreItem> pluginStoreItems() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_items;
}

wstring pluginStoreSource() { return kCatalogUrl; }

void pluginStoreRefresh(bool force) {
    int st = g_state.load();
    if (st == 1) return;
    if (st == 2 && !force) return;
    g_state.store(1);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_error.clear();
    }

    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<uint8_t> body;
        wstring err;
        std::vector<StoreItem> items;
        bool ok = httpGet(kCatalogUrl, body, nullptr, nullptr, nullptr, err);

        Json root;
        if (ok && !jsonParse(body, root)) {
            ok = false;
            err = T(L"Каталог пошкоджено");
        }
        if (ok) {
            const Json* list = root.find("plugins");
            if (!list || list->kind != Json::Arr) {
                ok = false;
                err = T(L"Каталог порожній");
            } else {
                for (const Json& e : list->arr) {
                    if (e.kind != Json::Obj) continue;
                    StoreItem it;
                    it.id = wide(e.s("id"));
                    it.file = wide(e.s("file"));
                    it.name = wide(e.s("name"));
                    it.author = wide(e.s("author"));
                    it.version = wide(e.s("version"));
                    it.description = wide(e.s("description"));
                    it.extensions = wide(e.s("extensions"));
                    it.platform = wide(e.s("platform"));
                    it.url = wide(e.s("url"));
                    it.sha256 = lowerOf(wide(e.s("sha256")));
                    it.bytes = (int)e.n("size");
                    // Only what runs here. The same catalogue serves the Linux
                    // build, which reads the rows marked for it instead.
                    if (it.platform != L"win-x64") continue;
                    if (it.id.empty() || it.file.empty() || it.url.empty() || it.sha256.size() != 64)
                        continue;
                    items.push_back(it);
                }
                if (items.empty()) err = T(L"Для цієї системи додатків немає");
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_m);
            g_items = std::move(items);
            g_error = err;
        }
        g_state.store(ok ? 2 : 3);
        pgLog("plugin store: %s (%d items)", ok ? "ok" : "failed", (int)pluginStoreItems().size());
        if (g_app) { g_app->requestAnim(); g_app->invalidate(); }
    }).detach();
}

// Downloading, checking and loading, in that order. The file is written under
// its final name only after the hash matches, so a broken download never
// leaves something loadable behind.
void pluginStoreInstall(const wstring& id) {
    if (g_busy.exchange(true)) return;

    StoreItem want;
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (auto& it : g_items)
            if (it.id == id) { it.status = 1; it.got = 0; it.error.clear(); want = it; }
    }
    if (want.id.empty()) { g_busy.store(false); return; }

    std::thread([want] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto note = [&](int status, const wstring& e, int got) {
            std::lock_guard<std::mutex> lk(g_m);
            for (auto& it : g_items)
                if (it.id == want.id) { it.status = status; it.error = e; it.got = got; }
        };

        std::atomic<int> got{ 0 }, total{ want.bytes };
        std::vector<uint8_t> body;
        wstring err;

        // Progress has to reach the interface while this runs, so the counter
        // is copied over on a timer rather than at the end.
        std::atomic<bool> done{ false };
        std::thread ticker([&] {
            while (!done.load()) {
                note(1, wstring(), got.load());
                if (g_app) { g_app->requestAnim(); g_app->invalidate(); }
                Sleep(120);
            }
        });

        bool ok = httpGet(want.url, body, &got, &total, nullptr, err);
        done.store(true);
        ticker.join();

        if (ok) {
            wstring have = sha256Hex(body);
            if (have != want.sha256) {
                ok = false;
                err = T(L"Не збігається контрольна сума");
                pgLog("plugin store: hash mismatch for %ls", want.file.c_str());
            }
        }

        wstring dir = pluginsUserFolder();
        if (ok && !dir.empty()) {
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            wstring tmp = joinPath(dir, want.file + L".part");
            wstring fin = joinPath(dir, want.file);
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                ok = false;
                err = T(L"Не вдалося записати файл");
            } else {
                DWORD wrote = 0;
                ok = WriteFile(h, body.data(), (DWORD)body.size(), &wrote, nullptr) &&
                     wrote == body.size();
                CloseHandle(h);
                if (ok) {
                    DeleteFileW(fin.c_str());
                    ok = MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                }
                if (!ok) { DeleteFileW(tmp.c_str()); err = T(L"Не вдалося записати файл"); }
            }
        } else if (ok) {
            ok = false;
            err = T(L"Немає куди встановити");
        }

        if (ok) {
            // Loading it here means it works immediately. New file formats are
            // the exception: that list is built once at startup.
            pluginsAdopt(dir, want.file);
            note(2, wstring(), (int)body.size());
            pgLog("plugin store: installed %ls (%d bytes)", want.file.c_str(), (int)body.size());
        } else {
            note(3, err, 0);
        }
        g_busy.store(false);
        if (g_app) { g_app->requestAnim(); g_app->invalidate(); }
    }).detach();
}

void pluginStoreRemove(const wstring& id) {
    wstring file;
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (auto& it : g_items)
            if (it.id == id) { file = it.file; it.status = 0; it.error.clear(); }
    }
    if (file.empty()) return;
    pluginsForget(file);
    DeleteFileW(joinPath(pluginsUserFolder(), file).c_str());
    pgLog("plugin store: removed %ls", file.c_str());
}
