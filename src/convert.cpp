// Converting media: one container and codec in, another out.
//
// Everything goes through Media Foundation's source reader and sink writer.
// The reader decodes whatever the system can play, the writer encodes whatever
// the system can write, and the pair inserts the decoder, the resampler and the
// encoder by itself. When the new container will accept the streams as they
// already are we copy them across instead, which is both instant and lossless -
// pulling the soundtrack out of an MP4 should not re-encode it.
#include "pg.h"

#include <mfapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>

// ------------------------------------------------------------------ formats
static std::vector<MediaFormat> g_mediaFormats;
static std::once_flag           g_mediaOnce;

// Is there an encoder on this machine for that subtype? A format we cannot
// write has no business in the list.
static bool hasEncoder(REFGUID category, REFGUID major, REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO info{ major, subtype };
    IMFActivate** act = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_ALL, nullptr, &info, &act, &n))) return false;
    for (UINT32 i = 0; i < n; ++i) if (act[i]) act[i]->Release();
    CoTaskMemFree(act);
    return n > 0;
}

static void probeMediaFormats() {
    bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    auto audio = [&](const wchar_t* ext, const wchar_t* name, REFGUID container,
                     REFGUID subtype, bool always = false) {
        if (!always && !hasEncoder(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, subtype)) return;
        MediaFormat f;
        f.ext = ext; f.name = name;
        f.container = container;
        f.audio = subtype;
        f.vcodec = GUID_NULL;
        f.video = false;
        f.bitrate = (subtype != MFAudioFormat_PCM && subtype != MFAudioFormat_FLAC);
        g_mediaFormats.push_back(f);
    };
    auto video = [&](const wchar_t* ext, const wchar_t* name, REFGUID container,
                     REFGUID vcodec, REFGUID acodec) {
        if (!hasEncoder(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, vcodec)) return;
        MediaFormat f;
        f.ext = ext; f.name = name;
        f.container = container;
        f.audio = acodec;
        f.vcodec = vcodec;
        f.video = true;
        f.bitrate = true;
        g_mediaFormats.push_back(f);
    };

    // Video first, the way the panel reads: the picture formats, then the ways
    // to keep only the sound.
    video(L".mp4", L"MP4 · H.264", MFTranscodeContainerType_MPEG4, MFVideoFormat_H264, MFAudioFormat_AAC);
    video(L".mp4", L"MP4 · HEVC", MFTranscodeContainerType_MPEG4, MFVideoFormat_HEVC, MFAudioFormat_AAC);
    video(L".wmv", L"WMV", MFTranscodeContainerType_ASF, MFVideoFormat_WMV3, MFAudioFormat_WMAudioV9);

    // PCM needs no encoder at all - the resampler alone can produce it.
    audio(L".mp3", L"MP3", MFTranscodeContainerType_MP3, MFAudioFormat_MP3);
    audio(L".m4a", L"M4A · AAC", MFTranscodeContainerType_MPEG4, MFAudioFormat_AAC);
    audio(L".wav", L"WAV", MFTranscodeContainerType_WAVE, MFAudioFormat_PCM, true);
    audio(L".flac", L"FLAC", MFTranscodeContainerType_FLAC, MFAudioFormat_FLAC);
    audio(L".wma", L"WMA", MFTranscodeContainerType_ASF, MFAudioFormat_WMAudioV9);

    if (started) MFShutdown();
}

const std::vector<MediaFormat>& mediaFormats() {
    std::call_once(g_mediaOnce, probeMediaFormats);
    return g_mediaFormats;
}

int mediaFormatFor(const wstring& ext, bool wantVideo) {
    wstring e = lowerOf(ext);
    const auto& fs = mediaFormats();
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].ext == e && fs[i].video == wantVideo) return (int)i;
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].video == wantVideo) return (int)i;
    return fs.empty() ? -1 : 0;
}

