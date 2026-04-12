// ============================================================
// main.cpp - device-agent 程序入口
// ============================================================
// device-agent 是运行在自助购药机终端上的守护进程。
//
// 程序职责：
//   1. 解析命令行参数（配置文件路径 / 环境变量）
//   2. 注册信号处理器（优雅退出）
//   3. 加载配置（JSON 文件或环境变量）
//   4. 初始化日志（级别 + 输出文件）
//   5. 创建业务 Bridge（Socket 或 Null）
//   6. 创建 gRPC 客户端（心跳 + 指令接收）
//   7. 启动并等待信号退出
//
// 整体架构：
//   [device-agent]  ← 本进程
//        │
//        ├── DeviceClient (gRPC 客户端)
//        │     ├── heartbeat_loop      → 定期心跳
//        │     ├── status_report_loop → 定期状态上报
//        │     └── command_stream_loop ← 长连接接收指令
//        │
//        └── Bridge (业务数据桥接)
//              └── SocketBridge / NullBridge
//                    └── 读写 Unix Domain Socket 与本地业务应用通信
//
// 编译方式：CMake + gRPC（见 CMakeLists.txt）
// ============================================================

#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

#include "client/device_client.h"
#include "client/command_handler.h"
#include "config/config.h"
#include "logger/logger.h"
#include "bridge/bridge.h"
#include "executor/executor.h"

// ============================================================
// 匿名命名空间：工具函数和全局状态
// ============================================================
// 匿名命名空间的变量/函数只在当前文件可见（类似 static）
// 但比 static 更现代，可以用于函数和变量

// g_running：原子布尔变量，控制主循环退出
// atomic_bool 保证多线程安全读写（不需要锁）
std::atomic<bool> g_running{true};

// signal_handler：信号处理函数
// 当收到 SIGINT（Ctrl+C）或 SIGTERM（kill）时，设置退出标志
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        // store() 是原子写入，保证线程安全
        g_running.store(false);
    }
}

// print_usage：打印命令行用法
void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c <file>    Config file (JSON), default: /etc/device-agent/config.json\n"
              << "  -e           Load config from environment variables\n"
              << "  -h           Show this help\n";
}

// ============================================================
// main()：程序入口
// ============================================================
int main(int argc, char* argv[]) {

    // ============================================================
    // 第 1 步：解析命令行参数
    // ============================================================
    // getopt 是一套标准的命令行参数解析方式
    // 但这里用了手写解析（更直观）：
    //   -c <file>：指定配置文件
    //   -e：从环境变量加载配置
    //   -h：打印帮助
    std::string config_file;
    bool use_env = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            config_file = argv[++i];  // ++i 同时跳过一个参数
        } else if (arg == "-e") {
            use_env = true;
        } else if (arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // ============================================================
    // 第 2 步：注册信号处理器
    // ============================================================
    // SIGINT：Ctrl+C，SIGTERM：kill 命令
    // 注册后，这些信号会触发 signal_handler()
    // 这实现了"优雅退出"：收到信号后不立即死掉，而是设置退出标志
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ============================================================
    // 第 3 步：加载配置
    // ============================================================
    // 两种配置来源：
    //   - JSON 配置文件（-c 参数）
    //   - 环境变量（-e 参数，适合容器/嵌入式环境）
    device_agent::Config config;
    if (use_env) {
        config = device_agent::Config::load_from_env();
    } else {
        if (config_file.empty()) {
            config_file = "/etc/device-agent/config.json";  // 默认路径
        }
        config = device_agent::Config::load(config_file);
    }

    // ============================================================
    // 第 4 步：初始化日志
    // ============================================================
    // Logger 是单例模式（Singleton）
    // instance() 获取唯一实例，然后配置日志级别和输出文件
    if (config.log_level == "debug") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::DEBUG);
    } else if (config.log_level == "warn") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::WARN);
    } else if (config.log_level == "error") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::ERROR);
    } else {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::INFO);
    }

    // 如果配置了日志文件，写入文件而不是只输出到 stdout
    if (!config.log_file.empty()) {
        device_agent::Logger::instance().set_output(config.log_file);
    }

    LOG_INFO("=== device-agent starting ===");
    LOG_INFO("Device ID: " + config.auth.device_id);
    LOG_INFO("Server: " + config.server.server_address());

    // ============================================================
    // 第 5 步：配置校验
    // ============================================================
    // device_id 和 token 是必须的，没有就没法认证
    if (config.auth.device_id.empty() || config.auth.token.empty()) {
        LOG_ERROR("device_id and token are required");
        return 1;
    }

    // ============================================================
    // 第 6 步：创建业务 Bridge
    // ============================================================
    // Bridge 负责和本地业务应用通信（获取业务数据）
    //
    // 两种模式（由 business_bridge.mode 配置）：
    //   listen（默认）：device-agent 监听 socket，业务应用主动连接上来
    //                  适合 device-agent 作为后台守护进程的场景（推荐）
    //   connect：device-agent 主动连接业务应用（少数场景）
    //
    // 类型：
    //   socket：Unix Domain Socket（Linux/Mac）或 TCP（Windows）
    //   null：不连接业务应用，只有系统指标（测试/演示用）
    std::shared_ptr<device_agent::IBusinessBridge> bridge;
    if (config.business_bridge.type == "socket") {
        // 根据 mode 决定 LISTEN 还是 CONNECT
        device_agent::BridgeMode mode = device_agent::BridgeMode::LISTEN;
        if (config.business_bridge.mode == "connect") {
            mode = device_agent::BridgeMode::CONNECT;
        }
        bridge = std::make_shared<device_agent::SocketBridge>(
            config.business_bridge.path, mode);
        LOG_INFO("Business bridge: socket (" + config.business_bridge.path +
                 ") mode=" + config.business_bridge.mode);
    } else {
        bridge = std::make_shared<device_agent::NullBridge>();
        LOG_INFO("Business bridge: null (no business data)");
    }

    // ============================================================
    // 第 7 步：创建指令处理器
    // ============================================================
    // CommandHandler 接收指令、执行、回报结果
    // ResultReporter 是一个 lambda：把结果通过 DeviceClient 发给服务端
    std::shared_ptr<device_agent::DeviceClient> client(
        new device_agent::DeviceClient(config));

    device_agent::CommandHandler handler(
        [client](const terminal_agent::v1::CommandResult& result) -> bool {
            return client->report_command_result(result);
        });

    // 根据平台选择正确的 Executor（放在配置校验前，确保日志能输出）
