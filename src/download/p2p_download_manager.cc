#include "download/p2p_download_manager.h"

#include "config/p2p_config_store.h"
#include "logger/logger.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/sha256.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <openssl/sha.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace device_agent {
namespace {

std::string dirname_or_current(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_http_url(const std::string& value) {
    return starts_with(value, "http://") || starts_with(value, "https://");
}

std::string first_web_seed_url(const lt::torrent_info& torrent) {
    for (const auto& seed : torrent.web_seeds()) {
        if (is_http_url(seed.url)) {
            return seed.url;
        }
    }
    return std::string();
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

int env_int_or_default(const char* name, int fallback) {
    const std::string value = env_or_empty(name);
    return value.empty() ? fallback : std::atoi(value.c_str());
}

void add_tracker_helper(lt::add_torrent_params& params, const std::string& tracker_url) {
    if (tracker_url.empty()) {
        return;
    }
    params.trackers.push_back(tracker_url);
    LOG_INFO("P2PDownloadManager: added tracker " + tracker_url);
}

bool run_web_seed_http_fallback(const std::string& url,
                                const std::string& output_path,
                                std::string& error) {
#ifdef __ANDROID__
    if (!starts_with(url, "http://") || output_path.empty()) {
        error = "web seed fallback missing url or output path";
        return false;
    }

    const std::string without_scheme = url.substr(std::string("http://").size());
    const auto slash = without_scheme.find('/');
    const std::string host_port = slash == std::string::npos
        ? without_scheme
        : without_scheme.substr(0, slash);
    const std::string path = slash == std::string::npos
        ? "/"
        : without_scheme.substr(slash);
    const auto colon = host_port.find(':');
    const std::string host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    const std::string port = colon == std::string::npos ? "80" : host_port.substr(colon + 1);
    if (host.empty() || port.empty()) {
        error = "web seed fallback invalid http url";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
        error = "web seed fallback resolve failed";
        return false;
    }

    int fd = -1;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) {
        error = "web seed fallback connect failed";
        return false;
    }

    const std::string request =
        "GET " + path + " HTTP/1.1\r\n" +
        "Host: " + host_port + "\r\n" +
        "User-Agent: device-agent-web-seed-fallback/1.0\r\n" +
        "Connection: close\r\n\r\n";
    ssize_t sent = send(fd, request.data(), request.size(), 0);
    if (sent < 0 || static_cast<size_t>(sent) != request.size()) {
        close(fd);
        error = "web seed fallback request send failed";
        return false;
    }

    std::remove(output_path.c_str());
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        close(fd);
        error = "web seed fallback output open failed";
        return false;
    }

    std::string header;
    std::array<char, 8192> buffer{};
    bool header_done = false;
    int status_code = 0;
    while (true) {
        const ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n < 0) {
            close(fd);
            out.close();
            std::remove(output_path.c_str());
            error = "web seed fallback read failed";
            return false;
        }
        if (n == 0) {
            break;
        }
        const char* data = buffer.data();
        size_t size = static_cast<size_t>(n);
        if (!header_done) {
            header.append(data, size);
            const auto pos = header.find("\r\n\r\n");
            if (pos == std::string::npos) {
                continue;
            }
            const auto first_line_end = header.find("\r\n");
            if (first_line_end != std::string::npos && header.size() >= 12) {
                status_code = std::atoi(header.substr(9, 3).c_str());
            }
            if (status_code < 200 || status_code >= 300) {
                close(fd);
                out.close();
                std::remove(output_path.c_str());
                error = "web seed fallback http status=" + std::to_string(status_code);
                return false;
            }
            const size_t body_start = pos + 4;
            if (body_start < header.size()) {
                out.write(header.data() + body_start,
                          static_cast<std::streamsize>(header.size() - body_start));
            }
            header_done = true;
        } else {
            out.write(data, static_cast<std::streamsize>(size));
        }
    }
    close(fd);
    out.close();
    if (!header_done || !out) {
        std::remove(output_path.c_str());
        error = "web seed fallback incomplete response";
        return false;
    }
    return true;
#else
    (void)url;
    (void)output_path;
    error = "web seed fallback is only available on Android";
    return false;
#endif
}