// ------------------------------------------------------------------ helpers
// What a container will take as it stands. Anything else has to be re-encoded.
static bool containerAccepts(const MediaFormat& fmt, REFGUID major, REFGUID subtype) {
    if (subtype == GUID_NULL) return false;
    if (fmt.container == MFTranscodeContainerType_MPEG4) {
        if (major == MFMediaType_Video)
            return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC;
        return subtype == MFAudioFormat_AAC;
    }
    if (fmt.container == MFTranscodeContainerType_ASF) {
        if (major == MFMediaType_Video)
            return subtype == MFVideoFormat_WMV3 || subtype == MFVideoFormat_WVC1;
        return subtype == MFAudioFormat_WMAudioV8 || subtype == MFAudioFormat_WMAudioV9;
    }
    if (major == MFMediaType_Video) return false;
    if (fmt.container == MFTranscodeContainerType_MP3)  return subtype == MFAudioFormat_MP3;
    if (fmt.container == MFTranscodeContainerType_WAVE) return subtype == MFAudioFormat_PCM;
    if (fmt.container == MFTranscodeContainerType_FLAC) return subtype == MFAudioFormat_FLAC;
    return false;
}

// The codecs inside a file, as the shell has already indexed them. Opening the
// file with Media Foundation would be the certain answer, but this is for a
// number on screen that changes as the user clicks around, and the real test
// happens when the conversion runs anyway.
static void sourceCodecs(const wstring& path, GUID& audio, GUID& video) {
    audio = GUID_NULL;
    video = GUID_NULL;
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return;

    auto data1 = [&](REFPROPERTYKEY key) -> unsigned {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        unsigned v = 0;
        if (SUCCEEDED(item->GetProperty(key, &pv))) {
            if (pv.vt == VT_CLSID && pv.puuid) v = pv.puuid->Data1;
            else if (pv.vt == VT_LPWSTR && pv.pwszVal && wcslen(pv.pwszVal) > 9 && pv.pwszVal[0] == L'{')
                v = (unsigned)wcstoul(wstring(pv.pwszVal + 1, 8).c_str(), nullptr, 16);
        }
        PropVariantClear(&pv);
        return v;
    };

    switch (data1(PKEY_Audio_Format) & 0xFFFFu) {
        case 0x0055: audio = MFAudioFormat_MP3; break;
        case 0x1610: case 0x00FF: audio = MFAudioFormat_AAC; break;
        case 0x0161: audio = MFAudioFormat_WMAudioV8; break;
        case 0x0162: audio = MFAudioFormat_WMAudioV9; break;
        case 0x0001: case 0x0003: audio = MFAudioFormat_PCM; break;
        case 0xF1AC: audio = MFAudioFormat_FLAC; break;
        default: break;
    }

    unsigned fcc = data1(PKEY_Video_Compression);
    char f[5] = { (char)(fcc & 0xFF), (char)((fcc >> 8) & 0xFF),
                  (char)((fcc >> 16) & 0xFF), (char)((fcc >> 24) & 0xFF), 0 };
    wstring four;
    for (int i = 0; i < 4; ++i) four += (wchar_t)toupper((unsigned char)f[i]);
    if (four == L"H264" || four == L"AVC1" || four == L"X264") video = MFVideoFormat_H264;
    else if (four == L"HEVC" || four == L"HVC1" || four == L"HEV1") video = MFVideoFormat_HEVC;
    else if (four == L"WMV3") video = MFVideoFormat_WMV3;
    else if (four == L"WVC1") video = MFVideoFormat_WVC1;
}

// Channels and sample rate of the source, so an uncompressed target can be
// predicted exactly rather than assumed to be CD stereo.
void mediaSourceAudio(const wstring& path, int& channels, int& sampleRate, int& kbps) {
    channels = 0;
    sampleRate = 0;
    kbps = 0;
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return;
    ULONG v = 0;
    if (SUCCEEDED(item->GetUInt32(PKEY_Audio_ChannelCount, &v))) channels = (int)v;
    if (SUCCEEDED(item->GetUInt32(PKEY_Audio_SampleRate, &v))) sampleRate = (int)v;
    if (SUCCEEDED(item->GetUInt32(PKEY_Audio_EncodingBitrate, &v))) kbps = (int)(v / 1000);
}

