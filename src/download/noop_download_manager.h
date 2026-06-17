#pragma once

#include "download/idownload_manager.h"

#include <atomic>

namespace device_agent {

class NoopDownloadManager : public IDownloadManager {
public:
    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;
    void cancel() override;
    bool is_downloading() const override;

private:
    std::atomic<bool> downloading_{false};
};

}  // namespace device_agent
