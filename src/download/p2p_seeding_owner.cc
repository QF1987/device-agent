#include "download/p2p_seeding_owner.h"
#include "download/p2p_upload_counters.h"

#include "config/p2p_config_store.h"
#include "logger/logger.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace device_agent {
namespace {

// 与 p2p_download_manager.cc 既有 throttle 契约保持同一数值（B5-S1 不改
// 前台文件；S2 handoff 时由 Reviewer 确认统一归属）。
constexpr int kCellularSuppressedUploadLimitBytesPerSecond = 5120;
constexpr int kCellularSuppressedMaxUploads = 1;

// ADR-20260901-01 Decision 3：编译期安全常量，首版不引入 backend/schema。
constexpr std::size_t kMaxActiveSeeds = 2;
// 有界 handoff 队列深度：> cap，吸收前台突发，满即丢（D2 非阻塞投递）。
constexpr std::size_t kSeedQueueDepth = 4;
constexpr int kListenVerifyTimeoutSeconds = 10;
constexpr int kSeedPollIntervalSeconds = 1;  // D3：poll 间隔不短于 1s

// 与 p2p_download_manager.cc seeding_allowed_on_network 同一判定语义。
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
        return 0;  // 0 = unlimited（B5-S0 probe 锁值）
    }
    return policy.max_upload_kbps * 1024;
}

int max_upload_peers_for_policy(const P2PSeedingPolicy& policy) {
    if (policy.max_upload_peers <= 0) {
        return -1;
    }
    return policy.max_upload_peers;
}

std::string make_seed_fingerprint() {
    std::uint64_t seed =
        static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        (static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) << 1);
    try {
        std::random_device rd;
        seed ^= (static_cast<std::uint64_t>(rd()) << 32);
        seed ^= static_cast<std::uint64_t>(rd());
    } catch (...) {
    }
    std::mt19937_64 rng(seed);
    constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string fingerprint = "-DA0010-";
    while (fingerprint.size() < 20) {
        fingerprint.push_back(kAlphabet[rng() % (sizeof(kAlphabet) - 1)]);
    }
    return fingerprint;
}

// info-hash 短前缀（8 hex），日志只用它，不记 URL/token/本地路径（D8）。
std::string short_log_id(const std::string& dedupe_key) {
    if (dedupe_key.size() <= 3) {
        return "unknown";
    }
    const std::string raw = dedupe_key.substr(3);
    return lt::aux::to_hex(
               lt::span<const char>(raw.data(), raw.size()))
        .substr(0, 8);
}

bool file_readable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

std::string candidate_file_path(const P2PSeedCandidate& candidate) {
    if (!candidate.torrent ||
            candidate.torrent->files().num_files() != 1) {
        return std::string();
    }
    return candidate.torrent->files().file_path(lt::file_index_t{0},
                                                candidate.save_path);
}

void apply_global_only_peer_class_filter(lt::session& session) {
    // L2：默认 libtorrent 对 local network peer 豁免限速；把全部 v4 段强制
    // 归入 global class，使 session/handle 级上限对 LAN peer 生效（v6 段在
    // 纯 v6 环境由 listen 实际使用情况决定，首版 fleet seeding 走 v4/v6
    // 双栈时 v6 侧依赖同一 filter 语义，Windows 原生 gate 复验）。
    lt::ip_filter filter;
    filter.add_rule(lt::make_address("0.0.0.0"),
                    lt::make_address("255.255.255.255"),
                    static_cast<std::uint32_t>(
                        lt::session_handle::global_peer_class_id));
    filter.add_rule(lt::make_address("::"),
                    lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"),
                    static_cast<std::uint32_t>(
                        lt::session_handle::global_peer_class_id));
    session.set_peer_class_filter(filter);
}

std::uint16_t configured_listen_port(const std::string& listen_interfaces) {
    const auto colon = listen_interfaces.find_last_of(':');
    if (colon == std::string::npos || colon + 1 >= listen_interfaces.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(std::atoi(listen_interfaces.c_str() + colon + 1));
}

}  // namespace

