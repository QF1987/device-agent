#pragma once

#include "download/idownload_manager.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace device_agent {

enum class P2PDownloadState {
    Idle,
    Downloading,
    Seeding,
    Stopping,
};

struct P2PSeedingPolicy {
    std::chrono::seconds ttl;
    double ratio_limit;

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

class P2PDownloadManager : public IDownloadManager {
public:
    struct Callbacks {
        std::function<void(const DownloadRequest&, const std::string&)> on_started;
        std::function<void(const DownloadRequest&, const DownloadProgress&)> on_progress;
        std::function<void(const DownloadRequest&, const std::string&, bool, const std::string&)> on_complete;
    };

    explicit P2PDownloadManager(
        Callbacks callbacks = {},
        P2PSeedingPolicy seeding_policy = P2PSeedingPolicy::alpha_defaults());
    ~P2PDownloadManager() override;

    P2PDownloadManager(const P2PDownloadManager&) = delete;
    P2PDownloadManager& operator=(const P2PDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;

    void cancel() override;
    bool is_downloading() const override;
    P2PDownloadState state() const;

private:
    void run_download(DownloadRequest req,
                      ProgressCallback on_progress,
                      CompleteCallback on_complete);
    void join_worker();
    void set_state_downloading();
    void set_state_seeding();
    void set_state_stopping();
    void set_state_idle();
    bool should_stop_seeding(double share_ratio) const;

    mutable std::mutex mu_;
    mutable std::mutex state_mu_;
    std::thread worker_;
    std::atomic<bool> downloading_{false};
    std::atomic<bool> cancel_requested_{false};
    Callbacks callbacks_;
    P2PSeedingStateMachine state_machine_;
};

}  // namespace device_agent
