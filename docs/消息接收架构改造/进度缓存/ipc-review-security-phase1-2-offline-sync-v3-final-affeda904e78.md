# 阶段 1/2 改造：审查结论、修复状态、未修缺陷与交接清单（离线同步档 · 第 3 版 · 最终）

> 作者：`ipc-review-security`（代码审查与安全工程师，团队 `cppipc_team`），仓库 `/home/zwc/cpp_ipc_dds`。
> 产出时间：leader 掉线后，2026-09-23 20:5x CST（本轮实测窗口 20:54–21:00 CST）。
> 用途：**离线同步**。全文自包含，每条结论都带可自行核对的文件/行号/md5/命令锚点。
> 纪律：本文档只做**记录与汇总**，不代表任何未落地的改动已经生效。凡「未修复」都逐条标出。
> 本版取代第 2 版（deliverable `ipc-review-security-phase1-2-review-fix-offline-sync-v2-e05176acf8e2.md`，
> 亦见共享区 `ipc-review-security_phase1_phase2_offline_sync_v2_2026-09-23.md`，md5 `e6cf4b72abcc2bfd02bfb9752105d1c6`）。
> 相对第 2 版的**新增/更正**集中在 §6。

---

## 0 一句话状态

| 项 | 状态 | 说明 |
|---|---|---|
| 阶段 1 `ShmControlScheduler` | **已修复、已提交、全绿** | 提交 `82d3760`；`git diff HEAD` 对三文件为空；套件 20/20 |
| 阶段 2 `RouteSession` + `shm_sub_ipc` 接入 | **代码已落地、全绿，但未提交** | 6 个文件在工作区；套件 14/14 |
| ⚠️ 叫醒伪影缺陷 | **已确认成立、未修复、归属未裁定** | 见 §5；当前最需要接手的一件事 |
| 阶段 3 `process_received_buffer` | 未落地 | `grep -rn process_received_buffer src/ include/ test/` 为空 |
| 阶段 4/5（`recv_wait_set` / worker） | 未开始 | 仅说明文档 |
| ⚠️ 阶段 2 文件撕裂风险 | **存在** | 3 个未跟踪 + 3 个已改；`git checkout/clean` 会让接入方引用不存在的头 ⇒ 编译失败 |
| 阶段 1 三条点名回归用例 | **仍缺席** | 见 §4.3；修复已落地但无测试锚点 |

---

## 1 仓库现状（本轮实测锚点，2026-09-23 20:54–21:00 CST）

```
HEAD = 906d1c6  (dev, origin/dev)  feat(docs):更新共享内存线程调度器设计文档
82d3760  feat(thead pool):完成docs/消息接收架构改造/事件驱动线程池需求.md的阶段1(未验证)
         ← 阶段 1（threepools 三文件 + 套件 + 全部阶段文档）都在此提交
```

### 1.1 工作区状态（`git status --porcelain`，本轮实测）

```
 M include/dzIPC/shm_pub_sub_ipc.h
 M src/dzIPC/shm_pub_sub_ipc.cc
 M test/CMakeLists.txt
?? include/dzIPC/shm_route_session.h
?? src/dzIPC/shm_route_session.cc
?? test/test_shm_route_session.cpp
```

- `docs/消息接收架构改造/` **已提交**（`git status --porcelain docs/` 为空）。
- `build/` **未被 gitignore**（`git check-ignore -v build` rc=1），但也不在 `git status` 里 ⇒ 说明构建产物路径另有忽略规则覆盖，非本次改动面。

### 1.2 md5 与行数（离线核对用，本轮实测）

| 文件 | md5 | 行数 | 跟踪状态 |
|---|---|---|---|
| `include/dzIPC/threepools/shm_control_scheduler.h` | `6e02fa85a5cc99007974e98473ced664` | 348 | 已提交 |
| `src/dzIPC/threepools/shm_control_scheduler.cc` | `6f50018db071a88082093066b7ce8823` | 870 | 已提交 |
| `test/test_shm_control_scheduler.cpp` | `2711232e8db72661cbd809b1449af99d` | 1093 | 已提交 |
| `include/dzIPC/shm_route_session.h` | `31a09c203935f2d6ae41ad1cac496ccf` | 138 | 未跟踪 |
| `src/dzIPC/shm_route_session.cc` | `5706eec2398d09f1827f21b05d20b731` | 168 | 未跟踪 |
| `test/test_shm_route_session.cpp` | `c1715ae8f148c67a0ee887f9c43984e4` | 673 | 未跟踪 |
| `include/dzIPC/shm_pub_sub_ipc.h` | `9d3d86a51be805449bce159ea85262c2` | 207 | 已改 |
| `src/dzIPC/shm_pub_sub_ipc.cc` | `31e8dc8ce27768e58e39c56310488af1` | 1086 | 已改 |
| `test/CMakeLists.txt` | `b4049aaa4080240313dc100116e34ecb` | — | 已改 |

阶段 1 三文件与 HEAD **逐字节一致**：

```bash
$ git diff --stat HEAD -- include/dzIPC/threepools src/dzIPC/threepools test/test_shm_control_scheduler.cpp
（空输出）
```

⇒ 阶段 1 的修复态**已被提交固化**，工作区无未提交的阶段 1 改动。
（本成员早前审查报告里的 `421fb56c…` 是修复**中途**的快照，非最终态；最终态是上表 `6e02fa85…`。）

### 1.3 本轮独立实测（非引用他人报告）

```bash
$ cmake -S . -B build                                      # rc=0
$ cmake --build build --target test_shm_control_scheduler test_shm_route_session -j4
                                                           # rc=0，warning 计数 = 0
$ ctest --test-dir build -R "test_shm_route_session|test_shm_control_scheduler" --output-on-failure
    Start 2: test_shm_control_scheduler .......   Passed    3.56 sec
    Start 3: test_shm_route_session ...........   Passed    1.32 sec
100% tests passed, 0 tests failed out of 2
```

单套件读数：

```
./build/bin/test_shm_control_scheduler   → [ PASSED ] 20 tests.  (3559 ms)
./build/bin/test_shm_route_session       → [ PASSED ] 14 tests.  (1315 ms)
```

