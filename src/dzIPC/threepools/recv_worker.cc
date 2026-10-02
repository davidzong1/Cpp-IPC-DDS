#include "dzIPC/threepools/recv_worker.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "libipc/utility/log.h"

/* §10.2 扫描/等待计数（W06 接线，D-23）。
 * ⛔ 只**引用**既有 ID 与既有 `ScanRoundScope`，不新增 ID、不改任何字段语义
 * （`include/dzIPC/measure/counters.h` 保持只读引用）。
 * 这些量的 `CounterMeta::diagnostics_only == true` ⇒ 只在诊断开启时累加；
 * 常开孪生量在 `RecvWorkerStats`（本文件同一批计数点旁），两者可互核。 */
#include "dzIPC/measure/counters.h"

namespace dzIPC {
namespace threepools {
namespace {

using Clock = std::chrono::steady_clock;

/* 与 dzIPC::common::fnv1a64（include/dzIPC/common/hash.h）**同一常量与算法**，
 * 只是把 domain_id 的 4 个字节接在 route_name 之后继续迭代，得到
 * FNV1a64(route_name ‖ domain_id)。
 *
 * 为什么内联而不调用 dzIPC::common::fnv1a64：归属计算必须是一个**纯函数**，
 * 且不得引入 std::string 构造（add_route 在话题建立路径上）与跨库符号依赖。
 * 前缀部分与 common::fnv1a64 对同一 ASCII 名字逐位一致，两者不会"同名不同值"。 */
constexpr std::uint64_t kFnvOffsetBasis64 = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime64 = 1099511628211ull;

std::uint64_t fnv1a64_route(const char* name, std::uint32_t domain_id) noexcept
{
    std::uint64_t hash = kFnvOffsetBasis64;
    if (name != nullptr)
    {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p != '\0'; ++p)
        {
            hash ^= static_cast<std::uint64_t>(*p);
            hash *= kFnvPrime64;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        hash ^= static_cast<std::uint64_t>((domain_id >> (8 * i)) & 0xFFu);
        hash *= kFnvPrime64;
    }
    return hash;
}

/* 能力探测结果的三态缓存（契约 §4.5：进程内缓存，**不得**运行中来回切换）：
 *   0 = 未探测（乐观：允许第一次尝试）
 *   1 = 可用（曾成功 add 过）
 *   2 = 不可用（首次 add 失败，永久回退 —— 之后不再尝试，直接返回
 *       backend_unavailable，模块作者据此保留兼容收包线程）
 * 探测本身由 ipc::recv_wait_set 内部完成（futex_waitv 0 超时调用），本层只观察
 * add 的返回值，**不重复**平台探测代码。 */
std::atomic<int>& backend_state() noexcept
{
    static std::atomic<int> state{0};
    return state;
}

constexpr std::size_t kMaxWorkerCount = 128;          ///< 与 recv_wait_set 的 kMaxRoutes 同量级
constexpr std::size_t kMaxEmptyPollsPerBudget = 4;    ///< has_pending() 恒真的防饥饿上限
constexpr std::chrono::milliseconds kQuiesceTimeout{2000};

bool quiesce_or_timeout(const std::atomic<std::size_t>& in_flight) noexcept
{
    const auto deadline = Clock::now() + kQuiesceTimeout;
    while (in_flight.load(std::memory_order_acquire) != 0)
    {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

}   // namespace

struct RecvWorker::Impl
{
    /* 一条在册 route 的全部 worker 侧状态。用 shared_ptr 保活：注销时从 routes
     * 摘除后，deferred 里可能还留着一份，正在跑的那一轮也持有一份。 */
    struct Entry
    {
        std::shared_ptr<RecvRouteSource> route;
        ipc::recv_wait_token token;
        const RecvRouteSource* key{nullptr};

        /* worker 侧 level-triggered 判据：稳定空读之前的序号快照。
         * 空读前后序号一致且 has_pending() == false 才确认此前的快照，因此
         *   · 预算耗尽让出的 route 下一轮仍会被选中（不会因为 wait-set 内部的
         *     last 已更新而永远漏掉）；
         *   · 消息到达与断开都通过同一个 seq 变化体现（需求 §5）。 */
        std::atomic<std::uint32_t> last_seq{0};

        /* 预算轮窗口计数。**整个 run_budget 都在窗口内**，而不只是 recv_once
         * 那一瞬 —— 否则 remove_route 等到 0 之后，worker 的循环可能立刻又对
         * 同一条 route 发起下一次 recv_once（TOCTOU）。窗口粒度是一个预算轮，
         * 上界 = 三项预算上限 + 一次 recv_once，仍然有界。 */
        std::atomic<std::size_t> in_flight{0};
        std::atomic<bool> removed{false};
        std::atomic<bool> queued{false};   ///< 已在 deferred 队列中（去重）
    };

    Impl(std::size_t id, RecvBudget b)
        : worker_id(id)
        , budget(b)
    {}

    const std::size_t worker_id;
    const RecvBudget budget;
    ipc::recv_wait_set wait_set;

    mutable std::mutex mtx;
    std::vector<std::shared_ptr<Entry>> routes;   ///< 固定归属表（本 worker 独占）
    std::deque<std::shared_ptr<Entry>> deferred;  ///< 预算耗尽/待处理 FIFO

    std::thread thread;
    std::atomic<bool> running{false};    ///< **活动期**语义（start 之后、stop 之前恒 true）
    std::atomic<bool> stopping{false};   ///< 请求停止（幂等，任何线程可置）
    std::atomic<bool> joined{false};     ///< join 所有权（恰好一个线程执行）

    /* ---- idle keep-alive 的线程生命周期状态（见头文件"空闲退出与按需拉起"）----
     *   alive               : 当前确有一条工作线程在 loop 内。空闲退出后为 false，
     *                         add_route 拉起后为 true。
     *   thread_started_once : 区分"首次启动"与"重拉起"（thread_restarts 计数）。
     *   join_mtx            : **串行化 std::thread 对象的 create/join**。故意与 mtx
     *                         分离：join 绝不持 mtx，否则会出现"拉起者持 mtx 等旧
     *                         线程退出、旧线程等 mtx"的循环。 */
    std::atomic<bool> alive{false};
    /* **代际编号**：每创建一条线程递增一次（只在 join_mtx 内递增，单写者）。
     * 线程收尾时只在自己仍属于当前代时才清 alive —— 旧代线程离开 loop 之后、
     * 执行到收尾清零之前，新代线程可能已经被 add_route 拉起并把 alive 置回
     * true；无条件清零会盖掉那次置位（alive==false 却有活线程 ⇒ 下一次
     * add_route 再拉起一条 ⇒ 同一 worker 两条消费者）。 */
    std::atomic<std::uint64_t> thread_epoch{0};
    bool thread_started_once{false};
    std::mutex join_mtx;

    std::atomic<std::uint64_t> wait_wakeups{0};
    std::atomic<std::uint64_t> wait_timeouts{0};
    std::atomic<std::uint64_t> wait_errors{0};
    std::atomic<std::uint64_t> routes_processed{0};
    std::atomic<std::uint64_t> messages_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> budget_yields{0};
    std::atomic<std::uint64_t> deferred_drains{0};
    std::atomic<std::uint64_t> recv_errors{0};
    std::atomic<std::uint64_t> idle_exits{0};        ///< 空闲退出次数（只增）
    std::atomic<std::uint64_t> thread_restarts{0};   ///< 按需拉起次数（只增，不含首次）

    /* W06（方案 §10.3）：单次 recv_once 耗时与预算超额。
     *   recv_once_calls       —— 调用次数（分母）
     *   recv_once_max_ns      —— 单次耗时最大值（ns）
     *   recv_once_over_budget —— 单次耗时 > max_processing_time_per_route 的次数
     * 计时在**完整 recv_once() 返回后**取值，与预算检查同一个安全边界。 */
    std::atomic<std::uint64_t> recv_once_calls{0};
    std::atomic<std::uint64_t> recv_once_max_ns{0};
    std::atomic<std::uint64_t> recv_once_over_budget{0};

    /* W06（t30 / 队长裁决 D-23，方案 §10.2）：**常驻**扫描量 —— 无时钟、无诊断门控。
     *
     * 为什么与 `measure` 的诊断孪生并存（而不是只留一套）：§10.2 明写"低开销常驻计数
     * 与详细诊断采样分开"。诊断门控的 `scan_*` 只在 `diagnostics_enabled()` 时累加，
     * 而正式性能窗口按 W03 口径通常**关闭**诊断（W08 已如此）；若只有门控版，正式窗口
     * 里就没有任何扫描量可读 ⇒ W10 只能拿 CPU 近似线性当替代量。本组三件套（轮数 /
     * 遍历 route 数 / 就绪轮数）加深度两件套，只做 relaxed 原子加与一次 CAS 最大值：
     *   · scan_rounds           —— `collect_pending()` 执行轮数（有效就绪比例的分母）
     *   · scanned_routes_total  —— 每轮遍历的 route 数之和（除以轮数 = 均值）
     *   · scan_ready_rounds     —— 本轮全扫**确实发现新就绪**的轮数（分子）
     *   · deferred_depth_last   —— 最近一轮入队前的 deferred 深度
     *   · deferred_depth_max    —— 深度最大值
     * 唯一不常驻的量是**扫描耗时**：它必须取两次时钟，成本不可忽略 ⇒ 只由诊断门控的
     * `ScanRoundScope` 提供（W03 实测：关 0.176 ns/轮、开 29.3 ns/轮）。两者在
     * `collect_pending()` 里**同一位置、同一取值**写入，故诊断开启时逐值相等（自洽判据）。 */
    std::atomic<std::uint64_t> scan_rounds{0};
    std::atomic<std::uint64_t> scanned_routes_total{0};
    std::atomic<std::uint64_t> scan_ready_rounds{0};
    std::atomic<std::uint64_t> deferred_depth_last{0};
    std::atomic<std::uint64_t> deferred_depth_max{0};

    /* R0-9/R0-10 的**结束值**每 worker 累加器（W06 接线，t53）。
     *
     * 一类量两个落点：门控全局计数（`scan.finish()`，W03 维护，⛔ 本层只读引用）与
     * **本 worker** 的常驻累加器（本成员）。为什么常驻侧也必须有：诊断关闭时门控计数
     * 是"未采集"，若只看门控面，正式性能窗口里就完全没有结束值可读（与 t30 的常驻/
     * 门控分工同一条理由）。
     *
     * ⛔ 本累加器**不写任何 gauge**（R0-10）：它按 worker 累积，全池当前总深度由
     * `RecvWorkerPool::stats()` 对 `deferred_depth_after_last` **求和**得到 —— 单个 gauge
     * 只剩"最后写入者"的值，无法表达多 worker 的总量（t44 自测反例：全池 12、gauge 7）。
     * 线程纪律：只由拥有本 worker 的那条线程在 `collect_pending()` 内使用（单消费者），
     * 故内部全为 relaxed 原子、无锁。 */
    dzIPC::measure::ScanRoundAccumulator scan_acc_;

    // 仅由 worker 线程访问；空读期间真实发生的新序号需要及时复查。
    bool sequence_progress{false};

    void loop();
    void thread_main(std::uint64_t epoch) noexcept;
    void wait_once(std::chrono::milliseconds timeout);
    bool ensure_thread_alive();
    void collect_pending();
    bool requeue(const std::shared_ptr<Entry>& entry);
    void drain_deferred();
    void run_budget(const std::shared_ptr<Entry>& entry);
    bool deferred_empty();
};

void RecvWorker::Impl::collect_pending()
{
    /* level-triggered 全扫：任何 seq != last_seq 的 route 都要进待处理队列。
     * 这一步覆盖三种"内核没点名"的情形：
     *   · wait 返回后被 consume_ready 清空，但 ready 里的 token 还没处理；
     *   · 上一轮预算耗尽让出；
     *   · 消息到达与"断开"用同一个 seq（断开也必须被处理）。
     * wait-set 的 ready 集合是提示，这里才是事实来源。 */
    std::lock_guard<std::mutex> lock(mtx);

    /* ---- §10.2 扫描观测（W06 接线，队长裁决 D-23）--------------------------------
     *
     * ⛔ 本块**只观测、不改判定**：下面那个 for 循环的遍历范围、跳过条件、入队条件
     * 与 CAS 顺序与接线前逐字相同（"不得为降低 CPU 而删除扫描或改为持续 try_recv
     * 轮询" —— 方案 §10.2 / W04 §10.2）。删除本块不影响任何一条 route 的就绪判定。
     *
     * 观测量的口径（与 W03 `ScanRoundScope` 及字段定义逐条对齐）：
     *   · `routes.size()`  —— 本轮**遍历**的 route 数（含已标记 removed 项：它们同样
     *     在循环里被读一次 seq，构成扫描成本）；→ scan_rounds / scanned_routes_total
     *   · 扫描耗时（scope 析构时取第二次 clock_gettime）；→ scan_time_ns_total
     *   · `deferred.size()`（**入队前**取，与 W03 样例 `ScanRoundScope(n, deferred.size())`
     *     同口径）；→ deferred_depth_last / deferred_depth_max
     *   · `queued > 0` —— 本轮全扫确实发现了至少一条新就绪 route；→ ready_observed
     *     （有效就绪比例 = ready_observed / scan_rounds，分母即扫描轮数）
     *
     * 开销：诊断**关闭**时构造 + 析构各只有一次 relaxed 布尔读（W03 实测 0.176 ns/轮）；
     * 开启时每轮多两次 `clock_gettime(CLOCK_MONOTONIC)`（W03 实测 29.3 ns/轮）。
     * ⛔ 这里**不**新增任何 CounterId、不另立平行命名。 */
    const std::size_t scanned = routes.size();
    const std::size_t depth_in = deferred.size();
    dzIPC::measure::ScanRoundScope scan(scanned, depth_in);
    std::size_t queued = 0;

    for (const auto& entry : routes)
    {
        if (entry->removed.load(std::memory_order_acquire)) continue;
        const auto seq = entry->token.sequence()->load(std::memory_order_acquire);
        if (seq == entry->last_seq.load(std::memory_order_acquire)) continue;
        bool expected = false;
        if (entry->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            deferred.push_back(entry);
            ++queued;
        }
    }
    scan.set_ready(queued > 0);

    /* ---- 常驻孪生（无时钟、无诊断门控；W06 接线）-------------------------------
     *
     * 为什么还要一套常驻量：§10.2 要求"低开销常驻计数与详细诊断采样分开"，且
     * W10 的规模工装在**不开诊断**的正式窗口里也要能读扫描量。这套量只做 relaxed
     * 原子加法与一次 CAS 最大值，**不取时钟**（唯一的时钟开销在 `ScanRoundScope`，
     * 恒受诊断门控）。两者在**同一位置、同一取值**写入 ⇒ 诊断开启时必须逐值相等，
     * 这构成一条可机械核对的自洽判据（见交付文档）。 */
    scan_rounds.fetch_add(1, std::memory_order_relaxed);
    scanned_routes_total.fetch_add(scanned, std::memory_order_relaxed);
    if (queued > 0) scan_ready_rounds.fetch_add(1, std::memory_order_relaxed);
    deferred_depth_last.store(depth_in, std::memory_order_relaxed);
    {
        std::uint64_t prev = deferred_depth_max.load(std::memory_order_relaxed);
        while (depth_in > prev
               && !deferred_depth_max.compare_exchange_weak(prev, depth_in, std::memory_order_relaxed))
        {
        }
    }

    /* ---- R0-9 / R0-10 结束值（W06 接线，t53）---------------------------------
     *
     * 为什么必须有这一段（t12 实测到的零值语义混淆）：`ScanRoundScope::finish()` 若不
     * 调用，`scan_ready_routes_total` / `deferred_depth_after_*` 四个门控计数**一个都不写**
     * ⇒ 读数恒 0，而 `0` 会被误读成"本轮没有新就绪 route / 扫描后队列为空"。接线之后
     * 它们才表达真实事实。⛔ 本段只**上报观测值**：上面的 for 循环逐字未动。
     *
     * 两套落点（与 t30 的常驻/门控分工同构，⛔ 不得互相替代）：
     *   · `scan.finish(queued, depth_after)` —— 写**门控**全局计数（诊断关闭时零写入，
     *     属"未采集"，⛔ 不得当作"深度为 0"；判别用 `counter_is_uncollected()`）；
     *   · `scan_acc_.add(scan.result())` —— 写**本 worker** 的常驻累加器（relaxed 原子、
     *     不取时钟、⛔ 不写任何 gauge），由 `RecvWorkerStats` 导出。全池**当前**总深度 =
     *     Σ 每 worker 的 `deferred_depth_after_last`（R0-10 冻结的求法；任一 gauge 都
     *     不能冒充它 —— t44 自测反例：全池 12 而 gauge 7）。
     *
     * ⚠️ 顺序（承重，别改）：`finish()` 放在**遍历 + `set_ready` + 常驻孪生之后**，
     * 于是 `result().elapsed_ns` 的区间与 `scan_time_ns_total`（构造 → 析构）几乎重合，
     * 两者可直接对账（t44 声明的守恒判据 `Σ elapsed_ns ≈ scan_time_ns_total`）。t44 的
     * `finish()` 明确允许它在 `set_ready()` 前后任一点调用，此处取"之后"。
     * `depth_after` 取**扫描后、运行预算前**的 `deferred.size()`；`set_ready()` 与常驻
     * 孪生都不改 `deferred`，故在其后读取仍是同一事实。
     *
     * 开销：常驻半（`scan_acc_`）无时钟、无门控，每轮 4 次 relaxed 原子加 + 1 次 CAS；
     * 门控半（`finish()`）在诊断关闭时只置两个成员即 return（⛔ 不读时钟）。 */
    const std::size_t depth_after = deferred.size();
    scan.finish(queued, depth_after);
    scan_acc_.add(scan.result());
}

bool RecvWorker::Impl::requeue(const std::shared_ptr<Entry>& entry)
{
    bool expected = false;
    if (!entry->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    std::lock_guard<std::mutex> lock(mtx);
    if (entry->removed.load(std::memory_order_acquire))
    {
        entry->queued.store(false, std::memory_order_release);
        return false;
    }
    deferred.push_back(entry);
    return true;
}

bool RecvWorker::Impl::deferred_empty()
{
    std::lock_guard<std::mutex> lock(mtx);
    return deferred.empty();
}

void RecvWorker::Impl::drain_deferred()
{
    /* 只处理"进入本函数时队列里的那些项"：run_budget 让出时 requeue 到队尾的
     * route 留到下一轮，于是同 worker 的其他 route 一定先被轮到（固定 FIFO，
     * 热 route 不会长期饿死冷 route）。 */
    std::size_t rounds = 0;
    {
        std::lock_guard<std::mutex> lock(mtx);
        rounds = deferred.size();
    }
    while (rounds-- > 0)
    {
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (deferred.empty()) break;
            entry = deferred.front();
            deferred.pop_front();
            entry->queued.store(false, std::memory_order_release);
        }
        if (entry->removed.load(std::memory_order_acquire)) continue;
        run_budget(entry);
        if (stopping.load(std::memory_order_acquire)) break;
    }
}

void RecvWorker::Impl::run_budget(const std::shared_ptr<Entry>& entry)
{
    entry->in_flight.fetch_add(1, std::memory_order_acq_rel);
    /* 双检：remove_route 可能在"从队列取出"与"进入窗口"之间完成了摘除+标记。
     * 此刻已标记 removed ⇒ 不得再碰 route（注销协议第 3 步之后 route 可能被释放）。 */
    if (entry->removed.load(std::memory_order_acquire))
    {
        entry->in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    std::size_t messages = 0;
    std::size_t bytes = 0;
    std::size_t empty_polls = 0;
    bool more = false;
    bool confirmed_empty = false;
    const auto deadline = Clock::now() + budget.max_processing_time_per_route;

    for (;;)
    {
        if (stopping.load(std::memory_order_acquire)) break;
        /* remove_route 可能在本次预算轮进行中完成摘除 + 标记（它在另一个线程）。
         * 契约 §4.4 第 1 步要求"禁止新的 recv_once"：一旦标记就必须立刻停手，
         * 不再对这条 route 发起下一次调用。在途的那一次仍要等它返回 —— 那是
         * remove_route 第 4 步（等 in_flight 归零）的职责。 */
        if (entry->removed.load(std::memory_order_acquire)) break;

        const auto seq_before = entry->token.sequence()->load(std::memory_order_acquire);
        std::size_t n = 0;
        /* W06（方案 §10.3）：单次 recv_once 耗时**单独**记录。
         * 计时边界 = 完整的一次 recv_once() 调用（含宿主侧组包/分流/入队）。这不是
         * 诊断门控项 —— §10.3 明确要求"单独记录 recv_once 耗时和预算超额"，否则
         * "时间预算是否生效"无法判断（200 µs 预算并不能抢占一次耗时调用）。
         * 代价：每次调用两次 steady_clock 读取（实测 ~20 ns/次量级），相对一次
         * 收包可忽略；开销对照见交付文档。 */
        const auto t0 = Clock::now();
        try
        {
            n = entry->route->recv_once();
        }
        catch (...)
        {
            /* 宿主实现的异常不得逃出 worker 线程（否则整个进程 terminate）。
             * 记一次并让出，由宿主自己去修。 */
            recv_errors.fetch_add(1, std::memory_order_relaxed);
            ipc::error("RecvWorker: recv_once threw for route '%s'\n",
                       entry->route->route_name() != nullptr ? entry->route->route_name() : "?");
            more = true;
            break;
        }
        {
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
            recv_once_calls.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t prev = recv_once_max_ns.load(std::memory_order_relaxed);
            while (ns > prev
                   && !recv_once_max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed))
            {
            }
            if (ns > static_cast<std::uint64_t>(budget.max_processing_time_per_route.count() * 1000))
            {
                recv_once_over_budget.fetch_add(1, std::memory_order_relaxed);
                /* §10.2/§10.3 诊断孪生（W06 接线）：与常开量同一计数点、同一判据
                 * （严格 `>` 预算，单位 µs→ns 的换算逐字相同）。 */
                DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::recv_once_over_budget);
            }
            /* `recv_once_calls` 的诊断孪生：**每次调用**都记（分母），与常开量同点。 */
            DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::recv_once_calls);
        }

        if (n == 0)
        {
            // 只能确认本次空读之前的序号。发布可以发生在 recv 返回空之后；
            // 把此刻最新的序号写回会把仍在共享队列中的消息误认为已消费。
            const auto seq_after = entry->token.sequence()->load(std::memory_order_acquire);
            if (seq_after != seq_before)
            {
                sequence_progress = true;
                more = true;
                break;  // 留给下一轮，先让同 worker 的其他 route 获得服务
            }
            const bool pending = entry->route->has_pending();
            if (pending && ++empty_polls < kMaxEmptyPollsPerBudget) continue;
            if (!pending)
            {
                entry->last_seq.store(seq_before, std::memory_order_release);
                confirmed_empty = true;
            }
            break;
        }
        empty_polls = 0;
        ++messages;
        bytes += n;

        /* **安全边界**：预算只在"一次完整 recv_once 返回"之后检查。
         * ⛔ 不得在组包中途切走（socket 侧 chunk_rev_* 一次调用可能持续到整条
         * 消息/请求组装完成）。 */
        if (messages >= budget.max_messages_per_route || bytes >= budget.max_bytes_per_route
            || Clock::now() >= deadline)
        {
            more = true;
            break;
        }
    }

