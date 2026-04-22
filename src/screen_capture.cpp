#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <vector>
#include <functional>
#include <cstdint>
#include "capture.h"
#include "device_list.h"

// Called with raw BGRA pixels for each captured frame.
// timestampHns is in 100-nanosecond units.
using FrameCallback = std::function<void(
    const uint8_t* bgra, int width, int height, int stride,
    int64_t timestampHns)>;

// --------------------------------------------------------------------------
// DXGI Desktop Duplication screen capture
// --------------------------------------------------------------------------
bool CaptureScreen(int screenIndex, const RecordingOptions& opts,
                   FrameCallback callback, HANDLE stopEvent)
{
    // --- D3D11 device -------------------------------------------------------
    ID3D11Device*        d3dDevice  = nullptr;
    ID3D11DeviceContext* d3dContext  = nullptr;
    D3D_FEATURE_LEVEL    featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, &featureLevel, &d3dContext);
    if (FAILED(hr)) {
        fprintf(stderr, "CaptureScreen: D3D11CreateDevice failed (0x%08X)\n", (unsigned)hr);
        return false;
    }

    // --- Find the right output ----------------------------------------------
    IDXGIDevice*  dxgiDevice  = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    d3dDevice->QueryInterface(__uuidof(IDXGIDevice),
                              reinterpret_cast<void**>(&dxgiDevice));
    dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();

    IDXGIOutput* dxgiOutput = nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIOutput* out = nullptr;
        if (dxgiAdapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
        if ((int)i == screenIndex) { dxgiOutput = out; break; }
        out->Release();
    }
    dxgiAdapter->Release();

    if (!dxgiOutput) {
        fprintf(stderr, "CaptureScreen: screen index %d not found\n", screenIndex);
        d3dContext->Release();
        d3dDevice->Release();
        return false;
    }

    // --- DXGI Output Duplication -------------------------------------------
    IDXGIOutput1* dxgiOutput1 = nullptr;
    dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1),
                               reinterpret_cast<void**>(&dxgiOutput1));
    dxgiOutput->Release();

    IDXGIOutputDuplication* duplication = nullptr;
    hr = dxgiOutput1->DuplicateOutput(d3dDevice, &duplication);
    dxgiOutput1->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "CaptureScreen: DuplicateOutput failed (0x%08X)\n", (unsigned)hr);
        d3dContext->Release();
        d3dDevice->Release();
        return false;
    }

    // --- Get dimensions -----------------------------------------------------
    DXGI_OUTDUPL_DESC duplDesc{};
    duplication->GetDesc(&duplDesc);
    int srcW = (int)duplDesc.ModeDesc.Width;
    int srcH = (int)duplDesc.ModeDesc.Height;

    // Apply crop region
    int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
    if (opts.regionW > 0 && opts.regionH > 0) {
        cropX = std::max(0, opts.regionX);
        cropY = std::max(0, opts.regionY);
        cropW = std::min(opts.regionW, srcW - cropX);
        cropH = std::min(opts.regionH, srcH - cropY);
    }

    // --- Staging texture (CPU-readable) ------------------------------------
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width              = (UINT)srcW;
    stagingDesc.Height             = (UINT)srcH;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);

    int64_t  startTime    = 0;
    LARGE_INTEGER qpc0{}; QueryPerformanceCounter(&qpc0);
    LARGE_INTEGER freq{};  QueryPerformanceFrequency(&freq);

    int64_t nextFrameTime = 0;  // in QPC ticks
    int64_t frameTicks    = freq.QuadPart / opts.fps;
    int64_t frameTimestampHns = 0;

    printf("Recording screen %d (%dx%d", screenIndex, srcW, srcH);
    if (opts.regionW > 0) printf(" crop %d,%d %dx%d", cropX, cropY, cropW, cropH);
    printf(") — press Ctrl+C or run 'wincap stop' to stop\n");
    fflush(stdout);

    int frameCount = 0;
    const char* stopReason = nullptr; // null = normal stop via event
    LARGE_INTEGER loopStart{}; QueryPerformanceCounter(&loopStart);

    std::vector<uint8_t> cropBuf;

    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        // Throttle to fps
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (now.QuadPart < nextFrameTime) {
            DWORD waitMs = (DWORD)((nextFrameTime - now.QuadPart) * 1000 / freq.QuadPart);
            if (waitMs > 1)
                WaitForSingleObject(stopEvent, waitMs - 1);
            continue;
        }
        nextFrameTime = now.QuadPart + frameTicks;

        IDXGIResource* resource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        hr = duplication->AcquireNextFrame(17, &frameInfo, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            stopReason = "display mode changed (DXGI_ERROR_ACCESS_LOST)";
            fprintf(stderr, "CaptureScreen: STOP — display mode changed, stopping\n");
            fflush(stderr);
            break;
        }
        if (FAILED(hr)) {
            static char reasonBuf[64];
            snprintf(reasonBuf, sizeof(reasonBuf),
                     "AcquireNextFrame failed (0x%08X)", (unsigned)hr);
            stopReason = reasonBuf;
            fprintf(stderr, "CaptureScreen: STOP — %s\n", stopReason);
            fflush(stderr);
            break;
        }

        ID3D11Texture2D* tex = nullptr;
        resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(&tex));
        resource->Release();

        // Copy entire texture to staging
        d3dContext->CopyResource(staging, tex);
        tex->Release();

        duplication->ReleaseFrame();

        // Map and read pixels
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            const uint8_t* src    = static_cast<const uint8_t*>(mapped.pData);
            int            srcStride = (int)mapped.RowPitch;

            if (cropX == 0 && cropY == 0 && cropW == srcW && cropH == srcH) {
                // No crop — pass directly
                callback(src, srcW, srcH, srcStride, frameTimestampHns);
            } else {
                // Crop: copy region into contiguous buffer
                cropBuf.resize(cropW * cropH * 4);
                for (int y = 0; y < cropH; ++y)
                    memcpy(cropBuf.data() + y * cropW * 4,
                           src + (cropY + y) * srcStride + cropX * 4,
                           cropW * 4);
                callback(cropBuf.data(), cropW, cropH, cropW * 4,
                         frameTimestampHns);
            }

            d3dContext->Unmap(staging, 0);

            frameCount++;
            if (frameCount == 1) {
                printf("CaptureScreen: first frame captured successfully\n");
                fflush(stdout);
            }
            if (frameCount % 300 == 0) {
                LARGE_INTEGER nowHb{}; QueryPerformanceCounter(&nowHb);
                double elapsed = (double)(nowHb.QuadPart - loopStart.QuadPart) / freq.QuadPart;
                printf("CaptureScreen: still recording — %d frames, %.1fs elapsed\n",
                       frameCount, elapsed);
                fflush(stdout);
            }
        }

        frameTimestampHns += 10'000'000LL / opts.fps;
    }

    LARGE_INTEGER loopEnd{}; QueryPerformanceCounter(&loopEnd);
    double totalSec = (double)(loopEnd.QuadPart - loopStart.QuadPart) / freq.QuadPart;

    if (stopReason) {
        fprintf(stderr, "CaptureScreen: *** Recording stopped unexpectedly: %s ***\n", stopReason);
        fflush(stderr);
    } else {
        printf("CaptureScreen: Recording stopped normally (stop event signalled)\n");
    }
    printf("CaptureScreen: finished — %d frames captured, %.1fs elapsed\n",
           frameCount, totalSec);
    fflush(stdout);

    staging->Release();
    duplication->Release();
    d3dContext->Release();
    d3dDevice->Release();
    return true;
}