std::string P2PSeedCandidate::dedupe_key() const {
    if (!torrent) {
        return std::string();
    }
    const lt::info_hash_t hashes = torrent->info_hashes();
    if (!hashes.v1.is_all_zeros()) {
        return std::string("v1:") + hashes.v1.to_string();
    }
    if (!hashes.v2.is_all_zeros()) {
        return std::string("v2:") + hashes.v2.to_string();
    }
    return std::string();
}

P2PSeedingPolicy P2PSeedingOwner::live_policy() const {
    if (config_.policy_provider) {
        return config_.policy_provider();
    }
    if (P2PConfigStore::has_global()) {
        // 与 P2PSeedingPolicy::alpha_defaults 的字段映射保持一致（owner 不
        // 链接前台 p2p_download_manager.cc；parity 由 Reviewer 校对）。
        const auto cfg = P2PConfigStore::global_snapshot();
        P2PSeedingPolicy policy;
        policy.ttl = std::chrono::seconds(cfg.seeding_ttl_seconds);
        policy.ratio_limit = cfg.max_share_ratio;
        policy.max_upload_kbps = cfg.max_upload_kbps;
        policy.cellular_seeding_enabled = cfg.cellular_seeding_enabled;
        policy.p2p_enabled = cfg.p2p_enabled;
        policy.seeding_enabled = cfg.seeding_enabled;
        policy.max_upload_peers = cfg.max_upload_peers;
        policy.lan_upload_enabled = cfg.lan_upload_enabled;
        policy.wan_upload_enabled = cfg.wan_upload_enabled;
        policy.cellular_download_enabled = cfg.cellular_download_enabled;
        policy.min_file_size_mb_for_p2p = cfg.min_file_size_mb_for_p2p;
        return policy;
    }
    return P2PSeedingPolicy{};
}

P2PSeedingOwner::P2PSeedingOwner(Config config,
                                 std::shared_ptr<NetworkPolicy> network_policy)
    : config_(std::move(config)),
      network_policy_(std::move(network_policy)) {
    if (network_policy_) {
        network_policy_->add_listener(this);
    }
}

P2PSeedingOwner::~P2PSeedingOwner() {
    // 先摘 listener（防止析构期间回调进入本对象），再幂等 Stop（L3 安全
    // teardown：不使用跨 session 析构持有的 session_proxy）。
    if (network_policy_) {
        network_policy_->remove_listener(this);
    }
    Stop();
}

bool P2PSeedingOwner::Start(std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) {
            return true;
        }
    }
    if (config_.listen_interfaces.empty()) {
        error = "seeding owner missing listen_interfaces";
        return false;
    }

    // L1 锁值：seed role 一次性（incoming on / outgoing off），DHT off
    // （fleet 为 opentracker+LSD 拓扑）、LSD on、端口冲突 fail-fast。
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::peer_fingerprint, make_seed_fingerprint());
    pack.set_bool(lt::settings_pack::enable_dht, false);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_bool(lt::settings_pack::listen_system_port_fallback, false);
    pack.set_int(lt::settings_pack::max_retry_port_bind, 0);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, false);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, true);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, false);
    pack.set_str(lt::settings_pack::listen_interfaces, config_.listen_interfaces);
    pack.set_int(lt::settings_pack::connections_limit,
                 config_.connections_limit > 0 ? config_.connections_limit : 32);
    const auto alert_mask =
        lt::alert_category::error | lt::alert_category::peer |
        lt::alert_category::connect | lt::alert_category::status |
        lt::alert_category::storage | lt::alert_category::tracker;
    pack.set_int(lt::settings_pack::alert_mask,
                 static_cast<int>(static_cast<std::uint32_t>(alert_mask)));

    lt::session_params params;
    params.settings = pack;
    auto session = std::make_unique<lt::session>(std::move(params));

    const std::uint16_t configured_port = configured_listen_port(config_.listen_interfaces);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kListenVerifyTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline && !session->is_listening()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool port_ok = configured_port == 0
        ? session->listen_port() != 0
        : session->listen_port() == configured_port;
    if (!session->is_listening() || !port_ok) {
        error = "seed session listen failed: is_listening=" +
                std::string(session->is_listening() ? "true" : "false") +
                " listen_port=" + std::to_string(session->listen_port()) +
                " configured=" + std::to_string(configured_port);
        LOG_WARN("P2PSeedingOwner: " + error);
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++counters_.stop_session_error;
        }
        session.reset();  // L3：安全 teardown（直接析构，不持 proxy）
        return false;
    }

    // L2：先设 filter（只影响新连接），任何 peer 接入前完成。
    apply_global_only_peer_class_filter(*session);

    NetworkType initial_network = NetworkType::WIFI;
    if (network_policy_) {
        initial_network = network_policy_->current_type();  // 首样本 NONE fail closed（D6）
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        network_type_ = initial_network;
        running_ = true;
        stop_requested_ = false;
    }
    session_ = std::move(session);
    thread_ = std::thread(&P2PSeedingOwner::run_loop, this);
    LOG_INFO("P2PSeedingOwner: started listen=" + config_.listen_interfaces +
             " actual_port=" + std::to_string(session_->listen_port()) +
             " max_active_seeds=" + std::to_string(kMaxActiveSeeds) +
             " connections_limit=" +
             std::to_string(config_.connections_limit > 0 ? config_.connections_limit : 32));
    return true;
}

