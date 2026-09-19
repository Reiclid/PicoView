// Application state shared by main.cpp (windowing, input) and ui.cpp (drawing).
#pragma once
#include "pg.h"

struct Picture {
    wstring              path;
    ComPtr<ID2D1Bitmap1> bmp;
    int      w = 0, h = 0;          // bitmap pixels
    int      srcW = 0, srcH = 0;    // true pixels of the file
    bool     full = false;          // bmp is native resolution
    bool     preview = false;       // bmp is a low-res placeholder
    bool     hasAlpha = false;
    bool     failed = false;
    wstring  error;
    ExifInfo exif;
    bool     exifRead = false;
    uint64_t fileSize = 0;
    FILETIME mtime{};
    double   decodeMs = 0;
    double   shownAt = 0;
    int      frameCount = 1;
    bool     upgrading = false;     // a higher-res decode is in flight
    int      upgradeW = 0;
};

struct Thumb {
    ComPtr<ID2D1Bitmap1> bmp;
    int    w = 0, h = 0;
    bool   requested = false;
    bool   failed = false;
    bool   generated = false;       // artwork we made up for a track with none
    double arrivedAt = 0;
};

enum class View { Viewer, Grid };

struct Rects {
    D2D1_RECT_F client{};
    D2D1_RECT_F titlebar{};
    D2D1_RECT_F caption{};        // draggable part of the titlebar
    D2D1_RECT_F btnMin{}, btnMax{}, btnClose{};
    D2D1_RECT_F content{};        // below the titlebar, left of the info panel
    D2D1_RECT_F canvas{};         // image area (viewer)
    D2D1_RECT_F filmstrip{};
    D2D1_RECT_F commandBar{};
    D2D1_RECT_F info{};
    D2D1_RECT_F comp{};           // compressor panel, slides in from the right
    D2D1_RECT_F gridHeader{};
    D2D1_RECT_F grid{};
};

// What the viewer looked like for a given file, so coming back to it restores
// the zoom and orientation you left it at.
struct ViewState {
    float zoom = 1.f, panX = 0, panY = 0;
    int   rot = 0;
    bool  flipH = false, flipV = false;
    int   fit = 0;                 // Fit enum as int
};

struct Toast {
    wstring text;
    double  until = 0;
};

// The Direct2D side of the generated artwork: one brush per lobe of the plan
// that coverart.cpp worked out, plus the vignette.
struct CoverArt {
    wstring   path;
    void*     device = nullptr;    // whose device context the brushes belong to
    CoverPlan plan;
    std::vector<ComPtr<ID2D1RadialGradientBrush>> brushes;
    ComPtr<ID2D1RadialGradientBrush> vignette;
};

// Reshapes a crop rectangle to a fixed ratio, keeping whichever edge the drag
// is not moving. Defined in main.cpp, used by the drag handler in ui.cpp.
void cropShapeTo(D2D1_RECT_F& r, float ar, float srcW, float srcH,
                 int west, int east, int north, int south);

struct App {
    HWND      hwnd = nullptr;
    HINSTANCE inst = nullptr;
    Gfx       gfx;
    Theme     th;
    Settings  cfg;
    Loader    loader;
    VideoPlayer  video;
    VideoPreview preview;
    ResumeStore  resume;
    double  pendingResume = -1;
    ImageFolder    folder;
    Rects     R;
    UiInput   in;

    // ---- navigation
    int     index = -1;
    wstring pendingPath;          // opened before the folder scan finished
    bool    folderScanned = false;

    // ---- caches
    std::unordered_map<wstring, std::shared_ptr<Picture>> pics;
    std::deque<wstring>                                   picLru;
    std::unordered_map<wstring, std::shared_ptr<Thumb>>   thumbs;
    std::deque<wstring>                                   thumbLru;
    size_t  picBudget = 10;
    size_t  thumbBudget = 900;

    // ---- video
    bool   videoMode = false;
    bool   videoInit = false;
    bool   seekDragging = false;
    double seekPreview = 0;
    double videoBarHidden = 0;
    double seekFlashUntil = 0;      // transient "-5 s" / "+5 s" overlay
    int    seekFlashDir = 0;
    double seekFlashAmount = 0;
    ComPtr<ID2D1Bitmap1> previewBmp;   // frame under the seek cursor
    int    previewW = 0, previewH = 0;
    double previewAt = -1;
    double previewWant = -1;
    bool   previewPending = false;  // a thumbnail decode is in flight
    float  previewX = -1.f;         // eased x of the thumbnail card
    float  previewFade = 0.f;       // card fades in instead of popping
    // Vertical volume flyout, used when the player bar is too narrow for the
    // inline slider.
    bool   volPopup = false;
    double volPopupUntil = 0;
    float  volAnim = 0.f;           // grow/fade of that flyout
    D2D1_RECT_F volPopupRect{};

