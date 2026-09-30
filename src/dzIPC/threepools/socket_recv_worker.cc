#include "dzIPC/threepools/socket_recv_worker.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "libipc/utility/log.h"

namespace dzIPC {
namespace threepools {
namespace {

using Clock = std::chrono::steady_clock;

/* 与 recv_worker.cc 的 fnv1a64_route **完全同一常量与算法**（FNV1a64(route_name ‖ domain_id)）。
 * 两处刻意各自内联而不共用一个导出符号：归属计算必须是纯函数，且不得引入跨 TU 依赖或
 * std::string 构造（add_route 在话题建立路径上）。两边对同一 ASCII 名字与 domain 逐位一致，
 * 因此 SHM 侧与 socket 侧的"同名同 domain ⇒ 同 worker 序号"可对照。 */
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

/* 能力探测的三态缓存（与 SHM 侧同一形状，契约 §4.5：进程内缓存，不得运行中来回切换）：
 *   0 = 未探测（乐观：允许第一次尝试）
 *   1 = 可用（曾成功 add 过）
 *   2 = 不可用（首次 add 失败，永久回退 —— 此后直接返回 backend_unavailable，
 *       模块作者据此保留每通道一条兼容收包线程）
 * 真探测由 SocketWaitSet 内部完成（epoll_create1），本层只观察 add 的返回值。 */
std::atomic<int>& backend_state() noexcept
{
    static std::atomic<int> state{0};
    return state;
}

constexpr std::size_t kMaxWorkerCount = 128;
/* has_pending() 恒真的防饥饿上限（与 SHM 侧同名常量同值）。 */
constexpr std::size_t kMaxEmptyPollsPerBudget = 4;
/* 连续"被内核报告就绪却读不到东西"的次数上限。达到它即进入**退避**（见 Entry 的
 * backoff_until 与 rearm_backoffs 的说明）—— 修复前这里只做 remove+add 重挂并
 * 清零计数，而 fd 仍是 LT 可读 ⇒ wait(0) 立刻再报就绪 ⇒ 实测 1.31M 次/s 忙转
 * （W04-F3）。 */
constexpr std::size_t kMaxFruitlessReadiness = 4;
/* 假就绪退避的**上限**。取 10ms 而不是 wait_timeout(100ms)：
 *   · 病理通道的循环速率收敛到 ≤ 100 次/s（修复前实测 1.31M 次/s，见 W04-F3）；
 *   · 真数据到达后的最坏重新检查延迟 ≤ 10ms ≪ wait_timeout(100ms)，也不影响
 *     "消息/断开/注销都有可靠唤醒"这条契约（方案 §10.2 明令不得为降 CPU 牺牲恢复延迟）。
 * 首档只有 1ms：偶发的瞬时假就绪（坏校验包被内核丢弃）几乎不付代价。 */
constexpr std::chrono::milliseconds kFruitlessBackoffMax{50};
constexpr std::chrono::milliseconds kFruitlessBackoffFirst{1};

/* 按"退避级数"指数增长：4 → 1ms, 5 → 2ms, 6 → 4ms, 7 → 8ms, 8 → 16ms, 9 → 32ms,
 * ≥10 → 50ms 封顶。每个退避周期最多 kMaxFruitlessReadiness(4) 次 recv_once + 1 次重挂
 * ⇒ 稳态循环速率上界 ≈ 5 / 50ms = **100 次/s**（修复前实测 1.31M 次/s）。 */
std::chrono::milliseconds backoff_delay(std::size_t fruitless) noexcept
{
    auto d = kFruitlessBackoffFirst;
    for (std::size_t i = kMaxFruitlessReadiness; i < fruitless && d < kFruitlessBackoffMax; ++i)
    {
        d *= 2;
    }
    return d < kFruitlessBackoffMax ? d : kFruitlessBackoffMax;
}

std::uint64_t steady_now_ns() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

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

/* DZIPC_SOCKET_RECV_WORKERS 覆盖：**进程内只读一次**（static const 初始化一次）。
 * 0 = 未设置/非法 ⇒ 用 hardware_concurrency()。这样做是因为"读环境变量"一旦放进
 * start() 里，同一进程不同调用点可能读到不同值（模块作者在测试里改 env），而池是
 * 一次性单例；缓存一次让线程数口径确定且可断言。 */
std::size_t env_worker_count() noexcept
{
    static const std::size_t cached = []() -> std::size_t {
        const char* s = std::getenv("DZIPC_SOCKET_RECV_WORKERS");
        if (s == nullptr || *s == '\0') return 0;
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (end == s) return 0;   // 非数字 ⇒ 当作未设置
        return static_cast<std::size_t>(v);
    }();
    return cached;
}

}   // namespace

struct SocketRecvWorker::Impl
{
    /* 一条在册 route 的全部 worker 侧状态。用 shared_ptr 保活：注销时从 routes
     * 摘除后，deferred 里可能还留着一份，正在跑的那一轮也持有一份。 */
    struct Entry
    {
        std::shared_ptr<SocketRecvRouteSource> route;
        SocketWaitToken token;
        const SocketRecvRouteSource* key{nullptr};

