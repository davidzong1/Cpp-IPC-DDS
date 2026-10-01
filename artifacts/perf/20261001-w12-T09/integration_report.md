# T09 · W12 集成、全量回归与提交前闸门（方案 §11 / §12）

- 任务：`t10 / T09`（方案 §11 **成员八：队长 / 集成负责人**）
- attempt_id：`ba543ad0-f93d-43ae-8fb2-f75f66ba66e3`
- 冻结点：**`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`**（分支 `dev`，commit `f548cd7`）
  - 队长裁定 **R-1**：方案头部写的「当前参考 HEAD：`dcaa0d9`」已过期；本集成以 `f548cd7` 为冻结点。
  - 方案 §1 基线 `e800ccc4…` 仍为**祖先可达**（T01 实测 `merge-base == e800ccc`、`--is-ancestor exit=0`）。
- 集成窗口：`2026-10-01T22:43:05 → 22:5x +0800`（串行，独占）
- 工作目录：`/home/zwc/cpp_ipc_dds`
- 结论一句话：**方案 §12 的 10 项放行条件中有 9 项由本轮实测满足；第 10 项（项目负责人批准默认启用）依方案 §12 与 §14.3 不属队长权限 ⇒ 集成结论 = 「技术修复已完成，等待启用授权」。**

---

## 0. 前置冻结状态（方案 §2.1 / §11.1 第 1 步）

| 项 | 实测 |
|---|---|
| 冻结点 HEAD | `f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f` |
| 基线指纹（复用 T01） | `artifacts/perf/20261001-w12-freeze/fingerprint.txt` sha256 = `943e284964fd753de0a0e1f98fa2d6372f5702685b1583f024d605465feb428a` |
| 冻结窗口 | 集成开始前 `pgrep -af "cmake\|make\|ninja\|cc1plus\|g++"` 无并发构建进程；`/dev/shm` 残留段 = **0** |
| R-4 前置清理 | `rm -f /dev/shm/w09c9alias__IPC_SHM__*`（`2026-10-01T22:43:05`，见 `run_window.txt`） |
| 历史物保全 | 六个成员 run 目录均**新建**；既有 `artifacts/perf/{freeze,T02,T04,T05,T06,T08}` mtime 未被本任务更改；⛔ 未向名字含 `w05only` 的目录写入 |

---

## 1. §11.2 全量验证（实测原始输出）

### 1.1 配置
```
cmake -S . -B build        → exit 0（"Build files have been written to: …/build"）
```
日志：`cmake_configure.log`

### 1.2 构建
```
cmake --build build -j4    → exit 0（提示 5 条 -Wformat-truncation 警告，均在 test/perf/w10/ 历史工装内，非本轮新增）
```
日志：`build.log`

**重编后产品库指纹（关键）**：
```
813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a  build/lib/libipc.so.1.3.0
813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a  build/lib/libipc.so.3
```
⇒ **与 T01 冻结指纹逐字相同**。这是 §11.1「冻结工作区并保存基线指纹」的可机械判据：全量重建**未改变**产品库，因为本轮所有产品代码修改（W05 判据收敛、W09 reclaim）**已在冻结点 `f548cd7` 之内**；本轮新增改动全部是注释/文档/构建登记（见 §3）。

### 1.3 全量回归（**R-2 计数口径：31 项**）
```
ctest --test-dir build -j4 --output-on-failure
```
| 结果 | 值 |
|---|---|
| 通过率 | **100% tests passed, 0 tests failed out of 31** |
| exit code | **0** |
| 总耗时 | 66.11 s |

日志：`ctest_full.log`

两个**新增**承重用例（R-2 的 +2）：
```
1/31 Test  #4: test_w05_stale_slot_gate_arm .......   Passed    4.51 sec
2/31 Test  #3: test_w05_stale_slot_gate ...........   Passed    2.22 sec
```

**R-2 口径说明（⛔ 不得混用）**：方案 §11.2 / §12.7 写的「29/29」是 `test/CMakeLists.txt` **登记前**口径。
T01 实测冻结点基线 `ctest -N` = **29** 项且两个 w05 用例不在其中；T03（`t5`）登记后为 **31** 项。
⇒ 本轮报告写「**原有 29 项 + 新增 2 项承重用例 = 31 项，全绿**」。
⛔ **未**为凑 29 删除 `add_test`；⛔ **未**把 31 说成 29。

