#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "audio.h"
#include "device_list.h"

#pragma comment(lib, "avrt.lib")

// --------------------------------------------------------------------------
// Helper: get IMMDevice by index from a collection
// --------------------------------------------------------------------------
static IMMDevice* GetDeviceByIndex(EDataFlow flow, int index) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;

    IMMDeviceCollection* collection = nullptr;
    enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (!collection) return nullptr;

    UINT count = 0;
    collection->GetCount(&count);
    IMMDevice* device = nullptr;
    if ((UINT)index < count)
        collection->Item((UINT)index, &device);
    collection->Release();
    return device;
}

// --------------------------------------------------------------------------
// One WASAPI reader (either capture or loopback)
// --------------------------------------------------------------------------
struct WasapiReader {
    IAudioClient*        client  = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX*        wfx     = nullptr;
    bool                 isLoopback = false;

    bool Init(IMMDevice* device, bool loopback) {
        isLoopback = loopback;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void**>(&client))))
            return false;

        if (FAILED(client->GetMixFormat(&wfx))) return false;

        // Force stereo float32 so our mixing is trivial
        // (if the device returns something else, WASAPI will re-sample for us)
        WAVEFORMATEXTENSIBLE* wfxEx = nullptr;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            wfxEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx);

        DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        // 200 ms buffer
        REFERENCE_TIME bufDur = 2'000'000;
        HRESULT hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, bufDur, 0, wfx, nullptr);
        if (FAILED(hr)) return false;

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&capture));
        return SUCCEEDED(hr);
    }

    bool Start() {
        return client && SUCCEEDED(client->Start());
    }

    void Stop() {
        if (client) client->Stop();
    }

    // Returns all available frames as interleaved float32 stereo.
    // Resamples from any format to float32 stereo (basic).
    void Drain(std::vector<float>& out) {
        if (!capture) return;

        UINT32 pktFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&pktFrames)) && pktFrames > 0) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            bool silence = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // Convert to float32 stereo
            int srcCh    = wfx->nChannels;
            int srcBits  = wfx->wBitsPerSample;
            bool srcFloat = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                            ([&]{
                                if (wfx->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return false;
                                auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx);
                                return ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                            }());

            for (UINT32 f = 0; f < frames; ++f) {
                // Left
                float L = 0.f, R = 0.f;
                if (!silence) {
                    if (srcFloat) {
                        const float* p = reinterpret_cast<const float*>(
                            data + f * wfx->nBlockAlign);
                        L = p[0];
                        R = (srcCh > 1) ? p[1] : p[0];
                    } else if (srcBits == 16) {
                        const int16_t* p = reinterpret_cast<const int16_t*>(
                            data + f * wfx->nBlockAlign);
                        L = p[0] / 32768.f;
                        R = (srcCh > 1) ? p[1] / 32768.f : L;
                    } else if (srcBits == 32) {
                        const int32_t* p = reinterpret_cast<const int32_t*>(
                            data + f * wfx->nBlockAlign);
                        L = p[0] / (float)0x80000000u;
                        R = (srcCh > 1) ? p[1] / (float)0x80000000u : L;
                    }
                }
                out.push_back(L);
                out.push_back(R);
            }

            capture->ReleaseBuffer(frames);
        }
    }

    ~WasapiReader() {
        if (capture) { capture->Release(); capture = nullptr; }
        if (wfx)     { CoTaskMemFree(wfx); wfx = nullptr; }
        if (client)  { client->Release();  client = nullptr; }
    }
};

// --------------------------------------------------------------------------
// CaptureAudio
// --------------------------------------------------------------------------
bool CaptureAudio(int inputDeviceIndex, int outputDeviceIndex,
                  AudioCallback callback, HANDLE stopEvent)
{
    const int OUT_SR  = 44100;
    const int OUT_CH  = 2;
    // ~10 ms delivery period
    const int FRAMES_PER_CALLBACK = OUT_SR / 100;

    // Boost thread priority for audio
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    WasapiReader mic, loop;
    bool hasMic  = false;
    bool hasLoop = false;

    if (inputDeviceIndex >= 0) {
        IMMDevice* dev = GetDeviceByIndex(eCapture, inputDeviceIndex);
        if (dev) {
            hasMic = mic.Init(dev, false);
            dev->Release();
            if (!hasMic)
                fprintf(stderr, "Warning: failed to init mic capture\n");
        }
    }

    if (outputDeviceIndex >= 0) {
        IMMDevice* dev = GetDeviceByIndex(eRender, outputDeviceIndex);
        if (dev) {
            hasLoop = loop.Init(dev, true);
            dev->Release();
            if (!hasLoop)
                fprintf(stderr, "Warning: failed to init loopback capture\n");
        }
    }

    if (!hasMic && !hasLoop) {
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        return false;
    }

    if (hasMic)  mic.Start();
    if (hasLoop) loop.Start();

    int64_t timestamp = 0;
    const int64_t ticksPerFrame = 10'000'000LL / OUT_SR;

    std::vector<float> micBuf, loopBuf, mixed;
    mixed.reserve(FRAMES_PER_CALLBACK * OUT_CH);

    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        Sleep(10);  // ~10 ms polling

        micBuf.clear();
        loopBuf.clear();

        if (hasMic)  mic.Drain(micBuf);
        if (hasLoop) loop.Drain(loopBuf);

        // Figure out how many stereo frames we have
        size_t micFrames  = micBuf.size()  / OUT_CH;
        size_t loopFrames = loopBuf.size() / OUT_CH;
        size_t maxFrames  = std::max(micFrames, loopFrames);
        if (maxFrames == 0) continue;

        mixed.resize(maxFrames * OUT_CH, 0.f);
        for (size_t i = 0; i < maxFrames * OUT_CH; ++i) {
            float m = (i < micBuf.size())  ? micBuf[i]  : 0.f;
            float l = (i < loopBuf.size()) ? loopBuf[i] : 0.f;
            float v = m + l;
            // Soft clip
            if      (v >  1.f) v =  1.f;
            else if (v < -1.f) v = -1.f;
            mixed[i] = v;
        }

        callback(mixed.data(), (int)maxFrames, OUT_CH, OUT_SR, timestamp);
        timestamp += (int64_t)maxFrames * 10'000'000LL / OUT_SR;
    }

    if (hasMic)  mic.Stop();
    if (hasLoop) loop.Stop();

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    return true;
}
