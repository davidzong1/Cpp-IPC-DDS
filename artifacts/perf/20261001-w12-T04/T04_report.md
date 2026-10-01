# T04 W05 独立复核报告（W12 §6 成员三 / 接收池负责人）

- 任务：`t6` / T04　attempt_id = `84e13e6b-7a26-44d5-80fd-04a1365107d4`
- 冻结点：**f548cd7**（`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`，tree `3cdc80e934f39bfba683b8069d2fd29a75066ec7`；队长裁定 R-1）
- 复核树：`build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4/`（独立导出 + 独立构建目录）
- 证据目录：`artifacts/perf/20261001-w12-T04/`
- 库绑定权威：**本 run 的 `fingerprint.txt`**（⛔ 不使用 manifest 生成时字段）
- **verdict：pass**（附 3 项已披露限制/未覆盖项 L-1/L-2/L-3，见 §7；needs_revision/reject 的触发条件均不成立）

---

## 1. 基准与独立重建

| 项 | 值 |
|---|---|
| 冻结提交 | `f548cd7`（HEAD 同值；工作区 `/home/zwc/cpp_ipc_dds`，分支 dev） |
| 复核树来源 | `git archive f548cd7 \| tar -x`（5770 个文件）+ **198 个** `.gitignore` 命中的生成物/未跟踪输入（`test/` 下 80 + `include/msg/python/…` 等 118；逐文件 sha256 落 `build/T04/<attempt>/logs/supplemental_inputs{,2}.txt`，另有 1 个悬空符号链接 `test/perf/artifacts/baseline_lib/libipc.so` 已如实登记为复制失败） |
| 8 个目标源文件 | 与 `git show f548cd7:<path>` 逐文件 sha256 **8/8 一致**（`fingerprint.txt` §4；`include/dzIPC/shm_pub_sub_ipc.h` 路径已修正为 include 侧） |
| 本复核树构建 | `cmake -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF -DLIBIPC_BUILD_TESTS=ON` + `--target test_w05_stale_slot_gate test_w05_stale_slot_gate_arm`（`logs/cmake_A.log`、`logs/build_frozen2.log`；rc=0） |
| 判据来源 | **本复核树**的 `libipc.so.1.3.0 = 6dc1e3b292b0ba0cc60bb576d0e35446899fb3b6ba69ecc62de7473396862c30` 与两个用例二进制；T02 的构建树**未**被用作判据来源 |

**构建等价性**（`logs/build_equivalence.txt`）：被测源文件的 `.o` 与工作区冻结 build 逐节相同（`.text/.rodata/.data/.data.rel.ro` SAME）；产品库**字节数相同（1291248）且 `.text/.rodata/.data` 逐节相同**，整体 sha256 不同的唯一来源是绝对路径 debug 串与 build-id。测试可执行文件的 `.text` 差异可完全归因于 gtest 静态归档不同（工作区归档系 9 月 26 日构建，本复核树按 f548cd7 源码重编），⛔ 不可归因于源码或编译选项。

> ⚠️ 冻结提交 `f548cd7` **不含** `test/perf/w10/w10_rebuild_crash.cpp`（该文件被 `.gitignore` 的 `*build_*` 规则忽略、且未被跟踪），而 `test/CMakeLists.txt:797` 引用了它 ⇒ 纯 `git archive` 导出**无法配置**（`No SOURCES given to target`）。本复核按 §1 表内方式补入 198 个生成物/未跟踪输入后配置成功；该事实已落盘，供 W12 收口时决定是否把该文件纳入版本控制。

## 2. 验收项 3：真实故障窗口的构造（原始输出）

探针 `artifacts/perf/20261001-w12-T04/src/t04_stale_window_probe*.cpp`（**与 gtest 用例独立实现**，不链接 gtest、不复用其构建树），流程 = `add_peer(gen)` → `acquire_peer_slot(gen, cc_id)` → `remove_peer(gen)`，**故意不调用** `release_peer_slot()`。

