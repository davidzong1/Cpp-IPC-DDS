#pragma once
/* 阶段 5 · SHM 固定 route 归属的收包 worker（RecvWorker / RecvWorkerPool）。
 *
 * 接口契约（冻结）：`ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md`
 * §4。本文件是该契约的实现面，签名与语义逐条对齐。
 * ⚠️ 同时读勘误 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md`
 * （E2 精确化了 add_route 的 duplicate/busy 判定顺序）。设计依据见
 * docs/消息接收架构改造/{事件驱动线程池需求.md §3.2/§5/§6,
 * shm_sub_thread_consolidation_plan.md 阶段 5}。
 *
 * 消费方（各自拥有自己的模块文件，不拥有本层）：shm_ser_cli / socket_pub_sub /
 * socket_ser_cli 三份移植方案。本层是共享层唯一 owner 的产物，**不要**在模块里
 * 复制一份 worker。
 *
 * ============================ 线程模型 ============================
 *   RecvWorkerPool（每进程 1 个单例；start() **一次性**，随进程存活）
 *     └─ RecvWorker[0..N-1]   N 默认 CPU 数（上限 128）
 *          ├─ 自有 ipc::recv_wait_set（归 worker 所有，**不随线程消亡**）
 *          ├─ 固定 route 表（route → entry，entry 持 shared_ptr<RecvRouteSource>）
 *          ├─ level-triggered 全扫 + 每 route 预算 + deferred FIFO
 *          ├─ 至多 1 条工作线程（空闲退出后可被 add_route 按需拉起，见下节）
 *          └─ Stats
 *
 * worker 循环（契约 §4.7 + idle keep-alive；⛔ W06-F1 修订了 else 分支的顺序）：
 *   while (!stopping):
 *       collect_pending()                      // level-triggered 全扫（事实来源）
 *       if (route 表与 deferred 都为空):
 *           连续空闲 >= budget.idle_keep_alive -> 结束本线程（idle exit）
 *           wait(min(wait_timeout, 剩余空闲窗口))   // 阻塞等待；超时不是错误
 *       else:
 *           drain_deferred()                   // ← 先干，再决定要不要让出（W06-F1）
 *           本轮一条数据都没取到 -> wait(wait_timeout)
 *       （drain_deferred 内含 collect_pending 之后的就绪项，固定 FIFO）
 *
 * ⛔ **两条分支的顺序是承重的，别改回去**（W06-F1，2026-09-29）：
 * 原实现是「`else` 分支先 `wait(wait_timeout)` 再 `drain_deferred()`」，等于上一步
 * `collect_pending()` 刚把就绪 route 排进队列，却先阻塞一个等待切片再服务它。实测后果：
 *   ① 交付延迟的地板恒等于 `wait_timeout`：反事实 mean **90.1 ms** / max **100.2 ms**，
 *      修后 mean **0.16 ms** / max **0.18 ms**（同一二进制、同一域，仅改此处顺序；见
 *      `artifacts/perf/20260929-r24-W06/logs/f1_counterfactual.log`）；
 *   ② 它还会把"延迟"升级成"延迟 + 丢一次发现"：`run_budget` 结尾 `last_seq.store(当前
 *      seq)` 读的是**返回那一刻**的值，落在窗口里的 seq 变化被吞掉 ⇒ 该 route 直到
 *      **下一次** seq 变化才被重新选中（generation 重建只有一次 seq 变化），表现为
 *      偶发慢/偶发收不到。
 * 修法如上：等待只出现在「确实没有可处理项」时。**但"直接 drain"不能无条件**——
 * 宿主 `has_pending()` 恒真的病态实现会让该 route 每轮自我 requeue（每轮 4 次空读）
 * ⇒ 无上限自旋（实测 947 次/s，而 `test_lifecycle_contract` 的 L4 判据是 ≤ 200 次/s）。
 * 故 drain 之后若**一条都没取到**（`messages_received` 未增），仍须阻塞让出
 * `wait_timeout`：速率上界还给等待，而不是忙转。等待不会吞唤醒 —— `wait_once()`
 * 返回后重新 `collect_pending()` 全扫，level-triggered 判据才是事实来源。
 *
 * ===================== route ↔ worker 固定归属 =====================
 *   worker_id = FNV1a64(route_name ‖ domain_id) % worker_count
 * route 在**整个生命周期内**只由该 worker 调用 recv_once()；注销后重新注册才
 * 允许重新分配。worker **不**做 work-stealing。
 * 依据：`libipc::conn_info_head::recv_cache()` 是 thread_local 分片缓存，同一
 * route 迁移到别的 worker 会让多片消息的前后片段进不同缓存而重组失败
 * （需求 §2.1）。socket 侧同理：data_rev.cc 的每 UDPNode 重组状态也假设单消费者。
 *
 * ============================== 每 route 预算 ==============================
 * max_messages_per_route / max_bytes_per_route / max_processing_time_per_route
 * **只在安全边界让出**：每完成一次完整 recv_once()（= 一条完整消息 / 一个完整
 * 请求）之后才检查三项上限。⛔ 不得在组包中途切走（socket 侧 chunk_rev_* 一次
 * 调用可能持续到整条消息组装完成）。
 * 预算耗尽**不是丢弃**：该 route 进 worker 的 deferred FIFO，本轮 wait 前后各
 * drain 一次；固定 FIFO 顺序保证热 route 不会长期饿死冷 route。
 *
 * ==================== 空闲退出与按需拉起（keep-alive）====================
 * 为什么需要：池是**进程级单例且 start() 一次性**（随进程存活），但"工作线程
 * 常驻"会把 CPU 核数那么多条线程永久留在进程里 —— 话题全部注销之后线程数
 * 不回落，既有回归 test/test_sercli_auto_path.cpp 的"析构后线程数回落"断言
 * 因此失败（before=1 / after=1+CPU 数）。两者的分工是：
 *   · start() 一次性管的是**活动期**（worker 对象与 wait-set 随进程存活、
 *     stop 之后不可重启）；
 *   · 本机制管的是活动期内**线程的存活**（可以起、停、再起）。
 * 二者不矛盾：活动期单调不减，线程则在空闲时归还操作系统。
 *
 * 规则：
 *   1. worker 线程在"自身 route 表为空 且 连续空闲 >= **有效窗口**"时结束自己
 *      （idle exit）；计时在"表第一次为空"时开始，期间任何注册活动都会重置它。
 *      有效窗口 = max(budget.idle_keep_alive, budget.wait_timeout)，下界理由：
 *      一次 wait 切片最长 wait_timeout，窗口若取得比它还小，"等待切片恰好吃满
 *      窗口"就会变成一个不变量依赖（负载高的机器上退出可能被推后到断言窗口
 *      之外）。例外：idle_keep_alive == 0 保持"下一轮空闲检查即退出"的字面
 *      语义（仅供测试把退出/重拉起交错压到最紧）。
 *   2. 之后任何 add_route 命中该 worker ⇒ 插入 route 表（mtx 内）后在 mtx 之外
 *      按需拉起线程。两个动作**不在**同一个临界区，但仍**不可能**出现“add_route
 *      返回 ok 却无人消费”（静默丢包）：线程“清 alive”与“判定 route 表为空”
 *      本来就在同一个 mtx 临界区内（实现 loop 的 idle-exit 分支），而 add_route
 *      已在锁内插好 route ⇒ 任一侧先拿锁都能得出“会有人消费”的结论：
 *        · add_route 先 ⇒ 线程随后在锁内看到表非空，不会清 alive，继续消费；
 *        · 线程先 ⇒ 它在表空的那一刻清了 alive，此后不再消费，add_route 在锁内
 *          读到 false 并拉起新线程。
 *      之所以要拆到锁外：ensure_thread_alive 要取 join_mtx（还可能要 join 旧代
 *      线程），而 stop 持 join_mtx 等线程退出、线程退出可能要取 mtx ⇒ 在 mtx 内
 *      拉起构成反向获取（三方交叠即死锁）。
 *   3. 多个 add_route 并发抢拉起**只成功一次**：thread_alive 的读/置位在
 *      同一 mtx 内，只有一个调用者看到 false。
 *   4. route 表非空期间线程**不会**退出 ⇒ 同一注册期内的 recv_once 始终在
 *      同一条线程上（thread_local 分片缓存的前提，见"固定归属"一节）；
 *      "注销后重新注册才允许重新分配"这条语义**没有被放松**。
 *   5. 退出与重拉起不动任何既有状态：wait-set 由 worker（而非线程）持有，
 *      route 表 / deferred / last_seq / in_flight 全部保留；重拉起的线程第一轮
 *      collect_pending 就能重新发现全部在册 route（以及 add_route 末尾那次
 *      remove+add 敲出的唤醒）。
 *   6. 空闲等待是 wait-set 的**阻塞**等待（超时 = min(wait_timeout, 剩余空闲
 *      窗口)，下界 1 ms 防止 wait(0) 退化），⛔ 不引入忙轮询。
 *   7. "拉起"可能失败（线程创建失败）或被**并发的 stop()** 挡下（活动期已结束）。
 *      此刻 route 表已经插入 ⇒ 必须**回滚**这条注册并显式返回 stopped：
 *      ⛔ 禁止"返回 ok 却无人消费"。回滚只摘表 / 摘等待集合 / 归还收包独占，
 *      不需要 stop_and_wake（线程从未见过这条 entry）。
 *   8. 注册成功时**无条件**让新 route 至少被处理一次（进 deferred FIFO），不依赖
 *      seq 变化：token 建立之前已经在队列里的消息（注册前到达 / 上一轮空闲退出
 *      窗口内到达）不会产生新 seq，只靠 collect_pending 的 seq 判据会一直看不到
 *      它。代价是每条 route 注册时多一次非阻塞 recv_once（读到 0 即让出）。
 *   9. 并发抢拉起的唯一性由 alive 的**双检**保证：外层读 + join_mtx 内再读，
 *      只有一个调用者能建线程；thread_restarts 因此恰好 +1。
 *  10. 线程收尾时的 alive 清零带**代际检查**（每条线程一个递增编号）。旧代线程
 *      离开 loop 之后、收尾清零之前，新代线程可能已被拉起并把 alive 置回 true；
 *      无条件清零会盖掉那次置位（alive==false 却有活线程 ⇒ 下一次 add_route
 *      再拉起一条 ⇒ 同一 worker 两条消费者）。只有当前代能清零。
 *
 * 窗口取值：RecvBudget::idle_keep_alive，默认 **1000 ms**，可配置；0 表示下一轮
 * 空闲检查即退出（仅供测试）。取值依据是既有断言窗口 4000 ms
 * （test_sercli_auto_path.cpp 的"route 全空后线程数回落"）：线程退出最坏耗时 =
 * idle_keep_alive + 一次 wait 切片（<= budget.wait_timeout，默认 100 ms）+ 调度，
 * 约 1.1 s，留 ~2.9 s 余量；同时不至于短到让"话题抖动"（反复 subscribe /
 * unsubscribe）把线程起停变成常态。
 *
 * 与 fork 安全闸的关系（**不放松**）：fork 之后子进程继承"池已 start"的状态，
 * 但它没有工作线程。子进程里 add_route 现在会走"按需拉起"路径（要取
 * lifecycle_mtx_ / mtx 并创建线程），而被 fork 打断的父进程可能正持有这些锁 ⇒
 * 子进程里有**死锁**风险，比旧行为的"返回 ok 后静默丢包"更严重。因此模块侧的
 * "owner pid 闸"（shm_ser_cli_ipc.cc 的 recv_pool_owner_pid）从"防丢包"升级为
 * **必须保留**的防死锁闸：owner pid != 当前 pid ⇒ 不得调用池的 add_route，
 * 继续用兼容收包线程。本层不做 pid 探测（不引入平台宏），只把约束写在这里。
 *
 * 进程退出路径：空闲退出的线程已从 loop 返回，join 只是收尸；stop() 与析构都会
 * 在 lifecycle_mtx 下 join（见实现），因此"空闲退出后没人 join 的 std::thread"
 * 不会走到 std::terminate。
 *
 * ======================= 生命周期与注销协议 =======================
 * add_route() 判定顺序（决定返回哪个 status）：
 *   route == nullptr                -> invalid_route
 *   !started（未 start / 已 stop）    -> stopped
 *   read_wait_token() 无效           -> invalid_token
 *   try_claim_recv(worker) 失败且已注册 -> duplicate  // 本 worker 已注册同一 route
 *   try_claim_recv(worker) 失败且未注册 -> busy       // 兼容 subscribe_thread_ 正在 recv
 *   wait_set.add 失败 && 从未成功    -> backend_unavailable   // 显式回退信号
 *   wait_set.add 失败 && 曾成功      -> wait_set_full
 * 后两条靠"是否曾经成功过"间接区分，前提（脆弱点，模块作者须知）：
 *   · 首次 add_route 时集合必为空，因此"首次 add 失败"只可能是后端不可用；
 *   · backend_supported() 是进程内静态缓存，一旦 true 永为 true，不会中途翻转。
 * 这两条前提由 ipc::recv_wait_set 保证（recv_wait_set.cpp 的 backend_supported
 * 静态缓存 + add 的两处 false 出口）。若将来 recv_wait_set::add 能直接区分原因，
 * 本层应改为读它的返回值，而不是继续推断。
 *
 * remove_route() **同步**完成（返回即安全释放 route），严格按契约 §4.4：
 *   1. 从 worker 的 route 表摘除 + 标记 removed   // 禁止新的 recv_once
 *   2. wait_set.remove(token)                    // 唤醒阻塞中的 worker wait
 *   3. route->stop_and_wake()                    // 禁止新 lease + 唤醒 route 内部 recv
 *   4. 等 worker 侧 in-flight recv_once 归零       // 有界：最多一次 recv_once
 *   5. route->wait_quiescent()                   // 等 lease 归零
 *   6. route->release_recv()                     // 归还收包独占（owner 回到 none）
 * 第 4 步在 route 表锁**之外**等待；worker 减 in-flight 计数不需要该锁
 * （entry 由本地 shared_ptr 保活），因此不存在"注销等 worker、worker 等表锁"的
 * 循环。第 1、4 步已保证此后 worker 不可能再进 recv_once。
 *
 * ==================== 单 route 单消费者（与兼容线程互斥）====================
 * 一条 route 同一时刻只允许一个消费者：要么兼容后端的 subscribe_thread_ 在
 * recv() 里等，要么进某个 RecvWorker。**禁止两路同时 recv**（需求 §5 末段）。
 * 落地方式就是 RecvRouteSource 的 try_claim_recv / release_recv / recv_owner
 * 三方法构成的显式独占状态机（宿主用 std::atomic<RecvOwner> 实现，约三行）：
 *   · 兼容线程启动前 try_claim_recv(compat_thread)；退出后 release_recv()；
 *   · add_route() 只在 try_claim_recv(worker) 成功时接管，否则返回 busy；
 *   · remove_route() 第 6 步把 owner 归还 none，之后兼容线程才允许重新接管。
 *
 * ================== 能力探测与显式回退（禁止忙轮询）==================
 * 平台探测**只做一次**并缓存（Linux: futex_waitv；由 ipc::recv_wait_set 内部完成，
 * 本层不重复探测代码），本层通过 wait_set.add() 的返回值观察结果并缓存到
 * backend_available()。backend_unavailable 是**显式回退信号**：模块作者必须保留
 * 原有兼容线程路径并打日志。
 * ⛔ 不得改成 try_recv() / receive_nowait() 全量忙轮询。
 * ⛔ 探测结果进程内缓存，不得在运行中来回切换后端（一次失败即永久回退，避免
 *    "半进程用池、半进程用线程"的不可推理状态）。
 *
 * =========================== 平台宏边界 ===========================
 * 本头文件**不得**出现任何平台宏；句柄/平台差异全部在 libipc（
 * include/libipc/recv_wait_set.h）与 socket_wait_set.h 后面。worker 只知道
 * `ipc::recv_wait_token`。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "libipc/export.h"
#include "libipc/recv_wait_set.h"

namespace dzIPC {
namespace threepools {

/* 一条 route 当前的收包独占者。宿主用 std::atomic<RecvOwner> 保存。 */
enum class RecvOwner
{
    none,           ///< 无人收包（可被任何一方 CAS 接管）
    compat_thread,  ///< 兼容后端的每 route 收包线程正在 recv
    worker          ///< 固定归属的 RecvWorker 正在 recv
};