        /* 连续"报告就绪却没读到数据"的轮数（读到数据即清零）。socket 侧没有 SHM 的
         * sequence 字可做事实判据，唯一可用的事实是"内核说可读"，而它可能因为
         * 坏校验包被内核丢弃等原因出现瞬时假就绪。
         *
         * W04-F3 修复：达到 kMaxFruitlessReadiness 后**不再**只做 remove+add 重挂
         * （那样 fd 仍 LT 可读 ⇒ wait(0) 立刻再报就绪 ⇒ 实测 1.31M 次/s 忙转），而是
         * 把该 token 从等待集合**摘出**并记下重新挂载时刻（backoff_until_ns），
         * 退避时长按连续假就绪次数指数增长（1→2→4→10 ms 封顶）。退避期内 wait 不再
         * 被这个 fd 唤醒 ⇒ 循环速率有界；真数据到达后的最坏重新检查延迟 ≤ 10ms。 */
        std::atomic<std::size_t> fruitless{0};
        /* 退避截止时刻（steady_clock 的 ns 计数）；0 = 未处于退避。 */
        std::atomic<std::uint64_t> backoff_until_ns{0};
        /* 退避**级数**：只增（读到真数据才清零）。它决定退避时长，而 `fruitless` 在每次
         * 到期重挂时清零 —— 两者必须分开：
         *   · 若重挂时不清 `fruitless`，重挂后第一次上报就再次 ≥ 阈值 ⇒ 立刻又被摘出，
         *     该通道**永远轮不到一次 recv_once** ⇒ 真数据到达也收不到（实测：恢复延迟
         *     2000ms 超时，正是 W04-F3 修复的第一版）；
         *   · 若清 `fruitless` 但用同一个计数做时长，则时长永远停在首档 ⇒ 速率不再下降。
         * 分开后每轮退避周期的成本 = 最多 kMaxFruitlessReadiness 次 recv_once + 一次
         * 到期重挂，而周期长度按级数指数增长 ⇒ 循环速率有界且随病理持续下降。 */
        std::atomic<std::size_t> backoff_level{0};

        /* 预算轮窗口计数。**整个 run_budget 都在窗口内**（上界 = 三项预算 + 一次
         * recv_once），而不是只有 recv_once 那一瞬 —— 否则 remove_route 等到 0 之后，
         * worker 循环可能立刻又对同一条 route 发起下一次 recv_once（TOCTOU）。 */
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

    /* **每 worker 自有**一个 SocketWaitSet（socket_wait_set.h 明确它不是进程单例）：
     * 共用一个集合会让 A worker 的 remove/stop 唤醒 B worker 的 wait。 */
    SocketWaitSet wait_set;

    mutable std::mutex mtx;
    std::vector<std::shared_ptr<Entry>> routes;   ///< 固定归属表（本 worker 独占）
    std::deque<std::shared_ptr<Entry>> deferred;  ///< 预算耗尽/待处理 FIFO

    std::thread thread;
    std::atomic<bool> running{false};    ///< **活动期**语义（start 之后、stop 之前恒 true）
    std::atomic<bool> stopping{false};   ///< 请求停止（幂等，任何线程可置）
    std::atomic<bool> joined{false};     ///< join 所有权（恰好一个线程执行）

    /* ---- idle keep-alive 的线程生命周期状态（与 SHM 侧同一机制）----
     *   alive               : 当前确有一条工作线程在 loop 内。
     *   thread_started_once : 区分"首次启动"与"重拉起"（thread_restarts 计数）。
     *   join_mtx            : **串行化 std::thread 对象的 create/join**，故意与 mtx
     *                         分离：join 绝不持 mtx，否则"拉起者持 mtx 等旧线程退出、
     *                         旧线程等 mtx"构成循环。
     *   thread_epoch        : 代际编号，旧代收尾不得清掉新代的 alive。 */
    std::atomic<bool> alive{false};
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
    std::atomic<std::uint64_t> idle_exits{0};
    std::atomic<std::uint64_t> thread_restarts{0};
    /* W04-F3 追加：假就绪退避的进入次数与到期重挂次数。诊断/验收用 ——
     * 两者都只增不减；"进入次数持续增长"说明确有病理性假就绪通道（真信号），
     * 而修复后 CPU 不再被它吃满（循环速率由 backoff 限制）。 */
    std::atomic<std::uint64_t> fruitless_backoffs{0};
    std::atomic<std::uint64_t> rearm_events{0};

