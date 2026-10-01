#include <dzIPC/shm_pub_sub_ipc.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>   /* ::getpid —— fork 闸（见 control_owner_pid()） */
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <typeinfo>
#include <vector>
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/detail/shm_sub_seam.h"   /* 内部测试缝: 默认空指针 ⇒ 零行为变化 */
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"   /* fast-path 借样物化(UF-012) */

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

/* 单 topic 的接收方上限 = libipc 连接位图的位宽(circ::cc_t = uint32_t)。
 * 与 control_plane.h 的 kMaxPeerSlots(64) 不同 —— 那是控制面登记表的容量, 比这里大一倍;
 * 两者的差额正是 docs/shm_defect_fixes.md 第 2 条那个黑洞的容量。 */
constexpr std::size_t kMaxShmReceiversPerTopic = 32;

/* 段名规则收在 dzIPC/common/name_operator.h —— 传输层、sniffer、工具都从那一处取,
 * 免得规则一改要同时改五处且漏掉的那处是静默失效(见该头文件的说明)。
 * 段名含 domain_id: 不含就等于 SHM 上没有 domain 隔离(docs/shm_defect_fixes.md 第 1 条),
 * 代价是与旧版本进程不互通 —— 这是有意的, 旧进程段名不带 domain, 能互通就说明没生效。 */
std::string shm_name_for_topic(const std::string& topic_name, size_t domain_id)
{
    return shm_topic_segment_name(topic_name, domain_id);
}

/* pub/sub 控制面段名 —— 本文件只是**转调**, 规则("数据段名 + _control2")的唯一出处在
 * dzIPC/common/name_operator.h 的 shm_topic_control_name(), 与 ser 侧的
 * service_control_name_for() 同构。
 *
 * 传原始 topic 名 + domain 而不是已经拼好的 topic_name_: 两处调用点手上都有
 * (raw_topic_name_, domain_id_), 而收成一个组合完整的入参后, 这里再也不可能出现
 * "把某个别的东西当数据段名传进来" 的写法 —— 段名拼错不报错, 只静默多出一个空段。 */
std::string topic_control_name_for(const std::string& topic_name, size_t domain_id)
{
    return shm_topic_control_name(topic_name, domain_id);
}

void wait_for_peer_drain(dzIPC::control_plane_shm::TopicControlPlane& control_plane)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (control_plane.peer_count() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/* ============================ W05：控制面时间语义 ============================
 *
 * 三项周期是**冻结值**，不是可调参数：sub_heartbeat=10ms、pub_heartbeat=50ms、
 * peer_dead_timeout=2s（ControlTiming 的默认值，且由 test_shm_control_scheduler
 * 的 TimeSemanticsArePreservedFromLegacyCode / dead_timeout 透传用例守门）。
 * W05 禁用「降低心跳频率制造空闲收益」——那会让死亡检测变慢、把收益记在错误的
 * 账上。这里显式写出来只为一件事：让**兼容驱动线程**与调度器走同一组常量，
 * 两条驱动路径的时间语义不可能分叉。 */
constexpr int64_t kPeerDeadTimeoutNs = 2'000'000'000LL;   ///< 与原 pub_handshake 逐位相同
const dzIPC::shm_control::ControlTiming kControlTiming{};   /* {10ms, 50ms, 2s} */

/* 进程内只读一次的特性开关。用法与 socket 侧的 DZIPC_SOCKET_COMPAT_THREAD 同构
 * （src/dzIPC/socket_pub_sub_ipc.cc:93-107），但**极性相反**，因为两边"兼容"的含义
 * 不同，必须显式写清以免用错：
 *
 *   · socket 侧 `DZIPC_SOCKET_COMPAT_THREAD=1` ⇒ **强制旧路径**（兼容收包线程）；
 *   · 本处 `DZIPC_SHM_CONTROL_SCHEDULER=1` ⇒ **同义**：强制每话题兼容控制线程；
 *   · 本处 `DZIPC_SHM_CONTROL_SCHEDULER=0` ⇒ 与 socket 侧相反，表示**显式启用新路径**
 *     （进程级调度器），即"我确认要新行为"。它等价于不设该变量。
 *
 * 也就是说：**任何非空且非 "0" 的值 = 回退**（含 "compat" / "legacy" / "1"）；
 * 空 / 未设 / "0" = 默认（进程级调度器）。只读一次是为了杜绝"半程切换驱动源"。
 *
 * ⛔ 回退只换**驱动源**，不换语义：兼容线程跑的是同一份回调体（见
 * shm_pub_ipc::compat_control_loop / shm_sub_ipc::compat_control_loop），把
 * ControlTiming 的 10ms/50ms/2s 与判死判据原样交给同一段代码。因此回退不改变
 * heartbeat / stale / generation 重建的任何行为，唯一差别是线程数回到 O(话题数)。 */
bool control_compat_forced()
{
    static const bool forced = []
    {
        const char* v = std::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
        if (v == nullptr || v[0] == '\0')
        {
            return false;   /* 未设 ⇒ 新路径（进程级调度器） */
        }
        if (v[0] == '0' && v[1] == '\0')
        {
            return false;   /* 显式 "0" ⇒ 新路径（与"未设"等价，便于脚本统一写） */
        }
        return true;        /* 其它任何值 ⇒ 强制兼容驱动 */
    }();
    return forced;
}

/* ---- fork 闸（与 socket 侧同型，见 src/dzIPC/socket_pub_sub_ipc.cc:69-88）----
 *
 * `ShmControlScheduler::instance()` 是**故意泄漏的进程级单例**，它的 worker 线程在
 * `fork()` 之后**不存在**（子进程只继承调用线程）。而单例对象与 `running` 标志被
 * 子进程继承 ⇒ `worker_active()` 仍读到 true，模块却把控制面动作注册进一个**没有
 * 线程**的调度器 ⇒ 子进程里的订阅者/发布者拿到"已注册"却永不回调（心跳不刷、握手不
 * 完成）—— 这是**静默失效**，不是崩溃，最难查。
 *
 * 处置与 socket 侧逐条同构：记下本进程**第一次**使用调度器时的 pid，此后 pid 不一致
 * （即本进程是 fork 出来的子进程）就完全不用调度器，退回每话题兼容驱动线程。
 * 判据只取本文件的静态锁，不触碰调度器内部任何锁（fork 时父进程可能正持那些锁）。
 * 进程级"第一个使用者即 owner"是刻意选择：`fork()` 在**首次使用之前**发生的子进程
 * 仍可用调度器（那时它自己就是 owner），只有"父进程已建单例、再 fork"才退避。 */
int32_t control_owner_pid()
{
    static std::mutex gate_mtx;
    static int32_t owner = 0;
    const int32_t pid =
#if defined(_WIN32)
        static_cast<int32_t>(::GetCurrentProcessId());
#else
        static_cast<int32_t>(::getpid());
#endif
    std::lock_guard<std::mutex> lock(gate_mtx);
    if (owner == 0)
    {
        owner = pid;
    }
    return owner;
}

bool control_scheduler_allowed_in_this_process()
{
#if defined(_WIN32)
    return control_owner_pid() == static_cast<int32_t>(::GetCurrentProcessId());
#else
    return control_owner_pid() == static_cast<int32_t>(::getpid());
#endif
}

}   // namespace