    // 预算耗尽、异常与生命周期退出都不等于确认读空。
    // 稳定空读后的新发布仍与 last_seq 不同，由下一次就绪检查发现。
    if (!confirmed_empty)
        more = !stopping.load(std::memory_order_acquire)
            && !entry->removed.load(std::memory_order_acquire);

    if (messages > 0)
    {
        routes_processed.fetch_add(1, std::memory_order_relaxed);
        messages_received.fetch_add(messages, std::memory_order_relaxed);
        bytes_received.fetch_add(bytes, std::memory_order_relaxed);
    }

    entry->in_flight.fetch_sub(1, std::memory_order_acq_rel);

    if (more && requeue(entry))
    {
        budget_yields.fetch_add(1, std::memory_order_relaxed);
        deferred_drains.fetch_add(1, std::memory_order_relaxed);
        /* §10.2 诊断孪生（W06 接线）：与上面两个常开量同一计数点，语义逐字对应。
         * 诊断关闭时是一条 relaxed 布尔读 + 分支不进入（实测 0.176 ns 量级）。 */
        DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::budget_yields);
        DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::deferred_drains);
    }
}

/* 一次**阻塞**等待 + 就绪消费。超时不是错误（只有后端已被判定不可用才算
 * 错误）。单独成函数是因为 loop() 里有两种等待时长（普通 wait_timeout /
 * 空闲窗口的剩余）。 */
