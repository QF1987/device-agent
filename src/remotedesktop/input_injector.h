#pragma once

#include <cstdint>
#include <string>

namespace device_agent::remotedesktop {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;

    virtual bool keyEvent(uint32_t rfb_keysym, bool down, std::string& err) = 0;
    virtual bool pointerEvent(uint8_t button_mask, uint16_t x, uint16_t y, std::string& err) = 0;
};

}  // namespace device_agent::remotedesktop
