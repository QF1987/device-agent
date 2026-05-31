#include "config/p2p_config_store.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace device_agent {
namespace {

std::mutex g_store_mu;
std::shared_ptr<P2PConfigStore> g_store;
constexpr int kDefaultSeedingTTLSeconds = 21600;

bool extract_number_token(const std::string& json, const std::string& key, std::string& out) {
    const std::string q = "\"" + key + "\"";
    const auto pos = json.find(q);
    if (pos == std::string::npos) {
        return false;
    }
    const auto colon = json.find(':', pos);
    if (colon == std::string::npos) {
        return false;
    }
    auto start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    auto end = start;
    while (end < json.size()) {
        const char c = json[end];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.')) {
            break;
        }
        ++end;
    }
    if (end == start) {
        return false;
    }
    out = json.substr(start, end - start);
    return true;
}

bool extract_bool(const std::string& json, const std::string& key, bool& out) {
    const std::string q = "\"" + key + "\"";
    const auto pos = json.find(q);
    if (pos == std::string::npos) {
        return false;
    }
    const auto colon = json.find(':', pos);
    if (colon == std::string::npos) {
        return false;
    }
    auto start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    if (json.compare(start, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(start, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool extract_int(const std::string& json, const std::string& key, int& out) {
    std::string token;
    if (!extract_number_token(json, key, token)) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool extract_double(const std::string& json, const std::string& key, double& out) {
    std::string token;
    if (!extract_number_token(json, key, token)) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

int env_seeding_ttl_seconds() {
    const char* value = std::getenv("P2P_SEEDING_TTL");
    if (value == nullptr || *value == '\0') {
        return kDefaultSeedingTTLSeconds;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
        return kDefaultSeedingTTLSeconds;
    }
    return static_cast<int>(parsed);
}

P2PConfigSnapshot default_snapshot() {
    P2PConfigSnapshot snapshot;
    snapshot.seeding_ttl_seconds = env_seeding_ttl_seconds();
    return snapshot;
}

}  // namespace

P2PConfigStore::P2PConfigStore() : snapshot_(default_snapshot()) {}

bool P2PConfigStore::apply(const std::string& json, std::string* error) {
    P2PConfigSnapshot next;
    {
        std::lock_guard<std::mutex> lock(mu_);
        next = snapshot_;
    }

    bool saw_field = false;
    int ttl = 0;
    if (extract_int(json, "seeding_ttl_seconds", ttl)) {
        if (ttl <= 0) {
            set_error(error, "invalid p2p config: seeding_ttl_seconds must be > 0");
            return false;
        }
        next.seeding_ttl_seconds = ttl;
        saw_field = true;
    }
    double ratio = 0;
    if (extract_double(json, "max_share_ratio", ratio)) {
        if (ratio <= 0) {
            set_error(error, "invalid p2p config: max_share_ratio must be > 0");
            return false;
        }
        next.max_share_ratio = ratio;
        saw_field = true;
    }
    bool cellular = false;
    if (extract_bool(json, "cellular_seeding_enabled", cellular)) {
        next.cellular_seeding_enabled = cellular;
        saw_field = true;
    }
    int max_upload = 0;
    if (extract_int(json, "max_upload_kbps", max_upload)) {
        if (max_upload < 0) {
            set_error(error, "invalid p2p config: max_upload_kbps must be >= 0");
            return false;
        }
        next.max_upload_kbps = max_upload;
        saw_field = true;
    }
    if (!saw_field) {
        set_error(error, "invalid p2p config: no known fields");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot_ = next;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

P2PConfigSnapshot P2PConfigStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
}

void P2PConfigStore::set_global(std::shared_ptr<P2PConfigStore> store) {
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_store = std::move(store);
}

P2PConfigSnapshot P2PConfigStore::global_snapshot() {
    std::shared_ptr<P2PConfigStore> store;
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        store = g_store;
    }
    if (!store) {
        return default_snapshot();
    }
    return store->snapshot();
}

}  // namespace device_agent
