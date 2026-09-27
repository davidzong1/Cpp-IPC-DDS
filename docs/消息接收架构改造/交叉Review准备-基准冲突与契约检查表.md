# 交叉 Review 准备：基准冲突清单 + 阶段 5 契约符合性检查表 + 三模块自检单

> 状态：**t7 前置准备（不是 t7 结论）**。owner：ipc-review-security。
> 为什么现在只做前置：claim t7 时其依赖 t3/t4/t5 全部未完成（`claim_task` 被拒：`task t7 is blocked by unfinished dependencies: t3, t4, t5`），三模块源文件当时与 HEAD 逐字节相同 ⇒ **无被审代码**。
> 本文只做「读 + 记录 + 给检查单」：**未修改任何产品源码、三份方案文档与共享层**。
> 取证：2026-09-27 21:0x–21:5x CST；引用统一用工作区 `docs/消息接收架构改造/` 路径（不引缓存路径）。
> ⚠️ 基线随时间前移：写作时 `HEAD = 0d1b672`（§0.1–§0.4 的读数），21:4x 后基线改为 `8c6dd08`（§0.6）。凡与本文「现状读数」冲突处，**以本文最新复测读数为准**（修正见 §1、§0.6）。
> ⚠️ t1《基准对齐笔记》是本文口径来源，但它发布（21:01）之后 t2 在 21:01–21:12 落地了共享层。差异见 §1。

---

## 0. 门禁与取证快照

### 0.1 依赖门禁（21:20 读数）

| 任务 | 状态 | assignee | 对 t7 的后果 |
|---|---|---|---|
| t3 shm_ser_cli | pending（attempt 0） | ipc-shm-sercli | 无 `SerState` / `SerRequestRoute`，无可审代码 |
| t4 socket_pub_sub | pending（attempt 0） | ipc-socket-pubsub | 无 `socket_sub_receive_state`，无可审代码 |
| t5 socket_ser_cli | pending（attempt 0） | ipc-socket-sercli | 无移植痕迹，无可审代码 |

复测：`python3 -c "import json;d=json.load(open('.agent-teams/cpp-ipc-threadpool-port/team.json'));print([(t['id'],t['status'],t['attempt']) for t in d['tasks'] if t['id'] in ('t3','t4','t5')])"` → 三项全 `pending, 0`。

### 0.2 三个模块的现状锚点（21:2x 时仍与 `0d1b672` 逐字节相同；21:4x 后 socket_ser_cli 已开始落地，见 §0.6）

`git status --porcelain` 中**没有**这三个模块的 M 项；`git diff --stat HEAD -- src/dzIPC/{shm_ser_cli_ipc.cc,socket_pub_sub_ipc.cc,socket_ser_cli_ipc.cc}` 为空。

| 模块 | 现状事实（实测行号） |
|---|---|
| shm_ser_cli | 析构 `:229-265`；`handshake_thread_` `:285`、`response_thread_` `:312`；`response_thread_func` `:376`；`ipc_r_ptr_->recv(50)` `:417`；`kPeerDeadTimeoutNs` 字面量 `:336`；**无** pid 闸（`grep recv_pool_owner_pid\|current_process_id` = 0）；**无** `SerState`/`SerRequestRoute`（grep = 0） |
| socket_pub_sub | `discovery_thread_` `:134`（50ms）；每订阅 `subscribe_thread_` `:690`；`chunk_rev_topic(subscriber_, local_msg, 50, ack_tx_, &wire)` `:721`；**无** `socket_sub_receive_state`（grep = 0） |
| socket_ser_cli | `#define ServerRevTime 200` `:15`；`chunk_rev_server(ipc_r_ptr_, local_msg, ServerRevTime, true, ack_r_tx_)` `:481`；`response_thread_` + `handshake_thread_` 独立 |

### 0.3 共享层（t2）已落地但**未入 git**（⚠️ 21:4x 已由 `8c6dd08` 闭合，见 §0.6）

| 文件 | 行数 | mtime | git |
|---|---|---|---|
| `include/dzIPC/threepools/recv_worker.h` | 391 | 14:43 | `??` 未跟踪（`git log --all` 为空） |
| `src/dzIPC/threepools/recv_worker.cc` | 985 | 14:43 | `??` |
| `include/dzIPC/threepools/socket_wait_set.h` | 145 | 09-26 11:56 | `??` |
| `src/dzIPC/threepools/socket_wait_set.cc` | 419 | 09-26 11:58 | `??` |
| **`include/dzIPC/threepools/socket_recv_worker.h`** | 235 | 21:06 | `??`（契约 §5 追加交付面） |
| **`src/dzIPC/threepools/socket_recv_worker.cc`** | 982 | 21:08 | `??` |
| `test/test_recv_worker.cpp` / `test/test_socket_wait_set.cpp` / `test/test_socket_recv_worker.cpp` | 912 / 493 / 452 | 21:0x | `??` |

已改动（M，未提交）：`include/libipc/udp.h`、`src/libipc/socket/udp.cpp`、`src/libipc/platform/{posix,win}/udp.h`、`include/dzIPC/common/data_rev.h`、`src/dzIPC/common/data_rev.cc`、`test/CMakeLists.txt`。
`git log --oneline -1` 仍为 `0d1b672` ⇒ **t2 尚未提交**，A3/A7 在 t7 执行时仍须复核。

注意：`socket_recv_worker.{h,cc}` 的文件权限是 `-rw-------`（600），与同目录其它文件 `644` 不一致；入 git 前建议统一（低严重级，不影响语义）。

