// ============================================================
// download/windows_p2p_download_manager_test.cc
// ============================================================
// ADR-20260831-01 · B1 离线确定性测试：路由、单 active、P2P 失败回退一次、
// cancel race、generation guard、生产 WinHTTP 适配（loopback）。
// 全程无公网 tracker / 真实 backend / 家庭设备；HTTP 仅 loopback/不可达端口。
// ============================================================

#include "download/windows_p2p_download_manager.h"

#include "config/p2p_config_store.h"
#include "download/network_policy.h"
#include "download/p2p_s2_test_helpers.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kShaFixture = "device-agent windows hybrid fixture\n";
constexpr const char* kShaFixtureDigest =
    "ceeebd5361828983ce1c8ed485d6f8e54e2cb2f6f34c68735d97ba8fd83fea53";

std::string bencoded_string(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

// 20 字节 piece hash（与 Phase A fixture 同构，hash 不需真实匹配——下载走回退）。
const unsigned char kPieceHash[20] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
    0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05,
};

int test_pid() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

void test_mkdir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
}

std::string test_temp_base() {
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr) {
        tmp = std::getenv("TMP");
    }
    return tmp != nullptr ? std::string(tmp) : std::string(".");
#else
    const char* tmp = std::getenv("TMPDIR");
    return tmp != nullptr ? std::string(tmp) : std::string("/tmp");
#endif
}

std::string make_test_dir(const char* tag) {
    std::string dir = test_temp_base() + "/windows-p2p-hybrid-test-" + tag + "-" +
                      std::to_string(test_pid());
    test_mkdir(dir);
    return dir;
}

