#pragma once
#include <functional>
#include <cstdint>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Called from audio capture thread with interleaved stereo float32 PCM.
// timestamp is in 100-nanosecond units (same as MF).
using AudioCallback = std::function<void(
    const float* samples,
    int          numFrames,
    int          channels,
    int          sampleRate,
    int64_t      timestampHns)>;

// Starts mic + loopback capture on the current thread (blocks until stopEvent).
// Pass -1 for inputDeviceIndex or outputDeviceIndex to disable that source.
bool CaptureAudio(
    int           inputDeviceIndex,
    int           outputDeviceIndex,
    AudioCallback callback,
    HANDLE        stopEvent);
