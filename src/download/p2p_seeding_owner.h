#pragma once

// ============================================================
// p2p_seeding_owner.h - Windows 历史 torrent 独立做种 owner
// (ADR-20260901-01 D1-D9；B5-S1)
// ============================================================
// 前台 P2PDownloadManager 保持 single-active 下载 admission；本 owner 持有
// 独立 seed-only libtorrent session（单线程、固定 cap=2 历史 torrent），
// 只接收前台在 release 成功后投递的不可变 P2PSeedCandidate。
// 边界纪律：
//   - owner 永不持有/调用 release completion callback（terminal-once 仍由
//     前台 worker 唯一负责，D7）；
//   - add/announce/seed 失败只记 telemetry，绝不反转已成功的 release（D2/D9）；
//   - 数据文件零复制，remove_torrent 不带 delete_files，绝不删 release
//     artifact（D3）；
//   - on_network_changed 回调内只做 snapshot，锁外才能调 libtorrent（D6）；
//   - seed session settings 遵守 B5-S0 probe 锁值 L1（max_retry_port_bind=0、
//     listen 校验）、L2（global-only peer-class filter + per-handle 分摊限速）、
//     L3（teardown 不允许 session_proxy 跨 session 析构持有）。
// ============================================================

#include "download/network_policy.h"
#include "download/p2p_download_manager.h"

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace device_agent {

// ADR-20260901-01 Decision 2：不可变 handoff candidate。TTL/ratio 是 admission
// snapshot，运行中配置变化不追溯改写已接纳 torrent 的生命周期（D6）。
struct P2PSeedCandidate {
    std::shared_ptr<lt::torrent_info> torrent;
    std::string save_path;
    std::chrono::steady_clock::time_point admitted_at{};
    std::chrono::seconds ttl{0};
    double ratio_limit = 0.0;

    // 去重键：v1 info-hash 优先，v2-only torrent 回退 v2（原始字节）。
    std::string dedupe_key() const;
};

class P2PSeedingOwner : public NetworkPolicy::Listener {
public:
    // ADR-20260901-01 Decision 8：有界 stop reason 集合。
    enum class StopReason {
        ttl,
        ratio,
        capacity,
        policy,
        file_missing,
        session_error,
        shutdown,
    };

    // 有界结构化 telemetry（D8）。仅 info-hash 短前缀进日志，不记 URL/路径。
    struct Counters {
        int admitted = 0;
        int duplicate = 0;
        int evicted_capacity = 0;
        int rejected_policy = 0;
        int rejected_queue_full = 0;
        int rejected_not_running = 0;
        int add_failed = 0;
        int stop_ttl = 0;
        int stop_ratio = 0;
        int stop_policy = 0;
        int stop_file_missing = 0;
        int stop_session_error = 0;
        int stop_shutdown = 0;
        int active_seeds = 0;  // gauge
    };

    struct Config {
        // L1 锁定格式 "<addr>:<port>"；port=0 表示 OS 分配（仅测试建议）。
        std::string listen_interfaces;
        int connections_limit = 32;  // ADR-20260901-01 D3
        // 实时策略 provider（网络开关/上传上限读最新 snapshot，D6）。
        // 默认读 P2PConfigStore 全局快照；无全局 store 时用结构体默认值。
        std::function<P2PSeedingPolicy()> policy_provider;
    };

    P2PSeedingOwner(Config config, std::shared_ptr<NetworkPolicy> network_policy = nullptr);
    ~P2PSeedingOwner() override;

    P2PSeedingOwner(const P2PSeedingOwner&) = delete;
    P2PSeedingOwner& operator=(const P2PSeedingOwner&) = delete;

    // L1：创建 seed-only session（seed role：incoming on / outgoing off）并
    // 校验 listen 结果；失败返回 false 且 owner 保持未启动（D9 失败隔离：
    // 前台 P2P 下载与 WinHTTP fallback 不受影响）。
    bool Start(std::string& error);

    // 幂等：停止接收、按 stop_reason=shutdown 清空 registry、安全 teardown
    // （L3：直接 ~session()，不使用跨析构的 session_proxy）。
    void Stop();

    // D2/D9：非阻塞投递 candidate。队列满或未运行时丢弃并计数，不阻塞前台。
    void Admit(P2PSeedCandidate candidate);

    void on_network_changed(NetworkType type) override;

#ifdef DEVICE_AGENT_TESTING
    Counters counters_for_test() const;
    std::vector<std::string> active_info_hashes_for_test() const;
    // 每 handle 的 (max_uploads, upload_limit_bytes) 快照（throttle 断言用）。
    struct ThrottleSample {
        int max_uploads = 0;
        int upload_limit = 0;
    };
    std::vector<ThrottleSample> active_throttles_for_test() const;
    bool is_running_for_test() const;
    // seed session 实际 listen 端口（测试 loopback 连接用；未启动返回 0）。
    int listen_port_for_test() const;
#endif

private:
    struct SeedEntry {
        P2PSeedCandidate candidate;
        lt::torrent_handle handle;
        std::int64_t last_all_time_upload = 0;
        int applied_max_uploads = -1;
        int applied_upload_limit = -1;
    };

    // 以下除 live_policy 外均只在 owner 线程执行（libtorrent 调用一律不持
    // mu_；registry_ 的唯一写者是 owner 线程，跨线程读写走 mu_）。
    void run_loop();
    void tick();
    void tick_policy_hard_stop(const P2PSeedingPolicy& policy);
    void tick_admissions(const P2PSeedingPolicy& policy);
    void tick_samples_and_expiry();
    void apply_throttles(const P2PSeedingPolicy& policy, bool force);
    // registry_ 的 owner 线程侧只读访问（单写者纪律，见上）。
    std::size_t registry_size() const;
    const P2PSeedCandidate* find_candidate(const std::string& dedupe_key) const;
    P2PSeedingPolicy live_policy() const;

    Config config_;
    std::shared_ptr<NetworkPolicy> network_policy_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<P2PSeedCandidate> queue_;
    std::vector<SeedEntry> registry_;
    Counters counters_;
    NetworkType network_type_{NetworkType::NONE};
    bool running_{false};
    bool stop_requested_{false};

    std::unique_ptr<lt::session> session_;
    std::thread thread_;
};

}  // namespace device_agent
