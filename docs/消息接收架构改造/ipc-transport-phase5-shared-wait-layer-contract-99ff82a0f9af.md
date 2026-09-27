# 阶段 5 共享等待层与固定收包 worker —— 接口契约（冻结）

> 状态：**接口冻结**。owner：ipc-transport（共享层唯一 owner）。
> 消费方：shm_ser_cli / socket_pub_sub / socket_ser_cli 三个模块作者。
> 冻结后新增字段/方法一律**追加**（带默认实现），不改既有签名与语义。
> **勘误优先**：与 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md`
> 冲突处按勘误读（E1 已就地修正在 §1；E2 已就地修正在 §4.4；E3 已补进 §0 交付面）。
> 落点见 `docs/消息接收架构改造/{事件驱动线程池需求.md §3.2/§5/§6, shm_sub_thread_consolidation_plan.md 阶段 5}`。

## 0. 交付面（5 个文件边界）

| 文件 | 角色 | 是否新增 |
|---|---|---|
| `include/libipc/udp.h` + `src/libipc/socket/udp.cpp` + `src/libipc/platform/{posix,win}/udp.h` | UDPNode 可等待句柄 + 取消等待（声明 / pimpl 转发 / 平台实现） | 改 |
| `include/dzIPC/threepools/socket_wait_set.h` + `.cc` | UDP 可等待 backend（Linux epoll / Windows WaitForMultipleObjects） | 新增 |
| `include/dzIPC/threepools/recv_worker.h` + `.cc` | SHM 固定 route 收包 worker + 进程级池 | 新增 |
| `include/dzIPC/common/data_rev.h` + `.cc` | UDPNode → SocketWaitToken 桥接（socket 两模块共用） | 改 |
| `include/dzIPC/threepools/socket_recv_worker.h` + `.cc` | socket 侧固定 route 收包 worker + 进程级池（与 SHM 侧同形，见 §5） | 新增 |

> **§5 的由来（追加项，契约正文原未覆盖）**：`src/CMakeLists.txt` 把 `src/dzIPC/*.cc` 全部编进
> 同一个 `libipc`，socket_pub_sub 与 socket_ser_cli 又同在 `namespace dzIPC::socket`。若两个模块
> 各自实现一份 socket 收包池，同一 .so 里就是**重复符号**（链接失败）；各自塞进匿名命名空间则退化成
> **两个进程级池**（2N 条 worker，固定线程数口径作废）。所以该组件归共享层唯一 owner，两个模块只写
> 各自的 route 适配器（`SocketRecvRouteSource`）。冻结规则不变：**新增一律追加**，不改既有签名与语义。

**平台宏边界（硬约束）**：`#if defined(__linux__)` / `_WIN32` 只允许出现在
`src/libipc/platform/*/udp.h`、`src/libipc/recv_wait_set.cpp`（既有）与
`src/dzIPC/threepools/socket_wait_set.cc`。三个 dzIPC 公开头
（`socket_wait_set.h` / `recv_worker.h` / `data_rev.h`）**不得出现任何平台宏**，
句柄一律以 `std::uintptr_t` 暴露（Linux = fd，Windows = WSAEVENT，0 = 无效）。

**勘误优先级**：本文档正文若与勘误
`ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md` 冲突，
**以勘误为准**（勘误 E1 已就地修正 §1 的 `wait_handle()` 描述；E2 精确化了
`add_route()` 的 duplicate/busy 判定顺序；E3 补列了 `src/libipc/socket/udp.cpp`）。

---

## 1. `ipc::socket::UDPNode` 新增（`include/libipc/udp.h`）

```cpp
class IPC_EXPORT UDPNode {
    /* ... 既有接口一字不动 ... */

    /* 该节点能否被多路等待：已 connect、入组（role != SendOnly）、未 cancel。
     * SendOnly 不入组 ⇒ 永远收不到东西，不构成可等待通道。 */
    bool waitable() const noexcept;

