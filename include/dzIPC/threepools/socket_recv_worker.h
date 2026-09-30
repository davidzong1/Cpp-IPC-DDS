#pragma once
/* 阶段 5 · socket 侧固定 route 归属的收包 worker（SocketRecvWorker / SocketRecvWorkerPool）。
 *
 * 与 SHM 侧 `include/dzIPC/threepools/recv_worker.h` **逐条对称**：同一套固定归属规则、
 * 同一套预算安全边界、同一套注销协议、同一套空闲退出与按需拉起、同一套显式回退信号。
 * 唯一的机制差别是**等待对象**：SHM 等的是共享内存里的 sequence 字（ipc::recv_wait_set），
 * socket 等的是内核的可读状态（SocketWaitSet / epoll），因此这里用 SocketWaitToken 作为
 * 通道身份，而不是 ipc::recv_wait_token。
 *
 * ---- 为什么这个组件在共享层而不是各 socket 模块里 ----
 * src/CMakeLists.txt 把 src/dzIPC/*.cc 全部编进同一个 libipc，两个 socket 模块同在
 * namespace dzIPC::socket。若 socket_pub_sub 与 socket_ser_cli 各自实现一份
 * `SocketRecvWorkerPool::instance()`，同一 .so 里就是重复符号（链接失败）；各自塞进
 * 匿名命名空间则退化成**两个进程级池**（2N 条 worker，线程数口径作废）。所以池归共享层
 * 唯一 owner，两个模块只写各自的 route 适配器（SocketRecvRouteSource）。
 *
 * ---- route ↔ worker 固定归属（与 SHM 侧同一常量与算法）----
 *   worker_id = FNV1a64(route_name ‖ domain_id) % worker_count
 * route 在整个生命周期内只由该 worker 调用 recv_once()；注销后重新注册才允许重新分配。
 * worker **不**做 work-stealing。socket 侧的理由与 SHM 同理：data_rev.cc 的每 UDPNode
 * 重组状态（分片表、ACK 进度、RTT 估计）都假设**单消费者**，迁移 worker 会让同一通道的
 * 前后分片落在两个线程的重组状态上 ⇒ 偶发丢消息/重复投递。
 *
 * ---- 每 route 预算（与 SHM 侧同一语义）----
 * max_messages_per_route(32) / max_bytes_per_route(1 MiB) / max_processing_time_per_route(200us)
 * **只在安全边界让出**：每完成一次完整 recv_once() 之后才检查三项上限。socket 侧的
 * recv_once() 等价物是 chunk_rev_topic / chunk_rev_server ⟶ 一次调用可能持续到
 * 完整消息/请求组装完成，⛔ **不得在组包中途切走**（半组装状态不能跨线程移交）。
 * 预算耗尽**不是丢弃**：该 route 进 deferred FIFO，本轮 wait 前后各 drain 一次。
 *
 * ⛔ 用户 callback **不得**进入 recv_once()（需求 §1.2 与 recv_worker.h 的同名约束）：
 * recv_once() 只做收取 + 重组 + wire 判别 + 投递到模块自己的队列；用户回调由模块的
 * 处理路径承担（t3 的裁定：callback 出收包 worker）。
 *
 * ---- level-triggered 与"不丢就绪"----
 * socket 侧没有 SHM 的 sequence 字，事实来源是 SocketWaitSet 的 ready 集合：
 *   · wait_set 是 LT：没读走的可读事件会让下一次 wait 立即返回 ⇒ 读完才算消费；
 *   · consume_ready() 只返回**当前在册**的 token，因此 fd 复用不会误关联（见
 *     socket_wait_set.h 的实测记录）；
 *   · 注册成功时**无条件**让新 route 至少被处理一次（进 deferred FIFO 并敲一次唤醒通道），
 *     不依赖"注册后还有新事件" —— 注册之前已经在接收队列里的数据不会产生新事件，
 *     只靠 ready 判据会一直看不到它。代价是每条 route 注册时多一次非阻塞 recv_once
 *     （读到 0 即让出）。
 *
 * ---- 空闲退出与按需拉起（与 SHM 侧同一机制）----
 * route 表连续为空 >= max(idle_keep_alive, wait_timeout) ⇒ 工作线程归还操作系统；
 * 之后任何 add_route 在锁外 ensure_thread_alive() 按需拉起。idle_keep_alive == 0 保持
 * "下一轮空闲检查即退出"的字面语义（仅供测试）。Stats 的 idle_exits / thread_restarts
 * 是这条机制的诊断出口。线程归还的是**线程**，不是 worker：start() 仍是一次性的，
 * running() 在活动期内恒为 true。
 *
 * ---- add_route() 判定顺序（与 SHM 侧同序，含勘误 E2 的精确化）----
 *   route == nullptr                        -> invalid_route
 *   !running()（未 start / 已 stop）          -> stopped
 *   wait_token() 无效                        -> invalid_token
 *   try_claim_recv(worker) 成功              -> 继续注册
 *   try_claim_recv(worker) 失败:
 *       本 worker 表内已有同一 route          -> duplicate
 *       否则（别的 owner，如兼容线程在收）      -> busy
 *   wait_set.add 失败 && 从未成功             -> backend_unavailable（显式回退信号）
 *   wait_set.add 失败 && 曾成功               -> wait_set_full
 * 任何非 ok 都由模块作者**显式回退兼容线程**（每通道一条收包线程），⛔ 不得改成
 * receive_nowait() 全量忙轮询。
 *
 * ---- remove_route() 同步完成（返回即"worker 不会再碰这条 route"）----
 *   1. 从 route 表摘除 + 标记 removed   // 禁止新的 recv_once
 *   2. wait_set.remove(token)          // 摘除 + **唤醒**阻塞中的 wait（fd 才能关）
 *   3. route->stop_and_wake()          // 禁止新工作 + udp_node_cancel_wait 打断在途 recv
 *   4. 等 worker 侧预算轮窗口归零        // 有界：最多一个预算轮
 *   5. route->wait_quiescent()         // 等模块自己的 in-flight 记账归零
 *   6. route->release_recv()           // 归还收包独占（owner 回到 none）
 * 第 4 步在 route 表锁**之外**等待；worker 减计数不需要该锁。
 *
 * ---- fork 安全（本层不做 pid 探测，约束在模块侧）----
 * 池是**进程级单例**且 start() 一次性：fork 之后子进程继承"已 start"状态却没有工作线程，
 * 子进程里 add_route 会走按需拉起路径（取 lifecycle_mtx_/mtx 并创建线程），而被 fork 打断的
 * 父进程可能正持有这些锁 ⇒ **死锁**。因此本层**不提供也不使用任何 pid 查询**（不引入平台宏），
 * pid 闸（recv_pool_owner_pid 之类）是**模块侧**责任：owner pid != 当前 pid ⇒ 不得调用池的
 * add_route，继续用兼容收包线程，且判断过程中**不触碰池内部锁**。
 *
 * ---- 平台宏边界（硬约束）----
 * 本头文件**不得**出现任何平台宏或平台头；句柄一律 std::uintptr_t（Linux = fd，
 * Windows = WSAEVENT，0 = 不可等待）。平台条件编译只允许出现在
 * src/dzIPC/threepools/socket_wait_set.cc；头文件与 socket_recv_worker.cc 都不含。
 *
 * ---- fork/线程数配置 ----
 * DZIPC_SOCKET_RECV_WORKERS 覆盖 worker 数（进程内**只读一次**）；未设置 ⇒
 * hardware_concurrency()（至少 1），上限 128。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dzIPC/threepools/recv_worker.h"       // RecvOwner / RecvBudget / RecvRegisterStatus / RecvWorkerStats
#include "dzIPC/threepools/socket_wait_set.h"  // SocketWaitToken / SocketWaitSet
#include "libipc/export.h"

namespace dzIPC {
namespace threepools {

/* socket 侧宿主（模块）实现的 route 抽象：与 SHM 侧 RecvRouteSource **逐条对称**，
 * 唯一差别是等待身份用 SocketWaitToken（宿主稳定身份 + 平台句柄）而不是 ipc::recv_wait_token。
 *
 * 线程模型：wait_token / recv_owner / route_name / domain_id 可从任意线程调用；
 * recv_once **只允许 owner worker 调用**；stop_and_wake / wait_quiescent / release_recv 由注销方调用。
 *
 * ⛔ recv_once() 内部**不得**执行用户回调（需求 §1.2）。
 */
