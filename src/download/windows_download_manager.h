// ============================================================
// download/windows_download_manager.h - Windows 下载管理器
// ============================================================
// Windows 平台的 IDownloadManager 实现。
//
// 定位（见 DeviceOps「统一边缘 P2P 文件分发平台方案」§4.3/§6/§17）：
//   本实现是 P2P 平台的「中心源站 / web-seed 兜底层」。Windows 终端先以
//   「download-only」身份接入分发平台——始终可用的 HTTP Range 下载通道，
//   完整 P2P 参与（Windows libtorrent + 策略引擎）作为 D1-后续终态另排。
//
// 与 Android（DeviceAgentService.kt resumableDownload）语义对齐：
//   - HTTP Range 断点续传（.part 临时文件 + 续传 / 416 回退 / 200 重下）
//   - 边下边算 SHA256，完成后校验 size + sha256
//   - 校验通过后原子改名 .part -> 最终文件（completionPath）
//   - 状态遥测带 peerBytes / webSeedBytes：本批走 web-seed/中心源站，
//     peer_bytes=0、web_seed_bytes=已下载字节，completion_path=WebSeedPrimary，
//     与现有 ReportReleaseStatus 账本一致。
//
// 实现仅依赖 Windows 原生 API：
//   - WinHTTP（下载，支持 HTTP/HTTPS，Win7+ 默认可用；TLS1.2 注意点同 9e81f2d）
//   - BCrypt / CNG（SHA256，无需引入 OpenSSL，Win7+ 可用）
//
// 自更新（apply）见 windows_download_manager.cc 的 stage / apply 设计说明。
// ============================================================

#pragma once

#ifdef _WIN32

#include "download/idownload_manager.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace device_agent {

// 与 P2PDownloadManager 的 CompletionPathTelemetry 取值保持一致
// （terminal_agent.v1.CompletionPath）。Windows 本批仅用到 WebSeedPrimary
// 与 HttpFallback* 语义。
enum class WindowsCompletionPath {
    Unspecified = 0,
    P2PPrimary = 1,
    WebSeedPrimary = 2,
    HttpFallbackStall = 3,
    HttpFallbackShaMismatch = 4,
};

class WindowsDownloadManager : public IDownloadManager {
public:
    WindowsDownloadManager() = default;
    ~WindowsDownloadManager() override;

    WindowsDownloadManager(const WindowsDownloadManager&) = delete;
    WindowsDownloadManager& operator=(const WindowsDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;

    void cancel() override;
    bool is_downloading() const override;

private:
    void join_worker();
    void run_download(DownloadRequest req,
                      ProgressCallback on_progress,
                      CompleteCallback on_complete);

    std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> downloading_{false};
    std::atomic<bool> cancel_requested_{false};
};

// ─── 工具函数（在 .cc 中实现，可单测 / 复用）──────────────────

// 由「下载目录 + file_id/URL basename」推导最终文件路径，与 P2P 下载器一致。
std::string windows_resolve_dest_path(const DownloadRequest& req);

// 计算文件 SHA256（小写 hex）。失败返回 false 并填 error。
bool windows_sha256_file(const std::string& path, std::string& out_hex, std::string& error);

// ─── 自更新 apply ─────────────────────────────────────────
// 运行中的 device-agent.exe 无法被直接覆盖。本函数实现「helper 批处理 +
// 服务重启」的原子替换：
//   1. 生成 %TEMP%\device-agent-update-<pid>.cmd，内容为：
//        - 等待当前进程退出（轮询 PID 不存在）
//        - net stop <service> / sc stop（若以服务运行）
//        - 备份旧 exe -> *.bak，move 新 exe -> 目标（原子替换）
//        - 失败则回滚 *.bak
//        - net start <service> / 直接重启进程
//        - 自删除 .cmd
//   2. 以分离进程启动该 .cmd（CREATE_NO_WINDOW），随后由调用方令本进程退出，
//      .cmd 等到退出后完成替换并拉起新版本。
//
// new_exe_path：已下载并校验通过的新版可执行文件（暂存路径）。
// target_exe_path：要被替换的目标（空 = 用当前进程 GetModuleFileName）。
// service_name：以 Windows 服务运行时的服务名（空 = 非服务，直接重启进程）。
// 返回 true 表示 helper 已成功调度；调用方应随后触发本进程退出。
bool windows_apply_self_update(const std::string& new_exe_path,
                               const std::string& target_exe_path,
                               const std::string& service_name,
                               std::string& error);

}  // namespace device_agent

#endif  // _WIN32
