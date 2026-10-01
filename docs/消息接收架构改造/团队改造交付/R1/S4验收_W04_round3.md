# S4 独立验收（第 3 轮）· W04 —— 针对 t62 收口的增量复核

> 复核者：**测量统计负责人**（W03；**未参与 W04 实现、t60 修复、t62 修复**）。
> 被复核对象：`t62`（repair round 3，实现者 = 共享层负责人）。
> 前序：第 1 轮 `R1/S4验收_W04.md`（needs_revision，8 findings）；第 2 轮 `R1/S4验收_W04_round2.md`（needs_revision，8/8 闭合 + 新增 N-1/N-2）。
> 本轮任务：`t63`，attempt `255fec5f-b257-4f3c-804b-734bbd9cc90d`。日期：2026-10-01。
> 纪律：**只读复核**；本报告是唯一新增产物。

## 0. Verdict（第 3 轮）

```text
W04 第 3 轮独立验收：verdict = pass
```

**结论**：第 2 轮遗留的 **2 条 finding（N-1 计数标签错配、N-2 计数符号名不存在）均已按可机械复算的形式改写**，
我逐项独立复算**全部对上**；第 1 轮的 8 条闭合状态未回退；四组核心结论（状态转换 / 锁顺序 / 32 条竞态三态 /
§10 准入可判定性）复跑一致。**W04 由此达到 S4（独立验收通过）**，无剩余 finding。

- 独立性：W04 实现者 = 共享层负责人；t60/t62 修复者 = 共享层负责人；我均未参与 ⇒ **独立性成立**。
- 本轮**无新增 finding**（findings 集合为空）。

---

## 1. 第 2 轮 2 条 finding 的闭合核实（⛔ 逐项独立复算，不采信自述）

### 1.1 N-1：`ShmControlScheduler::instance()` 计数标签错配 → 已按三口径定义改写

**t62 的改法**：两处（§3:118 与 §11.3 W04-F8 行）改为「**13 行**（提及），其中 **`::instance()` 5 行 =
**4 个真实调用点**（`:884`/`:895`/`:2234`/`:2242`）+ **1 行注释**（`:115`）」，并给出三口径定义式。

**我的独立复算**（本机实测，未读其读数）：

| 口径 | 命令 | 我的实测 | t62 声明 | 判定 |
|---|---|---|---|---|
| 提及 | `grep -c 'ShmControlScheduler' src/dzIPC/shm_pub_sub_ipc.cc` | **13** | 13 | ✅ |
| `::instance()` 行 | `grep -c 'ShmControlScheduler::instance()' …` | **5** | 5 | ✅ |
| 真实调用点 | 逐行看：`:884` `worker_active()`、`:895` `RegistrationToken`、`:2234` `worker_active()`、`:2242` `RegistrationToken` | **4**（+`:115` 注释 1 行） | 4 + 1 注释 | ✅ |

- 全文「13 处」残留：`grep -c "13 处"` = **0** ✅（标签不再被错贴）。
- 结论未受影响（`::instance()` 5 > 0 ⇒ 「已接入」仍成立，且 4 个调用点逐行可核对）。

### 1.2 N-2：`fallback_activated` 无同名符号 → 已改为实际符号 + 机械判定命令

**t62 的改法**：§8.4 两行 + §10 第 6 条改用真实符号，并新增【计数符号名对齐】块（含命令与内联读数）。

**我的逐项独立复算**：

| 项 | 命令 | 我的实测 | t62 声明 | 判定 |
|---|---|---|---|---|
| `fallback_total` 写入点 | `grep -rn 'CounterId::fallback_total' src/ \| wc -l` | **3** | 3 | ✅ |
| `fallback_backend_unavailable` | 同上 | **2** | 2 | ✅ |
| `fallback_capacity_full` | 同上 | **1** | 1 | ✅ |
| `wait_set_full` | 同上 | **1** | 1 | ✅ |
| `wait_token_invalid` | 同上 | **1** | 1 | ✅ |
| `fallback_activated` 出现 | `grep -rn 'fallback_activated' src/ include/ \| wc -l` | **2（均为注释）** | 2，均注释 | ✅ |
| 符号存在性 | `CounterId::fallback_activated` | **不存在**（仅 `counters.h:10/58` 注释） | 不存在 | ✅ |