void RecvWorker::Impl::wait_once(std::chrono::milliseconds timeout)
{
    if (!wait_set.wait(timeout))
    {
        /* 契约：超时不是错误。只有在后端已被判定不可用时才算错误。 */
        if (backend_state().load(std::memory_order_acquire) == 2)
            wait_errors.fetch_add(1, std::memory_order_relaxed);
        else
            wait_timeouts.fetch_add(1, std::memory_order_relaxed);
        /* §10.2「等待超时次数」的诊断孪生（W06 接线）：与常开量同一计数点。
         * ⛔ 语义澄清：本处只统计**真正超时返回**（`wait()` 返回 false）的次数；
         * `wait_errors`（后端不可用）与 `wait_wakeups`（有就绪/被打断）都不计入 ——
         * 把三者混算会让"等待超时"失去可解释性。 */
        DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::wait_timeout_count);
        return;
    }
    wait_wakeups.fetch_add(1, std::memory_order_relaxed);
    /* 必须消费：否则 wait-set 内部的 ready 缓存会无限累积，且下次 wait 会立刻
     * 返回（把超时语义变成忙转）。就绪内容由 collect_pending 的全扫重新发现。 */
    (void)wait_set.consume_ready();
    collect_pending();
}

void RecvWorker::Impl::loop()
{
    /* 空闲计时："连续无待处理项"从 idle_since 起算，任何一次注册/待处理活动都把
     * 它重置为当下。窗口取 max(idle_keep_alive, wait_timeout) 作下界 —— 原因是
     * 一次 wait 切片最长 wait_timeout，宿主实现也允许用满等长的预算（见契约
     * §4.2），把有效空闲上界抬到 wait_timeout 可以消掉"等待切片本身吃满整个
     * 窗口、空闲退出永不触发"这个不变量依赖（窗口 < wait_timeout 时，一台负载
     * 很高的机器可能让本线程饿到断言窗口之外才退出）。 */
    auto idle_since = Clock::now();
    /* idle_keep_alive == 0 是特例：保持"下一轮空闲检查即退出"的字面语义（测试
     * 用来把退出/重拉起交错压到最紧），**不**接受 max 下界；其余取值才抬到
     * wait_timeout（理由见上）。 */
    const auto idle_span = budget.idle_keep_alive.count() <= 0
                             ? std::chrono::milliseconds{0}
                             : std::max<std::chrono::milliseconds>(budget.idle_keep_alive, budget.wait_timeout);

    while (!stopping.load(std::memory_order_acquire))
    {
        collect_pending();

        if (deferred_empty())
        {
            const auto now = Clock::now();
            const auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - idle_since);
            if (idle_for < idle_span)
            {
                /* 空闲等待是 wait-set 的**阻塞**等待：取 min(wait_timeout, 剩余
                 * 空闲窗口)，下界 1 ms 防止 wait(0) 退化成忙轮询。 */
                auto slice = std::min(budget.wait_timeout, idle_span - idle_for);
                if (slice < std::chrono::milliseconds{1}) slice = std::chrono::milliseconds{1};
                wait_once(slice);
                continue;
            }

            /* 连续空闲 >= 窗口 ⇒ 空闲退出（idle exit）。
             * 与 add_route 的收敛**：线程退出的唯一途径是"持有 mtx 且看到表空"，
             * 而 add_route 的"插入 route 表 + 按需拉起"在同一个 mtx 临界区内完成，
             * 因此 "线程判定表空准备退出" 与 "add_route 刚插入却无人消费" 不可能
             * 重叠 —— add_route 返回 ok 却静默丢包是禁止的。 */
            std::unique_lock<std::mutex> lock(mtx);
            if (!routes.empty() || !deferred.empty())
            {
                lock.unlock();
                idle_since = Clock::now();   // 又有活了：空闲计时从头开始
                continue;
            }
            if (stopping.load(std::memory_order_acquire)) break;   // 与 stop 的交接：由 stop 唤醒/join
            alive.store(false, std::memory_order_release);
            lock.unlock();
            idle_exits.fetch_add(1, std::memory_order_relaxed);
            /* §10.2 诊断孪生（W06 接线，同一计数点）。⛔ 放在 alive.store(false) 之后、
             * return 之前：这次退出已经不可撤销，计数必须与它同真值。 */
            DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::idle_exits);
            ipc::log("RecvWorker[%zu]: idle exit (no routes for >= %lld ms)\n", worker_id,
                     static_cast<long long>(idle_span.count()));
            return;
        }

        /* 有待处理项先 drain。收到数据或空读期间序号真正变化时及时复查；
         * 只有不含新事件的无进展轮才阻塞退避，避免 has_pending 恒真忙转。
         * wait-set 记录的是通知观察进度，不能替代 worker 的消费确认边界。 */
        sequence_progress = false;
        const std::uint64_t msgs_before = messages_received.load(std::memory_order_relaxed);
        drain_deferred();
        if (!sequence_progress && messages_received.load(std::memory_order_relaxed) == msgs_before)
        {
            wait_once(budget.wait_timeout);
        }
        idle_since = Clock::now();   // 这一轮有活干：重置空闲计时
    }
}

