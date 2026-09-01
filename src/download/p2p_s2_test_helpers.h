// ============================================================
// download/p2p_s2_test_helpers.h - B5-S2 离线确定性测试共享辅助
// ============================================================
// 供 p2p_download_manager_test / windows_p2p_download_manager_test 复用：
// 本地 mini tracker（fleet opentracker 拓扑的进程内等价物）+ 进程内 BT
// seeder + 可注入 P2PSeedingOwner。仅测试构建包含；全部 loopback、无
// tracker/外网依赖。
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
#include <map>
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
#include <sys/time.h>
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
using s2_socket_t = SOCKET;
constexpr s2_socket_t kS2InvalidSocket = INVALID_SOCKET;
inline void s2_close_socket(s2_socket_t fd) { ::closesocket(fd); }
#else
using s2_socket_t = int;
constexpr s2_socket_t kS2InvalidSocket = -1;
inline void s2_close_socket(s2_socket_t fd) { ::close(fd); }
#endif

inline bool s2_port_is_listening(std::uint16_t port) {
    s2_socket_t probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe == kS2InvalidSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const int rc = ::connect(probe, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    s2_close_socket(probe);
    return rc == 0;
}

inline std::uint16_t s2_pick_free_port() {
    s2_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kS2InvalidSocket) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        s2_close_socket(fd);
        return 0;
    }
#ifdef _WIN32
    int addr_len = static_cast<int>(sizeof(addr));
#else
    socklen_t addr_len = sizeof(addr);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        s2_close_socket(fd);
        return 0;
    }
    const std::uint16_t port = ntohs(addr.sin_port);
    s2_close_socket(fd);
    return port;
}

// 内嵌 mini tracker（B5-S2 确定性测试的 peer 发现路径，与 fleet 的
// opentracker 拓扑一致）：登记各 info_hash 的 announce (port)，以 compact
// peers 回应。libtorrent add-on即 announce，seeder/manager 确定性互发现。
class LoopbackTrackerServer {
public:
    bool start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kS2InvalidSocket) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            s2_close_socket(listener_);
            listener_ = kS2InvalidSocket;
            return false;
        }
#ifdef _WIN32
        int addr_len = static_cast<int>(sizeof(addr));
