// How loud a file is, as a single number you can compare across files.
//
// Peak level says nothing about how loud something sounds: a film mixed with
// room for an explosion and a mastered-for-radio single both peak at full
// scale, yet one is twenty decibels quieter to the ear. ITU-R BS.1770 answers
// the right question - it filters the signal the way a head hears it, averages
// the energy over 400 ms blocks, and throws away the silence before taking the
// mean. The result is LUFS, and two files with the same LUFS sound equally
// loud. That is the whole basis of the leveller.
//
// A long film is not decoded from end to end for this. Two dozen short windows
// spread across it land within a decibel of the full measurement and cost a
// fraction of a second, which is what keeps this honest about being free.
#include "pg.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// ------------------------------------------------------------------ filters
// The two stages of K-weighting: a high shelf for the head's own resonance and
// a high-pass that takes out what the ear barely registers. Both are given as
// analogue prototypes in the standard; these are the bilinear transforms of
// them, so they are right at any sample rate rather than only at 48 kHz.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() { x1 = x2 = y1 = y2 = 0; }
    double run(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

static Biquad kShelf(double fs) {
    const double f0 = 1681.974450955533;
    const double G = 3.999843853973347;      // dB
    const double Q = 0.7071752369554196;
    double K = tan(3.14159265358979 * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    Biquad q;
    q.b0 = (Vh + Vb * K / Q + K * K) / a0;
    q.b1 = 2.0 * (K * K - Vh) / a0;
    q.b2 = (Vh - Vb * K / Q + K * K) / a0;
    q.a1 = 2.0 * (K * K - 1.0) / a0;
    q.a2 = (1.0 - K / Q + K * K) / a0;
    return q;
}

static Biquad kHighpass(double fs) {
    const double f0 = 38.13547087602444;
    const double Q = 0.5003270373238773;
    double K = tan(3.14159265358979 * f0 / fs);
    double a0 = 1.0 + K / Q + K * K;
    Biquad q;
    q.b0 = 1.0;
    q.b1 = -2.0;
    q.b2 = 1.0;
    q.a1 = 2.0 * (K * K - 1.0) / a0;
    q.a2 = (1.0 - K / Q + K * K) / a0;
    return q;
}

// ------------------------------------------------------------------ cache
// Measuring the same file twice in one session would be wasted work; a track
// on repeat, or a folder walked back and forth, is the common case.
struct LoudCache {
    std::mutex m;
    std::vector<std::pair<wstring, std::pair<float, float>>> v;

    bool get(const wstring& k, float& lufs, float& peak) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& e : v)
            if (e.first == k) { lufs = e.second.first; peak = e.second.second; return true; }
        return false;
    }
    void put(const wstring& k, float lufs, float peak) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& e : v)
            if (e.first == k) { e.second = { lufs, peak }; return; }
        if (v.size() >= 96) v.erase(v.begin());
        v.push_back({ k, { lufs, peak } });
    }
};
static LoudCache g_loudCache;

// ------------------------------------------------------------------ scan
struct LoudnessScan::Impl {
    std::thread       th;
    std::atomic<bool> quit{ false };
    std::atomic<bool> ready{ false };
    std::atomic<bool> ok{ false };
    std::atomic<float> lufs{ -70.f };
    std::atomic<float> peak{ 0.f };
    wstring           path;
};

LoudnessScan::~LoudnessScan() { close(); }

void LoudnessScan::close() {
    if (!p_) return;
    p_->quit.store(true);
    // Let go rather than wait. Stepping through a folder of films would
    // otherwise stall the interface on whatever ReadSample happens to be
    // doing, which on a network share is not a short time. The worker holds
    // the last reference and takes the block with it.
    if (p_->th.joinable()) p_->th.detach();
    p_.reset();
}

wstring LoudnessScan::path() const { return p_ ? p_->path : wstring(); }
bool    LoudnessScan::ready() const { return p_ && p_->ready.load(); }

bool LoudnessScan::result(float& lufs, float& peak) const {
    if (!p_ || !p_->ready.load() || !p_->ok.load()) return false;
    lufs = p_->lufs.load();
    peak = p_->peak.load();
    return true;
}

// Gated mean of the block energies, per BS.1770-4: silence is dropped outright,
// then anything more than 10 dB below the rest is dropped as well, so a film's
// dialogue sets the level and its quiet passages do not drag it down.
static float gatedLoudness(std::vector<double>& z) {
    if (z.empty()) return -70.f;
    auto loud = [](double e) { return -0.691 + 10.0 * log10(std::max(1e-12, e)); };

    double sum = 0;
    int    n = 0;
    for (double e : z)
        if (loud(e) > -70.0) { sum += e; ++n; }
    if (!n) return -70.f;

    double rel = loud(sum / n) - 10.0;
    double sum2 = 0;
    int    n2 = 0;
    for (double e : z)
        if (loud(e) > -70.0 && loud(e) > rel) { sum2 += e; ++n2; }
    if (!n2) return (float)loud(sum / n);
    return (float)loud(sum2 / n2);
}