### 0.4 构建产物新鲜度（A8 复测）

- `build/lib/libipc.so.1.3.0` mtime **21:12:32**，晚于 `socket_recv_worker.cc`（21:08）与契约文档（21:11）⇒ t2 在 21:12 重跑过 `cmake -S . -B build` + 重编（证据 `.t2_evidence_make.log` 21:12:37、`.t2_evidence_ctest.log` 21:13:05）。
- `nm -DC build/lib/libipc.so`：`SerRequestRoute|socket_sub_receive_state|SocketReceiveWorkerPool` = **0**（污染符号已消失）；`wait_handle|cancel_wait|udp_node_` = **6**（新交付面已在库内）。
- `ctest -N` = **11** 条（原 8 + `test_recv_worker` + `test_socket_wait_set` + `test_socket_recv_worker`，后三者 `test/CMakeLists.txt:222/225/234`）。

**A8 纪律不变**：上表只是「当前这一次构建是新鲜的」，t6/t7/t9 每次验收前仍必须显式 `cmake -S . -B build` + 重编，禁止直接采信 `build/bin/*` 的既有读数。

### 0.5 追加时间戳（21:3x 复测，晚于 §0.1–§0.4）

写本文时（21:2x）之后，任务图又变了，逐条追加如下（**不改上文读数**，只补新事实）：

| 时刻 | 变化 | 对 t7 的影响 |
|---|---|---|
| 21:3x | **t2 已 completed**（不再是 in_progress）；`git log -1` 复核见下 | §0.3 的「t2 尚未提交」与 A3/A7 需在 t7 时**重测**：若 t2 未 commit，A3/A7 仍是未闭环项 |
| 21:3x | **t3 `claimed`、t4/t5 `in_progress`** | t7 依赖仍未完成 ⇒ 仍不可 claim；被审代码开始出现（`socket_ser_cli_ipc.{h,cc}` 已 M） |
| 21:3x | **新增 t12「共享层追加：非阻塞可读判据 `readable()`/`udp_node_readable`（裁定C）+ 字节出口 `out_bytes`（裁定B）」owner=ipc-transport，in_progress** | 这是**契约冻结后按「新增一律追加」**加进共享层的第 7 项：`SocketWaitSet`/`UDPNode` 新增 `readable()` 类接口与 `out_bytes`。t7 必须把**新增接口**也纳入维度 A/C 核对（追加是否真的「只加不改」、既有签名是否被动过）；若 t4/t5 已按旧接口实现，需核对是否需要跟随 |
| 21:3x | t6 出现 `.t6_probe/`、`Testing/` 工作区目录 | 属 t6 证据产物，t7 复核时不得把它们当源码改动 |

### 0.6 追加时间戳（21:4x–21:5x 复测；队长裁决回填 + 保护性提交）

**(1) A3/A7 已闭合：保护性提交 `8c6dd08`**

| 项 | 读数 |
|---|---|
| `git log --oneline -1` | **`8c6dd08`**「wip(threadpool): 阶段5共享层 + 三模块移植进行中快照」 |
| `git show --stat 8c6dd08` | **25 files changed, 7914 insertions(+), 37 deletions(-)** |
| 纳入 | 阶段 5 共享层 6 文件（`recv_worker.{h,cc}` 391/985、`socket_wait_set.{h,cc}` 145/419、`socket_recv_worker.{h,cc}` 247/982 行）、UDPNode 可等待面（`include/libipc/udp.h` +56、`src/libipc/socket/udp.cpp` +45、`platform/posix/udp.h` +125、`platform/win/udp.h` +151）、`data_rev.{h,cc}`、三个测试（912/493/452 行）+ `test/CMakeLists.txt` +31、三份方案文档、契约与勘误、基准对齐笔记、本检查表、t6 准备记录、进行中的 `socket_ser_cli_ipc.cc` +567 |
| 刻意排除的过程产物 | `.agent-teams/`、`.t12_make1.log`、`.t2_evidence_*.log`、`.t6_probe/`、`Testing/` |
| 复核命令 | `git ls-files include/dzIPC/threepools src/dzIPC/threepools test` → 六个新增源/头文件全部**已被跟踪** |
| 可逆性 | `git reset --soft HEAD~1`（WIP 提交，不推远端） |

背景（队长给出，供 t7 报告引用）：该工作区 20:30 曾一次性清空 599 个文件（582 个在 `test/perf/`），而当时全部交付物都不在 git 里 —— 这是做该提交的直接原因。⇒ **t7 不再把 A3/A7 当未闭环项**；但每次验收前仍须 `cmake -S . -B build` + 重编（A8 纪律与提交无关）。

**(2) 三条拉响项已由队长裁定（按「已裁定」而非「未决」处理）**

