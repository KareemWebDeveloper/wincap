#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <stdio.h>
#include <vector>
#include <functional>
#include <cstdint>
#include "capture.h"
#include "device_list.h"

using FrameCallback = std::function<void(
    const uint8_t* bgra, int width, int height, int stride,
    int64_t timestampHns)>;

// --------------------------------------------------------------------------
// BitBlt-based window capture (works on all Windows versions)
// --------------------------------------------------------------------------
bool CaptureWindowBitBlt(HWND hwnd, const RecordingOptions& opts,
                          FrameCallback callback, HANDLE stopEvent)
{
    RECT r{};
    if (!GetClientRect(hwnd, &r)) {
        fprintf(stderr, "CaptureWindow: GetClientRect failed\n");
        return false;
    }
    int w = r.right  - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "CaptureWindow: window has zero size\n");
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP hbm  = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    SelectObject(hdcMem, hbm);

    int64_t frameTimestampHns = 0;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    int64_t frameTicks    = freq.QuadPart / opts.fps;
    LARGE_INTEGER nextQPC{};
    QueryPerformanceCounter(&nextQPC);

    printf("Recording window (HWND=%p, %dx%d)\n", (void*)hwnd, w, h);

    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (now.QuadPart < nextQPC.QuadPart) {
            DWORD ms = (DWORD)((nextQPC.QuadPart - now.QuadPart) * 1000 / freq.QuadPart);
            if (ms > 1) WaitForSingleObject(stopEvent, ms - 1);
            continue;
        }
        nextQPC.QuadPart += frameTicks;

        // Check if window is still alive and get current size
        if (!IsWindow(hwnd)) {
            fprintf(stderr, "CaptureWindow: window was closed\n");
            break;
        }
        GetClientRect(hwnd, &r);
        int newW = r.right - r.left;
        int newH = r.bottom - r.top;
        if (newW != w || newH != h) {
            // Resize bitmap — recreate
            w = newW; h = newH;
            bmi.bmiHeader.biWidth  = w;
            bmi.bmiHeader.biHeight = -h;
            DeleteObject(hbm);
            hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
            SelectObject(hdcMem, hbm);
        }

        // PW_RENDERFULLCONTENT captures GPU-composited content (browsers, games, etc.)
        // Falls back to BitBlt if PrintWindow fails (e.g. some UWP apps).
        BOOL ok = PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT);
        if (!ok) {
            HDC hdcWin = GetDC(hwnd);
            ok = BitBlt(hdcMem, 0, 0, w, h, hdcWin, 0, 0, SRCCOPY | CAPTUREBLT);
            ReleaseDC(hwnd, hdcWin);
        }

        if (ok && w > 0 && h > 0) {
            // DIBSection pixels are BGR; the 4th byte is 0xFF from PrintWindow
            // or 0x00 from BitBlt. Either way ARGB32 treats it as the alpha channel
            // and MF ignores alpha for H.264 encoding.
            callback(static_cast<const uint8_t*>(pixels), w, h,
                     w * 4, frameTimestampHns);
        }

        frameTimestampHns += 10'000'000LL / opts.fps;
    }

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return true;
}

// --------------------------------------------------------------------------
// Public entry: pick window by index, then capture it
// --------------------------------------------------------------------------
bool CaptureWindow(int windowIndex, const RecordingOptions& opts,
                   FrameCallback callback, HANDLE stopEvent)
{
    auto windows = GetWindows();
    if (windowIndex < 0 || windowIndex >= (int)windows.size()) {
        fprintf(stderr, "CaptureWindow: index %d out of range (have %zu)\n",
                windowIndex, windows.size());
        return false;
    }

    HWND hwnd = windows[windowIndex].hwnd;
    // Bring window to front so BitBlt gets actual content
    SetForegroundWindow(hwnd);
    Sleep(50);

    return CaptureWindowBitBlt(hwnd, opts, callback, stopEvent);
}
