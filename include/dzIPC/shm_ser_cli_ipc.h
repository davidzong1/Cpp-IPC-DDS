#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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

namespace threepools {
/* 阶段 5 共享层：SHM 固定 route 收包 worker 的 route 抽象
 * （include/dzIPC/threepools/recv_worker.h，由 ipc-transport 交付并冻结）。
 * 公开头只做前置声明，真正的 include 在 .cc —— 只 include 本头的消费者不必为此
 * 被拉进等待层（与 include/dzIPC/socket_ser_cli_ipc.h 同一写法）。 */
class RecvRouteSource;
}   // namespace threepools

namespace shm {
class shm_ser_ipc;
class shm_cli_ipc;

/* 阶段 5：服务端请求接收的 service shared state 与 route 适配器（**定义在 .cc**）。
 *
 * 收包 worker 只通过 SerRequestRoute 接触本状态，**不持**裸 shm_ser_ipc 指针
 * —— 这是"注销后不再回调已析构对象"的前提（需求 §3.2 / 契约 §4.1）。
 * 两者都只在本模块内部使用，不进对外导出面，因此这里只做前置声明。 */
struct SerState;
class SerRequestRoute;

/* 服务通道控制面段名("_ser_control2")。
 *
 * T3 补充: 占用判定(autopath::shm_channel_occupied)需要**只读**探测这个段
 * (见 control_plane_shm::occupied_by_other), 而段名拼接的唯一出处是
 * shm_ser_cli_ipc.cc —— 在这里导出它, 避免出现第二份字符串规则。 */
IPC_EXPORT std::string ser_service_control_name(const std::string& topic_name, size_t domain_id);

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
    path::Kind transport_current() const override { return path::Kind::Shm; }

    /* 禁用拷贝 */
    shm_ser_ipc(const shm_ser_ipc&) = delete;
    shm_ser_ipc& operator=(const shm_ser_ipc&) = delete;

protected:
    void response_thread_func();
    void ser_handshake();

    /* ---- 阶段 5：请求处理路径（worker 模式与兼容模式**共用同一份语义**）----
     *
     * ⛔ 用户 callback 与响应序列化/发送**只在这里**执行。共享收包 worker 的
     * recv_once() 内没有它们（需求 §1.2；recv_worker.h 对 recv_once 的 ⛔）。
     * callback 首期保持每 server 串行：本函数只由本 server 的处理线程调用。 */
    void process_request(const std::shared_ptr<SerState>& state, ipc::buffer raw_data);
    /* nodelet 进程内快路径：**兼容模式专属**（worker 模式下 InitChannel 不注册
     * fp_queue，见 .cc 文件头"nodelet 与 worker 二选一"）。返回 true = 已消化掉
     * 这个 item（与改造前逐条同序）；⛔ 同样不在共享 worker 里跑 callback。 */
    bool handle_fast_path(const std::shared_ptr<SerState>& state, IpcMsgBase& envelope_item);
    /* worker 模式的处理线程主体：只从有界 FIFO 取**完整请求** → process_request。 */
    void process_thread_func();
    /* 响应序列化 + 发送（DZFlat 借样优先，失败回退整包；与基准实现同一份代码）。 */
    void send_response(const std::shared_ptr<SerState>& state, const std::shared_ptr<ServiceData>& local_msg);

    /* 数据面接入：返回 true = 本次在共享 worker 上收包；false = **必须**走兼容
     * response_thread_func，并且已经在 stderr 打了显式原因（绝不静默回退）。
     * 两条路径都会建好 ser_state_/req_route_，因此注销协议（含 release_recv）
     * 对两条路径都成立。 */
    bool start_data_plane();
    /* 数据面注销：方案 §4 的第 2–6 步 + FIFO 作废记账。幂等。 */
    void stop_data_plane() noexcept;
    /* ser_state_ 的线程安全读口（处理线程与注销路径都用它，避免与
     * reset_message/reset_callback 的写入竞争）。 */
    std::shared_ptr<SerState> current_ser_state() const;

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

    /* ---- 阶段 5：固定 SHM worker 接入面 ----
     * worker 本体由共享层提供（只消费、不复制、不加平台宏）。worker 模式下
     * **只有** ipc_r_ptr_（请求数据通道）注册进池：ipc_w_ptr_ 是发送端点、
     * 控制面段与数据面分离，都不入组。 */
    std::shared_ptr<SerState> ser_state_;
    std::shared_ptr<SerRequestRoute> req_route_;
    /* true = 数据面在共享 worker 上（处理线程消费 FIFO，无 per-server 接收线程）；
     * false = 兼容后端（nodelet / fork 闸 / 后端不可用 / 注册失败）。 */
    bool worker_mode_{false};
    /* 数据面代际：每次接入 +1（诊断与固定归属日志用；服务端**不做** route 重建，
     * generation 只在控制面段单调递增）。 */
    std::atomic<std::uint32_t> receive_generation_{0};
    /* 只保护 ser_state_ 这个 shared_ptr 本身的读写（reset_message/reset_callback
     * 与注销路径并发）。⛔ 绝不在此锁内再取 SerState 的内部锁，避免锁序环。 */
    mutable std::mutex state_mtx_;
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
    path::Kind transport_current() const override { return path::Kind::Shm; }

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