/* 宿主（模块）实现的 route 抽象。worker 只通过本接口接触 route，**不持有**模块
 * 的裸对象指针 —— 这是"注销后不再回调已析构对象"的前提。
 *
 * 线程模型：read_wait_token / recv_owner / route_name / domain_id 可从任意线程
 * 调用；recv_once **只允许 owner worker 调用**；stop_and_wake / wait_quiescent /
 * release_recv 由注销方调用。 */
class IPC_EXPORT RecvRouteSource
{
public:
    virtual ~RecvRouteSource() = default;

    /* 固定归属的稳定 key。**生命周期内不得变化** —— 变了就等于换了 route，
     * 必须走 remove_route + add_route。返回的字符串由宿主保活。 */
    virtual const char* route_name() const noexcept = 0;
    virtual std::uint32_t domain_id() const noexcept = 0;

    /* libipc 读等待 token（`ipc::route::read_wait_token()`）。无效 ⇒ 不能进
     * wait-set（返回 invalid_token），模块作者应回退兼容线程。 */
    virtual ipc::recv_wait_token read_wait_token() const noexcept = 0;

    /* 取一次。返回 0 = 无数据 / 断开。**必须非阻塞或短超时**（try_recv 或
     * recv(0)），且内部完成 RouteSession lease 配对（acquire_receive /
     * release_receive 成对，含所有异常出口）。只允许 owner worker 调用。
     *
     * ⛔ 不得在这里做用户回调或长耗时处理（需求 §1.2：get/get_clone/用户回调
     * 不得搬进收包 worker）。socket 侧的等价物是"调用一次 chunk_rev_* 直到完整
     * 消息/请求组装完成"—— 一次调用可以跨多个分片，但必须在完整边界返回。 */
    virtual std::size_t recv_once() = 0;