`ctest -N`（工作区 `test/CMakeLists.txt`，第 126/134/147 行）：

```
Test #1: test_udp_port_boundary
Test #2: test_shm_control_scheduler
Test #3: test_shm_route_session          ← 该 add_test 未提交
Total Tests: 3
```

⚠️ HEAD 版 `test/CMakeLists.txt` **只含前两项**。工作区这份一旦丢失，
`test_shm_route_session` 会退出 ctest 自动回路（手工跑 `build/bin/...` 进不了任何自动回路）。

**邻接回归（本批改动直接命中面，本轮实测）**：

```
test_dzipc_shm            → [ PASSED ] 10 tests.  (4939 ms)
test_shm_domain_isolation → [ PASSED ]  3 tests.  (2409 ms)
test_shm_nodelet          → [ PASSED ] 13 tests.  (2313 ms)
test_nodelet_switch       → [ PASSED ]  8 tests.  (1148 ms)
test_shm_receiver_cap     → [ PASSED ]  2 tests.  (6361 ms)
test_wire_accept          → [ PASSED ]  9 tests.  ( 403 ms)
合计 45 条，0 失败
```

**`docs/hotpath_gate.sh`（本轮实测，rc=0 过闸）**：

```
门 1: payload=1048576  pubsub_shm_1048576B_tput  1837 msg/s  1837.1 MB/s  丢包= 0.00%
      payload=64       pubsub_shm_64B_tput       171766 msg/s   10.5 MB/s  丢包=77.22%
门 2: 邻接回归 8 套件全部 OK=1/3/3/2/10/9/8/4
```

> ⚠️ 门 1 的 `64B 丢包=77.22%` 与 `p999=3715us` 属**既有**现象（64B 高速 pub/sub 队列满即丢，非本次改动面）。
> 本成员**未做**改动前基线对比，故该读数**不构成**回归判据，仅如实记录。

---

## 2 本成员的任务链与产出

### 2.1 任务链（全部已正式回报）

| 任务 id | 类型 | 内容 | 结论 |
|---|---|---|---|
| `cpp_ipc_team-1790076458158056713-4` | 只读 | 审查阶段 1 需求与仓库现状 | 已回报（deliverable 被只读约束拒绝） |
| `cpp_ipc_team-1790077942318026918-8` | 只读 | 审查 `ShmControlScheduler` 设计/实现 | 已回报，**结论：不通过** |
| `cpp_ipc_team-1790088188169163074-2` | 写 | 修复并发/生命周期缺陷 + 构建验收 | 已回报 |
| `cpp_ipc_team-1790092796076413783-5` | 写 | 修 `SameRoundUnregisterSkipsDispatch` | 已回报 |
| `cpp_ipc_team-1790095790189527803-9` | 写 | 同上（直接修复并验证，含变异测试） | 已回报 |
| `cpp_ipc_team-1790157383793045195-2` | 只读 | 审查 `shm_sub_ipc`，定位 RouteSession 接入点 | 已回报 |
| `cpp_ipc_team-1790162700732065559-6` | 只读 | 调查叫醒伪影投递问题 | 已回报（**缺陷成立**） |

`member_get_my_task` 现在返回 **"has no unfinished task"**（无未完成任务）。

### 2.2 本成员的 deliverable（团队存储内，id 稳定）

| deliverable id | 大小 | 内容 |
|---|---|---|
| `ipc-review-security-phase1-shm-control-scheduler-readonly-review-be73b0792254.md` | 25384 B | 阶段 1 设计/实现分级审查：P0 阻断 1 项、P1 硬缺陷 4 项（F1–F4）、P2 中风险 8 项、P3 低风险 6 项 |
| `ipc-review-security-review-phase1-shm-control-scheduler-555b4243ad85.md` | 11861 B | 阶段 1 实现复审：**不通过**（B1 编译失败 + M1–M6），含 G0–G3 验收门槛 |
| `ipc-review-security-phase1-2-review-handoff-offline-sync-5349f58557ed.md` | 24206 B | 交接档第 1 版（已被取代） |
| `ipc-review-security-phase1-2-review-fix-offline-sync-v2-e05176acf8e2.md` | 33023 B | 交接档第 2 版（已被本文件取代） |
| **本文件** | — | 交接档第 3 版（最终） |

---

## 3 阶段 1 审查结论（本成员产出，已回报）

### 3.1 设计/实现分级审查（`...be73b0792254.md`）

- **P0（阻断）**：被审查的「实现对象不存在」——设计报告引用的行号与仓库现状不符。
- **P1（硬缺陷，编码前必须闭口）4 项**：
  - **F1** 异常路径下 `tick_inflight` 不结算 ⇒ 兜底 `catch` 引入**永久死锁**，且同轮后续项一起挂住。
  - **F2** `sub_handshake`/`pub_handshake` **循环外**的清理代码无归属 ⇒ PeerSlot 泄漏耗尽 / `TopicState` 卡在 Ready。
  - **F3** 稳态 tick 时长**无上界预算、无验收判据** ⇒ 心跳抖动可能逼近 2s，触发 UF-003 误杀回归。
  - **F4** 线程 QoS 在控制面路径**静默失效**（需求 §1.2 未覆盖）⇒ 需 leader 裁决。
- **P2 中风险 8 项**，含 **P2-1 CMake 落点风险**：`src/dzIPC/threepools/` 缺 `aux_source_directory` ⇒ 新 `.cc` **永不编译**（已修，见 §4.1）。
- **P3 低风险/文档一致性 6 项**。

### 3.2 实现复审（`...555b4243ad85.md`）—— 结论：不通过

- **B1（阻断）**：编译/链接失败 —— 头文件声明了 `RegistrationToken::reset()` 与
  `operator=(RegistrationToken&&)`，`.cc` 中**无定义**。实测未定义符号：

  ```
  U dzIPC::shm_control::RegistrationToken::reset()
  U dzIPC::shm_control::RegistrationToken::operator=(dzIPC::shm_control::RegistrationToken&&)
  ld: undefined reference ... collect2: error: ld returned 1 exit status
  ```

  同时测试用 `RegistrationToken(&sched, ...)`（指针签名）而头文件曾改为引用签名 ⇒ 20 处调用点不匹配。