void RecvWorker::Impl::thread_main(std::uint64_t epoch) noexcept
{
    /* ⛔ 不得让任何异常逃出线程（逃出即 std::terminate）。recv_once 已在
     * run_budget 内单独兜过；这里是最后一道闸，也覆盖宿主 read_wait_token /
     * has_pending 之类的实现面。 */
    try
    {
        loop();
    }
    catch (...)
    {
        ipc::error("RecvWorker[%zu]: exception escaped worker loop\n", worker_id);
    }
    /* 线程真的结束了。注意 running（**活动期**）不受影响：start 之后、stop 之前
     * 恒为 true，空闲退出只是把线程归还操作系统。
     *
     * ⛔ 清零**必须**带代际检查（见 Impl::thread_epoch）：本线程离开 loop 之后、
     * 执行到这一行为止，另一个 add_route 可能已经在 mtx 内把新线程拉起来
     * （上一代的 alive 已由 loop 的 idle-exit 分支持锁清零，所以
     * ensure_thread_alive 会走拉起路径并把 alive 置回 true）。此处若无条件清零，
     * 就会盖掉那次置位 ⇒ alive==false 却有一条活线程 ⇒ 下一次 add_route 再拉起
     * 一条 ⇒ 同一个 worker 两条消费者同时 recv_once（thread_local 分片缓存换槽，
     * 症状是偶发丢消息）。只在自己仍是当前代时清零，旧代的收尾因此不影响新代。 */
    if (thread_epoch.load(std::memory_order_acquire) == epoch)
    {
        alive.store(false, std::memory_order_release);
    }
}

