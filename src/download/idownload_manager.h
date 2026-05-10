// ============================================================
// download/idownload_manager.h - 下载管理器抽象接口
// ============================================================
// 定义文件下载的抽象接口，各平台自行实现：
//   - Android：JNI → Kotlin HttpURLConnection（已有，不重写）
//   - macOS/Linux/Windows：libcurl 或平台原生 API（待实现）
//
// 设计原则：
//   1. 接口在 C++ 层统一，实现各平台原生
//   2. 与 Executor 接口平行：Executor 管系统操作，DownloadManager 管文件下载
//   3. CommandHandler 通过此接口调度下载，不涉及平台细节
//
// 使用方式：
//   CommandHandler 持有 std::shared_ptr<IDownloadManager>
//   平台启动时注入具体的实现（AndroidDownloadManager / CurlDownloadManager）
// ============================================================

#pragma once

#include <string>
#include <functional>
#include <cstdint>

namespace device_agent {

// ─── DownloadRequest：下载请求 ────────────────────────────
// 统一封装下载所需的所有参数。
// 各平台实现按需使用字段（libcurl 只用 url/dest/sha256，
// Android 用全部字段用于 JNI 转发和进度上报）。
struct DownloadRequest {
    std::string url;            // 下载地址（HTTP/HTTPS）
    std::string dest_path;      // 本地目标路径
    std::string expected_sha256;// SHA256 校验值（空 = 不校验）
    int64_t file_size = 0;      // 文件大小（字节，0 = 未知）

    // 业务上下文（用于进度上报和任务追踪）
    std::string batch_id;       // 批次 ID
    std::string file_id;        // 文件 ID
    std::string command_id;     // 指令 UUID（透传给 Kotlin 用于 reportCommandStatus）
    std::string file_type;      // 文件类型（apk / firmware / config）
};

// ─── DownloadProgress：下载进度 ───────────────────────────
struct DownloadProgress {
    int64_t downloaded_bytes;   // 已下载字节数
    int64_t total_bytes;        // 总字节数（0 = 未知）
    int percent;                // 进度百分比 0-100
};

// ─── IDownloadManager：下载管理器抽象接口 ─────────────────
class IDownloadManager {
public:
    using ProgressCallback = std::function<void(const DownloadProgress&)>;
    using CompleteCallback = std::function<void(bool success, const std::string& err)>;

    virtual ~IDownloadManager() = default;

    // download：异步下载文件
    //   req：下载请求（URL、目标路径、校验值、业务上下文）
    //   on_progress：进度回调（可选，可为 nullptr）
    //   on_complete：完成回调
    //
    // 实现要求：
    //   - 支持断点续传（Range header）
    //   - 下载完成后校验 SHA256
    //   - 回调在非主线程执行
    //   - Android 实现：调用 Kotlin 层处理（下载+校验+安装+状态上报）
    //   - 桌面实现：libcurl 下载 + 校验，安装由调用方处理
    virtual void download(const DownloadRequest& req,
                          ProgressCallback on_progress,
                          CompleteCallback on_complete) = 0;

    // cancel：取消当前下载
    virtual void cancel() = 0;

    // is_downloading：是否正在下载
    virtual bool is_downloading() const = 0;
};

}  // namespace device_agent