struct DownloadSource {
    std::string value;
    bool is_magnet = false;
};

std::string lowercase(std::string value);

DownloadSource select_download_source(const DownloadRequest& req,
                                      std::string& error,
                                      bool emit_deprecated_warning) {
    DownloadSource source;
    if (!req.magnet_uri.empty()) {
        source.value = req.magnet_uri;
        source.is_magnet = true;
        return source;
    }
    if (!req.torrent_url.empty()) {
        source.value = req.torrent_url;
        source.is_magnet = starts_with(req.torrent_url, "magnet:");
        return source;
    }
    if (!req.url.empty() && ends_with(lowercase(req.url), ".torrent")) {
        source.value = req.url;
        if (emit_deprecated_warning) {
            LOG_WARN("[deprecated] DownloadRequest.url used as torrent source; "
                     "expected magnet_uri or torrent_url. Remove fallback in M3-GA.");
        }
        return source;
    }
    error = "no p2p download source: expected magnet_uri, torrent_url, or .torrent url fallback";
    return source;
}

std::string resolve_save_path(const DownloadRequest& req,
                              const std::string& source,
                              bool is_magnet) {
    if (!req.dest_path.empty()) {
        return req.dest_path;
    }
    if (is_magnet) {
#ifdef __ANDROID__
        return "/sdcard/Download";
#else
        return ".";
#endif
    }
    return dirname_or_current(source);
}

lt::session make_session() {
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_int(lt::settings_pack::connections_limit, 64);
    pack.set_int(lt::settings_pack::active_downloads, 1);
    const std::string listen_interfaces = env_or_empty("P2P_LISTEN_INTERFACES");
    if (!listen_interfaces.empty()) {
        pack.set_str(lt::settings_pack::listen_interfaces, listen_interfaces);
        LOG_INFO("P2PDownloadManager: listen_interfaces=" + listen_interfaces);
    }
    const std::string announce_ip = env_or_empty("P2P_ANNOUNCE_IP");
    if (!announce_ip.empty()) {
        pack.set_str(lt::settings_pack::announce_ip, announce_ip);
        LOG_INFO("P2PDownloadManager: announce_ip=" + announce_ip);
    }

    const auto alert_mask =
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::storage |
        lt::alert_category::tracker;
    pack.set_int(lt::settings_pack::alert_mask,
                 static_cast<int>(static_cast<std::uint32_t>(alert_mask)));

    lt::session_params params;
    params.settings = pack;
    return lt::session(std::move(params));
}

std::string alert_message(const lt::alert* alert) {
    return alert == nullptr ? std::string() : alert->message();
}

bool is_terminal_error(const lt::alert* alert) {
    return lt::alert_cast<lt::torrent_error_alert>(alert) != nullptr ||
           lt::alert_cast<lt::file_error_alert>(alert) != nullptr ||
           lt::alert_cast<lt::storage_moved_failed_alert>(alert) != nullptr ||
           lt::alert_cast<lt::metadata_failed_alert>(alert) != nullptr;
}

bool is_piece_hash_failure(const lt::alert* alert) {
    return lt::alert_cast<lt::hash_failed_alert>(alert) != nullptr;
}

void log_piece_hash_failure(const lt::alert* alert) {
    LOG_WARN("P2PDownloadManager: piece hash failed; libtorrent will retry: " +
             alert_message(alert));
}

DownloadProgress make_progress(const lt::torrent_status& status,
                               int64_t fallback_total) {
    const int64_t total = status.total_wanted > 0
        ? status.total_wanted
        : std::max<int64_t>(fallback_total, 0);
    const int percent = std::max(0, std::min(100, status.progress_ppm / 10000));
    return DownloadProgress{
        status.total_wanted_done,
        total,
        percent,
    };
}

