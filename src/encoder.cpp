#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "encoder.h"

// Named event used to signal stop between processes.
HANDLE CreateOrOpenStopEvent(bool create) {
    const wchar_t* name = L"Global\\WincapStopEvent";
    if (create) {
        // Create manual-reset, initially unsignaled
        HANDLE h = CreateEventW(nullptr, TRUE, FALSE, name);
        if (!h) {
            // May already exist from a crashed previous run — open it
            h = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
        }
        return h;
    } else {
        return OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    }
}
