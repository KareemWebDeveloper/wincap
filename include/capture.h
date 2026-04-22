#pragma once
#include <string>
#include <cstdint>

struct RecordingOptions {
    std::string output        = "recording.mp4";
    int  screenIndex          = 0;
    int  windowIndex          = -1;   // -1 = use screen
    DWORD windowPid           = 0;    // 0 = use windowIndex instead
    std::string windowTitle   = "";   // Filter by title (substring match)
    // Crop region; all -1 means full screen/window
    int  regionX = -1, regionY = -1;
    int  regionW = -1, regionH = -1;
    int  fps                  = 30;
    int  videoBitrateKbps     = 8000;
    std::string encoder       = "h264"; // "h264" | "h265"
    int  audioInputIndex      = -1;   // -1 = disabled
    int  audioOutputIndex     = -1;   // -1 = disabled
    bool noAudio              = false;
    int  outputWidth          = 0;    // 0 = match source
    int  outputHeight         = 0;    // 0 = match source
};