bool RecvWorker::Impl::ensure_thread_alive()
{
    if (alive.load(std::memory_order_acquire)) return false;
    if (stopping.load(std::memory_order_acquire)) return false;
    if (!running.load(std::memory_order_acquire)) return false;

    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(join_mtx);
        /* **双检，且必须在 join_mtx 内**：两个并发的 add_route 可能同时读到
         * alive==false 然后前后脚走到这里。只有第一个能建线程；第二个必须在
         * 这里再次看到 true 并返回 false，否则它会把第一个刚建好的 std::thread
         * 当成"上一代"收尸并再建一条 ⇒ 同一个 worker 两条消费者同时 recv_once
         * （违反单消费者互斥，且 thread_restarts 会多计）。 */
        if (alive.load(std::memory_order_acquire)) return false;
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!running.load(std::memory_order_acquire)) return false;

        /* 收尸上一代（它已从 loop 返回，join 立即完成），再建新线程：
         * 直接对 joinable 的 std::thread 赋值会 std::terminate。 */
        previous = std::move(thread);
        /* alive 由**创建者**置位，且在起线程**之前**，两个理由：
         *   · 拉起返回与线程真正跑起来之间有窗口，add_route 若在这窗口里看到
         *     alive==false 会再建一条线程（同一个 worker 两条消费者）；
         *   · 反过来（先起线程再置位）新线程可能在置位前就跑完一整轮空闲退出
         *     并把 alive 置回 false，创建者随后的置位会**盖掉**那次退出
         *     ⇒ alive==true 却没有任何线程，此后 add_route 不再拉起（静默丢包）。 */
        /* 代际编号先递增（在置 alive 与起线程之前）：旧线程的收尾清零会因为
         * 代际不匹配而失效，这样"旧代收尾"与"新代置位"之间不再需要 happens-before。 */
        const std::uint64_t epoch = thread_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        alive.store(true, std::memory_order_release);
        try
        {
            thread = std::thread([this, epoch] { thread_main(epoch); });
        }
        catch (...)
        {
            /* 线程创建失败（资源耗尽）：置位必须回滚并还原上一代，否则
             * alive==true 却无线程，后续 add_route 永远不会再拉起。 */
            alive.store(false, std::memory_order_release);
            thread = std::move(previous);
            return false;
        }
        if (thread_started_once)
        {
            thread_restarts.fetch_add(1, std::memory_order_relaxed);
            /* §10.2 诊断孪生（W06 接线，同一计数点）。⛔ 不含首次 start()：
             * 与常开量 `thread_restarts` 的语义逐字一致（"按需拉起次数，不含首次"）。 */
            DZIPC_MEASURE_DIAG_INC(dzIPC::measure::CounterId::thread_restarts);
        }
        thread_started_once = true;
    }
    if (previous.joinable()) previous.join();
    return true;
}

RecvWorker::RecvWorker(std::size_t worker_id, RecvBudget budget)
    : impl_(new Impl(worker_id, budget))
{}

RecvWorker::~RecvWorker()
{
    stop();
    if (impl_ != nullptr)
    {
        /* 兜底：stop() 若由 worker 线程自己调用（未消费 join 所有权），或线程已
         * 空闲退出但还未被收尸，都留到这里 join 一次，避免 joinable 的
         * std::thread 走到 std::terminate。thread 成员的访问与其他两处一样在
         * join_mtx 内（否则可能与并发的 add_route 拉起路径互相 move 赋值）；
         * join 本身在锁外做，阻塞不占锁。 */
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(impl_->join_mtx);
            if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()
                && !impl_->joined.exchange(true))
            {
                to_join = std::move(impl_->thread);
            }
        }
        if (to_join.joinable()) to_join.join();
        delete impl_;
        impl_ = nullptr;
    }
}

bool RecvWorker::start()
{
    if (impl_ == nullptr) return false;
    if (impl_->stopping.load(std::memory_order_acquire)) return false;   // stop 之后不可重启
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true)) return false;
    /* 首次启动也走"按需拉起"同一段代码：只有一处创建线程的地方，alive 的置位
     * 与 thread 对象的建立因此不可分裂。
     * ⛔ 不得在 mtx 内调用（与 add_route 同一条死锁链：ensure 取 join_mtx，而
     * stop 持 join_mtx 等线程退出、线程退出可能取 mtx）。这条路不需要 mtx ——
     * 它不插入 route 表，并发重复 start 已由上面的 CAS 挡掉。 */
    return impl_->ensure_thread_alive();
}