#ifdef __APPLE__
    handler.set_executor(std::make_shared<device_agent::MacOSExecutor>());
    LOG_INFO("Using MacOSExecutor");
#else
    handler.set_executor(std::make_shared<device_agent::LinuxExecutor>());
    LOG_INFO("Using LinuxExecutor");
#endif

    // ============================================================
    // 第 5.5 步：配置校验
    // ============================================================

    // 设置指令回调：当收到服务端指令时，交给 handler 处理
    client->set_command_callback(
        [&handler](const terminal_agent::v1::Command& cmd) {
            handler.handle(cmd);
        });

    // ============================================================
    // 第 8 步：启动 Bridge
    // ============================================================
    // 启动 Unix socket 监听，等待业务应用连接
    bridge->set_handler(nullptr);  // 业务处理器暂未注册
    bridge->start();

    // ============================================================
    // 第 9 步：启动 gRPC 客户端
    // ============================================================
    // DeviceClient::start() 会启动三个后台线程：
    //   heartbeat_thread：定期心跳
    //   status_report_thread：定期状态上报
    //   command_stream_thread：长连接接收指令
    client->start();

    // ============================================================
    // 第 10 步：业务指标轮询线程
    // ============================================================
    // 定期从 Bridge 获取业务数据，注入到状态上报中
    // 目前是 TODO：等业务应用接入后实现
    std::thread metrics_thread([&]() {
        while (g_running.load()) {
            // sleep_for 是标准库的时间工具
            std::this_thread::sleep_for(std::chrono::seconds(10));
            (void)bridge;  // 暂时不用，避免编译警告
        }
    });

    // ============================================================
    // 第 11 步：主循环等待退出信号
    // ============================================================
    // 每秒检查一次退出标志
    // 收到 SIGINT/SIGTERM 后，signal_handler 会设置 g_running = false
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ============================================================
    // 第 12 步：优雅退出
    // ============================================================
    // 顺序很重要：先停客户端（不再接收新指令）
    // 再停 Bridge（断开业务应用连接）
    // 最后等后台线程结束
    LOG_INFO("Shutting down...");
    client->stop();
    bridge->stop();
    metrics_thread.join();  // 等待指标线程结束

    LOG_INFO("=== device-agent stopped ===");
    return 0;
}
