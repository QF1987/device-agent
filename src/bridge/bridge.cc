// ============================================================
// bridge/bridge.cc - SocketBridge 实现
// ============================================================
//
// LISTEN 模式（默认）：
//   device-agent 监听在指定 socket 地址。
//   业务应用作为客户端主动连接上来。
//   通信架构：
//     device-agent (listen) ←── 连接 ──→ 业务应用 (connect)
//
// CONNECT 模式：
//   device-agent 作为客户端主动连接业务应用。
//   业务应用需要在指定地址监听。
//   断开后自动重连（指数退避）。
//
// 协议：JSON 行协议（每条消息一行，以 \n 分隔）
// ============================================================

#include "bridge/bridge.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_2.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <cstring>
#include <cerrno>
#include <sstream>
#include <iostream>
#include <chrono>

namespace device_agent {

// ─── SocketBridge ─────────────────────────────────────────

SocketBridge::SocketBridge(const std::string& config, BridgeMode mode)
    : config_(config), mode_(mode) {}

SocketBridge::~SocketBridge() {
    stop();
}

bool SocketBridge::start() {
    if (running_.exchange(true)) {
        return true;  // already started
    }

    if (mode_ == BridgeMode::LISTEN) {
        // LISTEN 模式：创建监听 socket，启动 accept 循环
        if (!setup_listen_socket()) {
            running_ = false;
            return false;
        }
        accept_thread_ = std::thread(&SocketBridge::accept_loop, this);
    } else {
        // CONNECT 模式：启动客户端连接循环（包含重连逻辑）
        reconnect_thread_ = std::thread(&SocketBridge::client_loop, this);
    }

    LOG_INFO(std::string("SocketBridge started: ") + config_ +
             " (mode=" + (mode_ == BridgeMode::LISTEN ? "LISTEN)" : "CONNECT)"));
    return true;
}

void SocketBridge::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // 关闭监听 socket
    if (listen_fd_ != -1) {
#ifdef _WIN32
        closesocket(listen_fd_);
#else
        close(listen_fd_);
#endif
        listen_fd_ = -1;
    }

    // 关闭 session socket
    cleanup_session();

    // 等待所有线程结束
    if (accept_thread_.joinable()) accept_thread_.join();
    if (session_thread_.joinable()) session_thread_.join();
    if (reconnect_thread_.joinable()) reconnect_thread_.join();

    connected_.store(false);
    LOG_INFO("SocketBridge stopped");
}

void SocketBridge::set_handler(std::shared_ptr<IBusinessHandler> handler) {
    handler_ = handler;
}

std::string SocketBridge::poll_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    std::string ret = latest_metrics_;
    latest_metrics_.clear();
    return ret;
}

std::string SocketBridge::poll_status() {
    std::lock_guard<std::mutex> lock(status_mu_);
    std::string ret = latest_status_;
    latest_status_.clear();
    return ret;
}

// ─── 通用工具 ─────────────────────────────────────────────

void SocketBridge::cleanup_session() {
    if (session_fd_ != -1) {
#ifdef _WIN32
        closesocket(session_fd_);
#else
        close(session_fd_);
#endif
        session_fd_ = -1;
    }
    connected_.store(false);
}

void SocketBridge::send_to_client(const std::string& json) {
    if (session_fd_ < 0) return;
    std::lock_guard<std::mutex> lock(send_mu_);
    std::string msg = json + "\n";
    send(session_fd_, msg.c_str(), msg.size(), 0);
}

// ─── LISTEN 模式 ───────────────────────────────────────────

bool SocketBridge::setup_listen_socket() {
#ifdef _WIN32
    // Windows: TCP socket
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERROR("WSAStartup failed");
        return false;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        LOG_ERROR("socket() failed");
        return false;
    }

    // 解析 host:port
    size_t colon = config_.find(':');
    std::string host = (colon == std::string::npos) ? "127.0.0.1" : config_.substr(0, colon);
    int port = std::stoi((colon == std::string::npos) ? config_ : config_.substr(colon + 1));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: " + std::to_string(WSAGetLastError()));
        return false;
    }
