#include "config/p2p_config_store.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using device_agent::P2PConfigSnapshot;
using device_agent::P2PConfigStore;

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    P2PConfigStore store;
    auto defaults = store.snapshot();
    expect(defaults.seeding_ttl_seconds == 21600, "default ttl mismatch");
    expect(defaults.max_share_ratio == 1.0, "default ratio mismatch");
    expect(!defaults.cellular_seeding_enabled, "default cellular mismatch");
    expect(defaults.max_upload_kbps == 0, "default max upload mismatch");

    std::string error;
    const bool ok = store.apply(
        R"({"kind":"p2p_seeding","seeding_ttl_seconds":7200,"max_share_ratio":1.5,"cellular_seeding_enabled":true,"max_upload_kbps":128})",
        &error);
    expect(ok, "apply valid config failed: " + error);
    auto snapshot = store.snapshot();
    expect(snapshot.seeding_ttl_seconds == 7200, "updated ttl mismatch");
    expect(snapshot.max_share_ratio == 1.5, "updated ratio mismatch");
    expect(snapshot.cellular_seeding_enabled, "updated cellular mismatch");
    expect(snapshot.max_upload_kbps == 128, "updated max upload mismatch");

    const bool invalid = store.apply(R"({"seeding_ttl_seconds":0})", &error);
    expect(!invalid, "invalid ttl was accepted");
    snapshot = store.snapshot();
    expect(snapshot.seeding_ttl_seconds == 7200, "invalid config mutated snapshot");

    auto shared = std::make_shared<P2PConfigStore>();
    expect(shared->apply(R"({"seeding_ttl_seconds":3600,"max_share_ratio":2.0})", &error),
           "apply global config failed");
    P2PConfigStore::set_global(shared);
    auto global = P2PConfigStore::global_snapshot();
    expect(global.seeding_ttl_seconds == 3600, "global ttl mismatch");
    expect(global.max_share_ratio == 2.0, "global ratio mismatch");
    P2PConfigStore::set_global(nullptr);
    global = P2PConfigStore::global_snapshot();
    expect(global.seeding_ttl_seconds == 21600, "cleared global did not return defaults");

    return 0;
}
