# W10 前置：门槛 3 可达性标定 + D-6（并发 ctest 偶发失败）复现

> 负责人：验证与性能负责人（W10/W11）
> 状态：**W10 未开工前的独立前置证据**（t11 尚未 claim；本文件为 D-6 处置与 W00 共同签认的证据底稿）
> 采集：2026-09-28 19:36–20:35 +0800，本机（i9-14900KF / 32 逻辑核 / governor=powersave）
> 被验证库指纹：`build/lib/libipc.so.1.3.0` = `65608d9255e7318a…`（**注意**：W00 §1.4 冻结指纹为 `97504a74…`，该文件在 20:32 已不可获得 —— 见《基线与门槛.md》§11.2.0 C-1）
> 原始日志：`test/perf/out/20260928_w00_gate3_calib/`（`src/` 10 个探针源码 + `logs/` 8 份原始日志）

## 0. 结论速览

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | 门槛 3 的 `空闲 ctx ≤ 50 /s` 在冻结配置下**不可达**：结构性下限 = `N_worker × 1000/wait_timeout` = 320 /s，叠加控制面 1000 项 × 10 ms ≈ 1655 /s | E-A（探针 + 源码行） |
| 2 | 门槛 1 的「静默回落 ≤ 8」与门槛 3 场景「1000 有效订阅」互斥：状态 3 常驻 33 线程 | E-A |
| 3 | 门槛 2/6 的关键前提已被本人独立复现：1000 独立话题 ⇒ 33 线程 / 1000 route / 2068 fd / 注册表 0 条表满 | E-A |
| 4 | **D-6 的主犯不是 `test_shm_sub_dtor_gate`，而是 `test_dzflat_builder`**：`-j32` 跑 25 轮，21 轮失败（84%），首次失败在 run 5 | E-A |
| 5 | 失败根因指向 **chunk 池（每档 40 块，池名空前缀 = 无话题/进程区分、全机共享）容量与归因缺陷**，不是 dtor_gate 自身竞态 | E-B（日志 + 配对实验 + 源码） |
| 6 | 与 builder 并发跑**任意**其它用例（7/7 个邻居）均 6/6 失败；单跑同窗口 20/20 通过 | E-B |

## 1. 门槛 3 可达性标定（W00 §6.2 第 3 行）

### 1.1 结构下限（与实现同形的裸探针）

| 探针 | 配置 | 60 s 窗口实测 |
|---|---|---|
| `epcal`（`epoll_wait` 固定切片，同 `SocketRecvWorker` 空闲等待） | 32 线程 @100 ms | **320.2 /s** |
| `epcal` | 32 线程 @1000 ms | 31.6 /s |
| `idlecal`（同形 + 1 个 10 ms 周期 tick） | 32 @100 ms | 420.3 /s |
| `idlecal` | 32 @640 ms | 150.6 /s |
| `idlecal` | 32 @1000 ms | 129.5 /s |

### 1.2 真实产品池（1000 独立话题，无发布者；修正既有 `t6scale` 的同 topic 缺陷）

```
$ ./idlepool 1000 702 60
pool_running=1 workers=32 route_count=1000 threads_now=33 fds_open=2068 max_fd=2067
window_s=60.039 cpu_cores=0.00450 ctx_per_s=324.3 ctx_per_route_per_s=0.324
threads_min=33 threads_max=33 route_count=1000 idle_exits=0 wait_timeouts=19193
```

⇒ ctx **324 /s**（= 32 × 10/s，与 320/s 结构下限一致）；CPU **0.0045 core**（远优于 0.20 core 判据）。

### 1.3 真实 `ShmControlScheduler`（**非模拟**，`ControlTiming` 默认 `sub_heartbeat=10 ms`）

| 在册控制项 N | 60 s 窗口 ctx | CPU | tick/60 s | overrun |
|---|---|---|---|---|
| 1 | **100.0 /s** | 0.0007 core | 6200 | 0 |
| 32 | 100.2 /s | 0.0010 | 6213 | 0 |
| 100 | 199.7 /s | 0.0022 | 12376 | 0 |
| 250 | 293.5 /s | 0.0032 | 18038 | 0 |
| **1000** | **1603.0 /s** | 0.0163 | 99289 | 0 |