    void loop();
    void thread_main(std::uint64_t epoch) noexcept;
    void wait_once(std::chrono::milliseconds timeout);
    void collect_pending();
    bool rearm_backoffs(std::chrono::milliseconds* next_due);
    void requeue_ready(std::vector<SocketWaitToken>&& ready);
    bool ensure_thread_alive();
    bool requeue(const std::shared_ptr<Entry>& entry);
    void drain_deferred();
    void run_budget(const std::shared_ptr<Entry>& entry);
    bool deferred_empty();
};

/* 把 wait-set 交出的就绪 token 映射回在册 route 并入 deferred。
 *
 * 三件事必须一起做，缺任何一条都会出问题：
 *   1. **只认当前在册的 token**：SocketWaitSet::consume_ready 已经按在册集合过滤
 *      （fd 复用不误关联），这里再按本 worker 的 route 表映射一次，彻底挡住
 *      "已 remove 的通道被重新点名"。
 *   2. **去重**（Entry::queued CAS）：同一个 token 在一次 wait 里可能被报告多次，
 *      也可能上一轮已经被 drain 排进 queue；重复入队会让同一条 route 在 FIFO 里
 *      占多个位置，破坏"固定 FIFO 不饿死冷 route"的前提。
 *   3. **病理假就绪的兜底（W04-F3 修复）**：连续 kMaxFruitlessReadiness 轮"报告就绪
 *      但读不到数据"的 token 会被从等待集合**摘出**并退避（1→2→4→10 ms 指数增长，
 *      见 backoff_delay），到期后由 loop() 重新挂载。
 *      ⛔ 修复前这里是"摘掉再立刻放回 + 清零计数"，而 fd 仍是 LT 可读 ⇒ 下一轮
 *      `wait(0)` 立刻又报就绪 ⇒ 实测 1.31M 次/s 忙转（W04-F3，方案 §4.5 明令
 *      "⛔ 不得忙轮询"）。把退避做成"摘出 + 到期重挂"之后，退避期内核事件不再唤醒
 *      本 worker ⇒ 循环速率有界（≤ 100 次/s），而真数据到达后的最坏重新检查延迟
 *      ≤ kFruitlessBackoffMax(10ms)，**没有**用恢复延迟换 CPU。 */
void SocketRecvWorker::Impl::requeue_ready(std::vector<SocketWaitToken>&& ready)
{
    std::vector<SocketWaitToken> rearm;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& token : ready)
        {
            const auto it = std::find_if(routes.begin(), routes.end(),
                                         [&](const std::shared_ptr<Entry>& e) { return e->token == token; });
            if (it == routes.end()) continue;   // 已注销 / 未知通道：跳过
            const std::shared_ptr<Entry>& entry = *it;
            if (entry->removed.load(std::memory_order_acquire)) continue;
            bool expected = false;
            if (!entry->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                continue;   // 已在队列里
            if (entry->fruitless.load(std::memory_order_relaxed) >= kMaxFruitlessReadiness)
            {
                entry->queued.store(false, std::memory_order_release);
                /* 退避时长按**级数**增长（级数只增到读到真数据为止）；同时把
                 * `fruitless` 清零，让重挂后有一次完整的服务机会（见 Entry::backoff_level
                 * 的说明 —— 不清零会让该通道永久失聪）。 */
                const std::size_t level = entry->backoff_level.load(std::memory_order_relaxed);
                const auto need = backoff_delay(kMaxFruitlessReadiness + level);
                const std::uint64_t until = steady_now_ns()
                    + static_cast<std::uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(need).count());
                entry->fruitless.store(0, std::memory_order_relaxed);
                entry->backoff_level.fetch_add(1, std::memory_order_relaxed);
                if (entry->backoff_until_ns.exchange(until, std::memory_order_acq_rel) == 0)
                {
                    /* 只在**进入**退避那一次计数（重复上报不重复计）。计数点必须在这里、
                     * 不能放 run_budget：达到阈值的那一刻 token 就被摘出等待集合。 */
                    fruitless_backoffs.fetch_add(1, std::memory_order_relaxed);
                }
                rearm.push_back(token);   // 摘出等待集合，到期由 loop() 重挂
                continue;
            }
            deferred.push_back(entry);
        }
    }
    /* 摘出在锁外做：SocketWaitSet 内部自锁，且 remove 会敲唤醒通道。 */
    for (const auto& token : rearm)
    {
        wait_set.remove(token);
    }
}

/* W04-F3：把退避到期的 entry 重新挂回等待集合，并返回「最近一个到期时刻」用于
 * loop() 选择等待切片（返回 nullopt = 当前没有处于退避的 entry）。
 *
 * ⛔ 为什么必须让 loop 知道最早到期时刻：退避期内该 fd **不在**等待集合里，worker
 *    若照常 `wait(wait_timeout)` 就只能在切片结束时才发现"该重挂了"——
 *    真数据到达时最坏多等一个 wait_timeout(100ms)。返回最近到期时刻后，loop 取
 *    `min(wait_timeout, 最早到期 - now)`，把最坏延迟压到 kFruitlessBackoffMax(10ms) 量级。
 * ⛔ 重挂必须与 remove 配对，且只在到期后做（未到期就重挂 = 退回忙转）。 */
bool SocketRecvWorker::Impl::rearm_backoffs(std::chrono::milliseconds* next_due)
{
    std::vector<SocketWaitToken> tokens;
    const std::uint64_t now = steady_now_ns();
    std::int64_t soonest_ns = -1;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& entry : routes)
        {
            if (entry->removed.load(std::memory_order_acquire)) continue;
            const std::uint64_t until = entry->backoff_until_ns.load(std::memory_order_acquire);
            if (until == 0) continue;
            if (now >= until)
            {
                entry->backoff_until_ns.store(0, std::memory_order_release);
                tokens.push_back(entry->token);
            }
            else
            {
                const std::int64_t delta = static_cast<std::int64_t>(until - now);
                if (soonest_ns < 0 || delta < soonest_ns) soonest_ns = delta;
            }
        }
    }
    for (const auto& token : tokens)
    {
        wait_set.add(token);
        rearm_events.fetch_add(1, std::memory_order_relaxed);
    }
    if (soonest_ns < 0)
    {
        return false;
    }
    if (next_due != nullptr) *next_due = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds{soonest_ns});
    return true;
}