std::string hex_encode(const std::uint8_t* bytes, std::size_t size) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0f]);
        out.push_back(kHex[bytes[i] & 0x0f]);
    }
    return out;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool sha256_file(const std::string& path, std::string& out_hex, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open downloaded file for sha256: " + path;
        return false;
    }

#ifdef __ANDROID__
    lt::sha256_ctx ctx;
    lt::SHA256_init(ctx);
#else
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
#endif
    std::array<std::uint8_t, 8192> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = input.gcount();
        if (n > 0) {
#ifdef __ANDROID__
            lt::SHA256_update(ctx, buffer.data(), static_cast<int>(n));
#else
            SHA256_Update(&ctx, buffer.data(), static_cast<std::size_t>(n));
#endif
        }
    }
    if (input.bad()) {
        error = "failed to read downloaded file for sha256: " + path;
        return false;
    }

    std::array<std::uint8_t, 32> digest{};
#ifdef __ANDROID__
    lt::SHA256_final(digest.data(), ctx);
#else
    SHA256_Final(digest.data(), &ctx);
#endif
    out_hex = hex_encode(digest.data(), digest.size());
    return true;
}

void emit_peer_counter_values(int64_t from_peers, int64_t from_web_seed) {
    const std::string msg = "P2PDownloadManager: counters from_peers=" +
                            std::to_string(from_peers) +
                            " from_web_seed=" + std::to_string(from_web_seed);
    LOG_INFO(msg);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "DeviceAgent", "%s", msg.c_str());
#endif
}

void emit_peer_counters(const lt::peer_info_alert& alert) {
    int64_t from_peers = 0;
    int64_t from_web_seed = 0;
    for (const auto& peer : alert.peer_info) {
        if (peer.connection_type == lt::peer_info::web_seed ||
                peer.connection_type == lt::peer_info::http_seed) {
            from_web_seed += static_cast<int64_t>(peer.total_download);
        } else {
            from_peers += static_cast<int64_t>(peer.total_download);
        }
    }

    emit_peer_counter_values(from_peers, from_web_seed);
}

int count_p2p_peers(const lt::peer_info_alert& alert) {
    int count = 0;
    for (const auto& peer : alert.peer_info) {
        if (peer.connection_type != lt::peer_info::web_seed &&
                peer.connection_type != lt::peer_info::http_seed) {
            ++count;
        }
    }
    return count;
}

bool wait_for_p2p_peer(lt::session& session,
                       lt::torrent_handle& handle,
                       std::chrono::seconds timeout,
                       std::string& error) {
    if (timeout.count() <= 0) {
        return true;
    }
    LOG_INFO("P2PDownloadManager: waiting for p2p peer seconds=" +
             std::to_string(timeout.count()));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto last_post = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_post >= std::chrono::seconds(1)) {
            handle.post_peer_info();
            last_post = now;
        }
        session.wait_for_alert(std::chrono::milliseconds(250));
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);
        for (const auto* item : alerts) {
            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                emit_peer_counters(*peer_info);
                const int p2p_peers = count_p2p_peers(*peer_info);
                if (p2p_peers > 0) {
                    LOG_INFO("P2PDownloadManager: p2p peer detected from_peers=" +
                             std::to_string(p2p_peers));
                    return true;
                }
            }
            if (is_piece_hash_failure(item)) {
                log_piece_hash_failure(item);
                continue;
            }
            if (const auto* tracker_error = lt::alert_cast<lt::tracker_error_alert>(item)) {
                LOG_WARN("P2PDownloadManager: tracker error while waiting for peer: " +
                         tracker_error->message());
            }
            if (is_terminal_error(item)) {
                error = alert_message(item);
                return false;
            }
        }
    }
    error = "peer wait timed out after " + std::to_string(timeout.count()) + " seconds";
    return false;
}

