# 交叉 Review 报告（t7）—— 三模块移植（shm_ser_cli / socket_pub_sub / socket_ser_cli）

> 任务：t7【交叉Review】三模块逐行审查；owner：ipc-review-security；attempt_id：3a83b019-be17-4f0e-9e86-08d2e2cbd060。
> 性质：**只读审查**，未修改任何产品源码（修复归 t8）。本文是 t7 的唯一交付物。
> 基线：`HEAD = 1d38512`「wip(threadpool): t3 收口 —— shm_ser_cli 服务端请求接收接入固定 SHM RecvWorker（三模块移植全部完成）」，工作区对三个模块文件无未提交改动（`git status --porcelain` 仅剩 `Testing/`、`test/perf/`、`tmp/` 等过程产物）。
> 取证：2026-09-27 22:3x–23:0x CST；引用统一用工作区 `docs/消息接收架构改造/` 路径。
> 纪律：一切读数取自**本次重跑 cmake + 重编**后的产物（A8），并显式 `env -u DZIPC_SOCKET_COMPAT_THREAD`。

## 0. 结论摘要（可判定）

**verdict = needs_revision（不通过）**：无 blocker 级设计违约（裁决 D1「callback 不入 worker」三模块均已落实），但存在 **1 条 high 级并发缺陷（本波新引入）+ 5 条 medium + 4 条 low**，需要 t8 修复后由 t9 复审。

| ID | 严重级 | 位置 | 一句话 |
|---|---|---|---|
| **C-01** | **high** | `src/dzIPC/socket_ser_cli_ipc.cc:397/412/842/952` | `receive_state_`（`std::shared_ptr`）被 `restart_data_plane()` 写、被 `reset_message()/reset_callback()/process_thread_func()` 读，**无任何锁** ⇒ 数据竞争（UB） |
| C-02 | medium | `src/dzIPC/socket_pub_sub_ipc.cc:957/1040` | 同类问题（`InitChannel` 写 / `reset_message` 读 `receive_state_`） |
| C-03 | medium | `src/dzIPC/socket_ser_cli_ipc.cc:786-829` | 兼容 `response_thread_func` **完全没有** `try_claim_recv(compat_thread)` 调用点，与契约 §4.6 不符 |
| C-04 | medium | `src/dzIPC/shm_ser_cli_ipc.cc:810-840` + `include/dzIPC/shm_ser_cli_ipc.h:86-89` | 回退路径 `req_route_` 未建 ⇒ 兼容线程的 claim/release 被整段跳过；且头注释声称"两条路径都会建好 req_route_"与实现不符 |
| C-05 | medium | `src/dzIPC/shm_ser_cli_ipc.cc:109-110` | 新增有界 FIFO 上限 64 条 / **8 MiB**，是改造前不存在的 chunk 持有窗口（SHM chunk 池每尺寸档仅 32 块）⇒ 可能放大 Publisher 端 DZFlat 回退；缺实测量化 |
| B-01 | medium | `shm_ser_cli线程池移植方案.md:34` vs `src/dzIPC/shm_ser_cli_ipc.cc:202-267` | 方案正文仍写 `recv_once` 含 callback/响应发送（代码已按裁决 D1 修正）⇒ 文档未回写；且方案 §7"数据接收线程数受 worker 上限约束"的**收益本波未达成**（per-server 线程数不变） |
| C-06 | low | `src/dzIPC/socket_pub_sub_ipc.cc:1156-1185` | 兼容模式停机顺序与方案 §5 表述不一致（`stopping=true` 后、`running=false` 前存在短暂紧循环窗口，已达 `stopping` 短路，非忙轮询但顺序可收紧） |
| C-07 | low | `src/dzIPC/shm_ser_cli_ipc.cc:290-297`、`src/dzIPC/socket_pub_sub_ipc.cc:326-332`、`src/dzIPC/socket_ser_cli_ipc.cc:313-319` | 三处 `wait_quiescent()` 超时静默（`wait_for` 返回值未检查、未打诊断），与契约"超时只打诊断"不符 |
| C-08 | low | `src/dzIPC/shm_ser_cli_ipc.cc:676-679`、`src/dzIPC/socket_pub_sub_ipc.cc:1049-1052` | 兼容线程 claim 失败时**静默 return**（无日志）⇒ "无人消费"无诊断面 |
| B-02 | low | 三模块常量名 | 名字与方案文档不一致（`kSocketQuiesceTimeout`/`kMaxSocketWorkers` vs 代码 `kSubQuiesceTimeoutMs`/`kSerQuiesceTimeoutMs`/`kMaxWorkerCount`）；**取值全部正确**（2000 / 128），属命名与文档回写问题 |

**维度小结**：A（方案符合性）= 基本符合，落地位置一致、只消费共享层，唯 B-01 为文档/验收缺口；B（配置与硬编码）= 常量取值逐条正确、两个开关真实生效且同名同义、回退条件齐备且有显式日志、**无忙轮询降级**，扣分点在 C-05 与 B-02；C（线程安全与资源释放）= 单消费者/裸指针/注销顺序/析构序/fork 闸均达标，扣分点在 C-01/C-02（新引入竞争）、C-03/C-04（claim 协议缺口）、C-07/C-08（诊断面）。

---

## 1. 取证纪律与本次构建读数（A8）

```
cmake -S . -B build                 -> CMAKE_RC=0
make -C build -j$(nproc)            -> MAKE_RC=0；grep -c "error:" = 0
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD ctest --output-on-failure
                                    -> CTEST_RC=0；12/12 Passed（28.9s）
```

12 条用例：`test_udp_port_boundary` / `test_shm_control_scheduler` / `test_shm_route_session` / `test_wakeup_artifact` / `test_shm_i5_pop_buffer` / `test_shm_sub_dtor_gate` / `test_shm_ready_transition` / `test_recv_wait_set` / `test_recv_worker` / `test_socket_wait_set` / `test_socket_recv_worker` / `test_socket_readable`。

模块级 worker 路径实证（stderr 原文，`env -u DZIPC_SOCKET_COMPAT_THREAD`）：