#else
        socklen_t addr_len = sizeof(addr);
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr),
                          &addr_len) != 0 ||
            ::listen(listener_, 16) != 0) {
            s2_close_socket(listener_);
            listener_ = kS2InvalidSocket;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        // accept 线程只使用 fd 局部副本（TSAN 干净）；stop 以 shutdown 唤醒。
        const s2_socket_t listener = listener_;
        thread_ = std::thread([this, listener] { accept_loop(listener); });
        return true;
    }

    std::uint16_t port() const { return port_; }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        if (listener_ != kS2InvalidSocket) {
            ::shutdown(listener_, SHUT_RDWR);
            s2_close_socket(listener_);
            listener_ = kS2InvalidSocket;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    static std::string query_value(const std::string& request,
                                   const std::string& key) {
        const auto qpos = request.find('?');
        if (qpos == std::string::npos) return std::string();
        const auto end = request.find_first_of(" \r\n", qpos);
        const std::string query = request.substr(qpos + 1,
                                                 end == std::string::npos
                                                     ? std::string::npos
                                                     : end - qpos - 1);
        const auto kpos = query.find(key + "=");
        if (kpos == std::string::npos) return std::string();
        const auto vbegin = kpos + key.size() + 1;
        const auto vend = query.find('&', vbegin);
        return query.substr(vbegin, vend == std::string::npos
                                        ? std::string::npos
                                        : vend - vbegin);
    }

    // announce key = 原始 info_hash 参数（两侧同编码，可直接匹配）；peers
    // 以 compact 6 字节（127.0.0.1 + port 大端）返回。
    void serve_announce(s2_socket_t fd, const std::string& request) {
        const std::string key = query_value(request, "info_hash");
        const std::string port_str = query_value(request, "port");
        const std::uint16_t peer_port =
            static_cast<std::uint16_t>(std::atoi(port_str.c_str()));
        if (key.empty() || peer_port == 0) {
            s2_close_socket(fd);
            return;
        }
        // 拨号回验证：announced port 必须真实在听（过滤 session 早期/SSL
        // 幻影端口），否则不登记、不回给其它 peer。
        if (!s2_port_is_listening(peer_port)) {
            s2_close_socket(fd);
            return;
        }
        std::vector<std::uint16_t> peers;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::string event = query_value(request, "event");
            auto& swarm = swarms_[key];
            if (event != "stopped" &&
                    std::find(swarm.begin(), swarm.end(), peer_port) ==
                        swarm.end()) {
                swarm.push_back(peer_port);
            }
            for (const std::uint16_t p : swarm) {
                if (p != peer_port) {
                    peers.push_back(p);
                }
            }
        }
        {
            sockaddr_in src_addr{};
#ifdef _WIN32
            int src_len = static_cast<int>(sizeof(src_addr));
#else
            socklen_t src_len = sizeof(src_addr);
#endif
            ::getpeername(fd, reinterpret_cast<sockaddr*>(&src_addr),
                          &src_len);
        }
        std::string peers_bytes;
        for (const std::uint16_t p : peers) {
            peers_bytes.push_back(static_cast<char>(127));
            peers_bytes.push_back(static_cast<char>(0));
            peers_bytes.push_back(static_cast<char>(0));
            peers_bytes.push_back(static_cast<char>(1));
            peers_bytes.push_back(static_cast<char>(p >> 8));
            peers_bytes.push_back(static_cast<char>(p & 0xff));
        }
        char head[160];
        std::snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Length: %d\r\n"
                      "Content-Type: text/plain\r\n"
                      "Connection: close\r\n\r\n",
                      static_cast<int>(34 + peers_bytes.size()));
        std::string body = "d8:intervali1e12:min intervali1e8:completei1e"
                           "10:incompletei0e5:peers" +
                           std::to_string(peers_bytes.size()) + ":" +
                           peers_bytes + "e";
        ::send(fd, head, static_cast<int>(std::strlen(head)), 0);
        ::send(fd, body.data(), static_cast<int>(body.size()), 0);
        s2_close_socket(fd);
    }

    void serve_one(s2_socket_t fd) {
        // 静默连接不能卡死单线程 accept 循环：5s 收不到完整请求头即断开。
#ifdef _WIN32
        DWORD rcv_timeout_ms = 5000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&rcv_timeout_ms),
                     sizeof(rcv_timeout_ms));
#else
        timeval rcv_timeout{};
        rcv_timeout.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout,
                     sizeof(rcv_timeout));
#endif
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const int n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                s2_close_socket(fd);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        if (request.find("/announce") != std::string::npos) {
            serve_announce(fd, request);
        } else {
            s2_close_socket(fd);
        }
    }

    void accept_loop(s2_socket_t listener) {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (stopping_) return;
            }
            const s2_socket_t client = ::accept(listener, nullptr, nullptr);
            if (client == kS2InvalidSocket) {
                return;
            }
            workers_.emplace_back([this, client] { serve_one(client); });
        }
    }

    s2_socket_t listener_ = kS2InvalidSocket;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::mutex mu_;
    bool stopping_ = false;
    std::map<std::string, std::vector<std::uint16_t>> swarms_;
};

// 生成 payload 文件 + 带 tracker 的 .torrent（seeder 与 manager 共用），
// 返回 .torrent 路径。
inline std::string make_tracked_torrent(const std::string& dir,
                                 const std::string& name,
                                 std::size_t size,
                                 unsigned char fill,
                                 std::uint16_t tracker_port) {
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
    const std::string tracker_url =
        "http://127.0.0.1:" + std::to_string(tracker_port) + "/announce";
    creator.add_tracker(tracker_url);
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

// 进程内 BT seeder：seed_mode 直接入种；tracker announce 后主动外连
// manager（outgoing on），下载方向为 manager <- seeder。
inline std::unique_ptr<lt::session> make_bt_seeder(const std::string& torrent_path,
                                            const std::string& payload_dir) {
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
    for (const auto& t : p.ti->trackers()) {
    }
    p.save_path = payload_dir;
    p.flags |= lt::torrent_flags::seed_mode;
    lt::error_code add_ec;
    const auto handle = session->add_torrent(std::move(p), add_ec);
    if (add_ec || !handle.is_valid()) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