    /* 可等待句柄。Linux = 接收 fd 本身，**阻塞模式保持原样（阻塞）**；
     * Windows = 惰性创建的 WSAEVENT（首次调用内部做 WSAEventSelect，
     * 由 WinSock 的固有副作用置非阻塞，**无**显式 FIONBIO）。0 表示不可等待。
     *
     * ⚠️ 本条原写"Linux = 非阻塞接收 fd……WSAEventSelect + FIONBIO"，与实现相反，
     * 已按**勘误 E1** 修正（勘误优先于正文）。消费方从 wait-set 拿到就绪后**一律**
     * 用 receive_nowait() / chunk_rev_* 读；⛔ 不得对该 fd 用裸 recvfrom()（阻塞 fd
     * 会让 worker 线程永久挂住、此后静默停收且不报错），⛔ 不得自行置 O_NONBLOCK
     * （会把 receive(invalid_value) 的无限等待分支退化成紧循环忙轮询）。 */
    std::uintptr_t wait_handle() const noexcept;

    /* 取消阻塞在 wait_handle 上的等待，并让本节点的接收面失效。
     * Linux: shutdown(fd, SHUT_RD) —— epoll_wait 立刻返回，阻塞中的 recvfrom
     *        返回 0；此后 receive()/receive_nowait() 一律返回空 buffer。
     * Windows: WSASetEvent（WSAEventSelect 事件属于 node，必须由 node 唤醒）。
     * 幂等。⚠️ cancel 之后**不得**复用该 node 接收；close()+connect() 会复位。 */
    void cancel_wait() noexcept;

    /* 清除 wait_handle 上的就绪提示（level-triggered 重检前调用）。
     * Linux: no-op（epoll 本身就是 level-triggered）。Windows: WSAEnumNetworkEvents。 */
    void clear_wait() noexcept;
};
```

**不变量**：`wait_handle() != 0` ⇒ `waitable() == true`；`cancel_wait()` 之后
`waitable() == false` 且 `wait_handle() == 0`。

## 2. `dzIPC::socket` 桥接（`include/dzIPC/common/data_rev.h`）

socket 两个模块不直接拼句柄，统一走这 4 个 `nullptr`-安全的转发（冻结）：

```cpp
IPC_EXPORT bool         udp_node_waitable(const std::shared_ptr<ipc::socket::UDPNode>& node) noexcept;
IPC_EXPORT std::uintptr_t udp_node_wait_handle(const std::shared_ptr<ipc::socket::UDPNode>& node) noexcept;
IPC_EXPORT void         udp_node_cancel_wait(const std::shared_ptr<ipc::socket::UDPNode>& node) noexcept;
IPC_EXPORT void         udp_node_clear_wait(const std::shared_ptr<ipc::socket::UDPNode>& node) noexcept;
```

## 3. `dzIPC::threepools::SocketWaitSet`（新增）

```cpp
struct SocketWaitToken {
    const void* owner{nullptr};   // 宿主侧稳定身份（UDPNode* 或 shared state 指针）
    std::uintptr_t handle{0};     // 平台可等待句柄
    bool valid() const noexcept;  // owner != nullptr && handle != 0
    friend bool operator==(const SocketWaitToken&, const SocketWaitToken&) noexcept;
};

class IPC_EXPORT SocketWaitSet {
public:
    static bool backend_available() noexcept;   // 进程内缓存；false ⇒ 必须显式回退
    static const char* backend_name() noexcept; // "epoll" / "WaitForMultipleObjects" / "none"
    static std::size_t max_channels() noexcept; // Linux: 实现上限; Windows: 63

    SocketWaitSet();
    ~SocketWaitSet();                            // 等价 stop()
    SocketWaitSet(const SocketWaitSet&) = delete;
    SocketWaitSet& operator=(const SocketWaitSet&) = delete;

