#include "dzIPC/threepools/shm_control_scheduler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dzIPC {
namespace shm_control {
namespace {

/* "当前线程就是调度 worker"的标志。
 *
 * 为什么用 thread_local 而不是 std::atomic<std::thread::id>：语义完全一致
 * （worker 线程体第一句置位，其余线程恒为 false），却不需要任何同步，也不依赖
 * std::atomic<std::thread::id> 这个非保证特化。用途只有一个 —— unregister 检测
 * "回调内注销"这种会自死锁的误用（见头文件约束 ②）。 */
thread_local bool t_on_control_worker = false;

/* 诊断唯一出口：**不分配、不抛**。
 *
 * 与 src/dzIPC 其余诊断同一口径：黄色前缀 + 单个 std::fprintf(stderr)。
 * 不依赖 dzipc_log —— 调度器是通用控制面组件，日志开关由调用方决定，
 * 而"坏项被隔离"属于静默失效类，不该要求开 verbose 才看得见
 * （先例：src/dzIPC/shm_pub_sub_ipc.cc:694-704）。
 *
 * ⚠️ 为什么必须是"变参 + 定长栈 buffer"，而不是先拼 std::string 再打印：
 * 本函数在 catch 块内被调用（隔离坏项、worker 兜底），若拼接本身抛 bad_alloc，
 * 异常会逃出那个 catch ⇒ std::terminate —— 把"隔离一个坏项"变成"带走整个进程"，
 * 恰好是隔离机制要防的事。vsnprintf 没有任何分配点。
 * 这条对**所有**调用点成立，因此不存在"普通路径用 std::string 版、异常路径用
 * 变参版"的分工：只有一个版本，就不会有人误用错的那个。 */
void warnf(const char* fmt, ...) noexcept
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\033[33m[ShmControlScheduler] %s\033[0m\n", buf);
}

}   // namespace

/* 一个注册项。所有可变的裸字段（tick_inflight / next_due）只在 impl_->mtx 下访问；
 * inactive 是 std::atomic 以便 tick 在锁外做快速判断（也便于将来无锁化）。 */
struct Entry
{
    enum class Kind
    {
        Subscriber,
        Publisher
    };

    ShmControlScheduler::EntryId id{ShmControlScheduler::kInvalidEntry};
    Kind kind{Kind::Subscriber};
    std::shared_ptr<SubControlState> sub;
    std::shared_ptr<PubControlState> pub;
    ControlTiming timing{};
    std::atomic<bool> inactive{false};
    bool tick_inflight{false};
    ControlClock::time_point next_due{};

    std::chrono::milliseconds period() const
    {
        return (kind == Kind::Subscriber) ? timing.sub_heartbeat : timing.pub_heartbeat;
    }

