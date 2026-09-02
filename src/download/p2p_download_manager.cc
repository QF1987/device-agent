#include "download/p2p_download_manager.h"
#include "download/p2p_seeding_owner.h"
#include "download/p2p_upload_counters.h"

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
#include <libtorrent/socket_io.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <openssl/evp.h>
constexpr int ANDROID_LOG_INFO = 4;
constexpr int ANDROID_LOG_WARN = 5;
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__) || defined(__APPLE__) || defined(__linux__)
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace device_agent {
namespace {

constexpr int kCellularSuppressedUploadLimitBytesPerSecond = 5120;

struct PeerCounters {
    int64_t from_peers = 0;
    int64_t from_web_seed = 0;
};

struct PeerCounterSample {
    int64_t total_download = 0;
    bool is_web_seed = false;
};

using PeerCounterLedger = std::unordered_map<std::string, PeerCounterSample>;

std::string dirname_or_current(const std::string& path) {
#ifdef _WIN32
    const auto slash = path.find_last_of("/\\");
#else
    const auto slash = path.find_last_of('/');
#endif
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

bool file_exists(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return input.good();
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

std::string make_peer_fingerprint() {
    const std::string override = env_or_empty("P2P_PEER_FINGERPRINT");
    if (!override.empty()) {
        return override;
    }

    std::uint64_t seed =
        static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        (static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) << 1) ^
        (static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())) << 7);
    try {
        std::random_device rd;
        seed ^= (static_cast<std::uint64_t>(rd()) << 32);
        seed ^= static_cast<std::uint64_t>(rd());
    } catch (...) {
        // Some Android builds expose a deterministic or unavailable random_device;
        // time plus thread entropy still avoids identical test-device peer IDs.
    }

    std::mt19937_64 rng(seed);
    constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string fingerprint = "-DA0010-";
    while (fingerprint.size() < 20) {
        fingerprint.push_back(kAlphabet[rng() % (sizeof(kAlphabet) - 1)]);
    }
    return fingerprint;
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
#if defined(__ANDROID__) || defined(__APPLE__) || defined(__linux__)
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
    error = "web seed http fallback is not available on this platform";
    return false;
#endif
}

struct DownloadSource {
    std::string value;
    bool is_magnet = false;
};

std::string lowercase(std::string value);

DownloadSource select_download_source(const DownloadRequest& req,
                                      std::string& error) {
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
    error = "no p2p download source: expected magnet_uri or torrent_url";
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

std::string basename_from_url(const std::string& url) {
    std::string path = url;
    const auto query = path.find_first_of("?#");
    if (query != std::string::npos) {
        path = path.substr(0, query);
    }
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < path.size()) {
        return path.substr(slash + 1);
    }
    return "download.bin";
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") {
#ifdef _WIN32
        return ".\\" + name;
#else
        return "./" + name;
#endif
    }
#ifdef _WIN32
    if (dir.back() == '/' || dir.back() == '\\') {
        return dir + name;
    }
    return dir + "\\" + name;
#else
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
#endif
}

std::string direct_http_output_path(const DownloadRequest& req) {
    const std::string filename = req.file_id.empty() ? basename_from_url(req.url) : req.file_id;
    return join_path(req.dest_path.empty() ? "." : req.dest_path, filename);
}

std::string torrent_metadata_output_path(const DownloadRequest& req) {
    const std::string filename =
        (req.file_id.empty() ? basename_from_url(req.torrent_url) : req.file_id) + ".torrent";
    return join_path(req.dest_path.empty() ? "." : req.dest_path, filename);
}

lt::session make_session() {
    lt::settings_pack pack;
    const std::string peer_fingerprint = make_peer_fingerprint();
    pack.set_str(lt::settings_pack::peer_fingerprint, peer_fingerprint);
    LOG_INFO("P2PDownloadManager: peer_fingerprint prefix=" +
             peer_fingerprint.substr(0, std::min<std::size_t>(8, peer_fingerprint.size())) +
             " length=" + std::to_string(peer_fingerprint.size()));
    const bool enable_utp = env_int_or_default("P2P_ENABLE_UTP", 0) == 1;
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, enable_utp);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, enable_utp);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, true);
    LOG_INFO(std::string("P2PDownloadManager: transport tcp=enabled utp=") +
             (enable_utp ? "enabled" : "disabled"));
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
        lt::alert_category::peer |
        lt::alert_category::connect |
        lt::alert_category::status |
        lt::alert_category::storage |
        lt::alert_category::tracker |
        lt::alert_category::piece_progress |
        lt::alert_category::block_progress;
    pack.set_int(lt::settings_pack::alert_mask,
                 static_cast<int>(static_cast<std::uint32_t>(alert_mask)));

    lt::session_params params;
    params.settings = pack;
    return lt::session(std::move(params));
}

void apply_peer_role_settings(lt::session& session, bool seed_role) {
    const bool enable_utp = env_int_or_default("P2P_ENABLE_UTP", 0) == 1;
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, enable_utp && !seed_role);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, enable_utp && seed_role);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, !seed_role);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, seed_role);
    session.apply_settings(pack);
    LOG_INFO(std::string("P2PDownloadManager: peer role=") +
             (seed_role ? "seed" : "download") +
             " outgoing=" + (seed_role ? "disabled" : "enabled") +
             " incoming=" + (seed_role ? "enabled" : "disabled"));
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

void emit_android_p2p_log(int priority, const std::string& msg) {
#ifdef __ANDROID__
    __android_log_print(priority, "DeviceAgent", "%s", msg.c_str());
#else
    (void)priority;
    (void)msg;
#endif
}

void log_peer_diag_info(const std::string& msg) {
    LOG_INFO(msg);
    emit_android_p2p_log(ANDROID_LOG_INFO, msg);
}

void log_peer_diag_warn(const std::string& msg) {
    LOG_WARN(msg);
    emit_android_p2p_log(ANDROID_LOG_WARN, msg);
}

std::string endpoint_to_string(const lt::tcp::endpoint& endpoint) {
    const std::string address = endpoint.address().to_string();
    if (endpoint.address().is_v6()) {
        return "[" + address + "]:" + std::to_string(endpoint.port());
    }
    return address + ":" + std::to_string(endpoint.port());
}