**行号与语义也对上**（我逐行核对）：
- 三处 `fallback_total` 写入点 = `shm_pub_sub_ipc.cc:1911 / 1915 / 1919`，全在 `fallback_to_compat()`（函数定义 `:1894`）内；文档写的「`:1911-1921`」区间**包含**这三处 ✅。
- `wait_set_full` = `:1921`、`wait_token_invalid` = `:1924`（与表中逐点引用一致）✅。
- **语义分工与代码一致**（文档声称「`kInvalidToken` 分支不加 `fallback_total`」）：我读 `:1894-1930` 的 `switch`——
  `kBackendUnavailable`/`kPoolStartFailed` ⇒ `fallback_total` + `fallback_backend_unavailable`；
  `kWaitSetFull` ⇒ `fallback_total` + `fallback_capacity_full` + `wait_set_full`；
  `kInvalidToken` ⇒ **只** `wait_token_invalid` + `registration_invalid_token`（**确不加** `fallback_total`）✅
  ⇒ 「原因计数之和 = 回退总量，且 `wait_token_invalid` 与回退族互斥」成立。

**额外加分项（我发现它主动消除了一个会误导后续复核者的读数矛盾）**：文档新增「两套 grep 口径」对照，
说明「写入点（`CounterId::` + `src/`）= 3」与「符号出现（含 `include/`）= 20」的差异来源。
我独立复算：`grep -rn 'fallback_total' src/ include/ | wc -l` = **20**、`grep -rn 'fallback_total' src/ | wc -l` = **6**
（= 3 写入点 + 3 条注释：`shm_pub_sub_ipc.cc:1902/1904`、`libipc/ipc.cpp:714`——我逐行确认后两条与 `ipc.cpp:714` **均为注释**）✅
⇒ 与文档写明的差值来源**逐项吻合**，且明确了「写入点数必须以 `CounterId::` 前缀 + `src/` 域为准」。

### 1.3 全文 `fallback_activated` 的 6 处出现，是否都不再当符号用

`grep -n "fallback_activated" 接口与生命周期.md` = **6 处**（`:442`/`:443`/`:450`/`:460`/`:465`/`:581`），我逐处读过：
全部以「**方案 §13.3 类别名 ↔ 本仓真实符号**」的映射形式出现；
`grep -n "CounterId::fallback_activated\|fallback_activated()"` = **0 命中**（无一处当符号用）✅

---

## 2. 第 1 轮 8 条 finding 的**未回退**核实（回退检测）

t62 只改 1 个文件（见 §5），但我仍复跑了第 1/2 轮的关键判据，确认**未被回退**：

| 项 | 我的复跑 | 结论 |
|---|---|---|
| F1 条数口径 | 文档四处（§0/§2/§9/§11.2）仍写 9 条（复审时点）+ 10 条（t60 后） | ✅ 未回退 |
| F2 §3 行号 | `git show e800ccc:…` 逐行复核 682/713/795/810-811/893-894/955/973-983 = **10/10 命中**（第 2 轮已核，本轮抽查 795/955 仍命中） | ✅ 未回退 |
| F3 时点快照 + 补测表 | 两表仍在且标注「不得混读」 | ✅ 未回退 |
| F4 责任方 | §1/§10 仍为 W05=socket与数据面负责人、W06=接收池负责人 | ✅ 未回退 |
| F5 §0 与 §11.3 口径 | 仍一致（仅 F2 为未收敛准入条件） | ✅ 未回退 |
| F6 R-01 常驻 L10 | `./build/bin/test_lifecycle_contract` = **10/10 Passed**；二进制 = `88e0a507…`（与 t60 声明一致） | ✅ 未回退 |
| F7 「8 条」 | `grep "8 条"` 仍只出现在沿革说明语境 | ✅ 未回退 |
| F8 时点/指纹列 | §11.1 列仍在（含 t60 两行补测） | ✅ 未回退 |

---

## 3. 四组核心复核（第 3 轮）

