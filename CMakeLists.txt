cmake_minimum_required(VERSION 3.20)
project(wincap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the exe is self-contained
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(wincap
    src/main.cpp
    src/device_list.cpp
    src/screen_capture.cpp
    src/window_capture.cpp
    src/audio_capture.cpp
    src/encoder.cpp
    src/muxer.cpp
)

target_include_directories(wincap PRIVATE include)

target_compile_definitions(wincap PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(wincap PRIVATE
    mf mfuuid mfplat mfreadwrite
    ole32 oleaut32
    d3d11 dxgi
    avrt
    windowscodecs
    dwmapi
    user32 gdi32 kernel32
    strmiids
)

target_compile_options(wincap PRIVATE /W4 /WX- /wd4100 /wd4189)

install(TARGETS wincap RUNTIME DESTINATION .)
