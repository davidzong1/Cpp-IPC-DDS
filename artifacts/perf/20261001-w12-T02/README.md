# 20261001-w12-T02 —— W05 发布侧语义收敛：唯一判据 `pub_control_tick()` 的双臂 + 反向消融 + ABI 证据

> 任务 `t3`（W12 成员一：W05 socket 与数据面负责人），依据《W12 缺陷修复执行方案》§4。
> 目标缺陷 = **R1-W05-F1**：`peer_count()==0` 但共享段仍有陈旧 `in_use` 槽位时，
> 槽位必须被回收，且回收不得误断随后建立的活订阅者。
> ⛔ 本目录是**新 run**；不覆盖任何既有 run 目录，也不改任何既有 run 的原始读数。
> 本轮**未新增产品代码变更**：生产逻辑在 `t84` 已采用修复候选，本任务的工作是
> **收敛 + 机械/语义核对 + 可复算证明**（详见 §2 的核对结论）。

## 0. 一句话结论

「何时扫 stale」的判据**结构上只存在一处**（内联自由函数 `pub_control_tick()`，`include/dzIPC/threepools/shm_control_scheduler.h:193`），
两条驱动源（进程级调度器 / L1 回退线程）**只调它**；消融那一处 ⇒ **两臂同时变红**（修复前是"消融调度器只红一臂"，正是 N1 的形态）。
`sizeof(PubControlState)` 修复前后同为 **8**，`nm -DC` 符号名集合 495/495 逐名一致，`pub_control_tick` **未成为导出符号**。

## 1. 交付物索引（本目录 = `artifacts/perf/20261001-w12-T02/`）

| 文件 | 内容 |
|---|---|
| `fingerprint.txt` | 源码/库/工装 SHA-256、命令、环境、采集起止时间 |
| `gate_runs.csv` | 方案三条 verify 命令各 3 次的逐次读数 |
| `both_arms_400.csv` | 400 条消息端到端：默认臂 / L1 回退臂各 3 次 |
| `ablation/ablation_matrix.csv` | 反向消融矩阵（三个变体 × 两臂） |
| `ablation/variant{1,2,3}_*.diff` | 每个变体**只改一处**的精确 diff |
| `abi_report.txt` | sizeof / alignof / `nm -DC` / `.dynsym` 对照 |
| `flake_observation.md` | 偶发 `99/100` 的观察、判别实验、已排除/未排除项（⛔ 如实登记，未淡化） |
| `flake/` | 偶发读数与判别实验的原始日志与逐次读数 |
| `logs/` | 全部原始日志（gate、400 条、消融、ABI、机械检查、构建、最终确认组） |
| `src/` | 本轮自建探针与采集脚本（`w05_tick_transition_probe.cpp`、`w05_harm400_probe.cpp`、`w05_abi_size_probe.cpp`、`w05_clean_baseline_probe.cpp`、`w05_stale_gate_diag_probe.cpp`、`collect_fingerprint.sh`、`rg` shim） |
| `commands.sh` | 按 §4.3 顺序的复算命令 |

## 2. §4.2 八条语义逐条核对（结论：**八条全部符合，产品代码一行未改**）

