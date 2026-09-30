#pragma once
/* 进程级 SHM 控制面调度器（阶段 1）—— 用 1 条线程替代每订阅/每发布者一条握手线程。
 *
 * 现状（本头文件落地前的对照物）：每个 shm_sub_ipc 一条 sub_handshake_thread_、
 * 每个 shm_pub_ipc 一条 publish_thread_，各自 sleep(10ms) / sleep(50ms) 轮询。
 * 1000 个订阅者 ⇒ 1000 条线程、约 10 万次/秒空闲唤醒（见
 * docs/消息接收架构改造/shm_sub_thread_consolidation_plan.md §2 的规模估算）。
 * 本调度器把"控制面周期动作"收成每进程一条线程，线程数从 O(话题数) 降为 O(1)。
 *
 * ⛔ 本阶段**只覆盖控制面**：不接管数据面、不接管 recv()、不接管
 *    get*、get_clone 与队列。收包线程、channel_mtx_ 内 recv(50)、
 *    view_queue_/msg_queue_ 的语义一行不动（接入数据面是阶段 2/3 的事）。
 *
 * ---- 时间语义（必须逐位保持，需求 §3.1）----
 *   订阅 heartbeat        10ms   （现状 src/dzIPC/shm_pub_sub_ipc.cc:754）
 *   发布 owner heartbeat  50ms   （现状 src/dzIPC/shm_pub_sub_ipc.cc:179）
 *   发布 stale 扫描       50ms   （与 owner heartbeat 同频；现状同上）
 *   peer 判死超时         2s     （现状 src/dzIPC/shm_pub_sub_ipc.cc:158）
 * 周期按**项**传入（ControlTiming），调度器不做全局假设 —— 将来要给实时话题
 * 单独调周期，不必改调度器。第一版全部用默认值。
 *
 * ---- 两条实现约束（写在接口上，不是可选建议）----
 * ① ⛔ 回调**不得抛出**。实现内部自己 try/catch。理由：现状
 *    control_plane_.open() 失败是在**该订阅自己的线程**里 throw
 *    （src/dzIPC/shm_pub_sub_ipc.cc:618-623），未捕获 ⇒ 该进程 terminate；
 *    迁进调度线程后同一失败会带走**该进程全部话题**的控制面。
 *    调度器仍会在 tick 外层兜底 catch 并隔离坏项（见下），但那是最后一道网，
 *    不是让实现偷懒的地方。
 * ② ⛔ 回调内**不得**调用 ShmControlScheduler::unregister。unregister 要等
 *    "正在执行的 tick 完成"，而该 tick 正是当前这次回调 —— 会永久等待。
 *    调度器检测到同线程注销会打印诊断并直接返回（不等待），但这是防误用的
 *    兜底，不是支持的用法。
 * ③ ⛔ 回调内**不得**调用 ShmControlScheduler::stop。stop 要 join worker 线程，
 *    而在 worker 线程里 join 自己即 std::system_error("Resource deadlock avoided")
 *    —— stop 标了 noexcept，于是直接 std::terminate（**实测复现**：进程 SIGABRT）。
 *    调度器检测到同线程 stop 会只置停止标志、**不 join**，把 join 推迟到析构或
 *    后续外部线程的 stop()；同样只是兜底，不是支持的用法。
 *    ⚠️ 两个"stop"是两件不同的事，实现里必须是**两个**状态：请求停止（幂等，
 *    任何线程可置）与执行 join（恰好一个线程做）。早期实现用一个 exchange 同时
 *    表达两者，于是"回调内 stop"会把 join 所有权一并吞掉 —— 之后外部线程的
 *    stop() 直接返回、不 join，join 只剩析构一条路（析构若由同一个回调触发，
 *    就是 join 自己 ⇒ terminate）。
 *
 * ---- 可扩展性 ----
 * 新控制动作一律以"带默认实现的 virtual"加入（如 debug_name()），这样既有实现
 * 不需要改一行；只有真正的必要动作才做成纯虚。这样阶段 2/3 增加动作时不会
 * 破坏已有派生类。
 *
 * 与需求文档《事件驱动线程池需求.md》§3.1 的签名差异（3 处，均向后兼容）：
 *   - register_* 追加默认参数 const ControlTiming& timing = ControlTiming{}；
 *   - 规定 kInvalidEntry = 0（EntryId 单调递增、永不复用，避免 ABA）；
 *   - 增加 Stats/stats()/entry_count()（需求 §10 阶段 1 要求记录扫描耗时与最大抖动）。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include "libipc/export.h"

namespace dzIPC {
namespace shm_control {

/// 调度器与被调度方共用的时钟。用 steady_clock：心跳与判死只关心"经过多久"，
/// 与墙钟跳变无关（现状 control_plane.cc 的 now_ns() 同源）。
using ControlClock = std::chrono::steady_clock;

/* 单个注册项的周期配置。默认值即第一版必须保持的时间语义（见文件头）。 */
struct ControlTiming
{
    std::chrono::milliseconds sub_heartbeat{10};
    std::chrono::milliseconds pub_heartbeat{50};
    std::chrono::nanoseconds peer_dead_timeout{2'000'000'000LL};
};

/* 订阅侧控制动作的抽象。实现者持有 shm_sub_ipc 的成员状态。
 *
 * 一次 on_sub_heartbeat 等价于现状 sub_handshake() 的一次循环体
 * （src/dzIPC/shm_pub_sub_ipc.cc:626-755）：
 *   首次打开控制面 → 读 generation()/state() → generation 重建分支
 *   → detach 分支 → peer_heartbeat() 刷新。
 * 那些"每项自己的记账"（peer_registered / attached_generation / peer_slot_）
 * 必须留在**实现里**，不能提到 topic 级：add_peer/remove_peer 改的是共享段里的
 * peer_count（src/dzIPC/common/control_plane.cc:164-185），同进程两个订阅项若
 * 共用记账会重复 add_peer，让 peer_count 偏大、发布端 subscribed_ 与 stale
 * 判定一起失真。
 *
 * 退避（例如连接位耗尽后 100ms 重试）由实现自己记一个 deadline 实现，调度器
 * 不提供返回值通道 —— 这样接口保持 void，实现也不必知道调度器的内部状态。 */
class IPC_EXPORT SubControlState
{
public:
    virtual ~SubControlState() = default;

