#include "download/network_policy.h"

#include <algorithm>

namespace device_agent {

bool NetworkPolicy::should_seed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_type_ == NetworkType::WIFI;
}

void NetworkPolicy::on_network_changed(NetworkType type) {
    std::vector<Listener*> listeners;
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_type_ = type;
        listeners = listeners_;
    }

    for (auto* listener : listeners) {
        if (listener != nullptr) {
            listener->on_network_changed(type);
        }
    }
}

void NetworkPolicy::add_listener(Listener* listener) {
    if (listener == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
        listeners_.push_back(listener);
    }
}

void NetworkPolicy::remove_listener(Listener* listener) {
    std::lock_guard<std::mutex> lock(mu_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                     listeners_.end());
}

}  // namespace device_agent