| 项 | 裁定 | t7 的判定口径（改写后） |
|---|---|---|
| **D1** callback/响应发送不得进 `recv_once()` | **以需求 §1.2 + 冻结契约 `recv_worker.h:218-220` 为准**；t3 任务描述已内嵌该裁决（标题即「裁决D1：callback 不入 worker」），t3 的 `SerState`/`SerRequestRoute` 当时尚未落笔；方案 `shm_ser_cli线程池移植方案.md:34` 的回写属 **t10 既定动作**，不是 t3 的挡路石 | ⛔ **若 t3 把 callback 放进 `recv_once()` ⇒ 判 blocker 并退回**；**不得**以「方案这么写所以合规」为由放行 |
| **C7** 池未转发 `wakeup()` | **已裁定**：nodelet 与固定 worker **二选一**，本波**不要求**共享层转发 `wakeup()`；nodelet 启用 ⇒ 保留兼容接收线程、worker 模式不注册进程内队列；t3/t5 已被告知「不得声称已支持 nodelet 走 worker」 | **不再列为 blocker**；只需核对「模块确实回退 + 无半吊子实现（既不注册又不回退 / 声称已支持）」 |
| **D9** `posix/udp.h:346 FD_SET` / `:352 ::select` | **已证毕为既有缺陷、非本波引入**：t6 只读准备用 `build_baseline` 与当前 HEAD 两库同探针对照，两库**同 `exit=134`**、归一化 gdb 帧 **`FRAMES_IDENTICAL=yes`**、两库均含 `__fdelt_chk@GLIBC_2.15` 未定义引用；证据目录 `.t6_probe/d9/`（`base.log`/`head.log` + `base_gdb.frames`/`head_gdb.frames`，均 21:15） | 按「**既有代码、非本波移植缺陷**」登记为遗留项（归属 ipc-transport，本波不修）；**不计入三模块审查扣分** |

**(3) t12 两个新增接口纳入 t7 核对（「只加不改」）**

| 新增项 | 落点（21:4x 复测） | t7 核对点 |
|---|---|---|
| `UDPNode::readable()` | `include/libipc/udp.h:111`（注释 `100-110`：不阻塞、不改状态、**不做读操作**、幂等 O(1) 无分配；fd 无效 / SendOnly / 已 cancel 一律 false；Linux `poll(POLLIN,0)`、Windows 0 超时 select） | ① 既有 4 接口（`waitable/wait_handle/cancel_wait/clear_wait`）**签名语义未被改动**；② 平台宏仍只在允许的落点；③ 是否真非阻塞且不消费数据（可由 `test_socket_wait_set`/`test_socket_recv_worker` 复核） |
| `udp_node_readable()` | `include/dzIPC/common/data_rev.h:327`（注释 `323-326`：语义同 `UDPNode::readable()`） | 追加是否 `nullptr` 安全、是否为纯转发 |
| `chunk_rev_*` 的 `out_bytes` 重载 | `include/dzIPC/common/data_rev.h:150`（`..., ipc::buffer* out_payload, std::size_t* out_bytes`）、`:153`（`..., ack_node, std::size_t* out_bytes`）；`out_bytes` = 发送端交给分片器的**载荷字节数**（`meta.total_size`），**不是** wire 字节数 | ① 旧签名（无 `out_bytes`）是否**保留**（追加 ≠ 替换）；② `recv_once()` 的「正返回值 = 字节数」语义是否与 `RecvWorker::run_budget` 的 `bytes += n` 记账一致（`recv_worker.cc:285/290`）；③ t4/t5 是否已跟随新接口、还是仍按旧接口实现（须在 t7 逐模块确认） |

---

## 1. t1 笔记的时效性修正（复测后）

| t1 笔记条目 | 21:0x 读数 | 21:2x 复测 | 处置 |
|---|---|---|---|
| A4「UDP 可等待句柄与 `udp_node_*` 任何地方都不存在」 | `grep -c` = 0 | **已被 t2 修复**：`include/libipc/udp.h:78/83/90/95` 四个声明；`src/libipc/socket/udp.cpp:126-150` 四个纯转发；`src/libipc/platform/posix/udp.h:432/451/472/490`、`win/udp.h:497/510/541/561` 平台实现；`include/dzIPC/common/data_rev.h:289-295` + `data_rev.cc:2899-2921` 四个 `udp_node_*` 转发 | t7 维度 C 的「A4」项按**已闭合**复核（消费侧仍需检查读路径） |
| A9「`add_test` 仅 8 条、新套件未进 ctest」 | 8 / 0 / 0 | **已被 t2 修复**：`grep -c ^add_test` = **11**，`test_recv_worker`/`test_socket_wait_set`/`test_socket_recv_worker` 各 1 | 按已闭合，t7 只需确认未被回退 |
| A8「build/ 是污染产物」 | `nm` 仍见 `SerRequestRoute` 等 | **当前 build 已刷新**（见 §0.4），污染符号 0 | 纪律保留，读数可信性按 §0.4 判定 |
| A3/A7「共享层未跟踪」 | `??` 六项 | 仍 `??`，且**新增** `socket_recv_worker.{h,cc}` 与 `test_socket_recv_worker.cpp` | t7/t11 复核 `git status`；契约 §5 已把该文件列为**共享层追加交付面**（`docs/…contract-99ff82a0f9af.md:18/354-420`），归属 ipc-transport，不算跨模块耦合 |
| C9/D2「共享层无空闲退出 ⇒ 线程不回落」 | 实现已提供 `idle_keep_alive` | 仍成立：`recv_worker.h:250`；socket 侧同构，`socket_recv_worker.cc:427-442` 空闲退出、`:476-552` 按需拉起 | t3/t4 的「已知偏差」必须按空闲退出重写（t1 笔记 D2 已裁定） |

---

## 2. 维度 A：方案文档 vs 阶段 1-4 基准 / 工作区事实（逐条可判定）

> 判定规则：每条给出「文档位置 → 事实位置 → 以谁为准 → 现在是否已闭环」。t7 执行时对 t3/t4/t5 各自合入后的方案文档重跑本节命令。