- **M1–M6**：并发/生命周期缺陷（回调内 `stop` 自 join、并发 `stop` 返回语义、令牌比调度器长寿等）。

---

## 4 阶段 1 修复：已落地内容（可在 HEAD 中逐条核对）

### 4.1 修复落点与核对命令（本轮实测行号）

| 缺陷 | 修复落点（实测行号） | 核对命令 |
|---|---|---|
| B1 `reset()`/`operator=` 无定义 | `.cc:829` `RegistrationToken::reset()`、`.cc:853` `operator=` | `grep -n "RegistrationToken::reset\|RegistrationToken::operator=" <cc>` |
| 编译阻断：`Impl` 可见性 | 头 `:233` `struct Impl;`、`:234` `share_impl`（均在**公开**区）、`:247` `impl_`、`:340` 令牌 `impl_` | `grep -n "struct Impl;\|share_impl" <h>` |
| 回调内 stop 自 join ⇒ terminate | `.cc:709` `stop()` 重写；`.cc:728` 的 `t_on_control_worker` 分支只置停止标志、**不取 join 权**、不等待 | `grep -n "t_on_control_worker" <cc>`（26/107/193/555/728） |
| 并发 stop 返回语义 | `stop()` 拆 **三状态**：`stopped`(atomic) + `stopping`(bool) + `stop_joining`/`stop_joined` + 独立 `stop_mtx`/`stop_cv`（`.cc:116-119`）；非 joiner 在 `.cc:743-747` 等 `stop_joined`；joiner 在 `.cc:774-781` 同临界区落 `running=false` + `stop_joined=true` | `grep -n "stop_joining\|stop_joined" <cc>` |
| 令牌与独立调度器析构顺序 | 令牌持 `std::shared_ptr<Impl>`（头 `:340`），`reset()` 走 `impl_->unregister_entry(id_)`（`.cc:842`）；`.cc:545` 新增 `Impl::unregister_entry`；公开 `unregister` 在 `.cc:691` **委托同一条路径**（`.cc:695`） | `grep -n "unregister_entry" <cc>`（545/693/695/842/845） |
| P2-1 落点风险 | `src/CMakeLists.txt:20` `aux_source_directory(.../src/dzIPC/threepools SRC_FILES)`（配置期展开 ⇒ 必须重跑 cmake） | `grep -n threepools src/CMakeLists.txt` |
| 同轮注销语义（`SameRoundUnregisterSkipsDispatch`） | 实现侧把同轮 due 按 **纯 `EntryId` 升序** 排序（`.cc:460-464`）；回调内注销**立即摘除** entries（不再只靠下一轮 tick 惰性清理） | 见 §4.2 |

### 4.2 同轮注销：根因与修法（**更正第 1 版交接档**）

**根因**：用例原注释断言「`std::unordered_map` 桶序与插入序一致，故 A 一定先于 B 被 dispatch」——
**该前提不成立**。实测 libstdc++ 对连续 key **逆序**遍历桶：

```
m.emplace(1,1); m.emplace(2,2);   →  iteration order: 2 1
```

于是 B（id=2）先于 A（id=1）被 dispatch。`dispatch` 的 inactive 复查本身**是对的**；
错的是用例的时序前提与 `entry_count()==0` 断言（只注销了 B，A 仍在册 ⇒ 正确值是 1）。

**修法两条（都已落地）**：

1. **实现侧定序**：`.cc:460-464` 用 `std::sort` 按 **纯 `EntryId`** 排序 `selected`。
   `.cc:435-448` 的注释**显式否决**了 EDF（`(next_due, EntryId)`）：同批各项 `next_due` 只差
   注册调用的微秒级时钟差 ⇒ 「谁先跑」取决于 `ControlClock::now()` 的分辨率，是隐式且不可控的前提。
   纯 `EntryId` 让「注册顺序 = 回调顺序」成为**无前提**的确定性契约（已知代价：同批混不同周期项时
   失去「更逾期者优先」，阶段 1 同批 `next_due` 仅差微秒级，可忽略）。
2. **用例侧自证前提**：`SameRoundUnregisterSkipsDispatch`（`test:510`）改为**运行期批次探针**版，
   用「A、B 首次回调携带**同一个 t0**」自证同批，不再依赖容器迭代序；判据是
   「B 在 A 动手的那一批（`action_t0`）里**没有**回调记录」。

**新增守门用例**：`SameRoundDispatchOrderFollowsRegistrationOrder`（`test:738`）——
用 `std::map<t0, vector<EntryId>>` 记录每批回调序列，要求**至少两个完整批次**，
且逐批断言 `[first_id, second_id]`（注册序）。删掉 `.cc` 的排序，它会直接变红。

**变异测试结论**（本成员实测）：
- 去掉 `dispatch` 的 inactive 复查 ⇒ 用例**变红**（守门有效）。
- 去掉「回调内立即摘除 entries」（只留惰性清理）⇒ **仍通过** —— 惰性清理太快，外部读数抓不住。
  因此 I3 判据已改为在**回调内**读，使其确定性。

### 4.3 ⚠️ 遗留缺口：任务点名的三条回归用例仍缺席（本轮复核）

任务 `-2` 点名要求覆盖三个缺陷，但下列用例名在测试文件中**一次都没有出现**（本轮复核，20:58）：

```
$ for n in CallbackInternalStopDoesNotDeadlock ConcurrentStopWaitsForJoin TokenOutlivesScheduler; do
    printf "%-40s %s\n" "$n" "$(grep -c "$n" test/test_shm_control_scheduler.cpp)"; done
CallbackInternalStopDoesNotDeadlock      0
ConcurrentStopWaitsForJoin               0
TokenOutlivesScheduler                   0
```

现有点名用例是 `StopAndWakeupSemantics`（`test:868`，含幂等 `stop()` 两次调用）、
`DtorStopsWorkerAndReleasesEntries`（`test:961`）、
`StopWakesIdleWorkerWithoutDeadlock`（`test:997`，fork 子进程 + 父侧硬超时）。