void write_seedless_torrent(const std::string& path) {
    const std::string announce = "http://127.0.0.1:9/announce";
    const std::string web_seed = "http://127.0.0.1:9/test.bin";

    std::string data = "d";
    data += "8:announce" + bencoded_string(announce);
    data += "4:info";
    data += "d6:lengthi16384e";
    data += "4:name" + bencoded_string("test.bin");
    data += "12:piece lengthi16384e";
    data += "6:pieces20:";
    data.append(reinterpret_cast<const char*>(kPieceHash), sizeof(kPieceHash));
    data += "e";
    data += "8:url-list" + bencoded_string(web_seed);
    data += "e";

    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void assert_true(bool value) {
    assert(value);
}

bool wait_until_downloading(device_agent::WindowsP2PDownloadManager& manager) {
    for (int i = 0; i < 40; ++i) {
        if (manager.is_downloading()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// worker 回调 → 主线程结果快照（mutex+cv，禁止普通 bool 跨线程轮询）。
struct CompletionCapture {
    void store(bool success, const std::string& error,
               const device_agent::DownloadCompletionTelemetry* telemetry) {
        std::lock_guard<std::mutex> lock(mu_);
        done_ = true;
        ok_ = success;
        err_ = error;
        if (telemetry != nullptr) {
            completion_path_ = telemetry->completion_path;
            peer_bytes_ = telemetry->peer_bytes;
            web_seed_bytes_ = telemetry->web_seed_bytes;
        }
        cv_.notify_all();
    }

    bool wait_and_get(bool* ok_out, std::string* err_out,
                      int* completion_path_out = nullptr,
                      int64_t* web_seed_bytes_out = nullptr) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool completed = cv_.wait_for(lock, std::chrono::seconds(10),
                                            [this] { return done_; });
        if (ok_out != nullptr) *ok_out = ok_;
        if (err_out != nullptr) *err_out = err_;
        if (completion_path_out != nullptr) *completion_path_out = completion_path_;
        if (web_seed_bytes_out != nullptr) *web_seed_bytes_out = web_seed_bytes_;
        return completed;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    bool ok_ = false;
    std::string err_;
    int completion_path_ = -1;
    int64_t peer_bytes_ = -1;
    int64_t web_seed_bytes_ = -1;
};

// 可配置的 HTTP manager fake：记录调用、可写 fixture 成功/失败/阻塞。
class FakeHttpManager : public device_agent::IDownloadManager {
public:
    device_agent::P2PDownloadManager::HttpFallback as_fallback() {
        return [this](const std::string& url, const std::string& output_path,
                      std::string& error) {
            ++calls;
            last_url = url;
            last_path = output_path;
            if (fail_next) {
                error = "fake http failure";
                return false;
            }
            std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
            out << kShaFixture;
            return static_cast<bool>(out);
        };
    }

    void download(const device_agent::DownloadRequest& req,
                  device_agent::IDownloadManager::ProgressCallback on_progress,
                  device_agent::IDownloadManager::CompleteCallback on_complete) override {
        (void)on_progress;
        ++calls;
        last_url = req.url;
        last_path = req.dest_path;
        device_agent::DownloadCompletionTelemetry telemetry;
        if (fail_next) {
            if (on_complete) {
                on_complete(false, "fake http failure", telemetry);
            }
            return;
        }
        std::string path = req.dest_path;
        if (!path.empty() && path.back() != '\\' && path.back() != '/') {
#ifdef _WIN32
            path += "\\";
#else
            path += "/";
#endif
        }
        path += req.file_id.empty() ? std::string("download.bin") : req.file_id;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << kShaFixture;
        telemetry.completion_path = 2;  // WebSeedPrimary
        telemetry.web_seed_bytes = static_cast<int64_t>(out.tellp());
        if (on_complete) {
            on_complete(true, "", telemetry);
        }
    }

    void cancel() override {
        ++cancel_calls;
    }

    bool is_downloading() const override {
        return false;
    }

    std::atomic<int> calls{0};
    std::atomic<int> cancel_calls{0};
    std::atomic<bool> fail_next{false};
    std::string last_url;
    std::string last_path;
};

// ─── Windows 真实适配 loopback server（_WIN32 only）─────────────────
#ifdef _WIN32

class LoopbackServer {
public:
    bool start(const std::string& fixture_bytes, const std::string& torrent_bytes) {
        fixture_ = fixture_bytes;
        torrent_ = torrent_bytes;
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(sock_, 4) != 0) {
            return false;
        }
        int len = sizeof(addr);
        if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    int port() const { return port_; }

    void stop() {
        if (sock_ != INVALID_SOCKET) {
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        WSACleanup();
    }

private:
    void serve() {
        for (int i = 0; i < 4; ++i) {
            SOCKET client = ::accept(sock_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            char buffer[2048];
            const int received = ::recv(client, buffer, sizeof(buffer), 0);
            const std::string request(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);
            const bool want_torrent = request.find("fixture.torrent") != std::string::npos;
            const std::string& body = want_torrent ? torrent_ : fixture_;
            std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                   std::to_string(body.size()) +
                                   "\r\nConnection: close\r\n\r\n" + body;
            ::send(client, response.data(), static_cast<int>(response.size()), 0);
            ::closesocket(client);
        }
    }

    std::string fixture_;
    std::string torrent_;
    SOCKET sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
};

#endif  // _WIN32

}  // namespace

int main() {
    using device_agent::DownloadRequest;
    using device_agent::NetworkPolicy;
    using device_agent::NetworkType;
    using device_agent::P2PConfigStore;
    using device_agent::P2PDownloadManager;
    using device_agent::WindowsHttpFallbackAdapter;
    using device_agent::WindowsP2PDownloadManager;

    const std::string dir = make_test_dir("main");
    const std::string save_dir = dir + "\\save";
    test_mkdir(save_dir);
    const std::string torrent_path = dir + "\\fixture.torrent";
    write_seedless_torrent(torrent_path);

    std::fprintf(stderr, "[TEST] 1 http-only\n");
    // 1. HTTP-only 路由：无 P2P source → 直达 http manager，telemetry 透传。
    {
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.url = "http://127.0.0.1:9/direct.bin";
        req.dest_path = save_dir;
        req.file_id = "direct.bin";
        req.expected_sha256 = kShaFixtureDigest;
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        std::string err;
        int completion_path = -1;
        int64_t web_seed_bytes = -1;
        assert_true(capture.wait_and_get(&ok, &err, &completion_path, &web_seed_bytes));
        assert(ok);
        assert(err.empty());
        assert(completion_path == 2);
        assert(web_seed_bytes > 0);
        assert(http.calls.load() == 1);
    }

    std::fprintf(stderr, "[TEST] 2 policy-disabled\n");
    // 2. policy disabled → HTTP 路由。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"p2p_enabled":false})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.torrent_url = "C:\\fixtures\\x.torrent";
        req.url = "http://127.0.0.1:9/fallback.bin";
        req.dest_path = save_dir;
        req.file_id = "fallback.bin";
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        assert_true(capture.wait_and_get(&ok, nullptr));
        assert(ok);
        assert(http.calls.load() == 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 3 min-size\n");
    // 3. min_file_size 门槛 → HTTP 路由。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":64})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.torrent_url = "C:\\fixtures\\x.torrent";
        req.url = "http://127.0.0.1:9/small.bin";
        req.dest_path = save_dir;
        req.file_id = "small.bin";
        req.file_size = 16 * 1024 * 1024;
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        assert_true(capture.wait_and_get(&ok, nullptr));
        assert(ok);
        assert(http.calls.load() == 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 4 cellular-denied\n");
    // 4. cellular denied → HTTP 路由。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"cellular_download_enabled":false,"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        auto network_policy = std::make_shared<NetworkPolicy>();
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(network_policy, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        network_policy->on_network_changed(NetworkType::CELLULAR);
        DownloadRequest req;
        req.torrent_url = "C:\\fixtures\\x.torrent";
        req.url = "http://127.0.0.1:9/cell.bin";
        req.dest_path = save_dir;
        req.file_id = "cell.bin";
        req.file_size = 64 * 1024 * 1024;
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        assert_true(capture.wait_and_get(&ok, nullptr));
        assert(ok);
        assert(http.calls.load() == 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 5 p2p-fail-fallback\n");
    // 5. P2P 失败（invalid magnet 快败）→ 回退 HTTP 恰好一次 → 成功。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.magnet_uri = "magnet:?xt=urn:btih:invalid";
        req.url = "http://127.0.0.1:9/after-p2p.bin";
        req.dest_path = save_dir;
        req.file_id = "after-p2p.bin";
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        std::string err;
        int completion_path = -1;
        assert_true(capture.wait_and_get(&ok, &err, &completion_path));
        assert(ok);
        assert(completion_path == 2);
        assert(http.calls.load() == 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 6 no-fallback\n");
    // 6. P2P 失败且无 http url → 不回退，透传 P2P 失败。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.magnet_uri = "magnet:?xt=urn:btih:invalid";
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = true;
        std::string err;
        assert_true(capture.wait_and_get(&ok, &err));
        assert(!ok);
        assert(err.find("failed to parse magnet URI") != std::string::npos);
        assert(http.calls.load() == 0);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 7 metadata-fallback\n");
    // 7. metadata 经适配失败 → hybrid 回退一次成功（torrent_url=http）。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(nullptr, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.torrent_url = "http://127.0.0.1:9/fixture.torrent";
        req.url = "http://127.0.0.1:9/meta-fallback.bin";
        req.dest_path = save_dir;
        req.file_id = "meta-fallback.bin";
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        assert_true(capture.wait_and_get(&ok, nullptr));
        assert(ok);
        assert(http.calls.load() == 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 8 cancel-race\n");
    // 8. cancel race：P2P 路由下载中取消 → 恰好一次取消失败，不启动回退。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        auto network_policy = std::make_shared<NetworkPolicy>();
        network_policy->on_network_changed(NetworkType::WIFI);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(network_policy, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.torrent_url = torrent_path;
        req.url = "http://127.0.0.1:9/after-cancel.bin";
        req.dest_path = save_dir;
        req.file_id = "after-cancel.bin";
        req.file_size = 64 * 1024 * 1024;
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        assert_true(wait_until_downloading(manager));
        manager.cancel();
        bool ok = true;
        std::string err;
        assert_true(capture.wait_and_get(&ok, &err));
        assert(!ok);
        assert(err.find("cancel") != std::string::npos);
        assert(http.calls.load() == 0);
        assert(http.cancel_calls.load() >= 1);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 9 single-active\n");
    // 9. 单 active：进行中第二个请求立即失败。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        auto network_policy = std::make_shared<NetworkPolicy>();
        network_policy->on_network_changed(NetworkType::WIFI);
        FakeHttpManager http;
        WindowsP2PDownloadManager manager(network_policy, {}, std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}));
        DownloadRequest req;
        req.torrent_url = torrent_path;
        req.dest_path = save_dir;
        req.file_size = 64 * 1024 * 1024;
        CompletionCapture first;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            first.store(ok, err, &t);
        });
        assert(manager.is_downloading());
        bool second_ok = true;
        std::string second_err;
        manager.download(req, nullptr,
                         [&](bool ok, const std::string& err,
                             const device_agent::DownloadCompletionTelemetry&) {
                             second_ok = ok;
                             second_err = err;
                         });
        assert(!second_ok);
        assert(second_err.find("already active") != std::string::npos);
        assert(http.calls.load() == 0);
        manager.cancel();
        bool first_ok = true;
        assert_true(first.wait_and_get(&first_ok, nullptr));
        assert(!first_ok);
        P2PConfigStore::set_global(nullptr);
    }

    std::fprintf(stderr, "[TEST] 10 default-adapter\n");
    // 10. 默认适配（POSIX fail-closed / Windows 真实 WDM）：无注入时直达路由。
    {
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);
        WindowsP2PDownloadManager manager;
        DownloadRequest req;
        req.url = "http://127.0.0.1:9/default.bin";
        req.dest_path = save_dir;
        req.file_id = "default.bin";
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = true;
        std::string err;
        assert_true(capture.wait_and_get(&ok, &err));
#ifdef _WIN32
        // 真实 WDM 对 127.0.0.1:9（discard 端口，无监听）必然失败但错误非空。
        assert(!ok);
        assert(!err.empty());
#else
        assert(!ok);
        assert(err.find("unavailable") != std::string::npos);
#endif
        P2PConfigStore::set_global(nullptr);
    }

#ifdef _WIN32
    // 11. Windows 真实 WinHTTP 适配端到端（loopback）：直达下载 + 元数据 fallback。
    {
        LoopbackServer server;
        assert_true(server.start(kShaFixture, ""));
        const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);

        WindowsP2PDownloadManager manager;
        DownloadRequest req;
        req.url = base + "/payload.bin";
        req.dest_path = save_dir;
        req.file_id = "payload.bin";
        req.expected_sha256 = kShaFixtureDigest;
        CompletionCapture capture;
        manager.download(req, nullptr, [&](bool ok, const std::string& err,
                                           const device_agent::DownloadCompletionTelemetry& t) {
            capture.store(ok, err, &t);
        });
        bool ok = false;
        std::string err;
        int completion_path = -1;
        int64_t web_seed_bytes = -1;
        assert_true(capture.wait_and_get(&ok, &err, &completion_path, &web_seed_bytes));
        assert(ok);
        assert(err.empty());
        assert(completion_path == 2);
        assert(web_seed_bytes > 0);

        std::ifstream downloaded(save_dir + "\\payload.bin", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(downloaded)),
                            std::istreambuf_iterator<char>());
        assert(content == kShaFixture);

        server.stop();
        P2PConfigStore::set_global(nullptr);
    }
#endif

    std::fprintf(stderr, "[TEST] 12 adapter-latch\n");
    // 12. adapter latch/发布原子性回归（RV-20260831-WIN-P2P-B1-01）。
    //     (a) cancel 先于 run：latch 生效，job factory 不被调用（不启动）。
    //     (b) cancel 恰落在发布边界之后（gate 保证已发布、job 未启动）：
    //         request_cancel 必达 job，run 以 cancelled 失败，恰好一次返回。
    //     (c) reset 清 latch 后可正常运行。
    {
        using device_agent::WindowsHttpFallbackAdapter;
        struct GateJob : WindowsHttpFallbackAdapter::RunJob {
            std::atomic<int> cancel_calls{0};
            std::atomic<bool> released{false};
            std::atomic<bool> succeed{false};
            bool execute(std::string& error) override {
                while (!released.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (cancel_calls.load() > 0) {
                    error = "windows http fallback cancelled";
                    return false;
                }
                if (!succeed.load()) {
                    error = "fake job failure";
                    return false;
                }
                return true;
            }
            void request_cancel() override { ++cancel_calls; }
        };

        // (a) latch 前置：run 不启动 job。
        {
            WindowsHttpFallbackAdapter adapter;
            std::atomic<int> factory_calls{0};
            adapter.cancel();
            adapter.set_job_factory_for_test(
                [&](const std::string&, const std::string&) {
                    ++factory_calls;
                    return std::unique_ptr<WindowsHttpFallbackAdapter::RunJob>(
                        nullptr);
                });
            std::string err;
            const bool ok = adapter.fallback()("http://127.0.0.1:9/x", "out", err);
            assert(!ok);
            assert(err.find("cancelled") != std::string::npos);
            assert(factory_calls.load() == 0);
        }

        // (b) 发布边界 cancel：必达 job → cancelled 失败，恰好一次。
        {
            WindowsHttpFallbackAdapter adapter;
            auto cancel_counter = std::make_shared<std::atomic<int>>(0);
            auto release_flag = std::make_shared<std::atomic<bool>>(false);
            adapter.set_job_factory_for_test(
                [cancel_counter, release_flag](const std::string&,
                                               const std::string&) {
                    struct GateJob : WindowsHttpFallbackAdapter::RunJob {
                        std::shared_ptr<std::atomic<int>> counter;
                        std::shared_ptr<std::atomic<bool>> release;
                        bool execute(std::string& error) override {
                            while (!release->load()) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(5));
                            }
                            if (counter->load() > 0) {
                                error = "windows http fallback cancelled";
                                return false;
                            }
                            error = "fake job failure";
                            return false;
                        }
                        void request_cancel() override { ++(*counter); }
                    };
                    auto* job = new GateJob();
                    job->counter = cancel_counter;
                    job->release = release_flag;
                    return std::unique_ptr<
                        WindowsHttpFallbackAdapter::RunJob>(job);
                });
            std::atomic<bool> gate_entered{false};
            std::atomic<bool> gate_release{false};
            adapter.set_start_gate_for_test([&] {
                gate_entered.store(true);
                while (!gate_release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            bool ok = true;
            std::string err;
            std::thread runner([&] {
                ok = adapter.fallback()("http://127.0.0.1:9/x", "out", err);
            });
            while (!gate_entered.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            adapter.cancel();  // 落在发布边界之后（确定性）
            gate_release.store(true);   // 释放 start gate（已进入）
            release_flag->store(true);  // 释放 job execute
            runner.join();
            assert(!ok);
            assert(err.find("cancelled") != std::string::npos);
            assert(cancel_counter->load() == 1);
            adapter.set_job_factory_for_test(nullptr);
            adapter.set_start_gate_for_test(nullptr);
        }

        // (c) reset 清 latch 后正常运行。
        {
            WindowsHttpFallbackAdapter adapter;
            adapter.cancel();
            adapter.reset();
            adapter.set_job_factory_for_test(
                [](const std::string&, const std::string&) {
                    struct OkJob : WindowsHttpFallbackAdapter::RunJob {
                        bool execute(std::string&) override { return true; }
                        void request_cancel() override {}
                    };
                    return std::unique_ptr<WindowsHttpFallbackAdapter::RunJob>(
                        new OkJob());
                });
            std::string err;
            const bool ok = adapter.fallback()("http://127.0.0.1:9/x", "out", err);
            assert(ok);
            adapter.set_job_factory_for_test(nullptr);
        }
    }

    std::fprintf(stderr, "[TEST] 13 handshake\n");
    // 13. HandshakeJob start/cancel handshake 回归（RV-20260831-WIN-P2P-B1-01
    //     · execute 内窗口，平台中立可跑 macOS/Windows）。
    //     (a) execute 入口前已取消 → start_download 不被调用；
    //     (b) cancel 与启动重叠（start_download 期间到达）→ 发布临界区补发
    //         cancel_download（必达）→ 终态 cancelled 失败；
    //     (c) 下载中 cancel（发布后）→ cancel_download 直达 → 终态 cancelled。
    {
        using device_agent::WindowsHttpFallbackAdapter;
        struct ProbeJob : WindowsHttpFallbackAdapter::HandshakeJob {
            std::atomic<int> start_calls{0};
            std::atomic<int> cancel_calls{0};
            std::atomic<bool> started{false};
            std::atomic<bool> release_start{false};
            std::atomic<bool> wait_entered{false};
            std::atomic<bool> release_wait{false};

            void start_download() override {
                started.store(true);
                while (!release_start.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                ++start_calls;
            }
            void wait_download() override {
                wait_entered.store(true);
                while (!release_wait.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            void cancel_download() override { ++cancel_calls; }
        };

        // (a) 入口前取消：不启动。
        {
            ProbeJob job;
            std::string err;
            job.request_cancel();
            assert(!job.execute(err));
            assert(err.find("cancelled") != std::string::npos);
            assert(job.start_calls.load() == 0);
            assert(job.cancel_calls.load() == 0);
        }

        // (b) cancel 与启动重叠：发布后补发必达。
        {
            ProbeJob job;
            std::string err;
            std::thread runner([&] { job.execute(err); });
            while (!job.started.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            job.request_cancel();  // start_download 期间到达（未发布）
            job.release_start.store(true);  // 放行启动 → 发布临界区补发
            while (job.cancel_calls.load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            job.release_wait.store(true);
            runner.join();
            assert(job.start_calls.load() == 1);
            assert(job.cancel_calls.load() == 1);  // 发布后必达且仅一次
        }

        // (c) 下载中取消：request_cancel 直达。
        {
            ProbeJob job;
            std::string err;
            std::thread runner([&] { job.execute(err); });
            while (!job.started.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            job.release_start.store(true);  // 放行启动 → wait_download
            while (!job.wait_entered.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            job.request_cancel();
            while (job.cancel_calls.load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            job.release_wait.store(true);
            runner.join();
            assert(job.cancel_calls.load() == 1);
        }
    }

    std::fprintf(stderr, "[TEST] 14 s2 owner handoff end-to-end\n");
    // 14. B5-S2 端到端：注入已启动 owner（tracker+seeder 发现），A 完成即
    // handoff（owner 持 seed、外层 admission 释放）；B 随后走 P2P 完成而
    // 不是 `already active` → HTTP fallback（http.calls 不增）；外层
    // terminal-once；owner cap=2 时 C 淘汰最老 seed。
    {
        using namespace p2p_s2_test;
        auto cfg = std::make_shared<P2PConfigStore>();
        std::string cfg_err;
        assert_true(cfg->apply(R"({"p2p_enabled":true,"min_file_size_mb_for_p2p":0})", &cfg_err));
        P2PConfigStore::set_global(cfg);

        const std::string s2_dir = make_test_dir("s2-e2e");
        test_mkdir(s2_dir);
        LoopbackTrackerServer tracker;
        assert_true(tracker.start());

        std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
        assert_true(owner != nullptr);

        FakeHttpManager http;
        WindowsP2PDownloadManager manager(
            nullptr, {},
            std::shared_ptr<device_agent::IDownloadManager>(&http, [](device_agent::IDownloadManager*) {}),
            nullptr,
            owner);

        const char* names[3] = {"s2e_a.bin", "s2e_b.bin", "s2e_c.bin"};
        for (int i = 0; i < 3; ++i) {
            DownloadRequest req;
            req.torrent_url = make_tracked_torrent(s2_dir, names[i], 64 * 1024,
                                                   static_cast<unsigned char>(0x81 + i),
                                                   tracker.port());
            assert_true(!req.torrent_url.empty());
            req.dest_path = s2_dir + "/dl";
            test_mkdir(req.dest_path);
            req.file_size = 64 * 1024;
            auto seeder = make_bt_seeder(req.torrent_url, s2_dir);
            assert_true(seeder != nullptr);

            CompletionCapture capture;
            manager.download(req, nullptr,
                             [&](bool ok, const std::string& err,
                                 const device_agent::DownloadCompletionTelemetry& t) {
                                 capture.store(ok, err, &t);
                             });
            bool ok = false;
            std::string err;
            int completion_path = -1;
            assert_true(capture.wait_and_get(&ok, &err, &completion_path));
            assert(ok);
            // B/C 不得因 single-active 拒绝后落到 HTTP fallback。
            assert(http.calls.load() == 0);
            assert(completion_path == 1 /* P2P_PRIMARY */);
            assert_true(s2_wait_for([&] { return !manager.is_downloading(); },
                                    5000));
            assert_true(s2_wait_for([&] {
                const auto counters = owner->counters_for_test();
                if (counters.admitted != i + 1) return false;
                if (i == 2) {
                    return counters.active_seeds == 2 &&
                           counters.evicted_capacity == 1;
                }
                return counters.active_seeds == i + 1;
            }, 5000));
            seeder.reset();
        }
        tracker.stop();
        owner->Stop();
        P2PConfigStore::set_global(nullptr);
    }

    return 0;
}
