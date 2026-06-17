#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace device_agent::remotedesktop {

struct Rect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

struct ScreenFrame {
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t stride = 0;
    // Pixels are BGRA, 8 bits per component, top-to-bottom.
    std::vector<uint8_t> bgra;
    std::vector<Rect> dirty_rects;
};

class IScreenCapturer {
public:
    virtual ~IScreenCapturer() = default;

    virtual bool capture(ScreenFrame& frame, std::string& err) = 0;
};

}  // namespace device_agent::remotedesktop
