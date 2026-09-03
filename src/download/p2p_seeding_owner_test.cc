// P2PSeedingOwner 离线自动测试（ADR-20260901-01 B5-S1）。
// 覆盖 task §22 B5-S1 要求：cap/dedupe/eviction、TTL/ratio、网络
// WIFI→CELLULAR→NONE→WIFI、aggregate upload/cellular bucket、init/add/remove
// failure、shutdown 幂等与 listener remove 并发。
// 全部 loopback-only：DHT/LSD(除 seed session 自身)/UPnP/NAT-PMP 关闭、
// 无 tracker 依赖（ratio 场景用进程内 downloader connect_peer 直连）。

#include "download/p2p_seeding_owner.h"
#include "download/p2p_upload_counters.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// 断言只检查结果；调用本身必须始终执行（RV-20260831-WIN-P2P-A-02）。
void assert_true(bool value) {
    assert(value);
}

int test_pid() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

void test_mkdir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
}

std::string make_test_dir(const std::string& name) {
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr) tmp = std::getenv("TMP");
    std::string base = tmp != nullptr ? tmp : ".";
#else
    const char* tmp = std::getenv("TMPDIR");
    std::string base = tmp != nullptr ? tmp : "/tmp";
#endif
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    std::string dir = base + "/" + name + "-" + std::to_string(test_pid());
    test_mkdir(dir);
    return dir;
}

void write_payload(const std::string& path, std::size_t size, unsigned char fill) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(64 * 1024);
    std::fill(buf.begin(), buf.end(), static_cast<char>(fill));
    std::size_t left = size;
    while (left > 0) {
        const std::size_t n = std::min(left, buf.size());
        out.write(buf.data(), static_cast<std::streamsize>(n));
        left -= n;
    }
}

std::shared_ptr<lt::torrent_info> make_torrent(const std::string& data_dir,
                                               const std::string& name,
                                               std::size_t size,
                                               unsigned char fill) {
    write_payload(data_dir + "/" + name, size, fill);
    lt::file_storage fs;
    fs.add_file(name, static_cast<std::int64_t>(size));
    lt::create_torrent creator(fs, 16 * 1024);
    lt::error_code ec;
    lt::set_piece_hashes(creator, data_dir, [](lt::piece_index_t) {}, ec);
    if (ec) return nullptr;
    std::vector<char> buffer;
    lt::bencode(std::back_inserter(buffer), creator.generate());
    return std::make_shared<lt::torrent_info>(buffer, ec, lt::from_span);
}

device_agent::P2PSeedCandidate make_candidate(
        const std::shared_ptr<lt::torrent_info>& torrent,
        const std::string& save_path,
        std::chrono::seconds ttl = std::chrono::seconds{0},
        double ratio_limit = 0.0) {
    device_agent::P2PSeedCandidate candidate;
    candidate.torrent = torrent;
    candidate.save_path = save_path;
    candidate.admitted_at = std::chrono::steady_clock::now();
    candidate.ttl = ttl;
    candidate.ratio_limit = ratio_limit;
    return candidate;
}

device_agent::P2PSeedingPolicy test_policy(int max_upload_kbps = 0) {
    device_agent::P2PSeedingPolicy policy;
    policy.ttl = std::chrono::seconds{0};
    policy.ratio_limit = 0.0;
    policy.max_upload_kbps = max_upload_kbps;
    policy.cellular_seeding_enabled = false;
    policy.p2p_enabled = true;
    policy.seeding_enabled = true;
    policy.max_upload_peers = 4;
    policy.lan_upload_enabled = true;
    policy.wan_upload_enabled = false;
    policy.cellular_download_enabled = true;
    policy.min_file_size_mb_for_p2p = 10;
    return policy;
}

