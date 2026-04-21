#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

struct MonitorInfo {
    int         index;
    std::string name;
    int         x, y, width, height;
    bool        isPrimary;
};

struct WindowInfo {
    int         index;
    std::string title;
    DWORD       pid;
    int         x, y, width, height;
    HWND        hwnd;
};

struct AudioDeviceInfo {
    int         index;
    std::string name;
    std::string id;   // IMMDevice endpoint ID
};

// Print to stdout
void ListScreens();
void ListWindows();
void ListAudioInputs();
void ListAudioOutputs();

// Return collections (used by capture code)
std::vector<MonitorInfo>     GetMonitors();
std::vector<WindowInfo>      GetWindows();
std::vector<AudioDeviceInfo> GetAudioInputDevices();
std::vector<AudioDeviceInfo> GetAudioOutputDevices();