void log_peer_diagnostic_alert(const lt::alert* alert) {
    if (alert == nullptr) return;

    if (const auto* listen = lt::alert_cast<lt::listen_succeeded_alert>(alert)) {
        log_peer_diag_info("P2PDownloadManager: listen succeeded endpoint=" +
                           endpoint_to_string(lt::tcp::endpoint(listen->address, listen->port)) +
                           " socket_type=" + std::to_string(static_cast<int>(listen->socket_type)));
        return;
    }
    if (const auto* listen = lt::alert_cast<lt::listen_failed_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: listen failed endpoint=" +
                           endpoint_to_string(lt::tcp::endpoint(listen->address, listen->port)) +
                           " socket_type=" + std::to_string(static_cast<int>(listen->socket_type)) +
                           " error=" + listen->error.message() +
                           " msg=" + listen->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_connect_alert>(alert)) {
        log_peer_diag_info("P2PDownloadManager: peer connected endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " direction=" +
                           (peer->direction == lt::peer_connect_alert::direction_t::in ? "in" : "out") +
                           " socket_type=" + std::to_string(static_cast<int>(peer->socket_type)) +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_disconnected_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: peer disconnected endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " socket_type=" + std::to_string(static_cast<int>(peer->socket_type)) +
                           " op=" + std::to_string(static_cast<int>(peer->op)) +
                           " reason=" + std::to_string(static_cast<int>(peer->reason)) +
                           " error=" + peer->error.message() +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_error_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: peer error endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " op=" + std::to_string(static_cast<int>(peer->op)) +
                           " error=" + peer->error.message() +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_blocked_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: peer blocked endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_snubbed_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: peer snubbed endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::peer_unsnubbed_alert>(alert)) {
        log_peer_diag_info("P2PDownloadManager: peer unsnubbed endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::request_dropped_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: request dropped endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " msg=" + peer->message());
        return;
    }
    if (const auto* peer = lt::alert_cast<lt::block_timeout_alert>(alert)) {
        log_peer_diag_warn("P2PDownloadManager: block timeout endpoint=" +
                           endpoint_to_string(peer->endpoint) +
                           " msg=" + peer->message());
        return;
    }
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
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        error = "failed to allocate sha256 context";
        return false;
    }
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        error = "failed to initialize sha256 context";
        return false;
    }
#endif
    std::array<std::uint8_t, 8192> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = input.gcount();
        if (n > 0) {
#ifdef __ANDROID__
            lt::SHA256_update(ctx, buffer.data(), static_cast<int>(n));
#else
            if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(n)) != 1) {
                error = "failed to update sha256 context";
                return false;
            }
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
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1 ||
            digest_len != digest.size()) {
        error = "failed to finalize sha256 digest";
        return false;
    }
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

bool is_web_seed_peer(const lt::peer_info& peer) {
    return peer.connection_type == lt::peer_info::web_seed ||
           peer.connection_type == lt::peer_info::http_seed;
}

std::string peer_counter_key(const lt::peer_info& peer) {
    const bool web_seed = is_web_seed_peer(peer);
    return std::string(web_seed ? "web:" : "peer:") + endpoint_to_string(peer.ip);
}

PeerCounters peer_counter_totals(const PeerCounterLedger& ledger) {
    PeerCounters counters;
    for (const auto& item : ledger) {
        if (item.second.is_web_seed) {
            counters.from_web_seed += item.second.total_download;
        } else {
            counters.from_peers += item.second.total_download;
        }
    }
    return counters;
}

PeerCounters emit_peer_counters(const lt::peer_info_alert& alert,
                                PeerCounterLedger* ledger = nullptr) {
    PeerCounters snapshot;
    for (const auto& peer : alert.peer_info) {
        const bool is_web_seed = is_web_seed_peer(peer);
        const int64_t total_download = static_cast<int64_t>(peer.total_download);
        if (is_web_seed) {
            snapshot.from_web_seed += total_download;
        } else {
            snapshot.from_peers += total_download;
        }
        if (ledger != nullptr) {
            auto& sample = (*ledger)[peer_counter_key(peer)];
            sample.is_web_seed = is_web_seed;
            sample.total_download = std::max(sample.total_download, total_download);
        }
        log_peer_diag_info("P2PDownloadManager: peer info endpoint=" +
                           endpoint_to_string(peer.ip) +
                           " type=" + (is_web_seed ? std::string("web_seed") : std::string("p2p")) +
                           " total_download=" + std::to_string(peer.total_download) +
                           " total_upload=" + std::to_string(peer.total_upload) +
                           " down_speed=" + std::to_string(peer.payload_down_speed) +
                           " up_speed=" + std::to_string(peer.payload_up_speed) +
                           " queue=" + std::to_string(peer.download_queue_length) +
                           " timed_out=" + std::to_string(peer.timed_out_requests) +
                           " request_timeout=" + std::to_string(peer.request_timeout) +
                           " rtt=" + std::to_string(peer.rtt) +
                           " remote_choked=" + std::to_string(
                               static_cast<int>(static_cast<bool>(peer.flags & lt::peer_info::remote_choked))) +
                           " interesting=" + std::to_string(
                               static_cast<int>(static_cast<bool>(peer.flags & lt::peer_info::interesting))) +
                           " seed=" + std::to_string(
                               static_cast<int>(static_cast<bool>(peer.flags & lt::peer_info::seed))) +
                           " snubbed=" + std::to_string(
                               static_cast<int>(static_cast<bool>(peer.flags & lt::peer_info::snubbed))));
    }

    const PeerCounters counters = ledger != nullptr ? peer_counter_totals(*ledger) : snapshot;
    emit_peer_counter_values(counters.from_peers, counters.from_web_seed);
    return counters;
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

bool should_refresh_peer_info_after_alert(const lt::alert* alert) {
    return lt::alert_cast<lt::piece_finished_alert>(alert) != nullptr ||
           lt::alert_cast<lt::block_finished_alert>(alert) != nullptr;
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
            log_peer_diagnostic_alert(item);
            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                emit_peer_counters(*peer_info);
                const int p2p_peers = count_p2p_peers(*peer_info);
                if (p2p_peers > 0) {
                    LOG_INFO("P2PDownloadManager: p2p peer detected from_peers=" +
                             std::to_string(p2p_peers));
                    return true;
                }
            }
            if (lt::alert_cast<lt::torrent_finished_alert>(item) != nullptr) {
                LOG_INFO("P2PDownloadManager: peer wait observed torrent finished");
                return true;
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
        const auto status = handle.status();
        if (status.is_finished || status.is_seeding) {
            LOG_INFO("P2PDownloadManager: peer wait completed by torrent status");
            return true;
        }
    }
    error = "peer wait timed out after " + std::to_string(timeout.count()) + " seconds";
    return false;
}

