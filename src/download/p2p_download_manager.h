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
    // Windows 默认 fail-closed，测试与未来平台原生实现（如 WinHTTP）可注入替换。
    using HttpFallback = std::function<bool(const std::string& url,
                                            const std::string& output_path,
                                            std::string& error)>;

    explicit P2PDownloadManager(
        Callbacks callbacks = {},
        P2PSeedingPolicy seeding_policy = P2PSeedingPolicy::alpha_defaults(),
        std::shared_ptr<NetworkPolicy> network_policy = nullptr);
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
#endif

private:
    void run_download(DownloadRequest req,
                      ProgressCallback on_progress,
                      CompleteCallback on_complete);
    void join_worker();
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
    std::atomic<bool> cancel_requested_{false};
    HttpFallback http_fallback_;
    Callbacks callbacks_;
    P2PSeedingStateMachine state_machine_;
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
