#pragma once

#include <functional>
#include <string>

#include "terminal_agent/v1/device.pb.h"

namespace device_agent {

struct NetworkInfoSnapshot {
    std::string gateway_mac;
    std::string bssid;
    std::string lan_ip;
    std::string lan_cidr;
    std::string public_ip;
    terminal_agent::v1::NetworkType net_type = terminal_agent::v1::NET_UNKNOWN;
    bool is_metered = false;
    bool is_roaming = false;
};

using NetworkInfoProvider = std::function<NetworkInfoSnapshot()>;

void set_network_info_provider(NetworkInfoProvider provider);
NetworkInfoSnapshot collect_network_info();
void populate_network_info_proto(terminal_agent::v1::NetworkInfo* out);

}  // namespace device_agent
