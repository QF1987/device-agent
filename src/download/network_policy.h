#pragma once

#include <mutex>
#include <vector>

namespace device_agent {

enum class NetworkType {
    NONE,
    WIFI,
    CELLULAR,
    OTHER,
};

class NetworkPolicy {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void on_network_changed(NetworkType type) = 0;
    };

    bool should_seed() const;
    void on_network_changed(NetworkType type);
    void add_listener(Listener* listener);
    void remove_listener(Listener* listener);

private:
    mutable std::mutex mu_;
    NetworkType current_type_{NetworkType::NONE};
    std::vector<Listener*> listeners_;
};

}  // namespace device_agent