    /* level-triggered 重检：recv_once() == 0 之后仍有可收数据时为 true。
     * 默认 false（绝大多数实现一次 recv_once 就把队列读空）。 */
    virtual bool has_pending() const noexcept { return false; }

    /* 收包独占状态机（宿主用 std::atomic<RecvOwner> 实现，约三行）。 */
    virtual RecvOwner recv_owner() const noexcept = 0;
    virtual bool try_claim_recv(RecvOwner who) noexcept = 0;   ///< CAS(none → who)
    virtual void release_recv() noexcept = 0;                   ///< CAS(worker → none)

    /* route 生命周期协议（阶段 2 RouteSession）：拒绝新 lease + 唤醒阻塞中的 recv。
     * 幂等，可在已有 route 为空时调用。 */
    virtual void stop_and_wake() noexcept = 0;
    /* 等 in-flight lease 归零。stop_and_wake 之后调用。 */
    virtual void wait_quiescent() noexcept = 0;
};

/* 每 route 预算。三项都只在"一次完整 recv_once 返回"这个安全边界检查。 */
struct RecvBudget
{
    std::size_t max_messages_per_route{32};
    std::size_t max_bytes_per_route{1u << 20};                     ///< 1 MiB
    std::chrono::microseconds max_processing_time_per_route{200};  ///< 200 us
    std::chrono::milliseconds wait_timeout{100};                   ///< 空闲 wait 上限
    /* 追加字段（冻结规则：新增一律追加，不改既有签名与语义）：idle keep-alive
     * 窗口。worker 的 route 表连续为空 >= 本窗口 ⇒ 工作线程空闲退出；之后任何
     * add_route 按需重新拉起。0 表示下一轮空闲检查即退出（仅供测试）。语义与
     * 取值依据见文件头"空闲退出与按需拉起"一节。 */
    std::chrono::milliseconds idle_keep_alive{1000};
};

