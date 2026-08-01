#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/pub_sub_base.h"
#include "libipc/count_sem.h"
#include "libipc/ipc.h"

namespace dzIPC {
namespace shm {
class shm_pub_ipc;
class shm_sub_ipc;

class IPC_EXPORT shm_pub_ipc : public pub_ipc_base
{
public:
    explicit shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                         int thread_priority = 0);
    ~shm_pub_ipc();
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void InitChannel(std::string extra_info = "");
    bool publish(std::shared_ptr<IpcMsgBase> msg);
    bool publish_best_effort(std::shared_ptr<IpcMsgBase> msg) override;
    bool publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm) override;
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg) override;

    bool has_subscribed() const { return subscribed_; }

    /* 禁用拷贝 */
    shm_pub_ipc(const shm_pub_ipc&) = delete;
    shm_pub_ipc& operator=(const shm_pub_ipc&) = delete;

private:
    void pub_handshake();
    // void sub_listener();

private:
    size_t domain_id_{0};
    std::atomic<bool> subscribed_{false};
    bool verbose_{false};
    std::atomic<bool> running{true};
    std::string topic_name_;
    std::string raw_topic_name_;
    std::shared_ptr<ipc::route> publisher_;
    std::thread* publish_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    std::shared_ptr<TopicData> topic_msg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    // Fast-path state, serialized by fast_path_mtx_.
    // Key is rebuilt from msg->msg_id() on each publish; any change resets
    // the consecutive counter — K=3 is only a gating threshold.
    mutable std::mutex fast_path_mtx_;
    ChannelKey last_fp_key_{};
    size_t last_fp_snapshot_size_{0};
    size_t last_fp_recv_count_{0};
    int fp_consecutive_{0};
    static constexpr int kFastPathConfirm = 3;
    // Rate-limited fallback warning: one shot per reason per instance lifetime.
    // Bitmask tracks which reasons have already been emitted.
    // Atomic to allow lock-free test-and-set; once set a bit is never cleared.
    mutable std::atomic<uint8_t> nodelet_warned_{0};
};

class IPC_EXPORT shm_sub_ipc : public sub_ipc_base
{
public:
    explicit shm_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         const size_t queue_size, bool verbose = false, bool enable_thread_qos = false,
                         int cpu_id = -1, int thread_priority = 0);
    ~shm_sub_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void get(std::shared_ptr<TopicData>& msg);
    bool try_get(std::shared_ptr<TopicData>& msg);
    /* 禁用拷贝 */
    shm_sub_ipc(const shm_sub_ipc&) = delete;
    shm_sub_ipc& operator=(const shm_sub_ipc&) = delete;

private:
    void sub_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed{false};
    bool data_update_{false};
    bool verbose_{false};
    std::string topic_name_;
    std::string raw_topic_name_;
    std::shared_ptr<ipc::route> subscriber_;
    std::mutex channel_mtx_;
    std::shared_ptr<TopicData> topic_msg_;
    std::mutex topic_msg_mtx_;
    std::shared_ptr<CircularQueue<IpcMsgBase>> msg_queue_;  // shared_ptr for fast-path fanout
    std::thread* subscribe_thread_{nullptr};
    std::thread* sub_handshake_thread_{nullptr};
    //
    ipc::sync::count_sem* empty_queue_;   // 用于通知订阅者消息队列中有新消息
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    uint32_t msg_id_{0};  // current registration key msg_id; updated on reset_message
    bool local_registered_{false};  // guarded by topic_msg_mtx_; true after InitChannel registers
};
}   // namespace shm
}   // namespace dzIPC