| ID | 冲突 | 文档位置 | 事实（实测） | 以谁为准 | 现在 |
|---|---|---|---|---|---|
| A1 | shm_ser_cli「S1-S3 已落地并验证」 | `shm_ser_cli线程池移植方案.md:3`（另 `:81/84/147`） | 模块源与 HEAD 逐字节相同，`grep -c SerRequestRoute` = **0** | **工作区** | ❌ 未闭环（等 t3 重建 + t10 回写状态行） |
| A2 | socket_pub_sub「U0–U3 已实现并验证」 | `socket_pub_sub线程池移植方案.md:3`（另 `:87/91/100`） | 同上，`grep -c socket_sub_receive_state` = **0** | **工作区** | ❌ 未闭环（等 t4/t10） |
| A5 | socket_ser_cli 状态行「执行方案，尚未实现」 | `socket_ser_cli线程池移植方案.md:3` | 与工作区一致（无移植痕迹） | 工作区 | ✅ 本来正确，t10 不要误改 |
| A6 | 三份方案的接口清单漏 `RecvRouteSource` | `socket_pub_sub…md` / `socket_ser_cli…md`：`grep -c RecvRouteSource` = **0 / 0** | 头文件 `recv_worker.h:200-237` 有 9 纯虚 + `has_pending`；契约 §4.1/§4.4 | **契约 + 头文件** | ❌ socket 两份未回写；`shm_ser_cli…md:29-42` 已补齐（唯一正确映射，可作回写模板） |
| A3/A7 | 共享层依赖未进 git | 三份方案均引用 `recv_worker.h` | `??` 未跟踪；`git log --all -- <path>` 为空 | **t2 必须入 git** | ❌ 未闭环（t2 仍在 in_progress） |
| A10 | `DZIPC_SOCKET_RECV_WORKERS` / `DZIPC_SOCKET_COMPAT_THREAD` 源码不存在 | `socket_pub_sub…md:19/20/98` | 全仓模块源 `grep getenv` 命中的只有 `data_rev.cc:628`（`DZIPC_TAU_DELAY_ROUNDS`）与 `logger-dzipc_log.cc:294`；`DZIPC_SOCKET_RECV_WORKERS` 只出现在**共享层** `socket_recv_worker.cc:87`；`DZIPC_SOCKET_COMPAT_THREAD` **全仓 0 命中** | **工作区** | ❌ t4 实现、t5 对齐；「共享层已读一次」≠「模块兼容开关已生效」 |
| A4 | UDPNode 可等待 API 不存在 | 契约 §1/§2、勘误 E3 | 见 §1 复测：已存在且已编入库 | 契约 + 勘误 E1 | ✅ 共享层侧闭合（t2） |
| A9 | 新套件未进 ctest | — | 11 条 add_test | 实现 | ✅ 共享层侧闭合（t2） |
| A8 | build/ 污染 | — | 已重编（§0.4） | **源码** | 纪律保留 |

### 2.1 落地位置一致性（t7 维度 A 的硬核对项）

| 检查项 | 期望（方案/契约） | 复测命令 | 现在 |
|---|---|---|---|
| 模块只改自己的 `.h/.cc` | t3 只动 `shm_ser_cli_ipc.{h,cc}`；t4/t5 同理 | `git status --porcelain \| grep -E "shm_ser_cli_ipc\|socket_pub_sub_ipc\|socket_ser_cli_ipc"` | 现在为空（模块未动） |
| 模块不得改共享层 | `threepools/*`、`libipc/*`、`common/*` 归 ipc-transport | `git diff --stat HEAD -- src/libipc src/dzIPC/common include/libipc include/dzIPC/common` | 现在有 t2 的改动（合法）；t7 时须确认**只有** t2 的痕迹 |
| 模块不得自造平台 ifdef | 句柄一律 `std::uintptr_t`，平台宏只在 `platform/*/udp.h`、`recv_wait_set.cpp`、`socket_wait_set.cc` | 见 §5.2 | 现在 0 |

---

## 3. 维度 B：阶段 1-4 说明文档 vs 基准实现（t7 复核对齐口径）

| ID | 项 | 文档 | 实现（实测） | 以谁为准 |
|---|---|---|---|---|
| B1 | 阶段 1 落点 | 说明 §2.1 建议 `common/` | 实际 `threepools/` + `src/CMakeLists.txt:20` `aux_source_directory` | 实现 |
| B4 | 2s 死连接常量未收敛 | 说明 §1.2/§8 R8 要求收敛 | 仍两份字面量：`shm_pub_sub_ipc.cc:274`、`shm_ser_cli_ipc.cc:336` | 实现（未做）⇒ t3 必须在自己的模块内处置，t6 记未完成项 |
| B5 | `Stats` 字段 | 说明 §3.2 六字段 | 实现 `recv_worker.h:150-159` 有第 4 个 `callback_exception_count` | 实现 |
| B7 | 阶段 2 接口清单 | 说明 §2 无 `release_receive` | `shm_route_session.h:108` 有；8 个 public | 头文件 |
| B9 | 阶段 4 落点 | 说明 §1 `platform/{linux,win}/` | 单文件 `src/libipc/recv_wait_set.cpp`（`__linux__`/`_WIN32` 同文件） | 实现（t2 不得新建平台目录） |
| B10 | 路数上限 | 说明 §3.1「128」 | `recv_wait_set.cpp:23-30` = Linux **127** / Windows 63 | 实现 |
| B11 | 唤醒伪影门落点与判据 | 旧交接档建议「留在收包循环、只扫尾 12 字节」 | 门在 `process_received_buffer()` 内（`shm_pub_sub_ipc.cc:67-71`），判据**整段全零**（`wire_accept.cc:104-110`） | 实现 ⇒ **t3/t4/t5 的每条新收包路径必须过门**（见 §6.3） |
| B12 | 析构步序 | 说明 §5 八步 | `shm_pub_sub_ipc.cc:661-724` 同序；worker 场景把 join 换成 `pool.remove_route()` | 实现 |