#else
    // Linux/Mac: Unix Domain Socket
    // 判断是 Unix socket 还是 TCP
    if (config_.find(':') != std::string::npos) {
        // TCP: host:port 格式
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            LOG_ERROR("socket() failed");
            return false;
        }

        size_t colon = config_.find(':');
        std::string host = config_.substr(0, colon);
        int port = std::stoi(config_.substr(colon + 1));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (host.empty()) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("bind() failed: " + std::to_string(errno));
            return false;
        }
    } else {
        // Unix Domain Socket
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            LOG_ERROR("socket() failed");
            return false;
        }

        // 删除旧 socket 文件（如果存在）
        unlink(config_.c_str());

        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, config_.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("bind() failed: " + config_);
            return false;
        }
    }
#endif

    if (listen(listen_fd_, 5) < 0) {
        LOG_ERROR("listen() failed");
        return false;
    }

    LOG_INFO("SocketBridge listening on: " + config_);
    return true;
}

// accept_loop：LISTEN 模式的核心
// 只负责 accept()，收到连接后启动 session_thread 处理读写
void SocketBridge::accept_loop() {
    LOG_INFO("SocketBridge accept loop started");

    while (running_) {
        // 设置监听 socket 为非阻塞，accept 不阻塞主循环
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(listen_fd_, FIONBIO, &mode);
#else
        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
#endif

#ifdef _WIN32
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        session_fd_ = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
#else
        if (config_.find(':') != std::string::npos) {
            // TCP
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            session_fd_ = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
        } else {
            // Unix socket
            sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            session_fd_ = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
        }
#endif

        if (session_fd_ < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                LOG_WARN("accept() failed: " + std::to_string(err));
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_WARN("accept() failed: " + std::string(strerror(errno)));
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 设置 session socket 为阻塞模式
#ifndef _WIN32
        if (config_.find(':') == std::string::npos) {
            int cflags = fcntl(session_fd_, F_GETFL, 0);
            fcntl(session_fd_, F_SETFL, cflags & ~O_NONBLOCK);
        }
#endif

        connected_.store(true);
        LOG_INFO("Business app connected");

        // 通知 handler
        if (handler_) {
            handler_->on_ready();
        }

        // 启动 session 线程处理读写
        session_thread_ = std::thread(&SocketBridge::session_loop, this, session_fd_);

        // 等待 session 结束（一个业务应用断连后，等待，再接受下一个）
        if (session_thread_.joinable()) {
            session_thread_.join();
        }

        cleanup_session();
        LOG_INFO("Business app disconnected, waiting for reconnect...");
    }
}

// session_loop：已连接 socket 的读写循环
// 在 session_thread 中运行，负责读取业务应用发来的消息
void SocketBridge::session_loop(int client_fd) {
    std::string buffer;
    char buf[4096];

    while (running_) {
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n < 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    LOG_WARN("recv() failed: " + std::to_string(err));
                }
#else
                LOG_WARN("recv() failed: " + std::string(strerror(errno)));
#endif
            }
            break;  // 连接断开或错误，退出 session_loop
        }
        buf[n] = '\0';
        buffer += buf;

        // 按行处理（JSON 行协议）
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            handle_message(line);
        }
    }
}

// ─── CONNECT 模式 ───────────────────────────────────────────

bool SocketBridge::setup_connect_socket() {
#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }

    session_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (session_fd_ == -1) return false;

    size_t colon = config_.find(':');
    std::string host = config_.substr(0, colon);
    int port = std::stoi(config_.substr(colon + 1));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(session_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(session_fd_);
        session_fd_ = -1;
        return false;
    }
#else
    if (config_.find(':') != std::string::npos) {
        // TCP
        session_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (session_fd_ == -1) return false;

        size_t colon = config_.find(':');
        std::string host = config_.substr(0, colon);
        int port = std::stoi(config_.substr(colon + 1));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(session_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(session_fd_);
            session_fd_ = -1;
            return false;
        }
    } else {
        // Unix socket
        session_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (session_fd_ == -1) return false;

        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, config_.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(session_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(session_fd_);
            session_fd_ = -1;
            return false;
        }
    }
#endif
    return true;
}

