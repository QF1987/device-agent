#include "download/network_policy.h"

#include <cassert>
#include <vector>

namespace {

class RecordingListener : public device_agent::NetworkPolicy::Listener {
public:
    void on_network_changed(device_agent::NetworkType type) override {
        events.push_back(type);
    }

    std::vector<device_agent::NetworkType> events;
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

    return 0;
}