⇒ **N=1 已是 50/s 预算的 2 倍**；1000 项是 32 倍。tick 粒度是**控制项级**（每到期项各唤醒一次），非每 tick 一次。

### 1.4 口径交叉核对

`cpuacct 32`（32 忙线程）：`/proc/self/stat` = 30.876 core vs 逐 TID 求和 = 30.849 core，**比值 1.00** ⇒ `/proc/self/stat` 的 `utime+stime` 确覆盖全部线程（CPU 可用）；ctx 仍须逐 TID 聚合（与 W03 冻结口径一致）。

## 2. D-6：并发 ctest 偶发失败复现（`ctest --test-dir build -j32`，当前 22 项）

### 2.1 失败率与首个失败

```
$ bash src/ctest_flaky2.sh <out> 25 32      # 25 轮，每轮 -j32，全量 ctest
轮次 1–4  : rc=0
轮次 5    : rc=8  → 17 - test_dzflat_builder (Failed)      ← 首个失败
轮次 6–22 : rc=8  → 17 - test_dzflat_builder (Failed)      （连续 17 轮）
轮次 23   : rc=8  → 3/4(SEGFAULT)/6/9/15/17/22 七项失败     ← 级联
轮次 24   : rc=8  → 15/17/22
轮次 25   : rc=8  → 11/15/17/22
合计：25 轮中 21 轮失败（84%）
```

各用例失败次数（25 轮）：

| 用例 | 失败轮数 | 说明 |
|---|---|---|
| **`test_dzflat_builder`** | **21/25（84%）** | 主犯；首个失败即它 |
| `test_w03_measurement` | 3/25 | 仅出现在级联轮（23–25） |
| `test_socket_high_fd` | 3/25 | 仅级联轮 |
| `test_shm_sub_dtor_gate` | **1/25** | 仅 run 23；`DtorWakesInflightRecvInOrder` 15.06 s 硬超时被杀 |
| `test_wakeup_artifact` | 1/25 | run 23 **SEGFAULT** |
| `test_recv_worker` | 1/25 | `thread_restarts 0 vs 1`（run 23 级联） |
| `test_shm_route_session` | 1/25 | 仅 run 23 |
| `test_socket_recv_worker` | 1/25 | 仅 run 25 |

⇒ **D-6 原文归因（dtor_gate）不成立**：dtor_gate 只在已级联的 run 23 失败 1 次；高频项是 dzflat_builder。

### 2.2 首个失败原文（run 5）

```
14/22 Test #17: test_dzflat_builder ..............***Failed    3.82 sec
[ RUN      ] DzFlatBuilder.Tier0ElementArrayWrittenInPlace
chunk pool exhausted: kind = loan, chunk_size = 132096, size = 131072, pool capacity = 40, count = 1 (本进程), prefix = ''  <= 空前缀: 本档池无话题/进程区分, 全机共享, 归因须查其他进程
/home/zwc/cpp_ipc_dds/test/test_dzflat_builder.cpp:260: Failure
Value of: published   Actual: false   Expected: true
[  FAILED  ] DzFlatBuilder.Tier0ElementArrayWrittenInPlace (1152 ms)
```

run 23 级联中 dtor_gate 原文：

```
[ RUN      ] ShmSubDtorGate.DtorWakesInflightRecvInOrder
test/test_shm_sub_dtor_gate.cpp:516: Failure
Value of: r.timed_out   Actual: true    Expected: false
子进程被硬超时杀死 —— 析构可能挂住了。log=[] 被信号 9 终止 (父进程硬超时, 已 SIGKILL)
[  FAILED  ] ShmSubDtorGate.DtorWakesInflightRecvInOrder (15058 ms)
```

### 2.3 归因实验（同一 shell 窗口内）