void P2PSeedingOwner::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (!session_) {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        return;
    }
    // L3：session 生命周期收尾全部在本函数内完成——先逐个 remove_torrent
    // （保留文件），再直接 reset（~session 同步收尾并通知 tracker stopped）。
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& entry : registry_) {
            if (entry.handle.is_valid()) {
                session_->remove_torrent(entry.handle);
            }
        }
        if (!registry_.empty()) {
            counters_.stop_shutdown += static_cast<int>(registry_.size());
            LOG_INFO("P2PSeedingOwner: shutdown cleared seeds count=" +
                     std::to_string(registry_.size()));
        }
        registry_.clear();
        queue_.clear();
        counters_.active_seeds = 0;
        running_ = false;
    }
    session_.reset();
    LOG_INFO("P2PSeedingOwner: stopped");
}

void P2PSeedingOwner::Admit(P2PSeedCandidate candidate) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_ || stop_requested_) {
        ++counters_.rejected_not_running;
        LOG_WARN("P2PSeedingOwner: handoff dropped, owner not running id=" +
                 short_log_id(candidate.dedupe_key()));
        return;
    }
    if (queue_.size() >= kSeedQueueDepth) {
        ++counters_.rejected_queue_full;
        LOG_WARN("P2PSeedingOwner: handoff queue full, candidate dropped id=" +
                 short_log_id(candidate.dedupe_key()));
        return;
    }
    queue_.push_back(std::move(candidate));
}

void P2PSeedingOwner::on_network_changed(NetworkType type) {
    // D6：回调内只 snapshot；libtorrent 调用一律在 owner 线程锁外进行。
    std::lock_guard<std::mutex> lock(mu_);
    network_type_ = type;
}

#ifdef DEVICE_AGENT_TESTING

P2PSeedingOwner::Counters P2PSeedingOwner::counters_for_test() const {
    std::lock_guard<std::mutex> lock(mu_);
    Counters snapshot = counters_;
    snapshot.active_seeds = static_cast<int>(registry_.size());
    return snapshot;
}

std::vector<std::string> P2PSeedingOwner::active_info_hashes_for_test() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(registry_.size());
    for (const auto& entry : registry_) {
        ids.push_back(short_log_id(entry.candidate.dedupe_key()));
    }
    return ids;
}

std::vector<P2PSeedingOwner::ThrottleSample>
P2PSeedingOwner::active_throttles_for_test() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ThrottleSample> samples;
    samples.reserve(registry_.size());
    for (const auto& entry : registry_) {
        samples.push_back(ThrottleSample{entry.applied_max_uploads,
                                         entry.applied_upload_limit});
    }
    return samples;
}

bool P2PSeedingOwner::is_running_for_test() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_;
}

int P2PSeedingOwner::listen_port_for_test() const {
    // session_handle 接口线程安全（内部 post 到 session 线程）。
    return session_ ? static_cast<int>(session_->listen_port()) : 0;
}