⚠️ 需注意 `StopAndWakeupSemantics` 覆盖的是**单线程**「stop 后 `worker_active()==false` + 幂等」，
**不覆盖**并发 `stop()` 的返回语义（N 线程同时 `stop()`，全部返回后 `worker_active()` 必须为 false）。

⇒ **修复已落地但缺对应回归用例**：三条缺陷的修复若被将来重构破坏，现有 20 条用例**不一定能变红**
（尤其「回调内 stop 不得吞 join 权」这条，只有实现注释与设计论证，没有测试锚点）。建议补：

1. `CallbackInternalStopDoesNotDeadlock` —— 在 `on_sub_heartbeat` 内调 `stop()`，
   断言不 terminate、`stop()` 返回后 worker 仍由**外部**线程 join 成功。
2. `ConcurrentStopWaitsForJoin` —— N 线程并发 `stop()`，断言**全部返回后** `worker_active() == false`。
3. `TokenOutlivesScheduler` —— 令牌比调度器长寿（ASan 下跑），断言无 use-after-scope。

**本成员在修复期已在隔离副本（`/tmp/b1`、`/tmp/b2`）中验证过这三条路径可复现/可修**
（缺陷态 `Resource deadlock avoided` rc=134、ASan `stack-use-after-scope`；修复态 rc=0），
但**未把这三条用例落进仓库**（当时的任务边界只要求修缺陷 + 构建验收）。

### 4.4 阶段 1 套件用例清单（20 条，本轮实测行号）

```
144 TimingSemanticsUnchanged                     393 UnregisterIsIdempotent
181 PublisherHeartbeatWithoutPeers               424 CallbackExceptionIsIsolated
201 CustomTimingIsHonored                        452 CallbackInternalUnregisterDoesNotDeadlock
230 CustomPeerDeadTimeoutIsPassedThrough          510 SameRoundUnregisterSkipsDispatch
248 RegisterDoesNotAddThreads                     738 SameRoundDispatchOrderFollowsRegistrationOrder
282 UnregisterStopsNewCallbacks                   847 StatsExposeTickCost
300 UnregisterWaitsForInflightTick                868 StopAndWakeupSemantics
325 UnregisterReleasesState                       910 WakeupDoesNotSpuriouslyTick
346 ConcurrentChurnIsRaceFree                     934 RegistrationTokenIsMoveOnly
                                                 961 DtorStopsWorkerAndReleasesEntries
                                                 997 StopWakesIdleWorkerWithoutDeadlock
```

### 4.5 F1–F4 的闭口状态（本轮复核）

| 项 | 状态 | 证据 |
|---|---|---|
| **F1** 异常路径 `tick_inflight` 不结算 | **已闭口** | `CallbackExceptionIsIsolated`（`test:424`）在册；`.cc` 的兜底 `catch` 会摘除坏项 |
| **F2** 循环外 PeerSlot 清理无归属 | **已闭口** | `sub_handshake` 尾部有兜底：`.cc:787` `remove_peer(attached_generation)`、`:790-791` `release_peer_slot(peer_slot_)` + `peer_slot_ = -1`（在 `while` **之外**） |
| **F3** tick 时长无上界预算 | **部分开放** | `Stats` 已提供**观测**：头 `:153` `tick_overrun_count`、`:157` `tick_duration_max_ns`（`StatsExposeTickCost` 在册）；但**无**「单轮预算」判据与超限处置 —— F3 的「无验收判据」部分仍成立 |
| **F4** 控制面线程 QoS 静默失效 | **未处理** | `grep -rn "qos\|QoS\|nice\|sched_setscheduler" src/dzIPC/threepools/*.cc include/dzIPC/threepools/shm_control_scheduler.h` 为空（仅第三方 `bs_thread_pool.h` 内有无关实现）⇒ **仍需 leader 裁决** |

### 4.6 ⚠️ 注释残留（不影响行为，但会误导读者；本轮实测）

1. `test/CMakeLists.txt:128` 仍写「阶段 1 控制面调度器聚焦用例 **(16 条)**」，实际 **20 条**。
2. `test/test_shm_control_scheduler.cpp` 有 **5 处**旧排序键注释残留（声称 `(next_due, EntryId)`）：

   ```
   497:  * A、B 都是 10ms 周期、A 先注册。tick 每轮把**同一批**到期项按 (next_due, EntryId)
   671:     *    (next_due, EntryId) 排序后回调，A 先跑是确定性的契约。 */
   714: /* tick 把同批到期项按 (next_due, EntryId) 排序后**依次**回调（见 .cc 中 tick 的
   835:     << "同批内先注册者必须排在前面（tick 的 (next_due, EntryId) 排序契约）";
   837:     << "同批内后注册者必须排在后面（tick 的 (next_due, EntryId) 排序契约）";
   ```

   实际契约是**纯 `EntryId` 升序**（§4.2）。这 5 处是**纯注释/断言消息文本**，不影响判据逻辑，
   但会让接手者按错误契约改实现（这正是第 1 版交接档犯过的错）。

---

## 5 ⚠️ 未修复缺陷：叫醒伪影被当作 `msg_id==0` 的 TLV 投递

**这是当前最需要接手的一件事。** 完整调查见任务 `-6` 的回报；此处给结论、机理、修复方案与判据。

### 5.1 结论

`disconnect()` 叫醒的 `recv` 返回「`size() == ipc::data_length`(64) 的**全零**缓冲」，
`raw_data.empty()` 判不住；当话题 `msg_id == 0` 时 `AcceptWire` 三道门**恰好全过** ⇒
一条全零假消息被 `push` 进 `msg_queue_`，用户侧 `try_get_clone` 返回 true。

**触发条件默认命中**：`msg_id_` 来自 `TopicDataPtrMake<T>()` 的默认实参 `0`
（`include/dzIPC/dzipc.h:82`）⇒ 所有未显式指定 msg_id 的 SHM 话题。

**暴露面在运行期**（不只是析构）：