    virtual void on_sub_heartbeat(ControlClock::time_point now) = 0;

    /// 诊断用名字（可为空）。仅在异常隔离/日志里使用，不得有副作用。
    virtual const char* debug_name() const noexcept { return nullptr; }
};

/* 发布侧控制动作的抽象。
 *
 * 与 SubControlState 一样标 IPC_EXPORT：两者都是**跨库边界**被派生实现的抽象基类
 * （派生类在 libipc 内，宿主也在 libipc 内，但 vtable/RTTI 必须跨 .so 边界一致）。
 * Linux 上 IPC_EXPORT 当前展开为空（include/libipc/export.h），一旦引入
 * -fvisibility=hidden，未标注的基类会导致 vtable 失配。 */
class IPC_EXPORT PubControlState
{
public:
    virtual ~PubControlState() = default;

    /* 每 pub_heartbeat：owner heartbeat + subscribed_ 刷新 + verbose 状态迁移日志。
     * ⚠️ owner heartbeat **必须无条件执行**，即使 peer_count()==0：发布端没有
     * 订阅者时也要让 owner 心跳延续，否则别的进程无法判断本发布者是否还活着
     * （需求 §3.1："即使没有 peer，发布端仍需维持 owner heartbeat"）。 */
    virtual void on_pub_heartbeat(ControlClock::time_point now) = 0;

    /// peer_count() > 0。为 false 时调度器**跳过** on_pub_stale_scan。
    virtual bool has_peers() const = 0;

    /* 每 pub_heartbeat，**仅当 has_peers() 为真**：
     * collect_stale_peers(dead_timeout) + disconnect_receivers(stale)
     * （现状 src/dzIPC/shm_pub_sub_ipc.cc:159-169）。 */
    virtual void on_pub_stale_scan(ControlClock::time_point now, std::chrono::nanoseconds dead_timeout) = 0;

    /// 诊断用名字（可为空）。语义同 SubControlState::debug_name()。
    virtual const char* debug_name() const noexcept { return nullptr; }
};

/* 进程级控制面调度器：1 条 worker 线程，按项到期驱动。
 *
 * 线程数契约：worker 在构造期启动，此后注册任意多项都**不再新增线程**
 * （原型实测：注册 200 项线程数零增长）。
 *
 * 空闲唤醒契约：worker 计算"距最近到期项的等待时长"并 cv.wait_for，**不是**
 * 固定 10ms 轮询 —— 只有发布项在册时等待就是 50ms，无任何项时无限期 cv.wait。
 *
 * 与 LocalPubSubRegistry::instance() 同构：**故意泄漏的指针单例**
 * （理由见 src/dzIPC/common/local_pub_sub_registry.cc:14-24 —— 函数内静态对象的
 * 析构顺序相对全局/静态 shm_sub_ipc / shm_pub_ipc 的析构顺序未定义，而后者析构
 * 时必须调用 unregister()，调度器必须活得比它们久）。 */
class IPC_EXPORT ShmControlScheduler
{
public:
    using EntryId = std::uint64_t;
    static constexpr EntryId kInvalidEntry = 0;