bool wait_for_peer_counters(lt::session& session,
                            std::chrono::milliseconds timeout,
                            int64_t fallback_web_seed_bytes,
                            PeerCounterLedger& ledger,
                            int64_t& fallback_reported_web_seed_bytes,
                            std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* alert = session.wait_for_alert(std::chrono::milliseconds(100))) {
            std::vector<lt::alert*> alerts;
            session.pop_alerts(&alerts);
            for (const auto* item : alerts) {
                log_peer_diagnostic_alert(item);
                if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                    emit_peer_counters(*peer_info, &ledger);
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
    const auto counters = peer_counter_totals(ledger);
    if (fallback_web_seed_bytes > 0 &&
            counters.from_peers == 0 &&
            counters.from_web_seed == 0) {
        fallback_reported_web_seed_bytes =
            std::max(fallback_reported_web_seed_bytes, fallback_web_seed_bytes);
        emit_peer_counter_values(0, fallback_reported_web_seed_bytes);
    }
    return true;
}

CompletionPathTelemetry choose_completion_path(bool stall_fallback,
                                               bool sha_fallback,
                                               int64_t peer_bytes,
                                               int64_t web_seed_bytes,
                                               int64_t total_payload_download,
                                               bool has_web_seed_hint) {
    if (sha_fallback) {
        return CompletionPathTelemetry::HttpFallbackShaMismatch;
    }
    if (stall_fallback) {
        return CompletionPathTelemetry::HttpFallbackStall;
    }
    if (peer_bytes > 0) {
        return CompletionPathTelemetry::P2PPrimary;
    }
    if (web_seed_bytes > 0) {
        return CompletionPathTelemetry::WebSeedPrimary;
    }
    if (total_payload_download > 0) {
        return has_web_seed_hint
            ? CompletionPathTelemetry::WebSeedPrimary
            : CompletionPathTelemetry::P2PPrimary;
    }
    return CompletionPathTelemetry::Unspecified;
}

DownloadCompletionTelemetry make_completion_telemetry(CompletionPathTelemetry completion_path,
                                                      int64_t peer_bytes,
                                                      int64_t web_seed_bytes) {
    DownloadCompletionTelemetry telemetry;
    telemetry.completion_path = static_cast<int>(completion_path);
    telemetry.peer_bytes = peer_bytes;
    telemetry.web_seed_bytes = web_seed_bytes;
    return telemetry;
}

void emit_complete_callback(const P2PDownloadManager::Callbacks& callbacks,
                            const DownloadRequest& req,
                            const std::string& downloaded_path,
                            bool success,
                            const std::string& error,
                            CompletionPathTelemetry completion_path,
                            int64_t peer_bytes,
                            int64_t web_seed_bytes) {
    if (callbacks.on_complete_with_path) {
        callbacks.on_complete_with_path(
            req,
            downloaded_path,
            success,
            error,
            completion_path,
            peer_bytes,
            web_seed_bytes);
    } else if (callbacks.on_complete) {
        callbacks.on_complete(req, downloaded_path, success, error);
    }
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
                log_peer_diagnostic_alert(item);
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
bool seeding_allowed_on_network(NetworkType type, const P2PSeedingPolicy& policy) {
    if (!policy.seeding_enabled) {
        return false;
    }
    if (type == NetworkType::WIFI) {
        return policy.lan_upload_enabled;
    }
    if (type == NetworkType::CELLULAR) {
        return policy.cellular_seeding_enabled;
    }
    return policy.wan_upload_enabled;
}

int upload_limit_bytes_per_second(const P2PSeedingPolicy& policy) {
    if (policy.max_upload_kbps <= 0) {
        return 0;
    }
    return policy.max_upload_kbps * 1024;
}

int max_upload_peers_for_policy(const P2PSeedingPolicy& policy) {
    if (policy.max_upload_peers <= 0) {
        return -1;
    }
    return policy.max_upload_peers;
}

void set_upload_throttle(lt::torrent_handle& handle,
                         bool stop_upload,
                         const P2PSeedingPolicy& policy) {
    if (stop_upload) {
        handle.set_max_uploads(1);
        handle.set_upload_limit(kCellularSuppressedUploadLimitBytesPerSecond);
    } else {
        handle.set_max_uploads(max_upload_peers_for_policy(policy));
        handle.set_upload_limit(upload_limit_bytes_per_second(policy));
    }
}

}  // namespace

#ifdef DEVICE_AGENT_TESTING
bool sha256_file_for_test(const std::string& path, std::string& out_hex, std::string& error) {
    return sha256_file(path, out_hex, error);
}

CompletionPathTelemetry completion_path_for_test(bool stall_fallback,
                                                 bool sha_fallback,
                                                 int64_t peer_bytes,
                                                 int64_t web_seed_bytes,
                                                 int64_t total_payload_download,
                                                 bool has_web_seed_hint) {
    return choose_completion_path(stall_fallback,
                                  sha_fallback,
                                  peer_bytes,
                                  web_seed_bytes,
                                  total_payload_download,
                                  has_web_seed_hint);
}

std::string join_path_for_test(const std::string& dir, const std::string& name) {
    return join_path(dir, name);
}

std::string dirname_or_current_for_test(const std::string& path) {
    return dirname_or_current(path);
}
#endif

P2PSeedingPolicy P2PSeedingPolicy::alpha_defaults() {
    const auto cfg = P2PConfigStore::global_snapshot();
    return P2PSeedingPolicy{std::chrono::seconds(cfg.seeding_ttl_seconds),
                            cfg.max_share_ratio,
                            cfg.max_upload_kbps,
                            cfg.cellular_seeding_enabled,
                            cfg.p2p_enabled,
                            cfg.seeding_enabled,
                            cfg.max_upload_peers,
                            cfg.lan_upload_enabled,
                            cfg.wan_upload_enabled,
                            cfg.cellular_download_enabled,
                            cfg.min_file_size_mb_for_p2p};
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
        std::shared_ptr<NetworkPolicy> network_policy,
        HttpFallback http_fallback,
        std::shared_ptr<P2PSeedingOwner> seeding_owner)
    : network_policy_(std::move(network_policy)),
      seeding_policy_(seeding_policy),
      http_fallback_(std::move(http_fallback)),
      callbacks_(std::move(callbacks)),
      state_machine_(seeding_policy),
      seeding_owner_(std::move(seeding_owner)) {
    if (network_policy_) {
        network_policy_->add_listener(this);
    }
    // ADR-20260901-01 B5-S2：owner 由注入方（Windows hybrid 生产构造 / 测试）
    // 创建并 Start，生命周期独立于本 manager（D7）；未运行时 handoff 静默
    // 降级为「不做种」（owner 侧 rejected 计数），绝不回退 inline seed（D9）。
    if (seeding_owner_) {
        LOG_INFO("P2PDownloadManager: seeding owner injected; inline seeding "
                 "disabled for this manager");
    }
}

P2PDownloadManager::~P2PDownloadManager() {
#ifdef DEVICE_AGENT_TESTING
    // 先 join 测试 peer 接入的重试线程（它们持有指向本对象 session 的
    // handle），必须先于 session 销毁退出。
    for (auto& t : test_connect_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
#endif
    if (network_policy_) {
        network_policy_->remove_listener(this);
    }
    cancel();
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_handles_.clear();
        // RV-20260901-WIN-P2P-B5-01：~session() 同步收尾（通知 tracker
        // stopped + join io 线程），不再使用跨 session 析构持有的
        // session_proxy——libtorrent 2.0.x 该顺序确定性崩溃（B5-S0 probe）。
        session_.reset();
    }
}

void P2PDownloadManager::download(const DownloadRequest& req,
                                  ProgressCallback on_progress,
                                  CompleteCallback on_complete) {
#ifdef DEVICE_AGENT_TESTING
    // admission-arrived seam：在任何 mu_ 获取之前锁外调用（hook_mu_ 同步
    // 拷贝）——cleanup gate 持有 lifecycle mu_ 期间该信号仍可发出，证明
    // 新 download 已发起并将面对 mu_ 边界（G4 断言）。
    if (auto attempt_gate = copy_hook_for_test(admission_attempt_for_test_)) {
        attempt_gate();
    }
#endif
    refresh_policy_from_config();
    P2PSeedingPolicy policy;
    NetworkType current_network = NetworkType::WIFI;
    {
        std::lock_guard<std::mutex> lock(mu_);
        policy = seeding_policy_;
        current_network = network_policy_ ? network_type_ : NetworkType::WIFI;
    }
    if (!policy.p2p_enabled) {
        if (on_complete) {
            on_complete(false, "p2p disabled by policy", DownloadCompletionTelemetry{});
        }
        return;
    }
    const int64_t min_bytes =
        static_cast<int64_t>(policy.min_file_size_mb_for_p2p) * 1024 * 1024;
    if (policy.min_file_size_mb_for_p2p > 0 && req.file_size > 0 && req.file_size < min_bytes) {
        if (on_complete) {
            on_complete(
                false,
                "p2p disabled by min_file_size_mb_for_p2p policy",
                DownloadCompletionTelemetry{});
        }
        return;
    }
    if (current_network == NetworkType::CELLULAR && !policy.cellular_download_enabled) {
        if (on_complete) {
            on_complete(
                false,
                "p2p cellular download disabled by policy",
                DownloadCompletionTelemetry{});
        }
        return;
    }
    std::string source_error;
    select_download_source(req, source_error);
    if (!source_error.empty() && !is_http_url(req.url)) {
        if (on_complete) {
            on_complete(false, source_error, DownloadCompletionTelemetry{});
        }
        return;
    }

    // ── B5-05 线性化协议：admission ──────────────────────────────
    // fast reject（advisory，权威检查在下方 mu_ 临界区内）。
    if (downloading_.load()) {
        if (on_complete) {
            on_complete(false, "p2p download already active", DownloadCompletionTelemetry{});
        }
        return;
    }
    // 上一个自然结束的 worker 在锁外 join（worker 运行期会取 mu_，禁止
    // 持锁 join）。
    join_worker();

    {
        // 单一 lifecycle 临界区：ticket 分配 + downloading_ 置位 + worker
        // 发布原子完成。「已准入但未发布」状态对外不可见——cancel 与本
        // 临界区互斥（RV-20260901-WIN-P2P-B5-05 窗口①）。
        std::unique_lock<std::mutex> lock(mu_);
        // drain 期间同样拒绝准入：旧 victim 的收尾尚未完成，此时准入会被
        // 旧收尾的 downloading_/state 清除所覆盖（B5-05 drain 协议）。
        if (downloading_.load() || drain_in_progress_) {
            lock.unlock();
            if (on_complete) {
                on_complete(false, "p2p download already active", DownloadCompletionTelemetry{});
            }
            return;
        }
        ++admission_counter_;
        const std::uint64_t generation = admission_counter_;
        downloading_.store(true);
        set_state_downloading();
#ifdef DEVICE_AGENT_TESTING
        // gate A 确定性窗口：ticket 已分配、worker 未发布、mu_ 在手——
        // 并发 cancel() 只能阻塞到本临界区结束（不得先返回再放行请求）。
        if (auto admission_gate = copy_hook_for_test(admission_gate_for_test_)) {
            admission_gate();
        }
#endif
        ensure_session_locked();
        worker_ = std::thread(&P2PDownloadManager::run_download, this, req,
                              std::move(on_progress), std::move(on_complete),
                              generation);
    }
}

void P2PDownloadManager::cancel() {
    // ── B5-05 线性化协议：invalidation + drain（与 join_worker 共用）──
    // 失效水位与 admission ticket 同锁互斥；任一 joinable worker 一律进
    // drain（drain 期间 admission 权威检查拒绝新准入）；并发 cancel 在
    // lifecycle_cv_ 上等 drain 完成——任意 cancel 返回 ⇒ 目标线程已 join
    // 终止。已进入 owner 的历史 seeds 不受影响（独立 seed epoch，D7）。
    set_state_stopping();
    std::thread victim;
    bool has_victim = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancel_epoch_.store(admission_counter_, std::memory_order_seq_cst);
#ifdef DEVICE_AGENT_TESTING
        if (auto drain_started_gate = copy_hook_for_test(drain_started_for_test_)) {
            drain_started_gate();
        }
#endif
        has_victim = take_worker_for_drain_locked(victim);
    }
    if (has_victim) {
        finish_worker_drain(victim);
    } else {
        std::unique_lock<std::mutex> lock(mu_);
#ifdef DEVICE_AGENT_TESTING
        if (auto wait_entered_gate = copy_hook_for_test(drain_wait_entered_for_test_)) {
            wait_entered_gate();
        }
#endif
        lifecycle_cv_.wait(lock, [&] { return !drain_in_progress_; });
        // drain 完成后目标请求已终止；drainer 已按水位条件化清理，此处
        // 无需重复（防御性清理由 drain 路径统一负责）。
    }
}

