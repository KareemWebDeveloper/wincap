#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <dxgi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "device_list.h"

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------
static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// --------------------------------------------------------------------------
// Monitors
// --------------------------------------------------------------------------
struct EnumMonitorCtx {
    std::vector<MonitorInfo>* out;
    int idx;
};

static BOOL CALLBACK EnumMonitorProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumMonitorCtx*>(lp);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hm, &mi);

    MonitorInfo info{};
    info.index     = ctx->idx++;
    info.name      = mi.szDevice;
    info.x         = mi.rcMonitor.left;
    info.y         = mi.rcMonitor.top;
    info.width     = mi.rcMonitor.right  - mi.rcMonitor.left;
    info.height    = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ctx->out->push_back(info);
    return TRUE;
}

std::vector<MonitorInfo> GetMonitors() {
    std::vector<MonitorInfo> result;
    EnumMonitorCtx ctx{ &result, 0 };
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc,
                        reinterpret_cast<LPARAM>(&ctx));
    return result;
}

void ListScreens() {
    auto monitors = GetMonitors();
    printf("%-4s  %-20s  %-10s  %s\n", "IDX", "DEVICE", "RESOLUTION", "POSITION");
    for (auto& m : monitors) {
        printf("%-4d  %-20s  %dx%d    (%d,%d)%s\n",
               m.index, m.name.c_str(),
               m.width, m.height,
               m.x, m.y,
               m.isPrimary ? "  [primary]" : "");
    }
}

// --------------------------------------------------------------------------
// Windows
// --------------------------------------------------------------------------
struct EnumWindowCtx {
    std::vector<WindowInfo>* out;
};

static BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[512]{};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (!title[0]) return TRUE;

    // Skip windows without a proper frame
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) return TRUE;

    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    auto* ctx = reinterpret_cast<EnumWindowCtx*>(lp);
    WindowInfo wi{};
    wi.index  = static_cast<int>(ctx->out->size());
    wi.title  = title;
    wi.pid    = pid;
    wi.x      = r.left;
    wi.y      = r.top;
    wi.width  = r.right - r.left;
    wi.height = r.bottom - r.top;
    wi.hwnd   = hwnd;
    ctx->out->push_back(wi);
    return TRUE;
}

std::vector<WindowInfo> GetWindows() {
    std::vector<WindowInfo> result;
    EnumWindowCtx ctx{ &result };
    EnumWindows(EnumWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return result;
}

void ListWindows() {
    auto windows = GetWindows();
    printf("%-4s  %-16s  %-7s  %-12s  %s\n", "IDX", "HWND", "PID", "SIZE", "TITLE");
    for (auto& w : windows) {
        char size[32];
        snprintf(size, sizeof(size), "%dx%d", w.width, w.height);
        printf("%-4d  0x%016llX  %-7lu  %-12s  %s\n",
               w.index,
               reinterpret_cast<unsigned long long>(w.hwnd),
               static_cast<unsigned long>(w.pid),
               size, w.title.c_str());
    }
}

// --------------------------------------------------------------------------
// Audio devices (common helper)
// --------------------------------------------------------------------------
static std::vector<AudioDeviceInfo> GetAudioDevices(EDataFlow flow) {
    std::vector<AudioDeviceInfo> result;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return result;

    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              &collection))) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) continue;

        IPropertyStore* props = nullptr;
        device->OpenPropertyStore(STGM_READ, &props);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string name;
        if (props && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv))
            && pv.vt == VT_LPWSTR)
        {
            name = WideToUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
        if (props) props->Release();

        LPWSTR idRaw = nullptr;
        device->GetId(&idRaw);
        std::string id = idRaw ? WideToUtf8(idRaw) : "";
        if (idRaw) CoTaskMemFree(idRaw);

        device->Release();

        AudioDeviceInfo di{};
        di.index = static_cast<int>(result.size());
        di.name  = name;
        di.id    = id;
        result.push_back(di);
    }

    collection->Release();
    enumerator->Release();
    return result;
}

std::vector<AudioDeviceInfo> GetAudioInputDevices() {
    return GetAudioDevices(eCapture);
}

std::vector<AudioDeviceInfo> GetAudioOutputDevices() {
    return GetAudioDevices(eRender);
}

void ListAudioInputs() {
    auto devs = GetAudioInputDevices();
    printf("%-4s  %s\n", "IDX", "NAME");
    for (auto& d : devs)
        printf("%-4d  %s\n", d.index, d.name.c_str());
    if (devs.empty()) printf("(none found)\n");
}

void ListAudioOutputs() {
    auto devs = GetAudioOutputDevices();
    printf("%-4s  %s\n", "IDX", "NAME");
    for (auto& d : devs)
        printf("%-4d  %s\n", d.index, d.name.c_str());
    if (devs.empty()) printf("(none found)\n");
}
