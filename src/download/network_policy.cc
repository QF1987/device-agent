#include "download/network_policy.h"

#include <algorithm>

namespace device_agent {
namespace {

// 本线程是否处于 on_network_changed 派发批次内（支持嵌套）。回调内自摘除
// 自身时豁免 remove 屏障等待，避免自死锁（见 network_policy.h 生命周期契约）。
thread_local int t_callback_batch_depth = 0;

}  // namespace

bool NetworkPolicy::should_seed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_type_ == NetworkType::WIFI;
}

NetworkType NetworkPolicy::current_type() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_type_;
}

void NetworkPolicy::on_network_changed(NetworkType type) {
    std::vector<Listener*> listeners;
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_type_ = type;
        listeners = listeners_;
        ++in_flight_batches_;
    }
    ++t_callback_batch_depth;
    // RAII 收尾：无论回调是否抛异常，都递减批次计数并唤醒 remove 等待者。
    struct BatchExit {
        NetworkPolicy* self;
        ~BatchExit() {
            --t_callback_batch_depth;
            {
                std::lock_guard<std::mutex> lock(self->mu_);
                --self->in_flight_batches_;
            }
            self->callback_cv_.notify_all();
        }
    } batch_exit{this};

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
    {
        std::lock_guard<std::mutex> lock(mu_);
        listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                         listeners_.end());
    }
    // 生命周期屏障（RV-20260901-WIN-P2P-B5-02）：等待其它线程已拷贝出去的
    // in-flight 批次派发完成；返回后不会再有任何回调进入 listener，调用方
    // 可安全析构。本线程自身批次内（回调内自摘除）豁免等待避免自死锁，
    // 该场景的跨线程安全性由调用方契约保证（见 network_policy.h）。
    std::unique_lock<std::mutex> lock(mu_);
    if (t_callback_batch_depth == 0) {
        callback_cv_.wait(lock, [this] { return in_flight_batches_ == 0; });
    }
}

}  // namespace device_agent
