#pragma once

#ifdef _WIN32

#include "remotedesktop/screen_capturer.h"

#include <windows.h>

namespace device_agent::remotedesktop::windows {

class WindowsScreenCapturer : public IScreenCapturer {
public:
    bool capture(ScreenFrame& frame, std::string& err) override;

private:
    bool captureWithGdi(ScreenFrame& frame, std::string& err);
};

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
