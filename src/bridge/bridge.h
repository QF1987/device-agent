// ============================================================
// bridge/bridge.h - 业务数据桥接层
// ============================================================
// Bridge 是 device-agent 与本地业务应用之间的数据通道。
//
// 背景：
//   device-agent 运行在自助购药机上，但它本身不做购药业务。
//   购药业务由另一个进程（业务应用）处理。
//   两者通过 Unix Domain Socket（或 TCP）通信。
//
// 通信方式：
//   device-agent（客户端） ←→  Unix Domain Socket  ←→  业务应用（服务端）
//
//   device-agent 通过 Socket 发送请求，业务应用返回业务数据：
//     - get_business_metrics：获取业务指标（今日交易笔数、库存告警等）
//     - get_business_status：获取业务状态（打印机状态、药槽数量等）
//
// 这样做的好处：
//   1. 业务应用可以独立开发、升级
//   2. device-agent 只关注设备管理，不耦合业务逻辑
//   3. 同一套 device-agent 可以搭配不同业务应用
//
// 接口设计：
//   IBusinessBridge：桥接接口，定义启动/停止/轮询等操作
//   IBusinessHandler：业务处理器接口，业务应用实现它来提供数据和接收指令
//
// 当前支持：
//   SocketBridge：Unix Domain Socket（Linux/Mac）或 TCP localhost（Windows）
//   NullBridge：不连接业务应用，只有系统指标（测试用）
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
// 业务 Bridge 接口（抽象了 Socket/HTTP/Plugin 等通信方式）
//
// device-agent 通过此接口操作 Bridge：
//   - start/stop：启动和停止桥接
//   - set_handler：注册业务处理器
//   - poll_metrics/poll_status：轮询业务数据（非阻塞）
class IBusinessBridge {
public:
    virtual ~IBusinessBridge() = default;

    // 启动 Bridge（建立 socket 连接等）
    virtual bool start() = 0;

    // 停止 Bridge（断开连接）
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
};

// ─── SocketBridge ─────────────────────────────────────────
// Unix Domain Socket / TCP Bridge 实现
//
// 架构：
//   device-agent 作为 socket 客户端，
//   业务应用作为 socket 服务端（监听 Unix socket 或 TCP 端口）。
//
// 平台差异：
//   Linux/Mac：优先使用 Unix Domain Socket（更安全，不暴露到网络）
//   Windows：不支持 Unix socket，自动降级到 TCP localhost
//
// 工作线程：
//   - server_thread：监听 socket，等待业务应用连接
//   - client_thread：主动连接业务应用的 socket
//
// 注意：这是"主动连接"模式，即 device-agent 主动连接业务应用。
//   也有"被动接受"模式（device-agent 监听，业务应用连接进来）。
//   当前实现是主动连接模式。
class SocketBridge : public IBusinessBridge {
public:
    // config：Unix socket 路径（Linux/Mac）或 "host:port"（Windows）
    explicit SocketBridge(const std::string& config);
    ~SocketBridge() override;

    bool start() override;
    void stop() override;
    void set_handler(std::shared_ptr<IBusinessHandler> handler) override;
    std::string poll_metrics() override;
    std::string poll_status() override;
    const char* type() const override { return "socket"; }
    bool is_connected() const override { return connected_.load(); }

private:
    void server_loop();       // 监听 socket，等待业务应用连接
    void client_loop();       // 主动连接业务应用
    void handle_message(const std::string& json);  // 处理收到的消息
    void send_to_client(const std::string& json);  // 发送消息到业务应用
    bool setup_server_socket();   // 创建 socket 监听
    bool setup_client_socket();   // 创建 socket 连接

    std::string config_;  // socket 路径或 host:port

    std::shared_ptr<IBusinessHandler> handler_;  // 业务处理器
    std::atomic<bool> running_{false};   // 运行标志
    std::atomic<bool> connected_{false}; // 连接标志
    std::atomic<bool> is_server_{false}; // 是否是监听模式

    int server_fd_ = -1;  // 监听 socket fd
    int client_fd_ = -1;  // 已连接 socket fd

    std::thread server_thread_;  // 监听线程
    std::thread client_thread_;  // 连接线程

    // mutex 保护 latest_metrics_ 和 latest_status_
    // 因为 poll_metrics/poll_status 可能和 server_loop/client_loop 并发调用
    std::mutex metrics_mu_;
    std::mutex status_mu_;
    std::string latest_metrics_;  // 最新业务指标
    std::string latest_status_;   // 最新业务状态

    std::mutex send_mu_;  // 保护 send_to_client（socket 写操作）
};

// ─── NullBridge ───────────────────────────────────────────
// 空 Bridge（用于测试/演示，不连接业务应用）
//
// 当 type != "socket" 时使用此实现。
// 所有接口都是空操作，业务指标返回 "{}"。
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
};

}  // namespace device_agent