```
$ build/T04/<attempt>/bin/t04_probe 7801 400            # 默认臂
CRAFT_DONE slot=0 peer_count=0 slot_in_use=1 slot_cc_id=1 (release_peer_slot NOT called)
```
```
$ DZIPC_SHM_CONTROL_SCHEDULER=1 build/T04/<attempt>/bin/t04_probe 7811 400   # L1 回退臂
CRAFT_DONE slot=0 peer_count=0 slot_in_use=1 slot_cc_id=1 (release_peer_slot NOT called)
```
⇒ **`peer_count == 0` 且 `slot.in_use == 1`**（心跳保持陈旧），构造判据成立。探针在构造后立即二次读段复核，读不出要求状态即 `CRAFT_FAIL` 退出码 2（全程未发生）。
原始日志：`logs/probe_default_run{1,2,3}.log`、`logs/probe_L1_run{1,2,3}.log`、`logs/settle2_*.log`、`logs/flake_probe_*.log`（共 1746 份）。

## 3. 验收项 4：两项承重判据

### 判据①：`slot.in_use` 在 `dead_timeout` 量级内变为 0

逐毫秒采样共享段（`REAP_TRACE` 每 200 ms 一行、心跳年龄 ≥1.75 s 后每 25 ms 一行）：

```
REAP_TRACE t=1901ms peer_count=0 slot_in_use=1 hb_age=1901ms
REAP_OK    t=2100ms peer_count=0 slot_in_use=0 last_seen_hb_age=2099ms in_use_samples=1962   # 默认臂
REAP_OK    t=2004ms peer_count=0 slot_in_use=0 last_seen_hb_age=2003ms in_use_samples=1864   # L1 臂
```

| 口径 | 运行数 | 判据①成立 | 回收耗时 min/中位/max |
|---|---|---|---|
| 默认臂（`craft=1`） | 56 | **56/56** | 2050 / 2100 / 2101 ms |
| L1 回退臂（`craft=1`） | 460 | **460/460** | 2001 / 2003 / 2008 ms |
| 合计 | **516** | **516/516** | 全部落在 `peer_dead_timeout = 2 s` 量级 |

⇒ 判据①在整个复核样本中**无一次失败**（`flake/all_probe_runs.csv` 可逐条追溯）。

### 判据②：随后建立活订阅者 + 发布 400 条 ⇒ 400/400

| 组 | 运行数 | 400/400 |
|---|---|---|
| 默认臂 `craft=1` | 13 | **13/13** |
| L1 回退臂 `craft=1` | 33 | **33/33** |
| 合计 | **46** | **46/46** |

`RESULT arm=default-sched craft=1 n=400 received=400 misses=0 first_miss_at=-1 criterion1_reaped=1 reap_ms=2100 criterion2_pass=1`
`RESULT arm=L1-compat    craft=1 n=400 received=400 misses=0 first_miss_at=-1 criterion1_reaped=1 reap_ms=2004 criterion2_pass=1`

## 4. 验收项 5：默认臂与 L1 臂各 3 次（合同 verify）

| 命令 | 运行 | 结果 |
|---|---|---|
| `./build/bin/test_w05_stale_slot_gate` | 3 | 3/3 rc=0（2175/2174/2223 ms） |
| `./build/bin/test_w05_stale_slot_gate_arm` | 3 | 3/3 rc=0（4510/4412/4408 ms） |
| `DZIPC_SHM_CONTROL_SCHEDULER=1 ./build/bin/test_w05_stale_slot_gate` | 3 | 3/3 rc=0（2163/2163/2159 ms） |
| `git status --short include/dzIPC src/dzIPC` | 1 | 见 §7 限制 L-1 |

本复核树工装同三条命令各 3 次亦全部 rc=0（`logs/verify3x_summary.txt` 的 `gate_default/gate_L1/arm_both` 各 3 行 rc=0；收尾复核见 `logs/final_verify_contract.txt` 与 `logs/final_frozen_{default,L1,arm}.log`）。
日志：`logs/verify3x_summary.txt`、`logs/ws_gate_*.log`、`logs/gate_*_run*.log`、`logs/arm_run*.log`。

## 5. 验收项 6：消融库必须让至少一项承重判据变红

两棵**新建**消融树（源码 diff 落 `ablation/*.diff`，逐文件指纹落 `fingerprint.txt` §5/§6）：

| 变体 | 消融点 | 默认臂 | L1 臂 | 指纹 |
|---|---|---|---|---|
| A `ablate-fallback-branch-removed` | 唯一判据 `pub_control_tick()` 的「无 peer 低频兜底」分支 | 判据①**变红**（:146 + :189，3/3 稳定） | 判据①**变红**（3/3） | 库 `134c257cbb1929b0…` |
| B `ablate-l1-driver-removed` | L1 回退臂（`compat_control_loop`）的驱动调用 | **仍绿**（:146 通过） | 判据①**变红**（:146，3/3） | 库 `fb9d8a0c4189bfb5…` |

