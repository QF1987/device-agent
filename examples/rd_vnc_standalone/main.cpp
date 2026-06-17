#include "remotedesktop/rfb/rfb_server.h"
#include "remotedesktop/test/test_pattern.h"
#include "remotedesktop/tunnel/tunnel_client.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "remotedesktop/platform/windows/windows_input_injector.h"
#include "remotedesktop/platform/windows/windows_screen_capturer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
static void closeSocket(socket_t s) { close(s); }
#endif

namespace {

struct Options {
    int listen_port = 5901;
    int width = 800;
    int height = 600;
    bool tunnel = false;
    bool insecure = false;
    std::string relay;
    std::string device;
    std::string token;
    std::string server_name;
};

std::atomic<bool> g_stop{false};

class SocketTransport : public device_agent::remotedesktop::rfb::IRfbTransport {
public:
    explicit SocketTransport(socket_t socket) : socket_(socket) {}
    ~SocketTransport() override { closeSocket(socket_); }

    bool readExact(uint8_t* data, size_t size, std::string& err) override {
        size_t got = 0;
        while (got < size) {
#ifdef _WIN32
            int n = recv(socket_, reinterpret_cast<char*>(data + got), static_cast<int>(size - got), 0);
#else
            ssize_t n = recv(socket_, reinterpret_cast<char*>(data + got), size - got, 0);
#endif
            if (n <= 0) {
                err = "recv failed";
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    bool writeAll(const uint8_t* data, size_t size, std::string& err) override {
        size_t sent = 0;
        while (sent < size) {
#ifdef _WIN32
            int n = send(socket_, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
#else
            ssize_t n = send(socket_, reinterpret_cast<const char*>(data + sent), size - sent, 0);
#endif
            if (n <= 0) {
                err = "send failed";
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

private:
    socket_t socket_;
};

void usage() {
    std::cout
        << "rd_vnc_standalone [--listen-port 5901] [--width 800] [--height 600]\n"
        << "rd_vnc_standalone --relay host:port --device DEVICE --token TOKEN [--insecure] [--servername NAME]\n";
}

bool parseOptions(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--listen-port") {
            const char* value = need("--listen-port");
            if (!value) return false;
            opt.listen_port = std::stoi(value);
        } else if (arg == "--width") {
            const char* value = need("--width");
            if (!value) return false;
            opt.width = std::stoi(value);
        } else if (arg == "--height") {
            const char* value = need("--height");
            if (!value) return false;
            opt.height = std::stoi(value);
        } else if (arg == "--relay") {
            const char* value = need("--relay");
            if (!value) return false;
            opt.relay = value;
            opt.tunnel = true;
        } else if (arg == "--device") {
            const char* value = need("--device");
            if (!value) return false;
            opt.device = value;
        } else if (arg == "--token") {
            const char* value = need("--token");
            if (!value) return false;
            opt.token = value;
        } else if (arg == "--servername") {
            const char* value = need("--servername");
            if (!value) return false;
            opt.server_name = value;
        } else if (arg == "--insecure") {
            opt.insecure = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else if (i == 1 && arg.find_first_not_of("0123456789") == std::string::npos) {
            opt.listen_port = std::stoi(arg);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    if (opt.tunnel && (opt.relay.empty() || opt.device.empty() || opt.token.empty())) {
        std::cerr << "--relay mode requires --device and --token\n";
        return false;
    }
    return true;
}

void onSignal(int) {
    g_stop = true;
}

bool initSockets() {
#ifdef _WIN32
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void cleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int runListenMode(const Options& opt,
                  device_agent::remotedesktop::IScreenCapturer& capturer,
                  device_agent::remotedesktop::IInputInjector& injector) {
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        std::cerr << "socket failed\n";
        return 1;
    }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
#ifdef __APPLE__
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(opt.listen_port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << "\n";
        closeSocket(listener);
        return 1;
    }
    std::cout << "rd_vnc_standalone listening on :" << opt.listen_port
              << " with fake frame source " << opt.width << "x" << opt.height << "\n";
    while (!g_stop.load()) {
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) {
            break;
        }
        std::thread([client, &capturer, &injector]() {
            device_agent::remotedesktop::rfb::RfbServer server(capturer, injector, "DeviceOps Test Pattern");
            SocketTransport transport(client);
            std::string err;
            if (!server.serveClient(transport, err)) {
                std::cerr << "RFB session ended: " << err << "\n";
            }
        }).detach();
    }
    closeSocket(listener);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseOptions(argc, argv, opt)) {
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!initSockets()) {
        std::cerr << "socket init failed\n";
        return 1;
    }
    device_agent::remotedesktop::test::TestPatternCapturer fake_capturer(
        static_cast<uint16_t>(opt.width), static_cast<uint16_t>(opt.height));
    device_agent::remotedesktop::test::LoggingInputInjector logging_input;

    int rc = 0;
    if (opt.tunnel) {
        std::string host, port, split_err;
        if (!device_agent::remotedesktop::tunnel::splitHostPort(opt.relay, host, port, split_err)) {
            std::cerr << split_err << "\n";
            cleanupSockets();
            return 2;
        }
        device_agent::remotedesktop::rfb::RfbServer server(fake_capturer, logging_input, "DeviceOps Tunnel Test Pattern");
        device_agent::remotedesktop::tunnel::TunnelClientConfig cfg;
        cfg.relay_host = host;
        cfg.relay_port = port;
        cfg.device_id = opt.device;
        cfg.token = opt.token;
        cfg.insecure_tls = opt.insecure;
        cfg.server_name = opt.server_name;
        cfg.screen_w = opt.width;
        cfg.screen_h = opt.height;
        cfg.heartbeat_seconds = 5;
        device_agent::remotedesktop::tunnel::TunnelClient client(
            cfg, server, [](bool on) { std::cerr << "BADGE " << (on ? "on" : "off") << "\n"; });
        std::string err;
        if (!client.run(g_stop, err)) {
            std::cerr << "tunnel client failed: " << err << "\n";
            rc = 1;
        }
    } else {
#ifdef _WIN32
        device_agent::remotedesktop::windows::WindowsScreenCapturer real_capturer;
        device_agent::remotedesktop::windows::WindowsInputInjector real_input;
        rc = runListenMode(opt, real_capturer, real_input);
#else
        rc = runListenMode(opt, fake_capturer, logging_input);
#endif
    }
    cleanupSockets();
    return rc;
}