void RecvWorker::stop() noexcept
{
    if (impl_ == nullptr) return;
    impl_->stopping.store(true, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
    /* 唤醒阻塞中的 wait（wait-set 的 stop 是幂等的，且会让后续 wait 立即返回）。 */
    impl_->wait_set.stop();
    std::thread to_join;
    {
        /* thread 成员只在 join_mtx 内读写（这里、析构、ensure_thread_alive 的拉起
         * 路径）。否则 stop 与并发的 add_route 拉起会同时 move 赋值同一个
         * std::thread 对象（数据竞争）。 */
        std::lock_guard<std::mutex> lock(impl_->join_mtx);
        /* 线程可能已空闲退出（已从 loop 返回，但未被 join）：joinable() 仍为 true，
         * join 只是收尸，立即返回。 */
        if (!impl_->thread.joinable()) return;
        /* ⛔ 不得从 worker 线程 join 自己（std::system_error → terminate）。同线程
         * 调用只置停止标志、**不**消费 join 所有权，把 join 留给析构或外部线程 ——
         * 这是 ShmControlScheduler 记录过的坑，这里必须分成两个独立状态。 */
        if (impl_->thread.get_id() == std::this_thread::get_id()) return;
        if (impl_->joined.exchange(true)) return;
        to_join = std::move(impl_->thread);   // 取出后在锁外 join
    }
    to_join.join();
}

bool RecvWorker::running() const noexcept
{
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

bool RecvWorker::thread_alive() const noexcept
{
    return impl_ != nullptr && impl_->alive.load(std::memory_order_acquire);
}

std::size_t RecvWorker::worker_id() const noexcept
{
    return impl_ != nullptr ? impl_->worker_id : 0;
}

RecvRegisterStatus RecvWorker::add_route(const std::shared_ptr<RecvRouteSource>& route)
{
    if (impl_ == nullptr) return RecvRegisterStatus::invalid_route;
    if (!route) return RecvRegisterStatus::invalid_route;
    if (impl_->stopping.load(std::memory_order_acquire) || !impl_->running.load(std::memory_order_acquire))
        return RecvRegisterStatus::stopped;

    const ipc::recv_wait_token token = route->read_wait_token();
    if (!token.valid()) return RecvRegisterStatus::invalid_token;

    /* 单 route 单消费者：兼容 subscribe_thread_ 正在 recv 时必须返回 busy，
     * 由模块作者保留兼容线程，**不得**静默双收。 */
    if (!route->try_claim_recv(RecvOwner::worker))
    {
        /* CAS 失败有两种原因，必须区分（否则消费方会把"自己重复注册"误读成
         * "兼容线程在收"，据此做出错误决策）：
         *   · 本 worker 已注册过同一条 route（owner == worker）⇒ duplicate；
         *   · 别的 owner（compat_thread）正在 recv ⇒ busy。 */
        std::lock_guard<std::mutex> lock(impl_->mtx);
        const auto it = std::find_if(impl_->routes.begin(), impl_->routes.end(),
                                     [&](const std::shared_ptr<Impl::Entry>& e) { return e->key == route.get(); });
        return (it != impl_->routes.end()) ? RecvRegisterStatus::duplicate
                                           : RecvRegisterStatus::busy;
    }

    {
        /* owner 为 none 但表内已有同一条 route（异常态）：恢复 none 并报 duplicate。 */
        std::lock_guard<std::mutex> lock(impl_->mtx);
        const auto it = std::find_if(impl_->routes.begin(), impl_->routes.end(),
                                     [&](const std::shared_ptr<Impl::Entry>& e) { return e->key == route.get(); });
        if (it != impl_->routes.end())
        {
            route->release_recv();
            return RecvRegisterStatus::duplicate;
        }
    }

    const int state = backend_state().load(std::memory_order_acquire);
    if (state == 2)
    {
        /* 永久回退：探测已判定不可用，不再尝试（禁止运行中来回切换后端）。 */
        route->release_recv();
        return RecvRegisterStatus::backend_unavailable;
    }

    if (!impl_->wait_set.add(token))
    {
        route->release_recv();
        if (state == 1)
        {
            /* 曾经成功过 ⇒ 这次失败是容量满，不是后端不可用。 */
            return RecvRegisterStatus::wait_set_full;
        }
        backend_state().store(2, std::memory_order_release);
        ipc::log("RecvWorker: wait-set backend unavailable; keep one receive thread per route\n");
        return RecvRegisterStatus::backend_unavailable;
    }
    backend_state().store(1, std::memory_order_release);

    auto entry = std::make_shared<Impl::Entry>();
    entry->route = route;
    entry->token = token;
    entry->key = route.get();
    entry->last_seq.store(token.sequence()->load(std::memory_order_acquire), std::memory_order_release);

    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->routes.push_back(entry);
        need_start = !impl_->alive.load(std::memory_order_acquire);
    }
    /* ⛔ 拉起**必须**在 mtx **之外**：ensure_thread_alive() 会取 join_mtx（还可能要
     * join 旧代线程），而 stop() 持 join_mtx 等 worker 线程退出、worker 线程退出
     * 前可能还要取 mtx（drain_deferred）⇒ 在 mtx 内调它就是 mtx → join_mtx
     * 的反向获取，三方交叠即死锁：
     *   add_route 持 mtx 等 join_mtx ‖ stop 持 join_mtx 等线程 R ‖ R 等 mtx。
     *
     * 原子性**不依赖**“同一临界区”，而是由两个不变量拼出：
     *   · 线程清 alive 与它判定 route 表为空在**同一个 mtx 临界区**内（见 loop）；
     *   · 本函数插入 route 与读 alive 也在**同一个 mtx 临界区**内（下面这个 block）。
     * 两者互斥 ⇒ 读到哪个值都安全：锁内读到 alive==true ⇒ 该线程要么尚未做表空
     * 判定（会看到非空的表），要么已判定并因表非空而放弃退出 ⇒ 它不会退出，会
     * 正常消费；锁内读到 alive==false ⇒ 该线程是在“表为空”那一刻清的 alive，
     * 此后不再消费任何 route ⇒ 由这里拉起新线程。于是“返回 ok 却无人消费”
     * （静默丢包）仍被禁止。多个 add_route 并发时，双检（外层 + join_mtx 内）
     * 仍保证只有一条线程建立。 */
    bool have_thread = !need_start;
    if (need_start)
    {
        (void)impl_->ensure_thread_alive();
        have_thread = impl_->alive.load(std::memory_order_acquire);
    }
    if (!have_thread)
    {
        /* 并发 stop()：活动期已结束，线程不会被拉起。**回滚**本次注册并显式
         * 返回 stopped —— 契约禁止"返回 ok 却无人消费"。此刻 worker 从未见过
         * 这条 entry（线程被 stop 挡住没起来 / 已经退出），回滚只需摘表 + 摘
         * 等待集合 + 归还收包独占，无需 stop_and_wake / wait_quiescent。 */
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->routes.erase(std::remove(impl_->routes.begin(), impl_->routes.end(), entry),
                                impl_->routes.end());
            impl_->deferred.erase(std::remove(impl_->deferred.begin(), impl_->deferred.end(), entry),
                                  impl_->deferred.end());
            entry->queued.store(false, std::memory_order_release);
        }
        entry->removed.store(true, std::memory_order_release);
        (void)impl_->wait_set.remove(token);
        route->release_recv();
        return RecvRegisterStatus::stopped;
    }

    /* 注册即让新 route **至少被处理一次**（无条件入 deferred，不依赖 seq 变化）：
     * token 建立之前队列里可能已经有数据（注册前的历史消息，或上一轮空闲退出
     * 窗口内到达的消息），那不会有新的 seq 变化来叫醒我们，而 last_seq 又是注册
     * 那一刻取的基线 ⇒ 只靠 collect_pending 的 seq 判据会一直看不到它。
     * 代价是每条 route 注册时多一次 recv_once（非阻塞、读到 0 即让出）。 */
    impl_->requeue(entry);
    impl_->wait_set.remove(token);
    impl_->wait_set.add(token);   // 敲一次唤醒通道，让 worker 立刻重扫
    return RecvRegisterStatus::ok;
}