    std::unordered_map<wstring, ViewState> viewStates;
    std::deque<wstring> viewStateLru;
    bool   hasPendingView = false;
    ViewState pendingView;

    // ---- viewer state
    View   view = View::Viewer;
    Fit    fitMode = Fit::Window;
    float  zoom = 1.f, zoomTarget = 1.f;
    float  panX = 0, panY = 0, panTargetX = 0, panTargetY = 0;
    int    rot = 0;                 // 0..3, user rotation (x90 CW)
    bool   flipH = false, flipV = false;
    bool   dragging = false;
    POINT  dragOrigin{};
    float  dragPanX = 0, dragPanY = 0;
    bool   fullscreen = false;
    WINDOWPLACEMENT prevPlacement{};
    bool   slideshow = false;
    double slideshowNext = 0;
    float  fadeIn = 1.f;            // cross-fade when a new image lands

    // ---- grid state
    float  gridScroll = 0, gridScrollTarget = 0;
    int    gridHover = -1;
    int    gridCols = 1;
    float  gridRowH = 1;
    int    filmHover = -1;
    float  filmScroll = 0, filmScrollTarget = 0;

    // ---- ui bookkeeping
    int    hot = 0, active = 0, pressedId = 0;
    LPCWSTR wantCursor = IDC_ARROW;    // decided while drawing the frame
    LPCWSTR shownCursor = nullptr;
    int    pendingCmd = CMD_NONE;
    double lastMouseMove = 0;
    float  barAlpha = 1.f;
    bool   barPinned = false;
    wstring tipText;
    D2D1_RECT_F tipAnchor{};
    Toast  toast;
    bool   animating = false;
    double frameDt = 0.016;         // seconds since the previous frame
    float  menuAnim = 0.f;          // flyout grow/fade
    bool   showHelp = false;
    bool   sortMenuOpen = false;
    D2D1_RECT_F sortMenuRect{};
    bool   settingsOpen = false;
    float  settingsScroll = 0, settingsScrollMax = 0;
    int    settingsTab = 0;
    bool   settingsDirty = false;
    std::vector<wstring>        assocAll;
    std::unordered_set<wstring> assocSel;
    bool   assocLoaded = false;
    bool   assocApplied = false;    // registry written at least once this run
    bool   moreMenuOpen = false;
    D2D1_RECT_F moreMenuAnchor{};
    D2D1_RECT_F moreMenuBounds{};
    float  menuScroll = 0, menuScrollMax = 0;
    // ---- crop
    // The rectangle lives in source-image pixels, so it survives zoom, pan,
    // rotation and mirroring without being recomputed.
    bool   cropMode = false;        // picking the rectangle right now
    bool   cropActive = false;      // a crop is applied to what you see
    D2D1_RECT_F cropRect{};
    int    cropDrag = -1;           // -1 none, 0..7 handles, 8 whole rectangle
    D2D1_POINT_2F cropGrabAt{};
    D2D1_RECT_F   cropGrabRect{};
    // The locked ratio, as the two numbers the user sees. 0:0 means free.
    int    cropRatioW = 0, cropRatioH = 0;
    float  cropAspect() const {
        return (cropRatioW > 0 && cropRatioH > 0) ? (float)cropRatioW / cropRatioH : 0.f;
    }

    // A tiny typed field: which one has focus, and the digits so far.
    int     editField = 0;          // 0 none, 1 width, 2 height, 3 ratio w, 4 ratio h
    wstring editBuf;

    // ---- compressor / converter
    Compressor   comp;
    bool         compOpen = false;
    int          compFormat = 0;
    CompressMode compMode = CompressMode::Quality;
    float        compQuality = 0.82f;
    float        compScale = 1.f;
    float        compPercent = 0.5f;      // Percent mode: share of the original
    double       compTargetMB = 1.0;      // TargetBytes mode
    bool         compDownscale = true;
    uint64_t     compSeq = 0;
    bool         compDirty = true;        // settings moved, a new preview is due
    double       compDueAt = 0;           // debounce: the slider is still moving
    bool         compPending = false;
    bool         compHasResult = false;
    CompressResult compRes;
    ComPtr<ID2D1Bitmap1> compBmp;         // the preview, decoded back from bytes
    bool         compCompare = false;     // holding the button to see the original
    int          compBatchDone = 0, compBatchTotal = 0;
    wstring      compStatus;
    wstring      compOutDir;
    float        compScroll = 0, compScrollMax = 0;
    float        compAnim = 0.f;          // 0 closed, 1 fully slid in

