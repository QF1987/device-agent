#include "download/p2p_download_manager.h"
#include "config/p2p_config_store.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kLibtorrentMaxUploadsUnlimited = 16777215;
constexpr int kLibtorrentUploadLimitUnlimited = -1;
constexpr int kConfiguredUploadLimitBytesPerSecond = 7 * 1024;
constexpr int kCellularSuppressedUploadLimitBytesPerSecond = 5120;
constexpr const char* kShaFixture = "device-agent sha256 fixture\n";
constexpr const char* kShaFixtureDigest =
    "a71202f43a7a48d735443b705a52c0acb30bb1084854969c9f557ba3f72d7a98";

std::string bencoded_string(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

std::string make_test_dir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string base = tmp != nullptr ? tmp : "/data/local/tmp";
    std::string dir = base + "/p2p-download-manager-test-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    return dir;
}

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

int main() {
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

    auto config_store = std::make_shared<P2PConfigStore>();
    std::string config_error;
    assert(config_store->apply(
        R"({"seeding_ttl_seconds":3,"max_share_ratio":1.25,"max_upload_kbps":7,"cellular_seeding_enabled":true})",
        &config_error));
    P2PConfigStore::set_global(config_store);
    const auto alpha_policy = P2PSeedingPolicy::alpha_defaults();
    assert(alpha_policy.ttl == std::chrono::seconds(3));
    assert(alpha_policy.ratio_limit == 1.25);
    assert(alpha_policy.max_upload_kbps == 7);
    assert(alpha_policy.cellular_seeding_enabled);
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
        [&](bool ok, const std::string& err) {
            assert(!ok);
            assert(!err.empty());
            empty_complete = true;
        });
    assert(empty_complete);
    assert(!empty_url_manager.is_downloading());

    P2PDownloadManager invalid_magnet_manager;
    bool invalid_magnet_complete = false;
    DownloadRequest invalid_magnet_req;
    invalid_magnet_req.magnet_uri = "magnet:?xt=urn:btih:invalid";
    invalid_magnet_manager.download(invalid_magnet_req, nullptr,
        [&](bool ok, const std::string& err) {
            assert(!ok);
            assert(err.find("failed to parse magnet URI") != std::string::npos);
            invalid_magnet_complete = true;
        });
    for (int i = 0; i < 40 && !invalid_magnet_complete; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(invalid_magnet_complete);
    assert(!invalid_magnet_manager.is_downloading());

    P2PDownloadManager url_torrent_fallback_manager;
    bool url_torrent_fallback_complete = false;
    DownloadRequest url_torrent_fallback_req;
    url_torrent_fallback_req.url = "/tmp/legacy-url-source.torrent";
    url_torrent_fallback_manager.download(url_torrent_fallback_req, nullptr,
        [&](bool ok, const std::string& err) {
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
    mkdir(save_dir.c_str(), 0700);
    write_seedless_torrent(torrent_path);

    const std::string sha_path = dir + "/sha-fixture.bin";
    {
        std::ofstream out(sha_path, std::ios::binary);
        out << kShaFixture;
    }
    std::string sha_hex;
    std::string sha_error;
    assert(device_agent::sha256_file_for_test(sha_path, sha_hex, sha_error));
    assert(sha_hex == kShaFixtureDigest);

    auto network_policy = std::make_shared<device_agent::NetworkPolicy>();
    network_policy->on_network_changed(NetworkType::WIFI);
    P2PDownloadManager manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 7, false},
        network_policy);
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
    req.file_size = 16384;

    manager.download(req,
        [&](const device_agent::DownloadProgress&) {
            std::lock_guard<std::mutex> lock(mu);
            ++progress_calls;
        },
        [&](bool ok, const std::string& err) {
            std::lock_guard<std::mutex> lock(mu);
            complete_called = true;
            complete_ok = ok;
            complete_error = err;
            cv.notify_one();
        });

    assert(wait_until_downloading(manager));
    assert(wait_until_active_handle(manager));
    assert(wait_until_upload_throttle(
        manager, kLibtorrentMaxUploadsUnlimited, kConfiguredUploadLimitBytesPerSecond));
    assert(manager.state() == P2PDownloadState::Downloading);
    network_policy->on_network_changed(NetworkType::CELLULAR);
    assert(wait_until_upload_throttle(
        manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
    network_policy->on_network_changed(NetworkType::WIFI);
    assert(wait_until_upload_throttle(
        manager, kLibtorrentMaxUploadsUnlimited, kConfiguredUploadLimitBytesPerSecond));
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
    unlimited_policy->on_network_changed(NetworkType::WIFI);
    P2PDownloadManager unlimited_manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 0, true},
        unlimited_policy);
    DownloadRequest unlimited_req;
    unlimited_req.torrent_url = torrent_path;
    unlimited_req.dest_path = save_dir;
    unlimited_req.file_size = 16384;
    unlimited_manager.download(unlimited_req, nullptr, nullptr);
    assert(wait_until_downloading(unlimited_manager));
    assert(wait_until_upload_throttle(
        unlimited_manager, kLibtorrentMaxUploadsUnlimited, kLibtorrentUploadLimitUnlimited));
    unlimited_manager.cancel();
    assert(!unlimited_manager.is_downloading());

    auto default_cellular_policy = std::make_shared<device_agent::NetworkPolicy>();
    default_cellular_policy->on_network_changed(NetworkType::WIFI);
    P2PDownloadManager default_cellular_manager(
        {},
        P2PSeedingPolicy{std::chrono::seconds(3), 1.25, 0, false},
        default_cellular_policy);
    DownloadRequest default_cellular_req;
    default_cellular_req.torrent_url = torrent_path;
    default_cellular_req.dest_path = save_dir;
    default_cellular_req.file_size = 16384;
    default_cellular_manager.download(default_cellular_req, nullptr, nullptr);
    assert(wait_until_downloading(default_cellular_manager));
    default_cellular_policy->on_network_changed(NetworkType::CELLULAR);
    assert(wait_until_upload_throttle(
        default_cellular_manager, 1, kCellularSuppressedUploadLimitBytesPerSecond));
    default_cellular_manager.cancel();
    assert(!default_cellular_manager.is_downloading());

    return 0;
}
