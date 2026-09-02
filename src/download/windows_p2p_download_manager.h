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

class P2PSeedingOwner;

// 可取消的同步 HTTP fallback 适配：P2PDownloadManager 内部的
// metadata/stall/SHA fallback 经由它落到 Windows 生产 WinHTTP 实现。
// 核心控制流（latch 检查 + active 发布原子性）平台中立，便于离线确定性
// 测试；_WIN32 的 job 包装真实 WindowsDownloadManager，其它平台 fail-closed。
class WindowsHttpFallbackAdapter {
public:
    // 平台 job：execute 阻塞至完成或被 request_cancel 中止；实现必须保证
    // request_cancel 在 execute 之前到达时，execute 立即失败且不产生网络 I/O。
    struct RunJob {
        virtual ~RunJob() = default;
        virtual bool execute(std::string& error) = 0;
        virtual void request_cancel() = 0;
    };

    // start/cancel handshake 基类（RV-20260831-WIN-P2P-B1-01 · execute 内窗口）：
    // 子类只实现三个平台操作——start_download() 发布底层 worker（异步立即返回）、
    // wait_download() 阻塞至完成、cancel_download() 中止底层下载。
    // 线性化保证（全程由 mu_ + cancelled_/published_/cancel_sent_ 状态机驱动）：
    //   - cancel 先于 execute 到达 → execute 入口即失败，start_download 不被调用；
    //   - cancel 与启动重叠（落在入口检查之后、发布之前）→ 发布临界区观察到
    //     cancelled_ 并补发 cancel_download，此时 worker 已在运行，必达；
    //   - cancel 与下载重叠（发布后）→ request_cancel 直接 cancel_download。
    // cancel_download 至多调用一次；取消后的终态统一为 cancelled 失败。
    class HandshakeJob : public RunJob {
    public:
        bool execute(std::string& error) final;
        void request_cancel() final;

    protected:
        virtual void start_download() = 0;
        virtual void wait_download() = 0;
        virtual void cancel_download() = 0;

        bool wait_ok_ = false;      // wait_download() 写入、execute 读取
        std::string wait_err_;

    private:
        bool wait_and_finish(std::string& error);

        mutable std::mutex mu_;
        bool cancelled_ = false;
        bool published_ = false;
        bool cancel_sent_ = false;
    };

    WindowsHttpFallbackAdapter() = default;
    ~WindowsHttpFallbackAdapter() = default;

    WindowsHttpFallbackAdapter(const WindowsHttpFallbackAdapter&) = delete;
    WindowsHttpFallbackAdapter& operator=(const WindowsHttpFallbackAdapter&) = delete;

    // 返回注入 P2PDownloadManager 的 fallback（绑定 this）。
    P2PDownloadManager::HttpFallback fallback();

    // 取消当前在飞的下载；无在飞时仅置 latch（下一次 run 直接失败）。
    void cancel();

    // 新请求开始前清除取消 latch。
    void reset();

#ifdef DEVICE_AGENT_TESTING
    // 测试 seam（RV-20260831-WIN-P2P-B1-01 回归）：替换平台 job、并在
    // “active 已发布、job 未启动”边界上插入确定性 gate。
    void set_job_factory_for_test(
        std::function<std::unique_ptr<RunJob>(const std::string& url,
                                              const std::string& output_path)>
            factory);
    void set_start_gate_for_test(std::function<void()> gate);
#endif

private:
    std::unique_ptr<RunJob> create_job(const std::string& url,
                                       const std::string& output_path);
    bool run(const std::string& url, const std::string& output_path,
             std::string& error);

    std::mutex mu_;
    bool cancel_requested_{false};
    std::unique_ptr<RunJob> active_;
#ifdef DEVICE_AGENT_TESTING
    std::function<std::unique_ptr<RunJob>(const std::string&,
                                          const std::string&)>
        job_factory_;
    std::function<void()> start_gate_;
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
    // seeding_owner：ADR-20260901-01 B5-S2 历史做种 owner。Windows 生产
    //   （_WIN32）默认自动创建并 Start（listen 取 P2P_SEED_LISTEN_INTERFACES，
    //   默认 0.0.0.0:6892）；其它平台/测试默认 nullptr（inner 保留 inline
    //   seeding），可显式注入替代。Start 失败仅告警，前台下载不受影响（D9）。
    explicit WindowsP2PDownloadManager(
        std::shared_ptr<NetworkPolicy> network_policy = nullptr,
        P2PDownloadManager::Callbacks p2p_callbacks = {},
        std::shared_ptr<IDownloadManager> http_manager = nullptr,
        std::shared_ptr<WindowsHttpFallbackAdapter> http_adapter = nullptr,
        std::shared_ptr<P2PSeedingOwner> seeding_owner = nullptr);
    ~WindowsP2PDownloadManager() override;

    WindowsP2PDownloadManager(const WindowsP2PDownloadManager&) = delete;
    WindowsP2PDownloadManager& operator=(const WindowsP2PDownloadManager&) = delete;

    void download(const DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override;
    void cancel() override;
    bool is_downloading() const override;

#ifdef DEVICE_AGENT_TESTING
    // 测试确定性 peer 接入：转发给 inner P2PDownloadManager（B5-S2）。
    void set_p2p_test_peer_endpoints_for_test(
        std::vector<lt::tcp::endpoint> endpoints);
#endif

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
    // ADR-20260901-01 D7 shutdown 顺序：成员析构逆序——p2p_（前台 cancel/
    // join + download session 收尾）先于 seeding_owner_（owner stop/join +
    // seed session 收尾）；owner 生命周期独立、由本类与 inner 共享持有。
    std::shared_ptr<P2PSeedingOwner> seeding_owner_;
    P2PDownloadManager p2p_;

    mutable std::mutex mu_;
    std::thread worker_;
    std::uint64_t generation_{0};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace device_agent