| # | 语义 | 证据（可复算） |
|---|---|---|
| 1 | `on_pub_heartbeat()` 每拍无条件执行 | `pub_control_tick()` 第①步在**任何可能抛出的判断之前**调用（`shm_control_scheduler.h:197`）；`logs/tick_transition.log` 的 ⓪ 组：`has_peers()` 抛异常时 `hb==1` 仍成立 |
| 2 | `has_peers()` 不再是 stale 扫描总门控 | `shm_control_scheduler.h:217-231`：`if (has) {扫}` **后接**无 peer 的到期兜底分支；`logs/semantic_check_4_2.txt` |
| 3 | 有 peer 按 `pub_heartbeat` 每拍扫 | `logs/tick_transition.log` ②组：5 拍 = 5 次 `on_pub_stale_scan`；拍长 = `Entry::period()` = `timing.pub_heartbeat`（`shm_control_scheduler.cc:75-78`） |
| 4 | 无 peer 按 `peer_dead_timeout` 同量级低频兜底 | `shm_control_scheduler.h:225-231`：`now < st.next_stale_due` 才跳过；到期即扫并重排 `next_stale_due = now + dead_timeout`（2s 量级） |
| 5 | 「何时扫」判据只存在于 `pub_control_tick()` | `logs/mech_call_sites.txt`：真调用点 **2 处**，`has_peers()` 的调用点只有判据内部 1 处（`:203`） |
| 6 | 调度器与 `compat_control_loop()` 都只调 `pub_control_tick()` | 同上：`shm_control_scheduler.cc:340`、`shm_pub_sub_ipc.cc:956` |
| 7 | 不向 `PubControlState` 增加数据成员、不改 ABI 布局 | `PubControlState` 内仅 5 个纯虚/默认虚函数、**0 数据成员**；`logs/abi_size.txt`：`sizeof=8`（=`alignof`）修复前后相同 |
| 8 | 每个驱动方独立持有自己的 `PubTickState` | 驱动方① `Entry::stale`（每注册项一份，`shm_control_scheduler.cc:73`）；驱动方② `PubHeartbeatState::compat_tick_state`（每话题一份，`shm_pub_sub_ipc.cc:777`）；`compat_tick_state` 在调度器侧出现 **0** 次 ⇒ 无跨驱动方共享 |

⇒ 机械检查与语义核对**未发现**生产代码与 §4.2 不符 ⇒ 本任务**没有改产品代码**
（`changedPaths` 只含新增证据目录与新增探针/脚本；目标源文件 6/6 与 `HEAD` blob 逐文件 SHA-256 一致，见 `fingerprint.txt` 的 `blob_cmp` 行）。

## 3. §4.3.2 节拍状态转移四种情形（`logs/tick_transition.log`，18 项断言全过）

| 情形 | 断言 | 结果 |
|---|---|---|
| 首次 tick | 只设 `next_stale_due = now + dead_timeout`，**不**立刻扫 | PASS |
| 有 peer | 每拍扫，并把兜底到期点推后 | PASS |
| 无 peer 未到期 | 不扫（心跳仍每拍执行） | PASS |
| 无 peer 到期 | 扫一次并**重排** `next_stale_due`，下一周期再扫 | PASS |
| 追加：有 peer→掉回无 peer 且未到期 | 不立刻补扫 | PASS |
| 追加：`has_peers()` 抛出 | 本拍放弃 stale 判定，但 `on_pub_heartbeat` 已先执行完、异常不外逃 | PASS |

## 4. §4.3.3 双臂读数（默认臂 / L1 回退臂各 3 次）

方案三条 verify 命令（逐次读数见 `gate_runs.csv`，原始日志 `logs/arm_*.log`）：

| 命令 | run1 | run2 | run3 |
|---|---|---|---|
| `./build/bin/test_w05_stale_slot_gate`（默认臂） | PASSED (2168ms) | PASSED (2219ms) | PASSED (2218ms) |
| `./build/bin/test_w05_stale_slot_gate_arm`（双臂） | PASSED 2/2 | PASSED 2/2 | PASSED 2/2 |
| `DZIPC_SHM_CONTROL_SCHEDULER=1 ./build/bin/test_w05_stale_slot_gate`（L1 回退臂） | PASSED (2159ms) | PASSED (2153ms) | PASSED (2153ms) |

判据两条在每次运行中都成立（用例内断言）：**①陈旧槽位在 `peer_dead_timeout` 量级内被回收**（`in_use` 1→0）；**②随后挂上的活订阅者不被误断**（100/100 收到）。

最终确认组（21:08:26–21:08:50，`logs/final/summary.txt`）把三条命令再各跑 3 次：**9/9 rc=0**。
更大样本：默认臂 **78/78 通过**、L1 臂 **157/158 通过**（1 次偶发，见下）。

⚠️ 如实记录三点：
1. **常驻用例内的 kMsgs = 100**（`test_w05_stale_slot_gate.cpp:169`、`test_w05_stale_slot_gate_arm.cpp:60`）。
   方案 §4.3.3 要求「400 条消息全部收到」，因此本 run 另用**同判据的 400 条探针**
   （`src/w05_harm400_probe.cpp`，派生自 t58 的 harm 探针，只改消息条数 200→400）双臂各跑 3 次：
   `both_arms_400.csv` 六行全部 `published=400 received=400 mismatch=0`，
   且每行都记录 `crafted peer_count=0 / slot_in_use=1` → `after_hold slot_in_use=0`（回收）→ `live_slots [0]=cc1` → `final peer_count=1`（活订阅者存活）。