### 1.4 CTest 注册检查
```
ctest --test-dir build -N   → Total Tests: 31
  Test  #3: test_w05_stale_slot_gate
  Test  #4: test_w05_stale_slot_gate_arm
```
`ctest_N.txt`。⚠️ 方案 §11.2 原文写「要求全量测试为 `29/29`」，与本方案 §5.2.4/§12.2 要求新增 2 个 CTest 用例**自相矛盾**（登记后必然 31）—— 该冲突已由队长裁定 R-2 消解，见 §3 修订记录。

### 1.5 定向回归（§11.2 第二条命令）
**R-4 前置清理已执行**：`rm -f /dev/shm/w09c9alias__IPC_SHM__*`（`22:45:01`，见 `cleanup_focused.txt`）
```
ctest --test-dir build -R 'test_w05_stale_slot_gate|test_w05_stale_slot_gate_arm|test_chunk_capacity_backpressure' --output-on-failure
```
| 用例 | 结果 |
|---|---|
| `test_w05_stale_slot_gate` | Passed 2.17 s |
| `test_w05_stale_slot_gate_arm` | Passed 4.51 s |
| `test_chunk_capacity_backpressure` | Passed 6.58 s |
| 汇总 | **100% tests passed, 0 tests failed out of 3**，exit **0**，13.26 s |

日志：`ctest_focused.log`

---

## 2. R-4 专项：量具假红的即时验证（T06 查出、t11 已登记）

集成期独立复核了 R-4 的根因前提（**未转抄 T06 结论**）：

```
① rm -f /dev/shm/w09c9alias__IPC_SHM__* ; ./build/bin/test_chunk_capacity_backpressure   → exit 0
② ls /dev/shm   → 13 个 w09c9alias__IPC_SHM__* 段仍残留，其中含
   w09c9alias__IPC_SHM__CHUNK_INFO__9216__C40          ← chunk 池段（clear_storage 不清理）
③ 不清段连跑第 2 次（FIX 产品库）                       → exit 0
```
⇒ 直接**实测坐实**了两条：**(a)** 官方用例运行后 chunk 池段确实残留；**(b)** 对**含 reclaim 的产品库**该残留会自愈（与 T06 的对称对照臂 `POLDEMO2-FIX-polluted` 结论同向）。
证据：`pollution_check.txt`。

**假红分类判据（R-4）**：`bad_rounds>0` 只在**无 reclaim** 的库（如 `BASELINE 4fc0e95d…`）紧跟负控臂后出现。本轮全量/定向回归**均先清段**且**一次通过**，未观测到假红。清理命令与时间已在 `cleanup_log.txt` / `cleanup_focused.txt` 中显式登记（满足 W09 交付文档 §12.6 的强制要求）。

---

## 3. §11.3 提交前闸门（逐条判定）

