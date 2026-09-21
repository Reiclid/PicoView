// PicoView - ultralight portable image viewer for Windows 11
// Shared declarations.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d3d11_1.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include "core/coverplan.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>

using Microsoft::WRL::ComPtr;
using std::wstring;

// ---------------------------------------------------------------- messages
#define WM_PG_DECODED   (WM_APP + 1)   // a decode job finished
#define WM_PG_SCANNED   (WM_APP + 2)   // a folder scan finished
#define WM_PG_ANIM      (WM_APP + 3)
#define WM_PG_VIDEO     (WM_APP + 4)   // media engine event
#define WM_PG_COMPRESS  (WM_APP + 5)   // a compress/convert job finished
#define WM_PG_CONVERT   (WM_APP + 6)   // a media conversion finished

// ---------------------------------------------------------------- language
enum { LANG_UK = 0, LANG_EN = 1, LANG_RU = 2 };
extern std::atomic<int> g_lang;
void langInit();
// Translates a Ukrainian source string; returns it unchanged for Ukrainian or
// when a translation is missing.
const wchar_t* T(const wchar_t* uk);

// ---------------------------------------------------------------- helpers
inline float  clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline int    clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
double nowSec();

wstring lowerOf(wstring s);
wstring extOf(const wstring& path);      // ".jpg" lowercase, with dot
wstring fileNameOf(const wstring& path);
wstring stemOf(const wstring& path);
wstring dirOf(const wstring& path);
wstring joinPath(const wstring& dir, const wstring& name);
wstring humanSize(uint64_t bytes);
wstring humanTime(const FILETIME& ft);
wstring exePath();
wstring exeDir();
bool    fileExists(const wstring& p);
bool    dirExists(const wstring& p);
// Registers PicoView in the "Open with" list for the given extensions.
// HKCU only: Windows 11 does not let an app make itself the default silently.
bool    registerAssociations(const std::vector<wstring>& exts);
void    unregisterAssociations(const std::vector<wstring>& allExts);
// Windows 11 never lets an app make itself the default, so this opens the page
// where the user can do it - for one extension, or PicoView's own entry.
void    openDefaultAppsPage(const wstring& ext = wstring());
void    pgLog(const char* fmt, ...);   // active only when PG_DEBUG=1

// ---------------------------------------------------------------- theme
struct Theme {
    bool dark = true;
    D2D1_COLOR_F accent, accentHover, accentText;
    D2D1_COLOR_F text, textDim, textMute;
    D2D1_COLOR_F layer, card, cardHover, cardPress;
    D2D1_COLOR_F stroke, strokeStrong, focus;
    D2D1_COLOR_F canvas, chrome, bar, barStroke, shadow;
    D2D1_COLOR_F danger, dangerHover;
    void update(bool isDark, COLORREF sysAccent, float canvasAlpha);
};

bool systemUsesDarkMode();
COLORREF systemAccentColor();

// ---------------------------------------------------------------- images
enum class Fit { Window, Actual, Free };

struct ExifInfo {
    wstring camera, lens, exposure, aperture, iso, focal, taken, software;
    bool any = false;
};

// Raw CPU-side decode output: 32bpp premultiplied BGRA.
struct PixelBuf {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    int stride() const { return w * 4; }
    bool valid() const { return w > 0 && h > 0 && px.size() >= (size_t)w * h * 4; }
    void clear() { px.clear(); px.shrink_to_fit(); w = h = 0; }
};

enum class JobKind : uint8_t {
    Preview,     // fastest possible: embedded EXIF thumb / shell cache
    Full,        // full (or scaled) decode for the viewer
    Thumb        // grid / filmstrip thumbnail
};

struct DecodeJob {
    wstring  path;
    JobKind  kind = JobKind::Full;
    int      targetW = 0;        // 0 => native resolution
    int      targetH = 0;
    uint64_t generation = 0;     // stale jobs are dropped
    int      priority = 0;       // lower runs first
    uint64_t seq = 0;            // tie-breaker, set by the loader
    bool     wantExif = false;
};