    bool add(const SocketWaitToken& token);      // 幂等（同 owner 重复 add 成功）
    bool remove(const SocketWaitToken& token);   // 幂等；必须唤醒阻塞中的 wait()
    bool wait(std::chrono::milliseconds timeout);
    std::vector<SocketWaitToken> consume_ready();
    void stop() noexcept;                        // 阻塞中的 wait 必须返回
    std::size_t size() const noexcept;
};
```

语义（与既有 `ipc::recv_wait_set` 逐条对齐）：

| 项 | 约定 |
|---|---|
| `wait` 返回 true | 至少一路就绪，**或**被 `remove`/`stop` 唤醒 ⇒ 调用方应 `consume_ready()`（可能为空） |
| `wait` 返回 false | 超时，或 backend 系统调用失败（已记日志）⇒ 按「后端不可用」处理，**禁止忙等** |
| `remove` 之后 | 该 token 不再出现在 `consume_ready()`；fd/handle 才能 close |
| level-triggered | 未读走的可读事件会让下一次 `wait` 立即返回（LT 天然成立），调用方必须真读 |
| fd 复用 | `remove` 是同步摘除；`consume_ready()` 只返回**当前在册**的 handle，旧 fd 复用不误触发 |
| 重复 owner / 不同 handle | 拒绝（返回 false），不改变已有集合 |

**Windows 容量**：`WaitForMultipleObjects` 上限 64，stop event 占 1 ⇒ 最多 63 路。
超出时 `add` 返回 false，由阶段 5 把通道分到别的 worker（**不循环多次 WFMO 假装无限路**）。

## 4. `dzIPC::threepools::RecvWorker` / `RecvWorkerPool`（新增）

### 4.1 宿主实现的 route 抽象

```cpp
enum class RecvOwner { none, compat_thread, worker };

class IPC_EXPORT RecvRouteSource {
public:
    virtual ~RecvRouteSource() = default;

    /* 固定归属的稳定 key。生命周期内不得变化。 */
    virtual const char* route_name() const noexcept = 0;
    virtual std::uint32_t domain_id() const noexcept = 0;

    /* libipc 读等待 token（ipc::route::read_wait_token()）。无效 ⇒ 不能进 wait-set。 */
    virtual ipc::recv_wait_token read_wait_token() const noexcept = 0;

    /* 取一次。0 = 无数据/断开。**必须非阻塞或短超时**（try_recv 或 recv(0)），
     * 内部完成 RouteSession lease 配对。只允许 owner worker 调用。 */
    virtual std::size_t recv_once() = 0;

    /* level-triggered 重检：recv_once()==0 之后仍有可收数据时为 true。 */
    virtual bool has_pending() const noexcept { return false; }

    /* 收包独占状态机（宿主用 std::atomic<RecvOwner> 实现，约三行）。 */
    virtual RecvOwner recv_owner() const noexcept = 0;
    virtual bool try_claim_recv(RecvOwner who) noexcept = 0;  // CAS(none → who)
    virtual void release_recv() noexcept = 0;                  // CAS(worker → none)

    /* route 生命周期协议（阶段 2 RouteSession）：拒绝新 lease + 唤醒阻塞中的 recv。 */
    virtual void stop_and_wake() noexcept = 0;
    /* 等 in-flight lease 归零。 */
    virtual void wait_quiescent() noexcept = 0;
};
```

### 4.2 预算与统计

```cpp
struct RecvBudget {
    std::size_t max_messages_per_route{32};
    std::size_t max_bytes_per_route{1u << 20};                        // 1 MiB
    std::chrono::microseconds max_processing_time_per_route{200};     // 200 us
    std::chrono::milliseconds wait_timeout{100};                      // 空闲 wait 上限
};

