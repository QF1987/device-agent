#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>

namespace device_agent::remotedesktop::windows {

class WindowsRemoteControlBadge {
public:
    ~WindowsRemoteControlBadge();

    bool show(std::string& err);
    void hide();

private:
    HANDLE process_ = nullptr;
};

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