struct DecodeResult {
    wstring   path;
    JobKind   kind = JobKind::Full;
    uint64_t  generation = 0;
    bool      ok = false;
    wstring   error;
    PixelBuf  img;
    int       srcW = 0, srcH = 0;   // true pixel size of the file
    bool      isFullRes = false;    // img is the native resolution
    bool      hasAlpha = false;
    ExifInfo  exif;
    uint64_t  fileSize = 0;
    FILETIME  mtime{};
    int       frameCount = 1;
    bool      generated = false;    // artwork we made up, not read from the file
    double    decodeMs = 0;
};

// ---------------------------------------------------------------- cover art
// What a track with no artwork of its own looks like: a hot core with petals
// of neighbouring hues leaning out of it, added together over black. Derived
// from the file name, so a song always comes back the same.
// The plan itself is platform-free and lives in core/, because three
// renderers draw it and none of them may disagree about what the picture is.
using CoverLobe = pv::CoverLobe;
using CoverPlan = pv::CoverPlan;
using pv::coverHsl;
using pv::coverFalloff;

void  coverPlan(const wstring& path, CoverPlan& out);
// The resting pose, rasterised for the thumbnail cache. Opaque BGRA, square.
void  coverRaster(const wstring& path, int size, PixelBuf& out);

// The same plan as a real object: spheres melted together, ray marched with a
// frosted translucent material, turning and swelling with the music. Rendered
// off-screen and handed to Direct2D as a bitmap, exactly like a video frame.
class BlobRenderer {
public:
    struct Impl;
    ~BlobRenderer();
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID2D1DeviceContext* d2d);
    void shutdown();
    bool ready() const;
    // Returns the square to draw, or null when the object is unavailable.
    ID2D1Bitmap1* frame(const CoverPlan& plan, int size, float time, float pulse, float level);

private:
    Impl* p_ = nullptr;
};

// ---------------------------------------------------------------- plugins
// Optional DLLs that teach the viewer a format it does not know, or offer to
// improve a picture that is already open. Nothing here costs anything until a
// `plugins` folder actually exists next to the executable.
struct PluginInfo {
    wstring  file;              // "upscale.dll" - the key the off-list uses
    wstring  dir, path;
    wstring  name, version, author, description;
    wstring  extensions;        // ".pcx,.ras", for decoders
    uint32_t caps = 0;          // PV_CAP_* from include/picoview_plugin.h
    bool     loaded = false;
    bool     enabled = true;
    wstring  error;             // why it is not loaded, if it is not
};
void pluginsInit(const wstring& disabled);      // comma separated file names
const std::vector<wstring>& pluginExtensions();
std::vector<PluginInfo> pluginsAll();
void pluginsSetEnabled(const wstring& file, bool on);
bool pluginsEnhanceAvailable();
wstring pluginsFolder();
wstring pluginsUserFolder();
bool pluginDecode(const wstring& path, PixelBuf& out, bool& hasAlpha);
// Take on a plugin that has just appeared on disk, or drop one that is about
// to be deleted. Both work while the program is running.
void pluginsAdopt(const wstring& dir, const wstring& file);
void pluginsForget(const wstring& file);
bool pluginsHave(const wstring& file);
// Starts and stops background-service plugins. Must be called from the thread
// that owns the window; returns immediately when nothing changed.
void pluginsTickServices();
bool pluginEnhance(const uint8_t* bgra, int w, int h, int stride, int scale,
                   PixelBuf& out, wstring& usedName);

// ---------------------------------------------------------------- plugin store
// One entry of the catalogue the project publishes. Fetched only when the
// plugins page is opened, never at startup.
struct StoreItem {
    wstring id, file, name, author, version, description, extensions, platform;
    wstring url, sha256;
    int     bytes = 0;
    int     status = 0;      // 0 offered, 1 downloading, 2 installed, 3 failed
    int     got = 0;         // bytes so far, while downloading
    wstring error;
};
void pluginStoreRefresh(bool force);
int  pluginStoreState();     // 0 idle, 1 fetching, 2 ready, 3 failed
bool pluginStoreBusy();
wstring pluginStoreError();
wstring pluginStoreSource();
std::vector<StoreItem> pluginStoreItems();
void pluginStoreInstall(const wstring& id);
void pluginStoreRemove(const wstring& id);

