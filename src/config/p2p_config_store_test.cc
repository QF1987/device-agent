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
    unsetenv("P2P_SEEDING_TTL");
    P2PConfigStore store;
    auto defaults = store.snapshot();
    expect(defaults.seeding_ttl_seconds == 21600, "default ttl mismatch");
    expect(defaults.max_share_ratio == 1.0, "default ratio mismatch");
    expect(!defaults.cellular_seeding_enabled, "default cellular mismatch");
    expect(defaults.max_upload_kbps == 0, "default max upload mismatch");
    expect(defaults.p2p_enabled, "default p2p enabled mismatch");
    expect(defaults.seeding_enabled, "default seeding enabled mismatch");
    expect(defaults.max_upload_peers == 4, "default max upload peers mismatch");
    expect(defaults.lan_upload_enabled, "default lan upload mismatch");
    expect(!defaults.wan_upload_enabled, "default wan upload mismatch");
    expect(defaults.cellular_download_enabled, "default cellular download mismatch");
    expect(defaults.min_file_size_mb_for_p2p == 10, "default min file size mismatch");

    std::string error;
    setenv("P2P_SEEDING_TTL", "120", 1);
    P2PConfigStore env_store;
    auto env_defaults = env_store.snapshot();
    expect(env_defaults.seeding_ttl_seconds == 120, "env ttl override mismatch");
    expect(env_store.apply(R"({"max_share_ratio":1.25})", &error), "apply env store ratio failed");
    env_defaults = env_store.snapshot();
    expect(env_defaults.seeding_ttl_seconds == 120, "env ttl polluted by partial JSON apply");
    P2PConfigStore::set_global(nullptr);
    expect(!P2PConfigStore::has_global(), "global store should be empty");
    auto global_env_defaults = P2PConfigStore::global_snapshot();
    expect(global_env_defaults.seeding_ttl_seconds == 120, "global default env ttl mismatch");

    setenv("P2P_SEEDING_TTL", "invalid", 1);
    P2PConfigStore invalid_env_store;
    auto invalid_env_defaults = invalid_env_store.snapshot();
    expect(invalid_env_defaults.seeding_ttl_seconds == 21600, "invalid env ttl did not fall back");
    unsetenv("P2P_SEEDING_TTL");

    const bool ok = store.apply(
        R"({"kind":"p2p_seeding","seeding_ttl_seconds":7200,"max_share_ratio":1.5,"cellular_seeding_enabled":true,"max_upload_kbps":128})",
        &error);
    expect(ok, "apply valid config failed: " + error);
    auto snapshot = store.snapshot();
    expect(snapshot.seeding_ttl_seconds == 7200, "updated ttl mismatch");
    expect(snapshot.max_share_ratio == 1.5, "updated ratio mismatch");
    expect(snapshot.cellular_seeding_enabled, "updated cellular mismatch");
    expect(snapshot.max_upload_kbps == 128, "updated max upload mismatch");
    expect(snapshot.p2p_enabled, "old payload polluted p2p enabled");
    expect(snapshot.seeding_enabled, "old payload polluted seeding enabled");
    expect(snapshot.max_upload_peers == 4, "old payload polluted upload peers");

    const bool extended_ok = store.apply(
        R"({"kind":"p2p_seeding","p2p_enabled":false,"seeding_enabled":false,"max_upload_peers":2,"lan_upload_enabled":false,"wan_upload_enabled":true,"cellular_download_enabled":false,"min_file_size_mb_for_p2p":64})",
        &error);
    expect(extended_ok, "apply extended config failed: " + error);
    snapshot = store.snapshot();
    expect(!snapshot.p2p_enabled, "updated p2p enabled mismatch");
    expect(!snapshot.seeding_enabled, "updated seeding enabled mismatch");
    expect(snapshot.max_upload_peers == 2, "updated max upload peers mismatch");
    expect(!snapshot.lan_upload_enabled, "updated lan upload mismatch");
    expect(snapshot.wan_upload_enabled, "updated wan upload mismatch");
    expect(!snapshot.cellular_download_enabled, "updated cellular download mismatch");
    expect(snapshot.min_file_size_mb_for_p2p == 64, "updated min file size mismatch");

    const bool invalid = store.apply(R"({"seeding_ttl_seconds":0})", &error);
    expect(!invalid, "invalid ttl was accepted");
    snapshot = store.snapshot();
    expect(snapshot.seeding_ttl_seconds == 7200, "invalid config mutated snapshot");
    const bool invalid_extended = store.apply(R"({"max_upload_peers":-1})", &error);
    expect(!invalid_extended, "invalid max_upload_peers was accepted");
    snapshot = store.snapshot();
    expect(snapshot.max_upload_peers == 2, "invalid extended config mutated snapshot");

    auto shared = std::make_shared<P2PConfigStore>();
    expect(shared->apply(R"({"seeding_ttl_seconds":3600,"max_share_ratio":2.0})", &error),
           "apply global config failed");
    P2PConfigStore::set_global(shared);
    expect(P2PConfigStore::has_global(), "global store should be configured");
    auto global = P2PConfigStore::global_snapshot();
    expect(global.seeding_ttl_seconds == 3600, "global ttl mismatch");
    expect(global.max_share_ratio == 2.0, "global ratio mismatch");
    P2PConfigStore::set_global(nullptr);
    global = P2PConfigStore::global_snapshot();
    expect(global.seeding_ttl_seconds == 21600, "cleared global did not return defaults");

    return 0;
}
