#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <atomic>
#include "capture.h"
#include "device_list.h"
#include "audio.h"
#include "encoder.h"

// Declared in screen_capture.cpp / window_capture.cpp
using FrameCallback = std::function<void(
    const uint8_t* bgra, int width, int height, int stride,
    int64_t timestampHns)>;

bool CaptureScreen(int screenIndex, const RecordingOptions& opts,
                   FrameCallback callback, HANDLE stopEvent);
bool CaptureWindow(HWND hwnd, const RecordingOptions& opts,
                   FrameCallback callback, HANDLE stopEvent);

// --------------------------------------------------------------------------
// CLI helpers
// --------------------------------------------------------------------------
static void PrintUsage() {
    printf(
        "Usage: wincap <command> [options]\n"
        "\n"
        "Commands:\n"
        "  list screens           List monitors\n"
        "  list windows           List visible windows\n"
        "  list audio-inputs      List microphone devices\n"
        "  list audio-outputs     List speaker/loopback devices\n"
        "  start [options]        Begin recording (blocks until stopped)\n"
        "  stop                   Signal a running instance to stop\n"
        "  status                 Check if a recording is active\n"
        "  help                   Show this message\n"
        "\n"
        "Start options:\n"
        "  --output <file>            Output file          (default: recording.mp4)\n"
        "  --screen <index>           Monitor index        (default: 0)\n"
        "  --window <index>           Capture a window by index\n"
        "  --window-hwnd <hwnd>       Capture a window by handle (most reliable)\n"
        "  --window-pid <pid>         Capture a window by process ID\n"
        "  --window-title <text>      Filter by window title (substring match)\n"
        "  --region <x,y,w,h>        Crop region on the screen\n"
        "  --fps <n>                  Frame rate           (default: 30)\n"
        "  --video-bitrate <kbps>     Video bitrate kbps  (default: 8000)\n"
        "  --encoder <h264|h265>      Codec               (default: h264)\n"
        "  --audio-input <index>      Mic device (-1=off)  (default: -1)\n"
        "  --audio-output <index>     Loopback (-1=off)    (default: -1)\n"
        "  --no-audio                 Disable all audio\n"
        "  --width <px>               Scale output width\n"
        "  --height <px>              Scale output height\n"
    );
}

static bool ParseInt(const char* s, int& out) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    out = (int)v;
    return true;
}

// Parse "--region x,y,w,h"
static bool ParseRegion(const char* s, int& x, int& y, int& w, int& h) {
    return sscanf(s, "%d,%d,%d,%d", &x, &y, &w, &h) == 4;
}

static bool ArgIs(const char* arg, const char* name) {
    return strcmp(arg, name) == 0;
}

static const char* NextArg(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        fprintf(stderr, "Option '%s' requires an argument\n", argv[i]);
        return nullptr;
    }
    return argv[++i];
}