#endif  // DEVICE_AGENT_TESTING

void P2PSeedingOwner::run_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(kSeedPollIntervalSeconds),
                         [this] { return stop_requested_; });
            if (stop_requested_) {
                return;
            }
        }
        tick();
    }
}

void P2PSeedingOwner::tick() {
    const P2PSeedingPolicy policy = live_policy();

    tick_policy_hard_stop(policy);
    tick_admissions(policy);
    tick_samples_and_expiry();
    apply_throttles(policy, false);
}

// seeding_enabled=false：拒绝新 candidate 并清空现有 seeds（D6 硬 stop）。
void P2PSeedingOwner::tick_policy_hard_stop(const P2PSeedingPolicy& policy) {
    std::vector<lt::torrent_handle> to_remove;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (policy.seeding_enabled || registry_.empty()) {
            return;
        }
        for (auto& entry : registry_) {
            if (entry.handle.is_valid()) {
                to_remove.push_back(entry.handle);
            }
        }
    }
    for (const auto& handle : to_remove) {
        session_->remove_torrent(handle);  // 无 delete_files，保留 artifact
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        const int removed = static_cast<int>(registry_.size());
        counters_.stop_policy += removed;
        counters_.active_seeds = 0;
        registry_.clear();
        LOG_INFO("P2PSeedingOwner: seeding_enabled=false hard stop, cleared " +
                 std::to_string(removed) + " seeds");
    }
}

void P2PSeedingOwner::tick_admissions(const P2PSeedingPolicy& policy) {
    std::deque<P2PSeedCandidate> incoming;
    {
        std::lock_guard<std::mutex> lock(mu_);
        incoming.swap(queue_);
    }

    for (auto& candidate : incoming) {
        const std::string key = candidate.dedupe_key();
        if (!candidate.torrent || key.empty() ||
                candidate.torrent->files().num_files() != 1) {
            std::lock_guard<std::mutex> lock(mu_);
            ++counters_.add_failed;
            LOG_WARN("P2PSeedingOwner: invalid candidate dropped (non-single-file "
                     "or missing metadata)");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_ || stop_requested_) {
                ++counters_.rejected_not_running;
                continue;
            }
            const bool duplicate = std::any_of(
                registry_.begin(), registry_.end(),
                [&](const SeedEntry& entry) {
                    return entry.candidate.dedupe_key() == key;
                });
            if (duplicate) {
                ++counters_.duplicate;  // 幂等：不延长原 deadline（D3）
                LOG_INFO("P2PSeedingOwner: duplicate seed candidate id=" +
                         short_log_id(key));
                continue;
            }
            if (!policy.seeding_enabled) {
                ++counters_.rejected_policy;
                continue;
            }
        }

        const std::string artifact = candidate_file_path(candidate);
        if (artifact.empty() || !file_readable(artifact)) {
            std::lock_guard<std::mutex> lock(mu_);
            ++counters_.stop_file_missing;
            LOG_WARN("P2PSeedingOwner: candidate artifact missing, dropped id=" +
                     short_log_id(key));
            continue;
        }

        // 满额：按最早 admitted_at FIFO 淘汰（D3）。
        if (registry_size() >= kMaxActiveSeeds) {
            std::string evict_key;
            lt::torrent_handle evict_handle;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto oldest = std::min_element(
                    registry_.begin(), registry_.end(),
                    [](const SeedEntry& a, const SeedEntry& b) {
                        return a.candidate.admitted_at < b.candidate.admitted_at;
                    });
                if (oldest != registry_.end()) {
                    evict_key = oldest->candidate.dedupe_key();
                    evict_handle = oldest->handle;
                }
            }
            if (evict_handle.is_valid()) {
                session_->remove_torrent(evict_handle);
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                registry_.erase(
                    std::remove_if(registry_.begin(), registry_.end(),
                                   [&](const SeedEntry& entry) {
                                       return entry.candidate.dedupe_key() == evict_key;
                                   }),
                    registry_.end());
                ++counters_.evicted_capacity;
                LOG_INFO("P2PSeedingOwner: capacity eviction id=" +
                         short_log_id(evict_key) + " active=" +
                         std::to_string(registry_.size()));
            }
        }

        lt::add_torrent_params params;
        params.ti = candidate.torrent;
        params.save_path = candidate.save_path;
        params.flags |= lt::torrent_flags::seed_mode;
        lt::error_code ec;
        lt::torrent_handle handle = session_->add_torrent(std::move(params), ec);
        if (ec || !handle.is_valid()) {
            std::lock_guard<std::mutex> lock(mu_);
            ++counters_.add_failed;  // D9：只记 telemetry，不影响前台 release
            LOG_WARN("P2PSeedingOwner: add_torrent failed id=" +
                     short_log_id(key) + " error=" + ec.message());
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            SeedEntry entry;
            entry.candidate = std::move(candidate);
            entry.handle = handle;
            registry_.push_back(std::move(entry));
            ++counters_.admitted;
            counters_.active_seeds = static_cast<int>(registry_.size());
            LOG_INFO("P2PSeedingOwner: seed admitted id=" + short_log_id(key) +
                     " active=" + std::to_string(registry_.size()));
        }
        apply_throttles(policy, true);
    }
}