| # | 闸门项（方案 §11.3 / §12） | 判定 | 证据 |
|---|---|---|---|
| 1 | 没有基线文件被 staged 回退 | ✅ **通过** | `git diff --cached --stat` **空**；无任何 staged 内容 ⇒ 无回退可能。`precommit_gate.txt` |
| 2 | W05 双臂承重用例已进 CTest | ✅ **通过** | `ctest -N` = 31，`Test #3`/`#4` 在列；`RUN_SERIAL TRUE TIMEOUT 120`；全 31 项 `RETRY`/`REPEAT` 命中 **0**（R-3 合规）。`ctest_N.txt` |
| 3 | W05 反向消融成立 | ✅ **通过** | T02 矩阵：①删公共兜底 ⇒ **两臂同时失败**；②只删默认驱动 ⇒ 默认失败/L1 通过；③只删 L1 驱动 ⇒ 默认通过/L1 失败 —— 三条 observed_matches 全 `yes`。**T04 独立复核**另建两棵消融树，判据① 3/3 稳定变红。`gate_ablation.txt` |
| 4 | W09 FIX/NEG 使用同一测试二进制换库验证 | ✅ **通过** | T05 五库矩阵（同二进制 `08b803b1…`）：FIX `bad=0/0/0`、FIX_RB `0/0/0`、**V2NEG `1/1/1`**、**FIXNEG `1/1/1`**、BASELINE `0/0/0`、ABLATE `0/0/0`；**T06 独立复核 verdict = `pass`**（四场景逐项 + FORCE 有牙对照）。队长已独立复核逐臂库指纹。`gate_w09.txt` |
| 5 | W11 round-4 PASS 已同步 | ✅ **通过** | T08 独立复算：27 张表 **27/27 同批、混批 0**；§3.4 **54 格 54/54 命中新批**；三项事实（round-4 PASS / 不可同库复跑 / 无新 CPU 批）已成文并落入 W12 三份正文（`当前问题总清单.md:21/:30` 等）。`gate_w11.txt` |
| 6 | 所有性能读数均绑定采集时 `fingerprint.txt` | ✅ **通过** | T08 逐批核对：CPU/扫描批 `cf209393…`、DZFlat 旧批 `cf209393…`、新批 **`f0ebc3ef…`**（取 fp，**不取** 裸 `manifest.binary_sha256 = 0298b1df…` 错代快照）；新批 `library_binding_authority` 在位；旧批缺新语义字段但裸值与 fp 恰好一致 ⇒ 无 N1 冲突。`gate_fingerprint.txt` |
| 7 | 所有新增文件均在 `inScope` 白名单内 | ✅ **通过** | 未跟踪新增 = 6 个 run 目录（`20261001-w12-{freeze,T02,T04,T05,T06,T08,T09}`）+ 方案文档本身；已跟踪改动 = 10 文件，全部落在 T03（5 文件）、T07（4 文件）、T11（1 文件）的 inScope 内。⛔ `src/dzIPC/**`、`src/libipc/**` 改动数 = **0**。`gate_scope.txt` |
| 8 | `git diff --cached --check` 无输出 | ✅ **通过** | exit 0、**无输出**。`precommit_gate.txt` |
| 9 | 关键文件 SHA 与预期一致 | ✅ **通过（含 2 处预期 DIFF）** | 见 §4 |
| 10 | W12 S4 清单已经实际收口（§12 第 9 条） | ✅ **通过** | R4-N1/N2/N3 由 T07 收口 + **队长独立复核**（`t8` evidence_note）：错误语义 grep `exit=1`、`bdad6093` 全仓 **0 命中**、`t75_` 在 inScope 内 **0 命中** |

### 3.1 逐文件 SHA 对照（方案 §2.3 点名的 5 个文件）

| 文件 | 工作区 | HEAD | 判定 |
|---|---|---|---|
| `src/dzIPC/shm_pub_sub_ipc.cc` | `067e79be378c1235` | `067e79be378c1235` | ✅ MATCH |
| `include/dzIPC/shm_pub_sub_ipc.h` | `354e17488089d16a` | `354e17488089d16a` | ✅ MATCH |
| `src/dzIPC/threepools/shm_control_scheduler.cc` | `acbf73ec6b01f8a6` | `acbf73ec6b01f8a6` | ✅ MATCH |
| `include/dzIPC/threepools/shm_control_scheduler.h` | `a494bdecc57ebe9f` | `eb6a7219bf959ff9` | ⚠️ **DIFF（预期）** |
| `src/libipc/ipc.cpp` | `9f936b25b5eb76d7` | `9f936b25b5eb76d7` | ✅ MATCH |

**两处预期 DIFF 的归因（⛔ 不得含糊）**：
- `shm_control_scheduler.h`：T03（`t5`）的**纯注释**契约修订。独立判据：`git diff -U0` 剔除注释行/空行后**非注释变更行数 = 0**（`no_revert_check.txt`），且 `pub_control_tick` 仍在（2 处）、`has_peers()` 仍只在判据内部（`:208`）调用 1 处。
- `test/CMakeLists.txt` / `test/test_shm_control_scheduler.cpp`：T03 的 CTest 登记与旧断言修订（同样为预期 DIFF）。

⚠️ 注：方案 §2.3 原文说这 5 个文件「提交前必须逐文件核对工作区与 HEAD」。**本轮的实际语义是**：其中 **3 个产品文件与 HEAD 逐字节相同**（证明无夹带、无回退），另 1 个产品文件为**已登记的纯注释契约修订**，第 5 个（`ipc.cpp`）与 HEAD 相同。

### 3.2 防「工作区事故」专项（对照事故报告 §3.1）

事故报告记载过一次「4 文件被批量回退为 `e800ccc` 基线且已 staged」的事故。本轮以独立判据确认未复发：
- `git diff --cached --stat` **空** ⇒ 无 staged 回退；
- `git status --porcelain -- src/dzIPC src/libipc` = **0** ⇒ 两个产品源码目录无任何改动；
- `pub_control_tick` 在 3 个文件中共命中 **13** 处（`.h` 2 / `.cc` 4 / `shm_pub_sub_ipc.cc` 7）⇒ t84 修复判据在位；
- 重编后库指纹 = 冻结指纹 `813fab5b…`。