void process_received_buffer(const std::shared_ptr<SubState>& state, ipc::buff_t&& raw_data)
{
    if (!state || raw_data.empty())
    {
        return;
    }
    if (IsWakeupArtifact(raw_data))
    {
        NoteWakeupArtifact();
        return;
    }

    std::uint32_t exp_id = 0, exp_hash = 0;
    bool viewable = false;
    {
        std::lock_guard<std::mutex> lock(state->topic_msg_mtx);
        if (!state->topic_msg)
        {
            return;
        }
        exp_id = state->msg_id;
        exp_hash = state->topic_msg->topic()->dzflat_schema_hash();
        viewable = (exp_hash != 0);
    }

    const bool is_dzflat = dzflat::looks_like_dzflat(raw_data.data(), raw_data.size());
    if (is_dzflat)
    {
        if (viewable)
        {
            std::uint32_t seg_id = 0;
            if (!IpcMsgBase::dzflat_peek_msg_id(raw_data.data(), raw_data.size(), seg_id)
                || seg_id != exp_id)
            {
                detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatIdSkipped);
                return;
            }
            dzflat::SegHeader h{};
            std::memcpy(&h, raw_data.data(), sizeof(h));
            if (h.schema_hash != exp_hash)
            {
                detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatSchemaDrop);
                return;
            }
            detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatAccepted);
            state->view_queue->push(std::make_shared<Sample>(std::move(raw_data), seg_id, exp_hash));
            return;
        }

        std::uint32_t seg_id = 0, seg_hash = 0;
        {
            dzflat::SegHeader h{};
            std::memcpy(&h, raw_data.data(), sizeof(h));
            seg_hash = h.schema_hash;
        }
        if (!IpcMsgBase::dzflat_peek_msg_id(raw_data.data(), raw_data.size(), seg_id)
            || seg_id != exp_id)
        {
            detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatIdSkipped);
            return;
        }
        detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatAccepted);
        std::shared_ptr<TopicData> local_msg;
        {
            std::lock_guard<std::mutex> lock(state->topic_msg_mtx);
            if (!state->topic_msg)
            {
                return;
            }
            local_msg.reset(state->topic_msg->clone());
        }
        const bool quota_ok = state->adopt_borrowed.load(std::memory_order_relaxed)
                              < static_cast<int>(state->adopt_cap);
        if (quota_ok && local_msg->topic()->dzflat_adopt(std::move(raw_data), seg_hash))
        {
            state->adopt_borrowed.fetch_add(1, std::memory_order_relaxed);
            std::shared_ptr<IpcMsgBase> ptr_cache;
            local_msg->swap(ptr_cache);
            state->msg_queue->push(std::move(ptr_cache));
            return;
        }
        if (!raw_data.empty() && local_msg->topic()->dzflat_read(raw_data.data(), raw_data.size()))
        {
            detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatAdoptSpilled);
            std::shared_ptr<IpcMsgBase> ptr_cache;
            local_msg->swap(ptr_cache);
            state->msg_queue->push(std::move(ptr_cache));
            return;
        }
        return;
    }
    if (dzflat::has_dzflat_magic(raw_data.data(), raw_data.size()))
    {
        detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatHeaderBad);
        return;
    }

    std::shared_ptr<TopicData> local_msg;
    {
        std::lock_guard<std::mutex> lock(state->topic_msg_mtx);
        if (!state->topic_msg)
        {
            return;
        }
        local_msg.reset(state->topic_msg->clone());
    }
    if (!AcceptWire(raw_data, local_msg->msg_id(), *local_msg->topic()))
    {
        return;
    }
    std::shared_ptr<IpcMsgBase> ptr_cache;
    local_msg->swap(ptr_cache);
    state->msg_queue->push(std::move(ptr_cache));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* ===================== W05：发布端控制面状态（项内记账 + 无裸宿主指针）=====================
 *
 * 本类就是 `pub_handshake()` 循环体的**逐句搬迁**，唯一的差别是"谁来按时调它"：
 *   · 默认由进程级 ShmControlScheduler 按 ControlTiming{pub_heartbeat=50ms} 驱动；
 *   · 显式回退（DZIPC_SHM_CONTROL_SCHEDULER=1）由每话题兼容线程按同一 50ms 驱动。
 * 周期、判死超时、日志与状态迁移判据一律逐位保持 —— 迁移不是重新设计。
 *
 * 为什么宿主指针能留在这里：本对象由 `shm_pub_ipc::pub_control_state_` 持有，只会
 * 随宿主一起析构；而宿主的析构体在销毁**任何**成员之前先 `stop_control_plane()`
 * 同步注销（等该项在途 tick 结算），因此回调不可能在宿主析构后再到达。调度器一侧
 * 只持 `shared_ptr<PubControlState>`（抽象基类），**看不到** shm_pub_ipc* ——
 * 「不保存可能析构的裸对象指针」这条要求针对的正是调度器那一侧。
 *
 * 记账纪律：`had_subscriber_` 属于**本项**（每话题一份），不得提到 topic 级 ——
 * 否则同进程两个发布者会互相改写"刚发现订阅者"这个状态迁移。 */
struct shm_pub_ipc::PubHeartbeatState : dzIPC::shm_control::PubControlState
{
    explicit PubHeartbeatState(shm_pub_ipc* host) noexcept : host_(host) {}

    const char* debug_name() const noexcept override
    {
        return (host_ != nullptr) ? host_->topic_name_.c_str() : nullptr;
    }

    /* 等价于原 pub_handshake() 单次循环体，**去掉 sleep**（周期由驱动方负责）。 */
    void on_pub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override
    {
        shm_pub_ipc* h = host_;
        if (h == nullptr) return;

        h->control_plane_.heartbeat();

        /* 死连接回收。
         *
         * libipc 的 force_push 以前在队列满时用 disconnect_receiver() 踢掉
         * "还没读完这一格"的订阅者 —— 那个判据区分不了慢和死, 会把活的慢
         * 订阅者永久摘下线。现在写路径只覆写不踢人, 回收改由这里驱动: 只有
         * 心跳停了 kPeerDeadTimeout 的订阅者才被判死。
         *
         * 超时取 2s = 200 个心跳周期(订阅端 10ms 一次), 留足余量, 宁可晚回收
         * 也不要误杀 —— 误杀正是这次要修掉的问题。
         *
         * ⚠️ 与旧实现的一处**有意的次序调整**：旧版无条件调 collect_stale_peers，
         * 新版把它挪到 `has_peers()` 之后（调度器只在 has_peers() 为真时才调
         * on_pub_stale_scan，见 shm_control_scheduler.h:118-124）。无 peer 时扫描
         * 全部槽位的结果必然是 0（没有 in_use 槽位可判死），因此**行为等价**；
         * 差别只在"无 peer 时不白扫 64 个槽位"。时间语义（50ms/2s）与判死判据不变。 */
        const bool has_peer = h->control_plane_.peer_count() > 0;
        h->subscribed_.store(has_peer, std::memory_order_release);
        if (has_peer && !had_subscriber_ && h->verbose_)
        {
            std::cerr << "\033[32m[" << h->topic_name_
                      << "PubInfo] Publisher detected a subscriber on topic: " << h->topic_name_ << "\033[0m"
                      << std::endl;
        }
        had_subscriber_ = has_peer;
    }

    bool has_peers() const override
    {
        const shm_pub_ipc* h = host_;
        return (h != nullptr) && h->control_plane_.peer_count() > 0;
    }

    /* 仅当 has_peers() 为真时被调用（调度器契约）。 */
    void on_pub_stale_scan(dzIPC::shm_control::ControlClock::time_point now,
                           std::chrono::nanoseconds dead_timeout) override
    {
        shm_pub_ipc* h = host_;
        if (h == nullptr) return;
        /* 用**调度器传入**的超时值（= ControlTiming::peer_dead_timeout = 2s），
         * 但逐位比对 kPeerDeadTimeoutNs —— 两者不等就说明周期配置被改过，
         * 那正是本工作包禁止的事（不得降频制造收益）。 */
        int64_t timeout_ns = static_cast<int64_t>(dead_timeout.count());
        if (timeout_ns != kPeerDeadTimeoutNs)
        {
            static std::atomic<bool> warned_once{false};
            if (!warned_once.exchange(true, std::memory_order_relaxed))
            {
                std::cerr << "\033[31m[ShmPubControl] peer_dead_timeout=" << timeout_ns
                          << "ns 与冻结值 " << kPeerDeadTimeoutNs
                          << "ns 不符 —— W05 不得改判死超时, 按冻结值执行\033[0m" << std::endl;
            }
            timeout_ns = kPeerDeadTimeoutNs;
        }
        (void)now;   /* 判死用控制面自己的单调时钟(collect_stale_peers 内部 now_ns()) */
        const uint32_t stale = h->control_plane_.collect_stale_peers(timeout_ns);
        if (stale != 0 && h->publisher_ && h->publisher_->valid())
        {
            h->publisher_->disconnect_receivers(stale);
            if (h->verbose_)
            {
                std::cerr << "\033[33m[" << h->topic_name_
                          << "PubInfo] reaped dead subscriber connection(s), cc_ids = 0x" << std::hex << stale
                          << std::dec << "\033[0m" << std::endl;
            }
        }
    }