bool P2PDownloadManager::take_worker_for_drain_locked(std::thread& victim_out) {
    if (!worker_.joinable()) {
        return false;
    }
    victim_out = std::move(worker_);
    drain_in_progress_ = true;
    return true;
}

void P2PDownloadManager::finish_worker_drain(std::thread& victim) {
    // 锁外 join：worker 收尾路径（completion/idle）会取 mu_。
    if (victim.joinable()) {
        victim.join();
    }
    // 同一 mu_ 边界：drain 解除 + 水位条件化清理 + idle。ticket > 水位的
    // 新代（drain 解除后准入）不被本收尾覆盖。
    std::lock_guard<std::mutex> lock(mu_);
    drain_in_progress_ = false;
    lifecycle_cv_.notify_all();
#ifdef DEVICE_AGENT_TESTING
    if (auto cleanup_gate = copy_hook_for_test(cancel_cleanup_gate_for_test_)) {
        // 确定性 cleanup gate（持 mu_）：阻塞期间新准入无法越过同锁边界；
        // 放行后本收尾先于新准入完成（G4 断言）。
        cleanup_gate();
    }
#endif
    if (admission_counter_ <= cancel_epoch_.load(std::memory_order_seq_cst)) {
        downloading_.store(false);
        set_state_idle();
    }
}