class IPC_EXPORT SocketRecvRouteSource
{
public:
    virtual ~SocketRecvRouteSource() = default;

    /* 固定归属的稳定 key。生命周期内不得变化。返回的字符串由宿主保活。 */
    virtual const char* route_name() const noexcept = 0;
    virtual std::uint32_t domain_id() const noexcept = 0;

    /* 可等待通道身份：owner = 宿主侧稳定身份（如 UDPNode*）；
     * handle = udp_node_wait_handle(node)（0 = 不可等待）。
     * ⛔ 不得自己拼平台调用；句柄一律走 dzIPC/common/data_rev.h 的 udp_node_* 转发。 */
    virtual SocketWaitToken wait_token() const noexcept = 0;

    /* 取一次，**到完整消息/请求边界返回**（chunk_rev_topic / chunk_rev_server 一次调用可以
     * 跨多个分片）。
     *
     * 【返回值语义，冻结 —— t4/t5 不得各写一套】
     *   · 正返回值 = 本次完成的**完整消息/请求的字节数**（≥ 1）；0 = 无数据 / 断开。
     *     worker 把该值累进 Stats::bytes_received 并用于 RecvBudget::max_bytes_per_route，
     *     因此返回"1"表示收到 1 字节的消息，不是"成功一次"。
     *   · 实现必须**先做非阻塞可读判据**（dzIPC::socket::udp_node_readable(node)）再调用
     *     chunk_rev_*：无数据时**立即返回 0**，⛔ 不得在共享 worker 线程上空读阻塞到 tm
     *     （chunk_rev_* 一律带 tm，空闲时阻塞到 tm 即 50ms/200ms；worker 的预算循环是
     *     `for(;;){ n = recv_once(); if (n == 0) break; ... }`，空读一次就白占一个共享线程）。
     *   · 字节数走 chunk_rev_topic / chunk_rev_server 的 `std::size_t* out_bytes` 出口
     *     （t12 追加的重载），不是 msg_ptr->size() 之类的近似。
     *
     * 内部完成模块自己的 in-flight 记账（含所有异常出口）。只允许 owner worker 调用。 */
    virtual std::size_t recv_once() = 0;