    /* 可观测性：需求 §10 阶段 1 要求"记录扫描耗时和最大抖动"。
     * 没有读数就无法验收"唤醒/上下文切换下降"，也无法定位回归。 */
    struct Stats
    {
        std::uint64_t tick_count{0};             ///< 已执行的 tick 轮数
        std::uint64_t tick_overrun_count{0};     ///< 单轮耗时 > 该轮最小到期周期的轮数
        std::uint64_t tick_deferred_count{0};    ///< 因等待在途 tick 而被延迟的注销次数
        std::uint64_t callback_exception_count{0};  ///< 被异常隔离摘除的注册项数
        std::int64_t tick_duration_last_ns{0};   ///< 最近一轮耗时
        std::int64_t tick_duration_max_ns{0};    ///< 最大轮耗时（扫描抖动上界）
        std::size_t entry_count{0};              ///< 当前注册项数
    };

    /* 生产代码用这个：进程级单例，故意泄漏，活得比所有全局对象久。
     * 构造/析构是公开的（便于测试与需要独立实例的场景，如分阶段回滚验证）。
     * 独立实例场景下 RegistrationToken 可以比调度器活得久：令牌与调度器**共享**
     * Impl 的生命周期（见 private 区 impl_ 注释），令牌后析构只是让 Impl 的析构
     * 推迟，而那时 worker 早已 join、entries 已清空 ⇒ 不产生任何回调。 */
    static ShmControlScheduler& instance();

    ShmControlScheduler();
    ~ShmControlScheduler();

    ShmControlScheduler(const ShmControlScheduler&) = delete;
    ShmControlScheduler& operator=(const ShmControlScheduler&) = delete;
    ShmControlScheduler(ShmControlScheduler&&) = delete;
    ShmControlScheduler& operator=(ShmControlScheduler&&) = delete;

    /* 注册。state 为空或调度器已 stop() 时返回 kInvalidEntry。
     * EntryId 单调递增、**永不复用**，0 保留为无效值 —— 否则迟到的
     * unregister(旧 id) 会注销别人的项（ABA）。
     * 注册**不执行任何回调**：第一次回调在下一个到期点。 */
    EntryId register_subscriber(std::shared_ptr<SubControlState> state, const ControlTiming& timing = ControlTiming{});
    EntryId register_publisher(std::shared_ptr<PubControlState> state, const ControlTiming& timing = ControlTiming{});

    /* 同步注销：标记 inactive → 唤醒调度器 → 等待**该项**的在途 tick 结算 → 摘除 → 返回。
     *
     * 返回后的三条保证（验收直接断言这些）：
     *   ① 不再发起该 id 的新回调 —— 含"同轮 due 内已被选中、但尚未回调"的项：
     *      tick 在每次 dispatch 前复查 inactive（I1 对同轮同样成立）；
     *   ② 在途回调已结算（不会"返回了但回调还在跑"）；
     *   ③ 调度器不再持有 state（shared_ptr 计数已回落，对象可安全析构）。
     *
     * ⚠️ 等待粒度是**该项**，不是整轮 tick：同轮其它项的回调与注销都不受影响。
     *    （早期协议写的是"等正在执行的 tick 完成"，那会让一个慢项阻塞同轮所有项的
     *    注销；实现改为按项结算，见 docs/消息接收架构改造/阶段1_控制面调度器实现说明.md §4。）
     * 幂等：kInvalidEntry / 未知 id / 重复注销都是无操作。
     * 可从任意线程调用。⛔ 不得从回调内部注销**该项自身**：那会等一个只有本回调返回后
     *    才会被清除的标志 ⇒ 永久阻塞。实现检测到 worker 线程内调用时跳过等待、只置
     *    inactive（tick 结算阶段照常清零），并打印诊断 —— 这是防误用兜底，不是支持的用法。
     * noexcept：任何异常都不允许逃出（互斥量/条件变量异常属进程级不可恢复错误）。 */
    void unregister(EntryId id) noexcept;

    /* 唤醒调度线程立即重算到期。不改变任何注册项，也不改变 next_due，
     * 因此**不会为未到期项产生额外回调**；仅用于让等待立刻结束并重新评估。 */
    void wakeup() noexcept;