#ifdef DEVICE_AGENT_TESTING
std::function<void()> P2PDownloadManager::copy_hook_for_test(
        const std::function<void()>& hook) const {
    std::lock_guard<std::mutex> lock(hook_mu_);
    return hook;
}

void P2PDownloadManager::set_drain_started_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    drain_started_for_test_ = std::move(gate);
}

void P2PDownloadManager::set_admission_attempt_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    admission_attempt_for_test_ = std::move(gate);
}
#endif

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

void P2PDownloadManager::set_http_fallback_for_test(HttpFallback fallback) {
    http_fallback_ = std::move(fallback);
}

void P2PDownloadManager::set_handoff_gate_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    handoff_gate_for_test_ = std::move(gate);
}

void P2PDownloadManager::set_admission_gate_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    admission_gate_for_test_ = std::move(gate);
}

void P2PDownloadManager::set_test_peer_endpoints_for_test(
        std::vector<lt::tcp::endpoint> endpoints) {
    std::lock_guard<std::mutex> lock(mu_);
    test_peer_endpoints_ = std::move(endpoints);
}

void P2PDownloadManager::set_drain_wait_entered_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    drain_wait_entered_for_test_ = std::move(gate);
}

void P2PDownloadManager::set_cancel_cleanup_gate_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    cancel_cleanup_gate_for_test_ = std::move(gate);
}

int P2PDownloadManager::listen_port_for_test() const {
    // session_handle 接口线程安全（内部 post 到 session 线程）。
    std::lock_guard<std::mutex> lock(mu_);
    return session_ ? static_cast<int>(session_->listen_port()) : 0;
}

void P2PDownloadManager::set_exit_gate_for_test(std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(hook_mu_);
    exit_gate_for_test_ = std::move(gate);
}
#endif

void P2PDownloadManager::sample_upload(std::int64_t all_time_upload) {
    const std::int64_t delta = all_time_upload - last_upload_sample_;
    NetworkType type = NetworkType::NONE;
    if (network_policy_) {
        type = network_policy_->current_type();
    } else {
        std::lock_guard<std::mutex> lock(mu_);
        type = network_type_;
    }
    accumulate_p2p_upload(delta, type);  // delta<=0 由计数器内部忽略
    if (all_time_upload > last_upload_sample_) {
        last_upload_sample_ = all_time_upload;
    }
}

void P2PDownloadManager::on_network_changed(NetworkType type) {
    std::vector<lt::torrent_handle> handles;
    P2PSeedingPolicy policy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        network_type_ = type;
        policy = seeding_policy_;
        active_handles_.erase(
            std::remove_if(active_handles_.begin(), active_handles_.end(),
                           [](const lt::torrent_handle& handle) { return !handle.is_valid(); }),
            active_handles_.end());
        handles = active_handles_;
    }
    const bool stop_upload = !seeding_allowed_on_network(type, policy);
    for (auto& handle : handles) {
        if (handle.is_valid()) {
            set_upload_throttle(handle, stop_upload, policy);
        }
    }
}

void P2PDownloadManager::refresh_policy_from_config() {
    if (!P2PConfigStore::has_global()) {
        return;
    }
    const auto policy = P2PSeedingPolicy::alpha_defaults();
    {
        std::lock_guard<std::mutex> lock(mu_);
        seeding_policy_ = policy;
    }
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        state_machine_ = P2PSeedingStateMachine(policy);
    }
}

void P2PDownloadManager::join_worker() {
    // B5-05：download() 准入前的旧 worker 回收走与 cancel 相同的 drain
    // 协议——任何从共享 worker_ 移出 joinable worker 的路径都先发布
    // drain，锁外 join，再在同一 mu_ 边界解除并条件化清理；并发 cancel
    // 因此不会因 worker_ 为空而提前返回。
    std::thread victim;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!take_worker_for_drain_locked(victim)) {
            return;
        }
#ifdef DEVICE_AGENT_TESTING
        if (auto drain_started_gate = copy_hook_for_test(drain_started_for_test_)) {
            drain_started_gate();
        }
#endif
    }
    finish_worker_drain(victim);
}

bool P2PDownloadManager::download_via_http_fallback(const std::string& url,
                                                    const std::string& output_path,
                                                    std::string& error) {
    if (http_fallback_) {
        return http_fallback_(url, output_path, error);
    }
    return run_web_seed_http_fallback(url, output_path, error);
}

