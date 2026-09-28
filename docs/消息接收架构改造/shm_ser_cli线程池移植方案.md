# SHM ser-cli 接收线程池移植方案

> 状态：**S1-S3 已落地并验证**（shm_ser_ipc 服务端请求接收接入进程级固定 SHM ReceiveWorker）；S4 仍待单独评审，本波不做。
> ⚠️ **本波未达成的收益（必读；t11 汇总时不得计入已交付）**：
> ① 处理路径为形态 (b) ⇒ **per-server 线程数未下降**（§6 第 1 条）；
> ② **D7** —— 控制面 tick 迁移（S3）**未执行** ⇒ `ser_handshake` 仍是 per-server 独立线程、仍 O(N)，
> 需求 §10 阶段 1「订阅握手线程 O(N)→0」**未达成**（见 §6「未达成项」）。
> 参照：shm_pub_sub_ipc 已落地的 SubState、process_received_buffer、RouteSession 与 recv_wait_set；固定 route worker 已由 ipc-transport 在 `include/dzIPC/threepools/recv_worker.h` 落地并冻结（含勘误 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md` 的 E1-E3）。
> 实现落点：`include/dzIPC/shm_ser_cli_ipc.h`（`SerState` / `SerRequestRoute`）、`src/dzIPC/shm_ser_cli_ipc.cc`（请求处理路径 / `start_data_plane` / 析构协议）。
> 范围：shm_ser_ipc 服务端请求接收优先；shm_cli_ipc 同步响应接收单独评审。
> 与阶段 1-4 基准实现对齐的更新点清单见 §8（文档优先：本方案已按基准实现回写）。

## 1. 当前结构与目标

shm_ser_ipc::response_thread_func 独占接收 _ser_r 通道，并在同一线程处理 nodelet 请求、wire 分流、用户 callback 和响应发送。每个 server 另有 ser_handshake 线程处理 heartbeat、死 client 回收和连接状态。

shm_cli_ipc 没有接收线程：send_request 在调用线程发送请求并从 _ser_w 同步等待响应；channel_mtx_ 与 cli_handshake 的 route 重建互斥。服务端被动请求接收可按 shm_sub_ipc 移植；客户端不能在没有请求关联设计时把接收移入共享 worker。

目标是让每个 server request channel 注册到固定 SHM ReceiveWorker，通过 read_wait_token 等待，并按有界预算处理完整请求。保持每 server 单消费者和 callback 串行。

## 1.5 当前状态（文档优先回写：现状 = 工作区实测，取代起草期描述）

> 本节按「文档优先」原则补写：**§1 第 1 段描述的是起草期（移植前）的结构**。
> 落地后的实际结构如下（实测锚点取自工作区，取证 2026-09-28，命令见 §11）。

| 维度 | 移植前 | 落地后（实测） |
| --- | --- | --- |
| 收包等待 | `response_thread_func` 在**同一线程**内 `ipc_r_ptr_->recv(50)`（阻塞）+ wire 分流 + callback + 响应发送 | worker 模式：**进程级固定 `RecvWorker`** 经 `SerRequestRoute::recv_once()`（非阻塞 `try_recv()`）收包 → 唤醒伪影门 → 入本 server 有界 FIFO；**本 server 的处理线程** `process_thread_func()` 出队 → `process_request()`（分流 → callback → `send_response()`） |
| 兼容回退 | —（唯一路径） | `response_thread_func()` 保留，语义与移植前逐条相同（仍为阻塞 `recv(50)` + 直接处理），与 worker 路径**共用** `process_request()` / `handle_fast_path()` |
| nodelet 快路径 | `response_thread_func` 每轮先 `fp_queue_->try_pop()` | 仅兼容模式注册 fp_queue（worker 模式**不注册**，见 §3） |
| 线程数（每 server） | 2（response 线程 + `ser_handshake`） | worker 模式 **2**（处理线程 + `ser_handshake`）；兼容模式 2（response 线程 + `ser_handshake`）⇒ **未下降** |
| 收包口 | `ipc_r_ptr_->recv(50)` | worker：`SerRequestRoute::recv_once()`；兼容：仍 `ipc_r_ptr_->recv(50)` |

⛔ 不得再用「数据接收线程数受 worker 上限约束」描述本模块的收益 —— 正确表述见 §6 第 1 条。

## 2. 从 shm_pub_sub_ipc 复用的机制

`shm_pub_sub_ipc` 已提供 shared state、消息处理抽取、RouteSession 和 wait token/wait-set 等机制。固定 route ReceiveWorker 已由 ipc-transport 落地并冻结（`RecvWorker` / `RecvWorkerPool` / `RecvRouteSource`，见 `include/dzIPC/threepools/recv_worker.h`），本模块**只消费、不复制**一份 worker。接口签名与调用约束以该头文件为准，本文不再重述。

1. 参照 SubState 将 worker 所需模板、队列、callback、消息 ID、状态锁、generation 和停止状态放入 shared state，worker 不保存裸对象指针。
2. 参照 RouteSession/ReceiveLease：先禁止新 lease、唤醒等待、等待 in-flight 归零，再 release 或重建通道。
3. 使用 SHM request route 的 `read_wait_token()` 和 `ipc::recv_wait_set`；route 固定属于一个 worker，不能由普通任务池或 work-stealing 迁移。
4. 把请求收包后的 wire 分流和投递提取成独立处理函数，保持 buffer 所有权及队列语义。
5. 析构顺序参照 shm_sub_ipc：先注销 LocalPubSubRegistry，再同步注销 worker，停止 route，等待静默，最后释放队列和通道。
6. wait-set 不可用时保留旧接收线程作为兼容后端，不做 try_recv 全量轮询。
7. 固定归属的 route 以 `RecvRouteSource` 适配器接入（本模块 = `SerRequestRoute`）。契约 §4.1 的纯虚接口与本模块实现：

| 接口 | 本模块实现 |
| --- | --- |
| `const char* route_name() const noexcept` | `state_->route_name`（`<service_prefix>_ser_r`，生命周期内不变） |
| `std::uint32_t domain_id() const noexcept` | `state_->domain_id` |
| `ipc::recv_wait_token read_wait_token() const noexcept` | `state_->req_ch->read_wait_token()`（无锁读：通道只在 `InitChannel` 赋值一次） |
| `std::size_t recv_once()` | 非阻塞 `try_recv()` + 唤醒伪影门 + 投递进本 server 的**有界 FIFO**，在完整请求边界返回。⛔ **不含**用户 callback、不含响应序列化/发送（裁决 D1：需求 §1.2 与 `recv_worker.h:218-220`）；它们由本 server 的处理线程 `process_request()` / `send_response()` 完成 |
| `bool has_pending() const noexcept` | 有界 FIFO 非空 **或** 上一次因满载未收取（`backpressured`）⇒ 让共享 worker 下一轮预算继续选中本 route（零丢失背压的重检判据） |
| `RecvOwner recv_owner() const noexcept` | `owner_`（`std::atomic<RecvOwner>`） |
| `bool try_claim_recv(RecvOwner who) noexcept` | `CAS(none → who)`；`who == none` 一律拒绝 |
| `void release_recv() noexcept` | `CAS(worker → none)`，回退路径再试 `CAS(compat_thread → none)` |
| `void stop_and_wake() noexcept` | 置 `stopping` + `req_ch->disconnect()` 唤醒 + `notify_all` |
| `void wait_quiescent() noexcept` | 等 `receive_inflight == 0` |

8. **注销第 6 步是 `release_recv()`**：宿主必须归还自己的 claim（worker 或 compat_thread），否则重新 `add_route` 会永久返回 `busy`。本模块两条数据面退出时都调用它（worker 路径由 `remove_route` 内部归还，兼容线程路径在 `response_thread_func` 末尾显式调用）。

## 3. 分支专属约束

- 单 server request route 只能由一个固定 worker 调用 recv；满足 thread_local 分片缓存和请求顺序约束。
- 多片重组、DZFlat/TLV 判别、msg_id/schema 校验、AcceptWire 和 chunk 归还逻辑不变。
- callback 首期保持每 server 串行执行。callback 时长不可控，须记录耗时并提供兼容独占线程逃逸配置，避免长 callback 阻塞同 worker 其他服务。
- response 的 DZFlat 借样发送、序列化和发送继续在 server 串行上下文完成。
- nodelet capacity-1 reply queue 必须将响应返回原 client；容量、克隆及 callback 语义不变。
- **nodelet 快路径与固定 worker 二选一**（基准实现修正）：`LocalPubSubRegistry` 的进程内请求队列**不纳入** SHM wait-set，而共享层只导出 `add_route` / `remove_route`（`RecvWorker::wakeup` 未由池转发），没有 route 级唤醒入口 —— worker 在 wait-set 里阻塞时无法被"队列有新请求"叫醒。故 `IsNodeletEnabled()` 为真时保留兼容接收线程（每轮 `try_pop` + `recv(50)`），worker 模式下 `InitChannel` **不注册** fp_queue（注册了反而会让请求投进没有消费者的队列而静默挂起）。
- ser_handshake **仍留在独立线程**（**已知限制 / 未达成项 D7**）：S3 未把 heartbeat/stale peer tick 迁入进程级控制调度器（`ShmControlScheduler`），`response_thread_func` 与它无关。保留现有 50ms 检查周期与 2s 死连接阈值。
  ⇒ 本模块的**控制面线程仍为每 server 一条、仍 O(N)**。需求 §10 阶段 1 的验收「握手线程从 O(N) 降为 0」在本模块**未达成**（该迁移属独立阶段，需与 SHM 控制面 owner 一起评审）。
  ⇒ 阶段 1 基线同样未接入产品：`ShmControlScheduler` 库内就绪但全仓无 `register_*` 调用点（见 `基准对齐笔记.md` §1.1、B3）。
- **fork 安全闸**（基准实现新增）：`RecvWorkerPool` 是进程级单例，worker **线程**不会随 fork 复制到子进程，但"已 start"标志会被继承 ⇒ 子进程 `add_route` 会返回 `ok` 却无人消费（静默丢包）。模块以 `current_process_id()` + `recv_pool_owner_pid()` 比对：pid 变化即回退兼容线程，且**不触碰池内部锁**（fork 时那把锁可能正被别的线程持有，子进程里会死锁）。
- shm_cli_ipc::send_request 保持调用线程同步接收。集中接收之前须定义 request ID、并发请求映射、超时取消、重建唤醒和析构协议。

## 4. 生命周期和 generation

server 停止/析构（基准实现的确切顺序）：

1. 注销 `LocalPubSubRegistry`（nodelet 注册过才做）；
2. `RecvWorkerPool::instance().remove_route(req_route_.get())` —— worker 侧同步注销，返回即"不会再碰这条 route"；
3. 置 `running=false`（停止 `ser_handshake` 循环）；
4. 标 route stopping 并 `disconnect()`/wakeup（`SerRequestRoute::stop_and_wake`）；
5. join 两条线程（`ser_handshake` 与兼容接收线程，若存在）；
6. `wait_quiescent()` 等 in-flight 归零；
7. 释放 request/response 通道（`clear`）与注册表项；
8. 释放 shared state（`state_` / `req_route_` 的 `shared_ptr` 引用计数归零）。

worker 路径的 claim 由 `remove_route` 内部 `release_recv()` 归还；兼容线程路径在第 5 步退出前显式归还。`generation` 在本模块只在控制面段单调递增（服务端不做 route 重建），已弹出的请求不会因 generation 变化被丢弃 —— 它们由 lease 保活并照常处理完。

generation 重建：禁止新 lease，唤醒旧接收，等待静默，remove 旧 token，释放旧 route，创建新 route 并发布 generation，再添加 token。已从旧 route 弹出的请求不能仅因 generation 更新而丢弃。

## 5. 执行阶段

### S0：基线 —— 已完成（由任务 D 采集，非本模块产出）
`test/perf/run_baseline.sh` + `test/perf/baseline_probe.cpp` 在 `build_baseline/`（HEAD 0d1b672 冻结快照）采集 1/100/1000 个空闲 SHM server 的线程数、上下文切换、CPU、RTT p50/p99、callback 耗时与关闭延迟，产物落 `test/perf/out/<stamp>/`。改造后用同一探针（工作区头 ABI）同口径复测。

### S1：抽取 server shared state 与处理函数 —— 已完成
新增 `SerState`（message/callback/两把锁、fp_queue、running、request_msg_id、generation、通道、`receive_inflight`、`stopping`、quiesce 条件变量、callback 耗时计数）。请求处理路径从 `response_thread_func` 抽出为 `process_request()`（wire 分流 → callback → 响应发送）与 `handle_fast_path()`（nodelet 快路径），两条数据面共用。

### S2：接入固定 SHM worker —— 已完成
`SerRequestRoute : RecvRouteSource`（9 接口 + `acquire_receive`/`release_receive` lease 配对）；`start_data_plane()` 在 nodelet 未启用时 `RecvWorkerPool::instance().start()` + `add_route()`，按 `RecvBudget`（32 条 / 1 MiB / 200 us）轮转，预算只在完整 `recv_once()` 边界让出。任何非 `ok` 状态（`backend_unavailable`/`busy`/`stopped`/…）一律显式回退兼容接收线程，**不**忙轮询。

### S3：控制面与关闭重建 —— 已完成（tick 迁移部分**未执行**）
析构/关闭按 §4 顺序落地（含注销第 6 步 `release_recv()`）；`ser_handshake` 仍为独立线程（见 §3）。

### S4：客户端响应接收单独评审 —— 本波不做
先设计请求关联和并发语义，再决定是否集中接收；不改变 `send_request` 同步 API。

## 6. 验收标准

- **收益口径（据实修订）**：本波**没有**减少每 server 的处理/握手线程数（worker 模式下仍为「1 处理线程 + 1 ser_handshake」，与改造前同为 2 条）。实际收益是：**收包等待/组包从 per-server 线程下沉到进程级固定 worker 池**（每 server 不再阻塞在 `recv(50)` 上；池线程空闲时按 `idle_keep_alive` 归还 OS）。因此本条不得再表述为「数据接收线程数受 worker 上限约束」；线程数相关数据须按实测基线报告（见 §6 证据索引与 t6 集成报告）。控制面线程变化单独报告。
- 单 server 请求和 callback 串行，多 client 顺序正确；分片重组完整且无错配。
- DZFlat/TLV、wire/schema 校验、chunk 所有权和 nodelet reply queue 不变。
- generation 重建、server 析构和 callback 执行期间 stop 均无死锁、悬挂 token 或悬空访问。
- wait-set 不可用时明确回退兼容线程，不忙轮询。

### 未达成项（t11 汇总口径；与上条「收益」并列，不得混读）

| ID | 未达成项 | 依据（文档位置 → 实现位置） | 归属 |
| --- | --- | --- | --- |
| **U-1** | per-server **线程数收益未达成**（形态 (b)：仍为处理线程 + `ser_handshake` = 2 条） | 本节第 1 条 vs `src/dzIPC/shm_ser_cli_ipc.cc` 文件头「线程数与收益」注释 | 本模块（设计选择，已获 captain 批准） |
| **U-2（=D7）** | **控制面 tick 迁移未执行** ⇒ `ser_handshake` 仍 O(N)，需求 §10 阶段 1「握手线程 O(N)→0」未达成 | §3 该条 vs `src/dzIPC/shm_ser_cli_ipc.cc` 的 `ser_handshake()`（独立线程，50ms/2s） | 独立阶段（需控制面 owner 评审） |
| **U-3** | C-05 的完整对照实验矩阵未跑（容量规则与严格上界已落地，定量对照待补） | t8 报告 §6.2/§7「R-1」 | 后续独立任务（t9 已按书面接受理由放行） |
| **U-4** | 5 项 FIFO 容量计数为 `SerState` 私有成员、**无公开取数口** | t9 报告 §5「N-3」（`include/dzIPC/shm_ser_cli_ipc.h` 无对应声明） | 后续独立任务（若补 U-3 实验则需 append-only 新增只读快照口） |

> 口径纪律：U-1/U-2 是**收益/验收未达成项**，U-3/U-4 是**证据与观测面缺口**；四者都**不得**计入已交付成果。

### 证据索引（改造后实测）

| 验收项 | 可复现命令 | 改造后实测 |
| --- | --- | --- |
| 构建零错误 | `cd build && make -j$(nproc)` | EXIT=0（见 deliverable §2） |
| 多 client 顺序（同 server 单消费者串行） | `env -u LD_LIBRARY_PATH build/bin/test_dzipc_shm --gtest_filter=DzIpcShm.RequestResponse` | 10/10 PASSED |
| 多片大请求（1 KiB/64 KiB/256 KiB/1 MiB/4 MiB 逐元素校验） | 探针 D：`mc_probe D`（源码与编译命令见 deliverable §5.2） | ok=100 bad=0 → PASS（改造前逐位一致） |
| 多 server 交错无串线 | 探针 E：`mc_probe E` | ok=160 bad=0 → PASS（改造前逐位一致） |
| server 重建 / generation | `env -u LD_LIBRARY_PATH build/bin/test_sercli_auto_path --gtest_filter='SerCliAutoPath.DisconnectThenReconnectRejudges'`；`build/bin/test_shm_ready_transition` | 1 PASS / 3 PASSED |
| callback 执行期间 stop | 探针 B：`ev_probe B`（源码见 deliverable §5.2） | dtor_ms≈391 cb_calls=1 → PASS（改造前 390.7 同口径） |
| 死 client 回收 | 探针 C：`ev_probe C`；`build/bin/test_uf003_crash_reclaim`（pub-sub 同机制） | new_client_handshake=1 → PASS；2 PASSED |
| nodelet 快路径（兼容线程分支） | `env -u LD_LIBRARY_PATH build/bin/test_shm_ser_cli_nodelet`；`test_shm_nodelet`；`test_nodelet_switch` | 5 / 13 / 8 PASSED |

探针为仓库外临时产物（不进 CMake、不进仓库），完整源码内嵌在交付 deliverable 中以保证可复现；仓库内既有用例覆盖上表其余各项。

#### 线程数与上下文切换实测（同探针同口径：`test/perf/baseline_probe.cpp`，空闲 server，`--mode=shm_ser_cli`）

| scale | threads_avg（前 → 后） | threads_per_server | ctx_vol_per_s | ctx_nonvol_per_s | cpu_cores |
| --- | --- | --- | --- | --- | --- |
| 1 | 4.000 → 35.000 | 4.000 → 35.000 | 40.658 → 341.158 | 0.667 → 0.000 | 0.013 → 0.020 |
| 100 | 202.000 → 134.000 | 2.020 → 1.340 | 3999.837 → 2318.075 | 6.658 → 1.998 | 0.060 → 0.033 |
| 1000 | 2002.000 → 1034.000 | 2.002 → 1.034 | 39960.783 → 20376.413 | 1208.539 → 3.975 | 0.415 → 0.113 |

读法（口径修订）：改造后**收包等待/组包**从 per-server 线程下沉到进程级固定池（`hardware_concurrency()=32` 的池只起一次），因此总线程数随 server 数增长的斜率下降（scale=1000 线程数减半、非自愿切换降 ~300x）；但**每 server 仍有一条处理线程**（形态 b：处理线程 + ser_handshake），per-server 线程数口径**未下降**。scale=1 的 +31 线程是池的启动代价，且会随空闲退出回落。

### 线程回落口径（已由共享层空闲退出解决，原「已知偏差」作废）

早期版本登记过一条「已知偏差」：`test/test_sercli_auto_path.cpp:337` 断言
`thread_count() <= threads_before`（析构后线程回落）在改造后失败（`before=1 after=33`）。
**该结论已被共享层实现推翻**：`RecvBudget::idle_keep_alive` 默认 1000 ms
（`include/dzIPC/threepools/recv_worker.h:250`），worker 的 route 表连续为空 >=
`max(idle_keep_alive, wait_timeout=100ms)` 时工作线程**归还操作系统**，之后任何
`add_route` 按需重新拉起（`Stats::idle_exits` / `thread_restarts`，同文件 `:271-272`）。
回落的可观测上界 = `idle_keep_alive` + 一次 wait 切片 + 调度调度 ≈ 1.1 s，远小于该断言
的 4000 ms 窗口。因此本模块**不需要**规避路径，也不得再把它写成不可达成的偏差；
实测口径见 `集成测试与性能基准报告.md`（t6）。

## 7. 风险

callback 运行时间不可控，共享 worker 会造成同 worker route 队头阻塞；先测 callback 时长并提供独占线程逃逸选项。shared_ptr 不能防止 server 内部句柄并发 release/rebuild，必须使用 lease/quiescent 屏障。

线程回落（见 §6）：`RecvWorkerPool` 的活动期随进程存活，但**工作线程**会在 route 表连续为空 >= `max(idle_keep_alive=1000ms, wait_timeout=100ms)` 时归还 OS，之后由 `add_route` 按需拉起 ⇒ "析构后线程回落"**成立**（可观测上界约 1.1 s）。不需要共享层额外能力，也不需要动测试断言。

模块内的两条禁令（**仍然有效**，与线程回落无关）：

- ⛔ 不得在 server 析构时调用 `RecvWorkerPool::stop()` —— 违反冻结契约：池的注释明确"模块只做 add_route/remove_route，**不**调用 stop"，且 §4.5 要求"不得在运行中来回切换后端（一次失败即永久回退）"；`stop()` 后池**不可重启**，后续 server 将永久回退兼容线程。
- ⛔ 不需要（也不得）为此新增共享层能力：线程归还已由共享层 `RecvBudget::idle_keep_alive` + 按需拉起提供。

fork 继承态：`RecvWorkerPool` 的"已 start"标志跨 fork 被继承而 worker 线程不会，若不做 pid 闸门会静默丢包；模块已加闸门并回退兼容线程（见 §3）。

## 8. 与阶段 1-4 基准实现对齐的更新点（文档优先回写）

| # | 原方案表述 | 基准实现 / 更新 |
| --- | --- | --- |
| 1 | "固定 route ReceiveWorker 尚未在该分支落地"（§2） | 已由 ipc-transport 落地并冻结于 `include/dzIPC/threepools/recv_worker.h`；本模块只消费 |
| 2 | §2 未给接口清单 | 补 `RecvRouteSource` 9 接口 + `has_pending` 与本模块映射表（§2 第 7 条） |
| 3 | §4 未点明注销末步 | 注销第 6 步 = `release_recv()`（归还 claim，否则重新 `add_route` 永久 `busy`） |
| 4 | §3 未区分 nodelet 与 worker | nodelet 启用 ⇒ 保留兼容接收线程，worker 模式下**不注册** fp_queue（无 route 级唤醒入口） |
| 5 | §3 "后续可将 ser_handshake tick 接控制调度器" | S3 该部分**未执行**；`ser_handshake` 仍为独立线程，50ms/2s 阈值不变 |
| 6 | §4 未提 fork | 新增 fork 安全闸：`current_process_id()` vs `recv_pool_owner_pid()`，pid 变化即回退兼容线程且不碰池锁 |
| 7 | §4 未说明 generation | 服务端无 route 重建，generation 只在控制面段单调递增；已弹出请求由 lease 保活、不因 generation 丢弃 |
| 8 | §6 验收未含线程回落 | 早期登记过「常驻池导致断言失败」，现已被共享层空闲退出推翻：worker 线程在 route 表连续为空 >= `max(idle_keep_alive=1000ms, wait_timeout=100ms)` 时归还 OS，`add_route` 按需拉起（`recv_worker.h:250/271-272`）⇒ 回落在断言 4000 ms 窗口内成立。**不作为已知偏差** |
| 9 | §5 阶段无落地状态 | S0/S1/S2/S3 标为已完成（S3 的 tick 迁移除外）；S4 本波不做 |

---

## 9. 同步边界表（P4 / 防复发 · 必填项）

> **本节为 t10 按 t7 finding P4 新增的模板与实例**：新引入的对象/指针/标志**必须**登记为「第几条同步边界」，
> 否则就会重演 C-01/C-02 —— `receive_state_` 当时**不在任何收尾清单里**，于是无人发现它被 concurrency 读写。
> 核对方法：对每个成员列出「谁写 / 谁读 / 同步原语或 API 约束」；凡"不加锁"的必须在同表写明约束来源。

| # | 对象 / 成员 | 谁写 | 谁读 | 同步原语 / 约束（实测锚点） |
| --- | --- | --- | --- | --- |
| B-1 | `SerState`（shared state，含 owner / stopping / receive_inflight / quiesce_cv / FIFO / 计数） | `start_data_plane()` 建 | worker（经 `SerRequestRoute`）、处理线程、注销路径 | **经 `state_mtx_` 只保护 `ser_state_` 这个 `shared_ptr` 本身**（`current_ser_state()` `:915-919`；写 `:939-940`；清 `:1047-1049`）；⛔ 锁内不再取 `SerState` 内部锁 |
| B-2 | `SerState::owner` | worker `add_route` / 兼容线程 claim / `remove_route` 第 6 步 | 三方 | `std::atomic<RecvOwner>` CAS，**集成在 state 级**（`ser_state_try_claim_recv` / `ser_state_release_recv`，`:158/:168`）；route 只转调（`:230/:237`） |
| B-3 | `SerState::pending`（有界 FIFO） | worker `recv_once()` 入队 | 处理线程 `process_thread_func()` 出队；注销路径作废 | `pending_mtx` + `pending_cv`；容量判定与峰值计数同在锁内；`has_pending()` 只读 `backpressured` / 计数（**无锁读口**） |
| B-4 | `SerState::stopping` / `receive_inflight` / `quiesce_cv` | `stop_and_wake()` / 注销方 | worker、注销方 | `std::atomic` + `quiesce_mtx`/`quiesce_cv`；`wait_quiescent()` 有界 **2000 ms**（`kSerQuiesceTimeoutMs`），超时只打诊断 |
| B-5 | `req_route_` | `start_data_plane()`（仅 worker 注册成功路径） | `stop_data_plane()` | **不加锁**，依赖 API 约束：`InitChannel` / `stop_data_plane` 由调用方串行化（auto 层 `leg_mtx_`）；⛔ 注销的 claim/release **不依赖**它（见 C-04 修复） |
| B-6 | `worker_mode_` / `response_thread_` | `start_data_plane()` / `InitChannel` / `stop_data_plane()` | 同上 | 同上（生命周期入口串行）；`response_thread_` 一个指针承载两种形态（worker=处理线程 / 兼容=接收线程，`:732-733`） |
| B-7 | `fp_queue_` / `fp_registered_` / `fp_msg_id_` | `InitChannel` / `reset_message` / 析构 | 兼容接收线程 | registry 的 `register/unregister_subscriber`；`reset_message` 在 `message_mtx_` 下换键 |

> ⛔ 本表是**收尾清单的一部分**：新增成员若未进本表，视为 C-01 同类风险。

## 10. 注释与调用点机械核对（P5 / 防复发）

> **根因**：C-03/C-04 的本质是「**注释抄了语义、函数体继承了旧实现**」——注释说"兼容线程会 claim"，函数体里却没有 claim。
> 仅靠人工读注释**无法**发现这一类缺陷。

**核对方法（机械、可判定）**：凡契约条款以注释形式落在代码里，必须在注释旁给出可 grep 的**调用点证据**。两类写法：

1. **条款注释 + 调用点计数**：在注释里写明「调用点见 `<symbol>`」，并保证 `grep -n "<symbol>"` 的命中数 ≥ 声明数。
   实例：`SerRequestRoute::try_claim_recv` 注释声明"兼容线程也会 claim" ⇒ `grep -n "ser_state_try_claim_recv" src/dzIPC/shm_ser_cli_ipc.cc` 必须同时命中定义（`:158`）、route 转调（`:230`）与**兼容线程真实调用**（`:808/813`）。
2. **不变量注释 + 反向判据**：注释若声明某符号"不依赖 X"，则必须给出 `grep -c "X"` 的期望值。
   实例：C-04 修复后声明"claim/release 不再依赖 `req_route_`" ⇒ 判据 `grep -c "req_route_->try_claim_recv\|req_route_->release_recv" src/dzIPC/shm_ser_cli_ipc.cc` = **0**；
   头文件 `include/dzIPC/shm_ser_cli_ipc.h:88` 原写"两条路径都会建好 `ser_state_`/`req_route_`"，实际 `req_route_` 只在 worker 注册成功时建（`:977` 唯一赋值点）。
   ⚠️ 该注释的修订**超出 t10 写权限**（t10 契约：⛔ 不得修改产品代码与既有测试）⇒ 本轮**未改**，已在 §11 登记为**转派项 W7**，须由持有 `include/dzIPC/shm_ser_cli_ipc.h` 写权限的模块 owner 执行。

## 11. 本轮文档优先回写与验收纪律

| # | 文档位置 | 改前 | 改后 | 依据（实现文件:行 / 实测读数） |
| --- | --- | --- | --- | --- |
| W1 | 本文件 §2 表格 `recv_once` 行 | 「非阻塞 `try_recv()` + **完整**处理（分流 / callback / 响应发送）」 | 「非阻塞 `try_recv()` + 唤醒伪影门 + 投递进有界 FIFO；⛔ 不含 callback / 响应发送（裁决 D1）」 | `src/dzIPC/shm_ser_cli_ipc.cc:202-267`（recv_once 体）vs `:1093-1180`（process_request 体）；契约 `include/dzIPC/threepools/recv_worker.h:214-220` ⛔；需求 §1.2 |
| W2 | 本文件 §1 第 1 段 | 只描述移植前结构，读起来像"现状" | 新增 §1.5「当前状态」对照表（实测锚点：`:732-733`、`:798`、`:1225`） | 同上 |
| W3 | 本文件 §6 第 1 条 | 「多 server 下数据接收线程数受 worker 上限约束」 | 「per-server 线程数**未下降**；收益是收包等待/组包下沉到进程级池」 | 实现文件头「线程数与收益」注释；t6 报告 §3.1 (`threads_max` 恒 33 / `threads_min` 2 是**池**的口径，per-server 仍是 1 处理线程) |
| W4 | 本文件 §3 / §6 | tick 迁移只说"未执行" | 升级为**未达成项 U-2（=D7）**：`ser_handshake` 仍 O(N)，需求 §10 阶段 1「握手线程 O(N)→0」未达成 | `src/dzIPC/shm_ser_cli_ipc.cc:743-796`（ser_handshake 独立线程，50ms/2s）；需求 §10 阶段 1 |
| W5 | 本文件 §4 第 3–6 步 | 顺序把"running=false"写在 `remove_route` 之后 | 与实现对齐：`stop_data_plane()` 内部先置 stopping/唤醒再 `remove_route`，最后 join 处理线程（`:987-1052`） | `src/dzIPC/shm_ser_cli_ipc.cc:995-1051` |
| W6 | 本文件 §9/§10 | （无） | 新增同步边界表（P4）与注释-调用点机械核对（P5） | t7 finding P4/P5 |
| W7 | `include/dzIPC/shm_ser_cli_ipc.h:88` 注释 | 「两条路径都会建好 `ser_state_`/`req_route_`」 | **待改**：「两条路径都会建好 `ser_state_`；`req_route_` 只在 worker 注册成功时建（`:977`），注销的 claim/release 已不依赖它」 | t9 报告 §5「N-2」；`req_route_ = route;` 仅 `:977` 一处。⚠️ **t10 无产品代码写权限，本轮未改** ⇒ 转派模块 owner |

> ⚠️ **复核提示（避免误判）**：本表"改前"列**必须**逐字保留原表述，因此 `grep -n "recv_once" <本文件> \| grep -c "完整.*callback"` 会命中**本行与上面的残留说明**，这是**预期**的。
> 判定 D1 是否已落地，请**只校验 §2 表格的那一行**：`sed -n "53p" <本文件> \| grep -c "不含"` = 1 且同行的 `完整.*callback` = 0。

### 验收纪律（A8，强制）

任何验收/复现前**必须**先重跑 CMake 配置再重编 —— 本仓库曾出现「build/ 里残留已消失实现的符号」导致**假绿**，
且 `aux_source_directory` / `file(GLOB)` 都是**配置期**展开：

```bash
cd /home/zwc/cpp_ipc_dds && cmake -S . -B build && make -C build -j$(nproc)
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS \
  ./bin/test_dzipc_shm --gtest_filter=DzIpcShm.RequestResponse
```

判据：`test_dzipc_shm` 的 stderr 必须出现 worker 证据行
`[<topic>_SerInfo] request receive on shared SHM worker <N> (generation 1, workers <M>)`；
若该行缺失且没有回退行，说明 worker 路径未被走到（**不可**据"用例通过"判定生效）。
