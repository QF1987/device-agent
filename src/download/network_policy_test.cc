#include "download/network_policy.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <vector>

namespace {

class RecordingListener : public device_agent::NetworkPolicy::Listener {
public:
    void on_network_changed(device_agent::NetworkType type) override {
        events.push_back(type);
    }

    std::vector<device_agent::NetworkType> events;
};

class ReentrantListener : public device_agent::NetworkPolicy::Listener {
public:
    explicit ReentrantListener(device_agent::NetworkPolicy& policy) : policy_(policy) {}

    void on_network_changed(device_agent::NetworkType) override {
        (void)policy_.should_seed();
        calls.fetch_add(1);
    }

    std::atomic<int> calls{0};

private:
    device_agent::NetworkPolicy& policy_;
};

}  // namespace

int main() {
    using device_agent::NetworkPolicy;
    using device_agent::NetworkType;

    NetworkPolicy policy;
    assert(!policy.should_seed());
    policy.on_network_changed(NetworkType::WIFI);
    assert(policy.should_seed());
    policy.on_network_changed(NetworkType::CELLULAR);
    assert(!policy.should_seed());
    policy.on_network_changed(NetworkType::NONE);
    assert(!policy.should_seed());
    policy.on_network_changed(NetworkType::OTHER);
    assert(!policy.should_seed());

    RecordingListener listener;
    policy.add_listener(&listener);
    policy.on_network_changed(NetworkType::WIFI);
    assert(listener.events.size() == 1);
    assert(listener.events.back() == NetworkType::WIFI);

    policy.remove_listener(&listener);
    policy.on_network_changed(NetworkType::CELLULAR);
    assert(listener.events.size() == 1);

    NetworkPolicy concurrent_policy;
    ReentrantListener reentrant_listener(concurrent_policy);
    concurrent_policy.add_listener(&reentrant_listener);
    auto change_network = [&concurrent_policy](NetworkType first, NetworkType second) {
        for (int i = 0; i < 100; ++i) {
            concurrent_policy.on_network_changed((i % 2 == 0) ? first : second);
        }
    };
    auto first = std::async(std::launch::async, change_network,
                            NetworkType::WIFI, NetworkType::CELLULAR);
    auto second = std::async(std::launch::async, change_network,
                             NetworkType::NONE, NetworkType::OTHER);
    assert(first.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(second.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    first.get();
    second.get();
    assert(reentrant_listener.calls.load() == 200);

    return 0;
}