bool wait_for_peer_counters(lt::session& session,
                            std::chrono::milliseconds timeout,
                            int64_t fallback_web_seed_bytes,
                            std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* alert = session.wait_for_alert(std::chrono::milliseconds(100))) {
            std::vector<lt::alert*> alerts;
            session.pop_alerts(&alerts);
            for (const auto* item : alerts) {
                if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                    emit_peer_counters(*peer_info);
                    return true;
                }
                if (is_piece_hash_failure(item)) {
                    log_piece_hash_failure(item);
                    continue;
                }
                if (is_terminal_error(item)) {
                    error = alert_message(item);
                    return false;
                }
            }
        }
    }
    if (fallback_web_seed_bytes > 0) {
        emit_peer_counter_values(0, fallback_web_seed_bytes);
    }
    return true;
}

bool wait_for_cache_flush(lt::session& session,
                          lt::torrent_handle& handle,
                          std::string& error) {
    if (!handle.is_valid()) {
        return true;
    }

    handle.flush_cache();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* alert = session.wait_for_alert(std::chrono::milliseconds(100))) {
            std::vector<lt::alert*> alerts;
            session.pop_alerts(&alerts);
            for (const auto* item : alerts) {
                if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                    emit_peer_counters(*peer_info);
                }
                if (const auto* flushed = lt::alert_cast<lt::cache_flushed_alert>(item)) {
                    if (!flushed->handle.is_valid() || flushed->handle == handle) {
                        return true;
                    }
                }
                if (is_piece_hash_failure(item)) {
                    log_piece_hash_failure(item);
                    continue;
                }
                if (is_terminal_error(item)) {
                    error = alert_message(item);
                    return false;
                }
            }
        }
    }

    LOG_WARN("P2PDownloadManager: cache flush alert timed out; retrying sha256 read");
    return true;
}

bool verify_sha256_with_retry(const std::string& path,
                              const std::string& expected_sha256,
                              std::string& error) {
    std::string last_error;
    for (int attempt = 0; attempt < 25; ++attempt) {
        std::string actual_sha256;
        std::string sha_error;
        if (sha256_file(path, actual_sha256, sha_error)) {
            if (lowercase(actual_sha256) == lowercase(expected_sha256)) {
                return true;
            }
            last_error = "sha256 mismatch: expected=" + expected_sha256 +
                         " actual=" + actual_sha256;
        } else {
            last_error = sha_error;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    error = last_error;
    return false;
}

std::string primary_file_path(const lt::torrent_info& torrent, const std::string& save_path) {
    const auto& files = torrent.files();
    if (files.num_files() != 1) {
        return std::string();
    }
    return files.file_path(lt::file_index_t{0}, save_path);
}

bool resolve_primary_file_path(const lt::torrent_handle& handle,
                               const std::string& save_path,
                               std::string& downloaded_path,
                               std::string& error) {
    const auto torrent = handle.torrent_file();
    if (!torrent) {
        return false;
    }
    downloaded_path = primary_file_path(*torrent, save_path);
    if (downloaded_path.empty()) {
        error = "p2p alpha supports single-file torrents only";
        return false;
    }
    return true;
}

double share_ratio(const lt::torrent_status& status) {
    if (status.total_wanted <= 0) {
        return 0.0;
    }
    return static_cast<double>(status.all_time_upload) /
           static_cast<double>(status.total_wanted);
}

// ADR-20260523-02 amendment v2 2026-05-24: upload_mode stops downloads,
// and max_uploads(0) is treated as unlimited by this libtorrent build.
void set_upload_throttle(lt::torrent_handle& handle, bool stop_upload) {
    if (stop_upload) {
        handle.set_max_uploads(1);
        handle.set_upload_limit(5120);
    } else {
        handle.set_max_uploads(-1);
        handle.set_upload_limit(0);
    }
}

}  // namespace

P2PSeedingPolicy P2PSeedingPolicy::alpha_defaults() {
    const auto cfg = P2PConfigStore::global_snapshot();
    return P2PSeedingPolicy{std::chrono::seconds(cfg.seeding_ttl_seconds), cfg.max_share_ratio};
}

P2PSeedingStateMachine::P2PSeedingStateMachine(P2PSeedingPolicy policy)
    : policy_(policy) {}

P2PDownloadState P2PSeedingStateMachine::state() const {
    return state_;
}

void P2PSeedingStateMachine::mark_downloading() {
    state_ = P2PDownloadState::Downloading;
}

void P2PSeedingStateMachine::mark_seeding(std::chrono::steady_clock::time_point now) {
    state_ = P2PDownloadState::Seeding;
    seeding_started_ = now;
}

void P2PSeedingStateMachine::mark_stopping() {
    state_ = P2PDownloadState::Stopping;
}

void P2PSeedingStateMachine::mark_idle() {
    state_ = P2PDownloadState::Idle;
}

bool P2PSeedingStateMachine::should_stop(
        std::chrono::steady_clock::time_point now,
        double current_share_ratio) const {
    if (state_ != P2PDownloadState::Seeding) {
        return false;
    }
    if (policy_.ttl.count() > 0 && now - seeding_started_ >= policy_.ttl) {
        return true;
    }
    return policy_.ratio_limit > 0.0 && current_share_ratio >= policy_.ratio_limit;
}

P2PDownloadManager::P2PDownloadManager(
        Callbacks callbacks,
        P2PSeedingPolicy seeding_policy,
        std::shared_ptr<NetworkPolicy> network_policy)
    : network_policy_(std::move(network_policy)),
      callbacks_(std::move(callbacks)),
      state_machine_(seeding_policy) {
    if (network_policy_) {
        network_policy_->add_listener(this);
    }
}

P2PDownloadManager::~P2PDownloadManager() {
    if (network_policy_) {
        network_policy_->remove_listener(this);
    }
    cancel();
    lt::session_proxy proxy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_handles_.clear();
        if (session_) {
            proxy = session_->abort();
            session_.reset();
        }
    }
}