struct RecvWorkerStats {
    std::uint64_t wait_wakeups{0}, wait_timeouts{0}, wait_errors{0};
    std::uint64_t routes_processed{0}, messages_received{0}, bytes_received{0};
    std::uint64_t budget_yields{0}, deferred_drains{0};
    std::size_t route_count{0};
};
```

**预算只在安全边界生效**：每完成一次完整 `recv_once()` 之后才检查三项上限。
SHM 的多片重组在 `libipc` 内完成（`recv` 返回的是完整消息），所以"一次
`recv_once` 返回"就是安全边界。socket 侧（`chunk_rev_topic`/`chunk_rev_server`
一次调用可能持续到完整消息/请求组装完成）同样以"该函数返回"为边界，不得在
组包中途切走（见 socket 两模块方案 §4/§8）。

预算耗尽**不是丢弃**：该 route 进入 worker 的 deferred FIFO，本轮 `wait` 前后
各 drain 一次。deferred 用**固定 FIFO 顺序**，因此热 route 不会长期饿死冷 route。

### 4.3 固定 route 归属（稳定分配，不 work-stealing）

```cpp
static std::size_t RecvWorkerPool::worker_for(const char* route_name,
                                              std::uint32_t domain_id,
                                              std::size_t worker_count) noexcept;
```

- 规则：`worker_id = FNV1a64(route_name ‖ domain_id) % worker_count`（需求 §3.2）。
- route 在**整个生命周期内**只由该 worker 调用 `recv_once()`；
  注销后重新注册才允许重新分配。worker **不**通过 work-stealing 抢占别的 route。
- 依据：`libipc::conn_info_head::recv_cache()` 是 `thread_local` 分片缓存，
  同一 route 迁移到别的 worker 会让多片消息的前后片段进不同缓存而重组失败
  （需求 §2.1）。
- worker 数默认 `hardware_concurrency()`，`worker_count==0` 表示取默认；
  上限 128（与 `recv_wait_set` 的 `kMaxRoutes` 同量级）。

### 4.4 生命周期与注销协议

```cpp
enum class RecvRegisterStatus {
    ok, backend_unavailable, duplicate, busy, stopped, invalid_token, invalid_route
};
class IPC_EXPORT RecvWorker {
public:
    RecvWorker(std::size_t worker_id, RecvBudget budget = RecvBudget{});
    ~RecvWorker();                                   // 等价 stop()
    bool start();                                    // 启动 1 条 worker 线程
    void stop() noexcept;                            // 唤醒 + join，幂等
    bool running() const noexcept;
    std::size_t worker_id() const noexcept;

    RecvRegisterStatus add_route(std::shared_ptr<RecvRouteSource> route);
    void remove_route(const RecvRouteSource* route) noexcept;   // 幂等，同步完成
    void wakeup() noexcept;                                     // 唤醒 wait 立即重评估
    RecvWorkerStats stats() const;
    std::size_t route_count() const noexcept;

    static bool backend_available() noexcept;   // 首次 add_route 后有效
    static const char* backend_name() noexcept;
};
```

`add_route()` 的判定顺序（决定返回哪个 status）：

```text
route == nullptr                     -> invalid_route
!running()                           -> stopped
read_wait_token() 无效                -> invalid_token
try_claim_recv(worker) 成功           -> 继续注册
try_claim_recv(worker) 失败:
    本 worker 的 route 表内已有同一 route -> duplicate   // 自己重复注册
    否则（别的 owner，如 compat_thread）  -> busy        // 兼容 subscribe_thread_ 正在 recv