| 实验 | 结果 |
|---|---|
| builder **单跑** ×20（同窗口） | **20/20 rc=0** |
| builder 单跑 ×12（另一次会话） | 4/12 rc=1（同样两条 `chunk pool exhausted` 断言） |
| builder 单跑指定用例 ×10（`Tier0…`）+ ×10（`UnpublishedLoan…`） | 20/20 rc=0 |
| builder **与邻居并发**（7 个邻居，各 6 次） | **全部 6/6 失败**：`test_dzflat_transport` 2/6、`test_lifecycle_contract` 6/6、`test_recv_worker` 6/6、`test_shm_ser_backpressure` 6/6、`test_shm_i5_pop_buffer` 6/6、`test_w08_dzflat_ab` 6/6、`test_dzflat_rx` 6/6 |

⇒ 失败与「是否有并发邻居占用同一 chunk 池」强相关，**不是** dtor_gate 自身竞态。

### 2.4 机制（源码 + 日志，E-B）

1. chunk 池按 **尺寸档** 命名：`__IPC_SHM__CHUNK_INFO__<chunk_size>__C40`（`src/libipc/ipc.cpp:368`），容量固定 **40 块/档**，`prefix=''` 时**无话题/进程区分** ⇒ 同机所有进程/用例共享同一档池（源码自带告警文案：「空前缀…全机共享, 归因须查其他进程」）。
2. 借样失败即 `note_pool_exhausted(..., prefix='')`（`ipc.cpp:530`）；`DzFlatBuilder.Tier0…` 断言 `publish_loaned` 成功（`test/test_dzflat_builder.cpp:260`），池耗尽即红灯。
3. 死持有者回收（`reclaim_dead_chunks`，`ipc.cpp:557`）**明确跳过 `published == 0` 的块**（「借出但未发布的块由发布者自己处置」）⇒ 进程被 ctest 硬超时 `SIGKILL` 后，**未发布借样永远不回收**，只能等最后一个持有者脱离后段被销毁。
4. 故在 `-j32` 下并发用例争抢**同一档 40 块**，叠加被 kill 的进程泄漏未发布借样，形成「越跑越紧」的级联（run 23 一次爆 7 项）。

### 2.5 归属建议（按 D-6 第 4 条）

| 维度 | 判定 |
|---|---|
| 是否本轮改造引入 | **否**。池名/容量/回收规则均来自既有 `libipc`（`ipc.cpp`），W04/W07/W08/W09 未改这三处；W08 只是新增了同样消费 132096 档的用例（`test_w08_dzflat_ab`），**加重而非造成**该耦合 |
| 产品侧缺陷 | **是（容量/归因缺陷）**：池「空前缀 = 全机共享 40 块/档」使「单机多进程共存」时的容量不可预测；回收跳过未发布借样使异常退出泄漏。建议归 W09（chunk 容量与背压）以「容量模型 + 归因前缀 + 异常退出回收」处理 |
| 测试工装侧缺陷 | **是（并发资源耦合）**：`test_dzflat_builder` 与任何消费同档的用例并发即红。建议归 W10（本工装）与架构负责人：按既有先例（`test_ipc_info_pool` / `test_ipc_info_pool_version`）对**碰共享池的用例**加 `RUN_SERIAL TRUE`（`RESOURCE_LOCK` 不足，见 `test/CMakeLists.txt:433-443` 的实测注释） |
| 与 `docs/dzflat_shm.md` §9.5 末尾的池耗尽段错误关系 | **同类但不同现象**：本例是「借样失败/断言红」，§9.5 是「耗尽 → 64 B 分片 → 环覆写 → 段错误」。本轮 run 23 的 `test_wakeup_artifact` **SEGFAULT** 已登记为待归因项（不排除同一族） |

### 2.6 已知限制 / 边界（不得过度外推）

1. 本机 `/dev/shm` 在**不同 shell 调用之间是隔离的**（实测写标记后下一调用不可见、`stat` 设备号变化），因此**跨调用**残留无法在本会话内验证；上述「级联泄漏」结论全部限定在**同一 shell 窗口内**成立。真实共享 `/dev/shm` 环境下的跨进程污染只会更重，不会更轻。
2. `ctest -j32` 的失败率对机器负载敏感（同类运行另有 20/20 全绿的窗口），故「84%」是**负载相关**读数，不是恒定值。
3. `test_w03_measurement` / `test_socket_high_fd` 的级联失败**未逐条归因**（只登记），归因工作随 W09/W10 正式执行。