// ---------------------------------------------------------------- decoding
void     decodeInit();
bool     decodeIsSupported(const wstring& ext);
const std::vector<wstring>& decodeExtensions();
wstring  decodeFilterString();                       // for GetOpenFileName
void     runDecodeJob(const DecodeJob& job, DecodeResult& out, const std::atomic<uint64_t>& liveGen);

// ---------------------------------------------------------------- encoding
// The formats this machine can actually write. Probed once, on first use: the
// list depends on which codec packs are installed, and HEIF in particular is
// enumerated even when its encoder cannot be created.
struct EncFormat {
    wstring ext;              // ".jpg"
    wstring name;             // "JPEG"
    GUID    container{};
    bool    quality = false;  // honours an ImageQuality setting
    bool    alpha = false;    // can carry transparency
};
const std::vector<EncFormat>& encodeFormats();
int encodeFormatFor(const wstring& ext);     // index, or -1

enum class CompressMode {
    Quality,       // the user picks the encoder quality directly
    Percent,       // aim for a share of the original file size
    TargetBytes    // aim for an absolute file size
};

struct CompressJob {
    uint64_t     id = 0;
    wstring      path;             // source file
    wstring      outPath;          // empty => preview only, nothing is written
    int          format = 0;       // index into encodeFormats()
    CompressMode mode = CompressMode::Quality;
    float        quality = 0.82f;
    float        scale = 1.f;      // output pixels, 0.05..1 of the original
    double       target = 0;       // bytes (TargetBytes) or 0..1 share (Percent)
    bool         allowDownscale = true;   // shrink if quality alone cannot reach it
    int          cropX = 0, cropY = 0, cropW = 0, cropH = 0;   // 0 size => whole picture
    int          previewMax = 1400;       // preview long edge; 0 => skip the preview
    int          batchIndex = 0, batchTotal = 0;
};

struct CompressResult {
    uint64_t id = 0;
    bool     ok = false;
    wstring  error;
    wstring  path, outPath;
    uint64_t srcBytes = 0;
    int      srcW = 0, srcH = 0;
    uint64_t outBytes = 0;
    int      outW = 0, outH = 0;
    float    usedQuality = 0;
    float    usedScale = 1.f;
    bool     missedTarget = false;   // smallest achievable is still over the target
    bool     saved = false;
    PixelBuf preview;                // decoded back from the bytes actually written
    double   ms = 0;
    int      batchIndex = 0, batchTotal = 0;
};

// One worker thread. Preview requests are coalesced - only the newest survives,
// because the user is dragging a slider - while saves are a queue and are never
// dropped.
class Compressor {
public:
    struct Impl;              // the worker needs it; not part of the interface
    ~Compressor();
    void start(HWND notify);
    void stop();
    void request(CompressJob job);
    void save(CompressJob job);
    bool pop(CompressResult& out);
    bool busy() const;
    void cancelBatch();

private:
    Impl* p_ = nullptr;
};

// ---------------------------------------------------------------- converting
// Media formats this machine can write. Probed once: the encoders that exist
// depend on the edition of Windows and on whatever codec packs are installed.
struct MediaFormat {
    wstring ext;              // ".mp3"
    wstring name;             // "MP3"
    bool    video = false;    // carries a picture as well as sound
    GUID    container{};      // MFTranscodeContainerType_*
    GUID    audio{};          // MFAudioFormat_*
    GUID    vcodec{};         // MFVideoFormat_*, unused when video is false
    bool    bitrate = true;   // a bitrate setting means something
};
const std::vector<MediaFormat>& mediaFormats();
int mediaFormatFor(const wstring& ext, bool wantVideo);
// Whether this file's streams could go into that format untouched. A guess
// from what the shell knows, good enough to put a number on screen; the real
// answer is whether the copy attempt succeeds.
bool mediaCanCopy(const wstring& path, int format);
void mediaSourceAudio(const wstring& path, int& channels, int& sampleRate, int& kbps);