void RecvWorker::remove_route(const RecvRouteSource* route) noexcept
{
    if (impl_ == nullptr || route == nullptr) return;

    /* W04-F1（队长裁决 D-11）：**同线程调用是不支持的用法**，实现里没有旁路。
     * 本函数第 4/5 步要等在途 recv_once 与 lease 归零；从 worker 线程调用时，那个在途
     * 调用就是调用者自己 ⇒ 必然等满 kQuiesceTimeout(2000ms) 才由超时分支返回。
     * debug 构建下当场断言（可机械核对：见头文件注释的 grep 判据）；Release 下保留
     * "等满 2000ms + 诊断日志"作为运行期可观测信号 —— 它不是兜底支持，只是不会永久死锁。 */
#if !defined(NDEBUG)
    assert(impl_->thread.get_id() != std::this_thread::get_id()
           && "remove_route() must not be called from the worker thread (see W04-F1)");
#endif

    std::shared_ptr<Impl::Entry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        const auto it = std::find_if(impl_->routes.begin(), impl_->routes.end(),
                                     [&](const std::shared_ptr<Impl::Entry>& e) { return e->key == route; });
        if (it == impl_->routes.end()) return;   // 幂等：未知 route 是无操作
        entry = *it;
        impl_->routes.erase(it);
        /* 摘除与标记在**同一个锁区间**内完成：worker 侧的双检（锁内查表 /
         * 锁内 requeue）因此能看到一致的 removed 状态。 */
        entry->removed.store(true, std::memory_order_release);
    }

    /* 1. 摘除 + 标记 removed 已在上面完成（禁止新的 recv_once）。 */

    /* 2. wait_set.remove：从等待集合摘掉并**唤醒阻塞中的 worker wait**。
     *    （本机实测 epoll_ctl(DEL) 本身不唤醒 epoll_wait，唤醒靠内部 eventfd，
     *    见 socket_wait_set.h 的实测记录；libipc 的 recv_wait_set 同理。） */
    impl_->wait_set.remove(entry->token);

    /* 3. 禁止新 lease + 唤醒 route 内部阻塞中的 recv。 */
    if (entry->route) entry->route->stop_and_wake();

    /* 4. 等 worker 侧的预算轮窗口归零（**在表锁之外**等待：worker 增减这个计数
     *    不需要 mtx，不存在"注销等 worker、worker 等锁"的循环）。
     *    有界：上界 = 一个预算轮 = 三项预算上限 + 一次 recv_once。 */
    if (!quiesce_or_timeout(entry->in_flight))
    {
        ipc::error("RecvWorker: route '%s' recv_once did not quiesce within %lld ms\n",
                   route->route_name() != nullptr ? route->route_name() : "?",
                   static_cast<long long>(kQuiesceTimeout.count()));
    }

    /* 5. 等 route 内部 lease 归零（阶段 2 RouteSession 协议）。 */
    if (entry->route) entry->route->wait_quiescent();

    /* 6. 归还收包独占（owner 回到 none），之后兼容线程才允许重新接管。 */
    if (entry->route) entry->route->release_recv();

    /* 从 deferred 里清掉残余引用（shared_ptr 本身由这里的 entry 与 worker 侧
     * 本地副本保活，摘除只是避免下次 drain 再选中它 —— removed 已能挡住）。 */
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->deferred.erase(std::remove(impl_->deferred.begin(), impl_->deferred.end(), entry),
                              impl_->deferred.end());
        entry->queued.store(false, std::memory_order_release);
    }
}

void RecvWorker::wakeup() noexcept
{
    if (impl_ == nullptr) return;
    /* wait-set 没有独立的 wakeup 接口，但 remove 会敲唤醒通道、add 会重新登记。
     * 对**任意一条**在册 token 做一次 remove+add 就等于唤醒 —— 唤醒通道是集合级
     * 的，与具体 token 无关。
     *
     * 这样做**不会**丢失就绪提示：wait-set 内部的 last_seq 被重置只是丢掉一次
     * 内核提示，而 worker 的 collect_pending 用自己的 last_seq 做全扫（事实来源
     * 是共享内存里的 seq，不是内核对象）。 */
    std::shared_ptr<Impl::Entry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->routes.empty()) return;
        entry = impl_->routes.front();
    }
    impl_->wait_set.remove(entry->token);
    impl_->wait_set.add(entry->token);
}

RecvWorkerStats RecvWorker::stats() const
{
    RecvWorkerStats out;
    if (impl_ == nullptr) return out;
    out.wait_wakeups = impl_->wait_wakeups.load(std::memory_order_relaxed);
    out.wait_timeouts = impl_->wait_timeouts.load(std::memory_order_relaxed);
    out.wait_errors = impl_->wait_errors.load(std::memory_order_relaxed);
    out.routes_processed = impl_->routes_processed.load(std::memory_order_relaxed);
    out.messages_received = impl_->messages_received.load(std::memory_order_relaxed);
    out.bytes_received = impl_->bytes_received.load(std::memory_order_relaxed);
    out.budget_yields = impl_->budget_yields.load(std::memory_order_relaxed);
    out.deferred_drains = impl_->deferred_drains.load(std::memory_order_relaxed);
    out.recv_errors = impl_->recv_errors.load(std::memory_order_relaxed);
    out.idle_exits = impl_->idle_exits.load(std::memory_order_relaxed);
    out.thread_restarts = impl_->thread_restarts.load(std::memory_order_relaxed);
    out.recv_once_calls = impl_->recv_once_calls.load(std::memory_order_relaxed);
    out.recv_once_max_ns = impl_->recv_once_max_ns.load(std::memory_order_relaxed);
    out.recv_once_over_budget = impl_->recv_once_over_budget.load(std::memory_order_relaxed);
    /* t30/D-23：§10.2 的常驻扫描量（本 worker 的三件套 + 深度两件套）。 */
    out.scan_rounds = impl_->scan_rounds.load(std::memory_order_relaxed);
    out.scanned_routes_total = impl_->scanned_routes_total.load(std::memory_order_relaxed);
    out.scan_ready_rounds = impl_->scan_ready_rounds.load(std::memory_order_relaxed);
    out.deferred_depth_last = impl_->deferred_depth_last.load(std::memory_order_relaxed);
    out.deferred_depth_max = impl_->deferred_depth_max.load(std::memory_order_relaxed);
    /* t53：R0-9/R0-10 结束值（本 worker 的常驻导出；与门控全局计数同源同点）。 */
    out.scan_ready_routes_total = impl_->scan_acc_.ready_routes_total();
    out.deferred_depth_after_last = impl_->scan_acc_.deferred_depth_after_last();
    out.deferred_depth_after_max = impl_->scan_acc_.deferred_depth_after_max();
    out.deferred_depth_after_total = impl_->scan_acc_.deferred_depth_after_total();
    out.scan_elapsed_ns_total = impl_->scan_acc_.elapsed_ns_total();
    out.route_count = route_count();
    return out;
}

const RecvBudget& RecvWorker::budget() const noexcept
{
    /* impl_ 永远非空（构造期 new）；budget 是 Impl 的 const 成员，随 Impl 存活。 */
    static const RecvBudget kFallback{};
    return impl_ != nullptr ? impl_->budget : kFallback;
}

std::size_t RecvWorker::route_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->routes.size();
}

bool RecvWorker::backend_available() noexcept
{
    return backend_state().load(std::memory_order_acquire) != 2;
}

const char* RecvWorker::backend_name() noexcept
{
    /* 中性名字：真实后端名由 ipc::recv_wait_set 内部决定（Linux futex_waitv /
     * Windows WaitForMultipleObjects），本层不做平台分支（见文件头"平台宏边界"）。 */
    return backend_state().load(std::memory_order_acquire) == 2 ? "none" : "recv_wait_set";
}

/* ==================== RecvWorkerPool ==================== */

struct RecvWorkerPool::Impl
{
    std::mutex mtx;
    std::vector<std::unique_ptr<RecvWorker>> workers;
    bool started{false};
};