bool mediaCanCopy(const wstring& path, int format) {
    const auto& fs = mediaFormats();
    if (fs.empty() || path.empty()) return false;
    const MediaFormat& fmt = fs[clampi(format, 0, (int)fs.size() - 1)];
    GUID a{}, v{};
    sourceCodecs(path, a, v);
    if (!containerAccepts(fmt, MFMediaType_Audio, a)) return false;
    if (fmt.video && !containerAccepts(fmt, MFMediaType_Video, v)) return false;
    return true;
}

static uint64_t sizeOfFile(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

// Of everything the encoder offers, the one closest to the bitrate asked for -
// preferring, among equals, the types that keep the source's sample rate and
// channel count so nothing is resampled or downmixed behind the user's back.
static ComPtr<IMFMediaType> pickAudioType(REFGUID subtype, int kbps, IMFMediaType* src) {
    ComPtr<IMFCollection> coll;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(subtype, MFT_ENUM_FLAG_ALL, nullptr, &coll)))
        return nullptr;
    DWORD n = 0;
    if (FAILED(coll->GetElementCount(&n)) || !n) return nullptr;

    UINT32 srcRate = 0, srcCh = 0;
    if (src) {
        src->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &srcRate);
        src->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &srcCh);
    }
    const double want = kbps * 1000.0 / 8.0;          // bytes per second

    ComPtr<IMFMediaType> best;
    double bestScore = 1e30;
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IUnknown> unk;
        if (FAILED(coll->GetElement(i, &unk))) continue;
        ComPtr<IMFMediaType> t;
        if (FAILED(unk.As(&t))) continue;

        UINT32 bps = 0, rate = 0, ch = 0;
        t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);

        double score = bps ? fabs((double)bps - want) / want : 1.0;
        if (srcRate && rate && rate != srcRate) score += 0.35;
        if (srcCh && ch && ch != srcCh) score += 0.30;
        if (score < bestScore) { bestScore = score; best = t; }
    }
    return best;
}

// FLAC will not take the types its own encoder enumerates - they come back
// with a nonsense 128 bytes/s and 8-bit samples, and the sink writer then
// refuses to connect the input. Describing the stream ourselves works, which
// is also the honest description: lossless, at the source's own rate.
static ComPtr<IMFMediaType> losslessType(REFGUID subtype, IMFMediaType* src) {
    UINT32 rate = 44100, ch = 2;
    if (src) {
        src->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        src->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    }
    ComPtr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate ? rate : 44100);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch ? ch : 2);
    return t;
}

// PCM straight from the reader, pinned to 16 bits so every sink accepts it.
static ComPtr<IMFMediaType> pcmType(IMFMediaType* src) {
    UINT32 rate = 44100, ch = 2;
    if (src) {
        src->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        src->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    }
    if (!rate) rate = 44100;
    if (!ch) ch = 2;
    ComPtr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 2);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return t;
}

static void copyAttr(IMFMediaType* from, IMFMediaType* to, const GUID& key) {
    UINT64 v = 0;
    if (SUCCEEDED(from->GetUINT64(key, &v))) to->SetUINT64(key, v);
}

// ------------------------------------------------------------------ worker
struct MediaConverter::Impl {
    std::thread             th;
    std::mutex              m;
    std::condition_variable cv;
    bool                    quit = false;
    std::deque<ConvertJob>  jobs;
    std::atomic<bool>       working{ false };
    std::atomic<bool>       cancel{ false };
    std::atomic<float>      progress{ 0.f };

    std::mutex                 om;
    std::vector<ConvertResult> out;
    HWND                       notify = nullptr;
};

namespace {

// One attempt at a conversion: either copying the compressed streams into the
// new container, or decoding and re-encoding them.
struct Attempt {
    MediaConverter::Impl* p;
    const ConvertJob&     job;
    const MediaFormat&    fmt;
    bool                  copy;
    double                duration;

    ComPtr<IMFSourceReader> rd;
    ComPtr<IMFSinkWriter>   wr;
    int   srcA = -1, srcV = -1;
    DWORD dstA = 0, dstV = 0;
    bool  haveA = false, haveV = false;
    wstring error;

    bool aborted() const { return p->cancel.load(std::memory_order_relaxed); }