struct ConvertJob {
    uint64_t id = 0;
    wstring  path, outPath;
    int      format = 0;          // index into mediaFormats()
    int      audioKbps = 192;
    int      videoKbps = 4000;
    float    scale = 1.f;         // output frame size, 0.25..1 of the original
    bool     copyStreams = true;  // remux instead of re-encoding when possible
    int      batchIndex = 0, batchTotal = 0;
};

struct ConvertResult {
    uint64_t id = 0;
    bool     ok = false, saved = false;
    bool     copied = false;      // streams were copied, nothing re-encoded
    wstring  error, path, outPath;
    uint64_t srcBytes = 0, outBytes = 0;
    double   seconds = 0;         // duration of the media
    double   ms = 0;              // how long the conversion took
    int      batchIndex = 0, batchTotal = 0;
};

// One worker thread and a queue. Conversions are never dropped or coalesced:
// every one of them is a file the user asked for.
class MediaConverter {
public:
    struct Impl;
    ~MediaConverter();
    void  start(HWND notify);
    void  stop();
    void  submit(ConvertJob job);
    bool  pop(ConvertResult& out);
    bool  busy() const;
    float progress() const;       // 0..1 through the running job
    int   queued() const;
    void  cancel();

private:
    Impl* p_ = nullptr;
};

// ---------------------------------------------------------------- loader
class Loader {
public:
    void start(HWND notify);
    void stop();
    void submit(DecodeJob job);
    void bumpGeneration();                 // invalidate everything queued
    uint64_t generation() const { return gen_.load(std::memory_order_acquire); }
    bool pop(DecodeResult& out);           // drain finished results (UI thread)
    bool busy() const { return pending_.load(std::memory_order_relaxed) > 0; }

private:
    void worker();

    std::vector<std::thread>  threads_;
    std::deque<DecodeJob>     queue_;
    std::vector<DecodeResult> done_;
    std::mutex                qm_, dm_;
    std::condition_variable   cv_;
    std::atomic<bool>         quit_{ false };
    std::atomic<uint64_t>     gen_{ 1 };
    std::atomic<int>          pending_{ 0 };
    std::atomic<uint64_t>     seq_{ 0 };
    HWND                      notify_ = nullptr;
};

// Remembers where playback stopped, per file, across sessions. Keys are a hash
// of the path so arbitrary file names survive an INI round trip.
class ResumeStore {
public:
    void load(const wstring& iniFile);
    void save(const wstring& iniFile) const;
    void put(const wstring& path, double seconds);
    bool get(const wstring& path, double& seconds) const;
    void forget(const wstring& path);

private:
    std::unordered_map<wstring, double> map_;
    std::deque<wstring>                 order_;
};

// ---------------------------------------------------------------- video
const std::vector<wstring>& videoExtensions();
bool    isVideoExt(const wstring& ext);
bool    isVideoPath(const wstring& path);

// Music files play through the same engine; only the canvas differs.
const std::vector<wstring>& audioExtensions();
bool    isAudioExt(const wstring& ext);
bool    isAudioPath(const wstring& path);
inline bool isMediaExt(const wstring& ext)  { return isVideoExt(ext) || isAudioExt(ext); }
inline bool isMediaPath(const wstring& path) { return isVideoPath(path) || isAudioPath(path); }

wstring formatTime(double seconds);
void    readMediaProps(const wstring& path, std::vector<std::pair<wstring, wstring>>& out);

// Title / artist / album, as the shell already has them indexed.
struct AudioTags {
    wstring title, artist, album;
    int     track = 0, year = 0;
};
void readAudioTags(const wstring& path, AudioTags& out);