void LoudnessScan::open(const wstring& path) {
    close();
    if (path.empty()) return;
    auto p = std::make_shared<Impl>();
    p->path = path;
    p_ = p;

    float clufs = 0, cpeak = 0;
    if (g_loudCache.get(path, clufs, cpeak)) {
        p->lufs.store(clufs);
        p->peak.store(cpeak);
        p->ok.store(true);
        p->ready.store(true);
        return;
    }

    p->th = std::thread([p] {        // this capture is what keeps Impl alive
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

        const DWORD kAudio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
        ComPtr<IMFSourceReader> rd;
        UINT32 rate = 0, ch = 0;
        double durSec = 0;

        if (mf && SUCCEEDED(MFCreateSourceReaderFromURL(p->path.c_str(), nullptr, &rd))) {
            rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
            rd->SetStreamSelection(kAudio, TRUE);

            ComPtr<IMFMediaType> want;
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
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                       MF_PD_DURATION, &pv)) &&
                pv.vt == VT_UI8)
                durSec = (double)pv.uhVal.QuadPart / 1e7;
            PropVariantClear(&pv);
        }

        if (rd && rate && ch) {
            const int    kBlock = (int)(rate * 0.4);     // 400 ms, per the standard
            const int    kHop = std::max(1, (int)(rate * 0.1));
            const double invBlock = 1.0 / (double)kBlock;

            // Whole short files; a sample of long ones. Thirty-odd seconds of
            // audio drawn from across a film tells you what it is mixed at.
            struct Window { double start, len; };
            std::vector<Window> wins;
            if (durSec <= 0 || durSec <= 150.0) {
                wins.push_back({ 0.0, durSec > 0 ? durSec + 1.0 : 1e9 });
            } else {
                const int kN = 24;
                for (int i = 0; i < kN; ++i)
                    wins.push_back({ durSec * (0.03 + 0.94 * i / (double)(kN - 1)), 1.6 });
            }

            std::vector<Biquad> sh(ch, kShelf(rate)), hp(ch, kHighpass(rate));
            std::vector<double> z;                   // one energy per block
            std::vector<double> acc(ch, 0.0);        // running square sum per channel
            float  peak = 0.f;
            int    filled = 0;                       // samples in the current block
            // A ring of the last block's worth of squares, so blocks can overlap
            // by 75% the way the standard asks without buffering the audio.
            std::vector<std::vector<double>> ring(ch, std::vector<double>(kBlock, 0.0));
            int ringPos = 0;
            int sinceHop = 0;

            for (size_t w = 0; w < wins.size() && !p->quit.load(std::memory_order_relaxed); ++w) {
                if (wins[w].start > 0.0) {
                    PROPVARIANT pos;
                    PropVariantInit(&pos);
                    pos.vt = VT_I8;
                    pos.hVal.QuadPart = (LONGLONG)(wins[w].start * 1e7);
                    HRESULT hr = rd->SetCurrentPosition(GUID_NULL, pos);
                    PropVariantClear(&pos);
                    if (FAILED(hr)) break;
                    // A seek breaks the filters' history, so they start clean and
                    // the first tenth of a second of each window is thrown away
                    // while they settle.
                    for (UINT32 c = 0; c < ch; ++c) { sh[c].reset(); hp[c].reset(); }
                    std::fill(acc.begin(), acc.end(), 0.0);
                    for (auto& r : ring) std::fill(r.begin(), r.end(), 0.0);
                    ringPos = 0; filled = 0; sinceHop = 0;
                }

                double taken = 0;
                int    settle = (wins[w].start > 0.0) ? (int)(rate * 0.1) : 0;

                while (taken < wins[w].len && !p->quit.load(std::memory_order_relaxed)) {
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
                    taken += (double)frames / (double)rate;

                    for (size_t i = 0; i < frames; ++i) {
                        if (settle > 0) { --settle; continue; }
                        for (UINT32 c = 0; c < ch; ++c) {
                            double v = pcm[i * ch + c] / 32768.0;
                            peak = std::max(peak, (float)fabs(v));
                            double y = hp[c].run(sh[c].run(v));
                            double sq = y * y;
                            acc[c] += sq - ring[c][ringPos];
                            ring[c][ringPos] = sq;
                        }
                        ringPos = (ringPos + 1) % kBlock;
                        if (filled < kBlock) ++filled;
                        if (filled >= kBlock && ++sinceHop >= kHop) {
                            sinceHop = 0;
                            double e = 0;
                            // Channel weights are 1.0 for the front pair; the
                            // surround boost of the standard would need to know
                            // the layout, and getting it wrong costs more than
                            // leaving it out.
                            for (UINT32 c = 0; c < ch; ++c) e += std::max(0.0, acc[c]) * invBlock;
                            z.push_back(e);
                        }
                    }
                    buf->Unlock();
                }
                if (wins.size() == 1) break;
            }

            if (!z.empty()) {
                float l = gatedLoudness(z);
                p->lufs.store(l);
                p->peak.store(peak);
                p->ok.store(true);
                g_loudCache.put(p->path, l, peak);
                pgLog("loudness %.1f LUFS peak %.3f blocks %d", l, peak, (int)z.size());
            }
        }

        p->ready.store(true);
        if (mf) MFShutdown();
        CoUninitialize();
    });
}
