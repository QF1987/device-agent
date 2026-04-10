// ============================================================
// bridge/bridge.h - 业务数据桥接层
// ============================================================
// Bridge 是 device-agent 与本地业务应用之间的数据通道。
//
// 背景：
//   device-agent 作为后台守护进程运行在自助购药机上，负责设备管理。
//   购药业务由另一个进程（业务应用）处理。
//   两者通过 Unix Domain Socket（或 TCP）通信。
//
// 通信模式（两种）：
//   ── Listen 模式（device-agent 监听）───────────────
//      device-agent 作为 socket 服务端，监听一个地址。
//      业务应用主动连接上来，推送业务数据。
//      这是【默认/推荐】模式，适合后台守护进程。
//      场景：device-agent 先启动，业务应用后连接。
//
//      通信架构：
//        device-agent（监听） ←── 连接 ──→  业务应用（客户端）
//
//   ── Connect 模式（device-agent 主动连接）───────────
//      device-agent 作为 socket 客户端，主动连接业务应用。
//      适合业务应用作为服务端接收连接的架构（较少见）。
//
// 协议格式（JSON 行协议）：
//   业务应用 → device-agent：
//     {"type":"metrics","data":{...}}
//     {"type":"status","data":{...}}
//     {"type":"execute_command","id":"xxx","command_type":"xxx","payload":{}}
//   device-agent → 业务应用：
//     {"type":"command_result","id":"xxx","success":true}
//     {"type":"ping"}
//
// 接口设计：
//   IBusinessBridge：桥接接口，定义启动/停止/轮询等操作
//   IBusinessHandler：业务处理器接口，业务应用实现它来提供数据和接收指令
//
// 平台差异：
//   Linux/Mac：优先使用 Unix Domain Socket（更安全，不暴露到网络）
//   Windows：不支持 Unix socket，降级到 TCP localhost
// ============================================================

#pragma once

#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include "logger/logger.h"

namespace device_agent {

// Forward declaration：前向声明，避免循环引用
class IBusinessHandler;

// ─── Bridge 模式 ──────────────────────────────────────────
// 决定 device-agent 是监听还是主动连接
enum class BridgeMode {
    LISTEN,   // device-agent 监听 socket，业务应用连接上来（推荐）
    CONNECT   // device-agent 主动连接业务应用（少数场景）
};

// ─── IBusinessHandler ───────────────────────────────────────
// 业务处理器接口
//
// 业务应用实现此接口，注入业务数据和接收指令。
// device-agent 定期调用 get_business_metrics() 和 get_business_status()。
// 业务应用通过 execute_business_command() 接收自定义指令。
//
// 为什么用接口而不是直接调用？
//   接口解耦了 device-agent 和具体业务实现。
//   只要业务应用实现了这个接口，就可以接入 device-agent。
//   这允许同一套 agent 搭配不同业务（售药机/售货机/快递柜等）。
class IBusinessHandler {
public:
    virtual ~IBusinessHandler() = default;

    // 获取业务指标（device-agent 定期调用）
    // device-agent 会把返回的 JSON 拼接到心跳/状态上报中
    // 返回 JSON 字符串，格式由业务定义，例如：
    //   {"transactions_today": 42, "inventory_alert": false}
    virtual std::string get_business_metrics() = 0;

    // 获取业务状态，拼接到设备状态一起上报
    // 例如：{"printer_status": "ok", "medicine_slots": 20}
    virtual std::string get_business_status() = 0;

    // 执行业务指令（服务端下发的自定义指令）
    // 标准指令（reboot/config_push 等）由 device-agent 直接处理，
    // 自定义指令才转发到这里。
    // 返回 true 表示成功，false 填充 error_message。
    virtual bool execute_business_command(
        const std::string& command_type,
        const std::string& payload_json,
        std::string& error_message) = 0;

    // 业务应用就绪回调（Bridge 连接建立后调用）
    // 业务应用可以在此通知自己"device-agent 已连接"
    virtual void on_ready() = 0;
};

// ─── IBusinessBridge ───────────────────────────────────────
// 业务 Bridge 接口
//
// device-agent 通过此接口操作 Bridge：
//   - start/stop：启动和停止桥接
//   - set_handler：注册业务处理器
//   - poll_metrics/poll_status：轮询业务数据（非阻塞）
class IBusinessBridge {
public:
    virtual ~IBusinessBridge() = default;

    // 启动 Bridge
    virtual bool start() = 0;

    // 停止 Bridge
    virtual void stop() = 0;

    // 注册业务处理器
    virtual void set_handler(std::shared_ptr<IBusinessHandler> handler) = 0;

