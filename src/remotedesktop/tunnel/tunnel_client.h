#pragma once

#include "remotedesktop/rfb/rfb_server.h"

#include <atomic>
#include <functional>
#include <string>

namespace device_agent::remotedesktop::tunnel {

struct TunnelClientConfig {
    std::string relay_host;
    std::string relay_port;
    std::string device_id;
    std::string token;
    std::string server_name;
    bool insecure_tls = false;
    int screen_w = 0;
    int screen_h = 0;
    int heartbeat_seconds = 30;
};

class TunnelClient {
public:
    using BadgeCallback = std::function<void(bool on)>;

    TunnelClient(TunnelClientConfig config, rfb::RfbServer& rfb_server, BadgeCallback badge_callback = {});

    bool run(std::atomic<bool>& stop, std::string& err);

private:
    bool runOnce(std::atomic<bool>& stop, std::string& err);
    void handleOpen(const std::string& stream_id);

    TunnelClientConfig config_;
    rfb::RfbServer& rfb_server_;
    BadgeCallback badge_callback_;
};

bool splitHostPort(const std::string& endpoint, std::string& host, std::string& port, std::string& err);

}  // namespace device_agent::remotedesktop::tunnel