    /* 停止调度线程：请求停止（幂等）→ 唤醒 → 由**恰好一个**调用者执行 join。
     * 幂等；**外部线程**调用的 stop() 返回后 worker_active() 必为 false ——
     * 包括"两个线程并发 stop"里那个没执行 join 的调用者（它等 join 真正完成
     * 再返回，否则调用方会看到"stop 已返回但线程还活着"）。
     * 此后 register_* 一律返回 kInvalidEntry（避免"注册了但永远不会被回调"）。
     * 析构会调用它。stop() 之后的 unregister 仍然安全（只是摘除条目）。
     * ⛔ 不得从回调内部调用（见文件头约束 ③）：此时只置停止标志、**不 join**
     *    并返回 —— 那一次 stop() 返回后 worker_active() 仍可能为 true（worker
     *    正在退出 loop 的路上），这是该误用唯一的例外。
     *    join 由析构或后续**外部线程**的 stop() 完成 —— 后者必须照常成为 joiner，
     *    因此"谁负责 join"与"是否已请求停止"是**两个**独立状态。 */
    void stop() noexcept;

    Stats stats() const;

    /// worker 是否仍在运行（stop() 返回后为 false）。线程安全。
    bool worker_active() const noexcept;

    /// 当前注册项数。线程安全。
    std::size_t entry_count() const noexcept;

    /* ⚠️ Impl 与 share_impl 必须在**公开**区（两者都是实现细节，不是产品契约）：
     *   · `RegistrationToken::reset()` 的**定义**要看到 Impl 的完整定义才能调用
     *     `impl_->unregister_entry(...)`。若 reset() 内联在头文件里，那一刻 Impl
     *     还是前向声明 ⇒ "invalid use of incomplete type"。因此 reset() 与
     *     移动赋值（会先析构旧 impl_，同样要求完整类型）都外置到 .cc；
     *   · share_impl 是令牌取得共享所有权的通道，而令牌是**独立类**（非 friend）。
     * 对外只暴露 register_* / unregister / stop / stats，内部结构不构成契约。 */
    struct Impl;
    static std::shared_ptr<Impl> share_impl(ShmControlScheduler& sched) noexcept { return sched.impl_; }

private:
    /* ⚠️ 用 shared_ptr 而不是 unique_ptr：RegistrationToken 必须能在调度器析构
     * **之后**安全 reset()。令牌持裸调度器指针时，栈上调度器先于令牌析构即
     * UAF（`sched_->unregister()` 打在已析构对象上）—— 头文件原先只用注释约束
     * "令牌不得比调度器活得久"，那是顺序约定而非机制。改为令牌持
     * shared_ptr<Impl> 后：
     *   · 调度器析构 → stop()（worker join、entries 清空）→ 释放自己那份 Impl；
     *   · 令牌后析构 → unregister 作用在一个已停止、entries 已空的 Impl 上
     *     （幂等无操作）→ 释放最后一份引用，Impl 这才真正析构。
     * 因此"令牌比调度器长寿"从 UB 变成安全，且不依赖任何声明顺序约定。
     * 生产路径上是故意泄漏的单例，Impl 永不析构，行为与原先逐位相同。 */
    std::shared_ptr<Impl> impl_;
};

/* RAII 注册令牌。持有它 = 该项在册；析构 / reset() = 同步注销。
 * 需求 §3.1："注册项必须通过 RAII 生命周期令牌持有，调度器不得保存裸 this"。
 *
 * 落地方式：调度器保存的是 std::shared_ptr<SubControlState> / <PubControlState>
 * （抽象基类），裸指针只存在于 shm_sub_ipc / shm_pub_ipc 内部（其状态实现以裸
 * 指针回指宿主）。宿主的析构体在销毁成员之前显式 reset() 令牌，因此
 * "调度器回调 → 已析构宿主"这条路径在类型层面就不存在。
 *
 * ⚠️ 令牌持的是**调度器内部状态的共享所有权**（shared_ptr<Impl>），不是裸调度器
 * 指针：调度器先析构时令牌照常 reset()，打在"已停止、entries 已空"的状态上（幂等
 * 无操作），随后才释放最后一份引用。栈上调度器 + 后声明的令牌（或令牌被搬进更长
 * 寿命的作用域）都因此安全，不依赖任何析构顺序约定。
 *
 * 默认构造即"未注册" ⇒ 未调 InitChannel() 就析构的对象无需特殊处理：
 * valid()==false，reset() 是无操作。 */
class RegistrationToken
{
public:
    RegistrationToken() noexcept = default;

