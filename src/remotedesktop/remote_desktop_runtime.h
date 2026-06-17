#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace device_agent::remotedesktop {

struct RemoteDesktopRuntimeConfig {
    std::string relay_endpoint;
    std::string device_id;
    std::string token;
    std::string server_name;
    bool insecure_tls = false;
    bool launch_active_session = true;
    int heartbeat_seconds = 5;
    int fallback_width = 1366;
    int fallback_height = 768;

    bool enabled() const {
        return !relay_endpoint.empty() && !device_id.empty() && !token.empty();
    }
};

bool runRemoteDesktopChild(const RemoteDesktopRuntimeConfig& config,
                           std::atomic<bool>& stop,
                           std::string& err);

class RemoteDesktopRuntime {
public:
    explicit RemoteDesktopRuntime(RemoteDesktopRuntimeConfig config);
    ~RemoteDesktopRuntime();

    bool start(std::string& err);
    void stop();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace device_agent::remotedesktop