bool wait_for(const std::function<bool()>& predicate, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

// 跨平台最小 socket wrapper（RV-20260901-WIN-P2P-B5-03）：Windows 用
// SOCKET/closesocket/int 长度，POSIX 保持 int/close/socklen_t。测试只绑定
// loopback 短生命周期 socket，无需异步语义。
#ifdef _WIN32
struct TestWinSockInit {
    WSADATA wsa{};
    bool ok = false;
    TestWinSockInit() { ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }
    ~TestWinSockInit() {
        if (ok) {
            ::WSACleanup();
        }
    }
};

using test_socket_t = SOCKET;
constexpr test_socket_t kInvalidTestSocket = INVALID_SOCKET;

inline void test_close_socket(test_socket_t fd) { ::closesocket(fd); }
#else
using test_socket_t = int;
constexpr test_socket_t kInvalidTestSocket = -1;

inline void test_close_socket(test_socket_t fd) { ::close(fd); }
#endif

std::uint16_t pick_free_port() {
    test_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidTestSocket) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        test_close_socket(fd);
        return 0;
    }
#ifdef _WIN32
    int addr_len = static_cast<int>(sizeof(addr));
#else
    socklen_t addr_len = sizeof(addr);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        test_close_socket(fd);
        return 0;
    }
    const std::uint16_t port = ntohs(addr.sin_port);
    test_close_socket(fd);
    return port;
}

lt::settings_pack downloader_pack() {
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, false);
    pack.set_bool(lt::settings_pack::enable_lsd, false);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_bool(lt::settings_pack::listen_system_port_fallback, false);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, false);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, false);
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
    return pack;
}

std::unique_ptr<device_agent::P2PSeedingOwner> make_started_owner(
        device_agent::P2PSeedingOwner::Config config) {
    auto network = std::make_shared<device_agent::NetworkPolicy>();
    if (!config.policy_provider) {
        const auto provider_policy = test_policy();
        config.policy_provider = [provider_policy] { return provider_policy; };
    }
    std::unique_ptr<device_agent::P2PSeedingOwner> owner =
        std::make_unique<device_agent::P2PSeedingOwner>(config, network);
    std::string error;
    const bool started = owner->Start(error);
    assert_true(started);
    assert_true(owner->listen_port_for_test() > 0);
    return owner;
}

}  // namespace

