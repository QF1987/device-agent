#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_screen_capturer.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

uint32_t unsignedArg(int argc, char** argv, const std::string& name, uint32_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != name) {
            continue;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(argv[i + 1], &end, 10);
        if (end != argv[i + 1] && *end == '\0' && parsed > 0) {
            return static_cast<uint32_t>(parsed);
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t frames = unsignedArg(argc, argv, "--frames", 100);
    const uint32_t interval_ms = unsignedArg(argc, argv, "--interval-ms", 100);
    SetEnvironmentVariableA("DEVICE_AGENT_FORCE_GDI", "1");

    device_agent::remotedesktop::windows::WindowsScreenCapturer capturer;
    uint64_t total_raw_bytes = 0;
    uint32_t zero_updates = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        device_agent::remotedesktop::ScreenFrame frame;
        std::string err;
        if (!capturer.capture(frame, err)) {
            std::cerr << "capture failed frame=" << i << " err=" << err << "\n";
            return 1;
        }
        uint64_t dirty_pixels = 0;
        for (const auto& rect : frame.dirty_rects) {
            dirty_pixels += static_cast<uint64_t>(rect.width) * rect.height;
        }
        const uint64_t raw_bytes = 4 + static_cast<uint64_t>(frame.dirty_rects.size()) * 12 +
                                   dirty_pixels * 4;
        const uint64_t full_raw_bytes = 4 + 12 +
                                        static_cast<uint64_t>(frame.width) * frame.height * 4;
        total_raw_bytes += raw_bytes;
        if (frame.dirty_rects.empty()) {
            ++zero_updates;
        }
        std::cout << "frame=" << i << " mode=" << capturer.lastCaptureMode()
                  << " size=" << frame.width << "x" << frame.height
                  << " rects=" << frame.dirty_rects.size()
                  << " dirty_pixels=" << dirty_pixels
                  << " estimated_raw_bytes=" << raw_bytes
                  << " full_raw_bytes=" << full_raw_bytes << "\n";
        Sleep(interval_ms);
    }
    std::cout << "summary frames=" << frames << " zero_updates=" << zero_updates
              << " estimated_raw_bytes=" << total_raw_bytes
              << " gdi_max_fps=" << capturer.gdiMaxFps()
              << " gdi_tile_size=" << capturer.gdiTileSize() << "\n";
    return 0;
}

#endif  // _WIN32