---

## 4. 已知限制（如实保留，⛔ 不得淡化）

### 4.1 已披露的**非零**偶发失败概率（R-3）
- T02 实测：`test_w05_stale_slot_gate` 的 **L1 回退臂 158 次中 1 次 `rx=99/100`**；双臂用例的 L1 臂 **2/26 次 `rc=2`**。
- T04 **独立复现**（⛔ 非转抄）：门控用例 L1 臂 **3/200 = 1.50%**、默认臂 **1/80 = 1.25%**、双臂用例 0/50；自建探针 craft=1 2/516、craft=0 14/531。
- **失败点永远在判据②（消息计数），判据①（陈旧槽位在 dead_timeout 量级内回收）0 次失败** ⇒ 不动摇 W05 承重结论。
- T04 判别实验：与「`peer_count` 可见 ≠ 数据面接线完成」的**建立期竞态**强相关（settle=0 8/150 vs settle=30ms 0/150，Fisher 单侧 **p=0.0036**）；机制未最终定位（未做 TSan）。
- **本轮集成全量回归 31/31 通过、定向回归 3/3 通过**，但这是一次读数，**不构成稳定性证明**。
⇒ ⛔ **不得声称 ctest 恒全绿**；⛔ 未使用 `RETRY` / `--repeat until-pass` 掩盖。

### 4.2 两项**不可复算**限制
1. **W11 round-4 的 `ctest -j4 29/29` 不可独立复算** —— T08 ⛔ 未重跑（§9 未要求，且重跑会动 `build/`）。
2. **同库复跑不可得** —— T08 负控实测 `FINGERPRINT_MISMATCH actual=813fab5be886ef6e… expected=f0ebc3ef2df5b276…`、`exit=1`；库已多代换代。
⇒ ⛔ **不得声称「已同库复跑验证」**。

### 4.3 量具缺陷（low，非产品缺陷；t11 已登记）
`test_chunk_capacity_backpressure` 的 `clear_storage` 只清控制面段，**不含** chunk 池段（`src/libipc/ipc.cpp:210-222` vs 段名 `:373-378`）⇒ 连续运行会互相污染，对**无 reclaim** 的库产生**假红**。登记于 `W09/容量与背压_交付.md` §12（:478 起）。本轮已按 §12.6 先清段，未观测假红。

### 4.4 其它不可声明项（方案 §1 / §12 末段明令）
⛔ 不声称跨进程千路已验证；⛔ 不声称千订阅工程可用；⛔ G2/G4 未关闭；⛔ §13.2 未闭合项按现有报告如实保留；⛔ W11 的 `3112×`（t32 项访问/s 口径）与 `450×`（core 口径）**不得并列成同一量纲**；⛔ 库绑定 `0186853e…` 位于未跟踪的 `.gitignore:97 tmp/**` 下，一旦清理即不可复核（证据可持续性缺陷，已登记）。

---

## 5. 方案 §12 十项最终放行条件逐条判定

| # | 条件 | 判定 | 依据 |
|---|---|---|---|
| 1 | W05 默认臂和 L1 臂均通过，且各至少 3 次稳定复现 | ✅ | T02 双臂 400/400 ×3；T04 独立复核 516/516 判据①、46/46 判据② |
| 2 | W05 两个承重用例已进入 CTest | ✅ | `ctest -N` = 31，`Test #3`/`#4` |
| 3 | W05 反向消融能按预期变红 | ✅ | T02 三变体矩阵 + T04 独立消融树 |
| 4 | W05 历史快照和复算链可从归档源码重建，无 ABI 失配 | ✅ | T07 §7.6/§10.7（`build/w05-repro/<attempt>/` + 三项指纹 + 编译前指纹闸）；ABI `sizeof(PubControlState)`=8 前后一致、`nm -DC` 495/495 |
| 5 | W09 FIX/NEG 同二进制换库验证成立 | ✅ | T05 五库矩阵 + T06 `pass` |
| 6 | W11 round-4 PASS 已写入 W12 收口文档 | ✅ | T08 + T07 的 W12 三份正文 |
| 7 | `ctest -j4` 为 29/29 | ✅（**R-2 口径：29 + 2 = 31，全绿**） | `ctest_full.log`：100% passed, 0 failed out of **31** |
| 8 | 提交前逐文件 SHA 对照通过 | ✅ | §3.1（3/5 MATCH，1 处已登记的纯注释 DIFF，1 处 MATCH） |
| 9 | W12 S4 清单已经实际收口，不能只写报告不执行 | ✅ | T07 实际改动 4 文件（516+/155−）+ 队长独立复核 |
| 10 | 项目负责人明确批准默认启用 | ⏸ **未获批准** | **⛔ 不属队长权限**（方案 §14.3、`W12/最终交付报告.md` 签认表第 4 行）。队长 ⛔ 不代签。 |