| 路径 | 位置 | 时机 |
|---|---|---|
| `begin_rebuild` 第 3 步 `old->disconnect()` | `src/dzIPC/shm_route_session.cc:61` | **运行期**（generation 变化、Ready 后首次 attach） |
| `stop_and_wake` ← `add_peer` 失败 | `src/dzIPC/shm_pub_sub_ipc.cc:681` | **运行期** |
| `stop_and_wake` ← 控制面离开 Ready | `src/dzIPC/shm_pub_sub_ipc.cc:764` | **运行期** |
| `stop_and_wake` ← 析构 | `src/dzIPC/shm_pub_sub_ipc.cc:560` | 析构期（危害较小） |

⇒ 每次 generation 重建/控制面抖动，`msg_id==0` 的话题可能给用户塞一条全零消息，**不崩、不报错**。

### 5.2 机理（逐行，可自查）

`src/libipc/ipc.cpp:1643 recv()`：

1. `:1661` `typename queue_t::value_t msg{};` —— **值初始化，全零**
2. `:1663` `wait_for(inf->rd_waiter_, pred, tm)`
3. `:1044-1055` `for (unsigned k=0; pred();) { ret = waiter.wait_if(...); if(!ret) return false; if (k==0) break; } return true;`
4. `src/libipc/waiter.h:107-110` `while (!quit_.load(...) && pred())` —— `quit_` 为真 ⇒ **pred 一次都不调**
5. `waiter.h:120` ⇒ `wait_if` **`return true`**（不是 false）
6. `ipc.cpp:1053` `k` 被 lambda 置 0 ⇒ `break` ⇒ **`wait_for` `return true`**
7. `:1672` `que->pop` **从未被调用** ⇒ `msg` 仍是第 1 步的全零
8. `:1684` `msg.cc_id_==0`，`inf->cc_id_!=0` ⇒ 不 continue
9. `:1689` `r_size = 64 + msg.remain_(0) = 64 > 0`
10. `:1699` `msg.storage_` 全零 false ⇒ 跳过 large 路径
11. `:1757` `return make_cache(msg.data_, 64)` ⇒ **`size()==64`、全零、`empty()==false`**

`waiter::quit_` 只在 `open()` 复位（`waiter.h:66`）；`disconnect_receiver()`（`ipc.cpp:1112-1120`）
置 `quit_` 后 `init()` 因 waiter 仍 `valid()` 不重开（`ipc.cpp:167-172`）⇒ **`quit_` 粘性**。
**只有叫醒路径产零填充**；超时路径 `wait_if` `return false` ⇒ `recv` 返回 `{}`（空）。

`AcceptWire`（`src/dzIPC/common/wire_accept.cc`）对全零 64B：

| 门 | 行 | 结果 |
|---|---|---|
| `looks_like_dzflat` | `:11` | `magic(0) != kMagic(0x4C465A44)` ⇒ false |
| `has_dzflat_magic` | `:34` | 同上 ⇒ false（不是"损坏段"） |
| `sink.check_id(raw, expected)` | `:40` | 读**尾部 4 字节** = 0 ⇒ `0 == expected` **当且仅当 `expected==0`** |
| `sink.deserialize` | `:45` | 生成代码所有 count 读得 0 ⇒ `tods_count_ok` 全过、`resize(0)` 安全 |
| `deserialize_ok()` | `:50` | **true** ⇒ `NoteDzFlatRx(kTlvAccepted)`(`:55`) + **`return true`** ⇒ 投递 |

⚠️ 嵌套子对象的越界**不传播**到外层 sink 的 `_deser_overflow`。
`msg_id != 0` 的话题：消息未污染，但**计数器被污染**（无消息却记一条 `kTlvIdSkipped`）。

### 5.3 为什么现有 RouteSession API 判不出（方案筛选关键）

| 候选判据 | 为何不成立 |
|---|---|
| `lease.generation` 落后 | 不变式 I5（`shm_route_session.h:30-32`）与说明 §3 明令**禁止**据此丢弃（真消息可能在 disconnect 前已弹出） |
| `current_route() != lease->route` | 同上，且有时序窗口 |
| `!lease->route->valid()` | `disconnect()` 只改 `connected_`，**不清 `h_`**（`ipc.h:231-237`）；`release()` 才清 |
| `!que->connected()` | `recv` 的 pred 会 `reconnect(&h,true)`（`ipc.cpp:1667-1670`）重新连上，且不复位 `quit_` |

### 5.4 最窄正确的修复方案（**建议接手项**）

**承重判据**：一次 `recv` 返回伪影 ⟺ 该次 recv 期间 route 被 `disconnect()` 过 **且**
返回值是 `size() == ipc::data_length` 的**全零**缓冲。

「全零」是**充要**的，不会误伤任何真实消息（含负载恰好全零的真实消息）：

- 真实 **TLV** wire 页尾 12 字节里 `page_cnt` 从 **1** 起（注释明写 "page start from 1, 0 is reserved"）
  ⇒ 不可能全零；
- 真实 **DZFlat** 段首是 `magic='DZFL'` ⇒ 不可能全零；
- **large message**（`storage_`）的 buff 同样是完整 wire（含页尾）⇒ 不可能全零；
- 伪影的 `msg{}` 是值初始化 ⇒ 全零，`size` 恒 64。

**落点**：`src/dzIPC/shm_pub_sub_ipc.cc` 的 `:836` `if (raw_data.empty())` **下一行**、
双 wire 分流之前（当前 `:836-838` 只有判空 + `continue`，门**未落地**，见 §5.6）：

```cpp
if (raw_data.empty()) { continue; }
/* ⛔ 叫醒伪影门（阶段 2 §5 第 4 步的必然副产物）:
 * disconnect() → waiter::quit_waiting() 置粘性 quit_, 使 wait_for 短路返回 true,
 * 而 recv 里的 msg{} 从未被 pop 填充 ⇒ 返回 ipc::data_length 字节的**全零**缓冲,
 * empty()==false 挡不住。全零是**充要**特征: 真实 TLV wire 的页尾 page_cnt 从 1 起、
 * DZFlat 段首是 'DZFL', 都不可能全零 —— 故不会误伤任何真实消息(含全零负载的)。
 * 不按 generation 判(违反 I5: 真消息可能已在 disconnect 前弹出)。 */
if (raw_data.size() == ipc::data_length && is_wakeup_artifact(raw_data)) { continue; }
```