struct RecvWorkerStats
{
    std::uint64_t wait_wakeups{0};    ///< wait 返回 true（有就绪或被打断）
    std::uint64_t wait_timeouts{0};   ///< wait 超时（正常空闲，不是错误）
    std::uint64_t wait_errors{0};     ///< 判定为后端异常的次数
    std::uint64_t routes_processed{0};///< 至少取到一条数据的预算轮数
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t budget_yields{0};   ///< 因预算耗尽让出的次数
    std::uint64_t deferred_drains{0}; ///< 进入 deferred FIFO 的次数
    std::size_t route_count{0};       ///< 当前在册 route 数
    /* 追加字段（冻结规则：新增一律追加，不改既有签名与语义）：宿主实现的
     * recv_once() 抛异常的次数。worker 捕获后记一次并让出，绝不让异常逃出
     * worker 线程（否则整个进程 terminate）。 */
    std::uint64_t recv_errors{0};
    /* 追加字段：空闲退出（idle exit）次数与线程按需拉起次数。诊断/验收用 ——
     * idle_exits 增长说明"route 表空 ⇒ 线程归还"生效；thread_restarts 增长说明
     * 之后被 add_route 重新拉起。两者都只增不减。 */
    std::uint64_t idle_exits{0};
    std::uint64_t thread_restarts{0};
    /* 追加字段（W04-F3 修复，2026-09-28）：socket 侧"假就绪退避"的两个计数。
     *   fruitless_backoffs —— 进入退避的次数（连续 kMaxFruitlessReadiness 轮报就绪却
     *     读不到数据 ⇒ 该通道被判定为病理性假就绪）；
     *   rearm_events       —— 退避到期后重新挂回等待集合的次数。
     * SHM 侧恒为 0（那里的事实判据是共享内存 seq，不存在"内核报了但读不到"这一形态）。
     * 两者只增不减；它们同时是"忙转已被消除"的正面证据：修复前该形态实测 1.31M 次/s
     * recv_once（W04-F3），修复后循环速率由退避上限约束。 */
    std::uint64_t fruitless_backoffs{0};
    std::uint64_t rearm_events{0};
    /* 追加字段（W06 接入，2026-09-29；冻结规则：新增一律追加，不改既有签名与语义）：
     * 单次 `recv_once()` 的耗时与预算超额（方案 §10.3）。
     *
     * 为什么要单独记：时间预算**不能抢占**一次耗时收包 —— 200 µs 预算并不保证 worker
     * 在 200 µs 内让出（一次分片组装/解码可能远超预算）。所以"预算是否生效"不能只看
     * 让出次数，必须把**单次调用耗时**与**超额次数**分开记录，才能判断队头阻塞。
     *   · recv_once_calls      —— 调用次数（分母）；
     *   · recv_once_max_ns     —— 单次调用耗时最大值（ns，单调钟；含宿主侧分流/入队）；
     *   · recv_once_over_budget—— 单次调用耗时 > budget.max_processing_time_per_route 的次数。
     * 计时在**完整 recv_once() 返回后**取值，与预算检查同一个安全边界（§10.3）。
     * 三者只增不减。**本结构体是 recv_once 耗时/超预算的常开事实来源**（不受
     * diagnostics 门控）：`dzIPC::measure::CounterId::{recv_once_calls,
     * recv_once_over_budget}` 是同一口径的**诊断门控**副本，写入点在 `run_budget()`
     * 同一计数点（t30/D-23 接线；本层只引用既有 ID，不新增、不改语义）。 */
    std::uint64_t recv_once_calls{0};
    std::uint64_t recv_once_max_ns{0};
    std::uint64_t recv_once_over_budget{0};
    /* 追加字段（W06/t30，2026-09-29；方案 §10.2 的"低开销常驻计数"半边）：
     * `collect_pending()` 的**常驻**扫描量 —— 无时钟、无诊断门控。
     *
     * §10.2 要求"每轮扫描 route 数、扫描耗时、等待超时次数、有效就绪比例、deferred
     * 深度"五项，并要求"低开销常驻计数与详细诊断采样分开"。分工如下：
     *   · 常驻（本结构体）：轮数 / 遍历 route 数 / 就绪轮数 / deferred 深度两件套 +
     *     等待超时次数（`wait_timeouts`，既有字段）；
     *   · 诊断门控（`dzIPC::measure::CounterId` 的 scan_* 族）：额外提供**扫描耗时**
     *     与同一批量的门控副本（唯一需要两次 `clock_gettime` 的量，故必须门控：
     *     W03 实测关 0.176 ns/轮、开 29.3 ns/轮）。
     * 两者在 `collect_pending()` 同一位置、同一取值写入 ⇒ 诊断开启时逐值相等。
     * 读法：均值 = scanned_routes_total / scan_rounds；有效就绪比例 =
     * scan_ready_rounds / scan_rounds。⛔ 这些量**只观测**，不参与任何就绪判定。 */
};