2. **L1 回退臂出现过 1 次偶发 `99/100`**（判据①已通过，失败在判据②；同批次默认臂 78/78 全过）。
   ⛔ 未定位机制、未淡化、未声称已修复：完整观察与判别实验见 `flake_observation.md`，原始日志在 `flake/`。
3. **两条工装尚未登记进 CTest**：`ctest --test-dir build -N` 共 29 项，其中**不含** `test_w05_stale_slot_gate*`
   （grep 0 命中）。登记归 **T03**（`test/CMakeLists.txt` 在本任务 out of scope），
   ⇒ 本轮双臂证据是**直接调用二进制**取得的，不声称 ctest 全绿。

## 5. §4.3.4 反向消融矩阵（每个变体**只改一处**；三棵消融树都是**新建独立目录**）

消融树（均在本 run 目录下，⛔ 未就地覆写任何历史变体树，尤其 `build/t58/w05only/`）：

```text
build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56/ablate-branch-removed/      # ①
build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56/ablate-driver1-removed/     # ②
build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56/ablate-driver2-removed/     # ③
build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56/baseline-prefix-dcaa0d9/    # ABI 修复前对照
```

| # | 消融点（只改一处） | 默认臂 | L1 回退臂 | 与预期一致 | 日志 |
|---|---|---|---|---|---|
| ① | `pub_control_tick()` 内**无 peer 低频兜底分支**删除 | **FAILED**(rc=1) | **FAILED**(rc=1) | ✅ 两臂同时失败 | `logs/ablate1_abs_armtest.log` |
| ② | 只删**默认臂驱动调用**（`dispatch()` 内的 `pub_control_tick(...)`） | **FAILED**(rc=1) | PASSED | ✅ 只对应臂失败 | `logs/ablate2_abs_armtest.log` |
| ③ | 只删**L1 驱动调用**（`compat_control_loop()` 内的 `pub_control_tick(...)`） | PASSED | **FAILED**(rc=1) | ✅ 只对应臂失败 | `logs/ablate3_abs_armtest.log` |

fail 码含义（用例内定义）：`1` = 陈旧槽位未被回收；`2` = 活订阅者被误断；`3` = 用例前提不成立；`-1` = 子进程异常。
三次都是 **rc=1**（"未回收"这一半先变红），说明消融命中的是"回收"判据本身，而不是误断/环境噪声。

⛔ **① 是承重证据**：判据上收成**一处**之后，消融那一处 ⇒ **两臂同时变红**；
修复前的形态是"消融调度器只红一臂"（N1：修一条、漏一条）。
②③ 进一步证明两条驱动**各自独立到达判据**，且任一驱动缺失只影响自己那一臂。

构建等价性自证：三棵消融树与工作区用**同一 cmake 选项**（Release / 无 Python / 不开消息生成器）；
与修改点无关的目标文件逐字节相同（如 `libipc/pool_alloc.cpp.o`、`libipc/shm.cpp.o`、
`dzIPC/server_ipc.cc.o`、`dzIPC/socket_pub_sub_ipc.cc.o` 的 SHA-256 两两相等），
⇒ 读数差异不来自编译选项漂移。

## 6. §4.3.5 ABI（`abi_report.txt`）

| 项 | 修复前（`dcaa0d9` 前缀树） | 修复后（工作区 `f548cd7`） | 判定 |
|---|---|---|---|
| `sizeof(PubControlState)` | 8 | 8 | 一致 |
| `alignof(PubControlState)` | 8 | 8 | 一致 |
| `sizeof(SubControlState)` | 8 | 8 | 一致 |
| `sizeof(shm_pub_ipc)` | 336 | 336 | 一致（回退臂的 `PubTickState` 藏在不完整类型内，未改变宿主布局） |
| `nm -DC --defined-only` 符号名集合 | 495 个 | 495 个 | `diff` 0 行 |
| `nm -DC` 全表符号名集合 | 1736 个 | 1736 个 | 逐名一致 |
| `.dynsym` / `.dynstr` 节尺寸 | 0xa2d8 / 0xd1a0 | 0xa2d8 / 0xd1a0 | 一致 |
| `pub_control_tick` 是否导出 | 0（不存在） | **0** | 未成为导出符号（`inline` 自由函数；目标文件级也 0 命中） |

