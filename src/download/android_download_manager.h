// ============================================================
// download/android_download_manager.h - Android 下载管理器
// ============================================================
// Android 平台的 IDownloadManager 实现。
// 通过 JNI 调用 Kotlin 层的 onDownloadReady()，不重复造轮子。
//
// Kotlin 层已有完整实现（DeviceAgentService.kt）：
//   - HttpURLConnection + Range 断点续传
//   - SHA256 边下边算
//   - 进度上报 → POST /api/v1/devices/{id}/release_status
//   - pm install 安装
//   - 完成回报 → POST /api/v1/devices/{id}/report
//
// 本类是 JNI 桥接层，将 C++ 调用转发给 Kotlin。
// ============================================================

#pragma once

#ifdef __ANDROID__

#include "download/idownload_manager.h"
#include <mutex>
#include <atomic>

namespace device_agent {

class AndroidDownloadManager : public IDownloadManager {
public:
    AndroidDownloadManager() = default;
    ~AndroidDownloadManager() override = default;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;

    void cancel() override;
    bool is_downloading() const override;

private:
    std::atomic<bool> downloading_{false};
    std::mutex mu_;
};

}  // namespace device_agent

#endif  // __ANDROID__