---

## 4. 维度 C/D：契约 vs 实现、方案文档内部冲突

| ID | 项 | 裁决 | 现状 |
|---|---|---|---|
| C1/E1 | `wait_handle()` 阻塞模式 | **勘误 E1**：Linux 返回**阻塞** fd、不做 `O_NONBLOCK`/`FIONBIO` | ✅ 已落地（`posix/udp.h:439-465` 注释 + 无任何 fcntl/FIONBIO；`grep -c "O_NONBLOCK\|FIONBIO"` 在新增面 = 0） |
| C2/E2 | `add_route` 判定顺序 | **勘误 E2**：CAS 失败 → 先查表（表内 ⇒ `duplicate`，否则 `busy`） | ✅ `recv_worker.cc:581-603`；`socket_recv_worker.cc` 同序 |
| C3/E3 | 交付面漏 `src/libipc/socket/udp.cpp` | **勘误 E3**：该文件是实现 4 声明的唯一落点 | ✅ `src/libipc/socket/udp.cpp:126-150` |
| C4 | `RecvRegisterStatus` 漏 `wait_set_full` | 实现（新增一律追加） | ✅ `recv_worker.h:286` |
| C5/C6 | `RecvBudget` / `Stats` 追加字段 | 实现（追加不破坏兼容） | ✅ `recv_worker.h:250`、`:267-272` |
| **C7** | **池无 `wakeup()` 转发** | 实现（池只导出 add/remove） | ⚠️ **仍成立**：`recv_worker.h:373-374` 无 wakeup；`socket_recv_worker.h:218-219` 同（契约 §5 第 386 行明写「池级 wakeup 未转发」）⇒ t3 的 nodelet 路径、t5 的「队列新请求叫醒」若无追加接口，**必须回退兼容线程**，不得声称已支持 |
| C8 | Windows 未验证 | 契约 | 状态不变（本机 Linux 6.8）；t7/t9 不得把 Windows 路径记为已验证 |
| **D1** | `recv_once()` 内含用户 callback 与响应发送 | **需求 §1.2 + 契约/头文件 `recv_worker.h:218-220` ⛔** | ❌ **方案未回写**：`shm_ser_cli线程池移植方案.md:34` 仍写「非阻塞 `try_recv()` + **完整**处理（分流 / callback / 响应发送）」。**这是本轮最高优先级设计级冲突**，t3 必须把 callback/响应发送移出 worker（或 leader 显式批准登记偏差）；t10 必须回写该行 |
| D2 | 常驻池导致线程不回落 | 实现已提供空闲退出 | ✅ 已推翻「共享层无空闲退出」的前提（`recv_worker.h:50-110/250`）；`shm_ser_cli…md:123-141` 的「已知偏差」须重写 |
| D3 | 热 route 饿死冷 route | 实现（`collect_pending()` 全扫是事实来源） | ✅ `recv_worker.cc:166-184/340`；socket 侧另有 `kMaxFruitlessReadiness=4` 兜底（`socket_recv_worker.cc:66/209-212`） |
| D4 | `running` 闸重建 | 文档（t4 待办） | 模块未动，t4 必须自证「`teardown_receive_path()` 后 `InitChannel` 重新置 true」 |
| D6 | 容量数字三层混用 | 各自实现 | SHM `recv_wait_set` 127/63；`SocketWaitSet::max_channels()` Linux **4096**/Win **63**（`socket_wait_set.cc:31/33`）；池 worker 上限 128（`recv_worker.cc:64`） |
| D7 | 控制面线程未收敛 | 需求 §10 阶段 1 验收 vs 方案现状 | ❌ 阶段 1 仍是「库内就绪、产品未接入」：全仓无 `ShmControlScheduler::register_subscriber/register_publisher` 产品调用点（`grep` 仅命中 `LocalPubSubRegistry`）；`shm_pub_sub_ipc.h:140/201` 两线程仍在；`shm_ser_cli_ipc.cc:285/336` 握手线程未迁。t11 必须列为**未达成验收项** |
| **D9** | `select()+FD_SET` 既有限制 | 工作区（既有缺陷，非本波引入） | ❌ **仍未修**：`src/libipc/platform/posix/udp.h:346 FD_SET` / `:352 ::select`；win 侧 `:327/392/482`。归因口径：既有代码、非模块移植缺陷；修复落点 ipc-transport（→ `poll()`）。t6 的 1000 订阅收包验收在其修复前**必红** |

---

## 5. 阶段 5 契约与勘误符合性检查表（共享层侧已可判定）

### 5.1 `RecvRouteSource` 9 纯虚 + 追加接口（接口兼容面）

**已用脚本逐字对比**（提取 `= 0;` 签名并归一化空白）：

| # | 签名（`recv_worker.h`） | 行 | 与契约 §4.1 |
|---|---|---|---|
| 1 | `const char* route_name() const noexcept` | 207 | 一字不差 |
| 2 | `std::uint32_t domain_id() const noexcept` | 208 | 一字不差 |
| 3 | `ipc::recv_wait_token read_wait_token() const noexcept` | 212 | 一字不差 |
| 4 | `std::size_t recv_once()` | 221 | 一字不差 |
| 5 | `RecvOwner recv_owner() const noexcept` | 228 | 一字不差 |
| 6 | `bool try_claim_recv(RecvOwner who) noexcept` | 229 | 一字不差 |
| 7 | `void release_recv() noexcept` | 230 | 一字不差 |
| 8 | `void stop_and_wake() noexcept` | 234 | 一字不差 |
| 9 | `void wait_quiescent() noexcept` | 236 | 一字不差 |
| 追加 | `bool has_pending() const noexcept { return false; }` | 225 | 带默认实现，不计入 9 纯虚 ✅ |