int main() {
#ifdef _WIN32
    TestWinSockInit wsa_init;
    assert_true(wsa_init.ok);
#endif
    using device_agent::NetworkType;
    using device_agent::P2PSeedCandidate;
    using device_agent::P2PSeedingOwner;
    using device_agent::P2PUploadCounters;

    const std::string data_dir = make_test_dir("p2p-seeding-owner-test");

    // ---------- T1 cap / dedupe / FIFO capacity eviction -------------------
    {
        auto owner = make_started_owner(
            P2PSeedingOwner::Config{"127.0.0.1:0"});
        auto t1 = make_torrent(data_dir, "cap_a.bin", 16 * 1024, 0x11);
        auto t2 = make_torrent(data_dir, "cap_b.bin", 16 * 1024, 0x22);
        auto t3 = make_torrent(data_dir, "cap_c.bin", 16 * 1024, 0x33);
        assert_true(t1 != nullptr && t2 != nullptr && t3 != nullptr);

        owner->Admit(make_candidate(t1, data_dir));
        owner->Admit(make_candidate(t1, data_dir));  // duplicate
        assert_true(wait_for([&] {
            const auto counters = owner->counters_for_test();
            return counters.admitted == 1 && counters.duplicate == 1 &&
                   counters.active_seeds == 1;
        }, 5000));

        owner->Admit(make_candidate(t2, data_dir));
        assert_true(wait_for([&] {
            return owner->counters_for_test().active_seeds == 2;
        }, 5000));

        owner->Admit(make_candidate(t3, data_dir));  // evicts t1 (earliest)
        const bool evicted = wait_for([&] {
            const auto counters = owner->counters_for_test();
            const auto active = owner->active_info_hashes_for_test();
            return counters.evicted_capacity == 1 && counters.active_seeds == 2 &&
                   active.size() == 2;
        }, 5000);
        assert_true(evicted);
        const auto counters = owner->counters_for_test();
        assert_true(counters.admitted == 3);
        assert_true(counters.duplicate == 1);
        assert_true(counters.evicted_capacity == 1);

        owner->Stop();
        assert_true(owner->counters_for_test().stop_shutdown >= 2);
    }

    // ---------- T2 TTL expiry ---------------------------------------------
    {
        auto owner = make_started_owner(
            P2PSeedingOwner::Config{"127.0.0.1:0"});
        auto t = make_torrent(data_dir, "ttl.bin", 16 * 1024, 0x44);
        owner->Admit(make_candidate(t, data_dir, std::chrono::seconds{1}));
        // ttl=1s 与 1s poll 同量级：准入与过期可能落在同一 tick，只等终态。
        const bool expired = wait_for([&] {
            const auto counters = owner->counters_for_test();
            return counters.admitted == 1 && counters.stop_ttl == 1 &&
                   counters.active_seeds == 0;
        }, 8000);
        assert_true(expired);
        owner->Stop();
    }

    // ---------- T3 file_missing（admission 前置检查） ----------------------
    {
        auto owner = make_started_owner(
            P2PSeedingOwner::Config{"127.0.0.1:0"});
        auto t = make_torrent(data_dir, "missing.bin", 16 * 1024, 0x55);
        std::remove((data_dir + "/missing.bin").c_str());
        owner->Admit(make_candidate(t, data_dir));
        const bool dropped = wait_for([&] {
            return owner->counters_for_test().stop_file_missing == 1;
        }, 5000);
        assert_true(dropped);
        assert_true(owner->counters_for_test().active_seeds == 0);
        owner->Stop();
    }

    // ---------- T4 add failure（无效 candidate） ----------------------------
    {
        auto owner = make_started_owner(
            P2PSeedingOwner::Config{"127.0.0.1:0"});
        owner->Admit(P2PSeedCandidate{});  // 无 metadata
        const bool failed = wait_for([&] {
            return owner->counters_for_test().add_failed == 1;
        }, 5000);
        assert_true(failed);
        assert_true(owner->counters_for_test().active_seeds == 0);
        owner->Stop();
    }

    // ---------- T5 init failure（端口占用） + after-fail Admit + 幂等 Stop --
    {
        const std::uint16_t occupied = pick_free_port();
        test_socket_t holder_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert_true(holder_fd != kInvalidTestSocket);
        sockaddr_in holder_addr{};
        holder_addr.sin_family = AF_INET;
        holder_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        holder_addr.sin_port = htons(occupied);
        assert_true(::bind(holder_fd, reinterpret_cast<sockaddr*>(&holder_addr),
                           sizeof(holder_addr)) == 0);
        assert_true(::listen(holder_fd, 1) == 0);

        auto network = std::make_shared<device_agent::NetworkPolicy>();
        P2PSeedingOwner::Config config;
        config.listen_interfaces =
            "127.0.0.1:" + std::to_string(occupied);
        const auto provider_policy = test_policy();
        config.policy_provider = [provider_policy] { return provider_policy; };
        P2PSeedingOwner owner(config, network);
        std::string error;
        const bool started = owner.Start(error);
        assert_true(!started);
        assert_true(!error.empty());
        assert_true(!owner.is_running_for_test());
        assert_true(owner.counters_for_test().stop_session_error == 1);

        auto t = make_torrent(data_dir, "initfail.bin", 16 * 1024, 0x66);
        owner.Admit(make_candidate(t, data_dir));
        assert_true(wait_for([&] {
            return owner.counters_for_test().rejected_not_running >= 1;
        }, 3000));
        owner.Stop();
        owner.Stop();  // 幂等
        test_close_socket(holder_fd);
    }

    // ---------- T6 真实 loopback 上传：ratio stop + upload/cellular bucket --
    {
        device_agent::reset_p2p_upload_counters_for_test();
        auto owner = make_started_owner(
            P2PSeedingOwner::Config{"127.0.0.1:0"});
        owner->on_network_changed(NetworkType::WIFI);
        auto t = make_torrent(data_dir, "ratio.bin", 256 * 1024, 0x77);
        owner->Admit(make_candidate(t, data_dir, std::chrono::seconds{0}, 0.5));
        assert_true(wait_for([&] {
            return owner->counters_for_test().active_seeds == 1;
        }, 5000));
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return !throttles.empty() && throttles.front().max_uploads == 4;
        }, 5000));

        // 进程内 downloader（outbound-only）直连 seed session。
        const int seed_port = owner->listen_port_for_test();
        lt::session_params dp;
        dp.settings = downloader_pack();
        lt::session downloader(std::move(dp));
        lt::add_torrent_params p;
        p.ti = t;
        p.save_path = data_dir + "/ratio-dl";