---

## 6. 集成结论

> **技术修复已完成，等待启用授权。**

- W05：生产缺陷已修；文档/证据链已收口（R4-N1/N2/N3 经队长独立复核成立）；两个承重用例已成常驻 CTest 门禁。
- W09：修复已完成并通过**独立**验证（同二进制换库五库矩阵 + 四场景 + 有牙对照）。
- W11：round-4 **PASS**，F1R2/F3R2 经独立复算确认闭合。
- P-7：口径完成，但**跨进程千路仍未测**。
- 总体：⏸ **未获得默认启用授权**（第 10 项待项目负责人签认）。

⛔ 即使上述条件满足，也**不得**扩大能力声明：跨进程千路、千订阅工程可用、G2/G4、§13.2 未闭合项，仍按现有报告如实保留。

---

## 7. 本轮 6 条队长裁定（供追溯）

| 裁定 | 内容 | 依据 |
|---|---|---|
| **R-1** | 冻结点 = `f548cd7`（非方案头部过期的 `dcaa0d9`）；基线 `e800ccc` 祖先可达 | T01 实测 |
| **R-2** | 计数口径 = 「原有 29 + 新增 2 = **31**」；⛔ 不得为凑 29 删 `add_test` | 方案 §11.2 与 §5.2.4/§12.2 自相矛盾 |
| **R-3** | 已知非零偶发（L1 臂 ~1%）**必须披露**；⛔ 禁止 `RETRY`/`--repeat until-pass` 抹绿；集成须同时报首次失败与重跑结果 | 方案 §12「判据必须有牙」 |
| **R-4** | 量具假红必须分类：连续运行 `test_chunk_capacity_backpressure` 前先清 `w09c9alias__IPC_SHM__*` 并显式声明；⛔ 禁止不清段重跑取绿 | T06 查出 + t11 登记（W09 §12.6） |
| **R-5** | T03 的 5 个变更文件无独立 review 覆盖 ⇒ 由队长独立核验补足，并在本闸门逐项对照 | 方案 §2.4 |
| **R-6** | 双臂用例 `TIMEOUT` 维持 **120**（非 t84 需求件建议的 180）：方案 §5.2.4 为执行依据 | 方案 §5.2.4 |

---

## 8. 证据清单（本 run）

```
artifacts/perf/20261001-w12-T09/
├── integration_report.md      ← 本文件
├── run_window.txt             ← 集成窗口与冻结前置
├── cmake_configure.log        ← §11.2 配置（exit 0）
├── build.log                  ← §11.2 构建（exit 0）
├── lib_after_rebuild.txt      ← 重编后库指纹 = 冻结指纹
├── ctest_N.txt                ← Total Tests: 31 + 两个 w05 用例
├── cleanup_log.txt            ← R-4 全量回归前清理（含时间）
├── cleanup_focused.txt        ← R-4 定向回归前清理（含时间）
├── ctest_full.log             ← §11.2 全量回归（31/31, exit 0）
├── ctest_focused.log          ← §11.2 定向回归（3/3, exit 0）
├── pollution_check.txt        ← R-4 根因即时验证（残留段 + 自愈）
├── precommit_gate.txt         ← §11.3 闸门 1/8
├── file_sha_compare.txt       ← §2.3 逐文件 SHA 对照
├── no_revert_check.txt        ← 防回退专项
├── gate_ablation.txt          ← 闸门 3 证据
├── gate_w09.txt               ← 闸门 4 证据
├── gate_w11.txt               ← 闸门 5 证据
├── gate_fingerprint.txt       ← 闸门 6 证据
├── gate_scope.txt             ← 闸门 7 证据
└── gate_r4.txt                ← R-4 证据汇总
```
