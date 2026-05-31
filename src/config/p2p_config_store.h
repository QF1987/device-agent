#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace device_agent {

struct P2PConfigSnapshot {
    int seeding_ttl_seconds = 21600;
    double max_share_ratio = 1.0;
    bool cellular_seeding_enabled = false;
    int max_upload_kbps = 0;
};

class P2PConfigStore {
public:
    P2PConfigStore();

    bool apply(const std::string& json, std::string* error = nullptr);
    P2PConfigSnapshot snapshot() const;

    static void set_global(std::shared_ptr<P2PConfigStore> store);
    static P2PConfigSnapshot global_snapshot();

private:
    mutable std::mutex mu_;
    P2PConfigSnapshot snapshot_;
};

}  // namespace device_agent