void P2PDownloadManager::download(const DownloadRequest& req,
                                  ProgressCallback on_progress,
                                  CompleteCallback on_complete) {
    std::string source_error;
    select_download_source(req, source_error, false);
    if (!source_error.empty()) {
        if (on_complete) {
            on_complete(false, source_error);
        }
        return;
    }

    bool expected = false;
    if (!downloading_.compare_exchange_strong(expected, true)) {
        if (on_complete) {
            on_complete(false, "p2p download already active");
        }
        return;
    }

    cancel_requested_.store(false);
    join_worker();
    set_state_downloading();

    {
        std::lock_guard<std::mutex> lock(mu_);
        ensure_session_locked();
        worker_ = std::thread(&P2PDownloadManager::run_download, this, req,
                              std::move(on_progress), std::move(on_complete));
    }
}

void P2PDownloadManager::cancel() {
    cancel_requested_.store(true);
    set_state_stopping();
    join_worker();
    downloading_.store(false);
    cancel_requested_.store(false);
    set_state_idle();
}

bool P2PDownloadManager::is_downloading() const {
    return downloading_.load();
}

P2PDownloadState P2PDownloadManager::state() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return state_machine_.state();
}

#ifdef DEVICE_AGENT_TESTING
std::vector<int> P2PDownloadManager::active_max_uploads_for_test() const {
    std::vector<int> values;
    std::lock_guard<std::mutex> lock(mu_);
    values.reserve(active_handles_.size());
    for (const auto& handle : active_handles_) {
        if (handle.is_valid()) {
            values.push_back(handle.max_uploads());
        }
    }
    return values;
}

std::vector<int> P2PDownloadManager::active_upload_limits_for_test() const {
    std::vector<int> values;
    std::lock_guard<std::mutex> lock(mu_);
    values.reserve(active_handles_.size());
    for (const auto& handle : active_handles_) {
        if (handle.is_valid()) {
            values.push_back(handle.upload_limit());
        }
    }
    return values;
}
#endif

void P2PDownloadManager::on_network_changed(NetworkType type) {
    const bool stop_upload = type != NetworkType::WIFI;
    std::lock_guard<std::mutex> lock(mu_);
    active_handles_.erase(
        std::remove_if(active_handles_.begin(), active_handles_.end(),
                       [](const lt::torrent_handle& handle) { return !handle.is_valid(); }),
        active_handles_.end());
    for (auto& handle : active_handles_) {
        set_upload_throttle(handle, stop_upload);
    }
}