    const char* name() const noexcept
    {
        if (kind == Kind::Subscriber)
        {
            return (sub != nullptr) ? sub->debug_name() : nullptr;
        }
        return (pub != nullptr) ? pub->debug_name() : nullptr;
    }
};

struct ShmControlScheduler::Impl
{
    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<EntryId, std::shared_ptr<Entry>> entries;
    EntryId next_id{1};
    bool stopping{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::thread worker;

    /* ⚠️ stop() 的两状态必须与 mtx 分开（不能复用 mtx/cv）：
     *   · "请求停止"（stopping/stopped）：与 tick 的锁序无关，任何线程可置；
     *   · "谁执行 join"（stop_joining）：**恰好一个**线程取得 join 权；
     *   · "join 已完成"（stop_joined）：其它 stop() 调用者必须等到它为 true
     *     才能返回 —— 否则会出现"stop 已返回、worker 还活着"。
     * 用独立的 stop_cv 而不是 cv：cv 的等待者（unregister 等 tick_inflight、
     * loop_body 等到期）都由 worker 唤醒，而这里等的是一次**外部**事件（另一个
     * 线程 join 完毕），混用会让 notify_all 的语义变得难以推理。
     *
     * ⚠️ 回调内 stop（t_on_control_worker）**不得**走这条路径：它不取 join 权、
     * 也不等待（worker 就是调用者自己），只置停止标志后返回；join 留给析构或
     * 后续外部线程的 stop()。因此 join 权的取得必须晚于该分支。
     *
     * ⚠️ stop_joined 由 joiner 在 worker.join() 返回后**立刻**置位（不等清空
     * entries、不等 running 落 false 之后）—— 否则 join 自身抛异常（noexcept 下
     * 即 terminate）或清空阶段被卡住时，等在 stop_cv 上的其它 stop() 调用者会
     * 永久悬空。running 的落位在 stop_joined 之前，因此"非 joiner 返回 ⇒
     * worker_active() == false"仍成立。 */
    std::mutex stop_mtx;
    std::condition_variable stop_cv;
    bool stop_joining{false};
    bool stop_joined{false};

    /* 统计：worker 线程写、任意线程读，全部走原子（避免再引入一把锁，
     * 也避免调用方为了读数去抢 mtx）。 */
    std::atomic<std::uint64_t> ticks{0};
    std::atomic<std::uint64_t> overruns{0};
    std::atomic<std::uint64_t> deferred{0};
    std::atomic<std::uint64_t> exceptions{0};
    std::atomic<std::int64_t> last_ns{0};
    std::atomic<std::int64_t> max_ns{0};

    void start() { worker = std::thread([this] { loop(); }); }

    /* 距最近到期项的等待时长。没有任何注册项时 *any = false（调用方改为无限期等待）。
     * 已到期返回 0，让调用方立刻 tick。
     *
     * 返回 ControlClock::duration（纳秒）而**不是** milliseconds：毫秒是向零截断，
     * 10ms 项的醒来点会落在 next_due 之前零点几毫秒，于是本轮一个项都选不中、
     * 白跑一次空 tick，把 tick_count 撑大、让 §10 的"唤醒/切换下降"读数失真。
     *
     * 初值用可表示的最大值（不是 1000ms）：只要有一项在册，等待时长就完全由该项的
     * next_due 决定。否则 ControlTiming 被配成 >1s 的周期时，worker 会以 1s 为步长
     * 空转唤醒 —— 与"按项到期、睡眠合并"的契约相悖。 */
    ControlClock::duration wait_for_due_locked(ControlClock::time_point now, bool* any) const
    {
        ControlClock::duration best = ControlClock::duration::max();
        *any = false;
        for (const auto& kv : entries)
        {
            const Entry& e = *kv.second;
            if (e.inactive.load(std::memory_order_acquire))
            {
                continue;   // 交给 tick 里的惰性清理，不参与等待计算
            }
            *any = true;
            const auto remain = e.next_due - now;
            if (remain < best)
            {
                best = remain;
            }
        }
        if (*any && best < ControlClock::duration::zero())
        {
            best = ControlClock::duration::zero();
        }
        return best;
    }

    /* 惰性清理：inactive 且不在途的条目由本处摘除。正常注销（unregister）自己会
     * erase，这里兜的是"回调内注销"那条被拒的路径 —— 它只置 inactive 不等待，
     * 于是条目会短暂留在 map 里。
     *
     * ⚠️ 必须在 loop 顶部**也**调用：wait_for_due_locked 会跳过 inactive 项，
     * 若此时 map 里只剩 inactive 项，*any 就是 false ⇒ 走无限期 cv.wait，条目永远
     * 等不到下一次 tick 来清理（泄漏一个 inactive 条目 + 永久挂起）。由
     * test_shm_control_scheduler 的 CallbackInternalUnregisterDoesNotDeadlock 守门。 */
    void erase_inactive_locked()
    {
        for (auto it = entries.begin(); it != entries.end();)
        {
            const Entry& e = *it->second;
            if (e.inactive.load(std::memory_order_acquire) && !e.tick_inflight)
            {
                it = entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void loop()
    {
        t_on_control_worker = true;
        /* ⚠️ 整体兜底：非回调异常（tick 里 vector 分配 bad_alloc、entries.erase、
         * cv.notify_all）一旦逃出 loop，线程会静默终止而 running 仍为 true ——
         * worker_active() 谎报、后续 register_* 仍返回有效 id 但永不回调（静默失效）。
         * 因此无论何种退出路径，都必须把 running 落为 false 并唤醒等待者。 */
        try
        {
            loop_body();
        }
        catch (const std::exception& ex)
        {
            /* 走 warnf 而非先拼 std::string：本函数在 worker 的兜底 catch 内，
             * 拼接自身抛 bad_alloc 会逃出这里 ⇒ terminate（见 warnf 注释）。 */
            warnf("worker loop aborted by unexpected exception: %s", ex.what());
        }
        catch (...)
        {
            warnf("worker loop aborted by unexpected non-std exception");
        }
        running.store(false, std::memory_order_release);
        cv.notify_all();
    }

    void loop_body()
    {
        std::unique_lock<std::mutex> lock(mtx);
        while (!stopping)
        {
            erase_inactive_locked();
            bool any = false;
            const auto wait = wait_for_due_locked(ControlClock::now(), &any);
            if (!any)
            {
                /* 无注册项：无限期等待，直到注册或 stop 唤醒。
                 * ⛔ 不在这里 sleep(period) —— 那会把"无项"也变成固定唤醒。 */
                cv.wait(lock);
                continue;
            }
            cv.wait_for(lock, wait);
            if (stopping)
            {
                break;
            }
            if (wait > ControlClock::duration::zero())
            {
                /* 走到这里有两种来路：① wait_for 超时（必有项到期）；② 被 notify 提前
                 * 唤醒（注册/注销/wakeup，未必有项到期）。必须回到循环顶部**重新计算**
                 * 等待时长，否则会拿旧的 wait 反复 wait_for(0) 空转 tick。
                 *
                 * 判据用"重算后的 wait 是否为 0"而不是"是否还有 active 项"：
                 * wait_for_due_locked 在**有项到期**时返回 0，在"有项但都未到期"时返回
                 * 正数。后者若被判成可 tick，会平白多跑一轮空 tick 把 tick_count 撑大，
                 * 让 §10 的"唤醒/切换下降"验收读数失真。 */
                bool any_now = false;
                const auto wait_now = wait_for_due_locked(ControlClock::now(), &any_now);
                if (!any_now || wait_now > ControlClock::duration::zero())
                {
                    continue;
                }
            }
            lock.unlock();
            tick();
            lock.lock();
        }
    }

    /* 把"坏项"从调度里摘除：置 inactive、清 tick_inflight、从 map 删除并唤醒等待者。
     *
     * 为什么要隔离而不是让异常继续：需求 §3.1 的调度器是**进程级**的 ——
     * 一个坏项的回调抛出会带走该进程全部话题的控制面。宁可丢掉一个坏项的
     * 控制面动作（该订阅退化为收不到 generation 重建），也不能全进程一起死。
     *
     * noexcept + 变参：本函数在 catch 块内被调用，自身再抛（例如拼接诊断字符串的
     * bad_alloc）会逃出 dispatch 的 catch ⇒ terminate，反而破坏隔离。诊断走定长栈
     * buffer 的 vsnprintf（不分配），与 warnf 同一口径。 */
    void quarantine(const std::shared_ptr<Entry>& e, const char* fmt, ...) noexcept
    {
        char what[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(what, sizeof(what), fmt, ap);
        va_end(ap);

        const EntryId id = e->id;
        /* ⚠️ debug_name() 是 noexcept 虚函数，但"派生类违反 noexcept"在 C++17
         * 里是**未定义行为而非编译错误**；隔离路径不能假设实现者守约。 */
        const char* name = nullptr;
        try
        {
            name = e->name();
        }
        catch (...)
        {
            name = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            e->tick_inflight = false;
            e->inactive.store(true, std::memory_order_release);
            entries.erase(id);
        }
        cv.notify_all();
        exceptions.fetch_add(1, std::memory_order_relaxed);
        warnf("entry %llu (%s) %s; entry isolated (no further callbacks)",
              static_cast<unsigned long long>(id), (name != nullptr) ? name : "unnamed", what);
    }

    void dispatch(const std::shared_ptr<Entry>& e, ControlClock::time_point now)
    {
        /* ⚠️ 每次回调前复查 inactive。这一条是 I1 对**同轮**成立的必要条件：
         * 回调 A 可能在同轮里注销了已被选入 due 的 B（worker 线程内注销走的是
         * "只置 inactive、不等待"的兜底路径，不会把 B 从 due 里摘掉），此时
         * dispatch(B) 若照跑，B 会多收一次回调 —— 与头文件 :168 保证①
         * "不再发起该 id 的新回调"直接冲突。
         * 不加锁读 std::atomic 即可：unregister 是 release 写，此处 acquire 读，
         * 一旦读到 true 就保证该值的写发生在本次读之前。 */
        if (e->inactive.load(std::memory_order_acquire))
        {
            return;
        }
        try
        {
            if (e->kind == Entry::Kind::Subscriber)
            {
                if (e->sub != nullptr)
                {
                    e->sub->on_sub_heartbeat(now);
                }
                return;
            }
            if (e->pub == nullptr)
            {
                return;
            }
            /* owner heartbeat 无条件执行；stale 扫描仅在**已有 peer** 时做
             * （需求 §3.1：可在 peer_count()==0 时跳过 stale 扫描）。 */
            e->pub->on_pub_heartbeat(now);
            /* has_peers() 是**查询**，不是控制动作：它抛出时不能让 owner heartbeat
             * 一起被丢弃（那正是需求 §3.1 要求"无条件维持"的东西）。单独一层 try。 */
            bool has_peers = false;
            try
            {
                has_peers = e->pub->has_peers();
            }
            catch (...)
            {
                quarantine(e, "has_peers() threw");
                return;
            }
            if (has_peers)
            {
                e->pub->on_pub_stale_scan(now, e->timing.peer_dead_timeout);
            }
        }
        catch (const std::exception& ex)
        {
            /* 带上 what()：open() 失败这类异常的信息量全在 what() 里，
             * 丢掉它会让隔离诊断变成"某个项抛了异常"而无法定位原因。
             * 走变参而不是先拼 std::string —— 这里正在 catch 内（见 quarantine 注释）。 */
            quarantine(e, "callback threw: %s", ex.what());
        }
        catch (...)
        {
            quarantine(e, "callback threw a non-std exception");
        }
    }

    /* 结算本轮已置位的 tick_inflight，并在正常路径下推进 next_due。
     *
     * ⛔ 必须被 tick 的**每一条**退出路径调用（正常返回 + 异常退出）。unregister 的
     * 同步协议是"置 inactive → 等 tick_inflight 清零"，它**不会**自己清这个标志；
     * 任何把 tick_inflight 留在 true 的退出路径都会让对应的 unregister 永久阻塞在
     * cv.wait 上 —— 是**死锁**，不是静默失效。
     *
     * 现实中能触发的异常路径只有一处：锁内 due.push_back 的 bad_alloc（回调异常已被
     * dispatch 的 catch 隔离，到不了这里）。虽罕见，但既然后果是死锁，就用 catch(...)
     * 兜底，而不是靠"分配不会失败"的假设。 */
    void settle_inflight(const std::vector<std::shared_ptr<Entry>>& due, bool advance_next_due)
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto now = ControlClock::now();
        for (const auto& e : due)
        {
            /* ⚠️ tick_inflight **必须无条件清零**，不能因 inactive 而跳过。
             *
             * unregister 的同步协议是"置 inactive → 等 tick_inflight 清零"，它
             * 置位 inactive 时**不会**自己清这个标志 —— 它正是在等这里清。若此处
             * 因 inactive 而 continue（早期版本就是如此），unregister 会永久阻塞：
             * 这是 I2 的直接反例，由 test_shm_control_scheduler 的
             * UnregisterWaitsForInflightTick 守门。
             * quarantine 已把 tick_inflight 置 false 并摘除条目，这里再清一次无害。 */
            if (advance_next_due && !e->inactive.load(std::memory_order_acquire))
            {
                /* 不追赶风暴：正常推进一个周期；若已落后到"下一轮也已过期"，
                 * 则对齐到 now + period —— 一次长 tick 之后不能让该项被连续补发，
                 * 那会把"扫描抖动"放大成"心跳风暴"。 */
                const auto period = e->period();
                const auto next = e->next_due + period;
                e->next_due = (next <= now) ? (now + period) : next;
            }
            e->tick_inflight = false;
        }
        cv.notify_all();
    }

    void tick()
    {
        const auto t0 = ControlClock::now();
        std::vector<std::shared_ptr<Entry>> due;
        /* 初值同 wait_for_due_locked：用最大值，让"最小到期周期"完全由本轮选中的
         * 项决定（周期 >1s 时 1000ms 的初值会把超期口径判错）。 */
        auto min_period = std::chrono::milliseconds{(std::chrono::milliseconds::rep)(std::numeric_limits<std::chrono::milliseconds::rep>::max)()};
        try
        {
            {
                std::lock_guard<std::mutex> lock(mtx);
                /* 惰性清理：inactive 且不在途的条目由本处摘除。正常注销自己会 erase，
                 * 这里兜的是"回调内注销"那条被拒的路径（它只置 inactive，不等待）。 */
                for (auto it = entries.begin(); it != entries.end();)
                {
                    const Entry& e = *it->second;
                    if (e.inactive.load(std::memory_order_acquire) && !e.tick_inflight)
                    {
                        it = entries.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                /* 先收集本轮到期项，按 **EntryId 升序**（= 注册先后）**确定性排序**后再回调。
                 *
                 * ⚠️ 为什么必须排序，不能沿用 unordered_map 的迭代序：
                 *   "同轮里回调 A 注销了已被选入 due 的 B" 这条 I1 形态，只有在 B
                 *   **尚未** dispatch 时才可判定；若 B 恰好先跑，那次回调是**合法**的
                 *   （注销发生在回调之后，不能追溯取消），用例会假红/假绿。
                 *   libstdc++ 对连续 key 的桶遍历是**逆序**（实测 emplace(1),(2)
                 *   ⇒ 迭代序 2,1），于是"A 先于 B"这个前提在原实现下并不成立 ——
                 *   test_shm_control_scheduler 的 SameRoundUnregisterSkipsDispatch
                 *   正是因此失败。排序给出确定性契约：同轮内**先注册者先回调**，
                 *   与容器实现、与注册调用的时钟抖动都无关。
                 *
                 * ⚠️ 排序键为什么是纯 EntryId，而不是 (next_due, EntryId)：
                 *   曾用"最早到期优先"（EDF：先 next_due 再 EntryId）。它被否决，因为
                 *   同批内各项的 next_due 只差注册调用的微秒级时钟差，于是"谁先跑"
                 *   取决于 ControlClock::now() 的**分辨率** —— 一个隐式且不可控的前提，
                 *   正是 SameRoundUnregisterSkipsDispatch 反复假红/假绿的来源。
                 *   EntryId 是注册时分配的单调序号，纯 EntryId 排序让"注册顺序 =
                 *   回调顺序"成为**无前提**的确定性契约。
                 *   已知代价（接受）：同批混有不同周期的项时，EDF 让"更逾期者优先"，
                 *   纯 EntryId 改由注册先后决定。阶段 1 同批各项的 next_due 仅差微秒级，
                 *   公平性差异可忽略；而"确定性"是 I1 可判定性的前提，优先级更高。
                 *   契约由 test_shm_control_scheduler 的
                 *   SameRoundDispatchOrderFollowsRegistrationOrder 直接守门 —— 去掉本排序
                 *   后它的红灯直接指向"顺序退化"，而 SameRoundUnregisterSkipsDispatch
                 *   在同样变异下报的是"I1 复查失效"（方向误导，实测确认）。 */
                std::vector<std::shared_ptr<Entry>> selected;
                selected.reserve(entries.size());
                for (const auto& kv : entries)
                {
                    const Entry& e = *kv.second;
                    if (e.inactive.load(std::memory_order_acquire) || e.tick_inflight || t0 < e.next_due)
                    {
                        continue;
                    }
                    selected.push_back(kv.second);
                }
                std::sort(selected.begin(), selected.end(),
                          [](const std::shared_ptr<Entry>& x, const std::shared_ptr<Entry>& y)
                          {
                              return x->id < y->id;   // EntryId 单调递增 ⇒ 注册顺序即回调顺序
                          });
                for (auto& s : selected)
                {
                    /* ⚠️ 必须**先入 due 再置 tick_inflight**，且置位仍在**同一临界区**内：
                     *   · 同一临界区：unregister 也是在临界区内"置 inactive + 检查
                     *     tick_inflight"，两者共用 mtx 才互斥 —— 要么 unregister 先看到
                     *     in-flight 并等待，要么 tick 先看到 inactive 而不选中它。这是同步
                     *     注销协议（I2/I3）成立的机制；
                     *   · 先入 due：若反过来（先置标志、再 push_back 抛 bad_alloc），标志会
                     *     留在 true 而该项不在 due 里 ⇒ settle_inflight 清不到它 ⇒ 对应的
                     *     unregister 永久阻塞在 cv.wait（死锁，本函数 noexcept 下不可恢复）。 */
                    due.push_back(s);
                    s->tick_inflight = true;
                    min_period = std::min(min_period, s->period());
                }
            }

            /* 回调在**锁外**执行：既不让回调阻塞注册/注销，也避免"回调里再注册"自死锁。 */
            for (const auto& e : due)
            {
                dispatch(e, t0);
            }
        }
        catch (...)
        {
            /* ⛔ 异常路径：只结算在途标记，**不**推进 next_due —— due 里可能有
             * "已置 tick_inflight 但回调还没跑"的项，推进它们会静默丢掉这次心跳。
             * 结算后原样抛出，交给 loop() 的兜底 catch（worker 退出并置
             * running=false，worker_active() 不谎报）。 */
            settle_inflight(due, /*advance_next_due=*/false);
            throw;
        }
        settle_inflight(due, /*advance_next_due=*/true);

        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(ControlClock::now() - t0).count();
        last_ns.store(elapsed, std::memory_order_relaxed);
        if (elapsed > max_ns.load(std::memory_order_relaxed))
        {
            max_ns.store(elapsed, std::memory_order_relaxed);
        }
        ticks.fetch_add(1, std::memory_order_relaxed);
        /* 超期口径：本轮回调耗时超过该轮最小到期周期 ⇒ 同轮其它项的心跳被推迟。
         * 阶段 1 的已知来源是 on_sub_heartbeat 重建分支等 channel_mtx_
         * （最坏被 recv(50) 持约 50ms，见实现说明 §5.1）。 */
        if (!due.empty() && std::chrono::nanoseconds(elapsed) > min_period)
        {
            overruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    EntryId insert(Entry::Kind kind, std::shared_ptr<SubControlState> sub, std::shared_ptr<PubControlState> pub,
                   const ControlTiming& timing)
    {
        if (stopped.load(std::memory_order_acquire))
        {
            return ShmControlScheduler::kInvalidEntry;
        }
        auto e = std::make_shared<Entry>();
        e->kind = kind;
        e->sub = std::move(sub);
        e->pub = std::move(pub);
        e->timing = timing;
        /* 第一次回调在下一个到期点：注册本身不执行任何回调。 */
        e->next_due = ControlClock::now() + e->period();
        std::lock_guard<std::mutex> lock(mtx);
        if (stopping)
        {
            return ShmControlScheduler::kInvalidEntry;
        }
        const EntryId id = next_id++;
        e->id = id;
        entries.emplace(id, std::move(e));
        cv.notify_all();
        return id;
    }

    /* 同步注销的**唯一**实现，两个入口共用：
     *   · ShmControlScheduler::unregister(id) —— 公开 API；
     *   · RegistrationToken::reset() —— 令牌走 Impl 而不是裸调度器指针，因为
     *     调度器可能已经析构（见头文件 RegistrationToken 的注释）。
     * noexcept：任何异常都不允许逃出（互斥量/条件变量异常属进程级不可恢复错误）。 */
    void unregister_entry(EntryId id) noexcept
    {
        if (id == ShmControlScheduler::kInvalidEntry)
        {
            return;
        }
        /* ⛔ 回调内注销会自死锁：第 ③ 步要等"本次 tick 结算"，而那个 tick 正是当前
         * 这次回调。检测到就只置 inactive（下一轮 tick 惰性摘除），不等待。
         * 判据是**当前线程是否 worker**，与注销的是哪个项无关 —— 同轮里注销别的项
         * 同样会等到自己那次回调结束（由 dispatch 的 inactive 复查兜住 I1）。 */
        if (t_on_control_worker)
        {
            /* ⚠️ 除了置 inactive，还必须**立刻从 entries 摘除**（不能只靠下一轮 tick 的
             * 惰性清理）。理由：本分支不等待、立即返回，调用方（回调内注销）拿到的
             * "注销完成"语义必须包含"调度器已不再持有该项"——否则 entry_count() 会
             * 短暂包含已注销项，I3（注销后不再持有 state）在**同轮**路径上不成立。
             * 该项若已在 due 里、尚未 dispatch，由 dispatch 的 inactive 复查兜住 I1。
             *
             * ⚠️ 与正常路径同口径：摘除的 shared_ptr 移到锁外局部变量析构。若在锁内
             * erase 让最后一份引用归零，Entry/state 会在锁内析构 ⇒ 其析构体若回调进
             * unregister()/stop() 即对同一把非递归 mutex 二次加锁 ⇒ std::terminate
             * （本函数 noexcept）。 */
            std::shared_ptr<Entry> removed;
            {
                std::lock_guard<std::mutex> lock(mtx);
                const auto it = entries.find(id);
                if (it == entries.end())
                {
                    return;
                }
                it->second->inactive.store(true, std::memory_order_release);
                removed = std::move(it->second);
                entries.erase(it);
            }
            cv.notify_all();
            /* ⚠️ 诊断放在**锁外**：warnf 走 fprintf，可能阻塞在管道/终端上，
             * 不该占着 mtx（会让 tick 与其它注销一起等 I/O）。 */
            warnf("unregister(%llu) called from inside a callback; entry marked inactive instead of "
                  "waiting (unregister must not be called from a callback)",
                  static_cast<unsigned long long>(id));
            return;
        }

        /* ⚠️ `removed` 必须在 lock **之前**声明：局部变量按声明逆序析构，于是
         * lock（unique_lock）先析构 ⇒ 解锁，removed 后析构 ⇒ state 在**锁外**释放。
         * 若在锁内 entries.erase(id) 让最后一个引用归零，Entry 会在锁内析构 →
         * 释放 SubControlState/PubControlState → 若该 state 的析构体调用
         * unregister()/stop()（宿主析构体的常见写法），会在同一线程对同一把
         * **非递归** mutex 二次加锁 ⇒ std::terminate（本函数 noexcept）。
         * 中间那个 `e` 副本即使仍在锁内析构也无害：removed 仍持引用。 */
        std::shared_ptr<Entry> removed;
        {
            std::unique_lock<std::mutex> lock(mtx);
            auto it = entries.find(id);
            if (it == entries.end())
            {
                return;   // 未知 id / 重复注销：幂等
            }
            const std::shared_ptr<Entry> e = it->second;
            e->inactive.store(true, std::memory_order_release);   // ① 标记 inactive
            cv.notify_all();                                      // ② 唤醒调度器
            if (e->tick_inflight)                                 // ③ 等本次 tick 结算
            {
                deferred.fetch_add(1, std::memory_order_relaxed);
                cv.wait(lock, [&e] { return !e->tick_inflight; });
            }
            /* ⛔ 必须**重新查找**，不能复用上面的 `it`。
             * cv.wait 会释放 mtx，而在那段时间里 worker 的惰性清理
             * （erase_inactive_locked / tick 顶部同一段）看到本项已 inactive 且
             * tick_inflight 已清零，就会把它 erase 掉 —— 旧迭代器随即失效。
             * 直接 `std::move(it->second)` 是 **heap-use-after-free**：实测 ASan
             * 报 "READ of size 8 ... in shared_ptr move-assign"，在 -O3 下表现为
             * SIGSEGV（本函数 noexcept，于是直接崩在 reset() 里）。由
             * test_shm_control_scheduler 的 UnregisterWaitsForInflightTick 守门
             * （该用例的慢回调正是让注销落在 cv.wait 上的构造）。 */
            it = entries.find(id);
            if (it == entries.end())
            {
                /* 已被 worker 惰性摘除 ⇒ 注销已经完成（不再有回调、state 已由
                 * worker 释放）。这里直接返回即可，removed 为空是正确结果。 */
                return;
            }
            removed = std::move(it->second);   // ④ 摘除；此后不再有任何回调
            entries.erase(it);
        }
    }
};

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ShmControlScheduler::ShmControlScheduler() : impl_(std::make_unique<Impl>())
{
    impl_->start();
    impl_->running.store(true, std::memory_order_release);
}

ShmControlScheduler::~ShmControlScheduler()
{
    stop();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ShmControlScheduler& ShmControlScheduler::instance()
{
    /* Intentionally leaked pointer — never destroyed.
     *
     * 与 LocalPubSubRegistry::instance() 同一理由（见
     * src/dzIPC/common/local_pub_sub_registry.cc:14-24）：函数内静态对象的析构
     * 顺序相对全局/静态 shm_sub_ipc / shm_pub_ipc 的析构顺序未定义，而后者析构时
     * 必须调用 unregister()，调度器必须活得比它们久。 */
    static ShmControlScheduler* inst = new ShmControlScheduler();
    return *inst;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ShmControlScheduler::EntryId ShmControlScheduler::register_subscriber(std::shared_ptr<SubControlState> state,
                                                                     const ControlTiming& timing)
{
    if (state == nullptr)
    {
        return kInvalidEntry;
    }
    return impl_->insert(Entry::Kind::Subscriber, std::move(state), nullptr, timing);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ShmControlScheduler::EntryId ShmControlScheduler::register_publisher(std::shared_ptr<PubControlState> state,
                                                                    const ControlTiming& timing)
{
    if (state == nullptr)
    {
        return kInvalidEntry;
    }
    return impl_->insert(Entry::Kind::Publisher, nullptr, std::move(state), timing);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ShmControlScheduler::unregister(EntryId id) noexcept
{
    /* 实现走 Impl::unregister_entry：与 RegistrationToken::reset() 共用同一条路径，
     * 避免"公开 API 与令牌两套注销逻辑"这种会各自跑偏的分叉。 */
    impl_->unregister_entry(id);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ShmControlScheduler::wakeup() noexcept
{
    impl_->cv.notify_all();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void ShmControlScheduler::stop() noexcept
{
    /* ⚠️ 两个独立状态，不能用一次 exchange 表达两者：
     *   · "是否已请求停止"（stopped）——幂等，任何线程可置；
     *   · "谁执行 join"（stop_joining）——**恰好一个**线程取得 join 权。
     * 早期实现用一个 exchange 同时表达两者，于是回调内 stop() 会把 join 所有权
     * 一并吞掉：之后外部线程的 stop() 直接返回、不 join，join 只剩析构一条路
     * —— 析构若由同一个回调触发，就是 join 自己 ⇒ terminate。 */
    impl_->stopped.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();

    /* ⛔ 回调内 stop：worker 就是当前线程。join 自己会抛
     * std::system_error("Resource deadlock avoided")，而本函数 noexcept
     * ⇒ 直接 std::terminate（实测进程 SIGABRT）。这里只置停止标志后返回；
     * join 留给析构或后续**外部线程**的 stop() —— 它照常成为 joiner。 */
    if (t_on_control_worker)
    {
        warnf("stop() called from inside a callback; stop requested but join deferred "
              "(stop must not be called from a callback)");
        return;
    }

    /* 取得 join 权；没取得的调用者必须等 join **真正完成**再返回 ——
     * 否则调用方会看到"stop() 已返回但 worker_active() 仍为 true"。 */
    {
        std::unique_lock<std::mutex> lock(impl_->stop_mtx);
        if (impl_->stop_joined)
        {
            return;   // 已有人 join 完毕
        }
        if (impl_->stop_joining)
        {
            impl_->stop_cv.wait(lock, [this] { return impl_->stop_joined; });
            return;
        }
        impl_->stop_joining = true;
    }

    /* join 必须在 stop_mtx **之外**：join 期间若占着 stop_mtx，其它 stop() 调用者
     * 拿不到锁，也就无法在 join 完成后被 stop_cv 唤醒 —— 它们会永久等待。 */
    if (impl_->worker.joinable())
    {
        /* 在途 tick 在这里被收尾（tick 是同步完成的：出锁回调、回锁清 tick_inflight），
         * 因此 join 返回后不可能还有 tick_inflight == true 的条目。 */
        impl_->worker.join();
    }

    /* ⚠️ "清空 entries" 与 "析构被清掉的 state" 必须拆开：entries.clear() 会释放
     * 最后一份 shared_ptr<Entry> ⇒ 析构 Sub/PubControlState ⇒ 若其析构体调用
     * unregister()/stop()（宿主析构体的常见写法），在**锁内**析构即对同一把
     * 非递归 mutex 二次加锁 ⇒ std::terminate（本函数 noexcept）。
     * 因此锁内只做 swap（不析构），真正的析构发生在锁外的 removed 析构处。
     *
     * ⛔ 清空必须放在 join **之后**：若放在之前，一个正在 unregister 里等
     * "tick_inflight 清零"的线程可能等一个永不被清零的标志（worker 已退出），
     * 变成死锁。join 之后清空 ⇒ 调度器不再持有任何 state，也不再有等待者。 */
    std::unordered_map<EntryId, std::shared_ptr<Entry>> removed;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->entries.swap(removed);
    }
    /* running 落位与 stop_joined 同临界区：等待者被唤醒返回时，两件事都已生效
     * （worker 已停 + entries 已空），不存在"stop() 返回了但线程还活着"的窗口。 */
    {
        std::lock_guard<std::mutex> lock(impl_->stop_mtx);
        impl_->running.store(false, std::memory_order_release);
        impl_->stop_joined = true;
    }
    impl_->stop_cv.notify_all();
    /* removed 在此析构（锁外）。若其中某个 state 的析构体再调 stop()，
     * 它会看到 stop_joined == true 而立即返回 —— 不会重入死锁。 */
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
ShmControlScheduler::Stats ShmControlScheduler::stats() const
{
    Stats s;
    s.tick_count = impl_->ticks.load(std::memory_order_relaxed);
    s.tick_overrun_count = impl_->overruns.load(std::memory_order_relaxed);
    s.tick_deferred_count = impl_->deferred.load(std::memory_order_relaxed);
    s.callback_exception_count = impl_->exceptions.load(std::memory_order_relaxed);
    s.tick_duration_last_ns = impl_->last_ns.load(std::memory_order_relaxed);
    s.tick_duration_max_ns = impl_->max_ns.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(impl_->mtx);
    s.entry_count = impl_->entries.size();
    return s;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool ShmControlScheduler::worker_active() const noexcept
{
    return impl_->running.load(std::memory_order_acquire);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::size_t ShmControlScheduler::entry_count() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->entries.size();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* RegistrationToken 的两个外置定义。必须在 .cc，不能内联回头文件：
 *   · reset() 要调用 impl_->unregister_entry(...)，而 Impl 在头文件里只有前向声明；
 *     若内联在头里，实例化那一刻 Impl 仍不完整 ⇒ "invalid use of incomplete type"。
 *   · operator= 会先 reset() 旧条目，同样需要完整类型。
 * 移动构造**不**需要外置：它只移动 shared_ptr，不触碰 Impl 的成员。 */

void RegistrationToken::reset() noexcept
{
    /* ⚠️ 走 Impl 而不是 `sched_->unregister(id)`：调度器可能已经析构，那个裸指针
     * 此时是悬垂的（这正是本类改为共享 Impl 要消除的缺陷）。impl_ 是共享所有权，
     * 到这里一定还活着；调度器已析构时 Impl 已 stop（worker join、entries 已空），
     * 于是 unregister_entry 是幂等无操作。
     * ⛔ 先置空 id_ 再注销：unregister_entry 可能触发 state 析构，而 state 的析构体
     * 若回调进本对象（宿主析构路径）必须看到"已失效"状态，不能二次注销。 */
    const auto id = id_;
    id_ = ShmControlScheduler::kInvalidEntry;
    sched_ = nullptr;
    if (impl_ != nullptr && id != ShmControlScheduler::kInvalidEntry)
    {
        impl_->unregister_entry(id);
    }
    /* ⚠️ impl_ 的释放必须在 unregister_entry 返回**之后**：若提前释放，而调度器
     * 早已析构，这里就是最后一份引用 ⇒ Impl 析构，接着 unregister_entry 打在
     * 已析构对象上（UAF）。放在最后则"注销 → 再释放所有权"顺序明确。 */
    impl_.reset();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
RegistrationToken& RegistrationToken::operator=(RegistrationToken&& other) noexcept
{
    if (this != &other)
    {
        /* 先注销自己当前的条目（自赋值已排除）。这一步会释放本对象持有的 Impl 引用，
         * 因此必须在移动 other 的成员**之前**完成。 */
        reset();
        sched_ = other.sched_;
        impl_ = std::move(other.impl_);
        id_ = other.id_;
        other.sched_ = nullptr;
        other.id_ = ShmControlScheduler::kInvalidEntry;
    }
    return *this;
}

}   // namespace shm_control
}   // namespace dzIPC
