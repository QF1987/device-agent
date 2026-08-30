#include "capability/capability_manifest.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using device_agent::capability::Platform;
using device_agent::capability::RuntimeCapabilities;

void assert_manifest(Platform platform,
                     const std::vector<std::string>& expected,
                     const RuntimeCapabilities& runtime = {}) {
    const auto actual = device_agent::capability::build_manifest(platform, runtime);
    assert(actual == expected);
    assert(std::set<std::string>(actual.begin(), actual.end()).size() == actual.size());
    assert(!actual.empty());
    assert(actual.front() == "capability_manifest_v1");
    for (const auto& forbidden : {"config_apply", "command_firmware"}) {
        assert(std::find(actual.begin(), actual.end(), forbidden) == actual.end());
    }

    terminal_agent::v1::DeviceCapability proto;
    proto.add_supported_features("stale_feature");
    device_agent::capability::populate_manifest(platform, runtime, &proto);
    assert(proto.proto_version() == 1);
    assert(proto.supported_features_size() == static_cast<int>(expected.size()));
    for (int i = 0; i < proto.supported_features_size(); ++i) {
        assert(proto.supported_features(i) == expected[static_cast<std::size_t>(i)]);
    }
}

}  // namespace

int main() {
    const std::vector<std::string> windows{
        "capability_manifest_v1", "heartbeat_basic", "heartbeat_metrics", "status_report",
        "inventory_basic", "network_info", "command_reboot", "release_download",
        "release_install_silent", "release_install_attended",
    };
    assert_manifest(Platform::kWindows, windows);

    auto windows_vnc = windows;
    windows_vnc.emplace_back("remote_desktop_vnc");
    assert_manifest(Platform::kWindows, windows_vnc, RuntimeCapabilities{true});

    assert_manifest(Platform::kAndroid, {
        "capability_manifest_v1", "heartbeat_basic", "status_report", "command_reboot",
        "release_download", "p2p_config", "command_app_upgrade",
    });
    assert_manifest(Platform::kLinux, {
        "capability_manifest_v1", "heartbeat_basic", "status_report", "command_reboot",
        "release_download",
    });
    assert_manifest(Platform::kMacOS, {
        "capability_manifest_v1", "heartbeat_basic", "status_report", "command_reboot",
        "release_download",
    });

    const auto unknown = device_agent::capability::build_manifest(Platform::kUnknown);
    assert(unknown == std::vector<std::string>{"capability_manifest_v1"});

#if defined(__ANDROID__)
    assert(device_agent::capability::current_platform() == Platform::kAndroid);
#elif defined(_WIN32)
    assert(device_agent::capability::current_platform() == Platform::kWindows);
#elif defined(__APPLE__)
    assert(device_agent::capability::current_platform() == Platform::kMacOS);
#elif defined(__linux__)
    assert(device_agent::capability::current_platform() == Platform::kLinux);
#else
    assert(device_agent::capability::current_platform() == Platform::kUnknown);
#endif

    std::cout << "capability_manifest_test PASS\n";
    return 0;
}
