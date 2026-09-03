#pragma once

#include <condition_variable>
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
    // Listener 生命周期契约（RV-20260901-WIN-P2P-B5-02）：
    //   - 回调内只做 snapshot、不得长时间阻塞（D6；remove 屏障等待上限
    //     即回调时长）；
    //   - remove_listener 返回后（非回调内自摘除场景）保证不再有任何回调
    //     进入该 listener，此后可安全析构；
    //   - 回调内自摘除自身（remove_listener(this)）允许且不死锁，但此时
    //     其它线程的 in-flight 派发不由屏障覆盖，须由调用方先行同步才能
    //     析构。现有使用方（P2PDownloadManager / P2PSeedingOwner）均在
    //     析构路径 remove，不走该分支。
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
    // in-flight 回调批次屏障：on_network_changed 派发期间计数>0；
    // remove_listener 等待归零，杜绝"已拷贝指针、回调未返回"窗口。
    std::condition_variable callback_cv_;
    int in_flight_batches_{0};
    NetworkType current_type_{NetworkType::NONE};
    std::vector<Listener*> listeners_;
};

}  // namespace device_agent
