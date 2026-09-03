#pragma once

#include <string>
#include <vector>

#include "terminal_agent/v1/device.pb.h"

namespace device_agent::capability {

enum class Platform {
    kWindows,
    kAndroid,
    kLinux,
    kMacOS,
    kUnknown,
};

struct RuntimeCapabilities {
    bool remote_desktop_vnc = false;
    // ADR-20260831-01 D5：仅 Windows P2P production build（option=ON）且
    // manager/config/network/fallback 初始化完整时为 true；非 P2P build /
    // 初始化失败保持 false → manifest 不声明 p2p_config。
    bool windows_p2p_ready = false;
};

Platform current_platform();
std::vector<std::string> build_manifest(Platform platform,
                                        const RuntimeCapabilities& runtime = {});
void populate_manifest(Platform platform,
                       const RuntimeCapabilities& runtime,
                       terminal_agent::v1::DeviceCapability* out);
void populate_current_manifest(const RuntimeCapabilities& runtime,
                               terminal_agent::v1::DeviceCapability* out);

}  // namespace device_agent::capability