// What onEvent() wants the app to do next.
enum { PGV_REPAINT = 1, PGV_SIZED = 2, PGV_ENDED = 4, PGV_ERROR = 8, PGV_QUIET = 16 };

// Wraps the Media Foundation Media Engine: one open file, decoded on the GPU
// into a Direct2D bitmap we can draw exactly like a still image.
class VideoPlayer {
public:
    ~VideoPlayer();

    bool init(ID3D11Device* dev, HWND notify);
    void shutdown();

    bool open(const wstring& path, bool autoPlay = true);
    void close();
    unsigned onEvent(unsigned ev, uintptr_t param); // from WM_PG_VIDEO -> PGV_* flags

    bool isOpen() const;
    bool hasVideo() const;          // false for music: there is no picture
    bool hasFrame() const;
    bool metaKnown() const;
    int  width() const;
    int  height() const;
    wstring error() const;
    wstring path() const;

    bool   playing() const;
    bool   ended() const;
    double duration() const;
    double position() const;

    void play();
    void pause();
    void togglePlay();
    void seek(double sec);
    void seekBy(double delta);

    // Audio is held at zero across a seek or a fresh open, because the engine
    // emits a click on the first sample after a discontinuity.
    void  beginQuiet();
    void  endQuiet();

    void  setVolume(float v);
    float volume() const;
    void  setMuted(bool m);
    bool  muted() const;
    void   setRate(double r);
    double rate() const;

    ID2D1Bitmap1* frame(ID2D1DeviceContext* dc);

private:
    struct Impl;
    Impl* p_ = nullptr;
};

// How loud a track is over time, decoded once on a background thread, so
// something on screen can move with the music. Two numbers every 1/30 s.
class AudioEnvelope {
public:
    ~AudioEnvelope();
    void    open(const wstring& path);
    void    close();
    wstring path() const;
    bool    done() const;                 // the whole file has been read
    // Level and low end at that moment, each 0..1 against the loudest point
    // so far. False when the worker has not got that far yet.
    bool    at(double seconds, float& level, float& bass) const;

private:
    struct Impl;
    Impl* p_ = nullptr;
};

// How loud a file is overall, in LUFS - the broadcast measure, which is what
// makes two recordings comparable at all. Measured on a background thread from
// a sample of the file, so a two-hour film costs about as much as a song.
class LoudnessScan {
public:
    ~LoudnessScan();
    void    open(const wstring& path);
    void    close();
    wstring path() const;
    bool    ready() const;                       // the measurement is in
    bool    result(float& lufs, float& peak) const;

private:
    struct Impl;
    // Shared with the worker, so closing never has to wait for it: the thread
    // holds the last reference and lets go when it notices it is not wanted.
    std::shared_ptr<Impl> p_;
};

// Decodes single frames for the seek-bar thumbnail, independently of playback.
class VideoPreview {
public:
    ~VideoPreview();
    void    open(const wstring& path, int maxWidth);
    void    close();
    void    request(double seconds);                 // coalesced, newest wins
    bool    poll(PixelBuf& out, double& atSeconds);  // true when a new frame is ready
    bool    ready() const;
    wstring path() const;

private:
    struct Impl;
    Impl* p_ = nullptr;
};

// ---------------------------------------------------------------- folder
struct FileEntry {
    wstring  name;
    uint64_t size = 0;
    FILETIME mtime{};
};

enum class SortBy { Name, Date, Size, Type };

class ImageFolder {
public:
    void scan(const wstring& dir, SortBy by, bool desc);
    void resort(SortBy by, bool desc);
    int  indexOf(const wstring& fileName) const;
    const wstring& dir() const { return dir_; }
    size_t count() const { return files_.size(); }
    size_t mediaCount() const { return media_; }   // of those, audio or video
    const FileEntry& at(size_t i) const { return files_[i]; }
    wstring pathAt(size_t i) const { return joinPath(dir_, files_[i].name); }
    void removeAt(size_t i);

private:
    wstring                dir_;
    std::vector<FileEntry> files_;
    size_t                 media_ = 0;
};