    bool openReader() {
        ComPtr<IMFAttributes> ra;
        if (FAILED(MFCreateAttributes(&ra, 4))) return false;
        // Resizing and pixel-format conversion, so the encoder always gets
        // something it understands. Not used when the streams are only copied.
        if (!copy) ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ra->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
        if (FAILED(MFCreateSourceReaderFromURL(job.path.c_str(), ra.Get(), &rd))) {
            error = T(L"Не вдалося прочитати файл");
            return false;
        }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);

        for (DWORD i = 0; i < 64; ++i) {
            ComPtr<IMFMediaType> nt;
            HRESULT hr = rd->GetNativeMediaType(i, 0, &nt);
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr)) continue;
            GUID mj{};
            if (FAILED(nt->GetGUID(MF_MT_MAJOR_TYPE, &mj))) continue;
            if (mj == MFMediaType_Audio && srcA < 0) srcA = (int)i;
            else if (mj == MFMediaType_Video && srcV < 0 && fmt.video) srcV = (int)i;
        }
        if (srcA < 0 && srcV < 0) { error = T(L"У файлі немає доріжок для конвертації"); return false; }

        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                   MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            duration = (double)pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv);
        return true;
    }

    ComPtr<IMFMediaType> videoOutType(IMFMediaType* in) {
        ComPtr<IMFMediaType> t;
        if (FAILED(MFCreateMediaType(&t))) return nullptr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, fmt.vcodec);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::max(100000, job.videoKbps * 1000));
        copyAttr(in, t.Get(), MF_MT_FRAME_SIZE);
        copyAttr(in, t.Get(), MF_MT_FRAME_RATE);
        copyAttr(in, t.Get(), MF_MT_PIXEL_ASPECT_RATIO);
        if (fmt.vcodec == MFVideoFormat_H264)
            t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        return t;
    }

    bool setupVideo() {
        if (srcV < 0) return true;
        ComPtr<IMFMediaType> in;

        if (copy) {
            if (FAILED(rd->GetNativeMediaType((DWORD)srcV, 0, &in))) return false;
            GUID sub{};
            in->GetGUID(MF_MT_SUBTYPE, &sub);
            if (!containerAccepts(fmt, MFMediaType_Video, sub)) return false;
            if (FAILED(rd->SetStreamSelection((DWORD)srcV, TRUE))) return false;
            if (FAILED(wr->AddStream(in.Get(), &dstV))) return false;
            if (FAILED(wr->SetInputMediaType(dstV, in.Get(), nullptr))) return false;
            haveV = true;
            return true;
        }

        // Decode to NV12, at the size the user asked for.
        ComPtr<IMFMediaType> native;
        if (FAILED(rd->GetNativeMediaType((DWORD)srcV, 0, &native))) return false;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (!w || !h) return false;
        if (job.scale > 0.f && job.scale < 0.999f) {
            // Encoders want even dimensions; H.264 in particular.
            w = (UINT32)std::max(16.0, floor(w * job.scale / 2.0) * 2.0);
            h = (UINT32)std::max(16.0, floor(h * job.scale / 2.0) * 2.0);
        }

        ComPtr<IMFMediaType> want;
        if (FAILED(MFCreateMediaType(&want))) return false;
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, w, h);
        if (FAILED(rd->SetStreamSelection((DWORD)srcV, TRUE))) return false;
        if (FAILED(rd->SetCurrentMediaType((DWORD)srcV, nullptr, want.Get()))) {
            // Some decoders will not resize; take the native size instead.
            MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, 0, 0);
            want->DeleteItem(MF_MT_FRAME_SIZE);
            if (FAILED(rd->SetCurrentMediaType((DWORD)srcV, nullptr, want.Get()))) return false;
        }
        if (FAILED(rd->GetCurrentMediaType((DWORD)srcV, &in))) return false;

        ComPtr<IMFMediaType> out = videoOutType(in.Get());
        if (!out) return false;
        if (FAILED(wr->AddStream(out.Get(), &dstV))) return false;
        if (FAILED(wr->SetInputMediaType(dstV, in.Get(), nullptr))) return false;
        haveV = true;
        return true;
    }

    bool setupAudio() {
        if (srcA < 0) return true;
        ComPtr<IMFMediaType> in;

        if (copy) {
            if (FAILED(rd->GetNativeMediaType((DWORD)srcA, 0, &in))) return false;
            GUID sub{};
            in->GetGUID(MF_MT_SUBTYPE, &sub);
            if (!containerAccepts(fmt, MFMediaType_Audio, sub)) return false;
            if (FAILED(rd->SetStreamSelection((DWORD)srcA, TRUE))) return false;
            if (FAILED(wr->AddStream(in.Get(), &dstA))) return false;
            if (FAILED(wr->SetInputMediaType(dstA, in.Get(), nullptr))) return false;
            haveA = true;
            return true;
        }

        ComPtr<IMFMediaType> want = pcmType(nullptr);
        if (!want) return false;
        want->DeleteItem(MF_MT_AUDIO_SAMPLES_PER_SECOND);
        want->DeleteItem(MF_MT_AUDIO_NUM_CHANNELS);
        want->DeleteItem(MF_MT_AUDIO_BLOCK_ALIGNMENT);
        want->DeleteItem(MF_MT_AUDIO_AVG_BYTES_PER_SECOND);
        if (FAILED(rd->SetStreamSelection((DWORD)srcA, TRUE))) return false;
        HRESULT hra = rd->SetCurrentMediaType((DWORD)srcA, nullptr, want.Get());
        if (FAILED(hra)) { pgLog("convert: audio decode type 0x%08X", hra); return false; }
        if (FAILED(rd->GetCurrentMediaType((DWORD)srcA, &in))) return false;

        ComPtr<IMFMediaType> out;
        if (fmt.audio == MFAudioFormat_PCM)       out = pcmType(in.Get());
        else if (fmt.audio == MFAudioFormat_FLAC) out = losslessType(fmt.audio, in.Get());
        else                                      out = pickAudioType(fmt.audio, job.audioKbps, in.Get());
        if (!out) { error = T(L"Немає кодувальника для цього формату"); return false; }
        HRESULT hr = wr->AddStream(out.Get(), &dstA);
        if (FAILED(hr)) { pgLog("convert: audio AddStream 0x%08X", hr); return false; }
        hr = wr->SetInputMediaType(dstA, in.Get(), nullptr);
        if (FAILED(hr)) { pgLog("convert: audio SetInputMediaType 0x%08X", hr); return false; }
        haveA = true;
        return true;
    }

    bool openWriter() {
        ComPtr<IMFAttributes> wa;
        if (FAILED(MFCreateAttributes(&wa, 3))) return false;
        wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE, fmt.container);
        wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        HRESULT hr = MFCreateSinkWriterFromURL(job.outPath.c_str(), nullptr, wa.Get(), &wr);
        if (FAILED(hr)) {
            pgLog("convert: MFCreateSinkWriterFromURL 0x%08X", hr);
            error = T(L"Не вдалося створити файл");
            return false;
        }
        return true;
    }

    // Pull samples until the source runs out, handing each to the writer.
    bool pump() {
        HRESULT hrb = wr->BeginWriting();
        if (FAILED(hrb)) { pgLog("convert: BeginWriting 0x%08X", hrb); return false; }
        const double total = duration > 0.05 ? duration : 0;
        for (;;) {
            if (aborted()) return false;
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = rd->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0,
                                        &stream, &flags, &ts, &sample);
            if (FAILED(hr)) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (flags & MF_SOURCE_READERF_ERROR) return false;
            if (!sample) continue;

            DWORD dst = 0;
            if (haveV && (int)stream == srcV)      dst = dstV;
            else if (haveA && (int)stream == srcA) dst = dstA;
            else continue;

            HRESULT hrw = wr->WriteSample(dst, sample.Get());
            if (FAILED(hrw)) { pgLog("convert: WriteSample 0x%08X", hrw); return false; }
            if (total > 0)
                p->progress.store(clampf((float)(ts / 1e7 / total), 0.f, 1.f),
                                  std::memory_order_relaxed);
        }
        HRESULT hrf = wr->Finalize();
        if (FAILED(hrf)) pgLog("convert: Finalize 0x%08X", hrf);
        return SUCCEEDED(hrf);
    }

    bool run() {
        duration = 0;
        if (!openReader()) return false;
        if (!openWriter()) return false;
        if (!setupVideo()) return false;
        if (!setupAudio()) return false;
        if (!haveA && !haveV) return false;
        return pump();
    }
};

} // namespace