## 2.7 【追加 · 队长采纳后的复核】RUN_SERIAL 生效仍未全绿，且根因已定位到"未发布借样不可回收"

队长已按 t23 让架构负责人对共享池用例加 `RUN_SERIAL TRUE`（当前 `ctest -N` = 23 项）。本人随后**复取**（2026-09-28 20:45–20:55）：

| 实验 | 结果 |
|---|---|
| `ctest -j32` ×15 轮（**RUN_SERIAL 生效后**） | **11/15 轮失败（73%）**：run 1–4 全绿；run 5 起 `test_dzflat_builder` **连续 11 轮失败**，且第 5 轮新增 `test_w08_dzflat_ab` 失败、第 6 轮起 `test_socket_recv_worker` **Subprocess aborted** |
| `test_dzflat_builder` 纯单跑 ×20（当前环境） | **20/20 通过** ⇒ 单跑不可复现 |
| Tier0 单用例 ×20；Alloc+ Tier0 / InPlace+Tier0 / Tier0+Tier2 各 ×10 | **全部 0 失败** ⇒ 不是"同进程前序用例"泄漏 |
| 串行链：先 `test_w08_dzflat_ab` 再 `test_dzflat_builder` ×3 | builder **0 失败** |
| **忠实复现**（未发布借样的持有者与 builder **并发**，中途 `SIGKILL` 该持有者）×3 | builder **3/3 失败**、`chunk pool exhausted` 3/3 |

**反事实判决实验（决定性，同尺寸档 132096、借满 40 块后 `SIGKILL`）**——每轮先清理**本探针自己创建**的段以消除跨轮污染：

| 分支 | 借满后的动作 | builder 结果（各 3 轮） |
|---|---|---|
| **A** | **发布**再被杀 | `rc=0`、`exhausted=0` ×3 |
| **B** | **不发布**被杀 | `rc=1`、`exhausted=1` ×3 |

⇒ **A/B 唯一差异 = 是否发布**，而结果完全二分。这把 `reclaim_dead_chunks` 的 `:577 published()==0 → continue`（跳过未发布借样）从"代码推断"升级为**受控实验证明**：死进程的**未发布借样永不可回收**，会把整档 40 块永久占住，直到最后一个持有者脱离后段被销毁。

**残余未知（登记，不强行闭合）**：`ctest -j32` 的 11/15 失败与"我的探针能造出同一失败"之间，**尚未拿到直接因果链**——需要 ctest 视角下的证据（哪个并发用例的借样在哪个时点泄漏、是否有进程被 ctest 超时 `SIGKILL`）。当前可确证的是：① 失败面**不可能**由 `RUN_SERIAL` 消除（builder 本身已 `RUN_SERIAL`，仍连挂 11 轮）；② `test_socket_recv_worker` 的 `Subprocess aborted` 是**新出现的第三类现象**（21 轮旧跑中只 1 次，现 10/15 次），值得单独立项；③ 这些失败全部发生在**共享池未释放**的窗口内。

⇒ 结论要点（供 t22/t23 引用）：**修复点在 libipc 侧**（池命名加进程/话题 scope，或让未发布借样可被回收），`RUN_SERIAL` 只能减小概率、**不能**消除；`test_socket_recv_worker` 的 abort 需与 `test_wakeup_artifact` 的 SEGFAULT 一起并入非性能缺陷线。

原始日志：`logs/ctest_flaky3_runserial_summary.tsv`、`logs/ctest_flaky3_run5.log`、`logs/builder_solo20_now.tsv`；反事实探针源码 `src/leaker.cpp`；完整段落见 `logs/gate3_calib.txt` §8。

## 2.8 【追加 · 回应 D-13 §三】持续性与「谁替死 route 清扫」的受控实验

队长 D-13 提出三个待判定问题，其中第 3 点（**泄漏者自己的 route 也消失后，是否没人替它扫 ⇒ 第二类永久泄漏**）与「run 5 起连续失败」的持续性直接相关。本人补做两组实验（2026-09-28 21:00–21:10）。

### （a）不可自愈性：池占用不会随时间/重试自愈

