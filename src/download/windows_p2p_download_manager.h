// ============================================================
// download/windows_p2p_download_manager.h - Windows 混合 P2P manager
// ============================================================
// ADR-20260831-01 D1/D2/D3（Phase B B1）：Windows production 下载入口的
// 混合 manager。核心路由/守卫逻辑平台中立（便于离线确定性测试与 TSAN），
// 仅生产 WinHTTP 适配依赖 _WIN32。
//
// 每请求路由（二选一）：
//   直达 HTTP（Windows 生产 = WindowsDownloadManager/WinHTTP）——
//     无 P2P source（magnet/torrent_url 均空）、policy 关闭、
//     文件低于 min_file_size_mb_for_p2p、蜂窝禁止下载；
//   先 P2P —— 其余情况；P2P 终态失败时最多回退 HTTP 一次（req.url 为
//     http/https 时），取消后不得再启动回退。
//
// 单 active + terminal guard：一次仅允许一个请求在飞；每请求 generation
// 递增，取消/旧回调一律丢弃；终端 completion 恰好一次，由混合 worker 统一
// 发出；cancel() 同时取消 P2P 与 HTTP 两条腿。
// ============================================================

#pragma once

#include "download/idownload_manager.h"
#include "download/p2p_download_manager.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace device_agent {

#ifdef _WIN32
class WindowsDownloadManager;  // fwd：仅适配器实现持有指针
#endif

// 可取消的同步 HTTP fallback 适配：P2PDownloadManager 内部的
// metadata/stall/SHA fallback 经由它落到 Windows 生产 WinHTTP 实现。
// _WIN32：每次调用创建独立 WindowsDownloadManager，同步等待完成，
//         完整保留 WinHTTP Range/.part/SHA256/原子改名；cancel() 中止在飞。
// 其它平台：run() fail-closed（仅逻辑测试用）。
class WindowsHttpFallbackAdapter {
public:
    WindowsHttpFallbackAdapter() = default;
    ~WindowsHttpFallbackAdapter() = default;

    WindowsHttpFallbackAdapter(const WindowsHttpFallbackAdapter&) = delete;
    WindowsHttpFallbackAdapter& operator=(const WindowsHttpFallbackAdapter&) = delete;

    // 返回注入 P2PDownloadManager 的 fallback（绑定 this）。
    P2PDownloadManager::HttpFallback fallback();

    // 取消当前在飞的下载；无在飞时 no-op。latch 保持到下一次 reset()。
    void cancel();

    // 新请求开始前清除取消 latch。
    void reset();

private:
    bool run(const std::string& url, const std::string& output_path,
             std::string& error);

    std::mutex mu_;
    bool cancel_requested_{false};
#ifdef _WIN32
    WindowsDownloadManager* active_{nullptr};
#endif
};

// Windows 混合 manager：统一持有 P2PDownloadManager 与 HTTP manager。
class WindowsP2PDownloadManager : public IDownloadManager {
public:
    using HttpFallback = P2PDownloadManager::HttpFallback;

    // http_manager：policy 绕行/无 P2P source 时的直达 HTTP leg 与
    //   P2P 失败后的唯一回退 leg（Windows 生产默认 = WindowsDownloadManager，
    //   其它平台默认 nullptr → 直达路由 fail-closed）；测试注入 fake。
    // http_adapter：注入 P2PDownloadManager 的可取消 fallback 适配
    //   （空则创建默认适配；Windows 生产默认落到 WinHTTP）。
    explicit WindowsP2PDownloadManager(
        std::shared_ptr<NetworkPolicy> network_policy = nullptr,
        P2PDownloadManager::Callbacks p2p_callbacks = {},
        std::shared_ptr<IDownloadManager> http_manager = nullptr,
        std::shared_ptr<WindowsHttpFallbackAdapter> http_adapter = nullptr);
    ~WindowsP2PDownloadManager() override;

    WindowsP2PDownloadManager(const WindowsP2PDownloadManager&) = delete;
    WindowsP2PDownloadManager& operator=(const WindowsP2PDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;
    void cancel() override;
    bool is_downloading() const override;

private:
    void join_worker();
    bool should_use_p2p(const DownloadRequest& req) const;
    void run_request(DownloadRequest req,
                     ProgressCallback on_progress,
                     CompleteCallback on_complete,
                     std::uint64_t generation,
                     bool use_p2p);

    std::shared_ptr<NetworkPolicy> network_policy_;
    std::shared_ptr<WindowsHttpFallbackAdapter> http_adapter_;
    std::shared_ptr<IDownloadManager> http_manager_;
    P2PDownloadManager p2p_;

    mutable std::mutex mu_;
    std::thread worker_;
    std::uint64_t generation_{0};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace device_agent
