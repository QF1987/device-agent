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
