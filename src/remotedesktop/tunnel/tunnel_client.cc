#include "remotedesktop/tunnel/tunnel_client.h"

#include "logger/logger.h"
#include "remotedesktop/tunnel/tunnel_protocol.h"

#ifndef _WIN32
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <schannel.h>
#include <security.h>
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

#ifndef _WIN32

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

#endif

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

#ifndef _WIN32

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

#else

struct CredHandleDeleter {
    void operator()(CredHandle* h) const {
        if (h) {
            FreeCredentialsHandle(h);
            delete h;
        }
    }
};

struct CtxtHandleDeleter {
    void operator()(CtxtHandle* h) const {
        if (h) {
            DeleteSecurityContext(h);
            delete h;
        }
    }
};

std::string secError(const std::string& op, SECURITY_STATUS status) {
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", op.c_str(), static_cast<unsigned long>(status));
    return buf;
}

bool sendAllRaw(socket_t socket, const uint8_t* data, size_t size, std::string& err) {
    size_t sent = 0;
    while (sent < size) {
        int n = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (n <= 0) {
            err = "tcp send failed";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

class TlsConnection : public rfb::IRfbTransport {
public:
    explicit TlsConnection(socket_t socket) : socket_(socket) {}

    ~TlsConnection() override {
        if (context_) {
            ApplyControlToken(context_.get(), nullptr);
        }
        if (socket_ != kInvalidSocket) {
            closeSocket(socket_);
        }
    }

    bool handshake(const TunnelClientConfig& cfg, std::string& err) {
        SCHANNEL_CRED schannel{};
        schannel.dwVersion = SCHANNEL_CRED_VERSION;
        schannel.dwFlags = SCH_CRED_NO_DEFAULT_CREDS |
                           (cfg.insecure_tls ? SCH_CRED_MANUAL_CRED_VALIDATION : SCH_CRED_AUTO_CRED_VALIDATION);
        // 显式启用 TLS1.2 客户端协议:Win7 SP1 的 Schannel 默认 disabled-by-default
        // 不提供 TLS1.2(grbitEnabledProtocols=0=系统默认 → 仅 1.0/1.1),会与 relay 的
        // MinVersion=TLS1.2 握手失败(0x80090302)。显式请求让 Win7 主动协商 1.2;
        // Win10/11 同走 1.2,不受影响。免改 relay、免逐台改注册表。
        schannel.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;

        auto cred = std::unique_ptr<CredHandle, CredHandleDeleter>(new CredHandle{});
        TimeStamp expiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleA(
            nullptr,
            const_cast<SEC_CHAR*>(UNISP_NAME_A),
            SECPKG_CRED_OUTBOUND,
            nullptr,
            &schannel,
            nullptr,
            nullptr,
            cred.get(),
            &expiry);
        if (status != SEC_E_OK) {
            err = secError("AcquireCredentialsHandle", status);
            return false;
        }

        std::string target = cfg.server_name.empty() ? cfg.relay_host : cfg.server_name;
        std::vector<uint8_t> incoming;
        bool have_context = false;
        DWORD attrs = 0;
        constexpr DWORD kReq = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                               ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM |
                               ISC_REQ_ALLOCATE_MEMORY;

        for (;;) {
            SecBuffer out_buf{};
            out_buf.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc{};
            out_desc.ulVersion = SECBUFFER_VERSION;
            out_desc.cBuffers = 1;
            out_desc.pBuffers = &out_buf;

            SecBuffer in_bufs[2]{};
            SecBufferDesc in_desc{};
            if (!incoming.empty()) {
                in_bufs[0].BufferType = SECBUFFER_TOKEN;
                in_bufs[0].pvBuffer = incoming.data();
                in_bufs[0].cbBuffer = static_cast<unsigned long>(incoming.size());
                in_bufs[1].BufferType = SECBUFFER_EMPTY;
                in_desc.ulVersion = SECBUFFER_VERSION;
                in_desc.cBuffers = 2;
                in_desc.pBuffers = in_bufs;
            }

            auto next_context = std::unique_ptr<CtxtHandle, CtxtHandleDeleter>(have_context ? nullptr : new CtxtHandle{});
            CtxtHandle* out_context = have_context ? context_.get() : next_context.get();
            status = InitializeSecurityContextA(
                cred.get(),
                have_context ? context_.get() : nullptr,
                const_cast<SEC_CHAR*>(target.c_str()),
                kReq,
                0,
                SECURITY_NATIVE_DREP,
                incoming.empty() ? nullptr : &in_desc,
                0,
                out_context,
                &out_desc,
                &attrs,
                &expiry);

            if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                bool sent = sendAllRaw(socket_, static_cast<uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer, err);
                FreeContextBuffer(out_buf.pvBuffer);
                if (!sent) {
                    return false;
                }
            }

            if (!have_context && (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS)) {
                context_ = std::move(next_context);
                have_context = true;
            }

            if (status == SEC_E_OK) {
                credentials_ = std::move(cred);
                SecPkgContext_StreamSizes sizes{};
                status = QueryContextAttributesA(context_.get(), SECPKG_ATTR_STREAM_SIZES, &sizes);
                if (status != SEC_E_OK) {
                    err = secError("QueryContextAttributes stream sizes", status);
                    return false;
                }
                sizes_ = sizes;
                return true;
            }
            if (status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE && status != SEC_I_INCOMPLETE_CREDENTIALS) {
                err = secError("InitializeSecurityContext", status);
                return false;
            }

            if (!incoming.empty() && in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                const auto* extra = static_cast<const uint8_t*>(in_bufs[1].pvBuffer);
                incoming.assign(extra, extra + in_bufs[1].cbBuffer);
            } else {
                incoming.clear();
            }

            uint8_t buf[4096];
            int n = recv(socket_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) {
                err = "tcp recv during TLS handshake failed";
                return false;
            }
            incoming.insert(incoming.end(), buf, buf + n);
        }
    }

    bool readExact(uint8_t* data, size_t size, std::string& err) override {
        size_t got = 0;
        while (got < size) {
            if (plain_.empty() && !decryptMore(err)) {
                return false;
            }
            size_t take = std::min(size - got, plain_.size());
            std::memcpy(data + got, plain_.data(), take);
            plain_.erase(plain_.begin(), plain_.begin() + static_cast<std::ptrdiff_t>(take));
            got += take;
        }
        return true;
    }

    bool writeAll(const uint8_t* data, size_t size, std::string& err) override {
        std::lock_guard<std::mutex> lock(write_mu_);
        size_t offset = 0;
        const size_t max_message = sizes_.cbMaximumMessage > 0 ? sizes_.cbMaximumMessage : 16384;
        while (offset < size) {
            size_t chunk = std::min(size - offset, max_message);
            std::vector<uint8_t> packet(sizes_.cbHeader + chunk + sizes_.cbTrailer);
            std::memcpy(packet.data() + sizes_.cbHeader, data + offset, chunk);

            SecBuffer bufs[4]{};
            bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
            bufs[0].pvBuffer = packet.data();
            bufs[0].cbBuffer = sizes_.cbHeader;
            bufs[1].BufferType = SECBUFFER_DATA;
            bufs[1].pvBuffer = packet.data() + sizes_.cbHeader;
            bufs[1].cbBuffer = static_cast<unsigned long>(chunk);
            bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
            bufs[2].pvBuffer = packet.data() + sizes_.cbHeader + chunk;
            bufs[2].cbBuffer = sizes_.cbTrailer;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = bufs;

            SECURITY_STATUS status = EncryptMessage(context_.get(), 0, &desc, 0);
            if (status != SEC_E_OK) {
                err = secError("EncryptMessage", status);
                return false;
            }
            size_t encrypted_size = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
            if (!sendAllRaw(socket_, packet.data(), encrypted_size, err)) {
                return false;
            }
            offset += chunk;
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
            if (!readExact(reinterpret_cast<uint8_t*>(&c), 1, err)) {
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
    bool decryptMore(std::string& err) {
        for (;;) {
            if (encrypted_.empty()) {
                uint8_t buf[8192];
                int n = recv(socket_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) {
                    err = "TLS read failed";
                    return false;
                }
                encrypted_.insert(encrypted_.end(), buf, buf + n);
            }

            SecBuffer bufs[4]{};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer = encrypted_.data();
            bufs[0].cbBuffer = static_cast<unsigned long>(encrypted_.size());
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = bufs;

            SECURITY_STATUS status = DecryptMessage(context_.get(), &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                uint8_t buf[8192];
                int n = recv(socket_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) {
                    err = "TLS read failed";
                    return false;
                }
                encrypted_.insert(encrypted_.end(), buf, buf + n);
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) {
                err = "TLS context closed";
                return false;
            }
            if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
                err = secError("DecryptMessage", status);
                return false;
            }

            std::vector<uint8_t> extra;
            for (auto& buf : bufs) {
                if (buf.BufferType == SECBUFFER_DATA && buf.cbBuffer > 0) {
                    const auto* p = static_cast<const uint8_t*>(buf.pvBuffer);
                    plain_.insert(plain_.end(), p, p + buf.cbBuffer);
                } else if (buf.BufferType == SECBUFFER_EXTRA && buf.cbBuffer > 0) {
                    const auto* p = static_cast<const uint8_t*>(buf.pvBuffer);
                    extra.assign(p, p + buf.cbBuffer);
                }
            }
            encrypted_ = std::move(extra);
            if (!plain_.empty()) {
                return true;
            }
        }
    }

    socket_t socket_;
    std::unique_ptr<CredHandle, CredHandleDeleter> credentials_;
    std::unique_ptr<CtxtHandle, CtxtHandleDeleter> context_;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<uint8_t> encrypted_;
    std::vector<uint8_t> plain_;
    std::mutex write_mu_;
};

std::unique_ptr<TlsConnection> connectTls(const TunnelClientConfig& cfg, std::string& err) {
    socket_t socket = connectTcp(cfg.relay_host, cfg.relay_port, err);
    if (socket == kInvalidSocket) {
        return nullptr;
    }
    auto conn = std::make_unique<TlsConnection>(socket);
    if (!conn->handshake(cfg, err)) {
        return nullptr;
    }
    return conn;
}

#endif

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

TunnelClient::~TunnelClient() {
    reapOpenWorkers(true);
}

bool TunnelClient::run(std::atomic<bool>& stop, std::string& err) {
    int backoff_ms = 1000;
    while (!stop.load()) {
        std::string once_err;
        if (runOnce(stop, once_err)) {
            backoff_ms = 1000;
        } else if (!stop.load()) {
            LOG_WARN("tunnel disconnected: " + once_err + ", reconnect in " +
                     std::to_string(backoff_ms) + "ms");
            for (int slept = 0; slept < backoff_ms && !stop.load(); slept += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100, backoff_ms - slept)));
            }
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
    LOG_INFO("tunnel connected as device " + config_.device_id);

    std::atomic<bool> heartbeat_stop{false};
    std::thread heartbeat([&]() {
        const int seconds = config_.heartbeat_seconds > 0 ? config_.heartbeat_seconds : 30;
        while (!heartbeat_stop.load() && !stop.load()) {
            for (int i = 0; i < seconds * 10 && !heartbeat_stop.load() && !stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
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
            reapOpenWorkers(true);
            return false;
        }
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "OPEN" && fields.size() == 2) {
            startOpen(fields[1]);
            reapOpenWorkers(false);
        } else if (fields[0] == "BADGE" && fields.size() == 2) {
            if (badge_callback_) {
                badge_callback_(fields[1] == "on");
            }
            LOG_INFO("tunnel badge " + fields[1]);
        } else if (fields[0] == "PONG") {
            // Heartbeat ack.
        }
    }

    heartbeat_stop = true;
    heartbeat.join();
    reapOpenWorkers(true);
    return true;
}

void TunnelClient::startOpen(const std::string& stream_id) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, stream_id, done]() {
        handleOpen(stream_id);
        done->store(true);
    });
    std::lock_guard<std::mutex> lock(open_workers_mu_);
    open_workers_.push_back(OpenWorker{std::move(worker), std::move(done)});
}

void TunnelClient::handleOpen(const std::string& stream_id) {
    std::string err;
    auto data = connectTls(config_, err);
    if (!data) {
        LOG_WARN("tunnel DATA connect failed: " + err);
        return;
    }
    if (!data->writeText(dataFrame(stream_id, config_.device_id, config_.token), err)) {
        LOG_WARN("tunnel DATA frame failed: " + err);
        return;
    }
    std::vector<std::string> ack;
    if (!readFrame(*data, ack, err) || ack.empty() || ack[0] != "OK") {
        LOG_WARN("tunnel DATA rejected: " + err);
        return;
    }
    LOG_INFO("tunnel DATA open stream=" + stream_id);
    if (!rfb_server_.serveClient(*data, err)) {
        LOG_WARN("RFB stream ended: " + err);
    }
}

void TunnelClient::reapOpenWorkers(bool join_all) {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(open_workers_mu_);
        auto it = open_workers_.begin();
        while (it != open_workers_.end()) {
            if (join_all || (it->done && it->done->load())) {
                if (it->thread.joinable()) {
                    to_join.push_back(std::move(it->thread));
                }
                it = open_workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& worker : to_join) {
        worker.join();
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
