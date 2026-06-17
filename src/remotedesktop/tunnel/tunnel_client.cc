#include "remotedesktop/tunnel/tunnel_client.h"

#include "remotedesktop/tunnel/tunnel_protocol.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
static void closeSocket(socket_t s) { close(s); }
#endif

namespace device_agent::remotedesktop::tunnel {

namespace {

struct SslCtxDeleter {
    void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
};

struct SslDeleter {
    void operator()(SSL* p) const { SSL_free(p); }
};

std::string sslError(const std::string& prefix) {
    unsigned long code = ERR_get_error();
    if (code == 0) {
        return prefix;
    }
    char buf[256]{};
    ERR_error_string_n(code, buf, sizeof(buf));
    return prefix + ": " + buf;
}

socket_t connectTcp(const std::string& host, const std::string& port, std::string& err) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        err = std::string("getaddrinfo failed: ") + gai_strerror(rc);
        return kInvalidSocket;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(result, freeaddrinfo);
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) {
            continue;
        }
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            return s;
        }
        closeSocket(s);
    }
    err = "tcp connect failed";
    return kInvalidSocket;
}

class TlsConnection : public rfb::IRfbTransport {
public:
    TlsConnection(socket_t socket, std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx, std::unique_ptr<SSL, SslDeleter> ssl)
        : socket_(socket), ctx_(std::move(ctx)), ssl_(std::move(ssl)) {}

    ~TlsConnection() override {
        if (ssl_) {
            SSL_shutdown(ssl_.get());
        }
        if (socket_ != kInvalidSocket) {
            closeSocket(socket_);
        }
    }

    bool readExact(uint8_t* data, size_t size, std::string& err) override {
        size_t got = 0;
        while (got < size) {
            int n = SSL_read(ssl_.get(), data + got, static_cast<int>(size - got));
            if (n <= 0) {
                err = "TLS read failed";
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    bool writeAll(const uint8_t* data, size_t size, std::string& err) override {
        std::lock_guard<std::mutex> lock(write_mu_);
        size_t sent = 0;
        while (sent < size) {
            int n = SSL_write(ssl_.get(), data + sent, static_cast<int>(size - sent));
            if (n <= 0) {
                err = "TLS write failed";
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool writeText(const std::string& text, std::string& err) {
        return writeAll(reinterpret_cast<const uint8_t*>(text.data()), text.size(), err);
    }

    bool readLine(std::string& line, std::string& err) {
        line.clear();
        while (line.size() <= kMaxFrameLen) {
            char c = '\0';
            int n = SSL_read(ssl_.get(), &c, 1);
            if (n <= 0) {
                err = "TLS read line failed";
                return false;
            }
            line.push_back(c);
            if (c == '\n') {
                return true;
            }
        }
        err = "tunnel frame too long";
        return false;
    }

private:
    socket_t socket_;
    std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
    std::mutex write_mu_;
};

std::unique_ptr<TlsConnection> connectTls(const TunnelClientConfig& cfg, std::string& err) {
    SSL_library_init();
    SSL_load_error_strings();

    socket_t socket = connectTcp(cfg.relay_host, cfg.relay_port, err);
    if (socket == kInvalidSocket) {
        return nullptr;
    }

    std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx) {
        closeSocket(socket);
        err = sslError("SSL_CTX_new failed");
        return nullptr;
    }
    if (cfg.insecure_tls) {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
            closeSocket(socket);
            err = sslError("SSL_CTX_set_default_verify_paths failed");
            return nullptr;
        }
    }

    std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(ctx.get()));
    if (!ssl) {
        closeSocket(socket);
        err = sslError("SSL_new failed");
        return nullptr;
    }
    SSL_set_fd(ssl.get(), static_cast<int>(socket));
    std::string server_name = cfg.server_name.empty() ? cfg.relay_host : cfg.server_name;
    SSL_set_tlsext_host_name(ssl.get(), server_name.c_str());
    if (!cfg.insecure_tls) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl.get());
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(param, server_name.c_str(), 0) != 1) {
            closeSocket(socket);
            err = "TLS hostname verification setup failed";
            return nullptr;
        }
    }
    if (SSL_connect(ssl.get()) != 1) {
        closeSocket(socket);
        err = sslError("SSL_connect failed");
        return nullptr;
    }
    if (!cfg.insecure_tls && SSL_get_verify_result(ssl.get()) != X509_V_OK) {
        closeSocket(socket);
        err = "TLS certificate verification failed";
        return nullptr;
    }