| 组 | 复跑方式与读数 | 结论 |
|---|---|---|
| **① 状态转换**（注册/注销/关闭/generation/控制 tick/接收 lease） | `git show e800ccc:src/dzIPC/threepools/recv_worker.cc \| grep -c release_recv()` = **5**（四处非 ok 出口 + `remove_route` 第 6 步）；`shm_route_session.cc` 本波**零改动** ⇒ 与第 1/2 轮逐行结论一致 | ✅ 一致，无反例 |
| **② 锁顺序 L-1..L-5** | 机械扫描 `recv_worker.cc` 全部 `lock(impl_->mtx)` 作用域：出现 `join_mtx`/`ensure_thread_alive` 的次数 = **0**（期望 0） | ✅ L-1/L-2/L-3 成立（L-4/L-5 仍为抽查，强度边界见 §6） |
| **③ 32 条竞态三态** | 分母 **32**；已由测试覆盖 **30 = 93.8%**；文档声明+机械核对 **1**（R-02）；仅探针 **1**（R-30）；missing 0 —— 与第 2 轮**逐值相同** | ✅ 表更准确且未回退 |
| **④ §10 准入可判定性** | W05-1（头文件成员 `grep -c` = 0）、W05-3（`RegistrationToken reg{` 两处）、W05-4（常驻用例）、**W05-5/W06-4 重建顺序 4/4 处**、W05-6（`tick_overrun_count`/`tick_duration_max_ns` 存在）仍可机械判定；**W06-6 现在也给出可机械判定命令与读数（3 / 1 / 1，我已复算一致）** | ✅ **由「唯一需修」变为全部可判定** |

**回归**：`ctest --test-dir build -j4` = **29/29 Passed**（56.0 s）；`test_lifecycle_contract` = **10/10 Passed**。

---

## 4. 32 条竞态清单三态表（第 3 轮，机械统计）

**判据**（与前两轮同法）：该条被至少一个**常驻 CTest 项**覆盖（用例所在 target 在 `ctest -N` 内）计「已覆盖」；
只有探针/一次性复现件的计「仅文档声明或未验证」，**⛔ 不计入已覆盖**。

| 三态 | 条数 | 占比 | 依据 |
|---|---|---|---|
| **已由测试覆盖**（常驻 CTest） | **30** | **93.8%** | R-01（L10）、R-03～R-17、R-18～R-29、R-31、R-32 |
| **文档声明 + 机械核对** | **1**（R-02） | 3.1% | `grep -c "this_thread::get_id"` = 3/3（我复核 `recv_worker.cc` 与 `socket_recv_worker.cc` **均为 3**）；§9 已显式标注「无独立常驻用例」 |
| **仅探针** | **1**（R-30） | 3.1% | `artifacts/perf/20260929-r24-W06/probe/w06_probe_forkarm`（W06 交付，非 CTest）；§9 已标注 |
| missing | **0** | — | — |
| **分母** | **32** | — | `grep -c '^| R-'` = 32 |

**逐条判定**（与文档 §9 表逐行对应，我逐行核对过「守门件」列）：

