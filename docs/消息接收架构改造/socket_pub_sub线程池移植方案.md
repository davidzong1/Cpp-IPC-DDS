# Socket pub-sub 接收线程池移植方案

> 状态：**U0–U3 已实现并验证**（2026-09-27，模块 C）；U4 完成评估，本波**不落地**。
> 实现落点（该模块唯一作者）：`include/dzIPC/socket_pub_sub_ipc.h`、`src/dzIPC/socket_pub_sub_ipc.cc`。
> 依赖的冻结契约：`ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md`
> ＋**必读**勘误 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md`（E1/E2/E3）。
> 范围：socket_sub_ipc 数据接收；socket_pub_ipc discovery_loop 见 §6·U4（仅评估）。
> ⚠️ **本波未达成项（t11 汇总时不得计入已交付）**：① §0·C9 / §6 的数据面 1000 订阅收包（`fd ≥ FD_SETSIZE(1024)` ⇒ abort，**既有缺陷**，归 ipc-transport）；
> ② L3 多热/冷公平性**无专门用例**（测试缺口）；③ §6·U4 的控制面（`discovery_loop`）迁移本波**不落地**。详见 §9。

## 0. 与基准实现的偏差（文档更正点）

本方案起草于阶段 5 接口冻结之前。以下各处**以基准实现为准**更新，均为本波实测后回填：

| # | 方案原写法 | 基准实现（本波据此落地） |
|---|---|---|
| C1 | §3「本分支必须先为 UDPNode 增加安全的可等待句柄及取消等待接口，并封装 SocketWaitSet」 | 等待层已由 ipc-transport 交付并冻结：`include/libipc/udp.h` 的 `waitable()/wait_handle()/cancel_wait()/clear_wait()`、`include/dzIPC/threepools/socket_wait_set.h` 的 `SocketWaitSet`、`include/dzIPC/common/data_rev.h` 的 4 个 `udp_node_*` 转发。本模块**只消费**，不新增任何等待原语，也不出现平台宏。 |
| C2 | §3「Linux 可用 epoll；Windows 使用适配底层 socket 的等价事件机制」 | **勘误 E1**：`wait_handle()` **不改变** fd 的阻塞模式（Linux 返回**阻塞**接收 fd）。就绪后读取一律走 `chunk_rev_topic`/`receive_nowait`；⛔ 禁止对该 fd 用裸 `recvfrom`（会永久挂住 worker 线程，静默停收），⛔ 禁止自行置 `O_NONBLOCK`（会把既有 `receive(invalid_value)` 无限等待退化成紧循环忙轮询）。 |
| C3 | §6·U0「扩展 UDPNode 的可等待句柄与取消等待 API」列在本模块阶段 | 该扩展属 ipc-transport 任务 A。本模块 U0 只做基线测量与回归对照（见 §7 证据表）。 |
| C4 | §5「释放 shared state」未定义 worker 池的生命周期 | 进程级 `SocketReceiveWorkerPool` 是**故意泄漏的指针单例**（与 `RecvWorkerPool`/`LocalPubSubRegistry` 同构）：模块只 `add_route`/`remove_route`，**不** stop 池 —— 否则全局对象析构时的 `remove_route` 会打在已析构的池上。 |
| C5 | 未写 worker 数与调参入口 | 默认 `hardware_concurrency()`，上限 128（实现里的常量名是 `kMaxWorkerCount`，见 `src/dzIPC/threepools/socket_recv_worker.cc:61`；早期草案写作 `kMaxSocketWorkers`，**以代码为准**），`DZIPC_SOCKET_RECV_WORKERS` 覆盖；进程内只读一次。 |
| C6 | §6·U2「保留旧线程作为不支持平台的回退路径和调试比较路径」未给强制开关 | 新增 `DZIPC_SOCKET_COMPAT_THREAD=1`：强制走兼容路径，用于"同一套语义两条路径"对比与无 wait backend 平台的模拟验收。 |
| C7 | §4「热 route 有界让出，冷 route 可调度」未给实现要点 | worker 循环在 deferred 非空时**先做一次 `wait(0)` 全量探测**再 drain。否则热 route 持续让出 ⇒ 循环再也进不到 `wait()`，新到达的冷 route 永远不被发现（实测 `DZIPC_SOCKET_RECV_WORKERS=1` 下冷 topic 30 条只到 1 条）。epoll 是 level-triggered，未读走的数据下一次 `wait(0)` 仍报到 ⇒ 该探测与 `recv_worker.cc` 的 `collect_pending()` 同为**事实来源**，wait-set 的 ready 缓存只是提示。 |
| C8 | §5 未提醒 `running` 闸的重建 | `teardown_receive_path()` 无条件把 `running` 置 false（兼容循环的退出闸），因此 `InitChannel` 在调用它之后**必须**重新置 true。否则首次初始化时兼容线程立刻看到 false 直接退出，订阅端静默收不到任何消息（worker 路径不看 `running`，缺陷只在回退路径暴露）。 |
| C9 | §7「1000 subscriber 的接收线程数固定为 worker 配置」被读作「1000 订阅可测」 | **线程数成立、数据面收包不成立**：订阅可建起（1000 订阅 `fds_after_init=2071`、`max_socket_fd=2069`，建订阅阶段无 abort，`threads_delta=32`），但**收包**在任一订阅 socket 的 fd **号** ≥ `FD_SETSIZE`(1024) 时 abort —— 实测 `scale1000 1 1000` → `max_socket_fd=2069 *** buffer overflow detected *** rc=134`；`scale1000 1 511` → `max_socket_fd=1091` 同样 abort；`scale1000 1 477` → `max_socket_fd=1023` 无 abort，`subs_with_any=429~445/477`（≈90–93%，best-effort 组播不保证每条都到，非本波缺陷）。**判据是 fd 号而不是订阅个数**：同一 478 订阅在 `max_socket_fd=1025`（rc=0）与 fd 号更大（abort）两种起点下都实测到过。根因在任务 A 交付面 `src/libipc/platform/posix/udp.h::UDPNode::receive(tm)` 的**既有** `select()+FD_SET(server_fd)` 定时等待（gdb：`__fdelt_chk` ← `UDPNode::receive` ← `recv_chunk_common_impl` ← `chunk_rev_topic` ← `socket_receive_once`）；改造前基线库同一探针在 511 订阅同样 abort（backtrace 逐帧相同）⇒ **既有限制、非本波引入**，但 board 的「改造后 >510/204 必须可测」第一验收项在 socket 数据面**未达成**。修复落点在 ipc-transport（该文件不在本模块写权限内）：把 `receive(tm)` 的 `select()` 换成 `poll()`（无 FD_SETSIZE 限制）。
**行号更正（t6/t7 实测，t10 回写）**：`src/libipc/platform/posix/udp.h` 的 `FD_SET(server_fd, &read_fds)` 在 **:347**、`::select(server_fd + 1, ...)` 在 **:353**
（任务书/早期草案引用的 326/332 是 **t2 改动前**的 HEAD 行号，已随 t2 改动漂移；win 侧同构：`:327/330`、`:392/398`、`:482/484`、`:592`）。
**归属复核（t10）**：本项**不修**，且**归 ipc-transport**（`src/libipc/platform/*/udp.h` 属共享层交付面，不在任何模块 inScope 内）。 |

## 1. 当前结构与目标

移植前：`socket_sub_ipc::InitChannel` 为每个订阅创建 `subscribe_thread_`。线程调用 `chunk_rev_topic` 收取、组装 UDP 分片，再做 DZFlat/TLV 分流、schema 校验并投递 `msg_queue_` 或 `view_queue_`。`data_rev.cc` 围绕 UDPNode 管理分片与 ACK/NACK 状态。

目标（已达成）：固定数量的 socket ReceiveWorker 等待多个订阅 socket 的可读事件。每个 UDPNode 生命周期内固定归属一个 worker，该 worker 独占调用 `chunk_rev_topic` 并完成消息处理。**没有 per-socket 代理线程，也不 work-stealing。**

## 2. 从 shm_pub_sub_ipc 复用的机制

- 参照 `SubState`，把模板、队列、消息 ID、topic/domain、生命周期标志收进 shared state；worker **不持有** `socket_sub_ipc` 裸指针。
- 参照 `process_received_buffer`，把完整消息组装后的分流与队列投递抽成 `process_received_wire()`，不改 DZFlat/TLV 语义。
- 按阶段 5 的固定 route worker 设计实现稳定分配、单 socket 单消费者、有界预算、ready 扫描和同步 remove。
- 参照 shm_sub_ipc 析构：先注销 nodelet registry，再注销 worker 并等待 in-flight，最后关闭 socket 和释放队列。
- nodelet LocalPubSubRegistry 继续直接投递本地队列，不注册到 UDP wait backend。

## 3. Socket 专属等待层

SHM `recv_wait_set` 等的是共享内存 sequence，不能等待 UDP socket —— 机制不可复用（详见 `socket_wait_set.h` 文件头）。本模块只依赖 ipc-transport 冻结的统一抽象：

- `SocketWaitSet`（每 worker 一实例，不是进程单例；Linux epoll / Windows `WaitForMultipleObjects` 上限 63 路）；
- `SocketWaitToken{owner, handle}` 作为 route 稳定身份，`remove` 同步摘除 + 唤醒，`consume_ready()` 只返回当前在册 handle（fd 复用不误关联）；
- 句柄获取只走 `data_rev.h` 的 `udp_node_waitable/wait_handle/cancel_wait/clear_wait`（`nullptr` 安全）。

⛔ 禁止每 socket 一个代理线程。backend 负责能力探测、add/remove/wait/stop、同轮全部就绪项收集、超时和错误日志。backend 不可用时保留 `subscribe_thread_` 兼容模式，**不以 `receive_nowait` 全量忙轮询降级**。

## 4. 必须保持的行为（逐条落地位置）

- `chunk_rev_topic`、分片组装和每 UDPNode 隐藏状态始终由同一个 worker 调用 —— 归属 = `RecvWorkerPool::worker_for(FNV1a64(route_name‖domain_id) % worker_count)`，生命周期内不变；注册前用 `owner` CAS（`none → worker|compat_thread`）保证单消费者，兼容线程在收时 `add_route` 返回 `busy`。
- ACK/NACK 继续走 `ack_tx_`；数据等待集合**只登记 `subscriber_`**。
- **不缩短**大消息组包超时（`kSocketSubRecvTimeoutMs = 50`，与移植前逐字相同）。三项预算只在"一次完整 `recv_once` 返回"这个安全边界检查。
- DZFlat 去帧独立缓冲、typed Sample 生命周期、schema-less `dzflat_adopt`、TLV owning 路径和拒收计数（`NoteDzFlatRx` 三档）不变 —— 分流代码整段抽出为 `process_received_wire()`，与抽取前逐行同义。
- `msg_queue_`/`view_queue_` 容量与背压不变（两者同容量，各自承接一种 wire）；`reset_message` 在 `state->mtx` 内同步模板快照与分流期望值并 `++generation`。
- publisher `discovery_loop` 非数据接收路径，本波仍每 50ms 扫描，保留"池快照失败时维持上次 `subscribed_` 状态"。

## 5. 注册状态和生命周期

`socket_sub_receive_state`（定义在 `.cc`，头文件只持 `shared_ptr`）保存：

| 类别 | 字段 |
|---|---|
| 稳定 key | `route_name`(topic) / `domain_id` / `port` / `msg_id` |
| 强引用 | `subscriber`、`ack_tx`（worker 持 state ⇒ socket 活到 route 注销之后） |
| 模板快照 | `topic_template` + `exp_msg_id` + `exp_schema_hash`，`mtx` 保护；`generation` |
| 队列 | `msg_queue`、`view_queue`（`shared_ptr`，与实例共享） |
| 生命周期 | `owner`（none/compat_thread/worker）、`active`（route stopping）、`in_flight`、`worker_id` |

析构顺序（`socket_sub_ipc::~socket_sub_ipc` → `teardown_receive_path()`，实现严格按 1–6 步）：

1. 注销 `LocalPubSubRegistry`（持有 `topic_msg_mtx_`）；
2. `active = false`（route stopping，禁止新的 `recv_once`）；
3. worker 模式：`pool.remove_route()` 内部依次 —— 摘除 wait 项并**唤醒**阻塞中的 `wait()` → `cancel_wait()` 打断阻塞中的 UDP receive → **等 `in_flight` 归零**（实现常量名 `kSubQuiesceTimeoutMs = 2000`，见 `src/dzIPC/socket_pub_sub_ipc.cc:36`；早期草案写作 `kSocketQuiesceTimeout`，**以代码为准**；超时打诊断）→ `owner` 归还 `none`；
   兼容模式：`cancel_wait()` → `join` 订阅线程；
4. 关闭 `subscriber_` / `ack_tx_`；
5. 释放 shared state。

**fd/句柄只在 wait 项删除且 in-flight 清零后关闭**（防句柄复用误关联）。`remove_route` 同步返回即"worker 不会再碰这条 route"。重复 `InitChannel` 先按同一顺序回收上一次的接收路径（并发 per-route 窗口计数保证 `run_budget` 整体在窗口内，避免 remove 等到 0 后 worker 又发起下一轮 recv 的 TOCTOU）。

## 6. 执行阶段

### U0：基线和 UDP wait 能力 —— 已完成（能力由 ipc-transport 交付）

线程数基线：改造前 100/300/1000 订阅 ⇒ 100/300/1000 条订阅线程。UDPNode 可等待句柄与 `cancel_wait` 由 ipc-transport 任务 A 交付并冻结；本模块未重复实现。

### U1：SocketWaitSet 与固定 worker —— 已完成（共享层由 ipc-transport 交付）

`SocketWaitSet` / `RecvWorkerPool` / `RecvRegisterStatus` / `RecvBudget` 均为共享层交付物；本模块实现的是**消费侧**：`SocketReceiveWorker`（自有 wait-set + 固定 route 表 + 每 route 预算 + 固定 FIFO deferred）与进程级 `SocketReceiveWorkerPool`。

### U2：接入 socket_sub_ipc —— 已完成

1. 抽取 `process_received_wire()`（TLV 物化 / schema-less adopt / typed view 三条分支，含拒收计数）；
2. 抽取 `socket_receive_once()`（模板 clone → `chunk_rev_topic` → 分流，返回本条字节数）；
3. 建立 `socket_sub_receive_state` 并注册固定 worker（worker 数受上限约束）；
4. 保留兼容 `subscribe_thread_` 作为显式回退与调试比较路径 —— 两条路径**共用**上面的处理函数与 state，语义只有一份。

回退触发条件（任一即回退，且**打印显式原因**，绝不忙轮询）：`SocketWaitSet::backend_available() == false`；`add_route` 返回 `backend_unavailable`/`invalid_token`/`wait_set_full`/`stopped`；线程创建失败；`DZIPC_SOCKET_COMPAT_THREAD=1`。

### U3：协议回归 —— 已完成（证据见 §7）

覆盖：既有 socket 回归全绿（worker 与兼容两模式）、多热 topic 热冷公平、>1MB 分片、动态注销/关闭唤醒、typed view/schema-less/TLV/nodelet 快路径（由既有用例覆盖）、`reset_message` 模板同步、析构并发。

### U4：publisher discovery 评估（本波只评估，不落地）

**现状**：每个 `socket_pub_ipc` 一条 `discovery_thread_`，循环 = `IpcInfoPool::snapshot()` 统计本 topic/domain 的存活 `SocketSub` 条目 → 回填 `subscribed_` → `sleep_cv.wait_for(50ms)`；池异常时保持上次判定。析构 = 置 `running=false` + `notify_all` + `join`。

**可复用设施（阶段 1 已落地）**：`ShmControlScheduler::register_publisher(std::shared_ptr<PubControlState>, ControlTiming)`，其中 `PubControlState::on_pub_heartbeat(now)` 的既定语义就是"owner heartbeat + subscribed_ 刷新 + verbose 状态迁移日志"，`ControlTiming::pub_heartbeat` 默认 50ms —— 与 `discovery_loop` 的循环体逐条对上；`RegistrationToken` 负责析构期同步注销（令牌后析构就是无操作）。因此**迁移路径是现成的**：`socket_pub_ipc` 派生 `PubControlState`，把循环体搬进 `on_pub_heartbeat`，构造期注册、析构期由令牌注销。

**本波不落地的理由（需 leader 决策后再做）**：

1. **调度器 tick 是单线程串行的**：`on_pub_heartbeat` 里的 `IpcInfoPool::snapshot()` 是跨进程共享段读取，其耗时分布本波未测。若某次 snapshot 卡住（段被回收 / 页换出），会连带拖慢**同进程 SHM 腿**的 10ms 订阅心跳 —— 这是跨模块风险，不是 socket 模块内部可以自行决定的事。
2. **回调约束更严**：契约要求回调不得抛出、不得在回调内 `unregister`/`stop`（同线程 `stop` 会 `std::terminate`）。现状 `discovery_loop` 的 `try/catch` 覆盖 snapshot，但迁移后 snapshot 抛异常会隔离掉**整个注册项**（调度器摘除坏项），需要把 catch 粒度重写为"单次 tick 内部消化"。
3. **收益与代价需先量化**：线程数收益 = 每个 socket publisher 减 1 条（1000 个 publisher 时显著）；代价 = 控制面单点。建议落地前先测 1/10/100 个 socket publisher 的 snapshot 耗时 p50/p99，再定是否加"慢项跳过"保护。

**评估结论**：迁移在接口上可行且几乎零新代码，但引入"慢 snapshot 影响 SHM 腿控制面"的跨模块耦合，**本波不落地**；建议作为独立阶段，与 SHM 控制面 owner 一起评审。

## 7. 验收标准与实测证据

> 证据采集：2026-09-27 12:40–13:03（首轮）＋ 13:06–13:11（交接前复跑）＋ 13:31–13:45（终版复跑，读数与首轮逐项一致）：
> `make -j$(nproc)` rc=0；13 项 socket 回归 worker 模式全绿、9 项 compat 模式全绿；
> `thread_probe workers 1000` → `threads_delta=32`、`compat 100` → `100`；`bigmsg 1500000` → `payload_verified=1`；
> `fairness 20`（`DZIPC_SOCKET_RECV_WORKERS=1`）→ `cold_received=20/20 cold_max_gap_ms=100`；`unreg 16` → `8/8×3`；
> `idle_probe workers 100 3` → `idle_cpu_cores=0.0033 nvcsw_delta=1`。
> ⚠️ 终版复跑一律在 `env -u DZIPC_SOCKET_COMPAT_THREAD` 下执行（原因见下方「环境变量坑」）。

| 验收项 | 结果 | 证据（命令 → 关键输出） |
|---|---|---|
| 构建零错误 | ✅ | `cd /home/zwc/cpp_ipc_dds/build && make -j$(nproc)` → `MAKE_RC=0`，`[100%] Built target ...`；`grep -cE 'error:' ` = 0。⚠️ 既有构建竞态（非本模块）：`generate_ipc_messages` 是 `ALL` 目标，每轮 `make` 先 `os.remove` 整个 `include/ipc_msg/**` 再重生成，与并行编译撞车 ⇒ 首批失败偶发 `test/dzflat_benchmark.cpp|test_generated_headers.cpp: fatal error: ipc_msg/std_msgs/*.hpp: 没有那个文件或目录`（失败目标均为既有测试/基准，0 处引用本模块）。连续重跑至生成器写完后即 `rc=0`（本波 try#3 成功；失败目标与本模块无关）。 |
| 既有回归（worker 模式） | ✅ 全绿 | `./bin/test_dzipc_socket`（2/2）、`test_dzipc_pub`（16/16）、`test_socket`(rc=0)、`test_socket_nodelet`（11/11）、`test_socket_borrow`（6/6）、`test_socket_endpoint_split`（18/18）、`test_socket_only_transport`（2/2）、`test_socket_reliable_crc`（4/4）、`test_socket_topic_isolation`（1/1）、`test_dzipc`（8/8）、`test_dzipc_larger_data`（1/1）、`test_nodelet_switch`（8/8）、`test_recv_wait_set`（9/9） |
| 既有回归（兼容回退模式） | ✅ 全绿 | `DZIPC_SOCKET_COMPAT_THREAD=1 ./bin/<同上 9 个 socket 用例>` → 全部 `[ PASSED ]`，并打印 `socket wait-set unavailable (...); keeping per-subscription receive thread` |
| 1000 subscriber 接收线程数受上限约束、无 per-socket 代理线程 | ✅ | `./thread_probe workers 1000`（`ulimit -n 8192`）→ `threads_before=1 threads_after=33 threads_delta=32 nproc=32`；`workers 300` → `delta=32`；`workers 100` → `delta=32`；线程名统计 `33 thread_probe`（**无** per-subscription 命名，33 = 1 主线程 + 32 worker） |
| 对照：兼容路径确实是每订阅一条线程 | ✅ | `DZIPC_SOCKET_COMPAT_THREAD=1 ./thread_probe compat 100` → `threads_delta=100` |
| >1MB 分片 | ✅ | `./accept_probe bigmsg 1500000` → `ack_ok=1 ack_ms=15~26 received=1 received_payload_bytes=1499984 payload_verified=1`；`bigmsg 1600000` → `ack_ms=29 received=1 payload_verified=1`；两次复跑均 `payload_verified=1`；兼容模式对照（`DZIPC_SOCKET_COMPAT_THREAD=1`）同样通过。⚠️ 偶发性：首轮 compat 对照出现 1 次 `ack_ok=1 payload_verified=0 recv_ms=5003`，随后两模式各 3/3 复跑均 `payload_verified=1`（见下） |
| 多热 topic 热冷公平 | ✅ | `DZIPC_SOCKET_RECV_WORKERS=1 ./accept_probe fairness 20`（热/冷同 worker，热 topic 每 200us 灌 512B）→ `hot_sent=7785~7834 cold_sent=20 cold_received=20 cold_max_gap_ms=100`（四次复跑一致，含收尾 13:03 复跑；`cold_max_gap_ms` = 发送间隔本身，无额外延迟、无饿死）。修前旧读数 `cold_received=1/30` |
| 动态注销/关闭唤醒 | ✅ | `./accept_probe unreg 16` → `destroyed=8 teardown_ms=0 survivors=8 survivors_receiving_all_rounds=8`、`unreg 64` → `destroyed=32 teardown_ms=0 survivors_receiving_all_rounds=32`；兼容对照 `teardown_ms=1` 同值 |
| 空闲不忙轮询（线程池 vs 兼容线程） | ✅ | `./idle_probe workers 100 3`（100 订阅、3s 不发数据）→ `threads_delta=32 idle_cpu_cores=0.0000~0.0033 nvcsw_delta=1 nvcsw_per_thread_per_sec=0.01 teardown_ms=1.8~2.0`；`DZIPC_SOCKET_COMPAT_THREAD=1 ./idle_probe compat 100 3` → `threads_delta=100 idle_cpu_cores=0.0133 teardown_ms=13.6`。worker 侧空闲 CPU 为 0（阻塞在 `epoll_wait`，`wait_timeout=100ms` 超时唤醒，**无** `receive_nowait` 全量轮询） |
| worker 线程内不调用用户回调（契约 `recv_worker.h:193` ⛔） | ✅（结构性核对） | 收包路径 `socket_receive_once`/`process_received_wire` 只做 `chunk_rev_topic` + `dzflat_adopt`/`clone` + `CircularQueue::push`：`grep -c 'std::function<' src/dzIPC/socket_pub_sub_ipc.cc` = **0**；未调用 `set_evict_cb`（唯一会把队列回收回调带进 worker 的入口，全文件 `grep -c evict_cb` = 0）；无 `get`/`get_clone`/`sleep`。**socket 侧不存在** shm_ser_cli `SerRequestRoute::recv_once` 内跑用户 callback 那类违约（board delta `e589a6141bf2` 的偏差只登记在 shm_ser_cli） |
| 不支持平台/后端不可用时显式回退，绝不忙轮询 | ✅ | 回退路径打印显式原因；`SocketWaitSet::backend_available()==false` 时池 `ensure_started()` 返回 false，`add_route` → `backend_unavailable`，调用方保留兼容线程；worker 循环的失败/超时分支只 `continue`（wait 超时 100ms，**无** `receive_nowait` 全量轮询） |
| fd 复用/悬挂事件 | ✅（结构性） | `SocketWaitSet::remove` 同步 `epoll_ctl(DEL)` + 独立 eventfd 唤醒；`consume_ready()` 只返回当前在册 handle；本模块 `lookup(token)` 查不到即跳过 |
| 数据面 1000 订阅收包（board 第一验收项） | ❌ **未达成，阻塞在 ipc-transport** | `scale1000 1 1000` → `fds_after_init=2071 max_socket_fd=2069 *** buffer overflow detected *** rc=134`；`scale1000 1 511` → `max_socket_fd=1091` 同样 abort；`scale1000 1 477` → `max_socket_fd=1023` 无 abort（`subs_with_any` = 429–445/477 ≈ 90–93% 与 477/477 = 100% 两档读数均实测到，波动来自 best-effort 组播不保证全达，非本波缺陷）。**abort 判据是订阅 socket 的 fd 号 ≥ `FD_SETSIZE`(1024)，不是订阅个数**（同一 478 订阅在 fd 起点不同时 rc=0 或 abort 均被实测到）；改造前基线库 511 订阅同样 abort（backtrace 逐帧相同）。根因与修复落点见 §0·C9 |

补充观察（非本模块缺陷，供交叉参考）：

- ⚠️ **环境变量坑（验证方必读）**：本波验证 shell 环境里被导出过 `DZIPC_SOCKET_COMPAT_THREAD=1`（`env | grep DZIPC_SOCKET` 可见）。它会让**所有** socket 用例静默走兼容线程路径 ⇒ 线程数读数反转成「每订阅一条」（`thread_probe workers 1000` 会得到 `delta=1000`，并在 fd 越过 1024 时以 `buffer overflow detected` abort），与线程池验收结论**相反**。复现线程池读数必须显式清掉该变量：`env -u DZIPC_SOCKET_COMPAT_THREAD <cmd>`。
- `test_sercli_auto_path` 的 `SerCliAutoPath.SameHostSwitchesToShm` 失败（`before=1 after=33`，析构后线程未回落），已由 ipc-protocol 单独上报 leader；该用例不创建 socket pub/sub，且 `DZIPC_SOCKET_COMPAT_THREAD=1` 下同样失败 ⇒ 与本模块无关（属 `RecvWorkerPool` 常驻 worker 的线程回落口径）。
- 探针 `iso` 的 `ndoubles=200000`（≈1.6MB **BestEffort** 单次发送、不重传）接收失败；用改造前基线库（`build_baseline/lib/libipc.so.1.3.0` + 基线头）编译同一探针同样失败 ⇒ **既有语义**（best-effort 大包在 50ms 组包窗口内收不全即丢弃），改造前后一致；>1MB 的可靠性路径见上表 `bigmsg`（Reliable + ACK）。
- **1000 订阅的线程数验收达成，收包验收未达成**（两条要分开读）：
  - 线程数：`thread_probe workers 1000` → `threads_delta=32`、全程无 abort（探针不触发收包路径）；同一规模下 `scale1000 1 1000` 的建订阅阶段 `fds_after_init=2071` 也无 abort（abort 只在发数据进入 `receive(tm)` 后发生）⇒ 「≥100 subscriber 接收线程数受上限约束、无 per-socket 代理线程」✅。
  - 收包：`scale1000` 一旦发数据，worker 进入 `chunk_rev_topic → UDPNode::receive(50ms) → select()+FD_SET`，订阅 socket 的 **fd 号** ≥ 1024 即 `__fdelt_chk` abort ⇒ 单 topic 实测可收上界受进程 fd 分配影响（fd 号 ≤ 1023 时 477–478 订阅可收、`subs_with_any` ≈ 90%；fd 号越界时 490/500/511/1000 订阅均 abort）。改造前基线库 511 订阅同样 abort（backtrace 逐帧相同），故为**既有**限制；board 的「>510/204 必须可测」需 ipc-transport 把 `UDPNode::receive(tm)` 的 `select()` 换成 `poll()`（详见 §0·C9）。

## 8. 风险

- `chunk_rev_topic` 包含分片组装和协议状态，不是单次非阻塞 recv。预算只能在安全的完整消息边界应用 —— 已实现为"一次 `recv_once` 返回后才检查三项预算"，**不得**在组包中途切走。
- 单次组包占用上界 = `kSocketSubRecvTimeoutMs`(50) × 可能的等 ACK 轮次；`remove_route` 的等待上界 = `kSubQuiesceTimeoutMs`(2000ms，即早期草案的 `kSocketQuiesceTimeout`)，超时只打诊断、不阻塞注销（之后 `cancel_wait` 已让该 node 接收面失效，调用方随即 `close`）。
- 若将来要细粒度公平调度（组包中途让出），应先把 `data_rev.cc` 隐藏的每 UDPNode 重组状态迁移成 route-owned session，再重新验证 ACK/NACK 与超时语义 —— 与本波"不改 `data_rev.cc`"的边界一致。
- Windows 分支（`WaitForMultipleObjects`、WSAEvent 路径）本次**未在 Windows 验证**，只做编译期与接口一致性保证；63 路上限由共享层 `max_channels()` 暴露，超限时 `add_route` 返回 `wait_set_full` 并由本模块显式回退兼容线程。

---

## 9. 未达成项、遗留项与同步边界表（t10 回写）

### 9.1 未达成项（t11 汇总口径）

| ID | 未达成项 | 依据（文档位置 → 实现/实测） | 归属 |
| --- | --- | --- | --- |
| **U-1（=D9/L1）** | **数据面 1000 订阅收包未达成**：任一订阅 socket 的 **fd 号** ≥ `FD_SETSIZE`(1024) 即 `__fdelt_chk` abort（本机有数据流时可用上界 ≈477 订阅者，`max_socket_fd ≤ 1023`）。**线程数**一项成立（`threads_max ≡ 33`），**收包**一项不成立 —— 两条必须分开读 | 本文 §0·C9、§7 证据表末行；t6 报告 §0（追加B）、§6 | **既有缺陷**，归 ipc-transport（`select()`→`poll()`），**本波不修** |
| **U-2** | 多热/冷公平性**无专门用例**（仅有 `test_recv_worker`/`test_socket_recv_worker` 的 deferred/预算让路用例间接覆盖） | t6 报告 §8「L3」 | 测试（下一波），与本波放行解耦 |
| **U-3** | 控制面 `socket_pub_ipc::discovery_loop` 迁移（§6·U4）**本波不落地** | 本文 §6·U4 三条理由 | 独立阶段，需与 SHM 控制面 owner 一起评审 |
| **U-4** | Windows 分支（`WaitForMultipleObjects` / WSAEvent）**未在 Windows 验证** | 本文 §8 末条；契约 §7 | 声明性限制（非缺陷）；只做编译期与接口一致性保证 |

### 9.2 补充观察（不构成本模块缺陷，但复核方必读）

| ID | 观察 | 依据 | 处置 |
| --- | --- | --- | --- |
| L2 | 默认 `worker_count = hardware_concurrency()` ⇒ **单个订阅者即拉起本机 32 个 worker** | t6 报告 §8「L2」；共享层 `DZIPC_SOCKET_RECV_WORKERS` 进程内只读一次 | 配置观察（非缺陷）；小规模部署建议显式设 `DZIPC_SOCKET_RECV_WORKERS`（如 4）。池空闲退出后线程会归还 |
| L4 | nodelet 模式走兼容线程 | 契约 §5 / captain 裁定 C7；t6 报告 §2.3（`socket_nodelet` 13 条 fallback 行） | **既定设计**，不是回退失败 |
| L5 | `build_baseline/bin/baseline_probe_after` ABI 陈旧，用它测**当前**库会得到假缺陷（`sysmalloc Assertion` 堆损坏） | t6 报告 §10 第 2 条 | ⛔ 测当前库必须按当前头文件重编探针；该二进制只对基线库有效 |
| R-4 | `large_msg_cache` 口径不一致：`include/libipc/def.h:49` = **40**，而 `src/libipc/ipc.cpp:461` / `include/dzIPC/common/nodelet_config.h:52` 注释写作 **32** | t7 报告 §5「C-05」；t8 报告 §1「C-05」 | **既有**文档不一致，非本波引入；**以代码为准（40）**；落点属 ipc-transport/文档侧 |

### 9.3 worker 路径与兼容路径的**唯一机制差异**（口径，防误判）

| 差异点 | worker 路径 | 兼容路径 | 为什么必须不同 |
| --- | --- | --- | --- |
| 可读判据 `udp_node_readable()` | **先判可读**，无数据立即 `return 0` | **不判**，保持阻塞 `chunk_rev_topic(tm=50ms)` | 兼容路径若加不可读即返回的判据，循环会退化成**忙轮询**（阶段 5 第一红线） |
| 收包原语调用次数 | 每次 `recv_once()` 只调一次 `chunk_rev_topic`，到**一条完整消息**边界返回 | 循环内连续调用，阻塞在 50ms 组包窗口 | 预算只在"一次完整 `recv_once` 返回"后由**共享 worker** 检查，⛔ 不得在组包中途切走 |
| 进程内快路径队列 | **不注册** `LocalPubSubRegistry` | 注册（nodelet 快路径在这里） | C7：`RecvWorkerPool` 未转发 `wakeup()`，worker 阻塞在 wait-set 时无法被"队列有新消息"叫醒 ⇒ 注册了会让消息投进没有消费者的队列而静默挂起 |
| 停机顺序（C-06 修复后） | `remove_route`（内部摘 wait 项并唤醒） | `stopping=true` **与 `running=false` 同处** → `udp_node_cancel_wait` → join | 消除"`cancel_wait` 与 `running=false` 之间再进一次 `chunk_rev_topic`"的窗口 |

### 9.4 同步边界表（P4 / 防复发 · 必填）

> **由来**：t7 finding **C-02** 的根因就是 `receive_state_` **未列入任何收尾清单**（同 C-01，只是暴露面略小）。
> 本节把新引入的同步面逐条登记；新增成员若未进本表，视为同类风险。

| # | 对象 / 成员 | 谁写 | 谁读 | 同步原语 / 约束（实测锚点） |
| --- | --- | --- | --- | --- |
| B-1 | `socket_sub_ipc::receive_state_` | `InitChannel` / `start_receive_path` / `teardown_receive_path` | `reset_message`、处理/兼容线程 | **`receive_state_mtx_` + 三个访问口**（`current_/set_/clear_receive_state()`）；业务代码**零**直接成员访问（修后仅访问口内 3 处） |
| B-2 | `socket_sub_receive_state::owner` | 共享 worker `add_route` / 兼容线程 claim / `remove_route` 第 6 步 | 三方 | `std::atomic<RecvOwner>` CAS（契约 §4.6 单消费者互斥；实测 `src/dzIPC/socket_pub_sub_ipc.cc:57`） |
| B-3 | `receive_route_` / `worker_mode_` / `subscribe_thread_` | `InitChannel` / `start_receive_path` / `teardown_receive_path` | 同上三个入口 | **不加锁**，依赖 API 约束：调用方必须串行化 `InitChannel` / `stop_data_plane`（auto 层由 `leg_mtx_` 保证）；头注释已写明该约束 |
| B-4 | `state->subscriber` / `state->ack_tx`（强引用） | `start_receive_path`（建 state 时） | worker `recv_once` | worker 侧 entry 持 `shared_ptr` ⇒ socket 活到 route 注销之后；**fd/句柄只在 wait 项删除且 `recv_in_flight` 清零后**才由 `close()` 关闭（防句柄复用误关联） |
| B-5 | `state->msg_queue` / `state->view_queue` | worker `process_received_wire()` 入队 | 用户 `get*` / `try_get*` | `CircularQueue` 自身同步；**worker 内不调用 `set_evict_cb`**（唯一会把回收回调带进 worker 的入口，实测 `grep -c evict_cb` = 0） |

> 锁序（t8 §3.2 已核对）：**成员锁（取完快照即释放）→ `state->mtx`**，两组绝不交叉持有。

### 9.5 注释与调用点机械核对（P5 / 防复发）

> **根因**：C-03/C-04 的本质是「**注释抄了语义、函数体继承了旧实现**」；同类风险在本模块表现为
> "注释声称走兼容线程会 claim"，因此同样适用「注释旁必须给出可 grep 的调用点证据」。

| 注释声明 | 机械判据（命令） | 期望 |
| --- | --- | --- |
| 兼容线程会 claim `compat_thread` | `grep -n "compat_thread" src/dzIPC/socket_pub_sub_ipc.cc` | **≥ 2**（claim 调用点 + 日志/名字表）—— 修前为 1（仅注释） |
| worker 只在完整消息边界让出 | `grep -n "gate_on_readiness" src/dzIPC/socket_pub_sub_ipc.cc` | 命中定义、worker 调用（`true`）与兼容调用（`false`）三处 |
| 读路径不碰 E1 红线 | `grep -n "recvfrom\|O_NONBLOCK\|FIONBIO" src/dzIPC/socket_pub_sub_ipc.cc` | **0 调用**（仅既有 `<fcntl.h>` include 不算） |
| 不调用池 `stop` | `grep -rn "Pool::instance().stop" src/dzIPC/*.cc` | **0** |

### 9.6 验收纪律（A8，强制）

```bash
cd /home/zwc/cpp_ipc_dds && cmake -S . -B build && make -C build -j$(nproc)
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS ctest --output-on-failure
env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS ./bin/test_dzipc_socket
```

判据（三条同时成立才算 worker 路径生效）：① 无 `socket wait-set unusable (...)` 回退行；
② `test_dzipc_socket` 与 `test_dzipc` rc=0、`[ FAILED ]` = 0；③（可选，需 `verbose`）出现 `subscribe receive on shared socket worker <N>` 证据行。
⛔ 两个环境变量都必须显式清空：`DZIPC_SOCKET_COMPAT_THREAD` 残留会让读数反转，`DZIPC_SOCKET_RECV_WORKERS` 被共享层 static 只读一次、残留会**静默**改写线程数口径。
⛔ 任何验收前必须重跑 CMake 配置再重编（`aux_source_directory`/`file(GLOB)` 是配置期展开；build/ 曾残留已消失实现的符号 ⇒ 旧产物假绿）。