结论：9 签名与契约 §4.1 **逐字符一致**（脚本判定 `header(9) == contract[0:9]`）；`has_pending` 是追加默认实现，冻结规则合规。

### 5.2 平台宏边界（硬约束）

命令：`for f in include/libipc/udp.h include/dzIPC/common/data_rev.h include/dzIPC/threepools/{recv_worker,socket_wait_set,socket_recv_worker}.h; do grep -cE "^\s*#\s*if.*(__linux__|_WIN32)" $f; done`

读数：**0 / 0 / 0 / 0 / 0**。允许出现平台宏的只有 `src/libipc/platform/*/udp.h`、`src/libipc/recv_wait_set.cpp`、`src/dzIPC/threepools/socket_wait_set.cc`（实测：`socket_wait_set.cc:10-17/30-34/36-52/104-124/187-200/212-236/248-316/399-408` 内，且 `recv_worker.cc` / `socket_recv_worker.cc` 各 **0**）。

### 5.3 注销第 6 步与信令顺序（消费侧必须自证；共享层已自证）

| 检查项 | 共享层位置（已核对） | 消费侧（t3/t4/t5 待核） |
|---|---|---|
| 1 摘除 + 标记 | `recv_worker.cc:697-707`、socket 同构 | 模块不得越过池自行摘表 |
| 2 `wait_set.remove` + 唤醒 | `recv_worker.cc:714`（`recv_wait_set::remove` 内 `interrupt+1` + `futex_wake`，`recv_wait_set.cpp:136-141`）；socket 侧 eventfd 唤醒 `socket_wait_set.cc:226-233` | — |
| 3 `stop_and_wake` | `recv_worker.cc:717` | 模块实现内应 `disconnect()` / `udp_node_cancel_wait()` |
| 4 等在途 recv_once 归零（锁外） | `recv_worker.cc:722-727`（上界 `kQuiesceTimeout=2000ms`，`:66-77`） | 模块的 `recv_once` 必须在有界时间内返回（契约 §4.1 第 4 条） |
| 5 `wait_quiescent` | `:730` | 模块 lease/in-flight 记账（含异常出口） |
| **6 `release_recv()`** | `:733` | **兼容线程路径必须在退出前显式归还**，否则 `add_route` 永久 `busy` |
| fd/句柄关闭时机 | 池侧 `remove_route` 同步返回 ⇒ 之后才可关 fd | `socket_wait_set.h:36-39/77-79` 明写 token 不拥有通道 |

### 5.4 E1 读路径约束（socket 模块的静默故障红线）

| 禁止项 | 检测命令（t7 用） | 期望 |
|---|---|---|
| 对接收 fd 用裸 `recvfrom` | `grep -n "recvfrom" src/dzIPC/socket_{pub_sub,ser_cli}_ipc.cc` | 0 |
| 自置 `O_NONBLOCK` / `FIONBIO` | `grep -nE "O_NONBLOCK\|FIONBIO\|fcntl" <模块源>` | 0 |
| 读路径走 `receive_nowait` / `chunk_rev_*` | `grep -n "receive_nowait\|chunk_rev_topic\|chunk_rev_server" <模块源>` | 命中（存在且是唯一读入口） |
| 忙轮询降级 | `grep -n "try_recv\|receive_nowait" <回退分支>` | 回退分支不得全量轮询 |

---

## 6. 维度 B/C 的三模块自检清单（交给 t3/t4/t5，t7 逐条复核）

### 6.1 三模块共同必须自证（缺一即 finding）