    // ---- media converter (the same slot as the compressor, for audio/video)
    MediaConverter conv;
    int      convFormat = 0;
    int      convAudioKbps = 192;
    int      convVideoKbps = 0;       // 0 = follow the source
    float    convScale = 1.f;
    bool     convCopy = true;         // copy the streams when the container takes them
    int      convBatchDone = 0, convBatchTotal = 0;
    bool     convRunning = false;
    bool     convHasResult = false;
    ConvertResult convRes;
    wstring  convStatus;
    wstring  convOutDir;
    double   convStartedAt = 0;
    wstring  convProbePath;           // what convCanCopy was worked out for
    int      convProbeFormat = -1;
    bool     convCanCopy = false;
    int      convSrcChannels = 0, convSrcRate = 0, convSrcKbps = 0;
    uint64_t convSrcBytes = 0;

    AudioEnvelope envelope;          // loudness of the track over time
    CoverArt  cover;                 // generated artwork for a track with none
    float     coverPulse = 0;        // smoothed low end: fast attack, slow release
    float     coverLevel = 0;
    double    coverTime = 0;         // advances only while the track plays

    wstring   audioTagsPath;         // whose tags audioTags holds
    AudioTags audioTags;
    wstring   coverUpgraded;         // cover art already re-fetched at full size

    wstring mediaPropsPath;
    std::vector<std::pair<wstring, wstring>> mediaProps;
    bool   needRelayout = true;
    double lastFrame = 0;
    double startupAt = 0;
    bool   firstImageShown = false;
    double firstImageMs = 0;

    // ---- helpers implemented in main.cpp
    void invalidate();
    void requestAnim();
    void showToast(const wstring& t, double seconds = 1.8);
    std::shared_ptr<Picture> current();
    std::shared_ptr<Thumb>   thumbFor(const wstring& path, int px, bool request);
    wstring currentPath() const;
    void openPath(const wstring& path);
    void scanFolderNow(const wstring& dir, const wstring& select);
    void goTo(int newIndex, bool resetView);
    void step(int delta);
    void requestPicture(const wstring& path, bool exif, int prio);
    void preload();
    void applyFit(bool animate);
    void setZoom(float z, D2D1_POINT_2F anchorPx, bool animate);
    void clampPan();
    void setView(View v);
    void setFullscreen(bool on);
    void applyTheme();
    void applyBackdrop();
    void applyTopmost();
    void storeResume();
    void loadAssociations();
    bool blurActive = false;
    // In "window follows the picture" the title bar floats over the image and
    // fades with the command bar, so the window is exactly the picture.
    bool titleOverlay() const { return cfg.autoSize == 2 && !fullscreen; }
    // Music plays through the same engine as video; there is simply no picture.
    bool audioOnly() const { return videoMode && !video.hasVideo(); }
    void copyToClipboard();
    void deleteCurrent();
    void openFileDialog();
    void openFolderDialog();
    void revealInExplorer();
    void setAsWallpaper();
    D2D1_RECT_F imageRect();        // where the current image lands, in px
    bool sourceSize(int& w, int& h);   // pixel size of whatever is on screen
    void autoSizeWindow();             // AutoSize == 2: window follows the picture
    void rememberView();               // stash the current zoom/orientation
    void takePendingView();            // apply a stashed one after the fit
    void cropBegin();
    void cropSetSize(int w, int h);          // exact pixels, kept inside the image
    void cropSetRatio(int rw, int rh);       // 0:0 unlocks
    void cropFitRatio();                     // reshape the rectangle to the ratio
    void editCommit();                       // push the typed digits into the crop
    void cropApply();
    void cropCancel();
    void cropReset();
    bool cropSize(int& w, int& h) const;   // the picture as cropped
    void openCompressor(bool on);
    bool convertMode() const { return compOpen && videoMode; }
    void convertReset();              // aim the panel at the current file
    ConvertJob convertJob(const wstring& path, const wstring& outPath) const;
    wstring convertOutPath(const wstring& src, const wstring& dir) const;
    void convertSave(bool askWhere);
    void convertBatch();
    double mediaDuration() const;     // of the open file, seconds
    void compressRequest(bool now = false);
    CompressJob compressJob(const wstring& path, const wstring& outPath) const;
    wstring compressOutPath(const wstring& src, const wstring& dir) const;
    void compressSave(bool askWhere);
    void compressBatch();
    void openVideo(const wstring& path);
    void videoSeekBy(double delta);
    void videoSeekTo(double seconds);
    void leaveVideo();
    void trimCaches();
};

extern App* g_app;
