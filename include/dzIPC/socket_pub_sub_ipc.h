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
    /* publish_blocking: "至少一个确认", 不是全部确认。
     *
     * 返回 true 当且仅当收到**任意一个**匹配本消息的 ACK —— chunk_send_ex
     * 首个匹配即判定 DeliveredAcked (data_rev.cc 的 got_ack 分支)。N 个订阅者
     * 时, true 只说明"至少有一个收到了", 不保证其余 N-1 个; false 说明等待
     * 窗口内一个都没收到, 或 CRC 校验失败。
     *
     * 防误用: 该接口无法表达"谁收到了、谁没收到"。全体确认需要 RTPS 的
     * Reader/Writer 配对与逐 Reader 确认状态, 本轮明确不做 (DECISIONS.md
     * D-3)。也不要用 IpcInfoPool 的"进程活着"当"正在收数据" —— 其 liveness
     * 是 kill(pid,0) (ipc_info_pool.cc:105-123), fork 入组慢 / socket 坏 /
     * pid 复用任一情形都会让基于它的全体确认永久超时。 */
    bool publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm) override;
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg) override;

    /* 是否已有订阅者。
     *
     * 与 SHM 的语义对齐(SHM 由 pub_handshake() 线程从控制面回填), socket 侧
     * 由 discovery_loop() 线程每 kDiscoveryPollMs 毫秒从 IpcInfoPool 统计本
     * topic/domain 下存活的 SocketSub 条目数回填。
     *
     * ---- 探测边界(与 nodelet 快路径相同) ----
     * IpcInfoPool 只能看到通过 dzIPC 自身 socket_sub_ipc::InitChannel 注册的
     * 订阅者。原生 UDP 监听程序、抓包工具、外部组播消费者对它不可见, 因此本
     * 接口返回 false 不代表组播上真的没有接收方。仅可用作"是否有 dzIPC 订阅
     * 者"的判据, 不可用作是否发送的开关。 */
    bool has_subscribed() const { return subscribed_; }

    bool client_subscribed() const { return cli_cnt; }

    /* 禁用拷贝 */
    socket_pub_ipc(const socket_pub_ipc&) = delete;
    socket_pub_ipc& operator=(const socket_pub_ipc&) = delete;

private:
    /* 订阅者发现线程: 周期性从 IpcInfoPool 回填 subscribed_。
     * UDP 组播没有反向发现通道, 只能靠这个进程外共享的信息池。 */
    void discovery_loop();
    static constexpr int kDiscoveryPollMs = 50;

    size_t domain_id_{0};
    int cli_cnt{0};
    std::atomic<bool> subscribed_{false};
    bool verbose_{false};
    std::atomic<bool> running{true};
    std::string topic_name_;
    std::shared_ptr<ipc::socket::UDPNode> publisher_;
    /* ACK 回传通道 (端点分离)。
     *
     * publisher_ 是 SendOnly: 不入组, 因此收不到自己发出去的分片回绕 —— 这是
     * Reliable 能工作的前提。但不入组也意味着它收不到订阅端的 ACK, 所以 ACK 走
     * 这条独立的 RecvOnly socket, 绑在 port_hash_ + kAckPortOffset 上。
     *
     * 只有 publish_blocking() 用到它; BestEffort 路径完全不碰。 */
    std::shared_ptr<ipc::socket::UDPNode> ack_rx_;
    uint16_t port_hash_;
    std::string ipaddr_;
    std::mutex sleep_mtx;
    std::condition_variable sleep_cv;
    std::thread* discovery_thread_{nullptr};
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
    /* ACK 发送通道 (端点分离): SendOnly, 绑在与发布端 ack_rx_ 相同的端口上。
     * subscriber_ 只收不发, ACK/NACK 从这条出去。 */
    std::shared_ptr<ipc::socket::UDPNode> ack_tx_;
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
