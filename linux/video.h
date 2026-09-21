// Video and music, through libmpv.
//
// The Windows build hands this to Media Foundation's media engine: open a
// file, get frames, get sound, seek. libmpv is the same shape of thing and is
// what every Linux program that is not a browser ends up using - writing a
// player on top of ffmpeg directly means writing demuxing, timing, audio sync
// and subtitle handling, which is a program in itself.
//
// The one unusual choice here is the software render API. mpv normally renders
// through OpenGL, and this front end has no GL context on purpose. mpv's SW
// path hands back a plain block of BGRA that goes into the same buffer the
// pictures go into, so video costs the program no graphics stack at all. It
// costs processor time instead, which for a viewer is the right way round.
#pragma once

#include <cstdint>
#include <string>

namespace pv {

class Video {
public:
    // Public because mpv's update callback is a free function and has to name
    // the type it is handed.
    struct Impl;

    ~Video();

    bool open(const std::string& path);
    void close();
    bool isOpen() const { return p_ != nullptr; }

    // True when a frame is waiting. Drawing without asking would burn a core
    // redrawing the same picture.
    bool wants() const;
    // Paint the current frame into a BGRA block. Returns false when there is
    // nothing yet - the first frames take a moment to arrive.
    bool render(uint8_t* dst, int w, int h, int stride);

    void togglePause();
    bool paused() const;
    void seekBy(double seconds);
    void seekTo(double seconds);
    double position() const;
    double duration() const;
    void setVolume(int percent);
    int  volume() const;

    int  width() const;
    int  height() const;
    bool hasVideo() const;          // false for a music file
    std::string error() const;

    // Drain mpv's event queue. Called from the main loop; cheap when idle.
    void pump();

private:
    Impl* p_ = nullptr;
};

bool isVideoName(const std::string& name);
bool isAudioName(const std::string& name);

}  // namespace pv