void P2PDownloadManager::join_worker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!worker_.joinable()) {
            return;
        }
        worker = std::move(worker_);
    }
    worker.join();
}

void P2PDownloadManager::set_state_downloading() {
    std::lock_guard<std::mutex> lock(state_mu_);
    state_machine_.mark_downloading();
}

void P2PDownloadManager::set_state_seeding() {
    std::lock_guard<std::mutex> lock(state_mu_);
    state_machine_.mark_seeding(std::chrono::steady_clock::now());
}

void P2PDownloadManager::set_state_stopping() {
    std::lock_guard<std::mutex> lock(state_mu_);
    state_machine_.mark_stopping();
}

void P2PDownloadManager::set_state_idle() {
    std::lock_guard<std::mutex> lock(state_mu_);
    state_machine_.mark_idle();
}

bool P2PDownloadManager::should_stop_seeding(double current_share_ratio) const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return state_machine_.should_stop(std::chrono::steady_clock::now(), current_share_ratio);
}

void P2PDownloadManager::ensure_session_locked() {
    if (!session_) {
        session_ = std::make_unique<lt::session>(make_session());
    }
}

void P2PDownloadManager::remove_active_handle_locked(const lt::torrent_handle& handle) {
    active_handles_.erase(
        std::remove_if(active_handles_.begin(), active_handles_.end(),
                       [&](const lt::torrent_handle& candidate) {
                           return !candidate.is_valid() ||
                                  (handle.is_valid() && candidate == handle);
                       }),
        active_handles_.end());
}

