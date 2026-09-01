#include "download/network_policy.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
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

// 阻塞门 listener：进入回调后等待 release，用于确定性构造 in-flight 批次。
class GatedListener : public device_agent::NetworkPolicy::Listener {
public:
    void on_network_changed(device_agent::NetworkType) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++entered_;
        }
        entered_cv_.notify_all();
        std::unique_lock<std::mutex> lock(mu_);
        gate_cv_.wait_for(lock, std::chrono::seconds(10),
                          [this] { return released_; });
        ++completed_;
        completed_cv_.notify_all();
    }

    bool wait_entered(int expected) {
        std::unique_lock<std::mutex> lock(mu_);
        return entered_cv_.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered_ >= expected; });
    }

    bool wait_completed(int expected) {
        std::unique_lock<std::mutex> lock(mu_);
        return completed_cv_.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return completed_ >= expected; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            released_ = true;
        }
        gate_cv_.notify_all();
    }

    int entered() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entered_;
    }

    int completed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return completed_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable entered_cv_;
    std::condition_variable completed_cv_;
    std::condition_variable gate_cv_;
    int entered_ = 0;
    int completed_ = 0;
    bool released_ = false;
};

// 回调内自摘除：验证 remove 屏障的本线程豁免分支不死锁。
class SelfRemovingListener : public device_agent::NetworkPolicy::Listener {
public:
    explicit SelfRemovingListener(device_agent::NetworkPolicy& policy)
        : policy_(policy) {}

    void on_network_changed(device_agent::NetworkType) override {
        policy_.remove_listener(this);
        removed_.fetch_add(1);
    }

    std::atomic<int> removed_{0};

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

    // ---- 确定性 unsubscribe 生命周期屏障（RV-20260901-WIN-P2P-B5-02）----
    // 回调已取得 snapshot 并阻塞时，remove_listener 必须等待 in-flight 批次
    // 完成；返回后绝不再有回调进入该 listener，此后析构安全。
    {
        NetworkPolicy barrier_policy;
        GatedListener gated;
        barrier_policy.add_listener(&gated);

        // 回调线程进入 GatedListener::on_network_changed 后阻塞在 gate。
        auto dispatcher = std::async(std::launch::async, [&barrier_policy] {
            barrier_policy.on_network_changed(NetworkType::WIFI);
        });
        assert(gated.wait_entered(1));  // 指针已拷贝、回调 in-flight

        std::atomic<bool> remove_started{false};
        auto remover = std::async(std::launch::async, [&] {
            remove_started.store(true);
            barrier_policy.remove_listener(&gated);
        });
        // 等 remover 确实跑起来（started 置位后断言才有方向性）。
        for (int i = 0; i < 100 && !remove_started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(remove_started.load());
        // 负向确定性：回调阻塞期间 remove 不得返回（屏障破坏则立即完成）。
        assert(remover.wait_for(std::chrono::milliseconds(300)) ==
               std::future_status::timeout);
        assert(gated.completed() == 0);

        // 释放回调 → 批次完成 → remove 返回。
        gated.release();
        assert(gated.wait_completed(1));
        assert(remover.wait_for(std::chrono::seconds(5)) ==
               std::future_status::ready);
        remover.get();
        dispatcher.get();

        // remove 返回后再派发：不得进入该 listener（此后析构安全）。
        barrier_policy.on_network_changed(NetworkType::CELLULAR);
        assert(gated.entered() == 1);
        assert(gated.completed() == 1);
    }

    // ---- 回调内自摘除：本线程豁免屏障，不死锁 ----
    {
        NetworkPolicy self_policy;
        SelfRemovingListener self_listener(self_policy);
        self_policy.add_listener(&self_listener);
        auto self_dispatch = std::async(std::launch::async, [&self_policy] {
            self_policy.on_network_changed(NetworkType::WIFI);
        });
        assert(self_dispatch.wait_for(std::chrono::seconds(5)) ==
               std::future_status::ready);
        self_dispatch.get();
        assert(self_listener.removed_.load() == 1);
        // 自摘除后单线程再派发不进入（无其它线程批次，作用域结束析构安全）。
        self_policy.on_network_changed(NetworkType::CELLULAR);
        assert(self_listener.removed_.load() == 1);
    }

    return 0;
}