```
$ ./bin/test_dzipc_shm
[request_response_test_SerInfo] request receive on shared SHM worker 20 (generation 1, workers 32)   # t3
$ ./bin/test_dzipc_socket
[request_response_testSerInfo] request receive on shared socket worker 12 (generation 1, workers 32)  # t5
[TestMsg2SubInfo] subscribe receive on shared socket worker 25 (generation 1, workers 32)             # t4
$ ./bin/test_dzipc   -> [  PASSED  ] 8 tests.  （同样命中两条 socket worker 日志）
$ ./bin/test_dzipc_shm|test_dzipc_socket|test_sercli_auto_path  -> rc=0/0/0，FAILED 计数 = 0/0/0
```

兼容开关实证：`DZIPC_SOCKET_COMPAT_THREAD=1 ./bin/test_dzipc_socket` → rc=0，且打印 2 条 `socket wait-set unusable (...); keeping per-subscription receive thread` ⇒ 开关真实生效（t4/t5 同名同义）。

---

## 2. 维度 A｜方案符合性（逐条）

### A.1 落地位置与写权限（核对方法：`git show --stat` 逐提交看改动面）

| 提交 | 改动文件 | 判断 |
|---|---|---|
| `1d38512` t3 | `include/dzIPC/shm_ser_cli_ipc.h`(+61)、`src/dzIPC/shm_ser_cli_ipc.cc`(+787/-78) | ✅ 只改自己的两文件 |
| `cd5b427` t4 | `include/dzIPC/socket_pub_sub_ipc.h`(+43)、`src/dzIPC/socket_pub_sub_ipc.cc`(+610/-118) | ✅ 只改自己的两文件 |
| `cbce11c` t13（t5 的裁定 C 补丁） | 仅 `src/dzIPC/socket_ser_cli_ipc.cc`(+24/-21) | ✅ 只改自己的文件 |
| `git diff --stat cd5b427 HEAD -- src/dzIPC/threepools src/libipc include/libipc include/dzIPC/common src/dzIPC/common` | **空** | ✅ 三个模块 owner 未碰共享层 |

> 说明：`36e4a33`（t12，owner=ipc-transport，裁定 B/C 追加 `readable()`/`udp_node_readable`/`out_bytes`）确实改了共享层——那是**共享层唯一 owner 的既定交付面**，不计为跨模块耦合。

### A.2 跨模块耦合（核对方法：include 面 + 平台原语检索）

```
$ grep -n "shm_ser_cli_ipc.h|socket_pub_sub_ipc.h|socket_ser_cli_ipc.h" src/dzIPC/shm_ser_cli_ipc.cc src/dzIPC/socket_pub_sub_ipc.cc src/dzIPC/socket_ser_cli_ipc.cc
   -> 每个 .cc 只 include 自己的头（三处，无互相 include）
$ grep -nE "epoll|WaitForMultiple|eventfd|WSAEvent|futex" <三个模块的 .h/.cc>
   -> 空（模块内无自造等待原语、无复制 RecvWorker/SocketWaitSet）
$ grep -n "Pool::instance().stop|pool.stop" <三个模块>   -> rc=1（不调用池的 stop，符合"池随进程存活"）
```
✅ 无跨模块耦合。

### A.3 必须保持的行为（逐条对照基准实现 `shm_pub_sub_ipc` / HEAD）

| 行为 | 基准锚点 | 三模块落地 | 结论 |
|---|---|---|---|
| 唤醒伪影门在**每条**收包路径上（B11） | `shm_pub_sub_ipc.cc:67-71`（门在分流函数内，判据整段全零 `wire_accept.cc:104-110`） | t3：门在 `process_request()` 内（`shm_ser_cli_ipc.cc:963-971`），**兼容路径与 worker 路径共用该函数**；worker 侧 `recv_once():234-238` 再挡一次 | ✅ 两条路径都过门 |
| DZFlat/TLV 分流与三档计数 | `wire_accept.h` / `NoteDzFlatRx` | t3 `classify_received`+`accept_wire` 原样调用（`:985-1005`）；t4 `process_received_wire()` 三支 kDzFlatIdSkipped/SchemaDrop/Accepted 与抽取前逐行同义（`:162-224`） | ✅ |
| `msg_id`/schema 校验、`AcceptWire()` | `wire_accept.cc:31-81` | 三模块均复用共享实现，未自写校验 | ✅ |
| chunk 生命周期与 adopt 配额 | 需求 §1.2 第二句 / §7 | t3 FIFO 存 `ipc::buffer`（chunk 由 libipc 池承担，buffer 析构即回收，未引入第二套记账）；t4 借样 `Sample(std::move(wire))` 与原实现一致 | ⚠️ 见 C-05（FIFO 引入新的持有窗口，但**未**新增记账/复制） |
| `kSocketSubRecvTimeoutMs = 50` 不得缩短 | 方案 §4 | `socket_pub_sub_ipc.cc:34` 常量 + `:267` 传参，**未改** | ✅ |
| `ServerRevTime = 200ms` 组包边界 | `socket_ser_cli_ipc.cc:20`（HEAD 同值） | 常量原样，`:247` 传入 `chunk_rev_server`，预算只在完整请求边界 | ✅ |
| 单 route 单消费者、不 work-stealing | 契约 §4.6 | 三模块 owner CAS + 固定归属；无一个模块自建 route→worker 映射 | ✅（claim 协议缺口见 C-03/C-04） |
| nodelet 快路径与 worker 二选一（裁决 C7） | 契约 §5 第 386 行 + captain 裁定 | t3 `start_data_plane:808-811` 与 `InitChannel:592-600`；t4 `start_receive_path:1101-1105` 与 `InitChannel:1063-1074` | ✅ 均"nodelet ⇒ 兼容线程 + worker 模式不注册进程内队列" |
| 控制面线程未迁移如实登记 | 方案 §3/§8 | t3 `ser_handshake` 仍独立线程、50ms/2s 阈值不变（`:615-665`，`:630` 2e9ns）；t4 `discovery_loop` 仍 50ms | ✅ 代码与方案自述一致（未达成的收益见 B-01） |

