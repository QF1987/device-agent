#include "remotedesktop/test/test_pattern.h"

#include <algorithm>
#include <iomanip>

namespace device_agent::remotedesktop::test {

TestPatternCapturer::TestPatternCapturer(uint16_t width, uint16_t height)
    : width_(std::max<uint16_t>(width, 64)), height_(std::max<uint16_t>(height, 64)) {}

bool TestPatternCapturer::capture(ScreenFrame& frame, std::string&) {
    frame.width = width_;
    frame.height = height_;
    frame.stride = static_cast<uint32_t>(frame.width) * 4;
    frame.bgra.assign(static_cast<size_t>(frame.stride) * frame.height, 0);

    const uint16_t box_size = 96;
    const uint16_t max_x = frame.width > box_size ? frame.width - box_size : 1;
    const uint16_t max_y = frame.height > box_size ? frame.height - box_size : 1;
    const uint16_t box_x = static_cast<uint16_t>((tick_ * 7) % max_x);
    const uint16_t box_y = static_cast<uint16_t>((tick_ * 5) % max_y);

    for (uint16_t y = 0; y < frame.height; ++y) {
        for (uint16_t x = 0; x < frame.width; ++x) {
            const size_t offset = static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x) * 4;
            const bool in_box = x >= box_x && x < box_x + box_size && y >= box_y && y < box_y + box_size;
            const bool grid = (x % 80 == 0) || (y % 80 == 0);
            frame.bgra[offset + 0] = static_cast<uint8_t>(in_box ? 0x20 : (grid ? 0x90 : (x + tick_) & 0xff));
            frame.bgra[offset + 1] = static_cast<uint8_t>(in_box ? 0x60 : (grid ? 0x90 : (y + tick_) & 0xff));
            frame.bgra[offset + 2] = static_cast<uint8_t>(in_box ? 0xff : (grid ? 0x90 : (x + y) & 0xff));
            frame.bgra[offset + 3] = 0xff;
        }
    }

    frame.dirty_rects = {Rect{0, 0, frame.width, frame.height}};
    ++tick_;
    return true;
}

LoggingInputInjector::LoggingInputInjector(std::ostream& out) : out_(out) {}

bool LoggingInputInjector::keyEvent(uint32_t rfb_keysym, bool down, std::string&) {
    std::lock_guard<std::mutex> lock(mu_);
    out_ << "RFB key " << (down ? "down" : "up") << " keysym=0x"
         << std::hex << rfb_keysym << std::dec << std::endl;
    return true;
}

bool LoggingInputInjector::pointerEvent(uint8_t button_mask, uint16_t x, uint16_t y, std::string&) {
    std::lock_guard<std::mutex> lock(mu_);
    out_ << "RFB pointer buttons=" << static_cast<int>(button_mask)
         << " x=" << x << " y=" << y << std::endl;
    return true;
}

}  // namespace device_agent::remotedesktop::test
