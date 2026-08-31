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
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "client/device_client.h"
#include "client/command_handler.h"
#include "config/config.h"
#include "enrollment/enrollment_manager.h"
#include "logger/logger.h"
#include "bridge/bridge.h"
#include "executor/executor.h"
#include "download/idownload_manager.h"
#ifdef __ANDROID__
#include "download/android_download_manager.h"
#elif defined(_WIN32)
#include "download/windows_download_manager.h"
#ifdef DEVICE_AGENT_ENABLE_WINDOWS_P2P
#include "config/p2p_config_store.h"
#include "download/network_policy.h"
#include "download/windows_p2p_download_manager.h"
#endif
#else
#include "download/p2p_download_manager.h"
#endif
#include "remotedesktop/remote_desktop_runtime.h"
#include "reboot_state/reboot_state.h"
#include "version/build_info.h"

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
              << "  --service    Run under Windows Service Control Manager\n"
              << "  --service-name <name>  Windows service name, default: DeviceAgent\n"
              << "  -h           Show this help\n";
}

int run_agent(int argc, char* argv[]);

#ifndef __ANDROID__
std::string desktop_download_dir() {
    const char* configured = std::getenv("DEVICE_AGENT_DOWNLOAD_DIR");
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir != nullptr && tmpdir[0] != '\0') {
        std::string dir(tmpdir);
        if (!dir.empty() && dir.back() == '/') {
            dir.pop_back();
        }
        return dir + "/device-agent-downloads";
    }
    return "./device-agent-downloads";
}

bool ensure_directory(const std::string& path) {
    if (path.empty()) {
        return true;
    }
#ifdef _WIN32
    if (::_mkdir(path.c_str()) == 0 || errno == EEXIST) {
#else
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
#endif
        return true;
    }
    LOG_ERROR("Failed to create download directory " + path + ": " + std::strerror(errno));
    return false;
}
#endif

#ifdef _WIN32
std::string env_string(const char* key) {
    const char* value = std::getenv(key);
    return value && value[0] ? std::string(value) : std::string();
}

bool env_bool(const char* key) {
    const std::string value = env_string(key);
    return value == "1" || value == "true" || value == "TRUE" || value == "yes";
}

bool parse_remote_desktop_child_args(int argc,
                                     char* argv[],
                                     device_agent::remotedesktop::RemoteDesktopRuntimeConfig& out) {
    bool child = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--remote-desktop-child") {
            child = true;
        } else if (arg == "--rd-relay" && i + 1 < argc) {
            out.relay_endpoint = argv[++i];
        } else if (arg == "--rd-device" && i + 1 < argc) {
            out.device_id = argv[++i];
        } else if (arg == "--rd-token" && i + 1 < argc) {
            out.token = argv[++i];
        } else if (arg == "--rd-server-name" && i + 1 < argc) {
            out.server_name = argv[++i];
        } else if (arg == "--rd-log" && i + 1 < argc) {
            out.child_log_path = argv[++i];
        } else if (arg == "--rd-insecure") {
            out.insecure_tls = true;
        }
    }
    return child;
}

device_agent::remotedesktop::RemoteDesktopRuntimeConfig remote_desktop_config_from_env(
        const device_agent::Config& config) {
    device_agent::remotedesktop::RemoteDesktopRuntimeConfig rd;
    rd.relay_endpoint = env_string("DEVICE_AGENT_RD_RELAY");
    rd.device_id = env_string("DEVICE_AGENT_RD_DEVICE_ID");
    if (rd.device_id.empty()) {
        rd.device_id = config.auth.device_id;
    }
    rd.token = env_string("DEVICE_AGENT_RD_TOKEN");
    if (rd.token.empty()) {
        rd.token = config.auth.token;
    }
    rd.server_name = env_string("DEVICE_AGENT_RD_SERVER_NAME");
    rd.child_log_path = env_string("DEVICE_AGENT_RD_CHILD_LOG");
    if (rd.child_log_path.empty() && !config.log_file.empty()) {
        rd.child_log_path = config.log_file + ".rd-child.log";
    }
    rd.insecure_tls = env_bool("DEVICE_AGENT_RD_INSECURE");
    rd.launch_active_session = !env_bool("DEVICE_AGENT_RD_NO_SESSION_LAUNCH");
    return rd;
}

int g_service_argc = 0;
char** g_service_argv = nullptr;
std::string g_service_name = "DeviceAgent";
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};

void report_service_status(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint_ms = 0) {
    if (g_service_status_handle == nullptr) {
        return;
    }
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit_code;
    g_service_status.dwWaitHint = wait_hint_ms;
    g_service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

DWORD WINAPI service_control_handler(DWORD control,
                                     DWORD,
                                     LPVOID,
                                     LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        g_running.store(false);
        return NO_ERROR;
    }
    return NO_ERROR;
}

