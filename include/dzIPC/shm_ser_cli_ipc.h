#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/ser_cli_base.h"
#include "libipc/ipc.h"

namespace dzIPC {
namespace shm {
class shm_ser_ipc;
class shm_cli_ipc;

class IPC_EXPORT shm_ser_ipc : public ser_ipc_base
{
public:
    explicit shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                         bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                         int thread_priority = 0);
    ~shm_ser_ipc();
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback);
    void InitChannel(std::string extra_info = "");

    bool handshake_completed() const override { return handshake_completed_.load(std::memory_order_acquire); }

    /* 禁用拷贝 */
    shm_ser_ipc(const shm_ser_ipc&) = delete;
    shm_ser_ipc& operator=(const shm_ser_ipc&) = delete;

protected:
    void response_thread_func();
    void ser_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
    bool verbose_{true};
    std::function<void(std::shared_ptr<ServiceData>&)> callback_;
    std::mutex callback_mtx_;
    std::string topic_name_;
    std::shared_ptr<ServiceData> message_;
    std::mutex message_mtx_;
    std::thread* response_thread_{nullptr};
    std::thread* handshake_thread_{nullptr};
    std::shared_ptr<ipc::server> ipc_r_ptr_;
    std::shared_ptr<ipc::server> ipc_w_ptr_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;

    // Nodelet fast-path: server-side request queue for same-process delivery.
    // Registered in LocalPubSubRegistry so clients can discover it.
    std::shared_ptr<CircularQueue<IpcMsgBase>> fp_queue_;
    bool fp_registered_{false};  // true after InitChannel registers in registry
    uint32_t fp_msg_id_{0};      // msg_id used for current registration
};

class IPC_EXPORT shm_cli_ipc : public cli_ipc_base
{
public:
    explicit shm_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                         bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                         int thread_priority = 0);
    ~shm_cli_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm = std::numeric_limits<uint32_t>::max());
    /* 禁用拷贝 */
    shm_cli_ipc(const shm_cli_ipc&) = delete;
    shm_cli_ipc& operator=(const shm_cli_ipc&) = delete;

    bool handshake_completed() const override { return handshake_completed_.load(std::memory_order_acquire); }

protected:
    void cli_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
    bool verbose_{true};
    std::shared_ptr<ServiceData> message_;
    std::mutex message_mtx_;
    std::string topic_name_;
    std::shared_ptr<ipc::server> ipc_r_ptr_;
    std::shared_ptr<ipc::server> ipc_w_ptr_;
    std::mutex channel_mtx_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    std::thread* handshake_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;

    // Nodelet fast-path gating, serialized by fast_path_mtx_.
    // Registry only stores server request queues (snapshot size must be 1).
    // Client creates a per-request capacity-1 reply queue placed in the
    // FastPathRequestEnvelope — no persistent client queue in registry.
    // K=3 consecutive observations of a local server gates activation.
    mutable std::mutex fast_path_mtx_;
    ChannelKey last_fp_ser_key_{};
    size_t last_fp_ser_snapshot_size_{0};
    int fp_consecutive_{0};
    static constexpr int kFastPathConfirm = 3;

    // One-shot warning suppression per instance + per reason.
    // std::atomic exchange(true) serves as both check-and-set in one operation.
    mutable std::atomic<bool> nodelet_no_server_warned_{false};
    mutable std::atomic<bool> nodelet_anomaly_warned_{false};

    /* 本客户端在控制面 PeerSlot 表中的槽位下标, -1 表示未登记。
     * 由 cli_handshake() 线程独占访问。 */
    int peer_slot_{-1};
};
}   // namespace shm
}   // namespace dzIPC