wait_set.add(token) 失败 && 从未成功  -> backend_unavailable  // 显式回退信号
wait_set.add(token) 失败 && 曾成功    -> wait_set_full
```

**注（勘误 E2）**：上面两行不是两条并列的独立判据，而是**一次 CAS + 一次查表**：
先 `try_claim_recv(worker)`，失败后才在 route 表内查是否已注册同一 route。对外语义
不变（"自己重复注册"与"兼容线程在收"处置相反），只是判定顺序实现为"先尝试接管、
失败再查表"。`RecvWorkerPool::add_route()` 同。

`remove_route()` **同步**完成（返回即安全释放 route），严格按协议：

```text
1. 从 worker 的 route 表摘除 + 标记 removed   // 禁止新的 recv_once
2. wait_set.remove(token)                     // 唤醒阻塞中的 worker wait
3. route->stop_and_wake()                     // 禁止新 lease + 唤醒 route 内部 recv
4. 等 worker 侧 in-flight recv_once 归零       // 有界：最多一次 recv_once
5. route->wait_quiescent()                    // 等 lease 归零
6. route->release_recv()                      // 归还收包独占（owner 回到 none）
```

**死锁排除**：第 4 步在 worker 的 `routes_mtx_` **之外**等待；worker 线程减
in-flight 计数不需要该锁（entry 由本地 `shared_ptr` 保活）。第 5/6 步时 worker
已不可能再进 `recv_once`（第 1、4 步已保证）。

**析构顺序（模块作者照此接入）**：

```text
1. 注销 LocalPubSubRegistry / 模块自己的 registry
2. pool.remove_route(route)      // 即上面的 1-6
3. 注销控制面调度器
4. 关闭 socket / release route
5. 释放队列与 shared state
```

### 4.5 能力探测与显式回退（禁止忙轮询 / 禁止运行中切换）

- libipc 的 `recv_wait_set` 在进程内**只探测一次**并缓存（Linux：
  `futex_waitv` 0 超时调用；`ENOSYS`/`EINVAL` ⇒ 不可用）。本层**不重复**平台探测代码。
- 本层通过 `wait_set.add(token)` 的返回值观察探测结果，并缓存到
  `RecvWorker::backend_available()`（原子三态：未探测 / 可用 / 不可用）。
- `backend_unavailable` 是**显式回退信号**：模块作者必须保留原有
  `subscribe_thread_` / `response_thread_` 兼容路径，并打一条「wait-set 不可用，
  保持每 route 收包线程」的日志。
- ⛔ 不得改成 `try_recv()` / `receive_nowait()` 全量忙轮询。
- ⛔ 探测结果**进程内缓存**，不得在运行中来回切换后端（一次失败即永久回退，
  避免"半进程用池、半进程用线程"的不可推理状态）。
- 本机实测：内核 6.8，`futex_waitv` 可用，`backend_available() == true`（见
  `build/bin/test_recv_wait_set` 9/9 PASS 与本次新增测试）。

### 4.6 单 route 单消费者（与兼容线程互斥）

一条 route 同一时刻只允许一个消费者：要么兼容后端的 `subscribe_thread_` 在
`recv()` 里等，要么进某个 `RecvWorker` 的 `recv_wait_set`。**禁止两路同时 recv**
（需求 §5 末段 / 阶段 4 说明 §1.1）。

落地方式：`RecvRouteSource` 的 `try_claim_recv()` / `release_recv()` /
`recv_owner()` 三方法构成显式独占状态机，由宿主用 `std::atomic<RecvOwner>` 实现：

- 兼容线程启动前 `try_claim_recv(compat_thread)`；退出后 `release_recv()`。
- `RecvWorker::add_route()` 只在 `try_claim_recv(worker)` 成功时接管，否则
  返回 `busy`（模块作者据此保留兼容线程，不静默双收）。
- `remove_route()` 第 6 步把 owner 归还 `none`，之后才允许兼容线程重新接管。

### 4.7 线程模型

```text
RecvWorkerPool (每进程 1 个, 单例)
  └─ RecvWorker[0..N-1]        每条 1 个线程, N 默认 CPU 数
       ├─ 自有 ipc::recv_wait_set
       ├─ 固定 route 表 (route → entry, entry 持 shared_ptr<RecvRouteSource>)
       ├─ ready 扫描 + 每 route 预算 + deferred FIFO
       └─ Stats 计数
```

worker 循环：

```text
while (running):
    drain_deferred()                 // 上一轮预算耗尽的 route, FIFO
    if (!running) break
    if (!wait_set.wait(budget.wait_timeout)) ++wait_timeouts; continue   // 超时不是错误
    for token in wait_set.consume_ready():
        entry = lookup(token)        // 已 remove / 未知 ⇒ 跳过（fd 复用不误触发）
        run_budget(entry)            // 预算内 recv_once, 耗尽则入 deferred
    drain_deferred()