| # | 三态 | 守门件（我核对到的 target / 依据） |
|---|---|---|
| R-01 | 已覆盖 | `LifecycleContract.GenerationRebuildRequiresRemoveRouteFirstOrCrashStall`（L10，常驻）+ 复现件 |
| R-02 | 文档声明+机械核对 | `grep -c "this_thread::get_id"` = 3/3（**非常驻用例**，如实标注） |
| R-03 | 已覆盖 | `RecvWorker.RemoveRouteWaitsForInFlightRecvAndStopsCallingIt` |
| R-04 | 已覆盖 | `RecvWorker.RemoveRouteInterleavedWithIdleExitKeepsTableConsistent` |
| R-05 | 已覆盖 | `RecvWorker.ConcurrentAddRouteRestartsThreadExactlyOnce` |
| R-06 | 已覆盖 | `RecvWorker.MessagesArrivingWhileThreadIsIdleExitingAreNotLost` |
| R-07 | 已覆盖 | `RecvWorker.ThreadStaysAliveWhileRouteRegisteredAndIdle` |
| R-08 | 已覆盖 | `RecvWorker.AddRouteIsRefusedWhileCompatThreadOwnsRoute` + `SocketRecvWorker.AddRouteJudgementOrderAndSingleConsumer` |
| R-09 | 已覆盖 | `LifecycleContract.AddRemoveChurnAlwaysReturnsOwnerToNone` |
| R-10 | 已覆盖 | `LifecycleContract.StopDoesNotReleaseOwnerSoModuleMustRemoveRouteFirst` |
| R-11 | 已覆盖 | `LifecycleContract.ConstructiveSkewShowsTotalCapacityIsOnlyAnUpperBound` |
| R-12 | 已覆盖 | `LifecycleContract.SameWorkerBoundary126_127_128AndWaitSetFullSemantics` |
| R-13 | 已覆盖 | 同上（后段：摘一条 ⇒ 第 128 条 ok 且**真收包**） |
| R-14 | 已覆盖 | `LifecycleContract.StubbornHasPendingIsBoundedAndDoesNotStarveNeighbour` |
| R-15 | 已覆盖 | `LifecycleContract.GatedRecvOnceCannotBePreemptedByTinyBudgetAndRecovers` |
| R-16 | 已覆盖 | `LifecycleContract.PoolFirstStartDecidesWorkerCountAndBudgetOnce` |
| R-17 | 已覆盖 | `LifecycleContract.RouteKeyMustNotCarryGenerationBecauseAffinityIsPureFunction` |
| R-18 | 已覆盖 | `RecvWorker.RecvOnceExceptionDoesNotKillWorker` |
| R-19 | 已覆盖 | `RecvWaitSet.RemoveWakesBlockedWait*` + `SocketWaitSet.RemoveWakesBlockedWait` + `RecvWorker.StopWakesBlockedWaitAndIsIdempotent` |
| R-20 | 已覆盖 | `SocketWaitSet.ReusedHandleDoesNotInheritStaleReadiness` |
| R-21 | 已覆盖 | `SocketWaitSet.CancelWaitWakesBlockedWaitAndDisablesNode` + `SocketReadable.IsFalseAfterCancelWaitAndRecoversOnReconnect` |
| R-22 | 已覆盖 | `SocketWaitSet.ConcurrentAddRemoveDuringWaitDoesNotHang` |
| R-23 | 已覆盖 | `RecvWaitSet.CapacityIsBounded` |
| R-24 | 已覆盖 | `RecvWaitSet.PublisherInAnotherProcessWakesWaiter`（唯一跨进程条目） |
| R-25 | 已覆盖 | `RouteSession.ConcurrentAcquireReleaseWithRebuildIsRaceFree` 等 2 条 |
| R-26 | 已覆盖 | `RouteSession.RebuildFailureLeavesEmptyAndRetryable` 等 2 条 |
| R-27 | 已覆盖 | `ShmI5PopBuffer.PoppedTlvSurvivesGenerationRebuild` |
| R-28 | 已覆盖 | `WakeupArtifactDetection.*`(7) + `WakeupArtifactGate.*`(3) → `test_wakeup_artifact` |
| R-29 | 已覆盖 | `ShmControlScheduler.*`(20) → `test_shm_control_scheduler` |
| R-30 | 仅探针 | `artifacts/perf/…/w06_probe_forkarm`（**非常驻**，如实标注） |
| R-31 | 已覆盖 | `test_socket_ser_concurrency`（3 条） |
| R-32 | 已覆盖 | `LifecycleContract.SocketFruitlessReadinessBacksOffWithBoundedCycleRateAndFastRecovery` |

---

## 5. 边界、独立性与自证

- ✅ **独立性**：W04 实现者与 t60/t62 修复者均为**共享层负责人**；我均未参与 ⇒ 独立性成立。本轮**未自审**任何我自己的产物。
- ⛔ **未改** W04 交付物 / 产品代码 / 工装 / 他人 WP / 既有 run：本报告（`R1/S4验收_W04_round3.md`）是唯一新增产物。
  本轮复核**未做任何就地注入**（第 2 轮的负控注入已在第 2 轮内逐字还原，本轮不再重复）。
- ✅ t62 声称「只改 1 个文件」，我核实成立：`git status --short` 中受跟踪改动仅
  `接口与生命周期.md`（**+121 / −39**）与 `test/test_lifecycle_contract.cpp`（**+182 / −1**，即 t60 的 L10，未再变；二进制仍 `88e0a507…`）。
- ⚠️ **本轮复核的强度边界**（如实声明）：L-4/L-5 仍为**抽查**；32 条三态仍是**静态判定**（用例存在且在 CTest），
  **未逐条实跑** —— 若某用例名对但断言被改弱，本方法看不出来。建议 W10 补一次「按 §9 表逐条跑并留原始输出」。

```text
W04 第 3 轮独立评审：verdict = pass
第 1 轮 8 findings：未回退（逐项复核）     第 2 轮 2 findings：N-1 ✅ / N-2 ✅（逐项复算全部对上）
反证覆盖：有效订阅 ☑  预期路径 ☑  故障触发 ☑（R-01 反例 + L10 常驻，第 2 轮已独立复跑）  统计口径 ☐（静态判定，未逐条实跑）
四组独立取证：状态转换 ✅ / 锁顺序 L-1..L-3 ✅（L-4·L-5 抽查）/ 32 条三态 30-1-1 ✅ / §10 准入全部可机械判定 ✅
回归：ctest 29/29、test_lifecycle_contract 10/10
评审者（非实现者）：测量统计负责人        日期：2026-10-01
```
