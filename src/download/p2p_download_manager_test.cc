#include "download/p2p_download_manager.h"
#include "download/p2p_s2_test_helpers.h"
#include "config/p2p_config_store.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr int kLibtorrentMaxUploadsUnlimited = 16777215;
constexpr int kLibtorrentUploadLimitUnlimited = -1;
constexpr int kDefaultMaxUploadPeers = 4;
constexpr int kLimitedMaxUploadPeers = 2;
constexpr int kConfiguredUploadLimitBytesPerSecond = 7 * 1024;
constexpr int kCellularSuppressedUploadLimitBytesPerSecond = 5120;
constexpr const char* kShaFixture = "device-agent sha256 fixture\n";
constexpr const char* kShaFixtureDigest =
    "a71202f43a7a48d735443b705a52c0acb30bb1084854969c9f557ba3f72d7a98";

std::string bencoded_string(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

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
    return tmp != nullptr ? std::string(tmp) : std::string("/data/local/tmp");
#endif
}

std::string make_test_dir() {
    std::string dir = test_temp_base() + "/p2p-download-manager-test-" +
                      std::to_string(test_pid());
    test_mkdir(dir);
    return dir;
}

bool file_readable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

// 断言只检查结果；调用本身必须始终执行（RV-20260831-WIN-P2P-A-02：
// 防 NDEBUG 下有副作用/出参的调用被整体裁掉，测试静默退化为空检查）。
// 本测试构建在 Release 下以 -UNDEBUG/UNDEBUG 保持断言开启；宏形式保留
// 调用点行号，便于失败定位。
#define assert_true(value) do { assert(value); } while (0)