/* 一次**非阻塞**全量探测（wait(0) + 消费就绪）。loop 每轮开头调用它，作用是
 * "drain 之前先看一眼内核"：deferred 非空时若不做这一步，热 route 让出后新一轮
 * 又立刻被热 route 占满，冷 route 可能长期轮不到（方案 §C7 实测冷 topic 30 条只到 1 条）。
 * 注意 wait(0) 返回 false 不是错误、也不计入 wait_timeouts：它只表示"此刻没有就绪"。 */
void SocketRecvWorker::Impl::collect_pending()
{
    if (!wait_set.wait(std::chrono::milliseconds{0}))
    {
        return;
    }
    requeue_ready(wait_set.consume_ready());
}

bool SocketRecvWorker::Impl::requeue(const std::shared_ptr<Entry>& entry)
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

bool SocketRecvWorker::Impl::deferred_empty()
{
    std::lock_guard<std::mutex> lock(mtx);
    return deferred.empty();
}

void SocketRecvWorker::Impl::drain_deferred()
{
    /* 只处理"进入本函数时队列里的那些项"：run_budget 让出时 requeue 到队尾的 route
     * 留到下一轮，于是同 worker 的其他 route 一定先被轮到（固定 FIFO，热 route 不会
     * 长期饿死冷 route）。 */
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

void SocketRecvWorker::Impl::run_budget(const std::shared_ptr<Entry>& entry)
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
    const auto deadline = Clock::now() + budget.max_processing_time_per_route;

    for (;;)
    {
        if (stopping.load(std::memory_order_acquire)) break;
        /* remove_route 可能在本次预算轮进行中完成摘除 + 标记（它在另一个线程）。
         * 契约 §4.4 第 1 步要求"禁止新的 recv_once"：一旦标记就必须立刻停手，不再对
         * 这条 route 发起下一次调用。在途的那一次仍要等它返回 —— 那是 remove_route
         * 第 4 步（等 in_flight 归零）的职责。 */
        if (entry->removed.load(std::memory_order_acquire)) break;

        std::size_t n = 0;
        try
        {
            n = entry->route->recv_once();
        }
        catch (...)
        {
            /* 宿主实现的异常不得逃出 worker 线程（否则整个进程 terminate）。 */
            recv_errors.fetch_add(1, std::memory_order_relaxed);
            ipc::error("SocketRecvWorker: recv_once threw for route '%s'\n",
                       entry->route->route_name() != nullptr ? entry->route->route_name() : "?");
            more = true;
            break;
        }

        if (n == 0)
        {
            /* 契约：0 = 无数据/断开。has_pending() 为真说明还有可收数据（level-triggered
             * 重检），再取一次；但它恒真时必须有上限，否则一个坏实现就能把整个 worker
             * 钉死在这条 route 上。 */
            if (entry->route->has_pending() && ++empty_polls < kMaxEmptyPollsPerBudget) continue;
            break;
        }
        empty_polls = 0;
        /* 真读到东西：假就绪计数、退避级数与退避截止一并清零（读了就是读了 —— 退避没有
         * 理由延续；这也是"恢复延迟不被拖长"的机制保证）。 */
        entry->fruitless.store(0, std::memory_order_relaxed);
        entry->backoff_level.store(0, std::memory_order_relaxed);
        entry->backoff_until_ns.store(0, std::memory_order_relaxed);
        ++messages;
        bytes += n;

        /* **安全边界**：预算只在"一次完整 recv_once 返回"之后检查。
         * ⛔ 不得在组包中途切走（socket 侧一次 chunk_rev_* 可能持续到整条消息/请求组装完成）。 */
        if (messages >= budget.max_messages_per_route || bytes >= budget.max_bytes_per_route
            || Clock::now() >= deadline)
        {
            more = true;
            break;
        }
    }

    if (more || entry->route->has_pending())
    {
        more = true;
    }
    /* socket 侧没有 sequence 字可推进，"本 route 这一轮到此为止"的判据就是上面的
     * more/has_pending；未让出且读空时不做任何事，等下一次内核报就绪。 */
    if (messages == 0)
    {
        /* 连续空读计数。达到 kMaxFruitlessReadiness 后由 requeue_ready() 把它摘出等待
         * 集合并退避（W04-F3），进入退避的那一次在那里计数；只有真读到数据才清零。 */
        entry->fruitless.fetch_add(1, std::memory_order_relaxed);
    }

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
    }
}

/* 一次**阻塞**等待 + 就绪消费。超时不是错误（只有后端已被判定不可用才算错误）。
 * 单独成函数是因为 loop() 里有两种等待时长（普通 wait_timeout / 空闲窗口的剩余）。 */
