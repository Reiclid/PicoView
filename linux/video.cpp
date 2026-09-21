#include "video.h"

#include <atomic>
#include <cstring>
#include <mpv/client.h>
#include <mpv/render.h>

namespace pv {
namespace {

bool endsLower(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[s.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return false;
    }
    return true;
}

}  // namespace

bool isVideoName(const std::string& s) {
    static const char* e[] = { ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".wmv",
                               ".mpg", ".mpeg", ".ts", ".m2ts", ".flv", ".ogv", ".3gp",
                               nullptr };
    for (int i = 0; e[i]; ++i)
        if (endsLower(s, e[i])) return true;
    return false;
}

bool isAudioName(const std::string& s) {
    static const char* e[] = { ".mp3", ".wav", ".flac", ".m4a", ".ogg", ".opus", ".wma",
                               ".aac", ".alac", ".aiff", ".ape", nullptr };
    for (int i = 0; e[i]; ++i)
        if (endsLower(s, e[i])) return true;
    return false;
}

struct Video::Impl {
    mpv_handle*         mpv = nullptr;
    mpv_render_context* ctx = nullptr;
    std::atomic<bool>   frame{ false };   // a new frame is ready to be drawn
    bool                gotAny = false;
    std::string         err;
    int                 vw = 0, vh = 0;
    bool                video = false;
};

namespace {
// mpv calls this from its own thread when a frame is ready. Setting a flag is
// all that is safe to do here.
void onUpdate(void* data) {
    ((Video::Impl*)data)->frame.store(true);
}
}  // namespace

Video::~Video() { close(); }

void Video::close() {
    if (!p_) return;
    if (p_->ctx) {
        mpv_render_context_set_update_callback(p_->ctx, nullptr, nullptr);
        mpv_render_context_free(p_->ctx);
    }
    if (p_->mpv) mpv_terminate_destroy(p_->mpv);
    delete p_;
    p_ = nullptr;
}

bool Video::open(const std::string& path) {
    close();
    auto* p = new Impl();
    p_ = p;

    p->mpv = mpv_create();
    if (!p->mpv) { p->err = "mpv_create failed"; return false; }

    // No window, no input, no configuration from the user's mpv.conf: this is
    // a decoder that happens to come with a player attached, and inheriting
    // someone's player settings would be a surprise.
    mpv_set_option_string(p->mpv, "config", "no");
    mpv_set_option_string(p->mpv, "terminal", "no");
    mpv_set_option_string(p->mpv, "osc", "no");
    mpv_set_option_string(p->mpv, "input-default-bindings", "no");
    mpv_set_option_string(p->mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(p->mpv, "vo", "libmpv");
    mpv_set_option_string(p->mpv, "hwdec", "auto-safe");
    mpv_set_option_string(p->mpv, "keep-open", "yes");
    mpv_set_option_string(p->mpv, "idle", "yes");

    if (mpv_initialize(p->mpv) < 0) { p->err = "mpv_initialize failed"; return false; }

    int advanced = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&p->ctx, p->mpv, params) < 0) {
        p->err = "this build of libmpv has no software renderer";
        return false;
    }
    mpv_render_context_set_update_callback(p->ctx, onUpdate, p);

    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    if (mpv_command(p->mpv, cmd) < 0) { p->err = "cannot open " + path; return false; }
    return true;
}

void Video::pump() {
    if (!p_ || !p_->mpv) return;
    for (;;) {
        mpv_event* e = mpv_wait_event(p_->mpv, 0);
        if (!e || e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
            case MPV_EVENT_FILE_LOADED: {
                int64_t w = 0, h = 0;
                mpv_get_property(p_->mpv, "width", MPV_FORMAT_INT64, &w);
                mpv_get_property(p_->mpv, "height", MPV_FORMAT_INT64, &h);
                p_->vw = (int)w;
                p_->vh = (int)h;
                p_->video = (w > 0 && h > 0);
                break;
            }
            case MPV_EVENT_END_FILE: {
                auto* ef = (mpv_event_end_file*)e->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)
                    p_->err = mpv_error_string(ef->error);
                break;
            }
            default: break;
        }
    }
}

bool Video::wants() const { return p_ && p_->frame.load(); }

bool Video::render(uint8_t* dst, int w, int h, int stride) {
    if (!p_ || !p_->ctx || w <= 0 || h <= 0) return false;
    p_->frame.store(false);

    int size[2] = { w, h };
    size_t st = (size_t)stride;
    // "bgr0" is what a wl_shm ARGB8888 buffer is on a little-endian machine
    // once the alpha is ignored, and the alpha here is always opaque.
    char fmt[] = "bgr0";
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, size },
        { MPV_RENDER_PARAM_SW_FORMAT, fmt },
        { MPV_RENDER_PARAM_SW_STRIDE, &st },
        { MPV_RENDER_PARAM_SW_POINTER, dst },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_render(p_->ctx, params) < 0) return false;

    // mpv leaves the alpha byte alone, and a wl_shm surface with zero alpha is
    // an invisible one.
    for (int y = 0; y < h; ++y) {
        uint8_t* row = dst + (size_t)y * stride;
        for (int x = 0; x < w; ++x) row[x * 4 + 3] = 255;
    }
    p_->gotAny = true;
    return true;
}

void Video::togglePause() {
    if (!p_ || !p_->mpv) return;
    int flag = paused() ? 0 : 1;
    mpv_set_property(p_->mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

bool Video::paused() const {
    if (!p_ || !p_->mpv) return true;
    int flag = 0;
    mpv_get_property(p_->mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

void Video::seekBy(double seconds) {
    if (!p_ || !p_->mpv) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char* cmd[] = { "seek", buf, "relative", nullptr };
    mpv_command(p_->mpv, cmd);
}

void Video::seekTo(double seconds) {
    if (!p_ || !p_->mpv) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", seconds < 0 ? 0 : seconds);
    const char* cmd[] = { "seek", buf, "absolute", nullptr };
    mpv_command(p_->mpv, cmd);
}

double Video::position() const {
    if (!p_ || !p_->mpv) return 0;
    double v = 0;
    mpv_get_property(p_->mpv, "time-pos", MPV_FORMAT_DOUBLE, &v);
    return v;
}

double Video::duration() const {
    if (!p_ || !p_->mpv) return 0;
    double v = 0;
    mpv_get_property(p_->mpv, "duration", MPV_FORMAT_DOUBLE, &v);
    return v;
}

void Video::setVolume(int percent) {
    if (!p_ || !p_->mpv) return;
    double v = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    mpv_set_property(p_->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

int Video::volume() const {
    if (!p_ || !p_->mpv) return 0;
    double v = 0;
    mpv_get_property(p_->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
    return (int)(v + 0.5);
}

int  Video::width() const { return p_ ? p_->vw : 0; }
int  Video::height() const { return p_ ? p_->vh : 0; }
bool Video::hasVideo() const { return p_ && p_->video; }
std::string Video::error() const { return p_ ? p_->err : std::string(); }

}  // namespace pv
