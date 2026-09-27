# SHM ser-cli 接收线程池移植方案

> 状态：**S1-S3 已落地并验证**（shm_ser_ipc 服务端请求接收接入进程级固定 SHM ReceiveWorker）；S4 仍待单独评审，本波不做。
> 参照：shm_pub_sub_ipc 已落地的 SubState、process_received_buffer、RouteSession 与 recv_wait_set；固定 route worker 已由 ipc-transport 在 `include/dzIPC/threepools/recv_worker.h` 落地并冻结（含勘误 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md` 的 E1-E3）。
> 实现落点：`include/dzIPC/shm_ser_cli_ipc.h`（`SerState` / `SerRequestRoute`）、`src/dzIPC/shm_ser_cli_ipc.cc`（请求处理路径 / `start_data_plane` / 析构协议）。
> 范围：shm_ser_ipc 服务端请求接收优先；shm_cli_ipc 同步响应接收单独评审。
> 与阶段 1-4 基准实现对齐的更新点清单见 §8（文档优先：本方案已按基准实现回写）。

## 1. 当前结构与目标

shm_ser_ipc::response_thread_func 独占接收 _ser_r 通道，并在同一线程处理 nodelet 请求、wire 分流、用户 callback 和响应发送。每个 server 另有 ser_handshake 线程处理 heartbeat、死 client 回收和连接状态。

shm_cli_ipc 没有接收线程：send_request 在调用线程发送请求并从 _ser_w 同步等待响应；channel_mtx_ 与 cli_handshake 的 route 重建互斥。服务端被动请求接收可按 shm_sub_ipc 移植；客户端不能在没有请求关联设计时把接收移入共享 worker。

目标是让每个 server request channel 注册到固定 SHM ReceiveWorker，通过 read_wait_token 等待，并按有界预算处理完整请求。保持每 server 单消费者和 callback 串行。

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
| `std::size_t recv_once()` | 非阻塞 `try_recv()` + **完整**处理（分流 / callback / 响应发送），在完整请求边界返回 |
| `bool has_pending() const noexcept` | nodelet 进程内队列非空（level-triggered 重检） |
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
- ser_handshake **仍留在独立线程**：S3 未把 heartbeat/stale peer tick 迁入进程级控制调度器，`response_thread_func` 与它无关。保留现有 50ms 检查周期与 2s 死连接阈值。
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

- 多 server 下数据接收线程数受 worker 上限约束；控制面线程变化单独报告。
- 单 server 请求和 callback 串行，多 client 顺序正确；分片重组完整且无错配。
- DZFlat/TLV、wire/schema 校验、chunk 所有权和 nodelet reply queue 不变。
- generation 重建、server 析构和 callback 执行期间 stop 均无死锁、悬挂 token 或悬空访问。
- wait-set 不可用时明确回退兼容线程，不忙轮询。

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

读法：改造后每 server 的接收线程数由 "1 接收 + 1 握手 + 通道" 降为常数项（`hardware_concurrency()=32` 的池只起一次），server 数越大收益越明显（scale=1000 线程数减半、非自愿切换降 ~300x）。scale=1 是常驻池的固定启动代价（+31 线程、+300 ctx/s），也是 §6 已知偏差的同一根因。

### 已知偏差（需 leader / 测试 owner 决策）

`test/test_sercli_auto_path.cpp:337` 断言 `thread_count() <= threads_before`（析构后线程回落到进入用例前水平）。改造后该用例失败（`before=1 after=33`，= 1 主线程 + 32 个池 worker）。根因是共享层设计：`RecvWorkerPool` 为**进程级一次性常驻**池（`hardware_concurrency()` 个 worker，`stop` 后不可重启），server 析构只 `remove_route`，池线程不退出；改造前每 server 一条接收线程、析构即 join，故断言成立。本模块**无权限**改测试或共享层；三条可行路径均超出本模块写权限：(a) 共享层提供空闲退出/可停池；(b) 测试断言改为"不随 server 数线性增长"；(c) 保留进程级池并将该断言登记为已知偏差。

## 7. 风险

callback 运行时间不可控，共享 worker 会造成同 worker route 队头阻塞；先测 callback 时长并提供独占线程逃逸选项。shared_ptr 不能防止 server 内部句柄并发 release/rebuild，必须使用 lease/quiescent 屏障。

常驻池线程与既有"线程回落"断言冲突（见 §6 已知偏差）：`RecvWorkerPool` 一次性常驻，server 析构不回收 worker 线程，因此"每 server 线程数"降为常数、但"析构后线程回落"不再成立。这是本移植的**可观测代价**，需要共享层（空闲退出/可停池）或测试断言侧决策。

模块内**无合法规避路径**（已逐条排除）：

- 在最后一个 server 析构时调用 `RecvWorkerPool::stop()` —— 违反冻结契约：池的注释明确"模块只做 add_route/remove_route，**不**调用 stop"，且 §4.5 要求"不得在运行中来回切换后端（一次失败即永久回退）"；`stop()` 后池**不可重启**，后续 server 将永久回退兼容线程，等于把"先建后析构再建"的常规流程永久降级。
- 让 worker 在空闲时自行退出 —— 需要共享层新增空闲退出能力，本模块无写权限（`threepools/*` 归 ipc-transport）。
- 修改 `test/test_sercli_auto_path.cpp:337` 的断言 —— 既有测试文件在本任务禁改清单内。

三条路径均超出本模块写权限，故按现状登记为已知偏差并上报 leader 决策。

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
| 8 | §6 验收未含线程回落 | 常驻池导致 `SameHostSwitchesToShm` 的 `thread_count() <= before` 断言失败（`before=1 after=33`），登记为已知偏差待决策 |
| 9 | §5 阶段无落地状态 | S0/S1/S2/S3 标为已完成（S3 的 tick 迁移除外）；S4 本波不做 |