    /* level-triggered 重检：recv_once() == 0 之后仍有可收数据时为 true。
     * 默认 false（绝大多数实现一次 recv_once 就把 socket 接收队列读空）。 */
    virtual bool has_pending() const noexcept { return false; }

    /* 收包独占状态机（宿主用 std::atomic<RecvOwner> 实现，约三行）。 */
    virtual RecvOwner recv_owner() const noexcept = 0;
    virtual bool try_claim_recv(RecvOwner who) noexcept = 0;   ///< CAS(none -> who)
    virtual void release_recv() noexcept = 0;                   ///< CAS(worker -> none)

    /* 置 stopping + udp_node_cancel_wait(node) 唤醒阻塞中的 recv。幂等。 */
    virtual void stop_and_wake() noexcept = 0;
    /* 等模块自己的 in-flight 记账归零。stop_and_wake 之后调用。 */
    virtual void wait_quiescent() noexcept = 0;
};

/* 一条固定 route 归属的 socket 收包线程。生命周期：start() → add_route/remove_route → stop()。
 * start() 一次性（stop 之后不可重启）；内部工作线程会在空闲时归还并由 add_route 按需拉起。 */
class IPC_EXPORT SocketRecvWorker
{
public:
    SocketRecvWorker(std::size_t worker_id, RecvBudget budget = RecvBudget{});
    ~SocketRecvWorker();   ///< 等价 stop()
    SocketRecvWorker(const SocketRecvWorker&) = delete;
    SocketRecvWorker& operator=(const SocketRecvWorker&) = delete;

    bool start();              ///< 启动 1 条 worker 线程；重复调用返回 false
    void stop() noexcept;      ///< 唤醒 + join，幂等；可从任意线程调用
    /* **活动期**语义：start() 之后、stop() 之前恒为 true（工作线程空闲退出期间仍为 true）。 */
    bool running() const noexcept;
    /* 当前是否有工作线程：空闲退出后为 false，add_route 成功拉起后为 true。 */
    bool thread_alive() const noexcept;
    std::size_t worker_id() const noexcept;