### A.4 文档 vs 基准实现冲突处是否以基准为准

- **B11（门落点）**：旧交接档建议"留在收包循环、只扫尾 12 字节"，实现是"门在分流函数内、整段全零"。三模块**以实现为准**（t3 复用 `process_request` 的整段判据；t4/t5 无 SHM 语义不适用）✅。
- **A6（方案漏 9 接口）**：三模块适配器均**照 `recv_worker.h`/`socket_recv_worker.h` 实现**，未照方案清单（t3 `SerRequestRoute` 5 方法 + `has_pending`；t4 `socket_sub_receive_route` 10 虚函数；t5 `socket_ser_request_route` 10 虚函数）✅。
- **D1（callback 不得进 `recv_once`）**：✅ **三模块全部落实**——
  - t3：`recv_once():202-267` 只做 `try_recv()` + 伪影门 + 入 FIFO；callback/响应发送在 `process_request():955-1048`（独立处理线程 `process_thread_func():1087-1114`）；
  - t4：收包路径只做 `chunk_rev_topic` + 分流 + 入队（`:230-286`），模块内 `std::function` 用户回调 0 处；
  - t5：`recv_once():206-268` 只做 readable 判据 + `chunk_rev_server` + 入队；callback/响应在 `process_thread_func():840-884`。
  ⇒ **若哪个模块把 callback 放进 `recv_once()` 即 blocker** 的判据未被触发。

**维度 A 结论**：符合。扣分项：B-01（文档未回写 + 一项验收收益未达成）。

---

## 3. 维度 B｜遗漏配置项与硬编码

### B.1 常量逐条核对（核对方法：`grep -n` 每个量 + 读调用点）

| 契约/方案要求 | 实测值 | 位置 | 结论 |
|---|---|---|---|
| 预算 32 条 / 1 MiB / 200 us / wait 100 ms | 三模块**均未覆写** `RecvBudget`（`grep RecvBudget` 无自定义实例）⇒ 用共享层默认 `recv_worker.h:242-250` 的 32 / 1 MiB / 200 us / 100 ms / `idle_keep_alive=1000ms` | — | ✅ 与契约 §4.2 逐字一致 |
| `kSocketSubRecvTimeoutMs = 50` | `constexpr std::uint64_t kSocketSubRecvTimeoutMs = 50;` | `socket_pub_sub_ipc.cc:34` | ✅（方案原写常量名 `kSocketSubRecvTimeoutMs`，代码同名） |
| `kSocketQuiesceTimeout = 2000` | t4 `kSubQuiesceTimeoutMs=2000`（`:36`）、t5 `kSerQuiesceTimeoutMs=2000`（`:55`）、t3 `kSerQuiesceTimeoutMs=2000`（`:189`） | 见左 | ✅ 取值正确；⚠️ 命名与方案文档不一致 ⇒ **B-02** |
| `kMaxSocketWorkers = 128` | `socket_recv_worker.cc:61` `kMaxWorkerCount = 128`，`:852` 截断 | 共享层 | ✅ 取值正确；⚠️ 名称为 `kMaxWorkerCount` ⇒ B-02 |
| Windows 63 路 | `socket_wait_set.cc:31` `kMaxChannels = 63`（`_WIN32` 分支） | 共享层 | ✅（本机 Linux 未验证，契约 §6 已声明） |
| worker 数默认 `hardware_concurrency()` | `socket_recv_worker.cc:849`；实测日志 `workers 32`（本机 nproc=32） | 共享层 | ✅ |
| 阻塞 fd 语义（勘误 E1） | 三模块读路径均走 `chunk_rev_topic` / `chunk_rev_server` / `try_recv`，`grep recvfrom\|O_NONBLOCK\|FIONBIO\|fcntl` 在 socket 两模块 = **0 命中**（`socket_ser_cli_ipc.cc:2` 的 `<fcntl.h>` 是 HEAD 既有 include，无调用） | — | ✅ 未触碰 E1 红线 |

**魔数散落检查**：三模块内新增的时限常量**只有**上述三个 `2000` 与既有 `ServerRevTime=200`，均具名 `constexpr`/`#define`；未发现裸字面量散落在调用点（`chunk_rev_topic(..., 50, ...)`、`chunk_rev_server(..., ServerRevTime, ...)` 均走常量）。

### B.2 开关变量（真实生效性）

| 开关 | 实现 | 只读一次？ | 同名同义？ | 证据 |
|---|---|---|---|---|
| `DZIPC_SOCKET_COMPAT_THREAD` | t4 `sub_compat_forced()`（`socket_pub_sub_ipc.cc:94-105`）、t5 `socket_recv_compat_forced()`（`socket_ser_cli_ipc.cc:128-139`），均为函数内 `static const bool` 初始化一次 | ✅ | ✅ 两者都判 `v[0]=='0' && v[1]=='\0'` 才算 false | `DZIPC_SOCKET_COMPAT_THREAD=1 ./bin/test_dzipc_socket` → rc=0 + 2 条 `socket wait-set unusable (DZIPC_SOCKET_COMPAT_THREAD=1); keeping per-subscription receive thread` |
| `DZIPC_SOCKET_RECV_WORKERS` | **只在共享层** `socket_recv_worker.cc:80-96` 的 `env_worker_count()`（`static const` 缓存） | ✅ | ⚠️ 模块不读（按裁定"只由共享池读一次"，正确） | 读代码 + 裁定一致 |

⚠️ **环境变量坑（复核方式）**：若验证 shell 残留 `DZIPC_SOCKET_COMPAT_THREAD`，socket 用例会静默全走兼容线程、读数反转。本报告所有读数均在 `env -u DZIPC_SOCKET_COMPAT_THREAD` 下取得；`env | grep -i DZIPC` 本次为空。复核命令：`env -u DZIPC_SOCKET_COMPAT_THREAD <cmd>`。

### B.3 回退路径（触发条件齐备性 + 显式原因）

