#pragma once

#include "download/idownload_manager.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace device_agent {

class P2PDownloadManager : public IDownloadManager {
public:
    P2PDownloadManager() = default;
    ~P2PDownloadManager() override;

    P2PDownloadManager(const P2PDownloadManager&) = delete;
    P2PDownloadManager& operator=(const P2PDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;

    void cancel() override;
    bool is_downloading() const override;

private:
    void run_download(DownloadRequest req,
                      ProgressCallback on_progress,
                      CompleteCallback on_complete);
    void join_worker();

    mutable std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> downloading_{false};
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace device_agent
