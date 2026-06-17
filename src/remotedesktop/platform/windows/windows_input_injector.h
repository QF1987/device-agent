#pragma once

#ifdef _WIN32

#include "remotedesktop/input_injector.h"

#include <windows.h>

namespace device_agent::remotedesktop::windows {

class WindowsInputInjector : public IInputInjector {
public:
    bool keyEvent(uint32_t rfb_keysym, bool down, std::string& err) override;
    bool pointerEvent(uint8_t button_mask, uint16_t x, uint16_t y, std::string& err) override;

private:
    uint8_t last_button_mask_ = 0;
};

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
