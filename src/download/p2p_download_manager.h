#pragma once

#include "download/idownload_manager.h"
#include "download/network_policy.h"

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace device_agent {

class P2PSeedingOwner;

enum class P2PDownloadState {
    Idle,
    Downloading,
    Seeding,
    Stopping,
};

enum class CompletionPathTelemetry {
    Unspecified = 0,
    P2PPrimary = 1,
    WebSeedPrimary = 2,
    HttpFallbackStall = 3,
    HttpFallbackShaMismatch = 4,
};

struct P2PSeedingPolicy {
    std::chrono::seconds ttl{0};
    double ratio_limit = 0.0;
    int max_upload_kbps = 0;
    bool cellular_seeding_enabled = false;
    bool p2p_enabled = true;
    bool seeding_enabled = true;
    int max_upload_peers = 4;
    bool lan_upload_enabled = true;
    bool wan_upload_enabled = false;
    bool cellular_download_enabled = true;
    int min_file_size_mb_for_p2p = 10;

    static P2PSeedingPolicy alpha_defaults();
};

class P2PSeedingStateMachine {
public:
    explicit P2PSeedingStateMachine(P2PSeedingPolicy policy = P2PSeedingPolicy::alpha_defaults());

    P2PDownloadState state() const;
    void mark_downloading();
    void mark_seeding(std::chrono::steady_clock::time_point now);
    void mark_stopping();
    void mark_idle();
    bool should_stop(std::chrono::steady_clock::time_point now, double share_ratio) const;

private:
    P2PSeedingPolicy policy_;
    P2PDownloadState state_{P2PDownloadState::Idle};
    std::chrono::steady_clock::time_point seeding_started_{};
};

class P2PDownloadManager : public IDownloadManager, public NetworkPolicy::Listener {
public:
    struct Callbacks {
        std::function<void(const DownloadRequest&, const std::string&)> on_started;
        std::function<void(const DownloadRequest&, const DownloadProgress&)> on_progress;
        std::function<void(const DownloadRequest&, const std::string&, bool, const std::string&)> on_complete;
        std::function<void(
            const DownloadRequest&,
            const std::string&,
            bool,
            const std::string&,
            CompletionPathTelemetry,
            int64_t,
            int64_t)> on_complete_with_path;
    };

    // HTTP web-seed fallback seam：把 torrent 元数据 / stall / sha-mismatch 的
    // 直接 HTTP 下载抽成可注入点。默认实现由平台提供（POSIX socket）；
    // Windows 默认 fail-closed，生产注入（如 Windows 混合 manager 注入 WinHTTP
    // 适配，ADR-20260831-01 D2）与测试可替换。
    using HttpFallback = std::function<bool(const std::string& url,
                                            const std::string& output_path,
                                            std::string& error)>;

    // seeding_owner：ADR-20260901-01 B5-S2 additive 可选 seam。仅 Windows
    // P2P hybrid 生产构造并注入；Android/Linux/macOS 保持 nullptr 走既有
    // inline seeding。注入后下载成功不再进入 inline seeding loop（ADR D9
    // 禁止回退成占 admission 的 inline seed），改为 cache flush/SHA/generation
    // 检查后把不可变 candidate 非阻塞移交给 owner。owner 生命周期独立于本
    // manager（由 hybrid/测试持有与停止）。
    explicit P2PDownloadManager(
        Callbacks callbacks = {},
        P2PSeedingPolicy seeding_policy = P2PSeedingPolicy::alpha_defaults(),
        std::shared_ptr<NetworkPolicy> network_policy = nullptr,
        HttpFallback http_fallback = nullptr,
        std::shared_ptr<P2PSeedingOwner> seeding_owner = nullptr);
    ~P2PDownloadManager() override;

    P2PDownloadManager(const P2PDownloadManager&) = delete;
    P2PDownloadManager& operator=(const P2PDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;

    void cancel() override;
    bool is_downloading() const override;
    P2PDownloadState state() const;
    void on_network_changed(NetworkType type) override;
#ifdef DEVICE_AGENT_TESTING
    std::vector<int> active_max_uploads_for_test() const;
    std::vector<int> active_upload_limits_for_test() const;
    // 注入 HTTP fallback（必须在 download() 之前调用；仅测试构建暴露）。
    void set_http_fallback_for_test(HttpFallback fallback);
    // B5-05 gate B（确定性）：handoff commit（最终 validity 校验 + Admit）
    // 之前的窗口入口——窗口内 cancel() 与 commit 同锁互斥，双向证明
    // cancel 赢则丢弃 / commit 赢则线性化早于 cancel。
    void set_handoff_gate_for_test(std::function<void()> gate);
    // B5-05 gate A（确定性）：admission 临界区内（ticket 已分配、worker 未
    // 发布、mu_ 在手）的窗口入口——并发 cancel() 必然阻塞到临界区结束，
    // 不存在「cancel 先返回、请求随后继续」。
    void set_admission_gate_for_test(std::function<void()> gate);
    // B5-05 drain gate（确定性）：worker 终态收尾（downloading_=false 与
    // idle 之前）的窗口入口——窗口内新准入与第二个 cancel 均不得越过
    // drain；放行后顺序、终态与新代状态不被旧收尾覆盖。
    void set_exit_gate_for_test(std::function<void()> gate);
    // B5-05 drain 等待到达信号（确定性）：cancel 无 victim 时进入
    // lifecycle_cv_ 等待前调用（持 mu_）——证明等待者已确定进入等待。
    void set_drain_wait_entered_for_test(std::function<void()> gate);
    // B5-05 cleanup gate（确定性）：cancel 最终清理临界区（drain 解除 +
    // 水位条件化清理 + idle，持 mu_）入口——新准入在此期间无法越过
    // （同锁互斥），证明不存在 post-unlock idle 覆盖。
    void set_cancel_cleanup_gate_for_test(std::function<void()> gate);
    // B5-05 drain 提取到达信号（确定性）：cancel/join_worker 从共享
    // worker_ 提取 joinable victim 并发布 drain 时调用（持 mu_）——证明
    // victim 已被某一路径认领，新准入只能面对 drain。
    void set_drain_started_for_test(std::function<void()> gate);
    // B5-05 admission-attempt 到达信号（确定性）：download() 入口、任何
    // mu_ 获取之前锁外调用（hook_mu_ 同步拷贝）——cleanup gate 持有
    // lifecycle mu_ 期间信号仍可发出，证明新 download 已发起并将面对
    // mu_ 边界（G4）；不得以线程 started flag + sleep 冒充到达。
    void set_admission_attempt_for_test(std::function<void()> gate);
    // 测试用：manager session 的实际 listen 端口（未创建 session 返回 0）。
    int listen_port_for_test() const;
    // 测试用确定性 peer 接入：worker 在 add_torrent 成功后对这些 endpoint
    // connect_peer（download 角色 incoming=off，peer 必须由 manager 外连）。
    void set_test_peer_endpoints_for_test(std::vector<lt::tcp::endpoint> endpoints);
#endif

private:
    void run_download(DownloadRequest req,
                      ProgressCallback on_progress,
                      CompleteCallback on_complete,
                      std::uint64_t generation);
    void join_worker();
    // B5-05 唯一 drain/join 路径（消除 worker ownership 旁路）：
    //   - take_worker_for_drain_locked：mu_ 内从共享 worker_ 提取 joinable
    //     worker 并发布 drain；返回是否提取到（未提取 ⇒ 无 in-flight）。
    //   - finish_worker_drain：锁外 join victim，随后在同一 mu_ 边界解除
    //     drain 并按水位条件化清理 downloading_/state（ticket > 水位的新代
    //     不被覆盖）。cancel 与 download()->join_worker 共用本路径。
    bool take_worker_for_drain_locked(std::thread& victim_out);
    void finish_worker_drain(std::thread& victim);
#ifdef DEVICE_AGENT_TESTING
    // hook_mu_ 下安全拷贝 testing gate：所有 *_for_test_ hook 的 setter 与
    // 读取点统一以 hook_mu_ 同步（独立于 lifecycle mu_），拷贝后按各 gate
    // 的锁语义调用——锁外 gate（admission_attempt/handoff/exit）锁外调用；
    // 持锁 gate（admission/drain_started/drain_wait_entered/cancel_cleanup）
    // 保持在 mu_ 临界区内调用（其阻塞窗口即被测窗口）。
    std::function<void()> copy_hook_for_test(const std::function<void()>& hook) const;
#endif
    // HTTP fallback 分发：优先注入实现，否则平台默认实现。
    bool download_via_http_fallback(const std::string& url,
                                    const std::string& output_path,
                                    std::string& error);
    void set_state_downloading();
    void set_state_seeding();
    void set_state_stopping();
    void set_state_idle();
    bool should_stop_seeding(double share_ratio) const;
    void ensure_session_locked();
    void remove_active_handle_locked(const lt::torrent_handle& handle);
    // 蜂窝守门采样:按增量分桶累积到进程级计数(ADR-20260612-01 D2);仅 worker 线程调用
    void sample_upload(std::int64_t all_time_upload);
    void refresh_policy_from_config();
    // ADR-20260901-01 B5-S2：成功终态后移交 candidate 给 seeding owner。
    // 仅 worker 线程调用；返回 false（stale generation / 无 metadata）时
    // 不做种也不反转已发出的 release success（D2/D7/D9）。
    bool try_handoff_to_owner(std::uint64_t generation,
                              const lt::torrent_handle& handle,
                              const std::string& save_path);

    mutable std::mutex mu_;
    mutable std::mutex state_mu_;
    std::thread worker_;
    std::unique_ptr<lt::session> session_;
    std::vector<lt::torrent_handle> active_handles_;
    std::shared_ptr<NetworkPolicy> network_policy_;
    P2PSeedingPolicy seeding_policy_;
    NetworkType network_type_{NetworkType::NONE};
    // P2P 上传分桶采样的上次样本(torrent all_time_upload);仅 worker 线程读写
    std::int64_t last_upload_sample_{0};
    std::atomic<bool> downloading_{false};
    // ADR-20260901-01 B5-05 单一线性化协议（lifecycle mutex = mu_）：
    //   - admission（ticket 分配 + downloading_ 置位 + worker 发布）在同一个
    //     mu_ 临界区内完成，「已准入未发布」状态对外不可见；
    //   - cancel 在同一 mu_ 下取失效水位 cancel_epoch_ = admission_counter_
    //     并移出 victim worker 锁外 join——返回 ⇒ 已准入请求必然已失效并终
    //     止，不存在「cancel 已返回但请求继续」；
    //   - handoff 的最终 validity 校验 + Admit commit 同样在 mu_ 下与失效
    //     互斥（candidate 昂贵构造在临界区外）；
    //   - worker 侧以 cancel_epoch_ 原子镜像做无锁 stale 轮询。
    std::atomic<std::uint64_t> cancel_epoch_{0};
    std::uint64_t admission_counter_{0};  // mu_ 保护
    // B5-05 drain 协议：cancel 锁外 join victim 期间置位；参与 admission
    // 权威检查（drain 期间拒绝新准入），第二个 cancel 在 lifecycle_cv_ 上
    // 等待 drain 完成——旧收尾不可能清除其后新准入的 downloading/state。
    bool drain_in_progress_{false};  // mu_ 保护
    std::condition_variable lifecycle_cv_;
    HttpFallback http_fallback_;
    Callbacks callbacks_;
    P2PSeedingStateMachine state_machine_;
    std::shared_ptr<P2PSeedingOwner> seeding_owner_;
#ifdef DEVICE_AGENT_TESTING
    // TESTING hook 专用锁：与 lifecycle mu_ 完全独立——cleanup gate 持有
    // mu_ 期间 admission-arrived seam 的读取/调用不受阻（TSAN 同步不依赖
    // 正被持有的 mu_）。锁序恒为 mu_ → hook_mu_（反向不存在）。
    mutable std::mutex hook_mu_;
    std::function<void()> handoff_gate_for_test_;
    // B5-05 gate A：admission 临界区内（ticket 已分配、worker 未发布）的
    // 确定性窗口入口。窗口内持 mu_，并发 cancel() 必然阻塞到临界区结束。
    std::function<void()> admission_gate_for_test_;
    std::function<void()> exit_gate_for_test_;
    std::function<void()> drain_wait_entered_for_test_;
    std::function<void()> cancel_cleanup_gate_for_test_;
    std::function<void()> drain_started_for_test_;
    std::function<void()> admission_attempt_for_test_;
    std::vector<lt::tcp::endpoint> test_peer_endpoints_;
    // 测试 peer 接入的重试线程：manager 析构时先 join（见 ~P2PDownloadManager），
    // 避免 detached 线程在 session 销毁后调用 handle。
    std::vector<std::thread> test_connect_threads_;
    // worker 生命周期持有的重试停止标志（TESTING）。
    std::shared_ptr<std::atomic<bool>> test_connect_stop_;
#endif
};

#ifdef DEVICE_AGENT_TESTING
CompletionPathTelemetry completion_path_for_test(bool stall_fallback,
                                                 bool sha_fallback,
                                                 int64_t peer_bytes,
                                                 int64_t web_seed_bytes,
                                                 int64_t total_payload_download,
                                                 bool has_web_seed_hint);

// 路径 helper 测试探针（Windows 反斜杠 / POSIX 正斜杠语义）。
std::string join_path_for_test(const std::string& dir, const std::string& name);
std::string dirname_or_current_for_test(const std::string& path);
#endif

}  // namespace device_agent