void SocketRecvWorker::Impl::wait_once(std::chrono::milliseconds timeout)
{
    if (!wait_set.wait(timeout))
    {
        if (backend_state().load(std::memory_order_acquire) == 2)
            wait_errors.fetch_add(1, std::memory_order_relaxed);
        else
            wait_timeouts.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    wait_wakeups.fetch_add(1, std::memory_order_relaxed);
    /* 必须消费：否则 wait-set 内部的 ready 缓存会累积，且下次 wait 立刻返回（把超时
     * 语义变成忙转）。就绪内容由这里的映射重新发现。 */
    requeue_ready(wait_set.consume_ready());
}

void SocketRecvWorker::Impl::loop()
{
    /* 空闲计时："连续无待处理项"从 idle_since 起算，任何一次注册/待处理活动都重置它。
     * 窗口取 max(idle_keep_alive, wait_timeout) 作下界（idle_keep_alive == 0 例外，保持
     * "下一轮空闲检查即退出"的字面语义，仅供测试）—— 理由与 SHM 侧逐字相同：一次 wait
     * 切片最长 wait_timeout，窗口若比它还小，退出时机就变成一个不变量依赖。 */
    auto idle_since = Clock::now();
    const auto idle_span = budget.idle_keep_alive.count() <= 0
                             ? std::chrono::milliseconds{0}
                             : std::max<std::chrono::milliseconds>(budget.idle_keep_alive, budget.wait_timeout);

    while (!stopping.load(std::memory_order_acquire))
    {
        /* 非阻塞全量探测：deferred 非空时也要先看一眼内核（防热 route 饿死冷 route）。 */
        collect_pending();

        /* 假就绪退避的到期重挂（W04-F3）：把到期项放回等待集合，并取回最近一个未到期
         * 项的剩余时长，用于把下面两个 wait 切片压到它以内 —— 这样真数据到达后的最坏
         * 重新检查延迟是 kFruitlessBackoffMax(10ms)，而不是一整个 wait_timeout(100ms)。 */
        std::chrono::milliseconds backoff_due{0};
        const bool has_backoff = rearm_backoffs(&backoff_due);

        if (deferred_empty())
        {
            const auto now = Clock::now();
            const auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - idle_since);
            if (idle_for < idle_span)
            {
                /* 空闲等待是 wait-set 的**阻塞**等待：取 min(wait_timeout, 剩余空闲窗口)，
                 * 下界 1 ms 防止 wait(0) 退化成忙轮询。 */
                auto slice = std::min(budget.wait_timeout, idle_span - idle_for);
                if (has_backoff && backoff_due >= std::chrono::milliseconds{1}
                    && backoff_due < slice)
                {
                    slice = backoff_due;
                }
                if (slice < std::chrono::milliseconds{1}) slice = std::chrono::milliseconds{1};
                wait_once(slice);
                if (!deferred_empty()) idle_since = Clock::now();
                continue;
            }

            /* 连续空闲 >= 窗口 ⇒ 空闲退出（idle exit）。
             * 与 add_route 的收敛：线程退出的唯一途径是"持有 mtx 且看到表空"，而
             * add_route 的"插入 route 表 + 按需拉起"由同一 mtx 临界区保证与它互斥
             * ⇒ "返回 ok 却无人消费"（静默丢包）不可能发生。 */
            std::unique_lock<std::mutex> lock(mtx);
            if (!routes.empty() || !deferred.empty())
            {
                lock.unlock();
                idle_since = Clock::now();
                continue;
            }
            if (stopping.load(std::memory_order_acquire)) break;
            alive.store(false, std::memory_order_release);
            lock.unlock();
            idle_exits.fetch_add(1, std::memory_order_relaxed);
            ipc::log("SocketRecvWorker[%zu]: idle exit (no routes for >= %lld ms)\n", worker_id,
                     static_cast<long long>(idle_span.count()));
            return;
        }

        auto slice = budget.wait_timeout;
        if (has_backoff && backoff_due >= std::chrono::milliseconds{1} && backoff_due < slice)
        {
            slice = backoff_due;
        }
        wait_once(slice);
        idle_since = Clock::now();
        drain_deferred();
    }
}

void SocketRecvWorker::Impl::thread_main(std::uint64_t epoch) noexcept
{
    /* ⛔ 不得让任何异常逃出线程（逃出即 std::terminate）。recv_once 已在 run_budget
     * 内单独兜过；这里是最后一道闸，也覆盖宿主的 wait_token / has_pending 实现面。 */
    try
    {
        loop();
    }
    catch (...)
    {
        ipc::error("SocketRecvWorker[%zu]: exception escaped worker loop\n", worker_id);
    }
    /* 线程真的结束了。running（**活动期**）不受影响：空闲退出只是把线程归还操作系统。
     *
     * ⛔ 清零**必须**带代际检查：本线程离开 loop 之后、执行到这一行为止，另一个
     * add_route 可能已经拉起新线程并把 alive 置回 true；无条件清零会盖掉那次置位
     * ⇒ alive==false 却有活线程 ⇒ 下一次 add_route 再拉起一条 ⇒ 同一 worker 两条消费者。 */
    if (thread_epoch.load(std::memory_order_acquire) == epoch)
    {
        alive.store(false, std::memory_order_release);
    }
}