| 模块 | 触发条件（实测代码） | 是否打印显式原因 |
|---|---|---|
| t3 shm_ser | nodelet（`shm_ser_cli_ipc.cc:808-811`）、fork pid 闸（`:812-815`）、`RecvWorkerPool::backend_available()==false`（`:816-819`）、`invalid_token`（`:822-825`）、`pool.start()` 失败（`:828-835`）、`add_route` 任何非 ok（`:836-840`，经 `ser_register_status_reason():753-775`） | ✅ `fallback()` lambda `:790-794` 打印 `shm recv worker unusable (<why>); keeping per-server receive thread` |
| t4 socket_sub | `DZIPC_SOCKET_COMPAT_THREAD=1`（`:1097-1100`）、nodelet（`:1101-1105`）、fork pid 闸（`:1106-1109`）、`backend_available()==false`（`:1110-1113`）、state 缺失（`:1114-1117`）、`wait_token` 无效（`:1119-1123`）、pool start 失败（`:1124-1132`）、`add_route` 非 ok（`:1133-1137`） | ✅ `fallback()` `:1091-1095` 打印 `socket wait-set unusable (<why>); keeping per-subscription receive thread` |
| t5 socket_ser | 同上（`socket_ser_cli_ipc.cc:890-894` + `:896-951`） | ✅ `fallback()` `:890-894` 打印 `socket wait-set unusable (<why>); keeping per-service receive thread` |

**忙轮询降级检查（阶段 5 红线段）**：

```
$ grep -n "receive_nowait|try_recv(" <三个模块>
  shm_ser_cli_ipc.cc:228  ipc::buffer raw = state_->req_ch->try_recv();   <- worker 的 recv_once（契约要求"非阻塞或短超时"）
  (socket 两模块：0 命中)
```

- t3 的兼容路径保持**阻塞** `ipc_r_ptr_->recv(50)`（`shm_ser_cli_ipc.cc:697`），注释明写"⛔ 不加可读判据，否则循环退化成忙轮询"✅
- t4 兼容路径 `socket_sub_receive_once(state, false)`（`gate_on_readiness=false`）⇒ 仍是 `chunk_rev_topic(tm=50ms)` 阻塞语义，**不带**可读判据（`:1055`）✅
- t5 兼容路径 `chunk_rev_server(..., ServerRevTime, ...)` 阻塞语义（`:803`）✅
- worker 路径均先做**非阻塞**可读判据（t4 `:236`、t5 `:220`）⇒ 消除空读阻塞（t4 实测 OLD 50.062ms → NEW 0.0016ms）✅

**维度 B 结论**：常量与开关全部正确、回退条件齐备且**逐条打印显式原因**、无忙轮询降级。扣分项：C-05（新增 FIFO 容量缺实测量化，见维度 C）、B-02（命名与文档不一致）。

---

## 4. 维度 C｜线程安全与资源释放

### C.0 逐条核对（无 finding 的项也写明核对方法）

| 检查项 | 核对方法 | 结论 |
|---|---|---|
| 单消费者归属（owner CAS / 固定归属） | 读 `try_claim_recv/release_recv/recv_owner` 三方法 + `add_route` 调用点 | ✅ 三模块均是 `std::atomic<RecvOwner>` CAS；固定归属由共享池 `worker_for()` 计算，模块未自建映射 |
| work-stealing 违规 | 搜模块内是否自行把同一条 route 交给不同 worker | ✅ 无；route 在注册期内不迁移 |
| worker 不持宿主裸指针 | 看 `state`/`route` 持有的成员类型 | ✅ t3 `SerRequestRoute` 只持 `shared_ptr<SerState>`；t4 `socket_sub_receive_route` 只持 `shared_ptr<socket_sub_receive_state>`；t5 同（`socket_ser_request_route`） |
| `shared_ptr` 生命周期足够（注销后不回调已析构对象） | 看 `remove_route` 后是否仍有持有者 | ✅ worker 侧 entry 持 `shared_ptr<RecvRouteSource>`；模块注销后重置自己的 `route`/`state`；`remove_route` 同步返回 |
| 注销同步性（摘 wait 项并唤醒 → cancel_wait → 等 in_flight → 关 fd） | 读 `teardown/stop_data_plane` + 共享层 `remove_route` 六步 | ✅ 顺序正确（详见 C.1） |
| 析构/stop/restart/generation 重建无永久等待 | 读三模块析构 + t5 `restart_data_plane()` + HEAD 对照 | ✅ 有界（`connect_with_retry` 可被 `running` 打断；join 前已 cancel_wait/remove_route） |
| 悬挂 wait 项 / fd 复用误关联 | 看 `remove_route` 与 `close` 的先后 | ✅ fd 只在 `remove_route` 返回后由 `close_data_plane()` 关闭；t5 restart 一律新建 `UDPNode`（不复用旧 fd） |
| 回调内不得 unregister/stop/抛出 | t3 `process_request():1017-1045` 捕获 callback 异常并计数；t4 无用户 callback；t5 `process_thread_func():872-875` | ⚠️ t4 无回调（N/A）；t3/t5 异常隔离到位；"callback 内调 unregister/stop" 属调用方契约，模块未额外设防（与基准一致，不计 finding） |
| **fork 防死锁闸（必查）** | 读 `ser_recv_pool_owner_pid`/`sub_pool_owner_pid`/`pool_owner_pid` 与 `add_route` 前置判定 | ✅ **三模块全部保留**（详见 C.2） |
| 池生命周期（模块不得调池的 stop） | `grep "Pool::instance().stop"` | ✅ 0 命中 |
| 空闲退出口径（不得写成不可达成） | 读模块注释 + `idle_keep_alive` 是否被覆写 | ⚠️ t3 文件头 `:61-69` 明确把"收包线程数不随 server 数线性增长"登记为**本波未达成**（诚实、可判定）；见 B-01 |

### C.1 注销同步性（逐模块）