// client_loop：CONNECT 模式的核心
// 主动连接业务应用，断开后自动重连（指数退避）
void SocketBridge::client_loop() {
    LOG_INFO("SocketBridge CONNECT mode: " + config_);

    int retry_delay = 1;  // 重连延迟（秒），指数退避

    while (running_) {
        if (setup_connect_socket()) {
            connected_.store(true);
            retry_delay = 1;  // 连接成功，重置延迟
            LOG_INFO("Connected to business app");

            if (handler_) {
                handler_->on_ready();
            }

            // 读写循环（和 session_loop 一样，但断线不退出线程）
            std::string buffer;
            char buf[4096];

            while (running_) {
                int n = recv(session_fd_, buf, sizeof(buf) - 1, 0);
                if (n <= 0) {
                    break;  // 断开，跳出读写循环，触发重连
                }
                buf[n] = '\0';
                buffer += buf;

                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);
                    handle_message(line);
                }
            }

            // 断开，清理并准备重连
            cleanup_session();
            LOG_WARN("Disconnected from business app, reconnecting in " +
                     std::to_string(retry_delay) + "s...");
        }

        // 指数退避：延迟最长 60 秒
        for (int i = 0; i < retry_delay && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        retry_delay = std::min(retry_delay * 2, 60);
    }
}

// ─── 消息处理 ─────────────────────────────────────────────

void SocketBridge::handle_message(const std::string& json) {
    // 简单的 JSON 解析（生产环境建议用 nlohmann/json 或 rapidjson）
    // 协议格式：{"type":"metrics"|"status"|"execute_command", ...}

    auto get_str = [&](const std::string& key) -> std::string {
        std::string q = "\"" + key + "\"";
        size_t pos = json.find(q);
        if (pos == std::string::npos) return "";
        size_t colon = json.find(':', pos);
        if (colon == std::string::npos) return "";
        size_t quote = json.find('"', colon);
        if (quote == std::string::npos) return "";
        size_t end_quote = json.find('"', quote + 1);
        if (end_quote == std::string::npos) return "";
        return json.substr(quote + 1, end_quote - quote - 1);
    };

    std::string type = get_str("type");
    LOG_DEBUG("Socket received: " + json);

    if (type == "metrics") {
        // {"type":"metrics","data":{...}}
        size_t data_pos = json.find("\"data\"");
        if (data_pos != std::string::npos) {
            size_t brace = json.find('{', data_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                std::lock_guard<std::mutex> lock(metrics_mu_);
                latest_metrics_ = json.substr(brace, end_brace - brace + 1);
            }
        }
    } else if (type == "status") {
        // {"type":"status","data":{...}}
        size_t data_pos = json.find("\"data\"");
        if (data_pos != std::string::npos) {
            size_t brace = json.find('{', data_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                std::lock_guard<std::mutex> lock(status_mu_);
                latest_status_ = json.substr(brace, end_brace - brace + 1);
            }
        }
    } else if (type == "execute_command") {
        // {"type":"execute_command","id":"xxx","command_type":"xxx","payload":{}}
        if (!handler_) return;

        std::string id = get_str("id");
        std::string cmd_type = get_str("command_type");

        size_t payload_pos = json.find("\"payload\"");
        std::string payload = "{}";
        if (payload_pos != std::string::npos) {
            size_t brace = json.find('{', payload_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                payload = json.substr(brace, end_brace - brace + 1);
            }
        }

        std::string err;
        bool ok = handler_->execute_business_command(cmd_type, payload, err);

        // 回复执行结果
        std::ostringstream resp;
        resp << "{\"type\":\"command_result\",\"id\":\"" << id << "\",\"success\":"
             << (ok ? "true" : "false");
        if (!ok) {
            resp << ",\"message\":\"" << err << "\"";
        }
        resp << "}";

        send_to_client(resp.str());
    }
}

}  // namespace device_agent
