#pragma once

#include "remotedesktop/input_injector.h"
#include "remotedesktop/screen_capturer.h"

#include <iostream>
#include <mutex>
#include <string>

namespace device_agent::remotedesktop::test {

class TestPatternCapturer : public IScreenCapturer {
public:
    TestPatternCapturer(uint16_t width = 800, uint16_t height = 600);

    bool capture(ScreenFrame& frame, std::string& err) override;

private:
    uint16_t width_;
    uint16_t height_;
    uint32_t tick_ = 0;
};

class LoggingInputInjector : public IInputInjector {
public:
    explicit LoggingInputInjector(std::ostream& out = std::cout);

    bool keyEvent(uint32_t rfb_keysym, bool down, std::string& err) override;
    bool pointerEvent(uint8_t button_mask, uint16_t x, uint16_t y, std::string& err) override;

private:
    std::ostream& out_;
    std::mutex mu_;
};

}  // namespace device_agent::remotedesktop::test