void P2PSeedingOwner::tick_samples_and_expiry() {
    struct Snapshot {
        std::string key;
        lt::torrent_handle handle;
        std::chrono::steady_clock::time_point admitted_at;
        std::chrono::seconds ttl;
        double ratio_limit;
    };
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshots.reserve(registry_.size());
        for (const auto& entry : registry_) {
            snapshots.push_back(Snapshot{entry.candidate.dedupe_key(),
                                         entry.handle,
                                         entry.candidate.admitted_at,
                                         entry.candidate.ttl,
                                         entry.candidate.ratio_limit});
        }
    }

    NetworkType network = NetworkType::NONE;
    {
        std::lock_guard<std::mutex> lock(mu_);
        network = network_type_;
    }

    struct Removal {
        std::string key;
        StopReason reason;
    };
    std::vector<Removal> removals;
    std::vector<std::pair<std::string, std::int64_t>> samples;

    for (const auto& snap : snapshots) {
        if (!snap.handle.is_valid()) {
            removals.push_back(Removal{snap.key, StopReason::session_error});
            continue;
        }
        const lt::torrent_status status = snap.handle.status();
        const std::int64_t all_time_upload =
            static_cast<std::int64_t>(status.all_time_upload);
        samples.emplace_back(snap.key, all_time_upload);

        // 每 seed 独立 delta 累加到进程级计数（D8；禁用单一 last_upload_sample_）。
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& entry : registry_) {
                if (entry.candidate.dedupe_key() == snap.key) {
                    const std::int64_t delta = all_time_upload - entry.last_all_time_upload;
                    if (delta > 0) {
                        accumulate_p2p_upload(delta, network);
                        entry.last_all_time_upload = all_time_upload;
                    } else if (all_time_upload < entry.last_all_time_upload) {
                        entry.last_all_time_upload = all_time_upload;
                    }
                    break;
                }
            }
        }

        const double share_ratio_value =
            status.total_wanted > 0
                ? static_cast<double>(all_time_upload) /
                      static_cast<double>(status.total_wanted)
                : 0.0;
        if (snap.ttl.count() > 0 &&
                std::chrono::steady_clock::now() - snap.admitted_at >= snap.ttl) {
            removals.push_back(Removal{snap.key, StopReason::ttl});
            continue;
        }
        if (snap.ratio_limit > 0.0 && share_ratio_value >= snap.ratio_limit) {
            removals.push_back(Removal{snap.key, StopReason::ratio});
            continue;
        }
        const std::string artifact = candidate_file_path(
            // candidate 不在快照里，取注册表原件比对文件存在性。
            *find_candidate(snap.key));
        if (artifact.empty() || !file_readable(artifact)) {
            removals.push_back(Removal{snap.key, StopReason::file_missing});
            continue;
        }
    }

    if (removals.empty()) {
        return;
    }
    for (const auto& removal : removals) {
        lt::torrent_handle handle;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& entry : registry_) {
                if (entry.candidate.dedupe_key() == removal.key) {
                    handle = entry.handle;
                    break;
                }
            }
        }
        if (handle.is_valid()) {
            session_->remove_torrent(handle);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::size_t before = registry_.size();
            registry_.erase(
                std::remove_if(registry_.begin(), registry_.end(),
                               [&](const SeedEntry& entry) {
                                   return entry.candidate.dedupe_key() == removal.key;
                               }),
                registry_.end());
            if (registry_.size() != before) {
                counters_.active_seeds = static_cast<int>(registry_.size());
                switch (removal.reason) {
                    case StopReason::ttl:
                        ++counters_.stop_ttl;
                        break;
                    case StopReason::ratio:
                        ++counters_.stop_ratio;
                        break;
                    case StopReason::file_missing:
                        ++counters_.stop_file_missing;
                        break;
                    case StopReason::session_error:
                        ++counters_.stop_session_error;
                        break;
                    default:
                        ++counters_.stop_session_error;
                        break;
                }
                LOG_INFO("P2PSeedingOwner: seed stopped reason=" +
                         std::to_string(static_cast<int>(removal.reason)) +
                         " id=" + short_log_id(removal.key) + " active=" +
                         std::to_string(registry_.size()));
            }
        }
    }
}

