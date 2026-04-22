#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "encoder.h"
#include "capture.h"

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) { fprintf(stderr, "Muxer: %s (hr=0x%08X)\n", msg, (unsigned)hr); return false; }

// --------------------------------------------------------------------------
// Muxer::Init
// --------------------------------------------------------------------------
bool Muxer::Init(const RecordingOptions& opts,
                 int videoWidth, int videoHeight)
{
    m_outW = (opts.outputWidth  > 0) ? opts.outputWidth  : videoWidth;
    m_outH = (opts.outputHeight > 0) ? opts.outputHeight : videoHeight;
    // Ensure even dimensions for H.264/H.265
    m_outW &= ~1;
    m_outH &= ~1;

    m_frameDurationHns = 10'000'000LL / opts.fps;

    // --- Create SinkWriter ------------------------------------------------
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    std::wstring wpath = Utf8ToWide(opts.output);
    HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs,
                                           &m_writer);
    attrs->Release();
    CHECK_HR(hr, "MFCreateSinkWriterFromURL");

    // --- Video output type (H.264 or H.265) -------------------------------
    {
        IMFMediaType* outType = nullptr;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE,   MFMediaType_Video);

        bool h265 = (opts.encoder == "h265" || opts.encoder == "hevc");
        outType->SetGUID(MF_MT_SUBTYPE,
                         h265 ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE,
                           (UINT32)(opts.videoBitrateKbps * 1000));
        outType->SetUINT32(MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, m_outW, m_outH);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, opts.fps, 1);
        MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (!h265)
            outType->SetUINT32(MF_MT_MPEG2_PROFILE,
                               eAVEncH264VProfile_High);

        DWORD streamIdx = 0;
        hr = m_writer->AddStream(outType, &streamIdx);
        outType->Release();
        CHECK_HR(hr, "AddStream(video)");
        m_videoStreamIndex = static_cast<int>(streamIdx);
    }

    // --- Video input type (BGRA) ------------------------------------------
    {
        IMFMediaType* inType = nullptr;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_ARGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, m_outW, m_outH);
        MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, opts.fps, 1);
        MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_DEFAULT_STRIDE, m_outW * 4);
        inType->SetUINT32(MF_MT_SAMPLE_SIZE, m_outW * m_outH * 4);

        hr = m_writer->SetInputMediaType(m_videoStreamIndex, inType, nullptr);
        inType->Release();
        CHECK_HR(hr, "SetInputMediaType(video)");
    }

    // --- Audio streams ----------------------------------------------------
    bool wantAudio = !opts.noAudio &&
                     (opts.audioInputIndex >= 0 || opts.audioOutputIndex >= 0);

    if (wantAudio) {
        const int SR = 44100;
        const int CH = 2;

        // Output: AAC
        IMFMediaType* outType = nullptr;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_AAC);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    16);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        CH);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);

        DWORD streamIdx = 0;
        hr = m_writer->AddStream(outType, &streamIdx);
        outType->Release();
        CHECK_HR(hr, "AddStream(audio)");
        m_audioStreamIndex = static_cast<int>(streamIdx);

        // Input: PCM float32
        IMFMediaType* inType = nullptr;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_Float);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    32);
        inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
        inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        CH);
        inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     CH * 4);
        inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, SR * CH * 4);

        hr = m_writer->SetInputMediaType(m_audioStreamIndex, inType, nullptr);
        inType->Release();
        CHECK_HR(hr, "SetInputMediaType(audio)");
    }

    hr = m_writer->BeginWriting();
    CHECK_HR(hr, "BeginWriting");

    return true;
}