enum class RecvRegisterStatus
{
    ok,
    backend_unavailable,   ///< 显式回退信号：模块必须保留兼容收包线程
    duplicate,             ///< 同一 route 已在本 worker 注册
    busy,                  ///< 兼容 subscribe_thread_ / 其它 owner 正在 recv
    stopped,               ///< worker 未 start 或已 stop
    invalid_token,         ///< read_wait_token() 无效
    invalid_route,         ///< route == nullptr
    /* 追加值（契约 §4.4 判定表提到但枚举未列出，按"新增一律追加"补齐）：
     * 曾经成功注册过、本次 add 失败 ⇒ 是容量满而不是后端不可用。 */
    wait_set_full
};

/* 一条固定 route 归属的收包线程。生命周期：start() → add_route/remove_route → stop()。
 * start() **一次性**（重复调用返回 false，stop 之后不可重启）；其内部工作线程
 * 则会在空闲时归还（见文件头"空闲退出与按需拉起"），由下一次 add_route 拉起。 */
class IPC_EXPORT RecvWorker
{
public:
    /* worker_id 仅用于诊断与归属计算（worker_for 的结果）。 */
    RecvWorker(std::size_t worker_id, RecvBudget budget = RecvBudget{});
    ~RecvWorker();   ///< 等价 stop()
    RecvWorker(const RecvWorker&) = delete;
    RecvWorker& operator=(const RecvWorker&) = delete;