探针原始输出（变体 A）：
```
REAP_FAIL timeout_6000ms last_seen_hb_age=6001ms
SUB_ATTACHED=1 peer_count=1 live_slots=[0]=cc1(hb_age=6012ms)[1]=cc1(hb_age=0ms)
RESULT arm=default-sched craft=1 n=400 received=36  misses=364 first_miss_at=36  criterion1_reaped=0   # 变体 A 默认臂
RESULT arm=L1-compat    craft=1 n=400 received=6   misses=394 first_miss_at=6   criterion1_reaped=0   # 变体 A L1 臂
```
变体 B（只掐 L1 驱动 ⇒ L1 判据①变红、默认臂不变红）说明判据**按臂**都有牙，而不是"只要任一臂变红"。
日志：`logs/ablation_A_{default,L1}.log`、`logs/ablation_B_{default,L1}.log`、`logs/ablation_gate*.log`。

## 6. 验收项 7/8：历史读数与库绑定

- **`bdad60939e9c75f80eff87c9f669268a09ea9b00e1539240e94a1167eb84a86c` = 历史读数**。本轮对 `build/**` 与 `artifacts/**` 全量遍历全部 `libipc.so*` 实体（79 个实体，含各变体树），**命中数 = 0**（`logs/bdad6093_exists_check.txt`）。⇒ 本复核**不使用**该库的任何读数，⛔ 不伪造其可用性。
- 库绑定一律引用本 run `fingerprint.txt` 的 §1/§2（含 `ldd`/`readlink`/`RUNPATH` 解析目标），⛔ 未使用任何 manifest 生成时字段。
- 轻量 ABI 旁证（`logs/abi_side_check.txt`，非本次目标）：`nm -DC` 导出符号 1736/1736 且**名集合完全相同**，`pub_control_tick` 导出计数 0/0。

## 7. 已披露限制与未覆盖项

**L-1（工作区状态，非本任务造成）**：合同验收文字为「`git diff` 对 `include/dzIPC`、`src/dzIPC` 为空」。实测 `git status --short include/dzIPC src/dzIPC` 为 **1 行**：`M include/dzIPC/threepools/shm_control_scheduler.h`。
- 归因证据：该文件 mtime = 2026-10-01 **21:26:27**，**早于本任务开工时刻 21:35:26**（`logs/scope_mtime_start.txt` 为开工时留档，`logs/scope_mtime_end.txt` 与之 **MTIME_IDENTICAL**）；同一个改动已在 T03（t5）的 §13 回报中登记为「接口契约修订（纯注释）」。
- 机械判定：`git diff -U0` 该文件的 11 个变更行中，**非注释行 = 0**（`logs/scope_diff_lines.txt`）。
- 结论：**T04 自身对 W05 产品代码零改动**（`src/libipc/` 空、`src/dzIPC/` 空、8 个目标源文件 mtime 逐条不变）；上述 1 行为 T03 的既存、已登记改动。⛔ 本条**不应**被读作"T04 复核通过后就地改了产品代码"。
- 附注（非本任务判据）：该既存注释新增了一句「`test_w05_stale_gate*` 登记为 `RUN_SERIAL TRUE TIMEOUT 120`」，与 `test/CMakeLists.txt` 的实际登记一致（T03 交付）。

**L-2（已复现的偶发，队长裁定 R-3 必答项）**：
- **我自己实测的频率**（含 95% 置信区间不必给，逐条可查）：

| 口径 | 运行数 | 失败 | 频率 |
|---|---|---|---|
| 门控用例 · L1 回退臂（本复核树） | 200 | 3（均 `rx=99/100`） | **1.50%** |
| 门控用例 · 默认臂（本复核树） | 80 | 1（`rx=99/100`） | **1.25%** |
| 双臂用例（默认臂 + L1 臂） | 50 | 0 | 0.00% |
| 自建探针 `craft=1`（有陈旧槽位） | 516 | 2 | 0.39% |
| 自建探针 `craft=0`（干净路径，无陈旧槽位） | 531 | 14 | 2.64% |

