# 20261001-t84-W05-F1 —— W05 控制面 stale 门控：**两条驱动源**双臂闭合（(B) 路线）

> 任务 `t84`（W05 第 3 轮修复，socket与数据面负责人）；依据 = **队长 W05 第 3 轮裁定**
> + R1-W05 第 3 轮 **N1（high）**：`t77` 的修复只落在 `ShmControlScheduler::Impl::dispatch()`，
> 而 **L1 运行时回退臂**（`DZIPC_SHM_CONTROL_SCHEDULER=1`）走的是每话题 `compat_control_loop()`，
> 它**自带一份同样判据且是修复前形态** ⇒ **回退臂比基线更差**（400 发 **0 收** vs 400/400）。
> ⛔ 本目录是**新 run**；不覆盖任何既有 run，也不改既有 run 的原始读数。

## 1. 根因（一句话）与结构性修法（(B) 路线）

**根因**：同一判据被**复制**到两条驱动路径 ⇒ 修一条、漏一条。
（同族：W10-F7 渲染与断言不一致、W08 失效值两处并存。）

**修法**：判据收敛成**唯一的内联自由函数** `dzIPC::shm_control::pub_control_tick(pub, now, dead_timeout, st)`
（`include/dzIPC/threepools/shm_control_scheduler.h`），两条驱动源**都只调它**：

| 驱动源 | 调用点 |
|---|---|
| ① 进程级调度器（默认臂） | `shm_control_scheduler.cc::dispatch()` → `pub_control_tick(*e->pub, now, e->timing.peer_dead_timeout, e->stale)` |
| ② 每话题兼容线程（L1 回退臂） | `shm_pub_sub_ipc.cc::compat_control_loop()` → `pub_control_tick(*pub_control_state_, now, kControlTiming.peer_dead_timeout, compat_tick_state_)` |

⇒ 「何时扫」的逻辑**结构上只存在一处**；「下次该扫的时刻」这种**驱动方排程状态**
放在 `PubTickState` 里，每驱动方各持一份（它属于"驱动方自己的排程"，不是判据）。

⚠️ **为什么不把判据做成 `PubControlState` 的（非虚）成员函数**（初版踩坑，已改）：
那需要把状态放进这个**导出抽象基类**，改变其布局（③d 硬闸）⇒ 任何派生它的二进制
（含测试替身）都会按旧尺寸构造、新成员被写花。实测表现为
`test_shm_control_scheduler` 的 `TimingSemanticsUnchanged` / `CallbackExceptionIsIsolated`
**乱红**（`owner_hb` 变成负巨值）。自由函数 + 外部状态 ⇒ `sizeof(PubControlState)` 仍为 **8**
（仅 vtable 指针），**布局零变化**，而"逻辑只一处"的目标同样达成。

## 2. 双臂读数（每条 3 次；探针 = `build/t58/bin/t58_harm_w05only`，库 = w05only 变体树 + 本轮修复）

| 臂 | run1 | run2 | run3 |
|---|---|---|---|
| 默认臂（进程级调度器） | 400/400 | 400/400 | 400/400 |
| **L1 回退臂**（`DZIPC_SHM_CONTROL_SCHEDULER=1`） | **400/400** | **400/400** | **400/400** |

修复前后对照：

| 臂 | t77 时点 | t84（本轮） | 基线（W05 关闭） |
|---|---|---|---|
| 默认臂 | 400/400 | 400/400 | 400/400 |
| **L1 回退臂** | **0/400** ⛔ | **400/400** ✅ | 400/400 |

原始读数：`both_arms.csv`。

## 3. 常驻用例（双臂）+ 反向验证

常驻用例 `test/test_w05_stale_slot_gate_arm.cpp`（**两条用例，各自 arm**，都在**子进程**里跑 ——
环境变量是进程内只读一次的静态量）。两条用例共享同一实现 `run_one_arm()`：

- 判据① **回收**：`peer_count()==0` 期间陈旧槽位必须在 `peer_dead_timeout` 量级内被清掉；
- 判据② **活订阅者不被误断**：随后挂真实订阅者 + 发布 100 条必须全收到。

**反向验证（判据的判据）：三次消融，每次只改一处** —— 见 `negative/README.md`。
⛔ **承重项是第三次**：消融**唯一判据** `pub_control_tick()` 内部的兜底分支 ⇒ **两臂同时变红**
（修复前是"消融调度器只红一臂"，正是 N1 的形态）。

## 4. 回归

`ctest --test-dir build -j4` **连跑 8 轮：7 轮 29/29 全绿，第 8 轮 `28 - test_dzflat_rx (SEGFAULT)`**
（`ctest_j4_run{1..8}.log`）。

⚠️ **如实记录该失败**：`test_dzflat_rx` 单跑 **1/1 Passed**（1.53 s），且该文件**不引用**
`shm_control_scheduler` / `compat_control_loop` / `has_peers`（grep 0 命中）⇒ 与本轮改动**无关**，
形态与已知的 `test_w02_xproc_benchmark` 共享段类偶发同族。
⛔ **不以"全绿"作结论**（附轮数正是为了不掩盖它）；该偶发归 W10/W11 的测试稳定性面，本包不认领。

## 5. 文档面（按臂如实）

`团队改造交付/W05/控制面接入_交付.md`：
- §1.2 新增「N1：判定曾被复制到两条驱动源 + (B) 路线 + 双臂表 + 三次消融」；
- §7 的 **R-6 行按臂分列**（默认臂 ✅ / L1 回退臂 ✅，各附读数）；
- §4.2 的 L1「语义不变」改为**由双臂读数支撑**；
- §10.5 的**危险建议补丁**（仍写已证伪结论）**已作废并标注「严禁照抄」**；
- §7.5 的「与 HEAD 零差异」更正为如实表述（对照对象是 t58 精确快照树；对 HEAD blob 是 **79/874**）；
- §0 的 `27/27` → `29/29`（并注明项数会随工作包增长，以 `ctest -N` 现值为准）。