bool SocketRecvWorker::Impl::ensure_thread_alive()
{
    if (alive.load(std::memory_order_acquire)) return false;
    if (stopping.load(std::memory_order_acquire)) return false;
    if (!running.load(std::memory_order_acquire)) return false;

    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(join_mtx);
        /* **双检，且必须在 join_mtx 内**：两个并发的 add_route 可能同时读到 alive==false
         * 然后前后脚走到这里。只有第一个能建线程；第二个必须在这里再次看到 true 并返回
         * false，否则它会把第一个刚建好的 std::thread 当成"上一代"收尸并再建一条
         * ⇒ 同一 worker 两条消费者（且 thread_restarts 多计）。 */
        if (alive.load(std::memory_order_acquire)) return false;
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!running.load(std::memory_order_acquire)) return false;

        previous = std::move(thread);   // 收尸上一代（已从 loop 返回，join 立即完成）
        /* alive 由**创建者**置位，且在起线程**之前**：
         *   · 否则拉起返回与线程跑起来之间的窗口里，另一个 add_route 会再建一条；
         *   · 反过来（先起线程再置位）新线程可能在置位前就跑完一轮空闲退出并把 alive
         *     置回 false，创建者随后的置位会盖掉那次退出 ⇒ alive==true 却没有任何线程。 */
        const std::uint64_t epoch = thread_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        alive.store(true, std::memory_order_release);
        try
        {
            thread = std::thread([this, epoch] { thread_main(epoch); });
        }
        catch (...)
        {
            /* 线程创建失败（资源耗尽）：置位必须回滚并还原上一代。 */
            alive.store(false, std::memory_order_release);
            thread = std::move(previous);
            return false;
        }
        if (thread_started_once) thread_restarts.fetch_add(1, std::memory_order_relaxed);
        thread_started_once = true;
    }
    if (previous.joinable()) previous.join();
    return true;
}

SocketRecvWorker::SocketRecvWorker(std::size_t worker_id, RecvBudget budget)
    : impl_(new Impl(worker_id, budget))
{}

SocketRecvWorker::~SocketRecvWorker()
{
    stop();
    if (impl_ != nullptr)
    {
        /* 兜底：stop() 若由 worker 线程自己调用（未消费 join 所有权），或线程已空闲退出
         * 但还未被收尸，都留到这里 join 一次，避免 joinable 的 std::thread 走到 terminate。 */
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

bool SocketRecvWorker::start()
{
    if (impl_ == nullptr) return false;
    if (impl_->stopping.load(std::memory_order_acquire)) return false;   // stop 之后不可重启
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true)) return false;
    /* 首次启动也走"按需拉起"同一段代码：只有一处创建线程的地方，alive 的置位与
     * thread 对象的建立因此不可分裂。⛔ 不得在 mtx 内调用（见 add_route 的死锁链）。 */
    return impl_->ensure_thread_alive();
}

void SocketRecvWorker::stop() noexcept
{
    if (impl_ == nullptr) return;
    impl_->stopping.store(true, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
    /* 唤醒阻塞中的 wait（wait-set 的 stop 是幂等的，且会让后续 wait 立即返回 true）。 */
    impl_->wait_set.stop();
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(impl_->join_mtx);
        if (!impl_->thread.joinable()) return;
        /* ⛔ 不得从 worker 线程 join 自己（std::system_error → terminate）：同线程调用
         * 只置停止标志、**不**消费 join 所有权，把 join 留给析构或外部线程。 */
        if (impl_->thread.get_id() == std::this_thread::get_id()) return;
        if (impl_->joined.exchange(true)) return;
        to_join = std::move(impl_->thread);   // 取出后在锁外 join
    }
    to_join.join();
}

