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
bool CaptureWindow(int windowIndex, const RecordingOptions& opts,
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
        "  --window <index>           Capture a window instead of a screen\n"
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
    int capW = 0, capH = 0;

    if (opts.windowIndex >= 0) {
        auto windows = GetWindows();
        if (opts.windowIndex < (int)windows.size()) {
            auto& wi = windows[opts.windowIndex];
            capW = wi.width;
            capH = wi.height;
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
        muxer.WriteVideoFrame(bgra, w, h, stride, ts);
    };

    bool ok;
    if (opts.windowIndex >= 0) {
        ok = CaptureWindow(opts.windowIndex, opts, frameCallback, stopEvent);
    } else {
        ok = CaptureScreen(opts.screenIndex, opts, frameCallback, stopEvent);
    }

    // --- Cleanup -----------------------------------------------------------
    SetEvent(stopEvent);  // ensure audio thread also exits

    if (audioThread.joinable()) audioThread.join();

    muxer.Finalize();
    printf("Saved: %s\n", opts.output.c_str());

    CloseHandle(stopEvent);
    MFShutdown();
    CoUninitialize();
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
