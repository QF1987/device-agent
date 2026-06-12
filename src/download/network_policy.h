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
    // 当前网络类型(P2P 上传分桶采样用,ADR-20260612-01 D2)
    NetworkType current_type() const;
    void on_network_changed(NetworkType type);
    void add_listener(Listener* listener);
    void remove_listener(Listener* listener);

private:
    mutable std::mutex mu_;
    NetworkType current_type_{NetworkType::NONE};
    std::vector<Listener*> listeners_;
};

}  // namespace device_agent