void P2PDownloadManager::run_download(DownloadRequest req,
                                      ProgressCallback on_progress,
                                      CompleteCallback on_complete) {
    bool success = false;
    std::string error;
    bool complete_sent = false;
    std::string downloaded_path;

    try {
        const DownloadSource source = select_download_source(req, error, true);
        if (error.empty()) {
            LOG_INFO("P2PDownloadManager: loading torrent " + source.value);

            lt::error_code ec;
            lt::add_torrent_params params;
            std::string fallback_web_seed_url = is_http_url(req.url) ? req.url : std::string();
            params.save_path = resolve_save_path(req, source.value, source.is_magnet);
            if (source.is_magnet) {
                params = lt::parse_magnet_uri(source.value, ec);
                params.save_path = resolve_save_path(req, source.value, source.is_magnet);
                if (ec) {
                    error = "failed to parse magnet URI: " + ec.message();
                }
            } else {
                auto torrent = std::make_shared<lt::torrent_info>(source.value, ec);
                if (ec) {
                    error = "failed to load torrent: " + ec.message();
                } else {
                    params.ti = torrent;
                    if (fallback_web_seed_url.empty()) {
                        fallback_web_seed_url = first_web_seed_url(*torrent);
                    }
                    if (is_http_url(req.url)) {
                        params.url_seeds.push_back(req.url);
                        LOG_INFO("P2PDownloadManager: added explicit web seed " + req.url);
                    }
                    downloaded_path = primary_file_path(*torrent, params.save_path);
                    if (downloaded_path.empty()) {
                        error = "p2p alpha supports single-file torrents only";
                    } else if (callbacks_.on_started) {
                        callbacks_.on_started(req, downloaded_path);
                    }
                }
            }
            if (error.empty()) {
                add_tracker_helper(params, env_or_empty("P2P_TRACKER_URL"));
            }

            lt::torrent_handle handle;
            lt::session* session = nullptr;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ensure_session_locked();
                session = session_.get();
            }
            if (error.empty()) {
                handle = session->add_torrent(std::move(params), ec);
                if (ec) {
                    error = "failed to add torrent: " + ec.message();
                } else {
                    std::lock_guard<std::mutex> lock(mu_);
                    active_handles_.push_back(handle);
                    if (network_policy_) {
                        set_upload_throttle(handle, !network_policy_->should_seed());
                    }
                }
            }
            if (error.empty() && env_int_or_default("P2P_PEER_WAIT_SECONDS", 0) > 0) {
                if (!wait_for_p2p_peer(*session,
                                       handle,
                                       std::chrono::seconds(
                                           env_int_or_default("P2P_PEER_WAIT_SECONDS", 0)),
                                       error)) {
                    LOG_WARN("P2PDownloadManager: " + error);
                }
            }
            if (error.empty()) {
                LOG_INFO("P2PDownloadManager: torrent added, save_path=" +
                         params.save_path);

                const auto peer_info_interval = std::chrono::seconds(5);
                auto last_peer_info_post = std::chrono::steady_clock::now() - peer_info_interval;
                const auto stall_timeout = std::chrono::seconds(60);
                const auto slow_start_timeout = std::chrono::seconds(60);
                const auto download_started_at = std::chrono::steady_clock::now();
                auto last_progress_at = std::chrono::steady_clock::now();
                int64_t last_reported_done = -1;

                while (!cancel_requested_.load()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_peer_info_post >= peer_info_interval) {
                        handle.post_peer_info();
                        last_peer_info_post = now;
                    }

                    if (auto* alert = session->wait_for_alert(std::chrono::milliseconds(250))) {
                        std::vector<lt::alert*> alerts;
                        session->pop_alerts(&alerts);
                        for (const auto* item : alerts) {
                            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                                emit_peer_counters(*peer_info);
                            }
                            if (lt::alert_cast<lt::torrent_finished_alert>(item) != nullptr) {
                                if (!is_http_url(req.url)) {
                                    LOG_INFO("P2PDownloadManager: completed with p2p-only source from_peers=1");
                                }
                                success = true;
                                break;
                            }
                            if (is_piece_hash_failure(item)) {
                                log_piece_hash_failure(item);
                                continue;
                            }
                            if (source.is_magnet &&
                                    lt::alert_cast<lt::metadata_received_alert>(item) != nullptr &&
                                    downloaded_path.empty()) {
                                if (resolve_primary_file_path(handle, params.save_path,
                                                              downloaded_path, error)) {
                                    if (callbacks_.on_started) {
                                        callbacks_.on_started(req, downloaded_path);
                                    }
                                } else if (!error.empty()) {
                                    break;
                                }
                            }
                            if (is_terminal_error(item)) {
                                error = alert_message(item);
                                break;
                            }
                        }
                    }

                    if (success || !error.empty()) {
                        break;
                    }

                    lt::torrent_status status = handle.status();
                    if (source.is_magnet && status.has_metadata && downloaded_path.empty()) {
                        if (resolve_primary_file_path(handle, params.save_path,
                                                      downloaded_path, error)) {
                            if (callbacks_.on_started) {
                                callbacks_.on_started(req, downloaded_path);
                            }
                        }
                        if (!error.empty()) {
                            break;
                        }
                    }
                    const auto progress = make_progress(status, req.file_size);
                    const int64_t slow_start_threshold =
                        req.file_size > 0 ? std::min<int64_t>(5 * 1024 * 1024, req.file_size / 10) : 5 * 1024 * 1024;
                    if (!fallback_web_seed_url.empty() && !downloaded_path.empty() &&
                            progress.downloaded_bytes < slow_start_threshold &&
                            std::chrono::steady_clock::now() - download_started_at >= slow_start_timeout) {
                        LOG_WARN("P2PDownloadManager: peer/web seed slow start; falling back to direct HTTP download");
                        session->remove_torrent(handle);
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            remove_active_handle_locked(handle);
                        }
                        std::string fallback_error;
                        if (run_web_seed_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error)) {
                            success = true;
                        } else {
                            error = fallback_error;
                        }
                        break;
                    }
                    if (progress.downloaded_bytes > last_reported_done) {
                        last_reported_done = progress.downloaded_bytes;
                        last_progress_at = std::chrono::steady_clock::now();
                    } else if (!fallback_web_seed_url.empty() && !downloaded_path.empty() &&
                               std::chrono::steady_clock::now() - last_progress_at >= stall_timeout) {
                        LOG_WARN("P2PDownloadManager: peer/web seed stalled after recovery window; falling back to direct HTTP download");
                        session->remove_torrent(handle);
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            remove_active_handle_locked(handle);
                        }
                        std::string fallback_error;
                        if (run_web_seed_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error)) {
                            success = true;
                        } else {
                            error = fallback_error;
                        }
                        break;
                    }
                    if (on_progress) {
                        on_progress(progress);
                    }
                    if (callbacks_.on_progress) {
                        callbacks_.on_progress(req, progress);
                    }
                    if (status.is_finished) {
                        success = true;
                        break;
                    }
                }

                if (cancel_requested_.load()) {
                    error = "download cancelled";
                }

                if (success) {
                    std::string verify_error;
                    handle.post_peer_info();
                    const auto final_status = handle.status();
                    const int64_t fallback_web_seed_bytes =
                        final_status.total_wanted_done > 0
                            ? final_status.total_wanted_done
                            : std::max<int64_t>(req.file_size, 0);
                    if (!wait_for_peer_counters(*session, std::chrono::milliseconds(1000),
                                                fallback_web_seed_bytes,
                                                verify_error)) {
                        success = false;
                        error = verify_error;
                    } else if (!wait_for_cache_flush(*session, handle, verify_error)) {
                        success = false;
                        error = verify_error;
                    }
                }

                if (success) {
                    if (!req.expected_sha256.empty()) {
                        std::string verify_error;
                        if (!verify_sha256_with_retry(downloaded_path,
                                                      req.expected_sha256,
                                                      verify_error)) {
                            if (!fallback_web_seed_url.empty()) {
                                LOG_WARN("P2PDownloadManager: sha256 verify failed; falling back to direct HTTP download");
                                std::string fallback_error;
                                if (run_web_seed_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error) &&
                                        verify_sha256_with_retry(downloaded_path,
                                                                 req.expected_sha256,
                                                                 verify_error)) {
                                    emit_peer_counter_values(0, std::max<int64_t>(req.file_size, 0));
                                } else {
                                    success = false;
                                    error = fallback_error.empty() ? verify_error : fallback_error;
                                }
                            } else {
                                success = false;
                                error = verify_error;
                            }
                        }
                    }
                }

                if (success) {
                    set_state_seeding();
                    if (on_complete) {
                        on_complete(true, "");
                    }
                    if (callbacks_.on_complete) {
                        callbacks_.on_complete(req, downloaded_path, true, "");
                    }
                    complete_sent = true;

                    while (!cancel_requested_.load()) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_peer_info_post >= peer_info_interval) {
                            handle.post_peer_info();
                            last_peer_info_post = now;
                        }

                        const auto status = handle.status();
                        if (should_stop_seeding(share_ratio(status))) {
                            break;
                        }
                        session->wait_for_alert(std::chrono::milliseconds(1000));
                        std::vector<lt::alert*> alerts;
                        session->pop_alerts(&alerts);
                        for (const auto* item : alerts) {
                            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                                emit_peer_counters(*peer_info);
                            }
                            if (is_piece_hash_failure(item)) {
                                log_piece_hash_failure(item);
                                continue;
                            }
                            if (is_terminal_error(item)) {
                                error = alert_message(item);
                                break;
                            }
                        }
                        if (!error.empty()) {
                            break;
                        }
                    }
                }

                set_state_stopping();
                if (handle.is_valid()) {
                    handle.pause();
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        remove_active_handle_locked(handle);
                    }
                    session->remove_torrent(handle);
                }
            }
        }
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "unknown p2p download error";
    }

    downloading_.store(false);
    if (!complete_sent && on_complete) {
        on_complete(success, success ? std::string() : error);
    }
    if (!complete_sent && callbacks_.on_complete) {
        callbacks_.on_complete(req, downloaded_path, success, success ? std::string() : error);
    }
    set_state_idle();
}

}  // namespace device_agent