    /* 宿主私有成员访问：本结构体是 shm_pub_ipc 的嵌套类型，天然有访问权。
     * 这里显式写出它持的是**宿主裸指针**，并说明其生命周期由宿主析构序保证（见上）。 */
    shm_pub_ipc* host_{nullptr};
    bool had_subscriber_{false};
};

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_(shm_name_for_topic(topic_name, domain_id))
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    topic_msg_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_PubOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 同步停止控制面驱动（幂等）。**必须在销毁任何被回调引用的成员之前调用**。
 *
 * 两步的分工：
 *   ① `control_reg_.reset()` —— RegistrationToken 的同步注销：标记 inactive →
 *      唤醒调度器 → 等**该项**在途 tick 结算 → 摘除 → 返回。返回后调度器不再持有
 *      本项的 state（shared_ptr 计数回落），也不可能再发起回调；
 *   ② join 兼容线程（仅在回退路径存在）—— compat_control_loop 的循环条件与唤醒
 *      依赖 `running`，因此本函数**只 join、不置 running**；置位由调用方在自己的
 *      顺序里做（发布端析构先置 false 再清控制面，订阅端析构先退出收包线程）。
 * 顺序不可交换：先 join 再 reset 的话，兼容线程可能正卡在一次长回调里，join 会等到
 * 它结束 —— 那没有错，但 reset 放在后面就失去"注销返回即无回调"的语义。
 * 这里先 reset 再 join：reset 让调度器路径立刻止住，join 让回退路径止住。 */
