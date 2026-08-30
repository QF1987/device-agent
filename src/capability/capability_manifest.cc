#include "capability/capability_manifest.h"

#include <algorithm>

namespace device_agent::capability {
namespace {

void append_unique(std::vector<std::string>& features, const char* feature) {
    if (std::find(features.begin(), features.end(), feature) == features.end()) {
        features.emplace_back(feature);
    }
}

}  // namespace

Platform current_platform() {
#if defined(__ANDROID__)
    return Platform::kAndroid;
#elif defined(_WIN32)
    return Platform::kWindows;
#elif defined(__APPLE__)
    return Platform::kMacOS;
#elif defined(__linux__)
    return Platform::kLinux;
#else
    return Platform::kUnknown;
#endif
}

std::vector<std::string> build_manifest(Platform platform,
                                        const RuntimeCapabilities& runtime) {
    std::vector<std::string> features;
    append_unique(features, "capability_manifest_v1");

    switch (platform) {
        case Platform::kWindows:
            append_unique(features, "heartbeat_basic");
            append_unique(features, "heartbeat_metrics");
            append_unique(features, "status_report");
            append_unique(features, "inventory_basic");
            append_unique(features, "network_info");
            append_unique(features, "command_reboot");
            append_unique(features, "release_download");
            append_unique(features, "release_install_silent");
            append_unique(features, "release_install_attended");
            if (runtime.remote_desktop_vnc) {
                append_unique(features, "remote_desktop_vnc");
            }
            break;
        case Platform::kAndroid:
            append_unique(features, "heartbeat_basic");
            append_unique(features, "status_report");
            append_unique(features, "command_reboot");
            append_unique(features, "release_download");
            append_unique(features, "p2p_config");
            append_unique(features, "command_app_upgrade");
            break;
        case Platform::kLinux:
        case Platform::kMacOS:
            append_unique(features, "heartbeat_basic");
            append_unique(features, "status_report");
            append_unique(features, "command_reboot");
            append_unique(features, "release_download");
            break;
        case Platform::kUnknown:
            break;
    }
    return features;
}

void populate_manifest(Platform platform,
                       const RuntimeCapabilities& runtime,
                       terminal_agent::v1::DeviceCapability* out) {
    if (out == nullptr) {
        return;
    }
    out->set_proto_version(1);
    out->clear_supported_features();
    for (const auto& feature : build_manifest(platform, runtime)) {
        out->add_supported_features(feature);
    }
}

void populate_current_manifest(const RuntimeCapabilities& runtime,
                               terminal_agent::v1::DeviceCapability* out) {
    populate_manifest(current_platform(), runtime, out);
}

}  // namespace device_agent::capability