    bool start();              ///< 启动 1 条 worker 线程；重复调用返回 false
    void stop() noexcept;      ///< 唤醒 + join，幂等；可从任意线程调用
    /* **活动期**语义（不是"线程恰好活着"）：start() 之后、stop() 之前恒为 true；
     * 工作线程空闲退出期间仍为 true（下一次 add_route 会把它拉回来）。要判断
     * "现在是否真的有线程"用 thread_alive()。 */
    bool running() const noexcept;
    /* 当前是否有工作线程：空闲退出后为 false，add_route 成功拉起后为 true。
     * 诊断/验收用，不改变任何语义。 */
    bool thread_alive() const noexcept;
    std::size_t worker_id() const noexcept;

    /* 见文件头判定顺序。成功后该 route 归本 worker 独占 recv。 */
    RecvRegisterStatus add_route(const std::shared_ptr<RecvRouteSource>& route);

    /* 同步注销：返回即"worker 不会再碰这条 route"，宿主可以安全 release/析构。
     * 幂等；未知 route / nullptr 都是无操作。
     *
     * ⛔ **不得**从 worker 线程调用（W04-F1 队长裁决 D-11：改注释 + 升级为可机械核对的判据）。
     *    原因不是"实现有兜底"，而是**它根本没有**：本函数按契约 §4.4 第 4/5 步等待在途
     *    `recv_once()` 与 lease 归零；从 worker 线程调用时，那个在途调用就是**调用者自己**
     *    ⇒ 必然等满 `kQuiesceTimeout`（2000 ms）后由超时分支打诊断返回，随后第 5/6 步
     *    （`wait_quiescent` / `release_recv`）作用在一条自己还持着的 route 上。
     *    ⇒ 该用法**未被支持**，只是"不会永久死锁"而已（边界由 2000 ms 超时给出）。
     *
     * 机械核对判据（可 grep，见 docs/消息接收架构改造/团队改造交付/接口与生命周期.md §9 R-02）：
     *   `grep -c "this_thread::get_id" src/dzIPC/threepools/recv_worker.cc` 期望 **3** —— `stop()`
     *   与析构各 1 处（"别 join 自己"），另 1 处是 `remove_route()` 入口的 debug `assert`；
     *   **没有**任何同线程旁路（这正是 D-11 收敛后的形态）。 */
    void remove_route(const RecvRouteSource* route) noexcept;