RecvWorkerPool& RecvWorkerPool::instance()
{
    /* 故意泄漏的指针单例：理由见头文件（与 LocalPubSubRegistry /
     * ShmControlScheduler 同构）。 */
    static RecvWorkerPool* pool = new RecvWorkerPool();
    return *pool;
}

std::size_t RecvWorkerPool::worker_for(const char* route_name,
                                       std::uint32_t domain_id,
                                       std::size_t worker_count) noexcept
{
    if (route_name == nullptr || worker_count == 0) return 0;
    return static_cast<std::size_t>(fnv1a64_route(route_name, domain_id) % worker_count);
}

RecvWorkerPool::RecvWorkerPool()
    : impl_(new Impl)
{}

RecvWorkerPool::~RecvWorkerPool()
{
    stop();
    delete impl_;
    impl_ = nullptr;
}

bool RecvWorkerPool::start(std::size_t worker_count, const RecvBudget& budget)
{
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    /* 池是**一次性**的进程级单例：stop() 之后不允许再 start。
     * 若允许重启，已停的旧 worker 还留在 workers 里，新 worker 会 push 到它们
     * 后面 ⇒ worker_for 的模数变化 + stats/route_count 重复统计。单例本来就随
     * 进程存活（故意泄漏），模块只做 add_route/remove_route，**不**调用 stop。 */
    if (impl_->started || !impl_->workers.empty()) return false;
    if (worker_count == 0)
    {
        worker_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (worker_count == 0) worker_count = 1;
    }
    if (worker_count > kMaxWorkerCount) worker_count = kMaxWorkerCount;
    impl_->workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        auto worker = std::unique_ptr<RecvWorker>(new RecvWorker(i, budget));
        if (!worker->start())
        {
            /* 启动失败：把已经起来的收回去，保持"要么全起、要么没起"的状态，
             * 否则 worker_for 计算出的归属会落到一个不存在的 worker 上。 */
            for (auto& started : impl_->workers) started->stop();
            impl_->workers.clear();
            return false;
        }
        impl_->workers.push_back(std::move(worker));
    }
    impl_->started = true;
    return true;
}

void RecvWorkerPool::stop() noexcept
{
    if (impl_ == nullptr) return;
    std::vector<RecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (!impl_->started && impl_->workers.empty()) return;
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
        impl_->started = false;
    }
    /* 在锁外 stop：stop 会 join worker 线程，而 worker 线程可能正卡在
     * add/remove_route 的 mtx 上（模块作者从别的线程调用），持锁 join 会死锁。 */
    for (auto* worker : workers) worker->stop();
}

bool RecvWorkerPool::running() const noexcept
{
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->started;
}

std::size_t RecvWorkerPool::worker_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->workers.size();
}

RecvRegisterStatus RecvWorkerPool::add_route(const std::shared_ptr<RecvRouteSource>& route)
{
    if (impl_ == nullptr || !route) return RecvRegisterStatus::invalid_route;
    RecvWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (!impl_->started || impl_->workers.empty()) return RecvRegisterStatus::stopped;
        const std::size_t id = worker_for(route->route_name(), route->domain_id(), impl_->workers.size());
        worker = impl_->workers[id].get();
    }
    return worker->add_route(route);
}

void RecvWorkerPool::remove_route(const RecvRouteSource* route) noexcept
{
    if (impl_ == nullptr || route == nullptr) return;
    std::vector<RecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
    }
    /* 逐个 worker 调用（每个都是幂等的）：不依赖 route 上的虚函数 ——
     * remove_route 的语义是"宿主即将释放 route"，此刻再调用 route_name() 是
     * 多余的风险。归属表在每个 worker 内部，线性查找的代价在话题生命周期上
     * 可以忽略。 */
    for (auto* worker : workers) worker->remove_route(route);
}

bool RecvWorkerPool::backend_available() noexcept
{
    return RecvWorker::backend_available();
}

const char* RecvWorkerPool::backend_name() noexcept
{
    return RecvWorker::backend_name();
}

RecvWorkerStats RecvWorkerPool::stats() const
{
    RecvWorkerStats out;
    if (impl_ == nullptr) return out;
    std::vector<RecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
    }
    for (auto* worker : workers)
    {
        const RecvWorkerStats s = worker->stats();
        out.wait_wakeups += s.wait_wakeups;
        out.wait_timeouts += s.wait_timeouts;
        out.wait_errors += s.wait_errors;
        out.routes_processed += s.routes_processed;
        out.messages_received += s.messages_received;
        out.bytes_received += s.bytes_received;
        out.budget_yields += s.budget_yields;
        out.deferred_drains += s.deferred_drains;
        out.recv_errors += s.recv_errors;
        out.idle_exits += s.idle_exits;
        out.thread_restarts += s.thread_restarts;
        out.recv_once_calls += s.recv_once_calls;
        out.recv_once_over_budget += s.recv_once_over_budget;
        if (s.recv_once_max_ns > out.recv_once_max_ns) out.recv_once_max_ns = s.recv_once_max_ns;
        /* t30/D-23：§10.2 常驻扫描量求和。⛔ 深度两项在池级取**最大值而不是求和**：
         * 逐 worker 的 `deferred_depth_last` 是"该 worker 最近一轮的深度"（仪表量），
         * 把 N 个 worker 的仪表值相加会得到一个不存在的"总和深度"（既非任一时刻的
         * 真实深度，也无法与 `deferred_depth_max` 比较）。池级的正确读法是
         * "最繁忙的那个 worker 有多深" ⇒ 取 max；`scan_*` 三件套是真计数 ⇒ 求和。 */
        out.scan_rounds += s.scan_rounds;
        out.scanned_routes_total += s.scanned_routes_total;
        out.scan_ready_rounds += s.scan_ready_rounds;
        if (s.deferred_depth_last > out.deferred_depth_last) out.deferred_depth_last = s.deferred_depth_last;
        if (s.deferred_depth_max > out.deferred_depth_max) out.deferred_depth_max = s.deferred_depth_max;
        /* t53（R0-10）：结束值的池级聚合纪律 —— 与上面两个"入队前"gauge **相反**：
         *   · `deferred_depth_after_last` = **Σ 每 worker**（全池**当前**总深度；任一 gauge
         *     都不能表达它 —— t44 自测反例：全池 12 而 gauge 7）；
         *   · `deferred_depth_after_total` / `scan_ready_routes_total` /
         *     `scan_elapsed_ns_total` = **sum**（都是可安全相加的累计量）；
         *   · `deferred_depth_after_max` = **max**（峰值，与 `deferred_depth_max` 同纪律）。 */
        out.scan_ready_routes_total += s.scan_ready_routes_total;
        out.deferred_depth_after_last += s.deferred_depth_after_last;
        out.deferred_depth_after_total += s.deferred_depth_after_total;
        out.scan_elapsed_ns_total += s.scan_elapsed_ns_total;
        if (s.deferred_depth_after_max > out.deferred_depth_after_max)
            out.deferred_depth_after_max = s.deferred_depth_after_max;
        out.route_count += s.route_count;
    }
    return out;
}

std::size_t RecvWorkerPool::route_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::vector<RecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
    }
    std::size_t total = 0;
    for (auto* worker : workers) total += worker->route_count();
    return total;
}

const RecvBudget& RecvWorkerPool::budget() const noexcept
{
    static const RecvBudget kDefault{};
    if (impl_ == nullptr) return kDefault;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->workers.empty()) return kDefault;
    return impl_->workers.front()->budget();
}

}   // namespace threepools
}   // namespace dzIPC