static void runConvertJob(MediaConverter::Impl* p, const ConvertJob& job, ConvertResult& res) {
    double t0 = nowSec();
    const auto& fs = mediaFormats();
    res.id = job.id;
    res.path = job.path;
    res.outPath = job.outPath;
    res.batchIndex = job.batchIndex;
    res.batchTotal = job.batchTotal;
    res.srcBytes = sizeOfFile(job.path);
    if (fs.empty()) { res.error = T(L"Немає доступних кодувальників"); return; }
    const MediaFormat& fmt = fs[clampi(job.format, 0, (int)fs.size() - 1)];

    p->progress.store(0.f, std::memory_order_relaxed);

    // Copying the streams is worth trying first: it is instant, it is lossless,
    // and it either works outright or fails before a single sample is written.
    bool done = false;
    if (job.copyStreams) {
        Attempt a{ p, job, fmt, true, 0 };
        done = a.run();
        if (done) { res.copied = true; res.seconds = a.duration; }
        else DeleteFileW(job.outPath.c_str());
    }
    if (!done && !p->cancel.load()) {
        Attempt a{ p, job, fmt, false, 0 };
        done = a.run();
        res.seconds = a.duration;
        if (!done) {
            DeleteFileW(job.outPath.c_str());
            res.error = a.error.empty() ? T(L"Не вдалося конвертувати") : a.error;
        }
    }

    if (p->cancel.load() && !done) {
        DeleteFileW(job.outPath.c_str());
        res.error = T(L"Скасовано");
    }

    res.saved = done;
    res.ok = done;
    res.outBytes = done ? sizeOfFile(job.outPath) : 0;
    res.ms = (nowSec() - t0) * 1000.0;
    p->progress.store(done ? 1.f : 0.f, std::memory_order_relaxed);
}

