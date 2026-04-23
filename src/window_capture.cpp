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
// Formats a Win32 error code as a human-readable string
// --------------------------------------------------------------------------
static void LogWin32Error(const char* context, DWORD err) {
    char msg[512] = {};
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, msg, (DWORD)sizeof(msg), nullptr);
    // Strip trailing newline from FormatMessage
    size_t len = strlen(msg);
    while (len > 0 && (msg[len-1] == '\n' || msg[len-1] == '\r')) msg[--len] = '\0';
    fprintf(stderr, "[wincap] %s failed (error %lu): %s\n", context, err, msg);
}

// --------------------------------------------------------------------------
// BitBlt-based window capture (works on all Windows versions)
// --------------------------------------------------------------------------
bool CaptureWindowBitBlt(HWND hwnd, const RecordingOptions& opts,
                          FrameCallback callback, HANDLE stopEvent)
{
    RECT r{};
    if (!GetClientRect(hwnd, &r)) {
        LogWin32Error("GetClientRect", GetLastError());
        return false;
    }
    int w = r.right  - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "[wincap] Window has zero size (%dx%d) — is it minimized?\n", w, h);
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        LogWin32Error("GetDC(screen)", GetLastError());
        return false;
    }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        LogWin32Error("CreateCompatibleDC", GetLastError());
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!hbm || !pixels) {
        LogWin32Error("CreateDIBSection", GetLastError());
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    SelectObject(hdcMem, hbm);

    int64_t frameTimestampHns = 0;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    int64_t frameTicks = freq.QuadPart / opts.fps;
    LARGE_INTEGER nextQPC{};
    QueryPerformanceCounter(&nextQPC);

    // Log window title for easier diagnosis
    char title[256] = {};
    GetWindowTextA(hwnd, title, (int)sizeof(title));
    printf("[wincap] Recording window HWND=%p \"%s\" (%dx%d) @ %d fps\n",
           (void*)hwnd, title, w, h, opts.fps);

    int frameCount = 0;
    int printWindowFailCount = 0;
    int bitbltFailCount = 0;
    const char* stopReason = nullptr; // null = normal stop via event

    LARGE_INTEGER loopStart{};
    QueryPerformanceCounter(&loopStart);

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
            stopReason = "window was closed";
            fprintf(stderr, "[wincap] STOP: Window HWND=%p was closed after %d frames\n",
                    (void*)hwnd, frameCount);
            fflush(stderr);
            break;
        }

        if (!GetClientRect(hwnd, &r)) {
            LogWin32Error("GetClientRect (frame loop)", GetLastError());
            continue;
        }
        int newW = r.right - r.left;
        int newH = r.bottom - r.top;

        if (newW <= 0 || newH <= 0) {
            // Window minimized — skip frame silently
            frameTimestampHns += 10'000'000LL / opts.fps;
            continue;
        }

        if (newW != w || newH != h) {
            printf("[wincap] Window resized: %dx%d -> %dx%d\n", w, h, newW, newH);
            w = newW; h = newH;
            bmi.bmiHeader.biWidth  = w;
            bmi.bmiHeader.biHeight = -h;
            DeleteObject(hbm);
            pixels = nullptr;
            hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!hbm || !pixels) {
                stopReason = "CreateDIBSection failed after resize";
                LogWin32Error("CreateDIBSection (resize)", GetLastError());
                fprintf(stderr, "[wincap] STOP: %s\n", stopReason);
                fflush(stderr);
                break;
            }
            SelectObject(hdcMem, hbm);
        }

        // PW_RENDERFULLCONTENT captures GPU-composited content (browsers, games, etc.)
        // Falls back to BitBlt if PrintWindow fails (e.g. some UWP apps).
        BOOL ok = PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT);
        if (!ok) {
            DWORD pwErr = GetLastError();
            printWindowFailCount++;
            if (printWindowFailCount == 1 || printWindowFailCount % 30 == 0) {
                fprintf(stderr, "[wincap] PrintWindow failed (error %lu) — falling back to BitBlt"
                        " (failure #%d)\n", pwErr, printWindowFailCount);
            }
            HDC hdcWin = GetDC(hwnd);
            if (!hdcWin) {
                LogWin32Error("GetDC(hwnd) for BitBlt fallback", GetLastError());
                frameTimestampHns += 10'000'000LL / opts.fps;
                continue;
            }
            ok = BitBlt(hdcMem, 0, 0, w, h, hdcWin, 0, 0, SRCCOPY | CAPTUREBLT);
            if (!ok) {
                DWORD bbErr = GetLastError();
                bitbltFailCount++;
                if (bitbltFailCount == 1 || bitbltFailCount % 30 == 0) {
                    fprintf(stderr, "[wincap] BitBlt also failed (error %lu) — frame dropped"
                            " (failure #%d)\n", bbErr, bitbltFailCount);
                }
            }
            ReleaseDC(hwnd, hdcWin);
        }

        if (ok && w > 0 && h > 0) {
            // DIBSection pixels are BGR; the 4th byte is 0xFF from PrintWindow
            // or 0x00 from BitBlt. Either way ARGB32 treats it as the alpha channel
            // and MF ignores alpha for H.264 encoding.
            callback(static_cast<const uint8_t*>(pixels), w, h,
                     w * 4, frameTimestampHns);
            frameCount++;
            if (frameCount == 1) {
                printf("[wincap] First frame captured successfully\n");
                fflush(stdout);
            }
            if (frameCount % 300 == 0) {
                LARGE_INTEGER nowHb{}; QueryPerformanceCounter(&nowHb);
                double elapsed = (double)(nowHb.QuadPart - loopStart.QuadPart) / freq.QuadPart;
                printf("[wincap] Still recording — %d frames, %.1fs elapsed\n",
                       frameCount, elapsed);
                fflush(stdout);
            }
        }

        frameTimestampHns += 10'000'000LL / opts.fps;
    }

    LARGE_INTEGER loopEnd{};
    QueryPerformanceCounter(&loopEnd);
    double totalSec = (double)(loopEnd.QuadPart - loopStart.QuadPart) / freq.QuadPart;

    if (stopReason) {
        fprintf(stderr, "[wincap] *** Recording stopped unexpectedly: %s ***\n", stopReason);
        fflush(stderr);
    } else {
        printf("[wincap] Recording stopped normally (stop event signalled)\n");
    }
    printf("[wincap] Window capture finished: %d frames captured, %.1fs elapsed, "
           "%d PrintWindow failures, %d BitBlt failures\n",
           frameCount, totalSec, printWindowFailCount, bitbltFailCount);
    fflush(stdout);

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return true;
}

// --------------------------------------------------------------------------
// Public entry: capture a specific window by HWND
// --------------------------------------------------------------------------
bool CaptureWindow(HWND hwnd, const RecordingOptions& opts,
                   FrameCallback callback, HANDLE stopEvent)
{
    if (!hwnd || !IsWindow(hwnd)) {
        fprintf(stderr, "[wincap] Invalid window handle\n");
        return false;
    }

    char title[256] = {};
    GetWindowTextA(hwnd, title, (int)sizeof(title));
    printf("[wincap] Selected window: HWND=%p \"%s\"\n",
           (void*)hwnd, title);

    if (!IsWindowVisible(hwnd)) {
        fprintf(stderr, "[wincap] Warning: window \"%s\" is not visible — capture may be blank\n",
                title);
    }

    RECT r{};
    GetWindowRect(hwnd, &r);
    printf("[wincap] Window screen rect: (%ld,%ld)-(%ld,%ld), size %ldx%ld\n",
           r.left, r.top, r.right, r.bottom,
           r.right - r.left, r.bottom - r.top);

    // Bring window to front so BitBlt gets actual content
    SetForegroundWindow(hwnd);
    Sleep(50);

    return CaptureWindowBitBlt(hwnd, opts, callback, stopEvent);
}