```

worker 默认使用普通调度；绑核 / `SCHED_FIFO` / 独占收包线程只作为显式 opt-in
逃逸通道（由模块作者用既有 `ThreadDispatch` 提供），本层不默认启用。

---

### 4.8 fork 安全（进程级池的单例代价）

`RecvWorkerPool` 是**进程级单例**且 `start()` 一次性随进程存活：fork 之后子进程继承
"已 start"的状态，但**没有**工作线程。子进程里 `add_route` 会走"按需拉起"路径（要取
`lifecycle_mtx_`/`mtx` 并创建线程），而被 fork 打断的父进程可能正持有这些锁 ⇒ 子进程里
有**死锁**风险（比旧行为的"返回 ok 后静默丢包"更严重）。

因此**接口侧只冻结约束、不实现 pid 探测**（本层不引入平台宏）：

- 本层不提供、也不使用任何 pid 查询；`recv_pool_owner_pid()` 之类的 pid 闸是**模块侧**
  责任（落点如 `shm_ser_cli_ipc.cc`）；
- 模块侧规则：owner pid != 当前 pid ⇒ **不得**调用池的 `add_route`，继续用兼容收包线程；
  且判断过程中**不触碰池内部锁**（fork 时那把锁可能正被别的线程持有）。

## 5. socket 侧固定收包 worker（追加交付面，§0 表末行）

`include/dzIPC/threepools/socket_recv_worker.h` + `.cc`：与 §4 的 SHM 侧**逐条同形**，
差别只有一处 —— 等待身份用 `SocketWaitToken`（宿主稳定身份 + 平台句柄）而不是
`ipc::recv_wait_token`。复用既有类型，不新造：`RecvOwner` / `RecvBudget` /
`RecvRegisterStatus` / `RecvWorkerStats`（recv_worker.h）、`SocketWaitToken`（socket_wait_set.h）。

```cpp
class SocketRecvRouteSource {   // 与 RecvRouteSource 逐条对称
    virtual const char* route_name() const noexcept = 0;
    virtual std::uint32_t domain_id() const noexcept = 0;
    virtual SocketWaitToken wait_token() const noexcept = 0;   // handle 由 udp_node_wait_handle(node) 给出
    virtual std::size_t recv_once() = 0;                       // 到完整消息/请求边界返回；⛔ 不含用户回调
    virtual bool has_pending() const noexcept { return false; }
    virtual RecvOwner recv_owner() const noexcept = 0;
    virtual bool try_claim_recv(RecvOwner who) noexcept = 0;
    virtual void release_recv() noexcept = 0;
    virtual void stop_and_wake() noexcept = 0;   // 置 stopping + udp_node_cancel_wait
    virtual void wait_quiescent() noexcept = 0;
};

class SocketRecvWorker {          // 与 RecvWorker 同形；每 worker **自有** SocketWaitSet 实例
    SocketRecvWorker(std::size_t worker_id, RecvBudget budget = RecvBudget{});
    bool start(); void stop() noexcept;
    bool running() const noexcept; bool thread_alive() const noexcept; std::size_t worker_id() const noexcept;
    RecvRegisterStatus add_route(const std::shared_ptr<SocketRecvRouteSource>&);
    void remove_route(const SocketRecvRouteSource*) noexcept;
    void wakeup() noexcept;
    RecvWorkerStats stats() const; std::size_t route_count() const noexcept;
    static bool backend_available() noexcept; static const char* backend_name() noexcept;
};