**t3 shm_ser**（`shm_ser_cli_ipc.cc:851-914`）：`stopping=true` → `remove_route`（内含契约 1-6 步）→ 处理线程 join → 兼容路径显式 `release_recv()` → FIFO 残余作废并记 `requests_dropped` → 重置 `req_route_/ser_state_`。析构 `:511-549` 顺序：registry 注销 → `stop_data_plane()` → join handshake → `clear` 通道 ✅

**t4 socket_sub**（`:1156-1194`）：`stopping=true` → worker 模式 `remove_route` / 兼容模式 `udp_node_cancel_wait` → `running=false` + join → `release_recv`（幂等）→ reset ✅；析构 `:898-924` 先 registry 注销再 `teardown_receive_path()` 再 `close()` 节点 ✅

**t5 socket_ser**（`:967-1007`）：worker 模式 `remove_route` / 兼容模式 `cancel_wait` → join 处理线程 → 队列作废计数 → reset ✅；`restart_data_plane():605-632` 严格"完全注销旧 generation → 关旧节点 → 建连新节点 → 注册新 generation → **最后**置 `data_plane_running_`" ✅（与裁定 ②⑥ 一致）

### C.2 fork 防死锁闸（必查项，逐模块）

| 模块 | 实现 | 是否触碰池锁 | 是否回退兼容线程 |
|---|---|---|---|
| t3 | `ser_recv_pool_owner_pid()`（`:735-746`，只取本文件 `static std::mutex`）+ `ser_recv_pool_allowed_in_this_process()`（`:748-751`）；`start_data_plane():812-815` 不匹配即 `fallback("forked child: recv pool owner pid mismatch")` | ✅ 只碰本文件静态锁 | ✅ |
| t4 | `sub_pool_owner_pid()`（`:74-85`）+ `sub_pool_allowed_in_this_process()`（`:87-90`）；`start_receive_path():1106-1109` 同样回退 | ✅ | ✅ |
| t5 | `pool_owner_pid()`（`:108-119`）+ `pool_allowed_in_this_process()`（`:121-124`）；`start_receive_path():900-904` 同样回退 | ✅ | ✅ |

三处均为"首次调用把本进程 pid 记为 owner"，fork 后子进程 `getpid()` 变化 ⇒ 判定不等 ⇒ **不调 `add_route`**。与 `recv_worker.h:112-118` 的约束一致 ✅。

---

## 5. Findings（t8 的修复输入；每条含最小复现）

### C-01 ｜ high ｜ `receive_state_` 无锁读写在并发路径上（socket_ser_cli）

- **文件:行**：`src/dzIPC/socket_ser_cli_ipc.cc:397`（读）、`:412`（读）、`:842`（读）、`:952`（写）、`:1004-1005`（写/reset）
- **问题**：`receive_state_` 是裸 `std::shared_ptr<socket_ser_receive_state>` 成员（头文件 `include/dzIPC/socket_ser_cli_ipc.h:118`）。`receive_state_ = state;`（worker 线程路径由 `start_receive_path()` 在 **InitChannel/restart 调用线程**写）与 `reset_message()/reset_callback()`（**任意调用线程**读）、`process_thread_func():842`（**处理线程**读）之间**没有任何同步**；`:1004-1005` 的 `receive_route_.reset(); receive_state_.reset();` 同理。对同一个 `shared_ptr` 的并发读+写是数据竞争（C++ 未定义行为），可观测症状是极偶发的 `shared_ptr` 引用计数错乱/段错误，**在常规回归里几乎必然为绿**。
- **注意**：t3 对同一模式用了 `state_mtx_`（`include/dzIPC/shm_ser_cli_ipc.h:137` + `shm_ser_cli_ipc.cc:779-783` 的 `current_ser_state()`），即**同一团队已认可该模式**；t5 未采用，属实现不一致。
- **最小复现（结构性，无需构造竞态）**：`grep -n "receive_state_" src/dzIPC/socket_ser_cli_ipc.cc` 得到的全部命中点中，**没有任何一处持锁**；对照 t3 的 `current_ser_state()`。
- **要求的修复**：按 t3 既有模式加一把只保护 `shared_ptr` 自身的 `mutable std::mutex`（或 `std::atomic<std::shared_ptr>` 若标准允许；本项目 C++17 ⇒ 用锁），所有读写点统一走访问器；锁内**不得**再取 `state->mtx`（避免锁序环）。

### C-02 ｜ medium ｜ 同类无锁 `receive_state_`（socket_pub_sub）

- **文件:行**：`src/dzIPC/socket_pub_sub_ipc.cc:957-965`（`reset_message` 读）、`:1040`（`InitChannel` 写）、`:1114/1119/1143`（`start_receive_path` 读）、`:1158-1192`（`teardown` 写/reset）
- **问题**：与 C-01 同一模式（`InitChannel` 线程写 vs `reset_message` 线程读）。t4 的**暴露面略小**（`reset_message` 是话题类型变更路径），但仍是数据竞争。
- **最小复现**：`grep -n "receive_state_" src/dzIPC/socket_pub_sub_ipc.cc` + 逐点确认无锁。
- **要求的修复**：同 C-01（加 `state_mtx_` 或等价访问器）。

### C-03 ｜ medium ｜ 兼容 `response_thread_func` 缺 `try_claim_recv(compat_thread)`（socket_ser_cli）

- **文件:行**：`src/dzIPC/socket_ser_cli_ipc.cc:786-829`（整个函数体）
- **问题**：契约 §4.6 / `recv_worker.h:151-158` 要求"兼容线程启动前 `try_claim_recv(compat_thread)`；退出后 `release_recv()`"。t5 的 `response_thread_func` **既没有 claim 也没有 release**。**实际双收风险 = 低**（`InitChannel`/`restart_data_plane` 里两条路径由构造互斥：`start_receive_path()` 成功时起 `process_thread_func`，失败时起 `response_thread_func`，不会同时存在），但这是**对显式冻结契约条款的偏离**，且 `grep -n "RecvOwner::compat_thread" src/dzIPC/socket_ser_cli_ipc.cc` = **0 命中**，与契约 §4.6、与 t3/t4 的做法（`shm_ser_cli_ipc.cc:676`、`socket_pub_sub_ipc.cc:1049`）都不一致；一旦将来两模式共存，这里就是静默双收的入口。
- **最小复现**：`grep -n "compat_thread" src/dzIPC/socket_ser_cli_ipc.cc` → 无输出；对照 `grep -n "compat_thread" src/dzIPC/socket_pub_sub_ipc.cc src/dzIPC/shm_ser_cli_ipc.cc` → 各 1 处。
- **要求的修复**：`response_thread_func` 进入循环前 `try_claim_recv(compat_thread)`（失败即返回，不双收），退出前 `release_recv()`；与 t3/t4 同形。