`is_wakeup_artifact` 放匿名 namespace（约 8 行，**只查页尾 12 字节**即可，更快且已充分）：

```cpp
bool is_wakeup_artifact(const ipc::buff_t& b) noexcept
{
    const std::size_t n = b.size();
    if (n < 12) return false;
    const auto* p = static_cast<const unsigned char*>(b.data());
    if (p == nullptr) return true;
    for (std::size_t i = n - 12; i < n; ++i) { if (p[i] != 0) return false; }
    return true;
}
```

开销：正常路径一次 `size()==64` 比较，命中才 12 字节扫描。**零 API 变更、零 libipc 变更。**

**关键边界**：这道门必须留在**收包循环里、与判空同层**，**不得**搬进阶段 3 的
`process_received_buffer` —— 否则阶段 5 的 worker 会各自复制一份或漏掉一处。

**根因层（记录，本阶段不做）**：真正根因在 `waiter::wait_if`（`waiter.h:104-121`）——
quit 短路与「pred 满足」**返回值不可区分**（都 `return true`）。但 `wait_if` 的返回值同时被
`send`（`ipc.cpp:1399-1420`）与 `wait_for_recv`（`:1282`）消费 ⇒ 是 libipc **公共等待语义变更**；
且阶段 2 说明 §1 明确「不改 libipc 的 recv 实现」⇒ 交回 libipc 侧单独评审。

### 5.5 回归判据（★ 必须做变异测试：删门 ⇒ 变红）

1. ★ **单测（确定性、无发送方）**：已有 `test/test_shm_route_session.cpp:441`
   `StopAndWakeUnblocksBlockedRecv` 守住「返回值只可能空或零填充」。**补一条**把该返回值
   喂进分流门，断言**不入 `msg_queue_`、`kTlvAccepted` 不涨**。
2. ★ **`msg_id==0` 端到端**：`TopicDataPtrMake<T>()`（默认 0）+ `IPC_SHM`，`InitChannel()` 后
   触发一次 generation 重建（或 `stop_and_wake`）。断言用户侧 `try_get_clone` **不返回**那条
   全零消息（当前实现下必返回 true ⇒ 未修复必红）。
3. **阴性对照（防判据过宽）**：同一 `msg_id=0` 话题发一条**真实** TLV 消息（负载可含零）
   ⇒ **必须投递**。守住「不许把真消息也丢了」。
4. ★ **计数判据**：无真实消息时触发一次重建，断言 `tlv_accepted`（`msg_id==0`）或
   `tlv_id_skipped`（`msg_id!=0`）**不增**。可用现成 `test/test_wire_accept.cpp` 的 `Snap` / `EXPECT_ONLY`。
5. **I5 不被破坏**：重建过程中**已弹出**的真消息仍投递 —— 判据不得退化成「generation 落后就丢」。
6. **计数不重复**：修复后那次伪影**不记任何** `DzFlatRxEvent`（与「根本没收到」一致）。

### 5.6 现状核对（门**未落地**，本轮实测）

```
$ grep -c "is_wakeup_artifact" src/dzIPC/shm_pub_sub_ipc.cc
0
$ sed -n '836,838p' src/dzIPC/shm_pub_sub_ipc.cc
                    if (raw_data.empty())
                    {
                        continue;
```

⇒ 判空之后**直接**进入 `:840` 的双 wire 分流，中间没有门。

### 5.7 队友侧已有线索（离线交叉核对用）

`test/test_shm_route_session.cpp` 抬头注释（`:35-46`）**已经记录了本缺陷**，并声明
「本文件不断言该缺陷的行为，只用 `empty || 零填充伪影` 守住『不得返回非零负载』这条更弱的界」。
`StopAndWakeUnblocksBlockedRecv`（`:441`）的断言里 `zero_filler` 判据（`:506`）即为此。
⇒ 阶段 2 的测试**已知**该缺陷存在，修复时应把 `:441` 那条更弱的界升级为「伪影不得入队」。

另：`ipc-protocol_交接同步_精简版.md`（共享区，md5 `46098cbaa3c952f695d4e2fc408aa063`）
也独立记录了同一缺陷，并指出「两方独立复现（另见 `ipc-test-perf-...-b29fc18e0a8e.md` §6）」。

---

## 6 相对第 2 版（`...e05176acf8e2.md`）的新增与更正

第 2 版的全部锚点本轮**复核仍然有效**（§1.2 的 md5 逐项一致）。以下是本轮新增或需要更正的部分：

| # | 项 | 内容 |
|---|---|---|
| 1 | **新增**：F2 已闭口 | §4.5 —— `sub_handshake` 尾部 `.cc:787/790-791` 有循环外兜底，第 2 版未给结论 |
| 2 | **新增**：F3 部分开放 | §4.5 —— `Stats` 有 `tick_overrun_count`/`tick_duration_max_ns`（**观测**），但无单轮预算判据与超限处置 |
| 3 | **新增**：F4 未处理 | §4.5 —— 控制面路径 `grep` 无线程 QoS 实现，**仍需 leader 裁决** |
| 4 | **新增**：注释残留 2 类 | §4.6 —— `test/CMakeLists.txt:128`「16 条」（实际 20）；测试文件 5 处旧排序键 `(next_due, EntryId)` 注释（497/671/714/835/837） |
| 5 | **新增**：本轮独立复跑 | §1.3 —— ctest 3/3、邻接回归 6 套件 45 条全过、`hotpath_gate.sh` rc=0 过闸 |
| 6 | **更正**：`StopAndWakeupSemantics` 不覆盖并发 stop | §4.3 —— 它只覆盖单线程「stop 后 `worker_active()==false` + 幂等」 |
| 7 | **更正**：`build/` 未 gitignore | §1.1 —— `git check-ignore -v build` rc=1；不影响结论，但第 2 版未提 |
| 8 | 维持 | §5.6 叫醒伪影门**仍未落地**（`grep -c` = 0，本轮复核） |
| 9 | 维持 | §4.3 三条点名用例**仍缺席**（`grep -c` = 0，本轮复核） |