#ifdef _WIN32
        _mkdir((data_dir + "/ratio-dl").c_str());
#else
        mkdir((data_dir + "/ratio-dl").c_str(), 0700);
#endif
        lt::error_code ec;
        auto handle = downloader.add_torrent(std::move(p), ec);
        assert_true(!ec);
        handle.connect_peer(
            lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                              static_cast<std::uint16_t>(seed_port)));

        // ratio_limit=0.5 → 上传 128KB 后 owner 应按 stop_ratio 淘汰。
        const bool ratio_stopped = wait_for([&] {
            return owner->counters_for_test().stop_ratio == 1;
        }, 20000);
        assert_true(ratio_stopped);

        const device_agent::P2PUploadCounters counters =
            device_agent::p2p_upload_counters();
        assert_true(counters.total >= 64 * 1024);  // 上传 delta 已进进程级计数
        assert_true(counters.cellular == 0);       // WIFI 期间不进蜂窝桶

        owner->Stop();
    }

    // ---------- T7 网络 WIFI→CELLULAR→NONE→WIFI + aggregate 分摊 ------------
    {
        auto network = std::make_shared<device_agent::NetworkPolicy>();
        P2PSeedingOwner::Config config;
        config.listen_interfaces = "127.0.0.1:0";
        const auto provider_policy = test_policy(32);  // 32KB/s 聚合
        config.policy_provider = [provider_policy] { return provider_policy; };
        auto owner = std::make_unique<P2PSeedingOwner>(config, network);
        std::string error;
        assert_true(owner->Start(error));
        // 首样本 NONE fail closed：admit 后保持受抑制 throttle。
        auto t1 = make_torrent(data_dir, "agg_a.bin", 16 * 1024, 0x88);
        auto t2 = make_torrent(data_dir, "agg_b.bin", 16 * 1024, 0x99);
        owner->Admit(make_candidate(t1, data_dir));
        assert_true(wait_for([&] {
            const auto counters = owner->counters_for_test();
            return counters.active_seeds == 1;
        }, 5000));
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return !throttles.empty() &&
                   throttles.front().max_uploads == 1 &&
                   throttles.front().upload_limit == 5120;
        }, 5000));

        network->on_network_changed(NetworkType::WIFI);
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            // 1 个 seed：聚合 32KB/s 全额分摊到该 handle。
            return !throttles.empty() &&
                   throttles.front().max_uploads == 4 &&
                   throttles.front().upload_limit == 32 * 1024;
        }, 5000));

        // 2 个 seed：32KB/s 聚合 → 每 handle 分摊 16KB/s（B5-S0 L2）。
        owner->Admit(make_candidate(t2, data_dir));
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return throttles.size() == 2 &&
                   throttles.front().upload_limit == 16 * 1024 &&
                   throttles.back().upload_limit == 16 * 1024 &&
                   throttles.front().max_uploads == 4 &&
                   throttles.back().max_uploads == 4;
        }, 5000));

        network->on_network_changed(NetworkType::CELLULAR);
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return throttles.size() == 2 &&
                   throttles.front().max_uploads == 1 &&
                   throttles.front().upload_limit == 5120 &&
                   throttles.back().max_uploads == 1 &&
                   throttles.back().upload_limit == 5120;
        }, 5000));

        network->on_network_changed(NetworkType::NONE);  // fail closed
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return throttles.size() == 2 &&
                   throttles.front().max_uploads == 1 &&
                   throttles.front().upload_limit == 5120;
        }, 5000));
        assert_true(owner->counters_for_test().active_seeds == 2);  // 抑制不清除

        network->on_network_changed(NetworkType::WIFI);  // 恢复
        assert_true(wait_for([&] {
            const auto throttles = owner->active_throttles_for_test();
            return throttles.size() == 2 &&
                   throttles.front().upload_limit == 16 * 1024 &&
                   throttles.front().max_uploads == 4;
        }, 5000));

        owner->Stop();
        owner.reset();  // 析构后再 Stop 幂等路径
    }

    // ---------- T8 listener 并发派发 smoke（changer 先 join 再析构； ----------
    // remove/析构与 in-flight 回调的确定性覆盖见 network_policy_test 屏障
    // 用例与本文件 T10）
    {
        auto network = std::make_shared<device_agent::NetworkPolicy>();
        P2PSeedingOwner::Config config;
        config.listen_interfaces = "127.0.0.1:0";
        const auto provider_policy = test_policy();
        config.policy_provider = [provider_policy] { return provider_policy; };
        auto owner = std::make_unique<P2PSeedingOwner>(config, network);
        std::string error;
        assert_true(owner->Start(error));

        auto t = make_torrent(data_dir, "concurrent.bin", 16 * 1024, 0xAA);
        owner->Admit(make_candidate(t, data_dir));

        std::vector<std::thread> changers;
        for (int i = 0; i < 2; ++i) {
            changers.emplace_back([&network, i] {
                for (int n = 0; n < 100; ++n) {
                    network->on_network_changed(
                        (i + n) % 2 == 0 ? NetworkType::WIFI
                                         : NetworkType::CELLULAR);
                }
            });
        }
        for (auto& changer : changers) {
            changer.join();
        }

        assert_true(wait_for([&] {
            return owner->counters_for_test().active_seeds == 1;
        }, 5000));
        owner->Stop();
        owner.reset();  // remove_listener

        // remove 后继续回调：不得崩溃/悬挂。
        for (int n = 0; n < 50; ++n) {
            network->on_network_changed(
                n % 2 == 0 ? NetworkType::WIFI : NetworkType::NONE);
        }
    }

    // ---------- T10 remove/析构 vs in-flight 回调（屏障语义，RV-...-B5-02）--
    // 派发线程仍在批次内时直接 Stop + 析构 owner：NetworkPolicy::remove_listener
    // 的生命周期屏障必须等待批次完成，此后派发线程继续回调不得进入已析构
    // owner（TSAN 下任何交错违例均可检出；屏障保证对任意时序成立）。
    {
        auto network = std::make_shared<device_agent::NetworkPolicy>();
        P2PSeedingOwner::Config config;
        config.listen_interfaces = "127.0.0.1:0";
        const auto provider_policy = test_policy();
        config.policy_provider = [provider_policy] { return provider_policy; };
        auto owner = std::make_unique<P2PSeedingOwner>(config, network);
        std::string error;
        assert_true(owner->Start(error));

        auto t = make_torrent(data_dir, "barrier.bin", 16 * 1024, 0xCC);
        owner->Admit(make_candidate(t, data_dir));
        assert_true(wait_for([&] {
            return owner->counters_for_test().active_seeds == 1;
        }, 5000));

        std::thread changer([&network] {
            for (int n = 0; n < 300; ++n) {
                network->on_network_changed(
                    n % 2 == 0 ? NetworkType::WIFI : NetworkType::CELLULAR);
            }
        });
        // 不 join，直接销毁：dtor 的 remove_listener 与 changer 的批次竞争。
        owner->Stop();
        owner.reset();
        changer.join();

        // remove/析构完成后继续派发：不得崩溃（回调不再进入 owner）。
        for (int n = 0; n < 50; ++n) {
            network->on_network_changed(
                n % 2 == 0 ? NetworkType::WIFI : NetworkType::NONE);
        }
    }

    // ---------- T9 seeding_enabled=false 硬 stop ----------------------------
    {
        auto policy = test_policy();
        policy.seeding_enabled = false;
        P2PSeedingOwner::Config config;
        config.listen_interfaces = "127.0.0.1:0";
        config.policy_provider = [policy] { return policy; };
        auto owner = make_started_owner(std::move(config));

        auto t = make_torrent(data_dir, "hardstop.bin", 16 * 1024, 0xBB);
        owner->Admit(make_candidate(t, data_dir));
        assert_true(wait_for([&] {
            const auto counters = owner->counters_for_test();
            return counters.rejected_policy == 1 && counters.active_seeds == 0;
        }, 5000));
        owner->Stop();
    }

    std::cout << "p2p_seeding_owner_test: all assertions passed\n";
    return 0;
}