void WINAPI service_main(DWORD, LPSTR*) {
    g_service_status_handle = RegisterServiceCtrlHandlerExA(
        g_service_name.c_str(),
        service_control_handler,
        nullptr);
    if (g_service_status_handle == nullptr) {
        return;
    }
    report_service_status(SERVICE_START_PENDING, NO_ERROR, 30000);
    g_running.store(true);
    report_service_status(SERVICE_RUNNING);
    const int rc = run_agent(g_service_argc, g_service_argv);
    report_service_status(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

bool has_arg(int argc, char* argv[], const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == name) {
            return true;
        }
    }
    return false;
}

std::string arg_value(int argc, char* argv[], const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

int run_windows_service(int argc, char* argv[]) {
    g_service_argc = argc;
    g_service_argv = argv;
    g_service_name = arg_value(argc, argv, "--service-name", "DeviceAgent");
    SERVICE_TABLE_ENTRYA service_table[] = {
        {const_cast<char*>(g_service_name.c_str()), service_main},
        {nullptr, nullptr},
    };
    if (StartServiceCtrlDispatcherA(service_table)) {
        return 0;
    }
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::cerr << "--service must be started by the Windows Service Control Manager\n";
    }
    return 1;
}
#endif

// ============================================================
// main()：程序入口
// ============================================================
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "device-agent " << device_agent::agent_version() << "\n";
            return 0;
        }
    }
#ifdef _WIN32
    if (has_arg(argc, argv, "--service")) {
        return run_windows_service(argc, argv);
    }
#endif
    return run_agent(argc, argv);
}

