// ============================================================
// download/p2p_s2_test_helpers.h - B5-S2 离线确定性测试共享辅助
// ============================================================
// 供 p2p_download_manager_test / windows_p2p_download_manager_test 复用：
// 进程内 BT seeder（确定性 connect_peer 直连 manager）+ 可注入
// P2PSeedingOwner。仅测试构建包含；全部 loopback、无 tracker/外网依赖、
// 无 announce 时序竞争。
// ============================================================

#pragma once

#include "download/p2p_seeding_owner.h"

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace p2p_s2_test {

#ifdef _WIN32
struct S2WinSockInit {
    WSADATA wsa{};
    bool ok = false;
    S2WinSockInit() { ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }
    ~S2WinSockInit() {
        if (ok) {
            ::WSACleanup();
        }
    }
};
#else
struct S2WinSockInit {};
#endif

inline std::string make_s2_dir(const std::string& name) {
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr) tmp = std::getenv("TMP");
    std::string base = tmp != nullptr ? tmp : ".";
#else
    const char* tmp = std::getenv("TMPDIR");
    std::string base = tmp != nullptr ? tmp : "/tmp";
#endif
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string dir = base + "/" + name + "-" +
                            std::to_string(static_cast<long>(::time(nullptr)) % 1000000) +
                            "-" + std::to_string(static_cast<long>(::rand()));
#ifdef _WIN32
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0700);
#endif
    return dir;
}

// 生成 payload 文件 + 无 tracker 的 .torrent（peer 由 seeder connect_peer
// 直连注入，无 announce 时序竞争）。返回 .torrent 路径。
inline std::string make_plain_torrent(const std::string& dir,
                                      const std::string& name,
                                      std::size_t size,
                                      unsigned char fill) {
    const std::string payload_path = dir + "/" + name;
    {
        std::ofstream out(payload_path, std::ios::binary | std::ios::trunc);
        std::vector<char> payload(size);
        std::fill(payload.begin(), payload.end(), static_cast<char>(fill));
        out.write(payload.data(), static_cast<std::streamsize>(size));
    }
    lt::file_storage fs;
    fs.add_file(name, static_cast<std::int64_t>(size));
    lt::create_torrent creator(fs, 16 * 1024);
    lt::error_code ec;
    lt::set_piece_hashes(creator, dir, [](lt::piece_index_t) {}, ec);
    if (ec) return std::string();
    std::vector<char> buffer;
    lt::bencode(std::back_inserter(buffer), creator.generate());
    const std::string torrent_path = dir + "/" + name + ".torrent";
    std::ofstream out(torrent_path, std::ios::binary | std::ios::trunc);
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return out ? torrent_path : std::string();
}

// 进程内 BT seeder：seed_mode 直接入种并监听（incoming on）。peer 接入
// 由 manager 侧 set_test_peer_endpoints_for_test 外连完成（download 角色
// incoming=off），因此 seeder 必须先于 manager.download 创建。
inline std::unique_ptr<lt::session> make_bt_seeder(
        const std::string& torrent_path, const std::string& payload_dir) {
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, false);
    pack.set_bool(lt::settings_pack::enable_lsd, false);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_bool(lt::settings_pack::listen_system_port_fallback, false);
    pack.set_int(lt::settings_pack::max_retry_port_bind, 0);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, true);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, false);
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
    lt::session_params params;
    params.settings = pack;
    auto session = std::make_unique<lt::session>(std::move(params));
    lt::add_torrent_params p;
    lt::error_code ec;
    p.ti = std::make_shared<lt::torrent_info>(torrent_path, ec);
    if (ec) return nullptr;
    p.save_path = payload_dir;
    p.flags |= lt::torrent_flags::seed_mode;
    lt::error_code add_ec;
    const auto handle = session->add_torrent(std::move(p), add_ec);
    if (add_ec || !handle.is_valid()) return nullptr;
    return session;
}

inline std::unique_ptr<device_agent::P2PSeedingOwner> make_s2_owner() {
    device_agent::P2PSeedingOwner::Config config;
    config.listen_interfaces = "127.0.0.1:0";
    const device_agent::P2PSeedingPolicy provider_policy = [] {
        device_agent::P2PSeedingPolicy policy;
        policy.min_file_size_mb_for_p2p = 0;
        return policy;
    }();
    config.policy_provider = [provider_policy] { return provider_policy; };
    auto owner = std::make_unique<device_agent::P2PSeedingOwner>(
        config, std::make_shared<device_agent::NetworkPolicy>());
    std::string error;
    if (!owner->Start(error)) {
        return nullptr;
    }
    return owner;
}

inline device_agent::P2PSeedingPolicy s2_manager_policy() {
    device_agent::P2PSeedingPolicy policy;
    policy.ttl = std::chrono::seconds{0};
    policy.ratio_limit = 0.0;
    policy.min_file_size_mb_for_p2p = 0;
    return policy;
}

inline bool s2_wait_for(const std::function<bool()>& predicate, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

}  // namespace p2p_s2_test
