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
#include "dzIPC/common/loaned_message.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/common/sample_message.h"
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

    /* ------------------------------------------------------------------ B 级借样
     *
     * loan<Flat>(varlen_budget) 借一块共享 chunk 并返回一个就地构造器: 大负载直接写
     * 进共享内存, 发布端零拷贝(docs/dzflat_shm.md §4.2)。
     *
     * **必须检查返回值**: 无接收方 / chunk 池耗尽(32 块/尺寸档位)/ 开关未开时返回无效
     * 对象, 调用方须回退到普通 publish()。这是背压而非错误。
     *
     * varlen_budget = 变长区(string / 数组 / 嵌套元素块)最多需要的字节数上界。超出后
     * alloc_* 返回空 span 且 publish_loaned 会失败并归还 chunk, 不会写出坏段。
     *
     * 用法见 loaned_message.h 顶部注释。
     */
    template<typename Flat>
    LoanedMessage<Flat> loan(std::uint32_t varlen_budget)
    {
        if (!dzIPC::IsDzFlatEnabled() || !publisher_)
        {
            return {};
        }
        auto lo = publisher_->loan(Flat::loan_size(varlen_budget));
        if (!lo.valid())
        {
            return {};
        }
        return LoanedMessage<Flat>{publisher_, lo, dzflat_msg_id()};
    }

    /// 投递一个就地构造完成的借样消息。失败时 chunk 由内部归还。
    template<typename Flag>
    bool publish_loaned(LoanedMessage<Flag>&& lo, std::uint64_t tm = 0)
    {
        if (!lo.valid() || !publisher_)
        {
            return false;
        }
        if (!lo.finalize())
        {
            /* 超出变长预算 —— lo 析构会归还 chunk。 */
            dzIPC::detail::NoteDzFlatPublish(false);
            return false;
        }
        const auto handle = lo.loan();
        /* publish_loan 接管所有权(成功与否都不再由 lo 归还): 失败时它自己 discard。 */
        lo.release();
        const bool ok = publisher_->publish_loan(handle, tm);
        dzIPC::detail::NoteDzFlatPublish(ok);
        return ok;
    }

    /* 禁用拷贝 */
    shm_pub_ipc(const shm_pub_ipc&) = delete;
    shm_pub_ipc& operator=(const shm_pub_ipc&) = delete;

private:
    void pub_handshake();
    // void sub_listener();

    /* B 级借样封口时写进段头的 msg_id。取自本发布者的话题模板 —— 与 A 级走
     * msg->dz_ipc_msg_id 等价, 但 B 级没有 owning 消息对象可问。 */
    std::uint32_t dzflat_msg_id() const
    {
        return (topic_msg_ && topic_msg_->topic()) ? topic_msg_->topic()->msg_id() : 0;
    }

    /* DZFlat 借样发布; 返回 false 表示本次须回退整包序列化(见 .cc 中的说明)。 */
    bool try_publish_dzflat(const std::shared_ptr<IpcMsgBase>& msg, std::uint64_t tm);

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
    /* ---- 视图路径(零拷贝, 只服务 DZFlat 段) ---- */
    void get(Sample& out);
    bool try_get(Sample& out);

    /* ---- 物化路径(TLV + 快速路径克隆对象 + schema-less 话题) ---- */
    void get_clone(std::shared_ptr<TopicData>& msg);
    bool try_get_clone(std::shared_ptr<TopicData>& msg);
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
    std::shared_ptr<CircularQueue<IpcMsgBase>> msg_queue_;  // 物化队列; shared_ptr for fast-path fanout
    std::shared_ptr<CircularQueue<Sample>> view_queue_;  // 视图队列: 借样的 DZFlat 段
    std::thread* subscribe_thread_{nullptr};
    std::thread* sub_handshake_thread_{nullptr};
    //
    ipc::sync::count_sem* empty_queue_;   // 用于通知订阅者消息队列中有新消息
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    uint32_t msg_id_{0};  // current registration key msg_id; updated on reset_message
    bool local_registered_{false};  // guarded by topic_msg_mtx_; true after InitChannel registers
    /* 本订阅者在控制面 PeerSlot 表中的槽位下标, -1 表示未登记。
     * 由 sub_handshake() 线程独占访问。 */
    int peer_slot_{-1};
};
}   // namespace shm
}   // namespace dzIPC
