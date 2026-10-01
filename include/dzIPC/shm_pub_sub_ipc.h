#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
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
#include "dzIPC/shm_route_session.h"
#include "dzIPC/pub_sub_base.h"
/* W09/t19: B 借样失败的分类计数落点（counters.h 为 header-only 测量库, 无 .so 符号/ABI 影响） */
#include "dzIPC/measure/counters.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "libipc/count_sem.h"
#include "libipc/ipc.h"

namespace dzIPC {
namespace threepools {
/* 阶段 5 共享层：SHM 固定 route 收包 worker 的 route 抽象
 * （include/dzIPC/threepools/recv_worker.h）。公开头只做**前置声明**，真正的
 * include 在 .cc —— 只 include 本头的消费者不必为此被拉进等待层（与
 * include/dzIPC/shm_ser_cli_ipc.h / socket_pub_sub_ipc.h 同一写法）。 */
class RecvRouteSource;
}   // namespace threepools

namespace shm {
class shm_pub_ipc;
class shm_sub_ipc;

/* W06（接收池接入）：订阅 route 的模块侧**独立状态**与适配器（定义在 .cc）。
 *
 * 收包 worker 只通过 SubRecvRoute 接触本状态与 RouteSession，**不持**裸
 * shm_sub_ipc 指针（与 SerState / socket_sub_receive_state 同型）：这是
 * "注销返回后 worker 不再回调已析构对象"的前提（契约 §4.4）。 */
class SubRecvRoute;
struct SubRecvState;

struct SubState
{
    /* 收包分流所需的完整状态；阶段 5 worker 只需持有这份 shared_ptr。 */
    std::shared_ptr<TopicData> topic_msg;
    std::mutex topic_msg_mtx;
    std::shared_ptr<CircularQueue<IpcMsgBase>> msg_queue;
    std::shared_ptr<CircularQueue<Sample>> view_queue;
    std::size_t adopt_cap{0};
    std::atomic<int> adopt_borrowed{0};
    std::uint32_t msg_id{0};
};

IPC_EXPORT void process_received_buffer(const std::shared_ptr<SubState>& state, ipc::buff_t&& raw_data);

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

    /* 预构造段发布(见 pub_ipc_base.h)。段由调用方写好, 这里只负责借 chunk 送出去 ——
     * 发布端因此**有一次整段 memcpy**(段在调用方地址空间里), 接收侧照旧真零拷贝。 */
    bool publish_prebuilt_segment(const void* seg, std::size_t len) override;

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
            /* W09/t19 (c)：借样对象无效 ⇒ 归 borrow_failed_*（⛔ 不碰 fallback_total）。 */
            dzIPC::measure::note_dzflat_borrow_failed(
                (publisher_ != nullptr) && (publisher_->recv_count() > 0), false, false);
            return false;
        }
        if (!lo.finalize())
        {
            /* 超出变长预算 —— lo 析构会归还 chunk。W09/t19 (c)：可精确判定。 */
            dzIPC::detail::NoteDzFlatPublish(false);
            dzIPC::measure::note_dzflat_borrow_failed(
                (publisher_ != nullptr) && (publisher_->recv_count() > 0), false, false);
            return false;
        }
        const auto handle = lo.loan();
        /* W08: 封口后的段字节数必须在 release 之前取(release 后 size() 返回 0)。 */
        const std::uint64_t seg_bytes = static_cast<std::uint64_t>(lo.size());
        /* publish_loan 接管所有权(成功与否都不再由 lo 归还): 失败时它自己 discard。 */
        lo.release();
        const bool ok = publisher_->publish_loan(handle, tm);
        dzIPC::detail::NoteDzFlatPublish(ok);
        /* W09/t19 (c)：B 借样失败单独成类, ⛔ 绝不进 fallback_total。 */
        if (!ok)
        {
            dzIPC::measure::note_dzflat_borrow_failed(
                (publisher_ != nullptr) && (publisher_->recv_count() > 0), true, false);
        }
        /* W08: 三路径分流计数(仅计数, 无行为变更)。B = 应用在借样内存里原地构造。
         * ⛔ 失败**不**在这里记 tlv —— 按借样契约失败是常态, 应用回退到普通 publish
         * 时会在 A/TLV 的调用点被记一次; 这里也记会把 dzflat/fallback 比值算歪。 */
        if (ok)
        {
            dzIPC::detail::NoteDzFlatPathDelivered(dzIPC::DzFlatPath::DzFlatB, seg_bytes);
        }
        return ok;
    }

    /* 禁用拷贝 */
    shm_pub_ipc(const shm_pub_ipc&) = delete;
    shm_pub_ipc& operator=(const shm_pub_ipc&) = delete;

