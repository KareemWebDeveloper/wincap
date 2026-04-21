#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <functional>
#include <mutex>
#include <string>
#include "capture.h"

// Wraps IMFSinkWriter.
// Thread-safe for concurrent WriteVideoFrame / WriteAudioFrame calls.
class Muxer {
public:
    Muxer() = default;
    ~Muxer();

    // Call once before writing.
    // videoWidth/Height: actual capture dimensions (before any scaling).
    bool Init(const RecordingOptions& opts,
              int videoWidth, int videoHeight);

    // Raw BGRA frame (stride may be > width*4).
    // timestampHns: 100-ns units from start of recording.
    bool WriteVideoFrame(const uint8_t* bgra, int width, int height,
                         int stride, int64_t timestampHns);

    // Interleaved float32 PCM.
    bool WriteAudioFrame(const float* samples, int numFrames,
                         int channels, int sampleRate,
                         int64_t timestampHns);

    // Flush + finalize. Must be called exactly once before destruction.
    void Finalize();

    bool HasVideo() const { return m_videoStreamIndex >= 0; }
    bool HasAudio() const { return m_audioStreamIndex >= 0; }

private:
    IMFSinkWriter* m_writer          = nullptr;
    int            m_videoStreamIndex = -1;
    int            m_audioStreamIndex = -1;
    std::mutex     m_mutex;
    bool           m_finalized        = false;

    // Cached output dimensions for scaling
    int m_outW = 0, m_outH = 0;
    // Duration of one video frame in 100-ns
    int64_t m_frameDurationHns = 0;
};

// Helper: create the stop event used to signal wincap start to exit.
// Name: L"Global\\WincapStopEvent"
HANDLE CreateOrOpenStopEvent(bool create);