### C-04 ｜ medium ｜ 回退路径未建 `req_route_`，且头注释与实现不符（shm_ser_cli）

- **文件:行**：`src/dzIPC/shm_ser_cli_ipc.cc:810/812/818/824/834/839`（六处 `return fallback(...)` 均在 `req_route_ = route;`（`:841`）之前）vs 头注释 `include/dzIPC/shm_ser_cli_ipc.h:86-89`「两条路径都会建好 `ser_state_`/`req_route_`，因此注销协议（含 `release_recv`）对两条路径都成立」。
- **问题**：走回退时 `req_route_` 为空，`response_thread_func():676` 的 `if (req_route_ && !try_claim_recv(...))` 与 `:707-710` 的 `release_recv()` **都被短路**⇒ 兼容线程既没 claim 也没 release，与契约 §4.6 的"兼容线程启动前 claim / 退出后 release"不符；头注释的承诺不成立。**实际双收风险 = 低**（回退时根本不会 `add_route`，owner 恒为 none），扣分点是"头注释承诺与实现相反"+"契约条款未落地"这两条可判定事实。
- **最小复现**：`DZIPC_SOCKET_COMPAT_THREAD` 不适用于 SHM；用 nodelet 路径即可触发（`InitChannel` 时 `IsNodeletEnabled()` 为真）→ 观察 stderr 的 `shm recv worker unusable (nodelet enabled...)`，此时 `req_route_` 为空；`grep -n "req_route_ =" src/dzIPC/shm_ser_cli_ipc.cc` 只有 `:841` 一处赋值。
- **要求的修复**：二选一——(a) 把小工具函数（claim/release）抽成不依赖 `req_route_` 的形态（直接在 `SerState` 上 CAS），回退路径也调用；或 (b) 让 `start_data_plane()` 在**所有**路径都先 `make_shared<SerRequestRoute>(state)` 并保存到 `req_route_`（兼容路径不 add_route 即可），再把头注释与实现对齐。

### C-05 ｜ medium ｜ 新增有界 FIFO 引入新的 chunk 持有窗口，缺实测量化（shm_ser_cli）

- **文件:行**：`src/dzIPC/shm_ser_cli_ipc.cc:109-110`（`kMaxPendingRequests = 64`、`kMaxPendingBytes = 8u << 20`）+ `:243-248`（入队）
- **问题**：FIFO 里放的是 `ipc::buffer`（完整请求字节）。SHM 每尺寸档 chunk 池容量 = `ipc::large_msg_cache`（`include/libipc/def.h:49` 实为 **40**，`nodelet_config.h:52/59` 与 `ipc.cpp:461` 的注释写作 "=32" 是既有口径不一致，另计）。8 MiB 上限意味着**单 server 最多可扣住 8 MiB 的 chunk**（远超单档 40 块的上限，实际受池本身约束），而改造前"取到即处理"不产生这个窗口；request 处理若走 DZFlat 借样路径，会与 Publisher 端 `dzflat` 借样争用同一尺寸档。方案 t3 未给该容量的**取值依据与实测**（`shm_ser_cli线程池移植方案.md` 全文 `grep kMaxPendingBytes` = 0）。
- **最小复现**：`grep -n "kMaxPendingBytes" docs/消息接收架构改造/shm_ser_cli线程池移植方案.md` → 无输出；`grep -c "DzFlatFallbackCount\|NoteDzFlatPublish" src/dzIPC/shm_ser_cli_ipc.cc` 亦无新增观测。
- **要求的修复**：在方案文档补"容量取值依据 + 与 chunk 池档位的关系"；或把上限降到与池同量级并在超限时**如实计数**（当前背压路径不丢数据，但会占住 chunk）。若判定为可接受，必须由 t8/t10 以实测量化（DZFlat 回退率前后对照）后**显式**登记为已知取舍。

### B-01 ｜ medium ｜ 方案文档未回写 + 一项验收收益未达成（shm_ser_cli）

- **文件:行**：`docs/消息接收架构改造/shm_ser_cli线程池移植方案.md:34`（`recv_once` = try_recv + **完整**处理（分流 / callback / 响应发送））vs 实现 `src/dzIPC/shm_ser_cli_ipc.cc:202-267`；方案 §6 第 1 条「数据接收线程数受 worker 上限约束」vs 实现文件头自我登记 `shm_ser_cli_ipc.cc:61-69`「per-server 线程数 = 2（处理线程 + 握手），与改造前相同 ⇒ 该收益本波未达成」。
- **问题**：① 方案正文仍与裁决 D1 冲突（代码正确、文档错误，属 t10 既定动作）；② 方案 §6 的收益表述（"数据接收线程数受 worker 上限约束"）在 **per-server 口径**下不成立——线程数并未下降，只是"等待/组包"从 per-server 线程移到了进程级池；t11 汇总时不得计入已交付收益。
- **最小复现**：`sed -n "34p" docs/消息接收架构改造/shm_ser_cli线程池移植方案.md`；`sed -n "61,69p" src/dzIPC/shm_ser_cli_ipc.cc`。
- **要求的修复**：t10 回写 §2 表格与 §6 验收口径（"per-server 线程数不变、等待/组包下沉到进程级池"），并在 §6 明确标注未达成项。

### C-06 ｜ low ｜ 兼容模式停机顺序可收紧（socket_pub_sub）

