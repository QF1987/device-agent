#include "download/noop_download_manager.h"

#include "logger/logger.h"

namespace device_agent {

void NoopDownloadManager::download(const DownloadRequest& req,
                                   ProgressCallback on_progress,
                                   CompleteCallback on_complete) {
    (void)on_progress;
    downloading_.store(false);
    const std::string err = "download_ready is not configured on this platform";
    LOG_WARN("NoopDownloadManager: rejecting download_ready file=" + req.file_id);
    if (on_complete) {
        on_complete(false, err, DownloadCompletionTelemetry{});
    }
}

void NoopDownloadManager::cancel() {
    downloading_.store(false);
}

bool NoopDownloadManager::is_downloading() const {
    return downloading_.load();
}

}  // namespace device_agent