// ---------------------------------------------------------------- graphics
struct Gfx {
    ComPtr<ID3D11Device>          d3d;
    ComPtr<ID3D11DeviceContext>   d3dCtx;
    ComPtr<IDXGISwapChain1>       swap;
    ComPtr<IDCompositionDevice>   dcomp;
    ComPtr<IDCompositionTarget>   dtarget;
    ComPtr<IDCompositionVisual>   dvisual;
    ComPtr<ID2D1Factory3>         d2dFactory;
    ComPtr<ID2D1Device2>          d2dDevice;
    ComPtr<ID2D1DeviceContext2>   dc;
    ComPtr<ID2D1Bitmap1>          target;
    ComPtr<IDWriteFactory3>       dw;
    ComPtr<ID2D1SolidColorBrush>  brush;

    ComPtr<IDWriteTextFormat> fCaption, fBody, fBodyStrong, fSmall, fTitle, fIcon, fIconBig, fIconSmall;

    HWND hwnd = nullptr;
    UINT dpi = 96;
    int  width = 0, height = 0;     // physical pixels
    bool composed = false;          // DirectComposition path active

    bool  init(HWND hwnd);
    static void warmStart();        // begin creating the D3D device right away
    void  shutdown();
    bool  resize(int w, int h);
    void  setDpi(UINT dpi);
    bool  begin();
    void  end();
    float s(float dip) const { return dip * (float)dpi / 96.0f; }   // dip -> px

    ID2D1SolidColorBrush* solid(const D2D1_COLOR_F& c) { brush->SetColor(c); return brush.Get(); }
    ComPtr<ID2D1Bitmap1> upload(const PixelBuf& buf);

    void text(const wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, const D2D1_COLOR_F& c,
              DWRITE_TEXT_ALIGNMENT ta = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_SIZE_F measure(const wstring& s, IDWriteTextFormat* f, float maxW = 4000.f);
    // A hint that may run onto a second line. Every format is created with
    // wrapping off, because nearly every label in the app is one line.
    D2D1_SIZE_F textWrap(const wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r,
                         const D2D1_COLOR_F& c);
    void roundRect(D2D1_RECT_F r, float radius, const D2D1_COLOR_F& fill);
    void pushRoundClip(D2D1_RECT_F r, float radius);
    void popRoundClip();
    void roundRectStroke(D2D1_RECT_F r, float radius, const D2D1_COLOR_F& c, float w);

    // The icon font draws some glyphs right out to the em box and others well
    // inside it - the window controls are half again the size of the speaker
    // at the same point size. Asking the font for each glyph's ink box and
    // scaling to a common size is what makes a row of icons look like one set.
    float iconScale(wchar_t ch);
    void  icon(const wchar_t* glyph, IDWriteTextFormat* f, D2D1_RECT_F r,
               const D2D1_COLOR_F& c, float extra = 1.f);

private:
    ComPtr<IDWriteFontFace>            iconFace_;
    bool                               iconFaceTried_ = false;
    std::unordered_map<wchar_t, float> iconScale_;
};

// ---------------------------------------------------------------- settings
struct Settings {
    int      themeMode = 0;        // 0 = follow system, 1 = dark, 2 = light
    int      viewMode = 0;         // 0 = viewer, 1 = grid
    int      thumbSize = 172;
    bool     filmstrip = false;
    bool     infoPanel = false;
    int      sortBy = 0;
    bool     sortDesc = false;
    int      wheelMode = 0;        // 0 = zoom, 1 = next/prev
    int      slideshowMs = 3500;
    bool     loopFolder = true;
    bool     smoothing = true;
    bool     rememberZoom = true;
    bool     singleInstance = true;
    int      barHideMs = 3000;     // idle before the overlays fade; -1 = never
    int      canvasDim = 55;       // 0..100 opacity of the photo backdrop
    int      backdrop = 3;         // 0 none, 1 mica, 2 acrylic, 3 blur-behind
    int      volume = 85;          // 0..100
    bool     muted = false;
    bool     autoPlay = true;
    int      autoSize = 1;         // 0 off, 1 picture to window, 2 window to picture
    int      autoSizeMax = 75;     // "window to picture": share of the screen it may take
    int      lang = -1;            // -1 = follow the system on first run
    RECT     placement{ 0,0,0,0 };
    bool     maximized = false;
    wstring  lastFolder;
    wstring  lastFile;
    wstring  associations;   // comma separated, e.g. ".jpg,.png"
    bool     fullscreen = false;
    float    lastZoom = 0;
    int      lastFit = 0, lastRot = 0, lastFlip = 0;
    bool     alwaysOnTop = false;
    bool     resumeVideo = true;
    int      volumeStep = 2;       // percent per Ctrl+wheel notch, 1..10
    bool     loudnessNorm = true;  // pull loud and quiet files to one level
    int      loudnessTarget = 1;   // 0 = -23, 1 = -19, 2 = -16 LUFS
    int      procPriority = 1;     // 0 = economy, 1 = normal, 2 = high
    wstring  pluginsOff;           // file names of the plugins switched off