// --------------------------------------------------------------------------
// Ctrl+C handler (also signals stop event)
// --------------------------------------------------------------------------
static HANDLE g_stopEvent = nullptr;

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT)
    {
        printf("\nInterrupted — stopping recording...\n");
        if (g_stopEvent) SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

// --------------------------------------------------------------------------
// 'start' command
// --------------------------------------------------------------------------
static int CmdStart(int argc, char** argv) {
    RecordingOptions opts;
    HWND targetWindowHwnd = nullptr;  // Can be set via --window-hwnd

    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (ArgIs(a, "--output")) {
            const char* v = NextArg(i, argc, argv);
            if (!v) return 1;
            opts.output = v;
        } else if (ArgIs(a, "--screen")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.screenIndex)) return 1;
        } else if (ArgIs(a, "--window")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.windowIndex)) return 1;
        } else if (ArgIs(a, "--window-hwnd")) {
            const char* v = NextArg(i, argc, argv);
            if (!v) return 1;
            // Parse hex HWND (e.g., 0x00000000001234AB)
            unsigned long long hwndValue = 0;
            if (sscanf(v, "0x%llX", &hwndValue) != 1 && sscanf(v, "%llX", &hwndValue) != 1) {
                fprintf(stderr, "--window-hwnd requires a valid hex value (e.g., 0x00000000001234AB)\n");
                return 1;
            }
            targetWindowHwnd = reinterpret_cast<HWND>(hwndValue);
        } else if (ArgIs(a, "--window-pid")) {
            const char* v = NextArg(i, argc, argv);
            long pid = 0;
            if (!v || !ParseInt(v, reinterpret_cast<int&>(pid)) || pid <= 0) {
                fprintf(stderr, "--window-pid requires a valid process ID\n");
                return 1;
            }
            opts.windowPid = static_cast<DWORD>(pid);
            opts.windowIndex = -2; // special marker: use PID
        } else if (ArgIs(a, "--window-title")) {
            const char* v = NextArg(i, argc, argv);
            if (!v) return 1;
            opts.windowTitle = v;
        } else if (ArgIs(a, "--region")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseRegion(v, opts.regionX, opts.regionY,
                                       opts.regionW, opts.regionH))
            {
                fprintf(stderr, "--region expects x,y,w,h (e.g. 0,0,1920,1080)\n");
                return 1;
            }
        } else if (ArgIs(a, "--fps")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.fps) || opts.fps <= 0) return 1;
        } else if (ArgIs(a, "--video-bitrate")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.videoBitrateKbps)) return 1;
        } else if (ArgIs(a, "--encoder")) {
            const char* v = NextArg(i, argc, argv);
            if (!v) return 1;
            opts.encoder = v;
            if (opts.encoder != "h264" && opts.encoder != "h265") {
                fprintf(stderr, "--encoder must be h264 or h265\n");
                return 1;
            }
        } else if (ArgIs(a, "--audio-input")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.audioInputIndex)) return 1;
        } else if (ArgIs(a, "--audio-output")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.audioOutputIndex)) return 1;
        } else if (ArgIs(a, "--no-audio")) {
            opts.noAudio = true;
        } else if (ArgIs(a, "--width")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.outputWidth)) return 1;
        } else if (ArgIs(a, "--height")) {
            const char* v = NextArg(i, argc, argv);
            if (!v || !ParseInt(v, opts.outputHeight)) return 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a);
            return 1;
        }
    }

    // --- Init COM & Media Foundation ---------------------------------------
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // --- Create stop event -------------------------------------------------
    HANDLE stopEvent = CreateOrOpenStopEvent(/*create=*/true);
    if (!stopEvent) {
        fprintf(stderr, "Failed to create stop event\n");
        return 1;
    }
    g_stopEvent = stopEvent;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // --- Determine capture dimensions by peeking at the source -------------
    // We do a quick preview capture just to get w/h before init-ing the muxer.
    // targetWindowHwnd is declared at function start and may already be set via --window-hwnd
    int capW = 0, capH = 0;

    // Priority 1: If user specified --width and --height, use those directly (always honor user choice)
    if (opts.outputWidth > 0 && opts.outputHeight > 0) {
        capW = opts.outputWidth;
        capH = opts.outputHeight;
    }
    // Priority 2: If HWND was specified directly, get its dimensions
    else if (targetWindowHwnd) {
        if (!IsWindow(targetWindowHwnd)) {
            fprintf(stderr, "Invalid or closed window handle: 0x%016llX\n",
                    reinterpret_cast<unsigned long long>(targetWindowHwnd));
            CloseHandle(stopEvent);
            MFShutdown();
            CoUninitialize();
            return 1;
        }
        RECT r{};
        GetWindowRect(targetWindowHwnd, &r);
        capW = r.right - r.left;
        capH = r.bottom - r.top;
        if (capW <= 0 || capH <= 0) {
            fprintf(stderr, "Window has invalid dimensions: %dx%d\n", capW, capH);
            CloseHandle(stopEvent);
            MFShutdown();
            CoUninitialize();
            return 1;
        }
    }
    // Priority 3: Get dimensions from window search by PID/title/index
    else if (opts.windowIndex >= -1) {  // -1 = screen, -2 = use PID, >= 0 = index
        auto windows = GetWindows();
        
        // Find window by PID and/or title if specified
        if (opts.windowPid > 0 || !opts.windowTitle.empty()) {
            bool found = false;
            for (auto& wi : windows) {
                // Match PID if specified
                bool pidMatch = (opts.windowPid == 0) || (wi.pid == opts.windowPid);
                // Match title if specified (case-insensitive substring)
                bool titleMatch = opts.windowTitle.empty();
                if (!opts.windowTitle.empty()) {
                    // Convert both to lowercase for case-insensitive comparison
                    std::string winTitle = wi.title;
                    std::string searchTitle = opts.windowTitle;
                    for (auto& c : winTitle) c = tolower(c);
                    for (auto& c : searchTitle) c = tolower(c);
                    titleMatch = (winTitle.find(searchTitle) != std::string::npos);
                }
                
                if (pidMatch && titleMatch) {
                    capW = wi.width;
                    capH = wi.height;
                    targetWindowHwnd = wi.hwnd;  // Store HWND for capture
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (opts.windowPid > 0 && !opts.windowTitle.empty()) {
                    fprintf(stderr, "Window with PID %lu and title containing \"%s\" not found\n",
                            static_cast<unsigned long>(opts.windowPid), opts.windowTitle.c_str());
                } else if (opts.windowPid > 0) {
                    fprintf(stderr, "Window with PID %lu not found\n",
                            static_cast<unsigned long>(opts.windowPid));
                } else {
                    fprintf(stderr, "Window with title containing \"%s\" not found\n",
                            opts.windowTitle.c_str());
                }
                fprintf(stderr, "Run 'wincap list windows' to see available windows\n");
                CloseHandle(stopEvent);
                MFShutdown();
                CoUninitialize();
                return 1;
            }
        }
        // Find window by index
        else if (opts.windowIndex >= 0 && opts.windowIndex < (int)windows.size()) {
            auto& wi = windows[opts.windowIndex];
            capW = wi.width;
            capH = wi.height;
            targetWindowHwnd = wi.hwnd;  // Store HWND for capture
        }
    }

    if (capW == 0) {
        // Fall back to screen dimensions
        auto monitors = GetMonitors();
        if (!monitors.empty()) {
            int idx = std::min(opts.screenIndex, (int)monitors.size() - 1);
            if (opts.regionW > 0 && opts.regionH > 0) {
                capW = opts.regionW;
                capH = opts.regionH;
            } else {
                capW = monitors[idx].width;
                capH = monitors[idx].height;
            }
        } else {
            capW = 1920; capH = 1080; // fallback
        }
    }

    // --- Init Muxer --------------------------------------------------------
    Muxer muxer;
    if (!muxer.Init(opts, capW, capH)) {
        fprintf(stderr, "Failed to initialize output muxer\n");
        CloseHandle(stopEvent);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    printf("Output: %s\n", opts.output.c_str());

    // --- Audio thread -------------------------------------------------------
    std::thread audioThread;
    bool wantAudio = !opts.noAudio &&
                     (opts.audioInputIndex >= 0 || opts.audioOutputIndex >= 0);

    if (wantAudio && muxer.HasAudio()) {
        audioThread = std::thread([&] {
            CaptureAudio(opts.audioInputIndex, opts.audioOutputIndex,
                [&](const float* samples, int frames, int ch, int sr,
                    int64_t ts)
                {
                    muxer.WriteAudioFrame(samples, frames, ch, sr, ts);
                },
                stopEvent);
        });
    }

    // --- Video capture (blocks until stop) ---------------------------------
    auto frameCallback = [&](const uint8_t* bgra, int w, int h, int stride,
                              int64_t ts)
    {
        if (!muxer.WriteVideoFrame(bgra, w, h, stride, ts)) {
            static int callbackFailCount = 0;
            callbackFailCount++;
            if (callbackFailCount == 1 || callbackFailCount % 30 == 0) {
                fprintf(stderr, "[main] ERROR: Muxer failed to write video frame - failure #%d\n",
                        callbackFailCount);
                fflush(stderr);
            }
        }
    };

    bool ok;
    printf("[main] Starting video capture...\n");
    fflush(stdout);
    
    // Use the HWND we found during dimension detection (avoids race condition)
    if (targetWindowHwnd) {
        // Get window info for logging
        char title[256] = {};
        GetWindowTextA(targetWindowHwnd, title, sizeof(title));
        DWORD pid = 0;
        GetWindowThreadProcessId(targetWindowHwnd, &pid);
        
        // Verify window still exists
        if (!IsWindow(targetWindowHwnd)) {
            fprintf(stderr, "Window disappeared before capture could start\n");
            SetEvent(stopEvent);
            if (audioThread.joinable()) audioThread.join();
            muxer.Finalize();
            CloseHandle(stopEvent);
            MFShutdown();
            CoUninitialize();
            return 1;
        }
        
        printf("[main] Capturing window HWND=%p: \"%s\" (PID %lu)\n",
               (void*)targetWindowHwnd, title, static_cast<unsigned long>(pid));
        fflush(stdout);
        ok = CaptureWindow(targetWindowHwnd, opts, frameCallback, stopEvent);
    } else {
        printf("[main] Capturing screen index %d\n", opts.screenIndex);
        fflush(stdout);
        ok = CaptureScreen(opts.screenIndex, opts, frameCallback, stopEvent);
    }
    
    printf("[main] Video capture function returned: %s\n", ok ? "SUCCESS" : "FAILURE");
    fflush(stdout);

    // --- Cleanup -----------------------------------------------------------
    printf("[main] Cleanup started - signaling stop event\n");
    fflush(stdout);
    
    SetEvent(stopEvent);  // ensure audio thread also exits

    if (audioThread.joinable()) {
        printf("[main] Waiting for audio thread to finish...\n");
        fflush(stdout);
        audioThread.join();
        printf("[main] Audio thread joined\n");
        fflush(stdout);
    }

    printf("[main] Finalizing muxer...\n");
    fflush(stdout);
    muxer.Finalize();
    printf("Saved: %s\n", opts.output.c_str());

    CloseHandle(stopEvent);
    MFShutdown();
    CoUninitialize();
    
    printf("[main] Exiting with code: %d\n", ok ? 0 : 1);
    fflush(stdout);
    
    return ok ? 0 : 1;
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const char* cmd = argv[1];

    // --- list ---
    if (ArgIs(cmd, "list")) {
        if (argc < 3) { fprintf(stderr, "Usage: wincap list <screens|windows|audio-inputs|audio-outputs>\n"); return 1; }
        const char* sub = argv[2];
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (ArgIs(sub, "screens"))       { ListScreens();      }
        else if (ArgIs(sub, "windows"))  { ListWindows();      }
        else if (ArgIs(sub, "audio-inputs"))  { ListAudioInputs();  }
        else if (ArgIs(sub, "audio-outputs")) { ListAudioOutputs(); }
        else { fprintf(stderr, "Unknown list target: %s\n", sub); CoUninitialize(); return 1; }
        CoUninitialize();
        return 0;
    }

    // --- start ---
    if (ArgIs(cmd, "start")) {
        return CmdStart(argc - 2, argv + 2);
    }

    // --- stop ---
    if (ArgIs(cmd, "stop")) {
        HANDLE ev = CreateOrOpenStopEvent(/*create=*/false);
        if (!ev) {
            fprintf(stderr, "No recording is active (or access denied)\n");
            return 1;
        }
        SetEvent(ev);
        CloseHandle(ev);
        printf("Stop signal sent.\n");
        return 0;
    }

    // --- status ---
    if (ArgIs(cmd, "status")) {
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, L"Global\\WincapStopEvent");
        if (ev) {
            // Event exists: check if it's signaled (recording stopped) or not
            DWORD w = WaitForSingleObject(ev, 0);
            CloseHandle(ev);
            if (w == WAIT_TIMEOUT)
                printf("Recording is ACTIVE.\n");
            else
                printf("No active recording (event is signaled or completed).\n");
        } else {
            printf("No active recording.\n");
        }
        return 0;
    }

    // --- help ---
    if (ArgIs(cmd, "help") || ArgIs(cmd, "--help") || ArgIs(cmd, "-h")) {
        PrintUsage();
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\nRun 'wincap help' for usage.\n", cmd);
    return 1;
}