---

## 7 阶段 2：RouteSession 与 `shm_sub_ipc` 接入（已落地、未提交）

### 7.1 模块

| 文件 | 说明 |
|---|---|
| `include/dzIPC/shm_route_session.h`（138 行） | `RouteSession` 协议：`ReceiveLease`/`acquire_receive`/`release_receive`/`begin_rebuild`/`stop_and_wake`/`wait_quiescent`/`current_route`/`generation`；不变式 I1–I5；接口语义裁定（`stop_and_wake` **幂等且非终态**） |
| `src/dzIPC/shm_route_session.cc`（168 行） | 实现。**`begin_rebuild` 第 3 步 `old->disconnect()`（`:61`）在锁外**；`stop_and_wake` 的 `cur->disconnect()`（`:139`）同样在锁外 —— 两者都是叫醒伪影的产生点（§5.1） |
| `test/test_shm_route_session.cpp`（673 行，14 条用例） | lease 配对 / 重建顺序 / stop-wake；含变异验证（队友报告 `ipc-test-perf-phase2-route-session-test-report-b29fc18e0a8e.md`） |

落点在 `src/dzIPC/` 下 ⇒ 被现有 `aux_source_directory(${...}/src/dzIPC)` 收编，
**无需**改 `src/CMakeLists.txt`（与阶段 2 说明 §1 一致）。

### 7.2 接入点（`src/dzIPC/shm_pub_sub_ipc.cc`，本轮实测行号）

| 行 | 调用 | 对应说明 § |
|---|---|---|
| `560` | `stop_and_wake()`（析构） | §5 第 4 步 |
| `587` / `589` | `wait_quiescent()` / `current_route()` 后 `release()` | §5 第 6–7 步 |
| `666` | `begin_rebuild(generation, create)` | §4 表格第 1 行 |
| `681-683` | `stop_and_wake()` + `wait_quiescent()` + `current_route()`→`release()`（`add_peer` 失败） | §4 表格第 2 行 |
| `699` | `current_route()` 后读 `connected_id()` | §4 表格第 3 行 |
| `764-766` | `stop_and_wake()` + `wait_quiescent()` + `current_route()`→`release()`（控制面离开 Ready） | §4 表格第 4 行 |
| `826` / `835` | `acquire_receive()` / `release_receive()`（订阅循环，`recv` 在 `:833`，判空在 `:836`） | §3 |

`channel_mtx_` 与 `subscriber_` 已**整体删除**（本轮实测）：

- `.cc` 中 `grep -n channel_mtx_` = **1**（仅 `:663` 一条历史注释）；
- 头文件仅 `:177` 一条注释；`RouteSession route_session_;` 在头 `:182`；
- `subscriber_` 在 `.cc` 仅剩 `:218` `reg.subscriber_snapshot(key)`（**同名不同义**，非本类成员）。

### 7.3 阶段 2 边界（两阶段同改同一 lambda）

- 阶段 2 只拥有 `:786-802`（recv 前后）；阶段 3 只拥有 `:803-946`（分流体）。
- 阶段 3 的 `process_received_buffer` **未落地**（`grep -rn` 为空）。

### 7.4 阶段 2 用例清单（14 条，本轮实测行号）

```
128 EmptyStateIsWellDefined                       419 StopThenSuccessfulRebuildReopensLeases
148 AcquireLeaseCarriesRouteAndGeneration          441 StopAndWakeUnblocksBlockedRecv
172 WaitQuiescentBlocksWhileInflightNonZero        519 StopThenQuiesceReturns
198 ExtraReleaseDoesNotUnderflowQuiescence         552 StopIsIdempotentAndNonTerminal
241 RebuildRejectsNewAcquire                       584 DtorReleasesOnlyItsOwnShare
303 RebuildWaitsForInflightBeforeReleasingOldRoute 609 ConcurrentAcquireReleaseWithRebuildIsRaceFree
364 RebuildSwapsRouteAndPublishesNewGeneration
389 RebuildFailureLeavesEmptyAndRetryable
```

### 7.5 ⚠️ 阶段 2 的文件撕裂风险（交接最高优先）

`shm_route_session.{h,cc}` + `test_shm_route_session.cpp` 是**未跟踪**；`shm_pub_sub_ipc.{h,cc}` 是**已跟踪已改**。
⇒ `git checkout` / `git clean` 会让「接入方引用不存在的头」⇒ **编译失败**。

```bash
git add include/dzIPC/shm_route_session.h src/dzIPC/shm_route_session.cc \
        test/test_shm_route_session.cpp test/CMakeLists.txt \
        include/dzIPC/shm_pub_sub_ipc.h src/dzIPC/shm_pub_sub_ipc.cc
```

提交分组：**A 单独一笔**（`RouteSession` 三文件，可独立编译），**B/C/D 第二笔**
（说明 §7 要求 B、C 同批合入，否则出现「一边已不持锁、另一边仍对裸 `subscriber_` 调 `release`」）。

---

## 8 阶段 3 边界（供接手者对齐）

阶段 3 说明（`docs/消息接收架构改造/阶段3_收包分流提取实现说明.md`）§4 给出合入后的形态：

```text
raw_data = recv(...)          // 阶段 2 已合入 ⇒ 现为 lease + recv
if (raw_data.empty()) continue;
process_received_buffer(sub_state_, std::move(raw_data));
```

**留在循环里的**：`running` / `handshake_completed` 判断、`recv`、`raw_data.empty()` 则 `continue`。
⇒ **§5.4 的伪影门必须留在循环里**（与判空同层），不得搬进 `process_received_buffer`。

**不经过该函数的**：nodelet 快路径（发布端直接 `msg_queue_->push`）、`get*`/`try_get*`（只出队）、
队列满时的 evict 回调注册点。

---

## 9 待办与交接清单（按建议优先级）