MediaConverter::~MediaConverter() { stop(); }

void MediaConverter::start(HWND notify) {
    if (p_) return;
    auto* p = new Impl();
    p->notify = notify;
    p_ = p;

    p->th = std::thread([p] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        for (;;) {
            ConvertJob job;
            {
                std::unique_lock<std::mutex> lk(p->m);
                p->cv.wait(lk, [p] { return p->quit || !p->jobs.empty(); });
                if (p->quit) break;
                job = std::move(p->jobs.front());
                p->jobs.pop_front();
            }
            p->working.store(true);
            p->cancel.store(false);

            ConvertResult res;
            if (mf) runConvertJob(p, job, res);
            else {
                res.path = job.path;
                res.outPath = job.outPath;
                res.error = T(L"Media Foundation недоступна");
            }

            {
                std::lock_guard<std::mutex> lk(p->om);
                p->out.push_back(std::move(res));
            }
            p->working.store(false);
            if (p->notify) PostMessageW(p->notify, WM_PG_CONVERT, 0, 0);
        }
        if (mf) MFShutdown();
        CoUninitialize();
    });
}

void MediaConverter::stop() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->quit = true;
        p_->jobs.clear();
    }
    p_->cancel.store(true);
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    delete p_;
    p_ = nullptr;
}

void MediaConverter::submit(ConvertJob job) {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->jobs.push_back(std::move(job));
    }
    p_->cv.notify_one();
}

bool MediaConverter::pop(ConvertResult& out) {
    if (!p_) return false;
    std::lock_guard<std::mutex> lk(p_->om);
    if (p_->out.empty()) return false;
    out = std::move(p_->out.front());
    p_->out.erase(p_->out.begin());
    return true;
}

bool MediaConverter::busy() const {
    if (!p_) return false;
    if (p_->working.load()) return true;
    std::lock_guard<std::mutex> lk(p_->m);
    return !p_->jobs.empty();
}

float MediaConverter::progress() const {
    return p_ ? p_->progress.load(std::memory_order_relaxed) : 0.f;
}

int MediaConverter::queued() const {
    if (!p_) return 0;
    std::lock_guard<std::mutex> lk(p_->m);
    return (int)p_->jobs.size();
}

void MediaConverter::cancel() {
    if (!p_) return;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        p_->jobs.clear();
    }
    p_->cancel.store(true);
}