⚠️ 如实说明：ABI 对照的另一端取 `dcaa0d9`（方案头部声明的"当前参考 HEAD"），
因为 W05 修复候选就在 `dcaa0d9 → f548cd7` 这一段里；`nm -DC` 的**整行**文本（含地址列）
在两棵独立构建树之间因地址整体平移而不同（例：`a0_cnd_broadcast 0x618c0 → 0x618e0`），
**符号名与个数一字不差** —— 集合比较因此按**符号名**而非地址列进行。这一点没有藏起来。

## 7. 本轮新增/修改的**产品代码**：无

- 目标 6 个源文件与 `git show HEAD:<path>` 逐文件 SHA-256 **6/6 一致**（`fingerprint.txt` 的 `blob_cmp` 行）。
- 新增文件全部落在本任务 in-scope 的两个目录：`build/T02/<attempt>/`、`artifacts/perf/20261001-w12-T02/`。
- ⛔ 未触碰：`src/libipc/ipc.cpp`（T05）、`test/CMakeLists.txt`（T03）、`build/t58/w05only/`、`artifacts/perf/20260928-r23-W05/` 等 out-of-scope 路径。
- 历史变体树 mtime 未变（`fingerprint.txt` 的 `untouched` 行，均为 2026-10-01 10:2x–15:0x）。

## 8. 失败或未覆盖项（如实）

1. **L1 回退臂偶发 `99/100`（1/158 次）**：`test_w05_stale_slot_gate` 在 L1 臂的复跑中出现过一次
   判据②失败（`rx=99/100`，判据①回收**已通过**），双臂用例另有 2/26 次 L1 臂 `rc=2`。
   ⛔ 未定位机制、未声称"已修复"。完整观察、判别实验（干净路径 50/50 通过、
   加压 12/12 通过）与**已排除 / 未排除**清单见 `flake_observation.md`，原始日志在 `flake/`。
   默认臂在同一批次 **78/78 通过**。
2. **`rg` 未安装**：`which rg` 为空、直接执行 exit 127。方案 §4.3.1 的命令由 run 内 shim
   （`build/T02/<attempt>/bin/rg`，`grep -rnE` 等价子集）执行；输出已落 `logs/mech_rg_publish_side.txt`，
   ⛔ 不声称"原样跑了 ripgrep"。
3. **两条 W05 工装未进 CTest**（29 项里没有它们）⇒ 本轮不声称 ctest 全绿；登记归 T03。
4. **常驻用例内核为 100 条**，方案的 400 条由独立探针覆盖（§4 第 1 点）。
5. **低频兜底不是降频优化**：本 run **不**声称任何 CPU/吞吐收益（方案 §4.2 明确禁止）。
6. 本 run **没有**做跨进程千路 / 千订阅工程可用性验证，也不对此作任何声明。
7. 本 run **没有**对 L1 回退臂做 TSan/helgrind 类并发检查（超出 T02 范围），因此第 1 条的机制未被排除。

## 9. 是否影响其他成员 / 下一步依赖

- **T03**（测试登记归口）：需要把 `test_w05_stale_slot_gate`、`test_w05_stale_slot_gate_arm`
  登记进 `test/CMakeLists.txt` 的 CTest 清单；本 run 的 `gate_runs.csv` 可作为登记后的对照读数。
- **T05**（`src/libipc/ipc.cpp`）：本 run 未触碰该文件；若 T05 改 `ipc.cpp`，需重跑 `fingerprint.txt`
  记录的工装指纹以确认工装未变。
- **成员二（§5 接口契约）**：本 run 为 §4.2 八条语义提供了逐条证据表（本文件 §2）与
  `logs/tick_transition.log`，可直接引用；本 run **未**改任何文档（W12 文档面不在本任务 in-scope）。
- 其他成员不受阻塞。