bool SocketRecvWorker::running() const noexcept
{
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

bool SocketRecvWorker::thread_alive() const noexcept
{
    return impl_ != nullptr && impl_->alive.load(std::memory_order_acquire);
}

std::size_t SocketRecvWorker::worker_id() const noexcept
{
    return impl_ != nullptr ? impl_->worker_id : 0;
}

RecvRegisterStatus SocketRecvWorker::add_route(const std::shared_ptr<SocketRecvRouteSource>& route)
{
    if (impl_ == nullptr) return RecvRegisterStatus::invalid_route;
    if (!route) return RecvRegisterStatus::invalid_route;
    if (impl_->stopping.load(std::memory_order_acquire) || !impl_->running.load(std::memory_order_acquire))
        return RecvRegisterStatus::stopped;

    const SocketWaitToken token = route->wait_token();
    if (!token.valid()) return RecvRegisterStatus::invalid_token;

    /* 单 route 单消费者：兼容的每通道收包线程正在 recv 时必须返回 busy，由模块作者
     * 保留兼容线程，**不得**静默双收。判定顺序按契约 §4.4 + 勘误 E2：
     * 先 try_claim_recv，失败后再查表区分 duplicate / busy。 */
    if (!route->try_claim_recv(RecvOwner::worker))
    {
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
        ipc::log("SocketRecvWorker: SocketWaitSet backend unavailable; keep one receive thread per channel\n");
        return RecvRegisterStatus::backend_unavailable;
    }
    backend_state().store(1, std::memory_order_release);

    auto entry = std::make_shared<Impl::Entry>();
    entry->route = route;
    entry->token = token;
    entry->key = route.get();

    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->routes.push_back(entry);
        need_start = !impl_->alive.load(std::memory_order_acquire);
    }
    /* ⛔ 拉起**必须**在 mtx **之外**：ensure_thread_alive() 会取 join_mtx（还可能要 join
     * 旧代线程），而 stop() 持 join_mtx 等 worker 线程退出、线程退出前可能还要取 mtx
     * ⇒ 在 mtx 内调它就是 mtx → join_mtx 的反向获取，三方交叠即死锁。
     * 原子性由两个不变量拼出（同 SHM 侧）：线程清 alive 与它判定 route 表为空在同一个 mtx
     * 临界区内；本函数插入 route 与读 alive 也在同一个 mtx 临界区内。两者互斥 ⇒ 读到
     * 哪个值都安全，"返回 ok 却无人消费"（静默丢包）被禁止。 */
    bool have_thread = !need_start;
    if (need_start)
    {
        (void)impl_->ensure_thread_alive();
        have_thread = impl_->alive.load(std::memory_order_acquire);
    }
    if (!have_thread)
    {
        /* 并发 stop()：活动期已结束，线程不会被拉起。**回滚**本次注册并显式返回
         * stopped —— 禁止"返回 ok 却无人消费"。此刻 worker 从未见过这条 entry，回滚
         * 只需摘表 + 摘等待集合 + 归还收包独占，无需 stop_and_wake / wait_quiescent。 */
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

    /* 注册即让新 route **至少被处理一次**（无条件入 deferred，不依赖新事件）：
     * 注册之前已经在 socket 接收队列里的数据不会产生新的可读事件，只靠 ready 判据会
     * 一直看不到它。代价是每条 route 注册时多一次 recv_once（非阻塞、读到 0 即让出）。 */
    (void)impl_->requeue(entry);
    impl_->wait_set.remove(token);
    impl_->wait_set.add(token);   // 敲一次唤醒通道，让 worker 立刻重扫
    return RecvRegisterStatus::ok;
}

void SocketRecvWorker::remove_route(const SocketRecvRouteSource* route) noexcept
{
    if (impl_ == nullptr || route == nullptr) return;

    /* W04-F1（队长裁决 D-11）：同线程调用是不支持的用法（见 SHM 侧同名注释与头文件
     * 的 grep 判据）。debug 构建下当场断言；Release 下由 2000ms 超时 + 诊断日志给出
     * 运行期可观测信号。 */
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
        /* 摘除与标记在**同一个锁区间**内完成（worker 侧双检因此能看到一致的 removed）。 */
        entry->removed.store(true, std::memory_order_release);
    }

    /* 1. 摘除 + 标记 removed 已在上面完成（禁止新的 recv_once）。 */

    /* 2. wait_set.remove：从等待集合摘掉并**唤醒阻塞中的 worker wait**（不能靠
     *    epoll_ctl(DEL)：本机实测它不会唤醒 epoll_wait，唤醒走内部 eventfd）。
     *    这一步也是宿主随后 close(fd) 的前提：remove 是同步摘除。 */
    (void)impl_->wait_set.remove(entry->token);

    /* 3. 禁止新工作 + 打断在途 recv（宿主实现内部应调 udp_node_cancel_wait）。 */
    if (entry->route) entry->route->stop_and_wake();

    /* 4. 等 worker 侧的预算轮窗口归零（**在表锁之外**等待：worker 增减这个计数不需要
     *    mtx，不存在"注销等 worker、worker 等锁"的循环）。有界：上界 = 一个预算轮。 */
    if (!quiesce_or_timeout(entry->in_flight))
    {
        ipc::error("SocketRecvWorker: route '%s' recv_once did not quiesce within %lld ms\n",
                   route->route_name() != nullptr ? route->route_name() : "?",
                   static_cast<long long>(kQuiesceTimeout.count()));
    }

    /* 5. 等模块自己的 lease/in-flight 记账归零。 */
    if (entry->route) entry->route->wait_quiescent();

    /* 6. 归还收包独占（owner 回到 none），之后兼容线程才允许重新接管。 */
    if (entry->route) entry->route->release_recv();

    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->deferred.erase(std::remove(impl_->deferred.begin(), impl_->deferred.end(), entry),
                              impl_->deferred.end());
        entry->queued.store(false, std::memory_order_release);
    }
}

void SocketRecvWorker::wakeup() noexcept
{
    if (impl_ == nullptr) return;
    /* SocketWaitSet 没有独立的 wakeup 接口，但 remove 会敲唤醒通道（内部 eventfd）、
     * add 会重新登记。对**任意一条**在册 token 做一次 remove+add 就等于唤醒 —— 唤醒
     * 通道是集合级的，与具体 token 无关。
     * 这样**不会**丢失就绪提示：清掉的只是 SocketWaitSet 的用户态 ready 残留，事实来源
     * 是内核的 LT 可读状态，下一轮 wait 会重新取证。 */
    SocketWaitToken token;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->routes.empty()) return;
        token = impl_->routes.front()->token;
    }
    impl_->wait_set.remove(token);
    impl_->wait_set.add(token);
}

RecvWorkerStats SocketRecvWorker::stats() const
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
    out.fruitless_backoffs = impl_->fruitless_backoffs.load(std::memory_order_relaxed);
    out.rearm_events = impl_->rearm_events.load(std::memory_order_relaxed);
    out.route_count = route_count();
    return out;
}

std::size_t SocketRecvWorker::route_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->routes.size();
}

bool SocketRecvWorker::backend_available() noexcept
{
    return backend_state().load(std::memory_order_acquire) != 2;
}