| # | 事项 | 依据 | 备注 |
|---|---|---|---|
| 1 | **`git add` 阶段 2 的 6 个文件**（§7.5） | 文件撕裂风险 | 最高优先；尤其 `test/CMakeLists.txt`，丢了会让套件退出 ctest |
| 2 | **裁定并修复叫醒伪影**（§5） | 任务 `-6` 回报 | 最窄方案、落点、判据、边界齐备；零 API/libipc 变更 |
| 3 | **补三条点名回归用例**（§4.3） | 任务 `-2` 点名 | 修复已落地但无测试锚点；本成员已在 `/tmp` 副本验证过路径可复现 |
| 4 | 清理注释残留（§4.6） | 本轮实测 | 5 处旧排序键 + 1 处「16 条」 |
| 5 | 复查 F3 是否要加单轮预算判据；**F4 需 leader 裁决** | §4.5 | F4 = 控制面路径线程 QoS 静默失效 |
| 6 | 阶段 3 `process_received_buffer` | 阶段 3 说明 | 与 §5 的门同层，注意边界（§8） |
| 7 | 阶段 4/5（`recv_wait_set` / worker） | 阶段 4 说明 | 未开始 |
| 8 | 根因层：`waiter::wait_if` 返回值不可区分 | §5.4 末段 | libipc 公共语义变更，须单独评审（`send`/`wait_for_recv` 都消费该返回值） |

---

## 10 复现命令

```bash
cd /home/zwc/cpp_ipc_dds

# —— 阶段 1（已提交）——
cmake -S . -B build            # aux_source_directory 是配置期展开，新增 .cc 必须重跑
cmake --build build --target test_shm_control_scheduler -j4
./build/bin/test_shm_control_scheduler        # 期望 20/20
ctest --test-dir build -R test_shm_control_scheduler --output-on-failure

# —— 阶段 2（未提交）——
cmake -S . -B build            # test/CMakeLists.txt 的 file(GLOB) 无 CONFIGURE_DEPENDS，必须重跑
cmake --build build --target test_shm_route_session -j4
./build/bin/test_shm_route_session            # 期望 14/14
ctest --test-dir build -R test_shm_route_session --output-on-failure

# —— 邻接回归（本批改动直接命中面）——
rm -f /dev/shm/*occupied*      # 段残留会让个别套件超时，非回归
for t in test_dzipc_shm test_shm_domain_isolation test_shm_nodelet \
         test_nodelet_switch test_shm_receiver_cap test_wire_accept; do
    LD_LIBRARY_PATH=build/lib:build/bin ./build/bin/$t | tail -1
done                           # 期望 6 套件 45 条全 PASSED

# —— 门禁 ——
bash docs/hotpath_gate.sh      # 期望 "过闸。" rc=0

# —— 缺陷现状核对 ——
grep -c "is_wakeup_artifact" src/dzIPC/shm_pub_sub_ipc.cc            # 期望 0（门未落地）
for n in CallbackInternalStopDoesNotDeadlock ConcurrentStopWaitsForJoin TokenOutlivesScheduler; do
    printf "%-40s %s\n" "$n" "$(grep -c "$n" test/test_shm_control_scheduler.cpp)"; done   # 期望全 0
git status --porcelain                                              # 期望 6 个文件（§1.1）
git diff --stat HEAD -- include/dzIPC/threepools src/dzIPC/threepools test/test_shm_control_scheduler.cpp  # 期望空
```

---

## 11 事实边界（必须声明）

1. **§1.1 的 `git status`、§1.2 的 md5/行数、§1.3 的 ctest 与邻接回归读数、§1.3 的 `hotpath_gate` 输出、
   §4.3/§4.5/§4.6/§5.6/§7.2 的 grep 结果**均为本成员**本次复核的实测输出**（2026-09-23 20:54–21:00 CST）。
2. **§4.1 的修复落点是「HEAD 内容与工作区一致」这一事实**（`git diff` 为空 + md5 相同），
   不是「本成员提交」的声明 —— 本成员按纪律**不做 `git add`/`commit`**。
3. **§4.2 的 `unordered_map` 迭代序**（`2 1`）由本成员在隔离探针中实测得出；
   §4.2 的变异测试结论（删复查 ⇒ 红；删立即摘除 ⇒ 仍绿）也是本成员实测。
4. **§5.2 的机理链**由逐行阅读 `ipc.cpp`/`waiter.h`/`wire_accept.cc` + 队友隔离探针实测
   （`pushed=1`，见 `ipc-test-perf-phase2-route-session-test-report-b29fc18e0a8e.md` §6）共同支撑；
   本成员**未独立跑**那个端到端探针（只读任务下未构造产品码）。
5. **未做 TSan 门**：本机 TSan 与该代码模式不兼容（`unexpected memory mapping`，需
   `setarch -R` 绕过 ASLR；最小复现也报 `double lock of a mutex` + `data race`）——
   见 `docs/消息接收架构改造/阶段1_控制面调度器实现说明.md` §8.2。
6. **三条点名用例缺席（§4.3）** 是基于 `grep -c` 的**文件事实**；不排除它们以别的名字存在但语义等价，
   需人工确认（本成员已逐一比对现有 20 条用例名，未发现等价物）。
7. 阶段 2 说明 §1 表格中「`sub_handshake()` 里五处 `channel_mtx_`」的表述与实际不符：实测为
   **4 处在 `sub_handshake()`（接入前 HEAD 版 `:642/:657/:671/:735`）+ 1 处在 `InitChannel()` 的订阅 lambda（`:792`）**
   —— 此为**接入前的 HEAD 行号**（`git show HEAD:src/dzIPC/shm_pub_sub_ipc.cc | grep -n channel_mtx_`）；
   接入后 `channel_mtx_` 已整体删除（§7.2）。
8. **§1.3 门 1 的 `64B 丢包=77.22%`** 未经改动前基线对比，**不构成**回归判据（§1.3 注）。
9. 本文件同时以 deliverable 形式发布到团队存储（id 见回报正文），并写入共享上下文区
   `/home/zwc/.mult_agent_mcp/contexts/cppipc_team/ipc-review-security_phase1_phase2_offline_sync_v3_2026-09-23.md`；
   两处内容一致。