1. **接口面**：适配器实现 `RecvRouteSource` 9 纯虚（签名一字不改），`has_pending` 只在确有多条待收时才为 true；不得改共享层签名或语义。
2. **单消费者**：`std::atomic<RecvOwner>` + `try_claim_recv/release_recv`；兼容线程启动前 `try_claim_recv(compat_thread)`、退出时 `release_recv()`；worker 路径由 `remove_route` 归还。检测：`grep -n "RecvOwner::" <模块源>` 应出现 none/compat_thread/worker 三态赋值。
3. **不回退契约**：`get/try_get/get_clone/try_get_clone`、队列背压/淘汰、DZFlat/TLV 分流、`msg_id`/schema 校验、`AcceptWire()`、adopt 配额与 `adopt_borrowed` 三点记账、evict 回调、chunk 生命周期、nodelet 快路径、顺序/多片重组、generation/断线回收 —— 逐条与 HEAD 对照（`git diff HEAD -- <模块源>` 只应出现移植新增，不应出现语义改写）。
4. **唤醒伪影门**：每条新收包路径必须过门。shm 侧必须复用 `process_received_buffer()`（门在 `shm_pub_sub_ipc.cc:67`）或**等价带门实现**（整段全零判据 + `NoteWakeupArtifact()`）。检测：`grep -n "IsWakeupArtifact\|NoteWakeupArtifact" <模块源>`。脚本侧尚无对应门（socket 无伪影语义），但必须自证「未读走事件不误当消息」。
5. **回退路径**：任一非 `ok`（`backend_unavailable`/`invalid_token`/`wait_set_full`/`stopped`/`busy`）+ 线程创建失败 + 开关强制，都必须保留兼容线程并**打印显式原因**；一次失败即永久回退，不得运行中切换；⛔ 不得 `try_recv`/`receive_nowait` 全量忙轮询。
6. **fork 防死锁闸（必查）**：`current_process_id()` vs `recv_pool_owner_pid()`；pid 变化 ⇒ 不得调 `add_route`，且判断过程不触碰池锁。检测：`grep -n "recv_pool_owner_pid\|current_process_id\|getpid" <模块源>`（现在 = 0）。
7. **池生命周期**：模块不得调用 `RecvWorkerPool::stop()` / `SocketRecvWorkerPool::stop()`（会把进程永久降级为兼容线程）。检测：`grep -n "\.stop()" <模块源>` 须为 0 命中池对象。
8. **空闲退出口径**：不得再把「析构后线程回落」写成不可达成/已知偏差；应自证 `Stats::idle_exits` / `thread_restarts` 增长（`recv_worker.h:271-272`）。
9. **常量/开关**：32 条 / 1 MiB / 200 us / `wait_timeout=100ms` 与契约一致；socket 侧 `kSocketSubRecvTimeoutMs=50` **不得缩短**；`SocketQuiesceTimeout=2000`；worker 上限 128；`DZIPC_SOCKET_RECV_WORKERS`（进程内只读一次）与 `DZIPC_SOCKET_COMPAT_THREAD` 必须真正生效；t4/t5 **同名同义**。
10. **析构/重启无永久等待**：按契约 §4.4 六步 + 需求 §4.2 补齐；`remove_route` 返回后才 `close`；重复 `InitChannel`/restart 走新 generation。

### 6.2 shm_ser_cli 专属（t3）

| 项 | 期望 | 检测 |
|---|---|---|
| `SerState` / `SerRequestRoute` | 存在且 worker 不持 `shm_ser_ipc*` 裸指针 | `grep -c "SerRequestRoute" src/dzIPC/shm_ser_cli_ipc.cc` > 0 |
| **D1 callback 出收包路径** | `recv_once()` 只做 `try_recv` + 分流 + 入队；callback/响应发送在 worker 之外（有界处理池或显式逃逸线程） | `grep -n "callback" <recv_once 实现所在行段>` 应 0 命中；方案 `:34` 已回写 |
| fork 闸 | pid 比对 + 不碰池锁 | 见 6.1-6 |
| B4 2s 常量 | 模块内收敛或如实上报未做 | `grep -n kPeerDeadTimeoutNs` |
| nodelet 二选一 | nodelet 启用时**不注册** fp_queue（`RecvWorkerPool` 无 wakeup 转发，C7） | `grep -n "IsNodeletEnabled\|fp_queue"` + 控制流核对 |
| 控制面未迁移如实上报 | S3 tick 迁移未执行 | `grep -n "ShmControlScheduler" src/dzIPC/shm_ser_cli_ipc.cc` = 0（现状如此） |

### 6.3 socket_pub_sub 专属（t4）

| 项 | 期望 | 检测 |
|---|---|---|
| `socket_sub_receive_state` / `process_received_wire` / `socket_receive_once` | 存在；worker 不持 `socket_sub_ipc*` 裸指针 | `grep -c` > 0 |
| 等待集合只登记 `subscriber_` | `ack_tx_` 不入组 | 人工核对 `add` 调用点 |
| `kSocketSubRecvTimeoutMs=50` | 不得缩短 | `grep -n "50" <chunk_rev_topic 调用>` |
| D4 `running` 闸重建 | `teardown_receive_path()` 后 `InitChannel` 重新置 true | `grep -n "running" <相关函数>` |
| A10 两个开关 | `DZIPC_SOCKET_COMPAT_THREAD=1` 强制兼容并打日志；`DZIPC_SOCKET_RECV_WORKERS` 生效 | `grep -n getenv <模块源>` 命中两者 |
| 无回调入 worker | `grep -c "std::function<" src/dzIPC/socket_pub_sub_ipc.cc` 在收包路径 = 0；无 `set_evict_cb` | 同方案 §7 的结构性核对法 |

### 6.4 socket_ser_cli 专属（t5）

| 项 | 期望 | 检测 |
|---|---|---|
| 首建：service 级 shared state + 固定 route | 存在 | `grep -c "SocketRecvRouteSource" src/dzIPC/socket_ser_cli_ipc.cc` > 0 |
| 只注册 `ipc_r_ptr_` | `ack_r_tx_` / 握手 socket 不入组 | 人工核对 |
| restart 新 generation | 完全注销旧 → 关旧节点 → 建连新节点 → 注册新 → 最后置 `data_plane_running_`；旧 fd 不复用 | 序列核对 + 无永久等待 |
| `ServerRevTime=200ms` 语义 | 组包边界不变；预算只在完整请求边界 | `grep -n ServerRevTime` |
| 开关与 t4 同名同义 | 避免同进程两条腿口径不同 | `grep -n "DZIPC_SOCKET"` |
| callback 执行中 stop | 默认等 callback 完成，禁止销毁后发响应 | 设计核对 + 测试证据 |

---

## 7. t7 执行计划与判定标准（依赖解锁后照此跑）

### 7.1 前置纪律

1. **A8**：一切验收前先 `cmake -S . -B build` + `make -j$(nproc)`，用**本次**构建的库与用例；禁止采信 `build/bin/*` 的既有读数与旧日志。
2. **环境变量坑**：验证 socket 前 `env | grep DZIPC_SOCKET`；读数可疑时一律 `env -u DZIPC_SOCKET_COMPAT_THREAD` 复核。
3. **只读审查**：t7 不得改产品代码（修复归 t8）；发现必须给出 `文件:行` + 命令 + 读数。