// worker 回调 → 主线程的结果快照（RV-20260831-WIN-P2P-A-03：
// mutex + condition_variable 同步，不用普通 bool 跨线程轮询）。
struct CompletionCapture {
    void store(bool success,
               const std::string& error,
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

    // 等待完成并在锁内取结果快照；返回 false = 超时。
    bool wait_and_get(bool* ok_out,
                      std::string* err_out,
                      int* completion_path_out = nullptr,
                      int64_t* peer_bytes_out = nullptr) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool completed = cv_.wait_for(lock, std::chrono::seconds(5),
                                            [this] { return done_; });
        if (ok_out != nullptr) *ok_out = ok_;
        if (err_out != nullptr) *err_out = err_;
        if (completion_path_out != nullptr) *completion_path_out = completion_path_;
        if (peer_bytes_out != nullptr) *peer_bytes_out = peer_bytes_;
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

void write_seedless_torrent(const std::string& path) {
    const std::string announce = "http://127.0.0.1:9/announce";
    const std::string web_seed = "http://127.0.0.1:9/test.bin";
    const unsigned char piece_hash[] = {
        0x89, 0x72, 0x56, 0xb6, 0x70, 0x9e, 0x1a, 0x4d, 0xa9, 0xda,
        0xba, 0x92, 0xb6, 0xbd, 0xe3, 0x9c, 0xcf, 0xcc, 0xd8, 0xc1,
    };

    std::string data = "d";
    data += "8:announce" + bencoded_string(announce);
    data += "4:info";
    data += "d6:lengthi16384e";
    data += "4:name" + bencoded_string("test.bin");
    data += "12:piece lengthi16384e";
    data += "6:pieces20:";
    data.append(reinterpret_cast<const char*>(piece_hash), sizeof(piece_hash));
    data += "e";
    data += "8:url-list" + bencoded_string(web_seed);
    data += "e";

    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool wait_until_downloading(device_agent::P2PDownloadManager& manager) {
    for (int i = 0; i < 40; ++i) {
        if (manager.is_downloading()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool wait_until_active_handle(device_agent::P2PDownloadManager& manager) {
    for (int i = 0; i < 40; ++i) {
        if (!manager.active_max_uploads_for_test().empty()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool wait_until_upload_throttle(device_agent::P2PDownloadManager& manager,
                                int expected_max_uploads,
                                int expected_upload_limit) {
    for (int i = 0; i < 40; ++i) {
        const auto max_uploads = manager.active_max_uploads_for_test();
        const auto upload_limits = manager.active_upload_limits_for_test();
        if (!max_uploads.empty() && !upload_limits.empty() &&
                max_uploads.front() == expected_max_uploads &&
                upload_limits.front() == expected_upload_limit) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

namespace device_agent {
bool sha256_file_for_test(const std::string& path, std::string& out_hex, std::string& error);
}

namespace {
void capture_a_store(CompletionCapture& capture, bool ok, const std::string& err,
                     const device_agent::DownloadCompletionTelemetry& t) {
    capture.store(ok, err, &t);
}
}  // namespace

int main() {
    using namespace p2p_s2_test;
    using device_agent::DownloadRequest;
    using device_agent::NetworkType;
    using device_agent::P2PDownloadState;
    using device_agent::P2PDownloadManager;
    using device_agent::CompletionPathTelemetry;
    using device_agent::P2PConfigStore;
    using device_agent::P2PSeedingPolicy;
    using device_agent::P2PSeedingStateMachine;

    P2PSeedingStateMachine state_machine(
        P2PSeedingPolicy{std::chrono::seconds(1), 1.0});
    const auto start = std::chrono::steady_clock::now();
    assert(state_machine.state() == P2PDownloadState::Idle);
    state_machine.mark_downloading();
    assert(state_machine.state() == P2PDownloadState::Downloading);
    state_machine.mark_seeding(start);
    assert(state_machine.state() == P2PDownloadState::Seeding);
    assert(!state_machine.should_stop(start + std::chrono::milliseconds(500), 0.5));
    assert(state_machine.should_stop(start + std::chrono::milliseconds(500), 1.0));
    assert(state_machine.should_stop(start + std::chrono::seconds(1), 0.5));
    state_machine.mark_stopping();
    assert(state_machine.state() == P2PDownloadState::Stopping);
    state_machine.mark_idle();
    assert(state_machine.state() == P2PDownloadState::Idle);

#ifdef _WIN32
    assert(device_agent::dirname_or_current_for_test("C:\\a\\b.torrent") == "C:\\a");
    assert(device_agent::dirname_or_current_for_test("C:/a/b.torrent") == "C:/a");
    assert(device_agent::dirname_or_current_for_test("C:\\file.torrent") == "C:");
    assert(device_agent::dirname_or_current_for_test("bare.torrent") == ".");
    assert(device_agent::join_path_for_test("C:\\dest", "f.bin") == "C:\\dest\\f.bin");
    assert(device_agent::join_path_for_test("C:\\dest\\", "f.bin") == "C:\\dest\\f.bin");
    assert(device_agent::join_path_for_test("C:\\dest/", "f.bin") == "C:\\dest/f.bin");
    assert(device_agent::join_path_for_test("", "f.bin") == ".\\f.bin");
    assert(device_agent::join_path_for_test(".", "f.bin") == ".\\f.bin");
#else
    assert(device_agent::dirname_or_current_for_test("/a/b.torrent") == "/a");
    assert(device_agent::dirname_or_current_for_test("/file.torrent") == "/");
    assert(device_agent::dirname_or_current_for_test("bare.torrent") == ".");
    assert(device_agent::join_path_for_test("/dest", "f.bin") == "/dest/f.bin");
    assert(device_agent::join_path_for_test("/dest/", "f.bin") == "/dest/f.bin");
    assert(device_agent::join_path_for_test("", "f.bin") == "./f.bin");
    assert(device_agent::join_path_for_test(".", "f.bin") == "./f.bin");
#endif

    auto config_store = std::make_shared<P2PConfigStore>();
    std::string config_error;
    assert_true(config_store->apply(
        R"({"seeding_ttl_seconds":3,"max_share_ratio":1.25,"max_upload_kbps":7,"cellular_seeding_enabled":true})",
        &config_error));
    P2PConfigStore::set_global(config_store);
    const auto alpha_policy = P2PSeedingPolicy::alpha_defaults();
    assert(alpha_policy.ttl == std::chrono::seconds(3));
    assert(alpha_policy.ratio_limit == 1.25);
    assert(alpha_policy.max_upload_kbps == 7);
    assert(alpha_policy.cellular_seeding_enabled);
    assert(alpha_policy.p2p_enabled);
    assert(alpha_policy.seeding_enabled);
    assert(alpha_policy.max_upload_peers == kDefaultMaxUploadPeers);
    assert(alpha_policy.lan_upload_enabled);
    assert(!alpha_policy.wan_upload_enabled);
    assert(alpha_policy.cellular_download_enabled);
    assert(alpha_policy.min_file_size_mb_for_p2p == 10);
    P2PConfigStore::set_global(nullptr);

    assert(device_agent::completion_path_for_test(
        false, false, 4096, 0, 4096, false) == CompletionPathTelemetry::P2PPrimary);
    assert(device_agent::completion_path_for_test(
        false, false, 0, 0, 4096, false) == CompletionPathTelemetry::P2PPrimary);
    assert(device_agent::completion_path_for_test(
        false, false, 0, 0, 4096, true) == CompletionPathTelemetry::WebSeedPrimary);
    assert(device_agent::completion_path_for_test(
        false, false, 0, 0, 0, false) == CompletionPathTelemetry::Unspecified);
    assert(device_agent::completion_path_for_test(
        true, false, 4096, 0, 4096, false) == CompletionPathTelemetry::HttpFallbackStall);
    assert(device_agent::completion_path_for_test(
        false, true, 4096, 0, 4096, false) == CompletionPathTelemetry::HttpFallbackShaMismatch);

    P2PDownloadManager empty_url_manager;
    bool empty_complete = false;
    empty_url_manager.download(DownloadRequest{}, nullptr,
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            assert(!ok);
            assert(!err.empty());
            empty_complete = true;
        });
    assert(empty_complete);
    assert(!empty_url_manager.is_downloading());

    P2PDownloadManager invalid_magnet_manager;
    CompletionCapture invalid_magnet_capture;
    DownloadRequest invalid_magnet_req;
    invalid_magnet_req.magnet_uri = "magnet:?xt=urn:btih:invalid";
    invalid_magnet_manager.download(invalid_magnet_req, nullptr,
        [&invalid_magnet_capture](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry& telemetry) {
            invalid_magnet_capture.store(ok, err, &telemetry);
        });
    bool invalid_magnet_ok = true;
    std::string invalid_magnet_err;
    assert_true(invalid_magnet_capture.wait_and_get(&invalid_magnet_ok, &invalid_magnet_err));
    assert(!invalid_magnet_ok);
    assert(invalid_magnet_err.find("failed to parse magnet URI") != std::string::npos);
    assert(!invalid_magnet_manager.is_downloading());

    P2PDownloadManager url_torrent_fallback_manager;
    bool url_torrent_fallback_complete = false;
    DownloadRequest url_torrent_fallback_req;
    url_torrent_fallback_req.url = "/tmp/legacy-url-source.torrent";
    url_torrent_fallback_manager.download(url_torrent_fallback_req, nullptr,
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            assert(!ok);
            assert(err.find("expected magnet_uri or torrent_url") != std::string::npos);
            assert(err.find("fallback") == std::string::npos);
            url_torrent_fallback_complete = true;
        });
    assert(url_torrent_fallback_complete);
    assert(!url_torrent_fallback_manager.is_downloading());

    P2PDownloadManager network_change_manager;
    network_change_manager.on_network_changed(NetworkType::CELLULAR);
    network_change_manager.on_network_changed(NetworkType::WIFI);
    network_change_manager.on_network_changed(NetworkType::NONE);
    assert(!network_change_manager.is_downloading());

    const std::string dir = make_test_dir();
    const std::string torrent_path = dir + "/fixture.torrent";
    const std::string save_dir = dir + "/save";
    test_mkdir(save_dir);
    write_seedless_torrent(torrent_path);

    const std::string sha_path = dir + "/sha-fixture.bin";
    {
        std::ofstream out(sha_path, std::ios::binary);
        out << kShaFixture;
    }
    std::string sha_hex;
    std::string sha_error;
    assert_true(device_agent::sha256_file_for_test(sha_path, sha_hex, sha_error));
    assert(sha_hex == kShaFixtureDigest);

    std::string missing_hex;
    std::string missing_error;
    assert_true(!device_agent::sha256_file_for_test(dir + "/missing-for-sha-test.bin",
                                                    missing_hex, missing_error));
    assert(!missing_error.empty());

    // magnet_uri 优先于 torrent_url：无效 magnet + 合法 torrent fixture，
    // 必须报 magnet 解析错误而不是加载 torrent。
    {
        P2PDownloadManager magnet_priority_manager;
        CompletionCapture capture;
        DownloadRequest magnet_priority_req;
        magnet_priority_req.magnet_uri = "magnet:?xt=urn:btih:invalid";
        magnet_priority_req.torrent_url = torrent_path;
        magnet_priority_manager.download(magnet_priority_req, nullptr,
            [&capture](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry& telemetry) {
                capture.store(ok, err, &telemetry);
            });
        bool magnet_priority_ok = true;
        std::string magnet_priority_err;
        assert_true(capture.wait_and_get(&magnet_priority_ok, &magnet_priority_err));
        assert(!magnet_priority_ok);
        assert(magnet_priority_err.find("failed to parse magnet URI") != std::string::npos);
        assert(!magnet_priority_manager.is_downloading());
    }

    // 注入 fallback：无 P2P source、仅 url 的直接 HTTP 下载端到端成功，
    // completion_path = HttpFallbackStall，peer_bytes = 0。
    {
        P2PDownloadManager direct_fallback_manager;
        direct_fallback_manager.set_http_fallback_for_test(
            [](const std::string&, const std::string& output_path, std::string&) {
                std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
                out << kShaFixture;
                return true;
            });
        DownloadRequest direct_req;
        direct_req.url = "http://127.0.0.1:9/direct-fallback.bin";
        direct_req.dest_path = save_dir;
        direct_req.expected_sha256 = kShaFixtureDigest;
        CompletionCapture capture;
        direct_fallback_manager.download(direct_req, nullptr,
            [&capture](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry& telemetry) {
                capture.store(ok, err, &telemetry);
            });
        bool direct_ok = false;
        std::string direct_err;
        int direct_completion_path = -1;
        int64_t direct_peer_bytes = -1;
        assert_true(capture.wait_and_get(&direct_ok, &direct_err,
                                         &direct_completion_path, &direct_peer_bytes));
        assert(direct_ok);
        assert(direct_err.empty());
        assert(direct_completion_path ==
               static_cast<int>(CompletionPathTelemetry::HttpFallbackStall));
        assert(direct_peer_bytes == 0);
        assert(file_readable(save_dir + "/direct-fallback.bin"));
    }

    // 注入 fallback + 错误 SHA256：下载成功但校验必须失败（fail-closed）。
    {
        P2PDownloadManager sha_mismatch_manager;
        sha_mismatch_manager.set_http_fallback_for_test(
            [](const std::string&, const std::string& output_path, std::string&) {
                std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
                out << kShaFixture;
                return true;
            });
        DownloadRequest sha_req;
        sha_req.url = "http://127.0.0.1:9/sha-mismatch.bin";
        sha_req.dest_path = save_dir;
        sha_req.expected_sha256 = std::string(64, '0');
        CompletionCapture capture;
        sha_mismatch_manager.download(sha_req, nullptr,
            [&capture](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry& telemetry) {
                capture.store(ok, err, &telemetry);
            });
        bool sha_ok = true;
        std::string sha_verify_error;
        assert_true(capture.wait_and_get(&sha_ok, &sha_verify_error));
        assert(!sha_ok);
        assert(sha_verify_error.find("sha256 mismatch") != std::string::npos);
        assert(!sha_mismatch_manager.is_downloading());
    }

    // torrent_url 指向 HTTP：注入 fallback 下载 .torrent 元数据（写入经
    // join_path 的 Windows/POSIX 路径），随后 torrent 加载 + cancel 生命周期。
    // fallback 完成 flag 用 release/acquire 原子变量同步，主线程不再轮询文件。
    {
        P2PDownloadManager metadata_manager;
        std::atomic<bool> metadata_fallback_done{false};
        metadata_manager.set_http_fallback_for_test(
            [&torrent_path, &metadata_fallback_done](const std::string&, const std::string& output_path, std::string&) {
                std::ifstream in(torrent_path, std::ios::binary);
                std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
                out << in.rdbuf();
                const bool ok = static_cast<bool>(out);
                metadata_fallback_done.store(true, std::memory_order_release);
                return ok;
            });
        DownloadRequest metadata_req;
        metadata_req.torrent_url = "http://127.0.0.1:9/fixture.torrent";
        metadata_req.dest_path = save_dir;
        metadata_req.file_id = "metadata-probe";
        metadata_req.file_size = 64 * 1024 * 1024;
        metadata_manager.download(metadata_req, nullptr, nullptr);
        assert_true(wait_until_downloading(metadata_manager));
        const std::string metadata_path = save_dir + "/metadata-probe.torrent";
        bool metadata_written = false;
        for (int i = 0; i < 40 && !metadata_written; ++i) {
            metadata_written = metadata_fallback_done.load(std::memory_order_acquire);
            if (!metadata_written) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        assert_true(metadata_written);
        assert(file_readable(metadata_path));
        metadata_manager.cancel();
        assert(!metadata_manager.is_downloading());
        assert(metadata_manager.state() == P2PDownloadState::Idle);
    }

    auto network_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 7, false},
        network_policy);
    // 订阅后再发事件：manager 的 network_type_ 才为 WIFI，初始 throttle 依据它。
    network_policy->on_network_changed(NetworkType::WIFI);
    std::mutex mu;
    std::condition_variable cv;
    bool complete_called = false;
    bool complete_ok = true;
    std::string complete_error;
    int progress_calls = 0;

    DownloadRequest req;
    req.torrent_url = torrent_path;
    req.url = "http://127.0.0.1:9/test.bin";
    req.dest_path = save_dir;
    req.file_size = 64 * 1024 * 1024;

    manager.download(req,
        [&](const device_agent::DownloadProgress&) {
            std::lock_guard<std::mutex> lock(mu);
            ++progress_calls;
        },
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            std::lock_guard<std::mutex> lock(mu);
            complete_called = true;
            complete_ok = ok;
            complete_error = err;
            cv.notify_one();
        });

    assert_true(wait_until_downloading(manager));
    assert_true(wait_until_active_handle(manager));
    assert_true(wait_until_upload_throttle(
        manager, kDefaultMaxUploadPeers, kConfiguredUploadLimitBytesPerSecond));
    assert(manager.state() == P2PDownloadState::Downloading);
    network_policy->on_network_changed(NetworkType::CELLULAR);
    assert_true(wait_until_upload_throttle(
        manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
    network_policy->on_network_changed(NetworkType::WIFI);
    assert_true(wait_until_upload_throttle(
        manager, kDefaultMaxUploadPeers, kConfiguredUploadLimitBytesPerSecond));
    manager.cancel();
    assert(!manager.is_downloading());
    assert(manager.state() == P2PDownloadState::Idle);

    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(2), [&] { return complete_called; });
        assert(complete_called);
        assert(!complete_ok);
        assert(complete_error.find("cancel") != std::string::npos);
        assert(progress_calls >= 0);
    }

    auto unlimited_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager unlimited_manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 0, true},
        unlimited_policy);
    unlimited_policy->on_network_changed(NetworkType::WIFI);
    DownloadRequest unlimited_req;
    unlimited_req.torrent_url = torrent_path;
    unlimited_req.dest_path = save_dir;
    unlimited_req.file_size = 64 * 1024 * 1024;
    unlimited_manager.download(unlimited_req, nullptr, nullptr);
    assert_true(wait_until_downloading(unlimited_manager));
    assert_true(wait_until_upload_throttle(
        unlimited_manager, kDefaultMaxUploadPeers, kLibtorrentUploadLimitUnlimited));
    unlimited_manager.cancel();
    assert(!unlimited_manager.is_downloading());

    auto default_cellular_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager default_cellular_manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 0, false},
        default_cellular_policy);
    default_cellular_policy->on_network_changed(NetworkType::WIFI);
    DownloadRequest default_cellular_req;
    default_cellular_req.torrent_url = torrent_path;
    default_cellular_req.dest_path = save_dir;
    default_cellular_req.file_size = 64 * 1024 * 1024;
    default_cellular_manager.download(default_cellular_req, nullptr, nullptr);
    assert_true(wait_until_downloading(default_cellular_manager));
    // 先等 torrent handle 就绪再发网络事件，保证节流应用在活跃 handle 上。
    assert_true(wait_until_active_handle(default_cellular_manager));
    default_cellular_policy->on_network_changed(NetworkType::CELLULAR);
    assert_true(wait_until_upload_throttle(
        default_cellular_manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
    default_cellular_manager.cancel();
    assert(!default_cellular_manager.is_downloading());

    auto hot_config = std::make_shared<P2PConfigStore>();
    assert_true(hot_config->apply(
        R"({"seeding_ttl_seconds":3,"max_share_ratio":1.25,"max_upload_kbps":7,"max_upload_peers":2})",
        &config_error));
    P2PConfigStore::set_global(hot_config);
    auto hot_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager hot_manager({}, P2PSeedingPolicy::alpha_defaults(), hot_policy);
    hot_policy->on_network_changed(NetworkType::WIFI);
    DownloadRequest hot_req;
    hot_req.torrent_url = torrent_path;
    hot_req.dest_path = save_dir;
    hot_req.file_size = 64 * 1024 * 1024;
    hot_manager.download(hot_req, nullptr, nullptr);
    assert_true(wait_until_downloading(hot_manager));
    assert_true(wait_until_upload_throttle(
        hot_manager, kLimitedMaxUploadPeers, kConfiguredUploadLimitBytesPerSecond));
    hot_manager.cancel();
    assert(!hot_manager.is_downloading());
    P2PConfigStore::set_global(nullptr);

    auto disabled_config = std::make_shared<P2PConfigStore>();
    assert_true(disabled_config->apply(R"({"p2p_enabled":false})", &config_error));
    P2PConfigStore::set_global(disabled_config);
    P2PDownloadManager disabled_manager;
    bool disabled_complete = false;
    DownloadRequest disabled_req;
    disabled_req.torrent_url = torrent_path;
    disabled_req.file_size = 64 * 1024 * 1024;
    disabled_manager.download(disabled_req, nullptr,
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            assert(!ok);
            assert(err.find("p2p disabled") != std::string::npos);
            disabled_complete = true;
        });
    assert(disabled_complete);
    assert(!disabled_manager.is_downloading());
    P2PConfigStore::set_global(nullptr);

    auto min_size_config = std::make_shared<P2PConfigStore>();
    assert_true(min_size_config->apply(R"({"min_file_size_mb_for_p2p":64})", &config_error));
    P2PConfigStore::set_global(min_size_config);
    P2PDownloadManager min_size_manager;
    bool min_size_complete = false;
    DownloadRequest min_size_req;
    min_size_req.torrent_url = torrent_path;
    min_size_req.file_size = 16 * 1024 * 1024;
    min_size_manager.download(min_size_req, nullptr,
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            assert(!ok);
            assert(err.find("min_file_size_mb_for_p2p") != std::string::npos);
            min_size_complete = true;
        });
    assert(min_size_complete);
    assert(!min_size_manager.is_downloading());
    P2PConfigStore::set_global(nullptr);

    auto cellular_download_config = std::make_shared<P2PConfigStore>();
    assert_true(cellular_download_config->apply(
        R"({"cellular_download_enabled":false,"min_file_size_mb_for_p2p":0})",
        &config_error));
    P2PConfigStore::set_global(cellular_download_config);
    // 先订阅、后切网：manager 只有作为 listener 收到 CELLULAR 事件后
    // network_type_ 才为 CELLULAR（断言开启时暴露的既有订阅时序缺陷）。
    auto cellular_download_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager cellular_download_manager(
        {}, P2PSeedingPolicy::alpha_defaults(), cellular_download_policy);
    cellular_download_policy->on_network_changed(NetworkType::CELLULAR);
    bool cellular_download_complete = false;
    DownloadRequest cellular_download_req;
    cellular_download_req.torrent_url = torrent_path;
    cellular_download_req.file_size = 64 * 1024 * 1024;
    cellular_download_manager.download(cellular_download_req, nullptr,
        [&](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry&) {
            assert(!ok);
            assert(err.find("cellular download disabled") != std::string::npos);
            cellular_download_complete = true;
        });
    assert(cellular_download_complete);
    assert(!cellular_download_manager.is_downloading());
    P2PConfigStore::set_global(nullptr);

    auto no_seed_config = std::make_shared<P2PConfigStore>();
    assert_true(no_seed_config->apply(
        R"({"seeding_enabled":false,"min_file_size_mb_for_p2p":0})",
        &config_error));
    P2PConfigStore::set_global(no_seed_config);
    auto no_seed_policy = std::make_shared<device_agent::NetworkPolicy>();
    P2PDownloadManager no_seed_manager(
        {},
        P2PSeedingPolicy::alpha_defaults(),
        no_seed_policy);
    no_seed_policy->on_network_changed(NetworkType::WIFI);
    DownloadRequest no_seed_req;
    no_seed_req.torrent_url = torrent_path;
    no_seed_req.dest_path = save_dir;
    no_seed_req.file_size = 64 * 1024 * 1024;
    no_seed_manager.download(no_seed_req, nullptr, nullptr);
    assert_true(wait_until_downloading(no_seed_manager));
    // global off：初始 throttle 必须直接生效（RV-04 统一判定后无需重发网络事件）。
    assert_true(wait_until_upload_throttle(
        no_seed_manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
    no_seed_manager.cancel();
    assert(!no_seed_manager.is_downloading());
    P2PConfigStore::set_global(nullptr);

    // LAN off：lan_upload_enabled=false 时 WIFI 初始 throttle 必须抑制。
    {
        auto lan_off_config = std::make_shared<P2PConfigStore>();
        assert_true(lan_off_config->apply(
            R"({"lan_upload_enabled":false,"min_file_size_mb_for_p2p":0})",
            &config_error));
        P2PConfigStore::set_global(lan_off_config);
        auto lan_off_policy = std::make_shared<device_agent::NetworkPolicy>();
        P2PDownloadManager lan_off_manager(
            {}, P2PSeedingPolicy::alpha_defaults(), lan_off_policy);
        lan_off_policy->on_network_changed(NetworkType::WIFI);
        DownloadRequest lan_off_req;
        lan_off_req.torrent_url = torrent_path;
        lan_off_req.dest_path = save_dir;
        lan_off_req.file_size = 64 * 1024 * 1024;
        lan_off_manager.download(lan_off_req, nullptr, nullptr);
        assert_true(wait_until_downloading(lan_off_manager));
        assert_true(wait_until_upload_throttle(
            lan_off_manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
        lan_off_manager.cancel();
        assert(!lan_off_manager.is_downloading());
        P2PConfigStore::set_global(nullptr);
    }

    // cellular global-off + cellular-on：总开关关闭时 cellular 标志
    // 不得在初始 throttle 重新放行上传。
    {
        auto cellular_gated_config = std::make_shared<P2PConfigStore>();
        assert_true(cellular_gated_config->apply(
            R"({"seeding_enabled":false,"cellular_seeding_enabled":true,"min_file_size_mb_for_p2p":0})",
            &config_error));
        P2PConfigStore::set_global(cellular_gated_config);
        auto cellular_gated_policy = std::make_shared<device_agent::NetworkPolicy>();
        P2PDownloadManager cellular_gated_manager(
            {}, P2PSeedingPolicy::alpha_defaults(), cellular_gated_policy);
        cellular_gated_policy->on_network_changed(NetworkType::CELLULAR);
        DownloadRequest cellular_gated_req;
        cellular_gated_req.torrent_url = torrent_path;
        cellular_gated_req.dest_path = save_dir;
        cellular_gated_req.file_size = 64 * 1024 * 1024;
        cellular_gated_manager.download(cellular_gated_req, nullptr, nullptr);
        assert_true(wait_until_downloading(cellular_gated_manager));
        assert_true(wait_until_upload_throttle(
            cellular_gated_manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
        cellular_gated_manager.cancel();
        assert(!cellular_gated_manager.is_downloading());
        P2PConfigStore::set_global(nullptr);
    }

    // cellular on + 全局开：CELLULAR 下初始 throttle 允许做种（默认上限）。
    {
        auto cellular_on_config = std::make_shared<P2PConfigStore>();
        assert_true(cellular_on_config->apply(
            R"({"cellular_seeding_enabled":true,"min_file_size_mb_for_p2p":0})",
            &config_error));
        P2PConfigStore::set_global(cellular_on_config);
        auto cellular_on_policy = std::make_shared<device_agent::NetworkPolicy>();
        P2PDownloadManager cellular_on_manager(
            {}, P2PSeedingPolicy::alpha_defaults(), cellular_on_policy);
        cellular_on_policy->on_network_changed(NetworkType::CELLULAR);
        DownloadRequest cellular_on_req;
        cellular_on_req.torrent_url = torrent_path;
        cellular_on_req.dest_path = save_dir;
        cellular_on_req.file_size = 64 * 1024 * 1024;
        cellular_on_manager.download(cellular_on_req, nullptr, nullptr);
        assert_true(wait_until_downloading(cellular_on_manager));
        assert_true(wait_until_upload_throttle(
            cellular_on_manager, kDefaultMaxUploadPeers, kLibtorrentUploadLimitUnlimited));
        cellular_on_manager.cancel();
        assert(!cellular_on_manager.is_downloading());
        P2PConfigStore::set_global(nullptr);
    }

    // fresh 实例状态隔离：前序用例的 global config / 会话状态不泄漏到新实例，
    // alpha_defaults 回到出厂默认（fresh process/test instance 语义）。
    {
        assert(!P2PConfigStore::has_global());
        const char* ttl_env = std::getenv("P2P_SEEDING_TTL");
        const bool ttl_env_absent = ttl_env == nullptr || *ttl_env == '\0';
        const auto fresh_defaults = P2PSeedingPolicy::alpha_defaults();
        if (ttl_env_absent) {
            assert(fresh_defaults.ttl == std::chrono::seconds(21600));
        }
        assert(fresh_defaults.ratio_limit == 1.0);
        assert(fresh_defaults.max_upload_kbps == 0);
        assert(!fresh_defaults.cellular_seeding_enabled);
        assert(fresh_defaults.p2p_enabled);
        assert(fresh_defaults.seeding_enabled);
        assert(fresh_defaults.min_file_size_mb_for_p2p == 10);

        P2PDownloadManager fresh_manager;
        assert(!fresh_manager.is_downloading());
        assert(fresh_manager.state() == P2PDownloadState::Idle);
        DownloadRequest empty_probe{};
        CompletionCapture capture;
        fresh_manager.download(empty_probe, nullptr,
            [&capture](bool ok, const std::string& err, const device_agent::DownloadCompletionTelemetry& telemetry) {
                capture.store(ok, err, &telemetry);
            });
        bool probe_ok = true;
        std::string probe_err;
        assert_true(capture.wait_and_get(&probe_ok, &probe_err));
        assert(!probe_ok);
        assert(probe_err.find("no p2p download source") != std::string::npos);
        assert(!fresh_manager.is_downloading());
    }

    // ==================== B5-S2 owner handoff 确定性回归 ====================
#ifdef _WIN32
    static S2WinSockInit s2_wsa;
    assert(s2_wsa.ok);
#endif
        std::fprintf(stderr, "[TEST] S2-1 handoff releases admission (A/B/C)\n");
        // A/B/C 顺序下载：A 完成即 handoff（owner 持 seed、admission 释放）、
        // B/C 走真实 BT peer 完成而非 already active→HTTP；terminal 恰一次；
        // cap=2 时 C 淘汰最老 seed；telemetry 不变。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);

            const char* names[3] = {"s2_a.bin", "s2_b.bin", "s2_c.bin"};
            const unsigned char fills[3] = {0xA1, 0xB2, 0xC3};
            for (int i = 0; i < 3; ++i) {
                device_agent::DownloadRequest req;
                req.torrent_url = make_plain_torrent(dir, names[i], 64 * 1024,
                                                     fills[i]);
                assert_true(!req.torrent_url.empty());
                req.dest_path = dir + "/dl";
                test_mkdir(req.dest_path);
                req.file_size = 64 * 1024;

                std::atomic<int> completion_count{0};
                CompletionCapture capture;
                auto seeder = make_bt_seeder(req.torrent_url, dir);
                assert_true(seeder != nullptr);
                assert_true(s2_wait_for([&] {
                    return seeder->listen_port() > 0;
                }, 5000));
                manager.set_test_peer_endpoints_for_test(
                    {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                       static_cast<std::uint16_t>(
                                           seeder->listen_port()))});
                manager.download(req, nullptr,
                    [&](bool ok, const std::string& err,
                        const device_agent::DownloadCompletionTelemetry& t) {
                        completion_count.fetch_add(1);
                        capture.store(ok, err, &t);
                    });
                bool ok = false;
                std::string err;
                int completion_path = -1;
                assert_true(capture.wait_and_get(&ok, &err, &completion_path));
                assert_true(ok);
                assert_true(err.find("already active") == std::string::npos);
                // terminal-once：handoff 之后不得出现第二次 completion（D2/D7）
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                assert_true(completion_count.load() == 1);
                // release telemetry 在 handoff 后不变：真实 BT peer 传输，
                // completion_path 仍为 P2P_PRIMARY。
                assert_true(completion_path ==
                            static_cast<int>(device_agent::CompletionPathTelemetry::P2PPrimary));
                // admission 释放：completion 后 worker 立即退出（不进 inline loop）
                assert_true(s2_wait_for([&] { return !manager.is_downloading(); },
                                        5000));
                // owner 接纳 seed；B/C 之后 A 仍在（FIFO 淘汰只发生在满额）
                assert_true(s2_wait_for([&] {
                    const auto counters = owner->counters_for_test();
                    if (counters.admitted != i + 1) return false;
                    if (i == 2) {
                        return counters.active_seeds == 2 &&
                               counters.evicted_capacity == 1;
                    }
                    return counters.active_seeds == i + 1;
                }, 5000));
                seeder.reset();  // 本轮 seeder 退出，不干扰下一轮
            }
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] S2-2 cancel keeps seeds, no fallback\n");
        // A 完成并 handoff 后，B 下载中 cancel：A seed 保留、fallback 零触发、
        // B terminal 恰一次。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            int fallback_calls = 0;
            device_agent::P2PDownloadManager manager(
                {}, s2_manager_policy(), nullptr,
                [&fallback_calls](const std::string&, const std::string&,
                                  std::string&) {
                    ++fallback_calls;
                    return false;
                },
                owner);

            device_agent::DownloadRequest req_a;
            req_a.torrent_url = make_plain_torrent(dir, "s2c_a.bin", 64 * 1024,
                                                   0x71);
            assert_true(!req_a.torrent_url.empty());
            req_a.dest_path = dir + "/dl";
            test_mkdir(req_a.dest_path);
            req_a.file_size = 64 * 1024;
            auto seeder_a = make_bt_seeder(req_a.torrent_url, dir);
            assert_true(seeder_a != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder_a->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder_a->listen_port()))});
            CompletionCapture cap_a;
            manager.download(req_a, nullptr,
                [&cap_a](bool ok, const std::string& err,
                         const device_agent::DownloadCompletionTelemetry& t) {
                    cap_a.store(ok, err, &t);
                });
            bool ok_a = false;
            std::string err_a;
            assert_true(cap_a.wait_and_get(&ok_a, &err_a));
            assert_true(ok_a);
            assert_true(s2_wait_for([&] {
                return owner->counters_for_test().active_seeds == 1;
            }, 5000));

            // B：大文件，等进入下载后 cancel。
            device_agent::DownloadRequest req_b;
            req_b.torrent_url = make_plain_torrent(dir, "s2c_b.bin",
                                                   4 * 1024 * 1024, 0x72);
            assert_true(!req_b.torrent_url.empty());
            req_b.dest_path = dir + "/dl";
            req_b.file_size = 4 * 1024 * 1024;
            CompletionCapture cap_b;
            manager.download(req_b, nullptr,
                [&cap_b](bool ok, const std::string& err,
                         const device_agent::DownloadCompletionTelemetry& t) {
                    cap_b.store(ok, err, &t);
                });
            assert_true(s2_wait_for([&] { return manager.is_downloading(); }, 5000));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            manager.cancel();
            assert_true(s2_wait_for([&] { return !manager.is_downloading(); }, 5000));

            // terminal 恰好一次（成功或取消失败均计一次），fallback 零触发。
            bool ok_b = false;
            std::string err_b;
            assert_true(cap_b.wait_and_get(&ok_b, &err_b));
            assert_true(fallback_calls == 0);
            // A 的 seed 不受 B cancel 影响（owner 独立 epoch，D7）。
            assert_true(s2_wait_for([&] {
                const auto counters = owner->counters_for_test();
                return counters.active_seeds >= 1 &&
                       counters.evicted_capacity == 0;
            }, 5000));
            seeder_a.reset();
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] S2-3 stale generation blocks handoff\n");
        // B5-05 gate B（cancel 赢）：handoff commit 窗口内 cancel —— commit
        // 判 stale 丢弃，admitted/active 均为 0；completion 不受影响。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);

            std::mutex gate_mu;
            std::condition_variable gate_cv;
            std::atomic<bool> gate_entered{false};
            std::atomic<bool> gate_release{false};
            manager.set_handoff_gate_for_test([&] {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_entered = true;
                gate_cv.notify_all();
                gate_cv.wait_for(lock, std::chrono::seconds(30),
                                 [&] { return gate_release.load(); });
            });

            device_agent::DownloadRequest req;
            req.torrent_url = make_plain_torrent(dir, "s2g_a.bin", 64 * 1024,
                                                 0x7D);
            assert_true(!req.torrent_url.empty());
            req.dest_path = dir + "/dl";
            test_mkdir(req.dest_path);
            req.file_size = 64 * 1024;
            CompletionCapture capture;
            auto seeder = make_bt_seeder(req.torrent_url, dir);
            assert_true(seeder != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder->listen_port()))});
            manager.download(req, nullptr,
                [&capture](bool ok, const std::string& err,
                           const device_agent::DownloadCompletionTelemetry& t) {
                    capture.store(ok, err, &t);
                });