private:
    /* ---- W05：控制面驱动（进程级 ShmControlScheduler，取代 per-route 控制线程）----
     *
     * 驱动源二选一（进程内只读一次，见 .cc 的 control_compat_forced()）：
     *   · 默认（未设 / 空 / 字面 "0"）：进程级 ShmControlScheduler —— 每进程一条 worker，
     *     线程数 O(1)；
     *   · 显式回退 `DZIPC_SHM_CONTROL_SCHEDULER=1`：每话题一条兼容驱动线程，
     *     用**同一份回调体**（PubHeartbeatState::on_pub_heartbeat / on_pub_stale_scan），
     *     周期与判死超时同源。回退只换驱动源，不换语义。
     *
     * ⚠️⚠️ **取值语义与 socket 侧「相同」，极易被误传为「极性与 socket 侧相反」**
     * （本行原先写作「=0 ⇒ 回退」，与实现相反，已按实测更正；t6 记录见
     * 团队改造交付/W05/控制面接入_交付.md §4.1；t75/t77 进一步把「极性相反」的措辞
     * 更正为「语义相同」）：
     *   `DZIPC_SHM_CONTROL_SCHEDULER` —— **非空且非 "0" 的任何值 ⇒ 回退**（含 "1"/"compat"）；
     *   空 / 未设 / "0" ⇒ 新路径（进程级调度器）。
     *   `DZIPC_SOCKET_COMPAT_THREAD` —— **同一张真值表**：未设 / "" / "0" ⇒ 新路径；
     *   "1" / "compat" / "2" / "00" / "legacy" ⇒ 回退。
     *   ⇒ **两侧取值语义相同**；SHM 侧**额外接受字面 `0` 作为显式新路径的写法**
     *     （与 `unset` 同义，便于脚本统一写），⛔ 这只是多一个等价取值，
     *     **不是**「两侧含义相反」—— 该说法会误导回滚手册作者。
     *   实测四值对照：未设→scheduler，`0`→scheduler，`1`→compat，`compat`→compat；
     *   socket 侧同值对照见 R1 证据 `polarity/two_sided_truth_table.log`（逐值相同）。
     *
     * ⛔ 生命周期纪律：调度器只持 shared_ptr<PubControlState>（本成员的派生对象），
     * 绝不持 shm_pub_ipc*。宿主析构体在销毁任何成员**之前**调用 stop_control_plane()
     * （同步注销 + join），因此"回调 → 已析构宿主"这条路径不存在。 */
    struct PubHeartbeatState;
    std::shared_ptr<PubHeartbeatState> pub_control_state_;
    /* ⛔ 回退臂的「低频兜底下次到期点」**不放在这里** —— 那会改变 shm_pub_ipc 的尺寸
     * （= 又一次 ③d 布局变更）。它放在 `PubHeartbeatState` 内部：该类在本头文件里只是
     * **不完整类型**（仅被 shared_ptr 引用），其完整定义在 .cc ⇒ 加成员
     * **不影响任何公开布局**。判据本身在自由函数 `pub_control_tick()`（唯一一处）。 */
    dzIPC::shm_control::RegistrationToken control_reg_;
    std::thread* compat_control_thread_{nullptr};

    /// 显式回退路径：每话题兼容驱动线程（跑同一份 PubHeartbeatState 回调体，50ms）。
    void compat_control_loop();

    /// 同步停止控制面驱动：reset 令牌（等该项在途 tick 结算）+ join 兼容线程。幂等。
    void stop_control_plane() noexcept;
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
    /// 带超时(毫秒)的视图 get; 超时返回 false。用于 TLV-only 话题上避免永久阻塞。
    bool get(Sample& out, std::uint64_t tm_ms);

    /* ---- 物化路径(TLV + 快速路径克隆对象 + schema-less 话题) ---- */
    void get_clone(std::shared_ptr<TopicData>& msg);
    bool try_get_clone(std::shared_ptr<TopicData>& msg);
    /* 禁用拷贝 */
    shm_sub_ipc(const shm_sub_ipc&) = delete;
    shm_sub_ipc& operator=(const shm_sub_ipc&) = delete;

