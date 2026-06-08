#include "client/network_info.h"

#include <cstdlib>
#include <iostream>

template <typename T, typename U>
void require_equal(const T& got, const U& want, const char* field) {
    if (got != want) {
        std::cerr << field << " mismatch" << std::endl;
        std::exit(1);
    }
}

void require_true(bool got, const char* field) {
    if (!got) {
        std::cerr << field << " mismatch" << std::endl;
        std::exit(1);
    }
}

void require_false(bool got, const char* field) {
    if (got) {
        std::cerr << field << " mismatch" << std::endl;
        std::exit(1);
    }
}

int main() {
    device_agent::set_network_info_provider([] {
        device_agent::NetworkInfoSnapshot info;
        info.gateway_mac = "aa:bb:cc:dd:ee:ff";
        info.bssid = "11:22:33:44:55:66";
        info.lan_ip = "192.168.1.23";
        info.lan_cidr = "192.168.1.0/24";
        info.public_ip = "203.0.113.7";
        info.net_type = terminal_agent::v1::WIFI;
        info.is_metered = true;
        info.is_roaming = false;
        return info;
    });

    terminal_agent::v1::NetworkInfo proto;
    device_agent::populate_network_info_proto(&proto);
    require_equal(proto.gateway_mac(), std::string("aa:bb:cc:dd:ee:ff"), "gateway_mac");
    require_equal(proto.bssid(), std::string("11:22:33:44:55:66"), "bssid");
    require_equal(proto.lan_ip(), std::string("192.168.1.23"), "lan_ip");
    require_equal(proto.lan_cidr(), std::string("192.168.1.0/24"), "lan_cidr");
    require_equal(proto.public_ip(), std::string("203.0.113.7"), "public_ip");
    require_equal(proto.net_type(), terminal_agent::v1::WIFI, "net_type");
    require_true(proto.is_metered(), "is_metered");
    require_false(proto.is_roaming(), "is_roaming");

    device_agent::set_network_info_provider(nullptr);
    return 0;
}