            {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return gate_entered.load(); });
            }
            assert_true(gate_entered.load());

            std::atomic<bool> cancel_started{false};
            std::thread canceller([&] {
                cancel_started.store(true);
                manager.cancel();  // 失效水位与 handoff commit 同锁互斥
            });
            for (int i = 0; i < 100 && !cancel_started.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert_true(cancel_started.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                std::lock_guard<std::mutex> lock(gate_mu);
                gate_release = true;
                gate_cv.notify_all();
            }
            canceller.join();

            bool ok = false;
            std::string err;
            assert_true(capture.wait_and_get(&ok, &err));
            assert_true(ok);  // release completion 恰一次且成功（不反转）
            assert_true(owner->counters_for_test().admitted == 0);  // stale 拦截
            assert_true(owner->counters_for_test().active_seeds == 0);
            assert_true(!manager.is_downloading());
            seeder.reset();
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] G1a admission window: cancel wins\n");
        // B5-05 gate A（cancel 赢）：ticket 已分配、worker 未发布（mu_ 在手）
        // 时 cancel —— cancel 阻塞到临界区结束，随后 victim 被 join、请求被
        // 失效终止；不存在「cancel 先返回、请求随后继续」。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);

            std::mutex gate_mu;
            std::condition_variable gate_cv;
            std::atomic<bool> gate_entered{false};
            std::atomic<bool> gate_release{false};
            manager.set_admission_gate_for_test([&] {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_entered = true;
                gate_cv.notify_all();
                // admission gate 在 main 线程的 download() 内执行（持 mu_）；
                // release 由 watcher 线程在负向断言后置位。
                gate_cv.wait_for(lock, std::chrono::seconds(30),
                                 [&] { return gate_release.load(); });
            });

            device_agent::DownloadRequest req;
            req.torrent_url = make_plain_torrent(dir, "g1a.bin", 64 * 1024,
                                                 0x91);
            assert_true(!req.torrent_url.empty());
            req.dest_path = dir + "/dl";
            test_mkdir(req.dest_path);
            req.file_size = 64 * 1024;

            // canceller 与 watcher 必须先于 download() 启动：admission gate
            // 在 main 线程内阻塞，download() 在放行前不会返回。
            std::atomic<bool> cancel_started{false};
            std::atomic<bool> cancel_done{false};
            std::atomic<bool> blocked_confirmed{false};
            std::thread canceller([&] {
                {
                    std::unique_lock<std::mutex> lock(gate_mu);
                    gate_cv.wait_for(lock, std::chrono::seconds(10),
                                     [&] { return gate_entered.load(); });
                }
                cancel_started.store(true);
                manager.cancel();
                cancel_done.store(true);
            });
            std::thread watcher([&] {
                for (int i = 0; i < 100 && !cancel_started.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                // 负向确定性：窗口未放行时 cancel 不得返回。
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                blocked_confirmed.store(!cancel_done.load());
                {
                    std::lock_guard<std::mutex> lock(gate_mu);
                    gate_release = true;
                    gate_cv.notify_all();
                }
            });

            CompletionCapture capture;
            manager.download(req, nullptr,
                [&capture](bool ok, const std::string& err,
                           const device_agent::DownloadCompletionTelemetry& t) {
                    capture.store(ok, err, &t);
                });
            assert_true(gate_entered.load());  // ticket 已分配、worker 未发布

            canceller.join();
            watcher.join();
            assert_true(cancel_started.load());
            assert_true(blocked_confirmed.load());  // 窗口内 cancel 未返回
            assert_true(cancel_done.load());        // 放行后 cancel 正常返回
            assert_true(s2_wait_for([&] { return !manager.is_downloading(); }, 5000));

            // 请求被失效终止：completion 恰一次（失败终态），owner 零接纳。
            bool ok = false;
            std::string err;
            assert_true(capture.wait_and_get(&ok, &err));
            assert_true(!ok);
            assert_true(owner->counters_for_test().admitted == 0);
            assert_true(owner->counters_for_test().active_seeds == 0);
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] G1b admission window: publication wins\n");
        // B5-05 gate A（publication 赢）：窗口立即放行 → 请求正常完成并
        // handoff；此后 cancel 为无害 no-op，seed 保留（D7）。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);
            std::atomic<int> gate_entries{0};
            manager.set_admission_gate_for_test([&] { gate_entries.fetch_add(1); });

            device_agent::DownloadRequest req;
            req.torrent_url = make_plain_torrent(dir, "g1b.bin", 64 * 1024,
                                                 0x92);
            assert_true(!req.torrent_url.empty());
            req.dest_path = dir + "/dl";
            test_mkdir(req.dest_path);
            req.file_size = 64 * 1024;
            auto seeder = make_bt_seeder(req.torrent_url, dir);
            assert_true(seeder != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder->listen_port()))});
            CompletionCapture capture;
            manager.download(req, nullptr,
                [&capture](bool ok, const std::string& err,
                           const device_agent::DownloadCompletionTelemetry& t) {
                    capture.store(ok, err, &t);
                });
            bool ok = false;
            std::string err;
            assert_true(capture.wait_and_get(&ok, &err));
            assert_true(ok);
            assert_true(s2_wait_for([&] {
                return owner->counters_for_test().active_seeds == 1;
            }, 5000));

            manager.cancel();  // 完成后的 cancel：无 in-flight 请求，不得影响 seed
            assert_true(!manager.is_downloading());
            assert_true(owner->counters_for_test().active_seeds == 1);
            assert_true(gate_entries.load() == 1);
            seeder.reset();
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] G2b handoff commit window: commit wins\n");
        // B5-05 gate B（commit 赢）：candidate 已构造、commit 窗口立即放行
        // → Admit 的线性化明确早于随后的 cancel —— seed 保留、completion
        // 不反转。（G2a「cancel 赢」= 既有 S2-3。）
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);
            manager.set_handoff_gate_for_test([] {});  // 立即放行：commit 先行

            device_agent::DownloadRequest req;
            req.torrent_url = make_plain_torrent(dir, "g2b.bin", 64 * 1024,
                                                 0x93);
            assert_true(!req.torrent_url.empty());
            req.dest_path = dir + "/dl";
            test_mkdir(req.dest_path);
            req.file_size = 64 * 1024;
            auto seeder = make_bt_seeder(req.torrent_url, dir);
            assert_true(seeder != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder->listen_port()))});
            CompletionCapture capture;
            manager.download(req, nullptr,
                [&capture](bool ok, const std::string& err,
                           const device_agent::DownloadCompletionTelemetry& t) {
                    capture.store(ok, err, &t);
                });
            bool ok = false;
            std::string err;
            assert_true(capture.wait_and_get(&ok, &err));
            assert_true(ok);
            assert_true(s2_wait_for([&] {
                return owner->counters_for_test().active_seeds == 1;
            }, 5000));

            // commit（Admit）已线性化完成；随后的 cancel 不得删除该历史 seed。
            manager.cancel();
            assert_true(!manager.is_downloading());
            assert_true(owner->counters_for_test().active_seeds == 1);
            assert_true(owner->counters_for_test().evicted_capacity == 0);
            seeder.reset();
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] G3 drain window: new admission and second cancel wait\n");
        // B5-05 drain 窗口（二次 verify 残留竞争，确定性双向）：exit gate
        // 位于 worker 已写 downloading_=false、但 idle/线程退出未完成的
        // joinable 收尾区间——新 download 必须仍被 drain 拒绝（此时
        // downloading_ 已为 false，拒绝只能来自 drain 检查），第二个
        // cancel 确定进入等待且放行前不返回；放行后收尾顺序完成，新代
        // 准入的 downloading/completion 不被旧收尾覆盖。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);

            std::mutex gate_mu;
            std::condition_variable gate_cv;
            std::atomic<bool> gate_entered{false};
            std::atomic<bool> gate_release{false};
            manager.set_exit_gate_for_test([&] {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_entered = true;
                gate_cv.notify_all();
                gate_cv.wait_for(lock, std::chrono::seconds(30),
                                 [&] { return gate_release.load(); });
            });

            device_agent::DownloadRequest req_a;
            req_a.torrent_url = make_plain_torrent(dir, "g3_a.bin", 64 * 1024,
                                                   0xD1);
            assert_true(!req_a.torrent_url.empty());
            req_a.dest_path = dir + "/dl";
            test_mkdir(req_a.dest_path);
            req_a.file_size = 64 * 1024;

            // A 完成并 handoff；worker 随后阻塞在 exit gate（收尾未做）。
            auto seeder_a = make_bt_seeder(req_a.torrent_url, dir);
            assert_true(seeder_a != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder_a->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder_a->listen_port()))});
            CompletionCapture cap_a;
            manager.download(req_a, nullptr,
                [&cap_a](bool ok, const std::string& err,
                         const device_agent::DownloadCompletionTelemetry& t) {
                    cap_a.store(ok, err, &t);
                });
            bool ok_a = false;
            std::string err_a;
            assert_true(cap_a.wait_and_get(&ok_a, &err_a));
            assert_true(ok_a);
            {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_cv.wait_for(lock, std::chrono::seconds(10),
                                 [&] { return gate_entered.load(); });
            }
            assert_true(gate_entered.load());
            assert_true(s2_wait_for([&] {
                return owner->counters_for_test().active_seeds == 1;
            }, 5000));

            // 两个并发 cancel：一个提取 victim 进入锁外 drain，另一个等待。
            std::atomic<bool> c1_started{false};
            std::atomic<bool> c1_done{false};
            std::atomic<bool> c2_started{false};
            std::atomic<bool> c2_done{false};
            std::atomic<bool> c2_wait_entered{false};
            manager.set_drain_wait_entered_for_test([&] {
                c2_wait_entered.store(true);
            });
            std::thread canceller1([&] {
                c1_started.store(true);
                manager.cancel();
                c1_done.store(true);
            });
            std::thread canceller2([&] {
                c2_started.store(true);
                manager.cancel();
                c2_done.store(true);
            });
            for (int i = 0; i < 100 && !(c1_started.load() && c2_started.load()); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert_true(c1_started.load() && c2_started.load());

            // 负向 1：drain 期间新准入被拒绝（不得越过 drain）。此时
            // downloading_ 已被 worker 收尾写为 false——拒绝只能来自
            // drain_in_progress_ 检查，证明 joinable 收尾区间被屏障覆盖。
            device_agent::DownloadRequest req_b;
            req_b.torrent_url = req_a.torrent_url;
            req_b.dest_path = req_a.dest_path;
            req_b.file_size = req_a.file_size;
            CompletionCapture cap_b;
            manager.download(req_b, nullptr,
                [&cap_b](bool ok, const std::string& err,
                         const device_agent::DownloadCompletionTelemetry& t) {
                    cap_b.store(ok, err, &t);
                });
            bool ok_b = false;
            std::string err_b;
            assert_true(cap_b.wait_and_get(&ok_b, &err_b));
            assert_true(!ok_b);
            assert_true(err_b.find("already active") != std::string::npos);

            // 负向 2：第二个 cancel 确定进入 drain 等待（到达信号），且在
            // 放行前不返回（放行只由本线程控制，drain 不可能提前结束）。
            assert_true(s2_wait_for([&] { return c2_wait_entered.load(); }, 5000));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(!c1_done.load() && !c2_done.load());

            // 放行 exit gate → 旧收尾顺序完成（downloading_/idle）→ drain
            // 结束 → 两个 cancel 依次返回。
            {
                std::lock_guard<std::mutex> lock(gate_mu);
                gate_release = true;
                gate_cv.notify_all();
            }
            canceller1.join();
            canceller2.join();
            assert_true(c1_done.load() && c2_done.load());
            assert_true(!manager.is_downloading());
            // A 的历史 seed 不被 cancel 删除（D7）。
            assert_true(owner->counters_for_test().active_seeds == 1);

            // 新代准入：旧收尾已完成（drain 顺序保证），其状态/终态不被覆盖。
            manager.set_exit_gate_for_test(nullptr);
            device_agent::DownloadRequest req_c;
            req_c.torrent_url = make_plain_torrent(dir, "g3_c.bin", 64 * 1024,
                                                   0xD2);
            assert_true(!req_c.torrent_url.empty());
            req_c.dest_path = dir + "/dl";
            req_c.file_size = 64 * 1024;
            auto seeder_c = make_bt_seeder(req_c.torrent_url, dir);
            assert_true(seeder_c != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder_c->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder_c->listen_port()))});
            std::atomic<int> completion_c_count{0};
            CompletionCapture cap_c;
            manager.download(req_c, nullptr,
                [&](bool ok, const std::string& err,
                    const device_agent::DownloadCompletionTelemetry& t) {
                    completion_c_count.fetch_add(1);
                    cap_c.store(ok, err, &t);
                });
            // 飞行中 downloading_ 恒为 true（旧收尾不可能再清除）。
            assert_true(s2_wait_for([&] { return manager.is_downloading(); }, 5000));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(manager.is_downloading());
            bool ok_c = false;
            std::string err_c;
            assert_true(cap_c.wait_and_get(&ok_c, &err_c));
            assert_true(ok_c);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(completion_c_count.load() == 1);  // terminal-once
            assert_true(s2_wait_for([&] {
                const auto counters = owner->counters_for_test();
                return counters.admitted == 2 && counters.active_seeds == 2;
            }, 5000));

            seeder_a.reset();
            seeder_c.reset();
            manager.set_drain_wait_entered_for_test(nullptr);
            owner->Stop();
        }

        std::fprintf(stderr, "[TEST] G4 cancel cleanup vs new admission\n");
        // B5-05 二次窗口②（确定性）：cancel 最终清理（drain 解除 + 水位
        // 条件化 downloading_/idle 清理，持 mu_）与新准入同锁互斥——cleanup
        // gate 阻塞期间新准入无法越过（mu_ 互斥）；放行后 cleanup 先于新
        // 准入完成（同一线性化边界），新代的 downloading 飞行态与
        // terminal-once 不被旧 cancel 的 post-unlock idle 覆盖。
        {
            const std::string dir = make_test_dir();
            std::shared_ptr<device_agent::P2PSeedingOwner> owner = make_s2_owner();
            assert_true(owner != nullptr);
            device_agent::P2PDownloadManager manager({}, s2_manager_policy(),
                                                    nullptr, nullptr, owner);

            std::mutex gate_mu;
            std::condition_variable gate_cv;
            std::atomic<bool> gate_entered{false};
            std::atomic<bool> gate_release{false};
            manager.set_cancel_cleanup_gate_for_test([&] {
                std::unique_lock<std::mutex> lock(gate_mu);
                gate_entered = true;
                gate_cv.notify_all();
                gate_cv.wait_for(lock, std::chrono::seconds(30),
                                 [&] { return gate_release.load(); });
            });

            device_agent::DownloadRequest req_a;
            req_a.torrent_url = make_plain_torrent(dir, "g4_a.bin", 64 * 1024,
                                                   0xE1);
            assert_true(!req_a.torrent_url.empty());
            req_a.dest_path = dir + "/dl";
            test_mkdir(req_a.dest_path);
            req_a.file_size = 64 * 1024;

            // cancel1：victim 进入 drain（A worker 在 stale 检查处退出），
            // 随后阻塞在 cleanup gate（持 mu_）。
            std::atomic<bool> c1_started{false};
            std::atomic<bool> c1_done{false};
            std::thread canceller([&] {
                c1_started.store(true);
                manager.cancel();
                c1_done.store(true);
            });
            for (int i = 0; i < 100 && !c1_started.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert_true(c1_started.load());
            assert_true(s2_wait_for([&] { return gate_entered.load(); }, 5000));
            assert_true(gate_entered.load());  // cancel1 已在最终清理临界区内（持 mu_）

            // gate 未放行：新准入无法越过 cleanup（同锁互斥）。
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(!c1_done.load());

            device_agent::DownloadRequest req_b;
            req_b.torrent_url = make_plain_torrent(dir, "g4_b.bin", 64 * 1024,
                                                   0xE2);
            assert_true(!req_b.torrent_url.empty());
            req_b.dest_path = dir + "/dl";
            req_b.file_size = 64 * 1024;
            auto seeder_b = make_bt_seeder(req_b.torrent_url, dir);
            assert_true(seeder_b != nullptr);
            assert_true(s2_wait_for([&] {
                return seeder_b->listen_port() > 0;
            }, 5000));
            manager.set_test_peer_endpoints_for_test(
                {lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(
                                       seeder_b->listen_port()))});
            std::atomic<int> completion_b_count{0};
            CompletionCapture cap_b;
            manager.download(req_b, nullptr,
                [&](bool ok, const std::string& err,
                    const device_agent::DownloadCompletionTelemetry& t) {
                    completion_b_count.fetch_add(1);
                    cap_b.store(ok, err, &t);
                });

            // 放行 cleanup → cancel1 完成清理（含条件化 idle，先于新代）→
            // 新准入 B 获锁进入（ticket > 水位，状态不被旧收尾覆盖）。
            {
                std::lock_guard<std::mutex> lock(gate_mu);
                gate_release = true;
                gate_cv.notify_all();
            }
            canceller.join();
            assert_true(c1_done.load());
            // gate lambda 捕获块局部 gate_mu/gate_cv——manager 存活期更长，
            // 卸载 hook 以免 dtor 路径再次调用已销毁的同步原语。
            manager.set_cancel_cleanup_gate_for_test(nullptr);
            assert_true(s2_wait_for([&] { return manager.is_downloading(); }, 5000));
            // 旧 cancel 的收尾不会在新代飞行期间把它切 idle。
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(manager.is_downloading());
            assert_true(s2_wait_for([&] {
                return owner->counters_for_test().active_seeds == 1;
            }, 5000));
            bool ok_b = false;
            std::string err_b;
            assert_true(cap_b.wait_and_get(&ok_b, &err_b));
            assert_true(ok_b);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert_true(completion_b_count.load() == 1);  // terminal-once
            assert_true(!manager.is_downloading());

            seeder_b.reset();
            owner->Stop();
        }

    return 0;
}