    /* 直接用调度器构造：令牌自行注册并接管返回的 id。
     * ⛔ 不要用 (sched, sched.register_subscriber(...)) 那种写法：register_* 先于
     *    令牌构造执行，它一旦抛异常（bad_alloc）就已注册的项没有任何令牌接管 ⇒
     *    条目泄漏且永不被注销。本构造函数先注册、拿到 id 才构造令牌，中间无抛点。 */
    RegistrationToken(ShmControlScheduler& sched, std::shared_ptr<SubControlState> state,
                      const ControlTiming& timing = ControlTiming{}) noexcept
        : RegistrationToken(sched, sched.register_subscriber(std::move(state), timing))
    {}

    RegistrationToken(ShmControlScheduler& sched, std::shared_ptr<PubControlState> state,
                      const ControlTiming& timing = ControlTiming{}) noexcept
        : RegistrationToken(sched, sched.register_publisher(std::move(state), timing))
    {}

    /* 低层构造：接管一个**已经注册好**的 id（id == kInvalidEntry 时等价于未注册）。
     * 仍保留公开：产品接入点可以按需求 §3.1 的原始签名 `register_*` + 令牌接管。
     *
     * 两种首参形式**都**保留，因为它们分别有既有约束：
     *   · `(ShmControlScheduler*, EntryId)` —— 设计说明 §3.4 的原文签名，也是既有
     *     测试与调用方（`RegistrationToken reg(&sched, sched.register_subscriber(..))`）
     *     的写法。缺了它，全部既有调用点编译失败（实测 18 处 error）；
     *   · `(ShmControlScheduler&, EntryId)` —— 引用便利版，新代码可少写一个 `&`。
     * 两者不构成重载歧义：对象→指针没有隐式转换，指针→引用也没有。
     *
     * sched 为空指针时按"未注册"处理（id 丢弃），不会解引用空指针 —— 低层构造是
     * 公开 API，不能假设调用方一定传了有效地址。 */
    RegistrationToken(ShmControlScheduler* sched, ShmControlScheduler::EntryId id) noexcept
        : sched_(sched)
        , impl_((sched != nullptr) ? ShmControlScheduler::share_impl(*sched) : nullptr)
        , id_((sched != nullptr) ? id : ShmControlScheduler::kInvalidEntry)
    {}

    RegistrationToken(ShmControlScheduler& sched, ShmControlScheduler::EntryId id) noexcept
        : RegistrationToken(&sched, id)
    {}

    ~RegistrationToken() { reset(); }

    RegistrationToken(RegistrationToken&& other) noexcept
        : sched_(other.sched_)
        , impl_(std::move(other.impl_))
        , id_(other.id_)
    {
        other.sched_ = nullptr;
        other.id_ = ShmControlScheduler::kInvalidEntry;
    }

    /* ⚠️ 定义在 .cc：赋值会先 reset() 旧条目，而 reset() 需要 Impl 完整定义
     * （见下面 reset() 注释）。移动构造**不**需要外置 —— 它只移动 shared_ptr，
     * 不触碰 Impl 的成员，也不要求完整类型。 */
    RegistrationToken& operator=(RegistrationToken&& other) noexcept;

    RegistrationToken(const RegistrationToken&) = delete;
    RegistrationToken& operator=(const RegistrationToken&) = delete;

    ShmControlScheduler::EntryId id() const noexcept { return id_; }
    bool valid() const noexcept { return id_ != ShmControlScheduler::kInvalidEntry; }

    /* 同步注销并释放共享所有权。定义在 .cc：
     *   ⚠️ 走 Impl 直接注销，而不是 `sched_->unregister(id)`：调度器可能已经析构，
     *   那个指针此时是悬垂的（这正是本类要消除的缺陷）。Impl 是共享所有权，
     *   到这里一定还活着；调度器已析构时 unregister 是幂等无操作。
     *   （必须外置的原因：Impl 在头文件里只有前向声明。） */
    void reset() noexcept;

private:
    /* "不得保存裸 this"约束的是**调度器**一侧：调度器只持有 shared_ptr 状态对象，
     * 不持有宿主的裸指针。令牌反向持有调度器指针是 RAII 的必要条件，且该指针只
     * 在 sched_ != nullptr 期间用于诊断/兼容，注销本身走 impl_（见 reset()）。 */
    ShmControlScheduler* sched_{nullptr};
    std::shared_ptr<ShmControlScheduler::Impl> impl_;
    ShmControlScheduler::EntryId id_{ShmControlScheduler::kInvalidEntry};
};

/// 别名：设计说明（docs/消息接收架构改造/阶段1_控制面调度器实现说明.md §3.4）用的名字。
using ControlRegistration = RegistrationToken;

}   // namespace shm_control
}   // namespace dzIPC