    /* 唤醒阻塞中的 wait 立即重评估（不改路由表）。用于"外部条件变了，希望马上
     * 重新检查"。实现走 wait-set 的 remove+add（remove 会敲唤醒通道），
     * 因此**不会**丢失就绪提示：worker 自己维护 last_seq 做 level-triggered
     * 全扫，wait-set 内部的 last 被重置不影响判定。 */
    void wakeup() noexcept;

    RecvWorkerStats stats() const;
    std::size_t route_count() const noexcept;

    /* 追加（W06）：本 worker 生效的预算。**只读回显**，不改变任何语义；供模块侧
     * 把"实际生效的预算"写进运行证据（方案 §10.4：配置来源与重复 start 参数不一致
     * 的处理方式必须可核对，不能只靠"我记得传了默认值"）。 */
    const RecvBudget& budget() const noexcept;

    /* 进程内缓存的能力探测结果。**首次 add_route 后才有确定值**；未探测时返回
     * true（乐观），首次 add 失败且从未成功过时变为 false 并永久保持。
     * false ⇒ 模块作者必须保留兼容收包线程，禁止忙轮询。 */
    static bool backend_available() noexcept;
    static const char* backend_name() noexcept;

    struct Impl;

private:
    Impl* impl_{nullptr};
};

/* 进程级 worker 池：按固定归属规则把 route 分到 N 个 RecvWorker。
 *
 * 与 LocalPubSubRegistry::instance() / ShmControlScheduler::instance() 同构：
 * **故意泄漏的指针单例** —— 函数内静态对象的析构顺序相对全局/静态模块对象未
 * 定义，而后者析构时必须调用 remove_route()，池必须活得比它们久。 */