### 7.2 三维度判定规则（结论必须可判定）

| 维度 | 判定 | 不通过的写法 |
|---|---|---|
| A 方案符合性 | 每条必须保持的行为给出「模块源行号 ↔ 方案条目 ↔ 基准实现锚点」三元组；冲突处按 §2/§3 的「以谁为准」判定 | 只写「基本符合」不给行号 = 不合格 |
| B 配置/硬编码 | 每个常量与开关列出 `文件:行` + 生效检测；魔数散落、开关不生效、回退无日志、忙轮询降级均记 finding | 缺任一常量/开关的定位 = 不合格 |
| C 线程安全/资源 | owner CAS、池不持裸指针、注销 6 步顺序、析构/stop/restart 无永久等待、无悬挂 wait 项、fd 复用不误关联、callback 内不 unregister/stop/抛出、fork 闸 | 出现高/阻断级 finding ⇒ 明确写「不得进入收尾」 |

### 7.3 findings 结构（t8 的输入契约）

`{ID, 严重级(blocker/high/medium/low), 文件:行, 问题, 要求的修复, 证据命令+读数}`；每条必须可由 t9 独立复现。

预置 ID 段位（便于 t8/t9 引用）：`A-xx` 方案符合性、`B-xx` 配置/硬编码、`C-xx` 线程安全/资源、`S-xx` 共享层（转 ipc-transport）。

---

## 8. 附录：本轮复测命令与读数

```bash
cd /home/zwc/cpp_ipc_dds
git log --oneline -1                    # 0d1b672（t2 未提交）
git status --porcelain                  # 6 个 M + ?? 共享层/测试/文档（含新增 socket_recv_worker.{h,cc}）
grep -c SerRequestRoute src/dzIPC/shm_ser_cli_ipc.cc                    # 0
grep -c socket_sub_receive_state src/dzIPC/socket_pub_sub_ipc.cc        # 0
grep -c RecvRouteSource docs/消息接收架构改造/socket_pub_sub线程池移植方案.md   # 0
grep -c RecvRouteSource docs/消息接收架构改造/socket_ser_cli线程池移植方案.md   # 0
grep -nE "waitable|wait_handle|cancel_wait|clear_wait" include/libipc/udp.h  # 78/83/90/95
grep -n "udp_node_" include/dzIPC/common/data_rev.h                      # 289/291/293/295
grep -c "^add_test" test/CMakeLists.txt                                 # 11
ctest -N                                                                # Total Tests: 11
grep -cE "SerRequestRoute|socket_sub_receive_state|SocketReceiveWorkerPool" <(nm -DC build/lib/libipc.so)  # 0
grep -n "FD_SET(server_fd\|::select(server_fd" src/libipc/platform/posix/udp.h   # 346/352（D9 仍在）
grep -rn "register_subscriber(\|register_publisher(" src test | grep -v shm_control_scheduler.cc   # 仅 LocalPubSubRegistry（阶段 1 未接入）
grep -rn "DZIPC_SOCKET_COMPAT_THREAD" src include test                   # 0（A10 未闭环）
grep -c "kMaxFruitlessReadiness" src/dzIPC/threepools/socket_recv_worker.cc      # 1（常量定义处）
```

---

## 9. 本轮结论（不可当 t7 verdict 用）

- **t7 未执行**：依赖 t3/t4/t5 全部 pending，无被审代码，claim 被拒；本文件是前置准备，等待依赖解锁后按 §7 执行并另出 `docs/消息接收架构改造/交叉Review报告.md`。
- **共享层侧（契约符合性）已可判定**：9 纯虚签名逐字一致、平台宏边界 0、E1 阻塞语义已冻结、E2/E3 已落地、E1 读路径红线未触碰；**未闭环项**（21:4x 追补后修订）：~~C7~~（已裁定二选一，非 blocker）、~~A3/A7~~（已由 `8c6dd08` 闭合）、~~D9~~（已裁定为既有缺陷遗留项）—— 见 §0.6 与 §9.1。
- **模块侧最需要提前拉响的一条**：D1（`shm_ser_cli线程池移植方案.md:34` 的 `recv_once` 含 callback/响应发送）与需求 §1.2 直接冲突，若 t3 照方案实现即为 blocker 级 finding。
### 9.1 21:4x 追补（口径以本节为准，覆盖上文对应结论）

1. **A3/A7 已闭合**：基线前移到保护性提交 `8c6dd08`（25 files / +7914 −37），共享层六文件全部已跟踪（详见 §0.6(1)）；`git log -1` 不再是 `0d1b672`。
2. **D1 / C7 / D9 均已裁定**（详见 §0.6(2)）：D1 若 t3 把 callback 放进 `recv_once()` ⇒ **blocker 退回**；C7 **不再列 blocker**，只核「确实回退 + 无半吊子」；D9 **按既有缺陷登记遗留项、不计三模块扣分**。
3. **t12 新接口纳入核对**：`UDPNode::readable()` / `udp_node_readable()` / `chunk_rev_*` 的 `out_bytes` 重载，按「只加不改」核对（详见 §0.6(3)）。
4. 本文件由 284 行扩为 327 行，全部为**追加**：§0.1–§0.4 的历史读数未改写，仅在标题与状态块标注了「已被后续时间戳覆盖」的指向。