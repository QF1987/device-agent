// ============================================================
// client/device_client.h - gRPC 设备客户端
// ============================================================
// DeviceClient 是 device-agent 连接服务端的唯一入口。
//
// 它封装了所有与服务端的通信：
//   1. Heartbeat（心跳） - 定期发送，证明设备活着
//   2. StatusReport（状态上报） - 定期发送设备状态（CPU/内存/业务数据）
//   3. EventReport（事件上报） - 异步发送重要事件（故障/告警）
//   4. CommandResult（指令结果回报） - 收到指令后执行完回报结果
//   5. CommandStream（指令流） - 服务端推送指令下来（长连接）
//
// 长连接 vs 短连接：
//   - Heartbeat/StatusReport/EventReport 是"请求-响应"模式（短连接）
//   - CommandStream 是"服务端推送"模式（长连接，Bidi Streaming）
//   - CommandResult 是响应 CommandStream 的，可以复用同一个连接
//
// 线程模型：
//   DeviceClient::start() 会启动三个后台线程：
//     heartbeat_thread → 心跳循环
//     status_report_thread → 状态上报循环
//     command_stream_thread → 指令接收循环（长连接）
//   这三个线程独立运行，通过 atomic 标志协调退出。
//
// 使用示例：
//   DeviceClient client(config);
//   client.set_command_callback([](const Command& cmd) { handler.handle(cmd); });
//   client.start();   // 启动三个线程
//   client.stop();    // 停止
// ============================================================

#pragma once

#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <grpcpp/grpcpp.h>

#include "terminal_agent/v1/service.grpc.pb.h"
#include "terminal_agent/v1/service.pb.h"
#include "terminal_agent/v1/device.grpc.pb.h"
#include "terminal_agent/v1/device.pb.h"

#include "config/config.h"
#include "capability/capability_manifest.h"
#include "download/network_policy.h"
#ifdef _WIN32
#include "observability/observability.h"
#endif

namespace device_agent {

// CommandCallback：指令回调函数类型
// 当收到服务端下发的指令时，DeviceClient 调用此回调
// 设备端在此回调中处理指令，然后调用 report_command_result 回报结果
using CommandCallback = std::function<void(const terminal_agent::v1::Command&)>;
using RuntimeCapabilityProvider = std::function<capability::RuntimeCapabilities()>;
// ADR-20260831-01 D4：网络类型观察回调。DeviceClient 复用 observability
// sampler 的快照，在 Windows 有有效网络事实时通知（WIFI/ETHERNET→WIFI、
// CELLULAR→CELLULAR；未知/缺失不通知，NetworkPolicy 保持 NONE fail closed）。
using NetworkTypeObserver = std::function<void(NetworkType)>;

// gRPC 设备客户端
class DeviceClient {
public:
    explicit DeviceClient(const Config& config);
    ~DeviceClient();

    // 启动客户端（启动三个后台线程）
    // start() 立即返回，线程在后台运行
    void start();

    // 停止客户端（停止所有线程）
    void stop();

    // 设置指令回调
    // 回调在 command_stream_thread 中执行，不要做耗时操作
    void set_command_callback(CommandCallback cb);

    // 必须在 start() 前设置；每次 heartbeat 调用，反映动态 runtime readiness。
    void set_runtime_capability_provider(RuntimeCapabilityProvider provider);

    // 必须在 start() 前设置；仅 Windows 生效——heartbeat/status 循环读到
    // 有效 observability 网络快照时通知（复用 sampler，无第二轮询线程）。
    void set_network_type_observer(NetworkTypeObserver observer);

    // 手动上报状态（除定期上报外，也可以主动调用）
    bool report_status(const terminal_agent::v1::StatusReport& status);

    // 手动上报事件（如故障、告警）
    bool report_event(const terminal_agent::v1::EventReport& event);

    // 回报指令执行结果
    bool report_command_result(const terminal_agent::v1::CommandResult& result);

    // 回报 Release 状态（下载/安装进度）
    // gen-android/gen-desktop 均含此 RPC，由 scripts/gen-proto.sh 统一生成
    bool report_release_status(const terminal_agent::v1::ReleaseStatusRequest& status);

    // 连接状态（用于监控）
    bool is_connected() const { return connected_.load(); }

private:
    // 三个后台循环线程
    void heartbeat_loop();           // 心跳循环
    void status_report_loop();       // 状态上报循环
    void command_stream_loop();     // 指令流循环（长连接）
    void reconnect_command_stream(); // 重连逻辑
#ifdef _WIN32
    void notify_network_type(const observability::ObservabilitySnapshot& snapshot);
#endif

    // 给每个 gRPC 请求附加认证元数据（device_id + token）
    // gRPC 的 metadata 机制：在 HTTP 头中携带认证信息
    void set_auth_metadata(grpc::ClientContext& ctx);

    const Config config_;  // 持有配置副本（避免悬垂引用）

    // gRPC Stub：生成的 RPC 客户端桩
    // unique_ptr 管理生命周期，自动释放
    std::unique_ptr<terminal_agent::v1::DeviceService::Stub> device_stub_;
    std::unique_ptr<terminal_agent::v1::CommandService::Stub> command_stub_;

    // 后台线程
    std::thread heartbeat_thread_;
    std::thread status_report_thread_;
    std::thread command_stream_thread_;

#ifdef _WIN32
    // Windows 采集在独立线程中执行；heartbeat/status 只读同一缓存快照。
    std::unique_ptr<observability::ObservabilitySampler> observability_sampler_;
#endif

    // 运行标志（atomic 保证多线程安全读写）
    std::atomic<bool> running_{false};                // start/stop 控制
    std::atomic<bool> connected_{false};               // 连接状态
    std::atomic<bool> command_stream_running_{false}; // 指令流状态

    CommandCallback command_callback_;  // 指令回调
    RuntimeCapabilityProvider runtime_capability_provider_;
    NetworkTypeObserver network_type_observer_;

    // stub_mu_：保护 stub 重连时的并发访问
    // 只有在重连时需要锁，正常运行时只有对应线程访问
    mutable std::mutex stub_mu_;

    // RV-20260602-20: CommandStream 的 ClientContext 句柄，供 stop() 取消阻塞的 Read()。
    // cmd_ctx_ 指向 reconnect_command_stream() 栈上的局部 ctx;读写/取消均持 cmd_ctx_mu_,
    // 且在该 ctx 析构前由 RAII 置空,避免 cancel-vs-destroy 的 UAF。
    std::mutex cmd_ctx_mu_;
    grpc::ClientContext* cmd_ctx_ = nullptr;

    // RV-20260602-20: 可中断 sleep —— 心跳/状态/重连退避循环用 stop_cv_ 等待,
    // stop() 置 running_=false 后 notify_all 立即唤醒,使 join 不必等满 30s/300s 间隔。
    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    // duration 内等待,期间 running_ 变 false 立即返回(优雅退出)
    void interruptible_sleep(std::chrono::seconds duration);
};

}  // namespace device_agent