void P2PDownloadManager::set_state_downloading() {
    std::lock_guard<std::mutex> lock(state_mu_);
    state_machine_.mark_downloading();
    last_upload_sample_ = 0;  // 新 torrent 的 all_time_upload 从 0 起
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

// handoff 仅在 Windows 生产（DEVICE_AGENT_ENABLE_WINDOWS_P2P）或测试构建
// 编译：Android/Linux/macOS 生产不含 owner 代码路径（ADR-20260901-01）。
#if defined(DEVICE_AGENT_ENABLE_WINDOWS_P2P) || defined(DEVICE_AGENT_TESTING)
bool P2PDownloadManager::try_handoff_to_owner(std::uint64_t generation,
                                              const lt::torrent_handle& handle,
                                              const std::string& save_path) {
    if (!seeding_owner_) {
        return false;
    }
    auto torrent = handle.torrent_file();
    if (!torrent) {
        LOG_WARN("P2PDownloadManager: missing torrent metadata; seed handoff dropped");
        return false;
    }
    // candidate 昂贵构造在 commit 临界区外（B5-05 允许）。
    P2PSeedCandidate candidate;
    // handle.torrent_file() 返回 const；candidate/add_torrent_params 需要
    // 非 const 元数据。仅复制 KB 级 torrent 元数据，数据文件零复制（D3）。
    candidate.torrent = std::make_shared<lt::torrent_info>(*torrent);
    candidate.save_path = save_path;
    candidate.admitted_at = std::chrono::steady_clock::now();
    {
        // TTL/ratio 取 admission snapshot（D6：运行中配置变化不追溯生命周期）。
        std::lock_guard<std::mutex> lock(mu_);
        candidate.ttl = seeding_policy_.ttl;
        candidate.ratio_limit = seeding_policy_.ratio_limit;
    }
#ifdef DEVICE_AGENT_TESTING
    {
        // hook 读取与 setter 同步于 hook_mu_（独立于 lifecycle mu_）；gate
        // 本体锁外调用。B5-05 gate B 确定性窗口：candidate 已构造、最终
        // commit（validity 校验 + Admit）未发生。窗口内 cancel() 的失效
        // 递增与下方 commit 同锁互斥——cancel 赢则 commit 判 stale 丢弃；
        // commit 赢则线性化明确早于 cancel，seed 保留。
        if (auto handoff_gate = copy_hook_for_test(handoff_gate_for_test_)) {
            handoff_gate();
        }
    }
#endif
    {
        // ── B5-05 handoff commit 临界区 ──
        // 最终 validity 校验 + Admit 与 cancel 的失效递增同锁互斥：commit
        // 之后才发生的 cancel 不影响已提交 candidate（seed 保留）；commit
        // 之前发生的 cancel 必然使本代 ≤ 水位而被丢弃。Admit 非阻塞
        // （有界队列入队），持锁调用无死锁（owner 不取 manager 锁）。
        std::lock_guard<std::mutex> lock(mu_);
        if (generation <= cancel_epoch_.load(std::memory_order_seq_cst)) {
            LOG_WARN("P2PDownloadManager: stale generation; seed handoff dropped");
            return false;
        }
        // 非阻塞移交：接受/去重/淘汰/拒绝都只进 owner telemetry；移交不
        // 反转已发出的 release success，也不触发第二次 completion（D2/D7/D9）。
        seeding_owner_->Admit(std::move(candidate));
    }
    LOG_INFO("P2PDownloadManager: seed handoff submitted to owner");
    return true;
}
#endif  // owner gate

void P2PDownloadManager::run_download(DownloadRequest req,
                                      ProgressCallback on_progress,
                                      CompleteCallback on_complete,
                                      std::uint64_t generation) {
    // ADR-20260901-01 D7：cancel()/新准入会递增 generation；stale worker
    // 不得 handoff、不得再启动 HTTP fallback。
    // B5-05：stale ⟺ 本代 ticket ≤ cancel 失效水位（cancel_epoch_ 原子镜像，
    // 无锁轮询；水位只在 lifecycle mutex 内前移）。
    const auto generation_stale = [this, generation]() {
        return generation <= cancel_epoch_.load(std::memory_order_seq_cst);
    };
    bool success = false;
    std::string error;
    bool complete_sent = false;
    std::string downloaded_path;
    bool completed_by_stall_fallback = false;
    bool completed_by_sha_fallback = false;
    PeerCounterLedger peer_counter_ledger;
    int64_t fallback_reported_web_seed_bytes = 0;
    int64_t last_total_payload_download = 0;
    bool has_web_seed_hint = false;

    try {
        const DownloadSource source = select_download_source(req, error);
        if (!error.empty() && is_http_url(req.url)) {
            error.clear();
            downloaded_path = direct_http_output_path(req);
            if (callbacks_.on_started) {
                callbacks_.on_started(req, downloaded_path);
            }
            std::string fallback_error;
            if (download_via_http_fallback(req.url, downloaded_path, fallback_error)) {
                success = true;
                completed_by_stall_fallback = true;
                has_web_seed_hint = true;
                if (on_progress) {
                    on_progress(DownloadProgress{req.file_size, req.file_size, 100});
                }
                if (callbacks_.on_progress) {
                    callbacks_.on_progress(req, DownloadProgress{req.file_size, req.file_size, 100});
                }
                if (!req.expected_sha256.empty()) {
                    std::string verify_error;
                    if (!verify_sha256_with_retry(downloaded_path, req.expected_sha256, verify_error)) {
                        success = false;
                        error = verify_error;
                    }
                }
            } else {
                error = fallback_error;
            }
        } else if (error.empty()) {
            std::string torrent_source = source.value;
            if (!source.is_magnet && is_http_url(source.value)) {
                const std::string metadata_path = torrent_metadata_output_path(req);
                LOG_INFO("P2PDownloadManager: downloading torrent metadata " + source.value +
                         " -> " + metadata_path);
                std::string metadata_error;
                if (!download_via_http_fallback(source.value, metadata_path, metadata_error)) {
                    error = "failed to download torrent metadata: " + metadata_error;
                } else {
                    torrent_source = metadata_path;
                }
            }

            LOG_INFO("P2PDownloadManager: loading torrent " + torrent_source);

            lt::error_code ec;
            lt::add_torrent_params params;
            std::string fallback_web_seed_url = is_http_url(req.url) ? req.url : std::string();
            params.save_path = resolve_save_path(req, torrent_source, source.is_magnet);
            if (source.is_magnet) {
                params = lt::parse_magnet_uri(source.value, ec);
                params.save_path = resolve_save_path(req, source.value, source.is_magnet);
                if (ec) {
                    error = "failed to parse magnet URI: " + ec.message();
                }
            } else {
                auto torrent = std::make_shared<lt::torrent_info>(torrent_source, ec);
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
                    has_web_seed_hint = !fallback_web_seed_url.empty();
	                    downloaded_path = primary_file_path(*torrent, params.save_path);
	                    if (downloaded_path.empty()) {
	                        error = "p2p alpha supports single-file torrents only";
	                    } else if (!req.expected_sha256.empty() && file_exists(downloaded_path)) {
	                        std::string seed_verify_error;
	                        if (verify_sha256_with_retry(downloaded_path,
	                                                      req.expected_sha256,
	                                                      seed_verify_error)) {
	                            params.flags |= lt::torrent_flags::seed_mode;
	                            LOG_INFO("P2PDownloadManager: existing file verified; enabling seed_mode");
	                        } else {
	                            LOG_WARN("P2PDownloadManager: existing file not trusted for seed_mode: " +
	                                     seed_verify_error);
	                        }
	                    }
	                    if (error.empty() && callbacks_.on_started) {
	                        callbacks_.on_started(req, downloaded_path);
	                    }
                }
            }
            if (error.empty()) {
                has_web_seed_hint = !fallback_web_seed_url.empty();
                add_tracker_helper(params, env_or_empty("P2P_TRACKER_URL"));
            }

            lt::torrent_handle handle;
            lt::session* session = nullptr;
            const std::string download_save_path = params.save_path;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ensure_session_locked();
                session = session_.get();
            }
	            if (error.empty()) {
	                const bool seed_role = static_cast<bool>(params.flags & lt::torrent_flags::seed_mode);
	                apply_peer_role_settings(*session, seed_role);
	                handle = session->add_torrent(std::move(params), ec);
                if (ec) {
                    error = "failed to add torrent: " + ec.message();
                } else {
                    // 初始 throttle 与 on_network_changed 共用同一
                    // seeding_allowed_on_network 判定（RV-20260831-WIN-P2P-A-04）：
                    // 同一把锁内取 network type + seeding policy 快照，锁外调
                    // libtorrent；不再单独读 should_seed 或做 cellular 放宽。
                    NetworkType network_type = NetworkType::WIFI;
                    P2PSeedingPolicy policy;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        active_handles_.push_back(handle);
                        policy = seeding_policy_;
                        network_type = network_policy_ ? network_type_ : NetworkType::WIFI;
                    }
                    if (network_policy_) {
                        set_upload_throttle(
                            handle,
                            !seeding_allowed_on_network(network_type, policy),
                            policy);
                    }
#ifdef DEVICE_AGENT_TESTING
                    // 确定性 peer 接入（测试基建）：handle 注册后由分离线程
                    // 外连注入的 endpoint（download 角色 incoming=off，peer
                    // 必须由 manager 外连；seeder 端 incoming=on 已就绪）。
                    // 重试覆盖对端 accept 就绪窗口；worker 不被阻塞，alert
                    // 处理与 completion 时序不受影响。未注入 endpoint 的
                    // 测试零开销。
                    std::vector<lt::tcp::endpoint> test_endpoints;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        test_endpoints = test_peer_endpoints_;
                    }
                    if (!test_endpoints.empty()) {
                        auto connect_handle = handle;
                        auto endpoints_copy = std::move(test_endpoints);
                        // 停止标志由 worker 生命周期持有：worker 退出下载
                        // 循环（cancel/完成/失败）即置位，重试线程随之退出。
                        auto stop_flag = std::make_shared<std::atomic<bool>>(false);
                        test_connect_stop_ = stop_flag;
                        test_connect_threads_.emplace_back(
                            [connect_handle, endpoints_copy, stop_flag] {
                            try {
                                for (int attempt = 0; attempt < 25; ++attempt) {
                                    if (!connect_handle.is_valid() ||
                                            stop_flag->load()) {
                                        return;
                                    }
                                    for (const auto& endpoint : endpoints_copy) {
                                        connect_handle.connect_peer(endpoint);
                                    }
                                    if (connect_handle.status().num_peers > 0) {
                                        return;
                                    }
                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(400));
                                }
                            } catch (...) {
                                // 测试基建线程：异常吞掉，不许 terminate。
                            }
                        });
                    }
#endif
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
                const auto event_peer_info_floor = std::chrono::milliseconds(250);
                auto last_peer_info_post = std::chrono::steady_clock::now() - peer_info_interval;
                const auto stall_timeout = std::chrono::seconds(60);
                const auto slow_start_timeout = std::chrono::seconds(60);
                const auto download_started_at = std::chrono::steady_clock::now();
                auto last_progress_at = std::chrono::steady_clock::now();
                int64_t last_reported_done = -1;

                while (!generation_stale()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_peer_info_post >= peer_info_interval) {
                        handle.post_peer_info();
                        last_peer_info_post = now;
                    }

                    if (auto* alert = session->wait_for_alert(std::chrono::milliseconds(250))) {
                        std::vector<lt::alert*> alerts;
                        session->pop_alerts(&alerts);
                        for (const auto* item : alerts) {
                            log_peer_diagnostic_alert(item);
                            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                                emit_peer_counters(*peer_info, &peer_counter_ledger);
                            }
                            const auto event_now = std::chrono::steady_clock::now();
                            if (should_refresh_peer_info_after_alert(item) &&
                                    event_now - last_peer_info_post >= event_peer_info_floor) {
                                handle.post_peer_info();
                                last_peer_info_post = event_now;
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
                    sample_upload(static_cast<std::int64_t>(status.all_time_upload));
                    last_total_payload_download = std::max<int64_t>(
                        last_total_payload_download,
                        static_cast<int64_t>(status.total_payload_download));
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
                            std::chrono::steady_clock::now() - download_started_at >= slow_start_timeout &&
                            !generation_stale()) {
                        LOG_WARN("P2PDownloadManager: peer/web seed slow start; falling back to direct HTTP download");
                        session->remove_torrent(handle);
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            remove_active_handle_locked(handle);
                        }
                        std::string fallback_error;
                        if (download_via_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error)) {
                            success = true;
                            completed_by_stall_fallback = true;
                        } else {
                            error = fallback_error;
                        }
                        break;
                    }
                    if (progress.downloaded_bytes > last_reported_done) {
                        last_reported_done = progress.downloaded_bytes;
                        last_progress_at = std::chrono::steady_clock::now();
                    } else if (!fallback_web_seed_url.empty() && !downloaded_path.empty() &&
                               std::chrono::steady_clock::now() - last_progress_at >= stall_timeout &&
                               !generation_stale()) {
                        LOG_WARN("P2PDownloadManager: peer/web seed stalled after recovery window; falling back to direct HTTP download");
                        session->remove_torrent(handle);
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            remove_active_handle_locked(handle);
                        }
                        std::string fallback_error;
                        if (download_via_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error)) {
                            success = true;
                            completed_by_stall_fallback = true;
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

                if (generation_stale()) {
                    error = "download cancelled";
                }

                if (success) {
                    std::string verify_error;
                    handle.post_peer_info();
                    const auto final_status = handle.status();
                    last_total_payload_download = std::max<int64_t>(
                        last_total_payload_download,
                        static_cast<int64_t>(final_status.total_payload_download));
                    const int64_t fallback_web_seed_bytes =
                        has_web_seed_hint && final_status.total_wanted_done > 0
                            ? final_status.total_wanted_done
                            : (has_web_seed_hint ? std::max<int64_t>(req.file_size, 0) : 0);
                    if (!wait_for_peer_counters(*session, std::chrono::milliseconds(1000),
                                                fallback_web_seed_bytes,
                                                peer_counter_ledger,
                                                fallback_reported_web_seed_bytes,
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
                            if (!fallback_web_seed_url.empty() && !generation_stale()) {
                                LOG_WARN("P2PDownloadManager: sha256 verify failed; falling back to direct HTTP download");
                                std::string fallback_error;
                                if (download_via_http_fallback(fallback_web_seed_url, downloaded_path, fallback_error) &&
                                        verify_sha256_with_retry(downloaded_path,
                                                                 req.expected_sha256,
                                                                 verify_error)) {
                                    fallback_reported_web_seed_bytes = std::max<int64_t>(
                                        fallback_reported_web_seed_bytes,
                                        std::max<int64_t>(req.file_size, 0));
                                    emit_peer_counter_values(0, fallback_reported_web_seed_bytes);
                                    completed_by_sha_fallback = true;
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
                    const auto final_counters = peer_counter_totals(peer_counter_ledger);
                    const int64_t final_web_seed_bytes =
                        std::max(final_counters.from_web_seed, fallback_reported_web_seed_bytes);
                    const auto completion_path =
                        choose_completion_path(completed_by_stall_fallback,
                                               completed_by_sha_fallback,
                                               final_counters.from_peers,
                                               final_web_seed_bytes,
                                               last_total_payload_download,
                                               has_web_seed_hint);
                    if (on_complete) {
                        on_complete(
                            true,
                            "",
                            make_completion_telemetry(
                                completion_path,
                                final_counters.from_peers,
                                final_web_seed_bytes));
                    }
                    if (callbacks_.on_complete || callbacks_.on_complete_with_path) {
                        emit_complete_callback(
                            callbacks_,
                            req,
                            downloaded_path,
                            true,
                            "",
                            completion_path,
                            final_counters.from_peers,
                            final_web_seed_bytes);
                    }
                    complete_sent = true;

                    // ADR-20260901-01 B5-S2：注入 owner 时成功终态即移交
                    // candidate（cache flush/SHA/generation 已全部通过），
                    // worker 立即退出释放 admission——任务 A 成功后任务 B
                    // 不再被 `already active` 阻塞。stale 代不做种；owner
                    // 注入后绝不回退 inline seeding（D9）。未注入 owner 的
                    // 平台（Android/Linux/macOS）保留 inline seeding loop
                    // （循环条件首项，注入时整体跳过）。handoff 代码仅在
                    // Windows 生产 / 测试构建编译（见函数尾部 gate）。
#if defined(DEVICE_AGENT_ENABLE_WINDOWS_P2P) || defined(DEVICE_AGENT_TESTING)
                    if (seeding_owner_) {
                        try_handoff_to_owner(generation, handle,
                                             download_save_path);
                    }
#endif
                    while (!seeding_owner_ && !generation_stale()) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_peer_info_post >= peer_info_interval) {
                            handle.post_peer_info();
                            last_peer_info_post = now;
                        }

                        const auto status = handle.status();
                        sample_upload(static_cast<std::int64_t>(status.all_time_upload));
                        if (should_stop_seeding(share_ratio(status))) {
                            break;
                        }
                        session->wait_for_alert(std::chrono::milliseconds(1000));
                        std::vector<lt::alert*> alerts;
                        session->pop_alerts(&alerts);
                        for (const auto* item : alerts) {
                            log_peer_diagnostic_alert(item);
                            if (const auto* peer_info = lt::alert_cast<lt::peer_info_alert>(item)) {
                                emit_peer_counters(*peer_info, &peer_counter_ledger);
                            }
                            const auto event_now = std::chrono::steady_clock::now();
                            if (should_refresh_peer_info_after_alert(item) &&
                                    event_now - last_peer_info_post >= event_peer_info_floor) {
                                handle.post_peer_info();
                                last_peer_info_post = event_now;
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
#ifdef DEVICE_AGENT_TESTING
    {
        // hook 读取与 setter 同步于 hook_mu_（独立于 lifecycle mu_）；gate
        // 本体锁外调用——B5-05 drain gate：worker 已写 downloading_=false、
        // 但 completion/idle/线程退出尚未完成的 joinable 收尾区间——新准入
        // 由 drain 拒绝、并发 cancel 在 drain 上等待（G3/G5 断言该区间被
        // 屏障覆盖）。
        if (auto exit_gate = copy_hook_for_test(exit_gate_for_test_)) {
            exit_gate();
        }
    }
#endif
    const auto final_counters = peer_counter_totals(peer_counter_ledger);
    const int64_t final_web_seed_bytes =
        std::max(final_counters.from_web_seed, fallback_reported_web_seed_bytes);
    const auto completion_path =
        choose_completion_path(completed_by_stall_fallback,
                               completed_by_sha_fallback,
                               final_counters.from_peers,
                               final_web_seed_bytes,
                               last_total_payload_download,
                               has_web_seed_hint);
    if (!complete_sent && on_complete) {
        on_complete(
            success,
            success ? std::string() : error,
            make_completion_telemetry(
                completion_path,
                final_counters.from_peers,
                final_web_seed_bytes));
    }
    if (!complete_sent && (callbacks_.on_complete || callbacks_.on_complete_with_path)) {
        emit_complete_callback(
            callbacks_,
            req,
            downloaded_path,
            success,
            success ? std::string() : error,
            completion_path,
            final_counters.from_peers,
            final_web_seed_bytes);
    }
    set_state_idle();
}

}  // namespace device_agent