    /* 见文件头判定顺序。成功后该 route 归本 worker 独占 recv。 */
    RecvRegisterStatus add_route(const std::shared_ptr<SocketRecvRouteSource>& route);

    /* 同步注销：返回即"worker 不会再碰这条 route"。幂等；未知 route / nullptr 都是无操作。
     *
     * ⛔ **不得**从 worker 线程调用（W04-F1 队长裁决 D-11：改注释 + 升级为可机械核对的判据）。
     *    与 SHM 侧逐字同因：本函数按契约 §4.4 第 4/5 步等待在途 `recv_once()` 与模块
     *    in-flight 归零，从 worker 线程调用时在途的那一次就是调用者自己 ⇒ 必然等满
     *    `kQuiesceTimeout`（2000 ms）后由超时分支打诊断返回。**实现里没有同线程旁路**，
     *    该用法不被支持，只是"不会永久死锁"。
     *
     * 机械核对判据：
     *   `grep -c "this_thread::get_id" src/dzIPC/threepools/socket_recv_worker.cc` 期望 **3**
     *   （`stop()`/析构各 1 + `remove_route()` 入口的 debug `assert`）；**没有**同线程旁路。 */
    void remove_route(const SocketRecvRouteSource* route) noexcept;

    /* 唤醒阻塞中的 wait 立即重评估（不改路由表）。 */
    void wakeup() noexcept;

    /* 复用 SHM 侧的统计结构（同一口径，便于 t6 对照）：stats 里 socket 侧不用的
     * wait_wakeups/wait_timeouts/wait_errors/routes_processed/messages_received/
     * bytes_received/budget_yields/deferred_drains/recv_errors/idle_exits/thread_restarts
     * 全部有定义（含义与 SHM 侧逐条相同）。 */
    RecvWorkerStats stats() const;
    std::size_t route_count() const noexcept;

    /* 进程内缓存的能力探测结果。**首次 add_route 后才有确定值**；未探测时返回 true（乐观）。 */
    static bool backend_available() noexcept;
    static const char* backend_name() noexcept;

    struct Impl;

private:
    Impl* impl_{nullptr};
};

/* 进程级 socket 收包 worker 池：按固定归属规则把 route 分到 N 个 SocketRecvWorker。
 *
 * 与 RecvWorkerPool / LocalPubSubRegistry / ShmControlScheduler 同构：**故意泄漏的指针单例**
 * —— 函数内静态对象的析构顺序相对全局/静态模块对象未定义，而后者析构时必须调用
 * remove_route()，池必须活得比它们久。 */
class IPC_EXPORT SocketRecvWorkerPool
{
public:
    static SocketRecvWorkerPool& instance();

    /* 固定归属规则：FNV1a64(route_name ‖ domain_id) % worker_count（与 SHM 侧同一常数）。
     * route_name == nullptr 或 worker_count == 0 ⇒ 0。相同输入恒定输出（路由表可重建、可断言）。 */
    static std::size_t worker_for(const char* route_name,
                                  std::uint32_t domain_id,
                                  std::size_t worker_count) noexcept;

    SocketRecvWorkerPool();
    ~SocketRecvWorkerPool();
    SocketRecvWorkerPool(const SocketRecvWorkerPool&) = delete;
    SocketRecvWorkerPool& operator=(const SocketRecvWorkerPool&) = delete;

    /* worker_count == 0 ⇒ DZIPC_SOCKET_RECV_WORKERS（进程内只读一次）⇒ hardware_concurrency()
     * （至少 1）；上限 128。**一次性**：start 只成功一次，stop 之后不可重启。 */
    bool start(std::size_t worker_count = 0, const RecvBudget& budget = RecvBudget{});
    void stop() noexcept;
    bool running() const noexcept;
    std::size_t worker_count() const noexcept;

    /* 按 worker_for 计算归属并委托给对应 worker。池未 start ⇒ stopped。 */
    RecvRegisterStatus add_route(const std::shared_ptr<SocketRecvRouteSource>& route);
    void remove_route(const SocketRecvRouteSource* route) noexcept;

    static bool backend_available() noexcept;
    static const char* backend_name() noexcept;

    RecvWorkerStats stats() const;
    std::size_t route_count() const noexcept;

    struct Impl;

private:
    Impl* impl_{nullptr};
};

}   // namespace threepools
}   // namespace dzIPC