class IPC_EXPORT RecvWorkerPool
{
public:
    static RecvWorkerPool& instance();

    /* 固定归属规则（契约 §4.3）：FNV1a64(route_name ‖ domain_id) % worker_count。
     * route_name == nullptr 或 worker_count == 0 ⇒ 0。相同输入恒定输出
     * （路由表可重建、可断言），不同 domain 的同名 route 会落到不同 worker。 */
    static std::size_t worker_for(const char* route_name,
                                  std::uint32_t domain_id,
                                  std::size_t worker_count) noexcept;

    RecvWorkerPool();
    ~RecvWorkerPool();
    RecvWorkerPool(const RecvWorkerPool&) = delete;
    RecvWorkerPool& operator=(const RecvWorkerPool&) = delete;

    /* worker_count == 0 ⇒ hardware_concurrency()（至少 1）；上限 128（与
     * recv_wait_set 的 kMaxRoutes 同量级）。**一次性**：start 只成功一次，
     * stop 之后不可重启（池随进程存活，模块只 add_route/remove_route）。 */
    bool start(std::size_t worker_count = 0, const RecvBudget& budget = RecvBudget{});
    void stop() noexcept;
    bool running() const noexcept;
    std::size_t worker_count() const noexcept;

    /* 按 worker_for 计算归属并委托给对应 worker。池未 start ⇒ stopped。 */
    RecvRegisterStatus add_route(const std::shared_ptr<RecvRouteSource>& route);
    void remove_route(const RecvRouteSource* route) noexcept;

    static bool backend_available() noexcept;
    static const char* backend_name() noexcept;

    /* 各 worker 求和（route_count 为总数；idle_exits / thread_restarts 同样
     * 求和 —— 前者增长说明线程都在归还，后者说明按需拉起在工作）。 */
    RecvWorkerStats stats() const;
    std::size_t route_count() const noexcept;

    /* 追加（W06）：**生效**的预算（由首个成功 start() 的调用方一次性决定，见
     * 契约 §8.1）。池未 start ⇒ 默认 RecvBudget{}。只读回显，供模块把"实际生效
     * 的配置"写进运行证据，避免"我以为传了 X"这种无法核对的假证据（方案 §10.4）。 */
    const RecvBudget& budget() const noexcept;

    struct Impl;

private:
    Impl* impl_{nullptr};
};

}   // namespace threepools
}   // namespace dzIPC