| 步骤 | 结果 |
|---|---|
| 起点：删除本探针创建的池段，`/dev/shm` 计数 0 | — |
| `leaker132 40 unpublished` 运行中 → `SIGKILL` | 池段 `__IPC_SHM__CHUNK_INFO__132096__C40` **仍存在**（由其它持有者维持），位图内含 40 块未发布借样 |
| builder **连跑 5 次**（不清理任何东西） | **5/5 失败**，`chunk pool exhausted` 5/5 ⇒ **不自愈** |
| **只删该池段**后再连跑 3 次 | **3/3 通过**，`exhausted=0` ⇒ 失败源确为该段内位图占用，**不是**环境噪声 |

⇒ 这解释了「run 5 起**连续** 11 轮失败」的**持续性**：一旦 `-j32` 窗口内有持有者被 `SIGKILL`，该档池在整个窗口内保持被占，后续每轮 builder 都必然踩到。

### （b）「谁替死 route 清扫」：**同话题的新发布者也救不回**

| 实验 | 设置 | 结果 |
|---|---|---|
| 同话题 | A 用 `/dzflat_b/shared_topic` 借满 40 块未发布 → `SIGKILL`；B 用**同一话题名**再借 40 块 | **`loaned=0` + `chunk pool exhausted`** |
| 跨话题 | A 用 `/dzflat_b/topic_a` 如上；D 用**不同话题名** `/dzflat_b/topic_b` 再借 | **`loaned=0` + `chunk pool exhausted`** |

⇒ 对 D-13 第 3 点给出**实验层面的确认**（机制层面的完整闭环仍属 t22）：
1. `reclaim_dead_chunks` 的三道门（`route_tag_` 相等 → `published != 0` → `proven_dead_bits`）决定了**跨路由完全不可见**（不同话题 = 不同 `route_tag_`，`ipc.cpp:575` 直接跳过）⇒ 别的 route 不会替它扫。
2. 更关键的是：**即使**新发布者的 `route_tag_` 相同（同话题重开），也只有**池穷尽时**才触发一次 `reclaim_dead_chunks`（调用点唯一：`ipc.cpp:683`，`id_pool::acquire()` 返回 <0 之后），而且 `:577` 的 `published==0` 判定仍会挡下这批未发布借样 ⇒ **同话题重开也救不回**，与实测一致。
3. 因此存在**两类**永久泄漏：(i) `published==0` 的未发布借样（A/B 反事实已证）；(ii) 一旦**泄漏者自己的 route 也消失**（进程退出/段被清），`route_tag_` 不再有对应持有者能发起清扫 ⇒ 无人认领。二者都只能靠"段整体销毁"消除。

### （c）与队长 D-13 §一 观察的衔接（不矛盾）

队长实测「`/dev/shm` 现为空」；本节实验显示 SIGKILL 后段**仍在**。两者一致，因为**段一旦被 unlink，位图随之消失**（占用是**段内**状态，不是段文件本身）。

> **⚠️ 措辞修正（2026-09-28 22:35，回应架构负责人 t23 的 r2/r3 实验）**：本节初稿写的「跨会话不累积」**表述不准确，不应作为产品语义**。架构负责人独立复现出更强的一条：**失败后单跑仍失败，清理未被映射的 `__IPC_SHM__CHUNK_INFO__*` 段后立刻 100% 全绿** ⇒ 泄漏是**跨进程、跨运行**持续的，只要池段仍存活（无人 unlink 它）。我本轮在同一调用内用三个**全新进程**复现确认：
> - A 借满 40 块未发布（132096 档）→ 外部 `SIGKILL`（跳过析构 ⇒ 无 `shm_unlink`）⇒ **池段遗留**，`/dev/shm` 段数 15；
> - B（**另一个话题名**、全新进程、不作任何清理）⇒ **`loaned=0` + `chunk pool exhausted`**（陈旧段对新进程可见，池显示"已被用掉"）；
> - C（**仅删池段**后）⇒ **`loaned=40`**。
> 准确表述应为：**泄漏不跨"段被清理/段被重组"的边界，但会跨进程与跨运行持续**。我原文只在本会话沙箱的**调用间隔离**下成立（实测：同一 bash 调用内多进程共享 `/dev/shm`，写 marker 后相邻调用不可见；`mnt_ns` 编号不变、`dev/ino` 不变），**这是本会话执行环境的特性，不是产品行为**。据此，架构负责人给出的 **t22 靶心三项（陈旧段识别/段生命周期、未发布借样的异常退出回收、归因前缀）成立**，且他的判定比我的原文更强、更准。