// --------------------------------------------------------------------------
// Muxer::WriteVideoFrame
// --------------------------------------------------------------------------
bool Muxer::WriteVideoFrame(const uint8_t* bgra, int width, int height,
                             int stride, int64_t timestampHns)
{
    if (!m_writer || m_videoStreamIndex < 0) {
        static bool logged = false;
        if (!logged) {
            fprintf(stderr, "[Muxer] ERROR: WriteVideoFrame called with invalid state (writer=%p, streamIdx=%d)\n",
                    (void*)m_writer, m_videoStreamIndex);
            fflush(stderr);
            logged = true;
        }
        return false;
    }

    // Scale / centre-crop to output dimensions if needed
    const bool needsScale = (width != m_outW || height != m_outH);

    DWORD bufSize = (DWORD)(m_outW * m_outH * 4);
    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bufSize, &buf);
    if (FAILED(hr)) {
        fprintf(stderr, "[Muxer] ERROR: MFCreateMemoryBuffer failed (0x%08X) for video frame\n", (unsigned)hr);
        fflush(stderr);
        return false;
    }

    BYTE* dst = nullptr;
    DWORD maxLen = 0, curLen = 0;
    buf->Lock(&dst, &maxLen, &curLen);

    if (!needsScale) {
        // Fast path: same size, just copy (respecting stride)
        for (int y = 0; y < m_outH; ++y)
            memcpy(dst + y * m_outW * 4,
                   bgra + y * stride,
                   m_outW * 4);
    } else {
        // Nearest-neighbour scale
        float sx = (float)width  / (float)m_outW;
        float sy = (float)height / (float)m_outH;
        for (int dy = 0; dy < m_outH; ++dy) {
            int sy0 = (int)(dy * sy);
            if (sy0 >= height) sy0 = height - 1;
            const uint8_t* srcRow = bgra + sy0 * stride;
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(dst + dy * m_outW * 4);
            for (int dx = 0; dx < m_outW; ++dx) {
                int sx0 = (int)(dx * sx);
                if (sx0 >= width) sx0 = width - 1;
                dstRow[dx] = reinterpret_cast<const uint32_t*>(srcRow)[sx0];
            }
        }
    }

    buf->SetCurrentLength(bufSize);
    buf->Unlock();

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buf);
    buf->Release();

    sample->SetSampleTime(timestampHns);
    sample->SetSampleDuration(m_frameDurationHns);

    std::lock_guard<std::mutex> lk(m_mutex);
    hr = m_writer->WriteSample(m_videoStreamIndex, sample);
    sample->Release();
    
    if (FAILED(hr)) {
        static int failCount = 0;
        failCount++;
        if (failCount == 1 || failCount % 30 == 0) {
            fprintf(stderr, "[Muxer] ERROR: WriteSample(video) failed (0x%08X) - failure #%d\n",
                    (unsigned)hr, failCount);
            fflush(stderr);
        }
    }
    
    return SUCCEEDED(hr);
}

// --------------------------------------------------------------------------
// Muxer::WriteAudioFrame
// --------------------------------------------------------------------------
bool Muxer::WriteAudioFrame(const float* samples, int numFrames,
                             int channels, int sampleRate,
                             int64_t timestampHns)
{
    if (!m_writer || m_audioStreamIndex < 0) {
        static bool logged = false;
        if (!logged) {
            fprintf(stderr, "[Muxer] ERROR: WriteAudioFrame called with invalid state (writer=%p, streamIdx=%d)\n",
                    (void*)m_writer, m_audioStreamIndex);
            fflush(stderr);
            logged = true;
        }
        return false;
    }

    DWORD byteCount = (DWORD)(numFrames * channels * sizeof(float));
    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(byteCount, &buf);
    if (FAILED(hr)) {
        fprintf(stderr, "[Muxer] ERROR: MFCreateMemoryBuffer failed (0x%08X) for audio frame\n", (unsigned)hr);
        fflush(stderr);
        return false;
    }

    BYTE* dst = nullptr;
    DWORD maxLen = 0, curLen = 0;
    buf->Lock(&dst, &maxLen, &curLen);
    memcpy(dst, samples, byteCount);
    buf->SetCurrentLength(byteCount);
    buf->Unlock();

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buf);
    buf->Release();

    // Duration in 100-ns
    int64_t durationHns = (int64_t)numFrames * 10'000'000LL / sampleRate;
    sample->SetSampleTime(timestampHns);
    sample->SetSampleDuration(durationHns);

    std::lock_guard<std::mutex> lk(m_mutex);
    hr = m_writer->WriteSample(m_audioStreamIndex, sample);
    sample->Release();
    
    if (FAILED(hr)) {
        static int failCount = 0;
        failCount++;
        if (failCount == 1 || failCount % 30 == 0) {
            fprintf(stderr, "[Muxer] ERROR: WriteSample(audio) failed (0x%08X) - failure #%d\n",
                    (unsigned)hr, failCount);
            fflush(stderr);
        }
    }
    
    return SUCCEEDED(hr);
}

// --------------------------------------------------------------------------
// Muxer::Finalize
// --------------------------------------------------------------------------
void Muxer::Finalize() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_finalized || !m_writer) return;
    
    printf("[Muxer] Finalizing recording...\n");
    fflush(stdout);
    
    m_finalized = true;
    HRESULT hr = m_writer->Finalize();
    
    if (FAILED(hr)) {
        fprintf(stderr, "[Muxer] ERROR: Finalize() failed (0x%08X)\n", (unsigned)hr);
        fflush(stderr);
    } else {
        printf("[Muxer] Finalize completed successfully\n");
        fflush(stdout);
    }
}

Muxer::~Muxer() {
    Finalize();
    if (m_writer) {
        m_writer->Release();
        m_writer = nullptr;
    }
}