std::size_t P2PSeedingOwner::registry_size() const {
    // owner 线程是 registry_ 唯一写者（Stop 在 join 之后才清空），此只读
    // 无需 mu_；跨线程读者一律走 mu_（test hooks）。
    return registry_.size();
}

const P2PSeedCandidate* P2PSeedingOwner::find_candidate(
        const std::string& dedupe_key) const {
    for (const auto& entry : registry_) {
        if (entry.candidate.dedupe_key() == dedupe_key) {
            return &entry.candidate;
        }
    }
    return nullptr;
}

void P2PSeedingOwner::apply_throttles(const P2PSeedingPolicy& policy, bool force) {
    NetworkType network = NetworkType::NONE;
    {
        std::lock_guard<std::mutex> lock(mu_);
        network = network_type_;
    }
    const bool allowed = seeding_allowed_on_network(network, policy);
    const int total_limit = upload_limit_bytes_per_second(policy);
    const int max_uploads = allowed ? max_upload_peers_for_policy(policy)
                                    : kCellularSuppressedMaxUploads;
    std::size_t active_count = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_count = registry_.size();
    }
    // L2：聚合上限按 active seed 数分摊（cap=2 → 各 1/2），保证总和 ≤ 上限；
    // total=0（unlimited）时 share=0 保持不限速。
    const int share_limit = !allowed
        ? kCellularSuppressedUploadLimitBytesPerSecond
        : (total_limit > 0
               ? std::max(1, total_limit / static_cast<int>(std::max<std::size_t>(active_count, 1)))
               : 0);

    struct Update {
        lt::torrent_handle handle;
        int limit;
        int max_uploads_value;
        std::string key;
    };
    std::vector<Update> updates;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& entry : registry_) {
            if (!force && entry.applied_upload_limit == share_limit &&
                    entry.applied_max_uploads == max_uploads) {
                continue;
            }
            updates.push_back(Update{entry.handle, share_limit, max_uploads,
                                     entry.candidate.dedupe_key()});
        }
    }
    for (const auto& update : updates) {
        if (!update.handle.is_valid()) {
            continue;
        }
        update.handle.set_max_uploads(update.max_uploads_value);
        update.handle.set_upload_limit(update.limit);
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& entry : registry_) {
                if (entry.candidate.dedupe_key() == update.key) {
                    entry.applied_upload_limit = update.limit;
                    entry.applied_max_uploads = update.max_uploads_value;
                    break;
                }
            }
        }
        LOG_INFO("P2PSeedingOwner: throttle applied id=" + short_log_id(update.key) +
                 " max_uploads=" + std::to_string(update.max_uploads_value) +
                 " upload_limit=" + std::to_string(update.limit) +
                 " network=" + std::to_string(static_cast<int>(network)));
    }
}

}  // namespace device_agent