    return std::make_unique<TlsConnection>(socket, std::move(ctx), std::move(ssl));
}

bool readFrame(TlsConnection& conn, std::vector<std::string>& fields, std::string& err) {
    std::string line;
    if (!conn.readLine(line, err)) {
        return false;
    }
    return parseFrame(line, fields, err);
}

}  // namespace

TunnelClient::TunnelClient(TunnelClientConfig config, rfb::RfbServer& rfb_server, BadgeCallback badge_callback)
    : config_(std::move(config)), rfb_server_(rfb_server), badge_callback_(std::move(badge_callback)) {}

bool TunnelClient::run(std::atomic<bool>& stop, std::string& err) {
    int backoff_ms = 1000;
    while (!stop.load()) {
        std::string once_err;
        if (runOnce(stop, once_err)) {
            backoff_ms = 1000;
        } else if (!stop.load()) {
            std::cerr << "tunnel disconnected: " << once_err << ", reconnect in " << backoff_ms << "ms\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 30000);
        }
    }
    err.clear();
    return true;
}

bool TunnelClient::runOnce(std::atomic<bool>& stop, std::string& err) {
    auto conn = connectTls(config_, err);
    if (!conn) {
        return false;
    }
    if (!conn->writeText(helloFrame(config_.device_id, config_.token, config_.screen_w, config_.screen_h), err)) {
        return false;
    }
    std::vector<std::string> fields;
    if (!readFrame(*conn, fields, err)) {
        return false;
    }
    if (fields.empty() || fields[0] != "OK") {
        err = "tunnel HELLO rejected";
        return false;
    }
    std::cerr << "tunnel connected as device " << config_.device_id << "\n";

    std::atomic<bool> heartbeat_stop{false};
    std::thread heartbeat([&]() {
        const int seconds = config_.heartbeat_seconds > 0 ? config_.heartbeat_seconds : 30;
        while (!heartbeat_stop.load() && !stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::string write_err;
            if (!heartbeat_stop.load() && !stop.load()) {
                (void)conn->writeText(pingFrame(), write_err);
            }
        }
    });

    while (!stop.load()) {
        if (!readFrame(*conn, fields, err)) {
            heartbeat_stop = true;
            heartbeat.join();
            return false;
        }
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "OPEN" && fields.size() == 2) {
            std::thread(&TunnelClient::handleOpen, this, fields[1]).detach();
        } else if (fields[0] == "BADGE" && fields.size() == 2) {
            if (badge_callback_) {
                badge_callback_(fields[1] == "on");
            }
            std::cerr << "tunnel badge " << fields[1] << "\n";
        } else if (fields[0] == "PONG") {
            // Heartbeat ack.
        }
    }

    heartbeat_stop = true;
    heartbeat.join();
    return true;
}

void TunnelClient::handleOpen(const std::string& stream_id) {
    std::string err;
    auto data = connectTls(config_, err);
    if (!data) {
        std::cerr << "tunnel DATA connect failed: " << err << "\n";
        return;
    }
    if (!data->writeText(dataFrame(stream_id, config_.device_id, config_.token), err)) {
        std::cerr << "tunnel DATA frame failed: " << err << "\n";
        return;
    }
    std::vector<std::string> ack;
    if (!readFrame(*data, ack, err) || ack.empty() || ack[0] != "OK") {
        std::cerr << "tunnel DATA rejected: " << err << "\n";
        return;
    }
    std::cerr << "tunnel DATA open stream=" << stream_id << "\n";
    if (!rfb_server_.serveClient(*data, err)) {
        std::cerr << "RFB stream ended: " << err << "\n";
    }
}

bool splitHostPort(const std::string& endpoint, std::string& host, std::string& port, std::string& err) {
    size_t pos = endpoint.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos == endpoint.size() - 1) {
        err = "endpoint must be host:port";
        return false;
    }
    host = endpoint.substr(0, pos);
    port = endpoint.substr(pos + 1);
    return true;
}

}  // namespace device_agent::remotedesktop::tunnel