class SocketRecvWorkerPool {      // 进程级故意泄漏的指针单例；池级 wakeup 未转发（同 SHM 侧）
    static SocketRecvWorkerPool& instance();
    static std::size_t worker_for(const char* route_name, std::uint32_t domain_id, std::size_t worker_count) noexcept;
    bool start(std::size_t worker_count = 0, const RecvBudget& budget = RecvBudget{});   // 一次性
    void stop() noexcept; bool running() const noexcept; std::size_t worker_count() const noexcept;
    RecvRegisterStatus add_route(const std::shared_ptr<SocketRecvRouteSource>&);
    void remove_route(const SocketRecvRouteSource*) noexcept;
    RecvWorkerStats stats() const; std::size_t route_count() const noexcept;
    static bool backend_available() noexcept; static const char* backend_name() noexcept;
};
```

语义（与 §4 逐条一致，t4/t5/t6 照此对照）：

| 项 | 约定 |
|---|---|
| 固定归属 | `worker_id = FNV1a64(route_name ‖ domain_id) % worker_count`，与 SHM 侧**同一常量与算法**（逐值一致，已在 `test/test_socket_recv_worker.cpp` 对拍） |
| 预算 | 三项（32 条 / 1 MiB / 200 us）**只在一次完整 `recv_once()` 返回后**检查；⛔ 不得在组包中途切走 |
| deferred | 固定 FIFO；**deferred 非空时先做一次 `wait(0)` 全量探测再 drain**（否则热 route 饿死冷 route） |
| 空闲 | 只走**阻塞** wait（`min(wait_timeout, 剩余空闲窗口)`，下界 1 ms）；⛔ 不得 `receive_nowait` 全量忙轮询 |
| add_route | 判定顺序与返回码同 §4.4（含 duplicate/busy/wait_set_full）；任何非 ok 由模块显式回退兼容线程 |
| remove_route | 同步：摘除 + 标记 → `wait_set.remove`（**唤醒**阻塞中的 wait）→ `stop_and_wake` → 等预算轮归零 → `wait_quiescent` → `release_recv`；返回后宿主才能关 fd |
| fd 复用 | `consume_ready()` 只返回当前在册 token；socket_wait_set 用代际 + 显式 DEL 双保险 |
| 空闲退出 | route 表连续为空 >= `max(idle_keep_alive, wait_timeout)` ⇒ 线程归还；之后 `add_route` 按需拉起；`idle_exits` / `thread_restarts` 计数 |
| 假就绪兜底 | `SocketWaitSet` 与 SHM 的 `recv_wait_set` 有一处**必须不同**：SHM 的事实判据是共享内存里的 seq 字，socket 只有"内核说可读"。为防病理性"报就绪却读不到"把 worker 变成忙轮询，连续 `kMaxFruitlessReadiness`(4) 轮无果的 token 会被 `remove+add` 重挂（清用户态 ready 残留）并重置计数 ⇒ 循环速率有界，真数据到达时第一轮就读到并清零 |
| 线程数 | 默认 `hardware_concurrency()`（至少 1），上限 128，`DZIPC_SOCKET_RECV_WORKERS` 覆盖且**进程内只读一次** |
| fork | 池**不做** pid 探测（不引入平台宏）；owner pid 闸是模块侧责任，pid 变化 ⇒ 不得调 `add_route` 且不触碰池锁（同 §4.8） |
| callback | ⛔ `recv_once()` 只做收取 + 重组 + wire 判别 + 投递；用户回调由模块的处理路径承担（需求 §1.2） |

至少被处理一次：注册成功时**无条件**让新 route 进 deferred FIFO 并敲一次唤醒通道，不依赖"注册后还有新事件"
—— 注册之前已经在 socket 接收队列里的数据不会产生新事件，只靠 ready 判据会一直看不到它。

聚焦自检：`test/test_socket_recv_worker.cpp`（5 条，注册进 CTest）—— 归属与 SHM 侧逐值对拍、
注册判定顺序与单消费者互斥（duplicate/busy）、真通道收取 + `remove_route` 同步唤醒、空闲退出与按需拉起、
池的固定归属与线程数口径。

---

## 6. 未在 Windows 验证（显式声明）

本机为 Linux 6.8，`cargo`/`rustc` 不可用不影响本任务。**Windows 路径
（`src/libipc/platform/win/udp.h` 的 WSAEventSelect/WSAEvent、
`src/dzIPC/threepools/socket_wait_set.cc` 的 `WaitForMultipleObjects` 分支、
`recv_wait_set.cpp` 既有 Windows 分支）本次**未在 Windows 验证**，只做编译期
与接口一致性保证。报告中将显式标注该限制。