- **文件:行**：`src/dzIPC/socket_pub_sub_ipc.cc:1156-1185`
- **问题**：`stopping=true`（`:1161`）→（兼容分支）`cancel_wait`（`:1172`）→ `running=false`（`:1176`）→ join（`:1177-1185`）。在 `cancel_wait` 与 `running=false` 之间，兼容线程可能已回到循环顶部并用 `gate_on_readiness=false` 再次进入 `chunk_rev_topic`（此时 receive 面已 shutdown，返回空 ⇒ `stopping` 短路返回 0，不会真的阻塞 50ms）。**不构成忙轮询**，但把 `running=false` 提前到 `cancel_wait` 之前可完全消除该窗口。
- **最小复现**：读 `:1156-1185` 的控制流；结合 `:232-239` 的 `stopping` 短路。
- **要求的修复**：`running=false` 提到 `stopping=true` 同一处（或直接并入）。

### C-07 ｜ low ｜ `wait_quiescent()` 超时静默（三模块）

- **文件:行**：`src/dzIPC/shm_ser_cli_ipc.cc:290-297`、`src/dzIPC/socket_pub_sub_ipc.cc:326-332`、`src/dzIPC/socket_ser_cli_ipc.cc:313-319`
- **问题**：三处都是 `cv.wait_for(...)` 后**丢弃返回值**、不打印任何诊断。契约 §4.1/§4.4 与共享层 `recv_worker.cc:722-727` 的做法是"超时只打诊断、不阻塞注销"；模块侧缺该诊断面，超时后**静默**继续（此后 `release_recv` 仍会执行，属可接受，但可观测性缺失）。
- **最小复现**：`grep -n -A6 "wait_quiescent" src/dzIPC/socket_pub_sub_ipc.cc` → 无 `ipc::error`/`std::cerr`。
- **要求的修复**：`wait_for` 返回 false 时打一条显式诊断（含 route key 与 2000ms 上界）。

### C-08 ｜ low ｜ 兼容线程 claim 失败静默返回（t3/t4）

- **文件:行**：`src/dzIPC/shm_ser_cli_ipc.cc:676-679`、`src/dzIPC/socket_pub_sub_ipc.cc:1049-1052`
- **问题**：claim 失败（说明已有别的消费者）时直接 `return`，无任何日志。此时该 server/subscription **一个消费者都没有**却无人知晓（正是契约 §4.6 要防的"静默双收/静默无收"的另一面）。
- **最小复现**：读两处控制流。
- **要求的修复**：失败分支打一条显式日志（含 route key），便于现场定位。

### B-02 ｜ low ｜ 常量命名与方案文档不一致（三模块）

- **文件:行**：`src/dzIPC/socket_pub_sub_ipc.cc:36`（`kSubQuiesceTimeoutMs`）、`src/dzIPC/socket_ser_cli_ipc.cc:55`（`kSerQuiesceTimeoutMs`）、`src/dzIPC/shm_ser_cli_ipc.cc:189`（`kSerQuiesceTimeoutMs`）、`src/dzIPC/threepools/socket_recv_worker.cc:61`（`kMaxWorkerCount`）
- **问题**：方案文档写 `kSocketQuiesceTimeout=2000`、`kMaxSocketWorkers=128`，代码用了三个不同名字。**取值全部正确**，属可读性与文档一致性问题（跨模块 grep 同一常量时无法一次命中）。
- **最小复现**：`grep -rn "kSocketQuiesceTimeout\|kMaxSocketWorkers" src include` → 0 命中。
- **要求的修复**：t10 统一文档口径（或 t8 统一改名，二选一即可，不得两个都不做）。

---

## 6. 归因口径（不计入三模块扣分）

| 项 | 位置 | 归因 | 处置 |
|---|---|---|---|
| `select()+FD_SET` 当 fd 号 ≥ `FD_SETSIZE` 时 `__fdelt_chk` abort | ~~`src/libipc/platform/posix/udp.h:346 FD_SET` / `:352 ::select`~~ → **【D-20 纠正】`src/libipc/platform/posix/udp.h:347 FD_SET` / `:353 ::select`**；win 侧 `:327/392/482`（原写法；win 侧未复核） | **既有代码、非本波引入**：t6 只读准备用 `build_baseline` 与当前 HEAD 两库同探针对照，两库**同 exit=134**、归一化 gdb 帧 `FRAMES_IDENTICAL=yes`、两库均含 `__fdelt_chk@GLIBC_2.15` 未定义引用（证据 `.t6_probe/d9/`） | 登记为遗留项（归属 ipc-transport，本波不修）；**不计三模块扣分** |

> **D-20 行号纠正依据**（2026-09-28，队长裁决 D-20）：锚定提交 `e800ccc`（该文件 blob `19b6132687874c23a742fa7d0b216a1aaa58cef2`）逐行为 `:345 fd_set read_fds` / `:346 FD_ZERO(&read_fds)` / `:347 FD_SET(server_fd, &read_fds)` / `:349-351 timeval timeout` / `:353 ::select(...)`。原 `:346`/`:352` **恰好各小 1**，经**全历史扫描**（含该文件的全部 10 个提交：`:346=FD_SET` 出现 **0** 次、`:347=FD_SET` 出现 **1** 次）裁定为 **off-by-one 计数错误**，**不是**"多套行号口径"。⛔ 文档负责人早前"三套计数口径并存"的推论**已被该扫描否定**（两提交该文件为同一 blob，不可能同时有 `:346` 与 `:347` 为 `FD_SET`）。**只改行号定位，不改任何数值与结论。** 正确对照：`收尾验收报告.md:132`（本来就写 `:347`/`:353`）。
| 共享层 `readable()`/`udp_node_readable`/`out_bytes` 追加 | `include/libipc/udp.h:111/`、`src/libipc/socket/udp.cpp`、`src/libipc/platform/*/udp.h`、`include/dzIPC/common/data_rev.h:150/153/327` | 裁定 B/C 的**共享层 owner 交付面**（t12，owner=ipc-transport） | 不作为跨模块耦合；只核"只加不改"（见 §7） |
| `socket_recv_worker.{h,cc}` 新增 | 共享层 | 契约 §5 追加交付面（t2） | 同上 |
| `test_sercli_auto_path.cpp:337` 的线程回落断言 | 既有测试 | 共享层**已实现空闲退出**（`recv_worker.h:250`，`idle_keep_alive=1000ms`），回落耗时约 1.1s ≪ 断言窗口 4000ms ⇒ 不预设为失败项 | t6 实测口径 |
| `DZIPC_SOCKET_COMPAT_THREAD` 残留导致读数反转 | 验证环境 | 环境变量坑，非产品缺陷 | 复核统一用 `env -u`（见 §1 与 B.2） |