- **失败点永远是判据②**：门控用例 280 次运行中，判据①断言（`test_w05_stale_slot_gate.cpp:146`）失败 **0 次**，判据②断言（`:189`）失败 4 次；探针 516 次 `craft=1` 运行中判据①**516/516 成立**。⇒ **② 的偶发绝不是① 的失败**：这些样本的陈旧槽位都在 `dead_timeout` 量级内被回收，回收之后断了的是"首条消息"。
- **形态（全部失败样本一致）**：`first_miss_at = 0`（丢的是**第一条**）；`rx_late = 0`（在 200 ms + 1.5 s 延长等待窗内**始终未到**）；随后一轮 publish 才取到它（`MISMATCH at index=1 got="t04-0" want="t04-1"`）。等待时长直方图：成功样本 max ≈ 1.05 ms，失败样本 max ≈ 201 ms（中间约 190× 真空）。
- **我做的判别实验（自证独立，不转抄 T02）**：
  1. **赛道隔离**：同臂同协议 n=100，`craft=1`（有陈旧槽位）失败 **2/427**，`craft=0`（无陈旧槽位）失败 **10/280**；同刻**交错**跑（paired）时 `craft=1` **0/60**、`craft=0` **4/60**。⇒ 偶发**不是**由陈旧槽位/回收路径引入的，干净路径上反而更频繁。
  2. **建立期竞态定向**：挂上订阅者后插入静默期，仅此一个变量 —— `settle=0 ms` 失败 **8/150（5.33%）**，`settle=30 ms` 失败 **0/150**（Fisher 单侧 **p = 0.0036**）。⇒ 偶发与「`peer_count>=1`（控制面登记可见）≠ 数据面接线完成」这一**建立期**强相关，与 stale 回收无关。
  3. **机制未定位**：本轮**未**定位到最终代码级机制（未做 TSan/helgrind），⛔ 因此既不声称"已修复"、也不声称"仅偶发"。它与 T02 登记的方向一致（T02：L1 臂 1/158、双臂 L1 臂 2/26 rc=2），但两项新增事实是 **T02 未登记的**：默认臂也可复现（1/80），且**干净路径失败率高于带陈旧槽位的路径**。
- **对承重结论的影响判定：不动摇。** 判据①在 516/516 次 `craft=1` 复核中成立；判据②在验收规模（400 条）上 **46/46 = 400/400**；偶发只出现在**首条**上，且在有/无陈旧槽位两条路径上都出现（干净路径更多）。⛔ 因此不得把它表述为「判据①失败」或「回收不成立」，也不得用它判 reject。

**L-3（未覆盖项，⛔ 不扩大结论）**：本复核**未**覆盖跨进程千路/千订阅、G2/G4、完整 ctest 全绿；⛔ 不声称千订阅工程可用、⛔ 不声称 `ctest` 恒全绿（L-2 已实测到反例）。CPU 加压组（32 路忙循环，loadavg 均值 8.8–14.0）中 400 条端到端 **20/20 全收**、判据① **20/20 成立**，但这不构成吞吐/规模结论。

## 8. 复算命令

`artifacts/perf/20261001-w12-T04/src/commands.sh`（逐条可粘贴；含冻结树导出、配置、构建、三条 verify、探针 400 条、消融、指纹）。
逐次读数表：`flake/gate_runs_clean.csv`、`flake/arm_runs_clean.csv`、`flake/all_probe_runs.csv`、`flake/e400_matrix.csv`、`flake/paired_craft.csv`、`flake/attach_race.csv`、`flake/first_msg_latency.csv`、`flake/first_visible.csv`、`flake/flake_stats_T04.csv`。

## 9. 证据目录完整性检查结果

- 目录创建前**不存在**（`artifacts/perf/20261001-w12-T04` absent at 21:35:26）；未覆盖/改动任何既有 `artifacts/perf/*`（`20261001-w12-{freeze,T02,T05,T06,T08}` 的 mtime 均未变）。
- 内容：`fingerprint.txt`（99 行）+ `logs/`（1746 文件，7.0 MB）+ `T04_report.md`+ `src/`（14）+ `flake/`（22）+ `ablation/`（6），共 7.4 MB。
- 合同 §6.3 交付物对照：独立复核报告 = 本文件；双臂日志 = `logs/gate_*_run*.log`、`logs/arm_run*.log`、`logs/probe_*_run*.log`；修复版/消融版指纹 = `fingerprint.txt` §1/§5；可复制复算命令 = `src/commands.sh`；完整性检查 = `logs/evidence_integrity_check.txt`。