### （d）对 t22 的输入（供参考，不代替 W09 的补丁论证）

- 门序问题（队长 D-13 §三-1）成立且**可测**：把 `:577`（`published==0`）移到 `:578`（生死判定）**之后**，至少能让"全部持有者已死"的未发布借样进入回收候选；是否充分取决于"未发布借样是否会与 in-flight 构造竞争"（D-13 §三-2），本人在 W10 位置**不做该判定**（属 W09 安全性论证）。
- 建议 t22 把**两类泄漏**分别成条目（`published==0` 未发布借样 / 死 route 无人清扫），并各配一个受控实验（本节的 A/B 与 (b) 两组可直接复用；探针 `src/leaker.cpp`，用法 `leaker132 <n> [publish] [topic]`）。

原始日志与命令细节见 `logs/gate3_calib.txt` §9。

## 2.9 【口径统一】两类失败要分开：瞬态争抢 vs 异常退出段遗留

架构负责人 t23 与我各自独立做完后，队长核查证据树指出其 `solo_b_1..8.log` **全部 Passed**，据此纠正了「失败后单跑仍失败 ⇒ 与并发无关」这种把两种情形混为一谈的写法；架构负责人已用我的 `leaker.cpp` 在同一 shell 内重做判决实验（`build/test-scratch/t23_evidence/leak_exp/`：`0_baseline` 100% passed → `1_leaker` 残留 `CHUNK_INFO__132096__C40` → `2v_solo_after_leak` **单跑 rc=8** → `3_clear` 仅删该段 → `4v_after_clean` **单跑 rc=0**）。

**本报告的统一口径（与本文件 §2.3/§2.5 的表述合并后的准确版）**：

| 现象 | 直接原因 | 我的实测 | 是否随「仅串行化」消失 |
|---|---|---|---|
| 同一窗口内 builder 与邻居**双方存活**时失败 | **瞬态争抢**同一档 40 块（真实并发占用） | 配对矩阵 **7/7 邻居均 6/6 失败**；串行链（先 w08 后 builder）**0/3 失败** | 是（串行化即可避免） |
| run 5 起**连续 11 轮**失败（跨运行持续） | **异常退出段遗留**：`SIGKILL` 跳过析构 ⇒ 不 `shm_unlink`（`src/libipc/platform/posix/shm_posix.cpp:232`，仅在引用计数归零的 `release()` 内 unlink；`created_registry` 退出扫除只覆盖本进程创建的段）⇒ 未发布借样（`reclaim_dead_chunks` 跳过 `published==0`）留在存活段内 | §2.8(a) 5/5 失败 vs 仅删段 3/3 通过；§2.8(b) 同话题/异话题均 `loaned=0` | **否**（builder 自身已 `RUN_SERIAL` 仍连挂 ⇒ 串行化改不了跨运行的段状态） |

⇒ **共同口径（与 W00 §4.4 ③b、D-13 表一致）**：失败**与并发邻居无直接关系、但与并发窗口内产生的异常退出段强相关**；段一旦遗留即**跨运行/跨进程持续**（只要未 `unlink`），**单跑照挂**，清理段即恢复。我 §2.3 的配对矩阵应读作「瞬态争抢」证据，**不是**持续失败的机制；持续失败的机制是本节与 §2.8。

## 3. 对 W10/W11 的直接约束（自加，见《基线与门槛.md》§11.2.0 D）

1. 千路规模结论只能由**逐 route 收发台账**出具；注册数/首条 route/调度线程数不得代表规模闭环。
2. 路径证据在 W06/W08 计数接线前标「未确认」。
3. 正式采集必须新建 `run_id` 并重取指纹（W00 §1.4 的 `97504a74…` 已不可获得）。