int run_agent(int argc, char* argv[]) {
#ifdef _WIN32
    device_agent::remotedesktop::RemoteDesktopRuntimeConfig child_rd_config;
    if (parse_remote_desktop_child_args(argc, argv, child_rd_config)) {
        g_running.store(true);
        if (!child_rd_config.child_log_path.empty()) {
            device_agent::Logger::instance().set_output(child_rd_config.child_log_path);
        }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::string err;
        std::atomic<bool> child_running{false};
        if (!device_agent::remotedesktop::runRemoteDesktopChild(child_rd_config, child_running, err)) {
            std::cerr << "remote desktop child failed: " << err << "\n";
            return 1;
        }
        return 0;
    }
#endif

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
    // 第 4.5 步：检查上次是否有未完成的 reboot
    // ============================================================
    // C+D 方案：如果 pending 状态文件存在，说明上次 reboot 命令没有完成
    // （可能是命令发出后系统还没重启，或者 reboot 失败了）
    // 此时文件已被清（子进程在 reboot 前会清文件），
    // 所以正常启动即可，不需要额外处理
    {
        device_agent::RebootStateManager& state_mgr = device_agent::RebootStateManager::instance();
        if (state_mgr.has_pending()) {
            LOG_WARN("Previous reboot pending state found - will be handled on next reconnect");
            state_mgr.clear_pending();
        }
    }

    // ============================================================
    // 第 5 步：配置校验
    // ============================================================
    // device_id 和 token 是必须的；缺 token 时先走首启 self-enroll。
    if (config.auth.token.empty()) {
        if (!device_agent::enrollment::ensure_enrolled(&config)) {
            return 1;
        }
    }
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
    handler.set_release_status_reporter(
        [client](const terminal_agent::v1::ReleaseStatusRequest& status) -> bool {
            return client->report_release_status(status);
        });

#ifdef _WIN32
#ifdef DEVICE_AGENT_ENABLE_WINDOWS_P2P
    // runtime p2p_config 动态声明的 ready 事实（在 Windows 分支完成 wiring 后置位；
    // remote desktop 段的合并 provider 按引用读取）。
    auto p2p_runtime_ready = std::make_shared<std::atomic<bool>>(false);
#endif
#endif

    // 根据平台选择正确的 Executor（放在配置校验前，确保日志能输出）
#ifdef __ANDROID__
    handler.set_executor(std::make_shared<device_agent::AndroidExecutor>());
    handler.set_download_manager(std::make_shared<device_agent::AndroidDownloadManager>());
    LOG_INFO("Using AndroidExecutor + AndroidDownloadManager");
#elif __APPLE__
    handler.set_executor(std::make_shared<device_agent::MacOSExecutor>());
    const std::string download_dir = desktop_download_dir();
    if (!ensure_directory(download_dir)) {
        return 1;
    }
    handler.set_download_directory(download_dir);
    handler.set_download_manager(std::make_shared<device_agent::P2PDownloadManager>());
    LOG_INFO("Using MacOSExecutor + P2PDownloadManager download_dir=" + download_dir);
#elif defined(_WIN32)
    handler.set_executor(std::make_shared<device_agent::WindowsExecutor>());
    {
        const std::string download_dir = desktop_download_dir();
        if (!ensure_directory(download_dir)) {
            return 1;
        }
        handler.set_download_directory(download_dir);
#ifdef DEVICE_AGENT_ENABLE_WINDOWS_P2P
        // ADR-20260831-01 B2：Windows production 混合 manager——P2P 主路 +
        // WinHTTP 直达/回退（WindowsDownloadManager 复用）。网络事实经
        // DeviceClient 窄回调驱动 NetworkPolicy，首个有效样本前 NONE 上传
        // fail closed；P2PConfigStore 全局默认值（backend p2p_config 下发
        // 后 apply 覆盖）。
        auto p2p_network_policy = std::make_shared<device_agent::NetworkPolicy>();
        auto p2p_http_adapter = std::make_shared<device_agent::WindowsHttpFallbackAdapter>();
        // 同一 store 实例：set_global 供 hybrid 读策略，handler 注入供
        // update_config kind=p2p_seeding 热更新（RV-20260901-WIN-P2P-B2-01，
        // 对齐 Android wiring——二者不是替代关系）。
        auto p2p_config_store = std::make_shared<device_agent::P2PConfigStore>();
        device_agent::P2PConfigStore::set_global(p2p_config_store);
        handler.set_p2p_config_store(p2p_config_store);
        auto p2p_hybrid = std::make_shared<device_agent::WindowsP2PDownloadManager>(
            p2p_network_policy,
            device_agent::P2PDownloadManager::Callbacks{},
            nullptr,       // http_manager：默认 WindowsDownloadManager
            p2p_http_adapter);
        handler.set_download_manager(p2p_hybrid);
        client->set_network_type_observer(
            [p2p_network_policy](device_agent::NetworkType type) {
                p2p_network_policy->on_network_changed(type);
            });
        // runtime p2p_config 动态声明：manager/config/network/fallback 与
        // command apply wiring 全部完成后才置 ready（compile 门由 CMake
        // option 承担）；VNC 合并的 provider 在 remote desktop 段统一设置。
        p2p_runtime_ready->store(true);
        LOG_INFO("Using WindowsExecutor + WindowsP2PDownloadManager (hybrid) download_dir=" + download_dir);
#else
        handler.set_download_manager(std::make_shared<device_agent::WindowsDownloadManager>());
        LOG_INFO("Using WindowsExecutor + WindowsDownloadManager download_dir=" + download_dir);
#endif
    }
#else
    handler.set_executor(std::make_shared<device_agent::LinuxExecutor>());
    const std::string download_dir = desktop_download_dir();
    if (!ensure_directory(download_dir)) {
        return 1;
    }
    handler.set_download_directory(download_dir);
    handler.set_download_manager(std::make_shared<device_agent::P2PDownloadManager>());
    LOG_INFO("Using LinuxExecutor + P2PDownloadManager download_dir=" + download_dir);
#endif

#ifdef _WIN32
    std::unique_ptr<device_agent::remotedesktop::RemoteDesktopRuntime> remote_desktop_runtime;
    const auto rd_config = remote_desktop_config_from_env(config);
    if (rd_config.enabled()) {
        remote_desktop_runtime.reset(new device_agent::remotedesktop::RemoteDesktopRuntime(rd_config));
        std::string rd_err;
        if (!remote_desktop_runtime->start(rd_err)) {
            LOG_ERROR("Remote desktop failed to start: " + rd_err);
        } else {
            LOG_INFO("Remote desktop runtime started");
#ifndef DEVICE_AGENT_ENABLE_WINDOWS_P2P
            client->set_runtime_capability_provider(
                [runtime = remote_desktop_runtime.get()]() {
                    device_agent::capability::RuntimeCapabilities capabilities;
                    capabilities.remote_desktop_vnc = runtime != nullptr && runtime->running();
                    return capabilities;
                });
#endif
        }
    }
#ifdef DEVICE_AGENT_ENABLE_WINDOWS_P2P
    // P2P build：VNC 与 p2p_config 就绪事实合并声明，互不覆盖（引用捕获
    // remote_desktop_runtime，按心跳时点读取 VNC 运行态）。
    client->set_runtime_capability_provider(
        [&remote_desktop_runtime, p2p_runtime_ready]() {
            device_agent::capability::RuntimeCapabilities capabilities;
            capabilities.windows_p2p_ready = p2p_runtime_ready->load();
            capabilities.remote_desktop_vnc =
                remote_desktop_runtime != nullptr && remote_desktop_runtime->running();
            return capabilities;
        });
#endif
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
            // RV-20260602-20: 按 1s 粒度检查 g_running,SIGTERM 后 join 不必等满 10s(优雅退出)
            for (int i = 0; i < 10 && g_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
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
#ifdef _WIN32
    if (remote_desktop_runtime) {
        remote_desktop_runtime->stop();
    }
#endif
    client->stop();
    bridge->stop();
    metrics_thread.join();  // 等待指标线程结束

    LOG_INFO("=== device-agent stopped ===");
    return 0;
}
