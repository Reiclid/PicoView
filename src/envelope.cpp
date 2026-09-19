// How loud a track is, moment by moment.
//
// The file is decoded once on a background thread and thrown away: all that is
// kept is two numbers every 1/30 s - overall level and low-frequency energy -
// which is a few hundred kilobytes for a whole album side. Decoding runs far
// faster than playback, so the curve is usually complete within a second of
// pressing play, and what has not arrived yet simply reads as "not known".
//
// This exists so the generated cover art can move with the music. It is not a
// spectrum analyser: a one-pole filter separating a kick drum from a hi-hat is
// all the detail a breathing shape can show.
#include "pg.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

static const int    kEnvRate = 30;             // envelope samples per second
static const size_t kEnvMax = (size_t)kEnvRate * 4 * 3600;   // stop after 4 hours

struct AudioEnvelope::Impl {
    std::thread       th;
    std::atomic<bool> quit{ false };
    std::atomic<bool> done{ false };
    wstring           path;

    mutable std::mutex m;
    std::vector<float> lvl, bass;
    float              peakLvl = 0.f, peakBass = 0.f;
};

AudioEnvelope::~AudioEnvelope() { close(); }

void AudioEnvelope::close() {
    if (!p_) return;
    p_->quit.store(true);
    if (p_->th.joinable()) p_->th.join();
    delete p_;
    p_ = nullptr;
}

wstring AudioEnvelope::path() const { return p_ ? p_->path : wstring(); }
bool    AudioEnvelope::done() const { return p_ && p_->done.load(); }

bool AudioEnvelope::at(double seconds, float& level, float& bass) const {
    if (!p_ || seconds < 0) return false;
    double x = seconds * kEnvRate;
    size_t i = (size_t)x;
    float  f = (float)(x - (double)i);

    std::lock_guard<std::mutex> lk(p_->m);
    if (i + 1 >= p_->lvl.size()) return false;
    // Scaled against the loudest moment seen so far, so a quiet recording
    // breathes as visibly as a loud one.
    float pl = std::max(0.02f, p_->peakLvl);
    float pb = std::max(0.02f, p_->peakBass);
    level = clampf((p_->lvl[i] * (1.f - f) + p_->lvl[i + 1] * f) / pl, 0.f, 1.f);
    bass = clampf((p_->bass[i] * (1.f - f) + p_->bass[i + 1] * f) / pb, 0.f, 1.f);
    return true;
}

void AudioEnvelope::open(const wstring& path) {
    close();
    if (path.empty()) return;
    auto* p = new Impl();
    p->path = path;
    p_ = p;

    p->th = std::thread([p] {
        // Never at the expense of playback or the interface.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

        const DWORD kAudio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
        ComPtr<IMFSourceReader> rd;
        if (mf && SUCCEEDED(MFCreateSourceReaderFromURL(p->path.c_str(), nullptr, &rd))) {
            rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
            rd->SetStreamSelection(kAudio, TRUE);

            ComPtr<IMFMediaType> want;
            UINT32 rate = 0, ch = 0;
            if (SUCCEEDED(MFCreateMediaType(&want))) {
                want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                if (SUCCEEDED(rd->SetCurrentMediaType(kAudio, nullptr, want.Get()))) {
                    ComPtr<IMFMediaType> cur;
                    if (SUCCEEDED(rd->GetCurrentMediaType(kAudio, &cur))) {
                        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                    }
                }
            }
            if (!rate) rate = 44100;
            if (!ch) ch = 1;

            const int   per = std::max(1, (int)(rate / kEnvRate));
            // A one-pole at 150 Hz: enough to tell a kick from a hi-hat.
            const float k = 1.f - expf(-2.f * 3.14159265f * 150.f / (float)rate);

            float  lp = 0.f;
            double sumSq = 0, sumB = 0;
            int    n = 0;
            std::vector<float> outLvl, outBass;          // batched, to hold the lock briefly

            while (!p->quit.load(std::memory_order_relaxed)) {
                DWORD flags = 0, actual = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(rd->ReadSample(kAudio, 0, &actual, &flags, &ts, &sample))) break;
                if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
                if (!sample) continue;

                ComPtr<IMFMediaBuffer> buf;
                if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
                BYTE* data = nullptr;
                DWORD len = 0;
                if (FAILED(buf->Lock(&data, nullptr, &len))) continue;

                const int16_t* pcm = (const int16_t*)data;
                size_t frames = (size_t)len / 2 / ch;
                for (size_t i = 0; i < frames; ++i) {
                    int acc = 0;
                    for (UINT32 c = 0; c < ch; ++c) acc += pcm[i * ch + c];
                    float v = (float)acc / ((float)ch * 32768.f);
                    lp += (v - lp) * k;
                    sumSq += (double)v * v;
                    sumB += (double)lp * lp;
                    if (++n >= per) {
                        outLvl.push_back(sqrtf((float)(sumSq / n)));
                        outBass.push_back(sqrtf((float)(sumB / n)));
                        sumSq = sumB = 0;
                        n = 0;
                    }
                }
                buf->Unlock();

                if (!outLvl.empty()) {
                    std::lock_guard<std::mutex> lk(p->m);
                    for (size_t i = 0; i < outLvl.size(); ++i) {
                        p->peakLvl = std::max(p->peakLvl, outLvl[i]);
                        p->peakBass = std::max(p->peakBass, outBass[i]);
                    }
                    p->lvl.insert(p->lvl.end(), outLvl.begin(), outLvl.end());
                    p->bass.insert(p->bass.end(), outBass.begin(), outBass.end());
                    outLvl.clear();
                    outBass.clear();
                    if (p->lvl.size() >= kEnvMax) break;
                }
            }
        }

        p->done.store(true);
        if (mf) MFShutdown();
        CoUninitialize();
    });
}