    // 轮询最新业务指标（非阻塞，返回空字符串表示无可用数据）
    // 线程安全：可以多次调用
    virtual std::string poll_metrics() = 0;

    // 轮询最新业务状态
    virtual std::string poll_status() = 0;

    // Bridge 类型标识（调试用）
    virtual const char* type() const = 0;

    // 是否已连接（业务应用已连接）
    virtual bool is_connected() const = 0;

    // 获取 Bridge 模式
    virtual BridgeMode mode() const = 0;
};

// ─── SocketBridge ─────────────────────────────────────────
// Unix Domain Socket / TCP Bridge 实现
//
// 支持两种模式（由 BridgeMode 决定）：
//   LISTEN 模式（默认）：
//     device-agent 作为服务端监听 socket。
//     业务应用主动连接上来，推送业务数据。
//     这是推荐的运行模式，适合 device-agent 作为后台守护进程。
//
//     Linux/Mac：监听 Unix Domain Socket（路径如 /var/run/device-agent/business.sock）
//     Windows：监听 TCP localhost（地址如 127.0.0.1:7890）
//
//   CONNECT 模式：
//     device-agent 作为客户端，主动连接业务应用。
//     业务应用需要在指定地址监听。
//     适合业务应用作为 socket 服务端接收连接的架构。
//
// 工作线程（LISTEN 模式下）：
//   - accept_thread：只负责 accept() 等待连接
//   - session_thread：读写已连接的 socket
//
// 断线重连（CONNECT 模式下）：
//   如果连接断开，自动重试（指数退避）
//   LISTEN 模式下由业务应用负责重连
class SocketBridge : public IBusinessBridge {
public:
    // config：socket 地址
    //   Unix socket：完整路径，如 /var/run/device-agent/business.sock
    //   TCP：地址格式 host:port，如 127.0.0.1:7890
    // mode：LISTEN（默认）或 CONNECT
    explicit SocketBridge(const std::string& config,
                          BridgeMode mode = BridgeMode::LISTEN);
    ~SocketBridge() override;

    bool start() override;
    void stop() override;
    void set_handler(std::shared_ptr<IBusinessHandler> handler) override;
    std::string poll_metrics() override;
    std::string poll_status() override;
    const char* type() const override { return "socket"; }
    bool is_connected() const override { return connected_.load(); }
    BridgeMode mode() const override { return mode_; }

private:
    // ── LISTEN 模式 ────────────────────────────────────
    // accept_loop：只负责 accept()，收到连接后启动 session_thread
    void accept_loop();

    // session_thread：已连接 socket 的读写循环
    // 由 accept_loop 在收到连接后启动
    void session_loop(int client_fd);

    // ── CONNECT 模式 ────────────────────────────────────
    // client_loop：主动连接并保持，断了自动重连
    void client_loop();

    // ── 通用 ───────────────────────────────────────────
    // 创建监听 socket（Unix 或 TCP）
    bool setup_listen_socket();
    // 创建连接 socket（Unix 或 TCP）
    bool setup_connect_socket();

    // 处理收到的 JSON 消息
    void handle_message(const std::string& json);
    // 发送消息到已连接的 socket
    void send_to_client(const std::string& json);
    // 清理 session
    void cleanup_session();

    std::string config_;        // socket 地址
    BridgeMode mode_;           // 模式

    std::shared_ptr<IBusinessHandler> handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    int listen_fd_ = -1;        // 监听 socket（LISTEN 模式）
    int session_fd_ = -1;       // 已连接 socket（当前 session）

    std::thread accept_thread_;     // accept 线程（LISTEN 模式）
    std::thread session_thread_;    // session 读写线程
    std::thread reconnect_thread_;  // 重连线程（CONNECT 模式）

    // mutex 保护 latest_metrics_ 和 latest_status_
    std::mutex metrics_mu_;
    std::mutex status_mu_;
    std::string latest_metrics_;
    std::string latest_status_;

    std::mutex send_mu_;  // 保护 send_to_client
};

// ─── NullBridge ───────────────────────────────────────────
// 空 Bridge（用于测试/演示，不连接业务应用）
class NullBridge : public IBusinessBridge {
public:
    NullBridge() = default;
    bool start() override { return true; }
    void stop() override {}
    void set_handler(std::shared_ptr<IBusinessHandler>) override {}
    std::string poll_metrics() override { return "{}"; }
    std::string poll_status() override { return "{}"; }
    const char* type() const override { return "null"; }
    bool is_connected() const override { return true; }
    BridgeMode mode() const override { return BridgeMode::LISTEN; }
};

}  // namespace device_agent