private:
    struct SubHandshakeState;
    std::shared_ptr<SubHandshakeState> sub_control_state_;
    dzIPC::shm_control::RegistrationToken control_reg_;
    std::thread* compat_control_thread_{nullptr};

    /// 显式回退路径：每话题兼容驱动线程（跑同一份 SubHandshakeState 回调体，10ms）。
    void compat_control_loop();

    /// 同步停止控制面驱动：reset 令牌（等该项在途 tick 结算）+ join 兼容线程。幂等。
    void stop_control_plane() noexcept;

    /* ---- W06：接收池接入面（阶段 5 固定 worker）----
     *
     * ⛔ 返回值语义（与 ser/socket 两侧逐条同型）：
     *   true  = 数据面在共享 RecvWorker 上收包，**不**起 subscribe_thread_；
     *   false = **必须**走兼容收包线程，且已在 stderr 打了显式原因（绝不静默）。
     *
     * ⛔ 路径决策**只做一次**（`recv_arm_worker_`）且发生在"首次拿到 route"这一刻
     * （控制面第一次 Ready 并 attach 成功之后）。冻结契约禁止运行中切换接收后端
     * （契约 §4.5 / 方案 §12）：首次决策之后就锁定，后续 generation 重建只按同一
     * 路径重注册，**不再重新决策**。之所以不能在 InitChannel 里决策：那一刻
     * route 还没建立（由控制面回调的 begin_rebuild 建），没有可等待的 token。
     *
     * 决策为 worker 但 `add_route` 非 ok ⇒ 显式回退兼容线程（打原因 + 计数），
     * 并把该实例锁在 compat —— 这次回退是"后端不可用/容量满"的**显式回退信号**
     * （契约 §8.4 容量行：容量满 ⇒ 该 route 显式回退兼容线程），与"运行中偷偷切
     * 后端"不是一回事。 */
    bool start_recv_path();
    /* 注销接收路径（契约 §4.3）：worker = pool.remove_route（同步 1-6 步，含
     * stop_and_wake / wait_quiescent / release_recv）；compat = 无操作（收包线程的
     * join 由析构在 stop_and_wake 之后做）。幂等；由析构线程调用。 */
    void teardown_recv_path() noexcept;
    /* generation 重建的**内存安全顺序**（W04 §7.2 / W04-F2）：
     *   pool.remove_route → begin_rebuild → pool.add_route
     * 两条都是 noexcept 且幂等；compat 路径上是无操作。调用方是控制面 tick 线程
     * （或 compat 控制线程），**不得**从 worker 线程调用（recv_worker.h 的 D-11）。 */
    void before_generation_rebuild() noexcept;
    void after_generation_rebuild() noexcept;
    /* 显式回退（**打原因 + 计数 + seam 事件**）：worker → compat，之后本实例不再进池。
     * reason_code 是 `dzIPC::detail::RecvPathReason` 的数值（定义在内部头
     * include/dzIPC/detail/shm_sub_seam.h，公开头不引入它）。 */
    void fallback_to_compat(int reason_code) noexcept;

    /* 兼容收包线程主体（既有 loop 的逐句搬迁：acquire_receive → recv(50) →
     * release_receive → process_received_buffer）。worker 路径下**不创建**它。
     * ⛔ 只搬语句，不改语义：recv 仍为阻塞 50ms、仍以 handshake_completed 为门。 */
    static void compat_recv_loop(shm_sub_ipc* self);
    /* 起兼容收包线程（幂等；保证 recv_state_ 已建，供 owner CAS 用）。
     * 决策为 compat 时由控制面回调在**首次 attach 成功**那一刻调用 —— 那一刻
     * 之前没有 route 可等，早了只是空转；那一刻起线程才有意义。 */
    void start_compat_recv_thread();

    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed{false};
    bool data_update_{false};
    bool verbose_{false};
    std::string topic_name_;
    std::string raw_topic_name_;
    /* 收包 route 的生命周期协议(阶段 2)。取代原先的 subscriber_ + channel_mtx_:
     * 收包线程只经 acquire_receive/release_receive 取用 route, 握手线程只经
     * begin_rebuild/stop_and_wake 换 route —— release 与 recv 不再可能并发,
     * 收包路径也不必再持有互斥量。
     * 见 docs/消息接收架构改造/阶段2_RouteSession实现说明.md §1-§6。 */
    RouteSession route_session_;
    std::shared_ptr<SubState> sub_state_;
    std::thread* subscribe_thread_{nullptr};

    /* ---- W06：接收池接入（阶段 5）----
     * worker_mode_ = true ⇒ 收包在共享 RecvWorker 上（无 per-route 收包线程）；
     * false ⇒ 兼容 subscribe_thread_（**显式**回退，原因已打日志）。
     * recv_route_ 只在 InitChannel 建好、注销路径**在 remove_route 之后**才 reset
     * —— 与 ser 侧同一条纪律（漏了会让重新注册永久 busy ⇒ 静默丢包）。 */
    std::shared_ptr<SubRecvRoute> recv_route_;
    std::shared_ptr<SubRecvState> recv_state_;
    /* ⛔ 下面四个成员全部由 `recv_path_mtx_` 串行化（写侧 = 控制面 tick 线程 /
     * 析构线程；读侧同上）。为什么必须串行：析构的"置 teardown + remove_route"
     * 与在途 tick 的"检查 teardown + add_route"若不互斥，会出现 **add_route 在
     * remove_route 之后落地**的交叉（池里留着一条宿主已释放的 route ⇒ W04-F2 的
     * SIGSEGV）。冷的成员锁，生命周期事件才取。 */
    mutable std::mutex recv_path_mtx_;
    /* 本实例选择的**臂**（进程内只决策一次）：true = 共享 RecvWorker；false = 兼容
     * 收包线程。显式回退后永久为 false（冻结契约禁止运行中来回切）。 */
    bool recv_arm_worker_{false};
    /* 当前这条 route **此刻**是否已注册进池（回退后为 false）。与 recv_arm_worker_
     * 的区别：后者是"选定的臂"，前者是"route 现在真的在池里"。 */
    bool worker_mode_{false};
    /* 析构已开始（⛔ 之后的任何 add_route 都必须拒绝：见上面的交叉说明）。 */
    bool recv_teardown_{false};
    /* 路径决策是否已做（诊断/日志口径；与 recv_arm_worker_ 同一临界区内更新）。 */
    bool recv_path_decided_{false};
    /* 接收路径代际（诊断/归属日志用；⛔ **不得**进 route key，见 W04 R-17）。 */
    std::atomic<std::uint32_t> recv_generation_{0};
    //
    ipc::sync::count_sem* empty_queue_;   // 用于通知订阅者消息队列中有新消息
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    uint32_t msg_id_{0};  // current registration key msg_id; updated on reset_message
    bool local_registered_{false};  // guarded by sub_state_->topic_msg_mtx
};
}   // namespace shm
}   // namespace dzIPC