void shm_pub_ipc::stop_control_plane() noexcept
{
    control_reg_.reset();   /* 幂等：未注册时是空操作 */
    pub_control_state_.reset();
    if (compat_control_thread_ != nullptr)
    {
        /* ⛔ 兼容线程的循环条件就是 `running`，因此**必须先置 false 再 join** ——
         * 反过来就是 join 一个永不退出的循环（旧实现同样是"先 running=false 再
         * join publish_thread_"，此处只是把置位收进本函数以免调用点漏掉）。 */
        running.store(false, std::memory_order_release);
        if (compat_control_thread_->joinable())
        {
            compat_control_thread_->join();
        }
        delete compat_control_thread_;
        compat_control_thread_ = nullptr;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::~shm_pub_ipc()
{
    /* ⛔ 第 1 步（顺序承重）：先**同步注销控制面**，再销毁任何被回调引用的成员。
     * reset() 返回后本项的 state 已从调度器摘除（等过在途 tick 结算），因此下面
     * 对 topic_name_/control_plane_/publisher_ 的操作不可能与回调并发。 */
    stop_control_plane();
    running.store(false, std::memory_order_release);
    /* ⛔ 发布端退出 = 控制面离开 Ready —— 这个**收尾动作**原在 pub_handshake() 循环
     * 退出后执行（`subscribed_=false` + `set_stopping()`），迁到调度器后没有"线程退出"
     * 这一刻，必须由析构体补上。⛔ 不能省：订阅端靠"控制面离开 Ready"才走
     * stop_and_wake/detach（test_wakeup_artifact 的 NoPhantomMessageOnGenerationRebuild
     * 与 test_shm_sub_dtor_gate 的前提都建立在"拆 pub ⇒ 状态离开 Ready"上）。
     * 位置与旧实现同序：清零 → set_stopping → 等 peer 排空 → clear。 */
    subscribed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
    if (publisher_ && publisher_->valid())
    {
        wait_for_peer_drain(control_plane_);
        publisher_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

void shm_pub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
    // Reset fast-path state: new message type requires re-confirmation.
    std::lock_guard<std::mutex> lock(fast_path_mtx_);
    fp_consecutive_ = 0;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_pub_ipc::InitChannel(std::string extra_info)
{
    try
    {
        if (!control_plane_.open(topic_control_name_for(raw_topic_name_, domain_id_)))
        {
            throw std::runtime_error("failed to open topic control plane");
        }
        control_plane_.begin_rebuild();
        ipc::route::clear_storage(topic_name_.c_str());
        publisher_ = std::make_shared<ipc::route>(topic_name_.c_str(), ipc::sender, verbose_);
        control_plane_.set_ready();
        /* W05：控制面驱动。默认进程级调度器（线程数 O(1)）；显式回退每话题兼容线程。
         * ⛔ 先建 state、再注册（RegistrationToken 的"先注册后构造"形式），
         *    避免 (sched, sched.register_...) 那种"注册成功但令牌构造抛异常 ⇒ 条目泄漏"。
         * ⛔ 幂等：重复 InitChannel 不得叠加第二个驱动（旧实现会再起一条线程并
         *    泄漏上一条的句柄）。 */
        if (pub_control_state_ == nullptr && compat_control_thread_ == nullptr)
        {
            pub_control_state_ = std::make_shared<PubHeartbeatState>(this);
            if (control_compat_forced())
            {
                compat_control_thread_ = new std::thread(&shm_pub_ipc::compat_control_loop, this);
            }
            else if (!control_scheduler_allowed_in_this_process())
            {
                /* fork 闸：本进程是子进程，而调度器单例（含 worker 线程）是父进程建的 ——
                 * 子进程里它没有线程，注册进去就是"永不回调"。退回兼容驱动并说明原因。 */
                std::cerr << "\033[33m[" << topic_name_
                          << "PubInfo] forked child: ShmControlScheduler owner pid mismatch; "
                             "keeping per-topic control thread\033[0m" << std::endl;
                compat_control_thread_ = new std::thread(&shm_pub_ipc::compat_control_loop, this);
            }
            else if (!dzIPC::shm_control::ShmControlScheduler::instance().worker_active())
            {
                /* 调度器已被 stop（不可恢复）：不静默降级为"注册了但永不被回调"，
                 * 退回兼容线程并把原因打出来（与 socket 侧的 fallback 同一口径）。 */
                std::cerr << "\033[33m[" << topic_name_
                          << "PubInfo] ShmControlScheduler 已停止; 退回每话题兼容控制线程\033[0m" << std::endl;
                compat_control_thread_ = new std::thread(&shm_pub_ipc::compat_control_loop, this);
            }
            else
            {
                dzIPC::shm_control::RegistrationToken reg{
                    dzIPC::shm_control::ShmControlScheduler::instance(), pub_control_state_, kControlTiming};
                control_reg_ = std::move(reg);
                if (!control_reg_.valid())
                {
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] ShmControlScheduler 注册失败; 退回每话题兼容控制线程\033[0m" << std::endl;
                    compat_control_thread_ = new std::thread(&shm_pub_ipc::compat_control_loop, this);
                }
            }
        }
        std::string topic_type_name = topic_msg_->topic()
                                          ? dzIPC::info_pool::demangle(typeid(*topic_msg_->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmPub, raw_topic_name_, topic_type_name, "shm",
                          static_cast<int32_t>(domain_id_), extra_info});
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 兼容驱动线程（显式回退路径）：跑**同一份**回调体，周期 = pub_heartbeat = 50ms。
 * 这里只负责"按时调它"与收尾，控制面语义全在 PubHeartbeatState 里 —— 两条驱动
 * 路径不可能分叉。收尾（subscribed_=false + set_stopping）与原 pub_handshake()
 * 逐句相同。 */
void shm_pub_ipc::compat_control_loop()
{
    if (verbose_)
        std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher has created topic: " << topic_name_ << "\033[0m"
                  << std::endl;
    while (running.load(std::memory_order_acquire))
    {
        if (pub_control_state_ != nullptr)
        {
            pub_control_state_->on_pub_heartbeat(dzIPC::shm_control::ControlClock::now());
            if (pub_control_state_->has_peers())
            {
                pub_control_state_->on_pub_stale_scan(dzIPC::shm_control::ControlClock::now(),
                                                      kControlTiming.peer_dead_timeout);
            }
        }
        std::this_thread::sleep_for(kControlTiming.pub_heartbeat);
    }
    subscribed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    return publish_best_effort(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    // --- Intra-process fast path ---
    // When all observed peers are local (same process), clone once and fanout
    // the same shared_ptr to every subscriber queue, skipping SHM serialize.
    // K=3 consecutive matching observations gates activation to reduce the
    // window where a remote peer may be completing handshake.
    //
    // Before any subscriber has completed handshake, recv_count() is 0 and
    // the snapshot is empty — the first few messages still go through SHM
    // so late joiners receive them correctly.
    //
    // Gated by the unified process-wide nodelet switch
    // (dzIPC::IsNodeletEnabled()).

    if (!dzIPC::IsNodeletEnabled())
    {
        return publish_for_sniffer(std::move(msg));
    }

    ChannelKey key{raw_topic_name_, domain_id_, msg->msg_id()};
    auto& reg = LocalPubSubRegistry::instance();
    auto snapshot = reg.subscriber_snapshot(key);
    const auto shm_recv = publisher_->recv_count();

    bool use_fast_path = false;
    {
        std::lock_guard<std::mutex> lock(fast_path_mtx_);
        // Reset on any state change: different key, or counts diverged.
        if (!(key == last_fp_key_) || snapshot.size() != last_fp_snapshot_size_
            || shm_recv != last_fp_recv_count_)
        {
            fp_consecutive_ = 0;
            last_fp_key_ = key;
            last_fp_snapshot_size_ = snapshot.size();
            last_fp_recv_count_ = shm_recv;
        }

        if (!snapshot.empty() && shm_recv == snapshot.size())
        {
            ++fp_consecutive_;
            if (fp_consecutive_ >= kFastPathConfirm)
            {
                use_fast_path = true;
            }
        }
        else
        {
            fp_consecutive_ = 0;
        }
    }

    if (use_fast_path)
    {
        // Clone once, fanout to all local queues.
        std::shared_ptr<IpcMsgBase> cloned(msg->clone());
        /* ⛔ 队列里的借样只允许来自接收侧 adopt 配额(UF-012)。发布侧消息若自身持
         * 借样(订阅后转发的 GenericMessage), clone 拷的是 shared_ptr<buffer> ——
         * 借样随克隆进各订阅者队列, 绕开配额钉池。物化掉: clone 自持堆块。 */
        if (cloned->dzflat_is_borrowed())
        {
            auto* gm = dynamic_cast<GenericMessage*>(cloned.get());
            if (gm != nullptr)
            {
                gm->dzflat_read(gm->dzflat_data(), gm->dzflat_len());
            }
        }
        for (auto& q : snapshot)
        {
            q->push(cloned);   // const& overload: copies shared_ptr
        }
        return true;
    }

    // Nodelet requested but unavailable: one-shot warning per reason.
    // Atomic fetch_or prevents data races across concurrent publish calls
    // and guarantees each reason fires at most once per instance lifetime.
    enum : uint8_t
    {
        kWarnNoLocalSubs = 1 << 0,
        kWarnMixedPeers = 1 << 1,
    };
    if (snapshot.empty())
    {
        if (!(nodelet_warned_.fetch_or(kWarnNoLocalSubs, std::memory_order_relaxed) & kWarnNoLocalSubs))
        {
            std::cerr << "\033[33m[" << raw_topic_name_
                      << "] nodelet requested but unavailable; falling back to SHM path"
                      << " (no local subscribers)\033[0m" << std::endl;
        }
    }
    else if (shm_recv != snapshot.size())
    {
        if (!(nodelet_warned_.fetch_or(kWarnMixedPeers, std::memory_order_relaxed) & kWarnMixedPeers))
        {
            std::cerr << "\033[33m[" << raw_topic_name_
                      << "] nodelet requested but unavailable; falling back to SHM path"
                      << " (mixed local/remote peers: local=" << snapshot.size()
                      << " shm=" << shm_recv << ")\033[0m" << std::endl;
        }
    }

    // Fallback: old SHM path (serialize + shared-memory send)
    return publish_for_sniffer(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    try
    {
        if (try_publish_dzflat(msg, tm))
        {
            dzIPC::detail::NoteDzFlatPublish(true);
            /* W08: 三路径分流计数(仅计数, 无行为变更)。A = 对象→共享段一次复制;
             * wire = 段字节数。 */
            dzIPC::detail::NoteDzFlatPathDelivered(
                dzIPC::DzFlatPath::DzFlatA, static_cast<std::uint64_t>(msg->dzflat_size()));
            return true;
        }
        dzIPC::detail::NoteDzFlatPublish(false);
        ipc::buffer response_data(std::move(msg->serialize()));
        /* W08: 为计数把原两个 return 收敛成一个局部 sent —— 控制流与返回值逐位不变。 */
        const bool sent = (publisher_->recv_count() == 0)
                              ? publisher_->no_member_try_send(response_data.data(), response_data.size(), 0)
                              : publisher_->try_send(response_data.data(), response_data.size(), tm);
        if (sent)
        {
            dzIPC::detail::NoteDzFlatPathDelivered(
                dzIPC::DzFlatPath::Tlv, static_cast<std::uint64_t>(response_data.size()));
        }
        return sent;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
    }
    return false;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* DZFlat 借样发布: 借一块共享 chunk, 把消息**直接按平坦布局写进共享内存**, 省掉
 * 「serialize() 整包 new + TLV 页尾分段拷贝 + send() 再 memcpy 进 chunk」这一整串。
 *
 * 返回 false 表示"本次不走 DZFlat", 调用方须回退既有整包路径。回退不是异常, 是常态:
 *   - 开关未开 / 消息类型不支持(手写类型、GenericMessage);
 *   - 该通道当前没有接收方(chunk 无人回收, loan 会拒绝);
 *   - chunk 池耗尽(每尺寸档位 32 块) —— 这是背压, 回退整包路径仍能送达。
 *
 * 生命周期: loan 成功后每条出口都必须以 publish_loan 或 discard_loan 结束。
 * publish_loan 失败时 chunk 已由其内部归还, 这里不得重复 discard。
 */
bool shm_pub_ipc::try_publish_dzflat(const std::shared_ptr<IpcMsgBase>& msg, std::uint64_t tm)
{
    if (!dzIPC::IsDzFlatEnabled() || !msg || !msg->dzflat_supported())
    {
        return false;
    }
    if (!publisher_ || publisher_->recv_count() == 0)
    {
        return false;
    }
    const std::uint32_t need = msg->dzflat_size();
    if (need == 0)
    {
        return false;
    }
    auto lo = publisher_->loan(need);
    if (!lo.valid())
    {
        return false;   // 池耗尽 / 无接收方 —— 回退整包
    }
    if (!msg->dzflat_write(lo.data, static_cast<std::uint32_t>(lo.size)))
    {
        publisher_->discard_loan(lo);
        return false;
    }
    return publisher_->publish_loan(lo, tm);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 预构造段发布(见 pub_ipc_base.h): 段由调用方(今天的唯一使用者是 Python)按自己的 schema
 * 写好交来, 这里只负责借一块 chunk 把它**原样**送出去。
 *
 * 与 try_publish_dzflat 的关系: 门槛完全同源(开关 / 接收方 / chunk 池), 差别只在"段是
 * 谁写的" —— 那边是消息类型就地写进借来的 chunk(发布端也零拷贝), 这边是段已在调用方
 * 地址空间里, 唯一能做的是那一跳 memcpy。接收侧两条路完全一致(都是 DZFlat 段, 都借样)。
 *
 * 不记 publish 事件日志: log_publish_event 需要 owning 消息对象去 clone + serialize,
 * 段路径没有(段本身就是序列化结果)。这是取舍, 见函数末尾注释。 */
bool shm_pub_ipc::publish_prebuilt_segment(const void* seg, std::size_t len)
{
    if (!dzIPC::IsDzFlatEnabled() || seg == nullptr)
    {
        return false;
    }
    /* 段头先自证: magic / layout_ver / total_size <= len / root_off。坏段一律不发 ——
     * 送出去只会被对端按 kDzFlatHeaderBad 丢掉, 还白占一块 chunk。 */
    if (!dzflat::looks_like_dzflat(seg, len))
    {
        return false;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, seg, sizeof(h));
    /* 段头 msg_id 必须等于本话题模板的 msg_id。订阅端判"这条是不是我的话题"用的就是
     * 这个值(shm_pub_sub_ipc.cc 订阅循环的 exp_id = 话题注册键), 不符 ⇒ 对端**静默丢弃**。
     * 宁可让调用方回退 TLV 走慢路径, 也不要发一条注定被丢的段。 */
    if (h.msg_id != dzflat_msg_id())
    {
        return false;
    }
    if (!publisher_ || publisher_->recv_count() == 0)
    {
        return false;
    }
    auto lo = publisher_->loan(h.total_size);
    if (!lo.valid())
    {
        return false;   // 池耗尽 —— 背压, 回退整包
    }
    std::memcpy(lo.data, seg, h.total_size);
    const bool ok = publisher_->publish_loan(lo, 0);
    if (ok)
    {
        dzIPC::detail::NoteDzFlatPublish(true);
        /* W08: 预构造段入口**单列**计数 —— ⛔不并进 A/B(既非"对象→段"也非"应用原地
         * 构造"; W03 的三路径 ID 未定义该入口, 已在交付里登记缺口)。 */
        dzIPC::detail::NoteDzFlatPathDelivered(
            dzIPC::DzFlatPath::PrebuiltSegment, static_cast<std::uint64_t>(h.total_size));
    }
    /* 失败不在这里计数: 调用方随后那次 TLV publish() 会记一次回退。两边各记一次会把
     * fallback/(dzflat+fallback) 这个比值算歪 —— 那个比值是判断"收益有没有生效"的唯一
     * 指标(nodelet_config.h), 不能因为记账方式失真。 */
    return ok;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    try
    {
        /* 有接收方时优先借样(tm=0: 与 no_member_try_send 的 best-effort 语义一致)。
         * 无接收方时必须走 no_member_try_send —— 那条路径会把负载送进 sniffer 环,
         * 而 sniffer 侧解析的是 TLV, 所以不能用 DZFlat。 */
        if (try_publish_dzflat(msg, 0))
        {
            dzIPC::detail::NoteDzFlatPublish(true);
            /* W08: 三路径分流计数(仅计数)。 */
            dzIPC::detail::NoteDzFlatPathDelivered(
                dzIPC::DzFlatPath::DzFlatA, static_cast<std::uint64_t>(msg->dzflat_size()));
            return true;
        }
        dzIPC::detail::NoteDzFlatPublish(false);
        ipc::buffer response_data(std::move(msg->serialize()));
        const bool sent = publisher_->no_member_try_send(response_data.data(), response_data.size(), 0);
        if (sent)
        {
            dzIPC::detail::NoteDzFlatPathDelivered(
                dzIPC::DzFlatPath::Tlv, static_cast<std::uint64_t>(response_data.size()));
        }
        return sent;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
    }
    return false;
}

namespace {

/* 步骤③ 的钉生效时的**一次性**诊断。
 *
 * 为什么必须有: view 队列容量是对调用方**显式传入**的 queue_size 的覆盖(该参数在
 * SubscriberIPCPtrMake 里必填、无默认值)。覆盖而不说 = 又一次"静默改变行为",
 * 与本仓反复踩过的那一类(ArgParser 的 BOOL 吞 token、create|open 造空壳段)同族。
 *
 * 去重口径: 按"被请求的 queue_size"去重 —— 同一个值只报一次, 不同值各报一次,
 * 这样一个应用建了多档订阅者时不会漏掉后一档。输出口径照 ipc_info_pool.cc 的
 * 说明: src/dzIPC 侧统一用 std::cerr(黄色)。 */
void warn_view_queue_pinned(std::size_t requested, std::size_t applied)
{
    static std::mutex lock;
    static std::vector<std::size_t> seen;
    {
        std::lock_guard<std::mutex> guard{lock};
        if (std::find(seen.begin(), seen.end(), requested) != seen.end())
        {
            return;
        }
        seen.push_back(requested);
    }
    std::cerr << "\033[33m[dzIPC][view_queue] queue_size = " << requested << " 超过钉上限, 被钉到 " << applied
              << ": chunk 池每尺寸档只有 " << static_cast<std::size_t>(ipc::large_msg_cache)
              << " 块且全机共享(建 route 不带 prefix), 队列配得比池大就会把池吃干。"
                 "两个钉面同用此上限: view 队列钉到 " << applied
              << "; adopt 借样(schema-less 话题的 DZFlat 经 msg_queue_)配额同为 " << applied
              << ", 配额满即自动物化拷贝 —— 零拷贝只在配额内生效, 超额每消息多一次拷贝。"
                 "见 docs/shm_chunk_pool_occupancy_plan.md §3 步骤③与 UF-012; "
                 "EnableViewQueuePin(false) 可恢复原值。\033[0m"
              << std::endl;
}

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::shm_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         const size_t queue_size, bool verbose, bool enable_thread_qos, int cpu_id,
                         int thread_priority)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_(shm_name_for_topic(topic_name, domain_id))
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    sub_state_ = std::make_shared<SubState>();
    sub_state_->topic_msg.reset(msg->clone());
    msg_id_ = sub_state_->topic_msg->topic()->msg_id();
    sub_state_->msg_id = msg_id_;
    sub_state_->msg_queue = std::make_shared<CircularQueue<IpcMsgBase>>(queue_size);
    /* 步骤③: view 队列钉 chunk(借样 Sample 持有 buff_t, 见本文件订阅循环 :773 处
     * 注释), adopt 借样(GenericMessage 收 schema-less DZFlat)经 msg_queue_ 也钉
     * chunk —— 两者的配额同源: ViewQueueCap() = large_msg_cache/4 = 10(对齐 ROS 2
     * 默认 QoS depth), 保持 "4 个订阅者满钉" 的池余量(10×4 = 40 = 池容量)。池每
     * 尺寸档 large_msg_cache 块且全机共享(建 route 不带 prefix) ⇒ 不设上限的队列
     * 配置就能把池吃干, 之后发布侧 loan 拿不到块而回退整包 TLV。设计与实测见
     * docs/shm_chunk_pool_occupancy_plan.md §3 步骤③。
     *
     * ⚠️ msg 队列的 TLV 物化消息不钉 chunk —— 钉的只是 adopt 借样, 由 adopt_cap_
     *    配额封顶(见订阅循环 adopt 分支与 UF-012); 缩 msg_queue_ 本体只是白减缓冲。
     * ⚠️ socket/UDP 侧不钉: 那边的 Sample/借样持有的是去帧独立堆块, 不占池。 */
    const std::size_t view_cap =
        dzIPC::IsViewQueuePinEnabled() ? dzIPC::ViewQueueCap() : queue_size;
    if (view_cap < queue_size)
    {
        warn_view_queue_pinned(queue_size, view_cap);
    }
    sub_state_->view_queue = std::make_shared<CircularQueue<Sample>>(
        (view_cap < queue_size) ? view_cap : queue_size);
    /* UF-012 adopt 借样配额: 与 view 队列同一上限、同一开关。借样进 msg_queue_
     * 的消息每条钉一块 chunk, 而队列深度是用户配置的 queue_size(可能远大于池),
     * 不设配额一个慢消费者就能把整档池钉干。计数三条路径: adopt 入队 +1,
     * pop(get_clone/try_get_clone) -1, 满队挤最老(evict 回调) -1。 */
    sub_state_->adopt_cap = view_cap;
    const auto state = sub_state_;
    sub_state_->msg_queue->set_evict_cb(
        [state](const std::shared_ptr<IpcMsgBase> &dropped)
        {
            if (dropped && dropped->dzflat_is_borrowed())
            {
                state->adopt_borrowed.fetch_sub(1, std::memory_order_relaxed);
            }
        });
}

/******************************************************************************************************/
/* ===================== W05：订阅端控制面状态（项内记账 + 无裸宿主指针）=====================
 *
 * 本类就是原 `sub_handshake()` 循环体的**逐句搬迁**，唯一的差别是"谁来按时调它"：
 *   · 默认由进程级 ShmControlScheduler 按 ControlTiming{sub_heartbeat=10ms} 驱动；
 *   · 显式回退（DZIPC_SHM_CONTROL_SCHEDULER=1）由每话题兼容线程按同一 10ms 驱动。
 *
 * ⛔ 三项记账（peer_registered / attached_generation / peer_slot）**留在本项内**，
 *    不提到 topic 级：add_peer/remove_peer 改的是共享段里的 peer_count，同进程两个
 *    订阅项若共用记账会重复 add_peer，让 peer_count 偏大、发布端 subscribed_ 与
 *    stale 判定一起失真（shm_control_scheduler.h:76-88 原文约束）。
 *
 * ⛔ 回调**不得抛出**。原实现在 control plane open 失败时 `throw std::runtime_error`
 *    （旧代码 :965），那时它发生在本订阅自己的线程里 ⇒ terminate 单个进程；迁进进程级
 *    调度线程后同一失败会带走**该进程全部话题**的控制面。故这里改为"告警一次（不受
 *    verbose_ 约束）+ 100ms 退避重试"，与原 100ms 重试的**周期**一致。
 *
 * ⛔ 回调**不得阻塞**。原循环体里有两处 sleep（cc_id==0 的 100ms 重试、循环底部的
 *    10ms 周期）。前者改为**项内 deadline 退避**（now < backoff_until_ 直接返回），
 *    后者交给驱动方的周期 —— 于是"一次 tick 的扫描耗时"有界，不会被某一个订阅者的
 *    退避睡掉（要求 4：单个慢 tick 不得无界阻塞所有 route）。 */
struct shm_sub_ipc::SubHandshakeState : dzIPC::shm_control::SubControlState
{
    explicit SubHandshakeState(shm_sub_ipc* host) noexcept : host_(host) {}

    const char* debug_name() const noexcept override
    {
        return (host_ != nullptr) ? host_->topic_name_.c_str() : nullptr;
    }

    /* 等价于原 `sub_handshake()` 单次循环体（去掉 sleep，`continue` → `return`）。 */
    void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point now) override
    {
        shm_sub_ipc* h = host_;
        if (h == nullptr) return;

        /* 退避窗口（原 cc_id==0 分支的 sleep(100ms) 的等价物）。 */
        if (now < backoff_until_)
        {
            return;
        }

        /* 控制面段**首次**打开。失败不抛：告警一次 + 100ms 退避，下一轮重试。 */
        if (!opened_)
        {
            if (!h->control_plane_.open(topic_control_name_for(h->raw_topic_name_, h->domain_id_)))
            {
                if (!open_warned_)
                {
                    open_warned_ = true;
                    /* ⛔ 不受 verbose_ 约束：这是"该话题将一直收不到控制面动作"的
                     * 静默失效类，要求开调试开关才看得见正是要避免的事
                     * （先例 shm_pub_sub_ipc.cc 的 connection-slots-exhausted 告警）。 */
                    std::cerr << "\033[31m[" << h->topic_name_
                              << "SubInfo] Error opening control plane for topic: " << h->topic_name_
                              << "; will retry every 100ms (this subscriber receives no control-plane "
                                 "action until it succeeds)\033[0m" << std::endl;
                }
                backoff_until_ = now + std::chrono::milliseconds(100);
                return;
            }
            opened_ = true;
        }

        const uint32_t generation = h->control_plane_.generation();
        const TopicState state = h->control_plane_.state();
        if (state == TopicState::Ready && generation != 0)
        {
            if (!h->handshake_completed.load(std::memory_order_acquire) || attached_generation_ != generation)
            {
                if (peer_registered_)
                {
                    h->control_plane_.remove_peer(attached_generation_);
                    h->control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered_ = false;
                }
                {
                    /* 换成新 generation 的 route(阶段 2 说明 §4 表格第 1 行)。
                     * begin_rebuild 内部顺序: 锁内置 rebuilding_ → 锁外 disconnect
                     * 旧 route(叫醒卡住的 recv) → 等 inflight 归零 → 锁内 release 旧
                     * route → 锁外 create 新 route → 锁内发布。所以 release 旧 route
                     * 与 recv 不再可能并发 —— 这正是原先 channel_mtx_ 想挡的事。
                     * 发布端已推进到新 generation, 旧 route 的共享同步对象可能已被
                     * clear, 故旧 route 一律不再被操作。 */
                    h->route_session_.begin_rebuild(
                        generation,
                        [h]()
                        {
                            return std::make_shared<ipc::route>(h->topic_name_.c_str(), ipc::receiver, h->verbose_);
                        });
                }
                attached_generation_ = generation;
                if (!h->control_plane_.add_peer(attached_generation_))
                {
                    /* add_peer 失败: 清空 route 且**不**建新对象(阶段 2 说明 §4
                     * 表格第 2 行)。stop_and_wake 拒绝新 lease 并叫醒在途 recv;
                     * wait_quiescent 之后对 current_route() 的拷贝调 release() 才安全。
                     * 下一轮循环会因 handshake_completed 仍为 false 而重新
                     * begin_rebuild, 即「不置 handshake_completed、稍后重试」。 */
                    h->route_session_.stop_and_wake();
                    h->route_session_.wait_quiescent();
                    std::shared_ptr<ipc::route> cur = h->route_session_.current_route();
                    if (cur && cur->valid())
                    {
                        cur->release();
                    }
                    return;
                }
                peer_registered_ = true;
                /* 向控制面登记本订阅者的 libipc 连接 bit, 并由下面的循环持续
                 * 刷新心跳。发布端据此判定死连接 —— 取代了 force_push 里那套
                 * "没读完就算无效读者"的误伤逻辑。 */
                uint32_t cc_id = 0u;
                {
                    /* begin_rebuild 返回之后读 route: 此刻没有并发的 release
                     * (阶段 2 说明 §4 表格第 3 行)。current_route() 的拷贝让 route
                     * 在读取 connected_id() 期间保活。 */
                    std::shared_ptr<ipc::route> cur = h->route_session_.current_route();
                    cc_id = (cur && cur->valid()) ? cur->connected_id() : 0u;
                    peer_slot_ = h->control_plane_.acquire_peer_slot(attached_generation_, cc_id);
                }

                /* 连接位耗尽 ⇒ **不能**宣布握手完成。
                 *
                 * libipc 的接收方连接位图是 cc_t = uint32_t, 只有 32 位; 位满时
                 * connect() 返回 0(circ/elem_def.h 的 "connection-slot is full")。旧实现
                 * 拿到 cc_id == 0 之后照样 handshake_completed.store(true), 于是第 33 个
                 * 订阅者进入一种**假成功态**: InitChannel 不报错、日志正常、
                 * handshake_completed 为真, 但一条消息都收不到 —— 而发布端只用
                 * recv_count() 判有无接收者, 两端都看不见这个截断。
                 *
                 * 控制面的 PeerSlot 表是 64 槽而连接位只有 32 个, 这个 2× 差额正是黑洞的
                 * 容量。acquire_peer_slot 本来就在 cc_id == 0 时返回 -1 —— 信号一直都在,
                 * 只是被丢掉了。见 docs/shm_defect_fixes.md 第 2 条。
                 *
                 * 处置: 不置 handshake_completed, 退掉已登记的 peer, 让下一轮重试 ——
                 * 有订阅者退出让出位时就能接上。告警**不受 verbose_ 约束**: 这是静默失败,
                 * 不该要求开了调试开关才看得见。
                 *
                 * ⚠️ 迁移点: 原实现此处 `sleep(100ms) + continue`；调度线程里 sleep 会
                 * 让同进程**全部**话题的控制面一起等这 100ms。改为项内 deadline 退避 ——
                 * 重试周期逐位保持 100ms，扫描耗时不再被它拖长。 */
                if (cc_id == 0)
                {
                    static std::atomic<bool> warned_once{false};
                    if (!warned_once.exchange(true, std::memory_order_relaxed))
                    {
                        std::cerr << "\033[31m[" << h->topic_name_
                                  << "SubInfo] connection slots exhausted (max "
                                  << kMaxShmReceiversPerTopic
                                  << " receivers per topic); this subscriber is NOT connected and "
                                     "will receive nothing. Retrying until a slot frees up.\033[0m"
                                  << std::endl;
                    }
                    if (peer_registered_)
                    {
                        h->control_plane_.remove_peer(attached_generation_);
                        peer_registered_ = false;
                    }
                    backoff_until_ = now + std::chrono::milliseconds(100);
                    return;
                }

                if (peer_slot_ < 0 && h->verbose_)
                {
                    std::cerr << "\033[33m[" << h->topic_name_
                              << "SubInfo] no free peer slot in control plane; this subscriber "
                                 "will not be reaped automatically if it dies\033[0m"
                              << std::endl;
                }
                h->handshake_completed.store(true, std::memory_order_release);
                if (h->verbose_)
                {
                    std::cerr << "\033[32m[" << h->topic_name_
                              << "SubInfo] Subscriber has subscribed to topic: " << h->topic_name_ << "\033[0m"
                              << std::endl;
                }
            }
        }
        else
        {
            if (h->handshake_completed.exchange(false, std::memory_order_acq_rel))
            {
                {
                    /* 控制面离开 Ready: 与 add_peer 失败同处置(阶段 2 说明 §4
                     * 表格第 4 行)—— 清空 route 且不建新对象。 */
                    h->route_session_.stop_and_wake();
                    h->route_session_.wait_quiescent();
                    std::shared_ptr<ipc::route> cur = h->route_session_.current_route();
                    if (cur && cur->valid())
                    {
                        cur->release();
                    }
                }
                if (peer_registered_)
                {
                    h->control_plane_.remove_peer(attached_generation_);
                    h->control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered_ = false;
                }
            }
        }
        /* 心跳: 只要本订阅者进程还活着, 这里就会每 10ms 刷新一次。
         * 进程崩溃后心跳停止, 发布端超时即可安全回收其连接。 */
        h->control_plane_.peer_heartbeat(peer_slot_);
    }

    /* 原 `sub_handshake()` 循环退出后的收尾（remove_peer + release_peer_slot）。
     * 调度路径没有"线程退出"这一刻，由宿主析构体在**同步注销令牌之后**显式调用；
     * 兼容路径由驱动线程退出时调用。幂等：两次调用只有第一次生效。 */
    void detach_on_exit() noexcept
    {
        shm_sub_ipc* h = host_;
        if (h != nullptr && peer_registered_)
        {
            h->control_plane_.remove_peer(attached_generation_);
        }
        if (h != nullptr)
        {
            h->control_plane_.release_peer_slot(peer_slot_);
        }
        peer_slot_ = -1;
        peer_registered_ = false;
    }

    shm_sub_ipc* host_{nullptr};
    /* ---- 项内记账（⛔ 不得提到 topic 级）---- */
    bool opened_{false};
    bool open_warned_{false};
    bool peer_registered_{false};
    uint32_t attached_generation_{0};
    int peer_slot_{-1};
    dzIPC::shm_control::ControlClock::time_point backoff_until_{};
};

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 兼容驱动线程（显式回退路径）：跑**同一份**回调体，周期 = sub_heartbeat = 10ms。
 * 与调度路径的唯一差别就是"谁按时调它"；收尾（detach_on_exit）在循环退出后做，
 * 与旧 `sub_handshake()` 的循环尾逐句相同。 */
void shm_sub_ipc::compat_control_loop()
{
    while (running.load(std::memory_order_acquire))
    {
        if (sub_control_state_ != nullptr)
        {
            sub_control_state_->on_sub_heartbeat(dzIPC::shm_control::ControlClock::now());
        }
        std::this_thread::sleep_for(kControlTiming.sub_heartbeat);
    }
    if (sub_control_state_ != nullptr)
    {
        sub_control_state_->detach_on_exit();
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 同步停止控制面驱动（幂等）。⛔ 必须在销毁任何被回调引用的成员之前调用。 */
void shm_sub_ipc::stop_control_plane() noexcept
{
    control_reg_.reset();   /* 同步：等**本项**在途 tick 结算；幂等 */
    if (compat_control_thread_ != nullptr)
    {
        /* ⛔ 先置 running=false 再 join：兼容线程的循环条件就是它（同发布端）。 */
        running.store(false, std::memory_order_release);
        if (compat_control_thread_->joinable())
        {
            compat_control_thread_->join();
        }
        delete compat_control_thread_;
        compat_control_thread_ = nullptr;
    }
    if (sub_control_state_ != nullptr)
    {
        /* 原 `sub_handshake()` 循环退出后的收尾：remove_peer + release_peer_slot。
         * 调度路径没有"线程退出"这一刻，所以在注销令牌（+ join 兼容线程）之后显式
         * 做；此时 control_plane_ 还没被析构。幂等：兼容路径上驱动线程已做过。 */
        sub_control_state_->detach_on_exit();
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::~shm_sub_ipc()
{
    /* ---- 第 1 步：先注销 LocalPubSubRegistry，快路径投递立刻断源（原有顺序不变）---- */
    {
        std::lock_guard<std::mutex> lock(sub_state_->topic_msg_mtx);
        if (local_registered_)
        {
            ChannelKey key{raw_topic_name_, domain_id_, msg_id_};
            LocalPubSubRegistry::instance().unregister_subscriber(key, sub_state_->msg_queue);
            local_registered_ = false;
        }
    }
    /* 测试缝(阶段2_全量测试方案 §4.2): 记录"registry 注销发生在收包停止之前"这一条
     * 因果序。默认钩子为空 ⇒ 只有一次 relaxed load。 */
    detail::FireSeam({detail::SeamPoint::kDtorAfterUnregister, 0, nullptr, nullptr, 0});

    /* ---- 第 2 步（W05 新增，W04 §4.3 的"注销控制面调度器"）：同步摘除本项 ----
     *
     * ⛔ 位置承重：必须在销毁任何回调会触及的成员（topic_name_ / control_plane_ /
     * route_session_ / sub_state_）**之前**。RegistrationToken::reset() 是同步的 ——
     * 标记 inactive → 唤醒调度器 → 等**本项**在途 tick 结算 → 摘除 → 返回。返回后
     * "调度器回调 → 已析构宿主"这条路径不存在（W04 §10 准入第 3 条）。
     * ⛔ 不得从回调内部调用本函数（会等一个只有本回调返回后才被清除的标志）；
     * 这里在**析构线程**上调用，符合契约。
     * ⛔ W06 施工位：按 W04 §4.3 的完整顺序，`pool.remove_route(route_adapter)` 应在
     * **本步之前**（即紧接 registry 注销之后）、`begin_rebuild` 之前；今天的 W05 里
     * 该 route 尚未注册进 worker 池（接收池接入属 t7/W06），因此本步不存在
     * "wait-set 仍持有旧 route 的 seq 地址"这一风险面。W06 接入时在此行之前插入
     * remove_route，并在 begin_rebuild 之后插入 add_route。 */
    stop_control_plane();   /* 含 detach_on_exit（remove_peer + release_peer_slot） */

    /* 阶段 2 说明 §5: running = false 只能让循环在 recv 返回后退出; 卡在
     * recv(50) 里时必须靠 stop_and_wake() 的 disconnect/quit_waiting 叫醒, 不能
     * 只靠 50ms 超时。stop_and_wake 同时拒绝新 lease, 于是 inflight 只减不增。 */
    running.store(false, std::memory_order_release);
    route_session_.stop_and_wake();
    /* §4.2 的承重点: 析构必须**叫醒**在途 recv, 不能只靠 recv(50) 超时。 */
    detail::FireSeam({detail::SeamPoint::kDtorAfterStopAndWake, 0, nullptr, nullptr, 0});

    /* 先 join 收包线程: 它退出前会做完最后一次 release_receive, 使 inflight 归零;
     * 控制面回调若正卡在 begin_rebuild 的等待里, 也由此得以推进（并因令牌已在上面
     * reset、不会再被调用）。析构线程不得持有 RouteSession 锁时 join —— 这里没有持锁。 */
    if (subscribe_thread_ != nullptr)
    {
        if (subscribe_thread_->joinable())
        {
            subscribe_thread_->join();
        }
        delete subscribe_thread_;
        subscribe_thread_ = nullptr;
    }
    detail::FireSeam({detail::SeamPoint::kDtorAfterJoinSubscribe, 0, nullptr, nullptr, 0});
    /* ⚠️ 兼容回退路径的驱动线程已在 stop_control_plane() 里 join（W05 之前这里是
     * `sub_handshake_thread_` 的 join 点）。**本打点必须保留**：它是析构八步因果序
     * 的组成（test_shm_sub_dtor_gate 断言 6 个点严格递增、顺序 16→21），且语义仍成立
     * ——"没有任何控制面回调在跑了"在这一刻已由上面两步（令牌 reset + join）共同保证。
     * 若将来把控制面驱动换回线程，这里就是它的 join 点。 */
    detail::FireSeam({detail::SeamPoint::kDtorAfterJoinHandshake, 0, nullptr, nullptr, 0});

    /* 第 6-7 步: 两个线程都已退出 ⇒ 无在途 recv, 此刻对 current_route() 的拷贝调
     * release() 与 recv 不并发(说明 §4 表格第 2 行)。disconnect 已由 stop_and_wake
     * 做过, 这里只释放句柄; 对象本身的 shared_ptr 由 route_session_ 析构时放掉。 */
    route_session_.wait_quiescent();
    detail::FireSeam({detail::SeamPoint::kDtorAfterQuiescent, 0, nullptr, nullptr, 0});
    {
        std::shared_ptr<ipc::route> cur = route_session_.current_route();
        if (cur && cur->valid())
        {
            cur->release();
        }
    }
    detail::FireSeam({detail::SeamPoint::kDtorAfterRelease, 0, nullptr, nullptr, 0});
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<TopicData> new_msg;
    new_msg.reset(msg->clone());
    const uint32_t new_msg_id = new_msg->topic()->msg_id();

    {
        std::lock_guard<std::mutex> lock(sub_state_->topic_msg_mtx);
        sub_state_->topic_msg = std::move(new_msg);

        // If already registered (InitChannel completed) and msg_id changed,
        // re-register under the new key so publishers using the new msg_id
        // can find us via the fast path.
        if (local_registered_ && msg_id_ != new_msg_id)
        {
            auto& reg = LocalPubSubRegistry::instance();
            ChannelKey old_key{raw_topic_name_, domain_id_, msg_id_};
            ChannelKey new_key{raw_topic_name_, domain_id_, new_msg_id};
            reg.unregister_subscriber(old_key, sub_state_->msg_queue);
            reg.register_subscriber(new_key, sub_state_->msg_queue);
        }
        msg_id_ = new_msg_id;
        sub_state_->msg_id = new_msg_id;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::InitChannel(std::string extra_info)
{
    std::shared_ptr<TopicData> topic_template;
    {
        std::lock_guard<std::mutex> lock(sub_state_->topic_msg_mtx);
        topic_template = sub_state_->topic_msg;
    }
    std::string topic_type_name =
        (topic_template && topic_template->topic())
            ? dzIPC::info_pool::demangle(typeid(*topic_template->topic()).name())
            : std::string{};
    topic_type_name = extract_last_segment(topic_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmSub, raw_topic_name_, topic_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    /* ===== W05：控制面驱动（取代 per-route `sub_handshake_thread_`）=====
     *
     * 顺序承重：控制面驱动必须**先于**收包线程就绪。原因有两个，都跟"接管窗口"有关：
     *   ① 收包线程只认 `handshake_completed`；先起它只是空转，语义无害但没必要；
     *   ② 反过来的顺序（先收包线程）在 shut-down 竞态里更差：析构第一步就是同步注销
     *      控制面 + join 兼容线程，而收包线程此时可能还在 50ms sleep 分支上 —— 与旧
     *      实现同型（旧实现也是先起握手线程）。这里保持旧次序，只是驱动源换了。
     *
     * ⛔ 先建 state、再注册（RegistrationToken 的"先注册后构造"形式）：
     *    避免 `(sched, sched.register_...)` 那种"注册成功但令牌构造抛异常 ⇒ 条目泄漏、
     *    永不被注销"的写法（W04 §10 准入第 3 条）。
     * ⛔ 幂等：重复 InitChannel 不得叠加第二个驱动。 */
    if (sub_control_state_ == nullptr && compat_control_thread_ == nullptr)
    {
        sub_control_state_ = std::make_shared<SubHandshakeState>(this);
        if (control_compat_forced())
        {
            compat_control_thread_ = new std::thread(&shm_sub_ipc::compat_control_loop, this);
        }
        else if (!control_scheduler_allowed_in_this_process())
        {
            /* fork 闸，同发布端：子进程不继承调度器 worker 线程。 */
            std::cerr << "\033[33m[" << topic_name_
                      << "SubInfo] forked child: ShmControlScheduler owner pid mismatch; "
                         "keeping per-topic control thread\033[0m" << std::endl;
            compat_control_thread_ = new std::thread(&shm_sub_ipc::compat_control_loop, this);
        }
        else if (!dzIPC::shm_control::ShmControlScheduler::instance().worker_active())
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "SubInfo] ShmControlScheduler 已停止; 退回每话题兼容控制线程\033[0m" << std::endl;
            compat_control_thread_ = new std::thread(&shm_sub_ipc::compat_control_loop, this);
        }
        else
        {
            dzIPC::shm_control::RegistrationToken reg{dzIPC::shm_control::ShmControlScheduler::instance(),
                                                      sub_control_state_, kControlTiming};
            control_reg_ = std::move(reg);
            if (!control_reg_.valid())
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "SubInfo] ShmControlScheduler 注册失败; 退回每话题兼容控制线程\033[0m" << std::endl;
                compat_control_thread_ = new std::thread(&shm_sub_ipc::compat_control_loop, this);
            }
        }
    }
    subscribe_thread_ = new std::thread(
        [this]()
        {
            /* 进入订阅循环 */
            while (running.load(std::memory_order_acquire))
            {
                if (handshake_completed.load(std::memory_order_acquire))
                {
                    /* 阶段 2 说明 §3: 用 lease 取 route, 在**不持有 RouteSession 锁**
                     * 的情况下 recv, recv 返回后立刻 release_receive(无论 buffer 是否
                     * 为空)。lease 的 shared_ptr 在 recv 全程保活 route; release 旧 route
                     * 只可能发生在 inflight 归零之后, 所以 recv 与 release 不再并发。
                     * ⚠️ 不得因 lease.generation 落后于当前 generation 就丢掉已弹出的
                     * buffer —— 字节已从旧 route 弹出, 丢掉就是丢消息(说明 §3)。 */
                    auto lease = route_session_.acquire_receive();
                    if (!lease.has_value())
                    {
                        /* stopping / rebuilding / 尚无 route: 与未握手时同样睡 50ms
                         * (说明 §3), 不再忙等自旋。 */
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                    buff_t raw_data;
                    try
                    {
                        raw_data = lease->route->recv(50);
                    }
                    catch (...)
                    {
                        /* 阶段 2 说明 §2/§3: 每个成功 lease 必须配对释放，包括 recv
                         * 抛异常的出口。单次 route 错误不应让工作线程因未配对 lease
                         * 卡死后续重建；释放后按未收到数据处理，继续循环。 */
                        route_session_.release_receive();
                        continue;
                    }
                    /* 测试缝(§4.2): 记录这次 recv 的返回形状。钩子读 route->connected_id()
                     * 即可判定"是 disconnect 叫醒(0)还是真的收到消息(非 0)" —— 这是
                     * 析构守门里"叫醒而非超时"那条判据的观测面。
                     * ⚠️ 此刻 inflight 仍为 1 ⇒ 钩子在这里**不得阻塞**: 阻塞会让任何
                     * 并发的 begin_rebuild 卡在第 4 步等归零。要暂停请用下面那个点。 */
                    detail::FireSeam({detail::SeamPoint::kAfterRecv, lease->generation,
                                      lease->route.get(), raw_data.data(), raw_data.size()});
                    route_session_.release_receive();
                    /* 测试缝(§4.1 的 I5 暂停点): recv 已返回、buffer 已在本线程手里, 而
                     * inflight 已归零(所以重建方能推进到 §4 第 5 步 release 旧 route)。
                     * 用例在这里把本线程停住, 去推进 generation, 再放行 —— 从而证明
                     * "已弹出的字节不会因为 lease.generation 落后于当前 generation 而
                     * 被丢弃"。**本点是唯一允许阻塞的收包点**(钩子内可阻塞, 见
                     * shm_sub_seam.h 的设计约束)。⚠️ 必须留在**分流之前**: 阶段 3 提取
                     * process_received_buffer 时本点随函数体一并迁移(见 shm_sub_seam.h)。 */
                    detail::FireSeam({detail::SeamPoint::kAfterRecvRelease, lease->generation,
                                      lease->route.get(), raw_data.data(), raw_data.size()});
                    if (raw_data.empty())
                    {
                        continue;
                    }
                    process_received_buffer(sub_state_, std::move(raw_data));
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    dzIPC::ThreadDispatch::apply_thread_options(subscribe_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SubReceiveThread");

    // Register for intra-process fast-path delivery (once only).
    {
        std::lock_guard<std::mutex> lock(sub_state_->topic_msg_mtx);
        if (!local_registered_)
        {
            ChannelKey key{raw_topic_name_, domain_id_, msg_id_};
            LocalPubSubRegistry::instance().register_subscriber(key, sub_state_->msg_queue);
            local_registered_ = true;
        }
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* ---- 视图路径: 只服务借样的 DZFlat 段(见 subscribe 循环的分流) ---- */
void shm_sub_ipc::get(Sample& out)
{
    std::shared_ptr<Sample> s;
    sub_state_->view_queue->pop(s);   /* 阻塞直到有 Sample; TLV-only 话题请用 get_clone */
    if (s)
    {
        out = std::move(*s);
    }
}

bool shm_sub_ipc::get(Sample& out, std::uint64_t tm_ms)
{
    /* CircularQueue::pop 本来就支持超时(见其 tm 参数), 之前只是没接线 —— 于是只发 TLV 的
     * 话题上调 get(Sample&) 会永久挂死。见 docs/shm_defect_fixes.md 第 4 条。 */
    std::shared_ptr<Sample> s;
    if (!sub_state_->view_queue->pop(s, tm_ms))
    {
        return false;   // 超时
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

bool shm_sub_ipc::try_get(Sample& out)
{
    std::shared_ptr<Sample> s;
    if (!sub_state_->view_queue->try_pop(s))
    {
        return false;
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

void shm_sub_ipc::get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    sub_state_->msg_queue->pop(ipc_msg);
    /* 出队即离开配额账面(消息可能带着借样移交给用户 —— 与 view 路径同一契约:
     * 队列驻留有上限, 用户手持期是用户的约定)。 */
    if (ipc_msg && ipc_msg->dzflat_is_borrowed())
    {
        sub_state_->adopt_borrowed.fetch_sub(1, std::memory_order_relaxed);
    }
    msg->update(ipc_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_sub_ipc::try_get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    if (sub_state_->msg_queue->try_pop(ipc_msg))
    {
        if (ipc_msg && ipc_msg->dzflat_is_borrowed())
        {
            sub_state_->adopt_borrowed.fetch_sub(1, std::memory_order_relaxed);
        }
        msg->update(ipc_msg);
        return true;
    }
    else
    {
        return false;
    }
}
}   // namespace shm
}   // namespace dzIPC