    void load();
    void save() const;
    wstring file() const;
};

// ---------------------------------------------------------------- ui glue
struct UiInput {
    D2D1_POINT_2F mouse{ -1, -1 };
    bool  down = false;          // left button currently held
    bool  pressed = false;       // went down this frame
    bool  released = false;      // went up this frame
    bool  doubleClick = false;
    float wheel = 0;
    bool  hasMouse = false;
};

// Owned by main.cpp, used by ui.cpp.
struct App;

void uiFrame(App& app);
void uiLayout(App& app);
int  uiHitTestCaption(App& app, POINT ptClient);   // HTCAPTION / HTMAXBUTTON / HTCLIENT
void uiOnCommand(App& app, int cmd);

// Command ids shared between ui.cpp and main.cpp
enum {
    CMD_NONE = 0,
    CMD_PREV, CMD_NEXT, CMD_FIRST, CMD_LAST,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_FIT, CMD_ACTUAL,
    CMD_ROT_L, CMD_ROT_R, CMD_FLIP_H, CMD_FLIP_V,
    CMD_FULLSCREEN, CMD_SLIDESHOW, CMD_GRID, CMD_VIEWER, CMD_TOGGLE_VIEW,
    CMD_INFO, CMD_FILMSTRIP, CMD_DELETE, CMD_COPY, CMD_OPEN, CMD_OPEN_FOLDER,
    CMD_REVEAL, CMD_THEME, CMD_MINIMIZE, CMD_MAXIMIZE, CMD_CLOSE, CMD_SETWALLPAPER,
    CMD_SORT_NAME, CMD_SORT_DATE, CMD_SORT_SIZE, CMD_SORT_TYPE, CMD_SORT_DIR,
    CMD_PLAYPAUSE, CMD_MUTE, CMD_SEEK_BACK, CMD_SEEK_FWD, CMD_SPEED, CMD_STOP,
    CMD_AUTOSIZE, CMD_MORE, CMD_SETTINGS, CMD_NEWWINDOW, CMD_ASSOC_APPLY,
    CMD_ASSOC_CLEAR, CMD_ASSOC_WINDOWS, CMD_ASSOC_POPULAR, CMD_PIN,
    CMD_ESCAPE, CMD_PRINT, CMD_HELP, CMD_ROTATE_SAVE,
    CMD_COMPRESS, CMD_COMP_SAVE, CMD_COMP_SAVEAS, CMD_COMP_BATCH, CMD_COMP_CANCEL,
    CMD_CROP, CMD_CROP_APPLY, CMD_CROP_CANCEL, CMD_CROP_RESET,
    CMD_CONV_SAVE, CMD_CONV_SAVEAS, CMD_CONV_BATCH, CMD_CONV_CANCEL,
    CMD_LOUDNESS, CMD_ENHANCE, CMD_PLUGIN_FOLDER
};
