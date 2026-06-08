#include "client/network_info.h"

#if !defined(__ANDROID__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include <mutex>

namespace device_agent {

namespace {

std::mutex g_provider_mu;
NetworkInfoProvider g_provider;

#if !defined(__ANDROID__)
int prefix_len_from_netmask(sockaddr* netmask) {
    if (netmask == nullptr || netmask->sa_family != AF_INET) {
        return 0;
    }
    auto* addr = reinterpret_cast<sockaddr_in*>(netmask);
    uint32_t mask = ntohl(addr->sin_addr.s_addr);
    int bits = 0;
    while (mask & 0x80000000u) {
        bits++;
        mask <<= 1;
    }
    return bits;
}

std::string cidr_from_addr_and_netmask(const sockaddr_in* addr, sockaddr* netmask, int prefix) {
    if (addr == nullptr || netmask == nullptr || netmask->sa_family != AF_INET || prefix <= 0) {
        return "";
    }
    auto* mask = reinterpret_cast<sockaddr_in*>(netmask);
    in_addr network{};
    network.s_addr = addr->sin_addr.s_addr & mask->sin_addr.s_addr;
    char network_ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &network, network_ip, sizeof(network_ip)) == nullptr) {
        return "";
    }
    return std::string(network_ip) + "/" + std::to_string(prefix);
}
#endif

NetworkInfoSnapshot collect_default_network_info() {
    NetworkInfoSnapshot info;
#if !defined(__ANDROID__)
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return info;
    }
    for (ifaddrs* it = ifaddr; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_LOOPBACK) != 0 || (it->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == nullptr) {
            continue;
        }
        info.lan_ip = ip;
        int prefix = prefix_len_from_netmask(it->ifa_netmask);
        info.lan_cidr = cidr_from_addr_and_netmask(addr, it->ifa_netmask, prefix);
        info.net_type = terminal_agent::v1::ETHERNET;
        break;
    }
    freeifaddrs(ifaddr);
#endif
    return info;
}

}  // namespace

void set_network_info_provider(NetworkInfoProvider provider) {
    std::lock_guard<std::mutex> lock(g_provider_mu);
    g_provider = std::move(provider);
}

NetworkInfoSnapshot collect_network_info() {
    NetworkInfoProvider provider;
    {
        std::lock_guard<std::mutex> lock(g_provider_mu);
        provider = g_provider;
    }
    if (provider) {
        return provider();
    }
    return collect_default_network_info();
}

void populate_network_info_proto(terminal_agent::v1::NetworkInfo* out) {
    if (out == nullptr) {
        return;
    }
    const NetworkInfoSnapshot info = collect_network_info();
    out->set_gateway_mac(info.gateway_mac);
    out->set_bssid(info.bssid);
    out->set_lan_ip(info.lan_ip);
    out->set_lan_cidr(info.lan_cidr);
    out->set_public_ip(info.public_ip);
    out->set_net_type(info.net_type);
    out->set_is_metered(info.is_metered);
    out->set_is_roaming(info.is_roaming);
}

}  // namespace device_agent