---

## 7. 阶段 5 契约「只加不改」核对（t12/t13 追加面）

| 追加项 | 落点 | 核对 | 结论 |
|---|---|---|---|
| `UDPNode::readable()` | `include/libipc/udp.h:111` | 既有 4 接口（`waitable/wait_handle/cancel_wait/clear_wait`）签名与语义未动（`git diff 8c6dd08 HEAD -- include/libipc/udp.h` 只增不删）；平台宏仍只在允许的落点 | ✅ 追加合规 |
| `udp_node_readable()` | `include/dzIPC/common/data_rev.h:327` | `nullptr` 安全；纯转发 | ✅ |
| `chunk_rev_*` 的 `out_bytes` 六参重载 | `include/dzIPC/common/data_rev.h:150/153`；实现 `src/dzIPC/common/data_rev.cc:1982-2011` | 旧五参重载**保留**（`:1985` 转调同一实现）；语义 = 载荷长度 `meta.total_size`（`:1703/1950`） | ✅ |
| 模块对 `out_bytes` 的使用 | t4 `socket_pub_sub_ipc.cc:267`（`, &bytes`）、t5 `socket_ser_cli_ipc.cc:247-248`（`, &bytes`） | 与 `socket_recv_worker.cc` 的 `bytes += n` 记账一致；**未**用 1 充字节 | ✅ |
| `RecvRouteSource` 9 纯虚 | `include/dzIPC/threepools/recv_worker.h:207-236` | 逐字脚本比对 = 与契约 §4.1 一字不差；`has_pending` 仍是带默认实现的追加项 | ✅ |
| 平台宏边界 | 五个公开头 | `grep -cE "^\s*#\s*(if|ifdef).*(__linux__|_WIN32)"` → `recv_worker.h`/`socket_wait_set.h`/`socket_recv_worker.h`/`data_rev.h`/`udp.h` 全 **0** | ✅ |

---

## 8. 结论与放行判定

1. **维度 A（方案符合性）**：通过。落地位置与写权限无越界、无跨模块耦合；基准行为逐条保持；D1/C7 裁定落实；B11 门在两条路径上都过。扣 B-01。
2. **维度 B（配置与硬编码）**：通过（取值层面）。常量逐条正确、两个开关真实生效且同名同义、回退条件齐备并**逐条打印显式原因**、无忙轮询降级。扣 C-05、B-02。
3. **维度 C（线程安全与资源释放）**：**不通过**。C-01（high）与 C-02/C-03/C-04（medium）必须修；C-01 是"常规回归全绿、偶发 UB"的类型，不能以"测试通过"为由放过。

**放行判定：needs_revision —— 存在 high 级未闭环项，不得进入收尾（t10/t11）**。修复范围：C-01～C-08 + B-01/B-02（其中 B-01 的文档部分归 t10；D9 遗留项归 ipc-transport、不在本波）。t8 修复后由 t9 逐条复现，C-01/C-02/C-03/C-04 必须给出"命令 + 输出"级证据（结构性核对即可，但必须证明并发访问点已全部收口）。

## 9. 附录：本次审查用到的命令

```bash
cd /home/zwc/cpp_ipc_dds
git log --oneline -1                                   # 1d38512
cmake -S . -B build && make -C build -j$(nproc)        # CMAKE_RC=0 / MAKE_RC=0 / 0 error
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD ctest --output-on-failure   # 12/12 Passed
env -u DZIPC_SOCKET_COMPAT_THREAD ./bin/test_dzipc_shm      # [request_response_test_SerInfo] request receive on shared SHM worker 20 (generation 1, workers 32)
env -u DZIPC_SOCKET_COMPAT_THREAD ./bin/test_dzipc_socket   # 两条 socket worker 日志（worker 12 / 25）
DZIPC_SOCKET_COMPAT_THREAD=1 ./bin/test_dzipc_socket        # rc=0 + 2x socket wait-set unusable (...); keeping per-subscription receive thread
git diff --stat cd5b427 HEAD -- src/dzIPC/threepools src/libipc include/libipc include/dzIPC/common src/dzIPC/common   # 空
grep -nE "epoll|WaitForMultiple|eventfd|WSAEvent|futex" src/dzIPC/{shm_ser_cli_ipc,socket_pub_sub_ipc,socket_ser_cli_ipc}.cc   # 空
grep -n "recvfrom|O_NONBLOCK|FIONBIO" src/dzIPC/socket_{pub_sub,ser_cli}_ipc.cc    # 仅 <fcntl.h> include（HEAD 既有），无调用
grep -rn "Pool::instance().stop" src/dzIPC/*.cc        # 0
grep -n "compat_thread" src/dzIPC/socket_ser_cli_ipc.cc              # 0 命中  <- C-03
grep -n "compat_thread" src/dzIPC/socket_pub_sub_ipc.cc src/dzIPC/shm_ser_cli_ipc.cc   # 各 1 处
grep -n "req_route_ =" src/dzIPC/shm_ser_cli_ipc.cc    # 仅 :841（六处 fallback 均在其前）<- C-04
grep -n "kMaxPendingBytes" docs/消息接收架构改造/shm_ser_cli线程池移植方案.md   # 0 命中 <- C-05
sed -n "34p" docs/消息接收架构改造/shm_ser_cli线程池移植方案.md               # 方案仍写 recv_once 含 callback <- B-01
grep -rn "kSocketQuiesceTimeout|kMaxSocketWorkers" src include                   # 0 命中 <- B-02
```