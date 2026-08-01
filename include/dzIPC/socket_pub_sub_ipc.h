#pragma once
// #include <semaphore.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/pub_sub_base.h"
#include "libipc/udp.h"

namespace dzIPC {
namespace socket {
class socket_pub_ipc;
class socket_sub_ipc;

class IPC_EXPORT socket_pub_ipc : public pub_ipc_base
{
public:
    explicit socket_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                            bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                            int thread_priority = 20);
    ~socket_pub_ipc();
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void InitChannel(std::string extra_info = "");
    bool publish(std::shared_ptr<IpcMsgBase> msg);
    bool publish_best_effort(std::shared_ptr<IpcMsgBase> msg) override;
    bool publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm) override;
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg) override;

    bool has_subscribed() const { return subscribed_; }

    bool client_subscribed() const { return cli_cnt; }

    /* 禁用拷贝 */
    socket_pub_ipc(const socket_pub_ipc&) = delete;
    socket_pub_ipc& operator=(const socket_pub_ipc&) = delete;

private:
    size_t domain_id_{0};
    int cli_cnt{0};
    std::atomic<bool> subscribed_{false};
    bool verbose_{false};
    std::atomic<bool> running{true};
    std::string topic_name_;
    std::shared_ptr<ipc::socket::UDPNode> publisher_;
    uint16_t port_hash_;
    std::string ipaddr_;
    std::mutex sleep_mtx;
    std::condition_variable sleep_cv;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    std::shared_ptr<TopicData> topic_msg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;

    // ---- intra-process fast-path state ----
    // K=3 consecutive publishes with the same (key, local snapshot size,
    // IpcInfoPool SocketSub count) must be observed before the fast path
    // engages.  Any topology change resets the counter.
    // Cross-process subs are detected by comparing the IpcInfoPool total
    // SocketSub count (all processes) against the local registry count.
    mutable std::mutex fast_path_mtx_;
    ChannelKey last_fp_key_{};
    size_t last_fp_snapshot_size_{0};
    size_t last_fp_total_subs_{0};   // from IpcInfoPool snapshot
    int fp_consecutive_{0};
    static constexpr int kFastPathConfirm = 3;

    // One-shot warning throttles (per instance, per reason).
    bool warned_no_local_sub_{false};       // nodelet on but registry empty
    bool warned_cross_process_{false};      // cross-process SocketSub detected
    bool warned_pool_unavailable_{false};   // IpcInfoPool returned 0 total subs
};

class IPC_EXPORT socket_sub_ipc : public sub_ipc_base
{
public:
    explicit socket_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                            const size_t queue_size, bool verbose = false, bool enable_thread_qos = false,
                            int cpu_id = -1, int thread_priority = 20);
    ~socket_sub_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void get(std::shared_ptr<TopicData>& msg);
    bool try_get(std::shared_ptr<TopicData>& msg);
    /* 禁用拷贝 */
    socket_sub_ipc(const socket_sub_ipc&) = delete;
    socket_sub_ipc& operator=(const socket_sub_ipc&) = delete;

private:
    size_t domain_id_{0};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> running{true};
    bool data_update_{false};
    bool verbose_{false};
    std::string topic_name_;
    uint16_t port_hash_;
    std::string ipaddr_;
    std::shared_ptr<ipc::socket::UDPNode> subscriber_;
    std::shared_ptr<TopicData> topic_msg_;
    std::mutex topic_msg_mtx_;
    std::shared_ptr<CircularQueue<IpcMsgBase>> msg_queue_;  // shared_ptr for fast-path fanout
    std::thread* subscribe_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    uint32_t msg_id_{0};             // current registration key msg_id
    bool local_registered_{false};   // guarded by topic_msg_mtx_
};
}   // namespace socket
}   // namespace dzIPC