const char* SocketRecvWorker::backend_name() noexcept
{
    /* 中性名字：真实后端名由 SocketWaitSet 内部决定（Linux epoll / Windows
     * WaitForMultipleObjects），本层的公开头不做平台分支。诊断请用
     * SocketWaitSet::backend_name()。 */
    return backend_state().load(std::memory_order_acquire) == 2 ? "none" : "socket_wait_set";
}

/* ==================== SocketRecvWorkerPool ==================== */

struct SocketRecvWorkerPool::Impl
{
    std::mutex mtx;
    std::vector<std::unique_ptr<SocketRecvWorker>> workers;
    bool started{false};
};

SocketRecvWorkerPool& SocketRecvWorkerPool::instance()
{
    /* 故意泄漏的指针单例：理由见头文件（与 RecvWorkerPool / LocalPubSubRegistry /
     * ShmControlScheduler 同构）。 */
    static SocketRecvWorkerPool* pool = new SocketRecvWorkerPool();
    return *pool;
}

std::size_t SocketRecvWorkerPool::worker_for(const char* route_name,
                                             std::uint32_t domain_id,
                                             std::size_t worker_count) noexcept
{
    if (route_name == nullptr || worker_count == 0) return 0;
    return static_cast<std::size_t>(fnv1a64_route(route_name, domain_id) % worker_count);
}

SocketRecvWorkerPool::SocketRecvWorkerPool()
    : impl_(new Impl)
{}

SocketRecvWorkerPool::~SocketRecvWorkerPool()
{
    stop();
    delete impl_;
    impl_ = nullptr;
}

bool SocketRecvWorkerPool::start(std::size_t worker_count, const RecvBudget& budget)
{
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    /* 池是**一次性**的进程级单例：stop() 之后不允许再 start（理由同 SHM 侧：旧 worker
     * 还留在 workers 里会让 worker_for 的模数变化 + stats/route_count 重复统计）。 */
    if (impl_->started || !impl_->workers.empty()) return false;
    if (worker_count == 0) worker_count = env_worker_count();
    if (worker_count == 0)
    {
        worker_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (worker_count == 0) worker_count = 1;
    }
    if (worker_count > kMaxWorkerCount) worker_count = kMaxWorkerCount;
    impl_->workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        auto worker = std::unique_ptr<SocketRecvWorker>(new SocketRecvWorker(i, budget));
        if (!worker->start())
        {
            /* 启动失败：把已经起来的收回去，保持"要么全起、要么没起"的状态，否则
             * worker_for 计算出的归属会落到一个不存在的 worker 上。 */
            for (auto& started : impl_->workers) started->stop();
            impl_->workers.clear();
            return false;
        }
        impl_->workers.push_back(std::move(worker));
    }
    impl_->started = true;
    return true;
}

void SocketRecvWorkerPool::stop() noexcept
{
    if (impl_ == nullptr) return;
    std::vector<SocketRecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (!impl_->started && impl_->workers.empty()) return;
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
        impl_->started = false;
    }
    /* 在锁外 stop：stop 会 join worker 线程，而 worker 线程可能正卡在 add/remove_route
     * 的 mtx 上（模块作者从别的线程调用），持锁 join 会死锁。 */
    for (auto* worker : workers) worker->stop();
}

bool SocketRecvWorkerPool::running() const noexcept
{
    if (impl_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->started;
}

std::size_t SocketRecvWorkerPool::worker_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->workers.size();
}

RecvRegisterStatus SocketRecvWorkerPool::add_route(const std::shared_ptr<SocketRecvRouteSource>& route)
{
    if (impl_ == nullptr || !route) return RecvRegisterStatus::invalid_route;
    SocketRecvWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (!impl_->started || impl_->workers.empty()) return RecvRegisterStatus::stopped;
        const std::size_t id = worker_for(route->route_name(), route->domain_id(), impl_->workers.size());
        worker = impl_->workers[id].get();
    }
    return worker->add_route(route);
}

void SocketRecvWorkerPool::remove_route(const SocketRecvRouteSource* route) noexcept
{
    if (impl_ == nullptr || route == nullptr) return;
    std::vector<SocketRecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
    }
    /* 逐个 worker 调用（每个都是幂等的）：不依赖 route 上的虚函数 —— 此刻宿主即将
     * 释放 route，再调用 route_name() 是多余的风险。 */
    for (auto* worker : workers) worker->remove_route(route);
}

bool SocketRecvWorkerPool::backend_available() noexcept
{
    return SocketRecvWorker::backend_available();
}

const char* SocketRecvWorkerPool::backend_name() noexcept
{
    return SocketRecvWorker::backend_name();
}

RecvWorkerStats SocketRecvWorkerPool::stats() const
{
    RecvWorkerStats out;
    if (impl_ == nullptr) return out;
    std::vector<SocketRecvWorker*> workers;
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
        out.fruitless_backoffs += s.fruitless_backoffs;
        out.rearm_events += s.rearm_events;
        out.route_count += s.route_count;
    }
    return out;
}

std::size_t SocketRecvWorkerPool::route_count() const noexcept
{
    if (impl_ == nullptr) return 0;
    std::vector<SocketRecvWorker*> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        workers.reserve(impl_->workers.size());
        for (auto& worker : impl_->workers) workers.push_back(worker.get());
    }
    std::size_t total = 0;
    for (auto* worker : workers) total += worker->route_count();
    return total;
}

}   // namespace threepools
}   // namespace dzIPC
