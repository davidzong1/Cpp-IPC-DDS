# R1 分部复核 · S4 独立验收 W05（第 4 轮：(B) 路线双臂闭合与判据唯一性）

> 任务：`t84`（review-round-4）；被复核对象：t77 上以 `evidence_note` 追加登记的 **(B) 路线修复**（socket与数据面负责人）
> attempt `f4a9c132-3d24-4f9b-8509-6ea3c31d4863`；复核人：**接收池负责人**（round 1/2/3 的同一 reviewer；⛔ 非 W05/t75/t77/t84 实现者）⇒ 独立性成立
> run_id：**`20261001-r84-W05-S4R4`**；证据：`artifacts/perf/20261001-r84-W05-S4R4/`
> 复核起点指纹（`fingerprints.txt`）：
> `shm_control_scheduler.cc` = `acbf73ec…`、`shm_control_scheduler.h` = `eb6a7219…`、
> `shm_pub_sub_ipc.cc` = `067e79be…`、`shm_pub_sub_ipc.h` = `354e1748…`、
> `test_w05_stale_slot_gate.cpp` = `31758502…`、`test_w05_stale_slot_gate_arm.cpp` = `8e00fca0…`、
> [`W05/控制面接入_交付.md`](../W05/控制面接入_交付.md) = `352fca97…`、`接口责任表.md` = `f142574d…`、
> `build/lib/libipc.so.1.3.0` = `813fab5b…`、归档快照 `.cc.W05` = `256bab86…`（未被本轮触碰）
> 前序：round 1（F1 high+F2/F3 med+F4/F5/F6 low）→ round 2（N1 blocker+N2/N3 med+N4/N5/N6 low）→ round 3（**N1 high**：修复只覆盖默认臂，L1 回退臂仍 0/400）→ **本轮**
> **边界遵守**：⛔ 未改 W05 交付物、归档快照、`test/**`、产品代码、他人 WP、既有 run。全部消融均在**非产品临时树** `build/t85/**`（构建产物路径 `/home/zwc/cpp_ipc_dds/build/t85`）内完成。

---

## 0. Verdict

```text
W05 第 4 轮独立验收：verdict = needs_revision
```

**(B) 路线本身——判据唯一化 + 双臂闭合——经独立复现成立，round 3 的 N1（high）关闭。**
本轮判 `needs_revision` 的原因是**新引入的两项可复现性/一致性缺陷**（1 medium + 2 low），都不在生产逻辑里：

| 本轮承重项 | 结论 |
|---|---|
| N1 两臂均 400/400（独立复现） | ✅ **成立**（默认臂 3/3、L1 回退臂 3/3；另加两次干净重编复测） |
| N1 判据唯一性（机械核对） | ✅ **成立**：`pub_control_tick` 恰 2 个调用点、`on_pub_stale_scan` 恰 2 个调用点（均在唯一判据内部）、**无第三驱动源** |
| N1 承重消融「消融唯一判据 ⇒ 两臂同时红」 | ✅ **成立**（我自建 4 棵树的独立消融：`0/0` vs 单臂 `0/0`）；用例级也一致 |
| N1 常驻用例两臂 PASSED + 有牙 | ✅ **成立**（`test_w05_stale_slot_gate_arm` 双条 × 两臂；消融后逐臂变红） |
| ③d 布局零变化 | ✅ **成立**：`sizeof(PubControlState)` = 基线/HEAD/现树**均为 8**；导出符号 **1736 = 1736**、名字级 diff **0**；`pub_control_tick` **未导出** |
| N2 / N4 | ✅ **闭合** |
| N3 | ⚠️ **部分闭合** → **R4-N2（low）** |
| 已通过项零回退 | ✅ 逐项复测未回退（见 §6）；**但**其**文档化复算命令**已因本轮覆写而失效 → **R4-N1（medium）** |

| id | 严重度 | 一句话 |
|---|---|---|
| **R1-W05R4-N1** | **medium** | t84 在复现过程中**覆写了 `build/t58/w05only/` 变体树**（14:32），使文档 ✓6 处引用的「W05 精确快照库 `bdad6093`」**全仓已无副本**，且 §8/§10.3 的**文档化复算命令现在直接崩溃**（`malloc(): invalid size`），§10.2 的「三变体只差门控一行」演示也不再成立（`on` 现在给 400/400 而非 0/400）；t84 未在任何位置披露 |
| **R1-W05R4-N2** | low | §10.5 **同一节内部自相矛盾**：第 732 行已写「**已于 t84 全部作废，⛔ 严禁照抄**」，第 749 行仍写「本节 `.cc` 建议补丁**仍然适用**」；且那段**含已被证伪结论**的 diff 块仍**物理内联**在 758–787 行 |
| **R1-W05R4-N3** | low | §10.6/R-7 只登记了 `test_w05_stale_slot_gate` 未进 ctest；本轮**新增的** `test_w05_stale_slot_gate_arm`（承重双臂用例）同样未进 ctest 且未被任何 R-7 行覆盖；另 §1.1 第 84 行仍有 `t75_*` **悬空引用**（round 2 起遗留） |

---

## 1. N1 承重项一：两臂读数（⛔ 全部我自己跑，未采信修复者）

探针 = 从交付 §10.1 抽取的 `gate_case`（终态用控制面 API 直造，不依赖崩溃时序），现树库 `813fab5b` / 头 `354e1748`：

| 臂 | run1 | run2 | run3 | 结论 |
|---|---|---|---|---|
| 默认臂（进程级调度器） | `in_use=0` / **400 收** | `in_use=0` / **400 收** | `in_use=0` / **400 收** | ✅ |
| **L1 回退臂**（`DZIPC_SHM_CONTROL_SCHEDULER=1`） | `in_use=0` / **400 收** | `in_use=0` / **400 收** | `in_use=0` / **400 收** | ✅ 与 round 3 的 `0/400` 相比已闭合 |

（`f1/both_arms_mine.log`）对照 round 3 同一探针的读数：默认 400/400、**回退 0/400（4/4 次）**。

**修复前的 0/400 现象可复现（两条独立路径）** —— 我自建消融树 `abl_tick`（见 §3）**两臂均 `0/0`**；归档的修复前变体二进制 `build/t58/bin/t58_harm_on` 单跑亦 `published=400 received=0`（`f2/prefix_counterexample.log`）。

---

## 2. N1 承重项二：判据唯一性（机械核对，非阅读自述）

**`pub_control_tick` 的调用点 = 恰 2 处**（其余全在注释里）：

| # | 调用点 | 驱动源 |
|---|---|---|
| ① | `src/dzIPC/threepools/shm_control_scheduler.cc:340` | 进程级调度器（默认臂）|
| ② | `src/dzIPC/shm_pub_sub_ipc.cc:956` | `compat_control_loop()`（L1 回退臂）|

**`on_pub_stale_scan` 的调用点 = 恰 2 处**，且**两处都在唯一判据内部**（`include/dzIPC/threepools/shm_control_scheduler.h:221` 与 `:231`，即 `pub_control_tick` 的「有 peer 每拍」与「无 peer 低频兜底」两个分支）⇒ 两条驱动源**无法**各自决定"何时扫"。

**第三驱动源检查（⛔ 若有第三条，按合同应报 blocker）**：
- `on_pub_heartbeat` 在**产品代码**里**只有 1 个调用者** = `pub_control_tick` 内部（`:197`）；其余命中全是 `virtual` 声明、测试替身、或注释。
- 发布侧控制面驱动方在 `src/dzIPC/` 内**只有** `dispatch()` 与 `compat_control_loop()`（订阅侧 `shm_sub_ipc::compat_control_loop()` 只调 `on_sub_heartbeat`，**不涉**发布侧 stale 判据）。
- `Entry::next_stale_due` **已删除**，取而代之的是每驱动方自持的 `PubTickState`（`Entry::stale` / `PubHeartbeatState::compat_tick_state`）——**排程状态分裂、判据未分裂**，符合 (B) 路线定义。
- `has_peers()` 的**唯一**非注释使用点 = `pub_control_tick:203`。

⇒ **判据唯一性成立，未发现第三条驱动源。**

---

## 3. N1 承重项三：独立完成「消融唯一判据内部兜底 ⇒ 两臂同时红」

我在**非产品临时树** `build/t85/` 下自建 4 棵独立树（每棵 = `git archive HEAD` 全树 + 覆盖 4 个当前文件），各自 `cmake+make ipc`，用**同一个** `gate_case` 源码编译探针（每格跑 2 次，取一致）：

| 变体（我自建） | 库指纹 | 默认臂（收/发） | **L1 回退臂**（收/发） |
|---|---|---|---|
| `cur`（零消融） | `fdda9806` | 400 / 400、400 / 400 | 400 / 400、400 / 400 |
| `abl_sched`：`dispatch()` 改回 `if (has_peers) { stale_scan }` | `91578ae5` | **0 / 400、0 / 400** | 400 / 400、400 / 400 |
| `abl_compat`：`compat_control_loop()` 改回同形态 | `bcc448ba` | 400 / 400、400 / 400 | **0 / 400、0 / 400** |
| **`abl_tick`：删掉 `pub_control_tick()` 内部的低频兜底分支** | `7b7d6a5a` | **0 / 400、0 / 400** | **0 / 400、0 / 400** |

（`f2/three_ablations.log`；消融是否真在源码里已逐条 `grep` 核对，`cur` 树零命中）

**承重项 ③ 成立**：消融**唯一判据**内部 ⇒ **两臂同时红**；而分别消融**某一臂的自带判据**（= t77 的历史形态）⇒ **只红那一臂**。这正是「同一判据被复制到两条驱动路径」的结构性指纹，与本轮修复前的形态一致。

**用例级复核**（把常驻双臂用例跑在这 4 棵库上，`f3/arm_test_ablations.log`）：

| 库 | 默认臂用例 | 回退臂用例 |
|---|---|---|
| `cur` | PASSED | PASSED |
| `abl_sched` | **FAILED** | PASSED |
| `abl_compat` | PASSED | **FAILED** |
| `abl_tick` | **FAILED** | **FAILED** |

⇒ 与探针级完全一致，**判据有牙且臂粒度正确**。

---

## 4. 常驻用例两臂 + ③d 布局（独立核对）

### 4.1 常驻用例

| 用例 | 默认臂 | L1 回退臂 |
|---|---|---|
| `test_w05_stale_slot_gate_arm`（t84 新增，**双条各自 arm、`fork` 子进程**） | **2/2 PASSED**（2201 / 2202 ms） | **2/2 PASSED** |
| `test_w05_stale_slot_gate`（t77 单臂版，靠环境变量切臂） | PASSED | PASSED |
| `test_shm_control_scheduler`（20 条） | 20/20 PASSED | 20/20 PASSED |

（`f3/resident_tests_both_arms.log`）我核对了 `test_w05_stale_slot_gate_arm.cpp:184-189`：确实用 `fork()` + `setenv/unsetenv` 保证每条 arm 在独立子进程内跑（环境变量进程内只读一次）——**这个设计是对的**，否则两臂会互相污染。

### 4.2 ③d 布局核对（⛔ 不读自述，自己量）

| 量 | 基线 `e800ccc` 头 | `HEAD` 头 | 现树头 |
|---|---|---|---|
| `sizeof(PubControlState)` | **8** | **8** | **8** |
| `sizeof(SubControlState)` | **8** | **8** | **8** |
| `alignof(PubControlState)` | 8 | 8 | 8 |

（`f4/layout.log`；现树 8 = 纯 vtable，无数据成员）

**导出符号**（`nm -DC`）：HEAD 库 `1736` 行、现树库 `1736` 行；去掉地址后**名字+类型**逐条比对 ⇒ **HEAD 独有 0 条、现树独有 0 条**。`nm -DC | grep -c pub_control_tick` = **0** ⇒ 唯一判据是 `inline` 自由函数，**不进导出符号表**，天然不改变 ABI。

`compat_tick_state` 的归属也已核对：它**不在** `PubControlState` 里，而在 `src/dzIPC/shm_pub_sub_ipc.cc:777` 的 `PubHeartbeatState`（该类在公开头里只是**不完整类型**，仅被 `shared_ptr` 引用）⇒ 连 `sizeof(shm_pub_ipc)` 都未变（HEAD **336** = 现树 **336**）。

⇒ **③d 成立：布局与导出符号零变化**，与承接者「初版把判据做成非虚成员 → 替身按旧尺寸构造 → 成员被写花」的踩坑记录一致，且**改法正确**。

---

## 5. **R1-W05R4-N1（medium）**：文档化复算链被本轮覆写打断，且未披露

### 5.1 事实

`build/t58/w05only/` **变体树在 2026-10-01 14:32:35 被就地覆写**，内容 = **本轮（t84）修复后的现树源码**：

| 文件 | 覆写后 sha | 现树 sha | 应为的 W05 快照 |
|---|---|---|---|
| `src/dzIPC/shm_pub_sub_ipc.cc` | `067e79be` | `067e79be` ✅同 | `256bab86` ❌ |
| `include/dzIPC/shm_pub_sub_ipc.h` | `c682ea48` | `354e1748` | `4148d65c` ❌ |
| `src/dzIPC/threepools/shm_control_scheduler.cc` | `acbf73ec` | `acbf73ec` ✅同 | —（t84 版）|
| 库 `b/lib/libipc.so.1.3.0` | **`5f5edfcc`** | `813fab5b` | **`bdad6093`** ❌ |

t84 自己的 `artifacts/perf/20261001-t84-W05-F1/README.md:32` 写「库 = **w05only 变体树 + 本轮修复**」—— 即该树确实被当成本轮修复的构建场，但**未在任何地方登记"这棵已不再是 W05 快照树"**（我 `grep` 了 t84 的 README 与 negative/README，无一处提及 `bdad6093`/覆写/重建）。

### 5.2 后果（三条，均可机械复现）

**(a) 文档引用的「W05 精确快照库 `bdad6093`」全仓已无副本。** 我搜遍 `/home/zwc` 下所有 `libipc.so*`：**0 命中**。而交付文档**6 处**、共 **20 处**引用 `build/t58`。

**(b) §8/§10.3 的文档化复算命令现在直接崩溃。** 逐字执行文档给的命令：

```text
$ LD_LIBRARY_PATH=build/t58/w05only/b/lib artifacts/perf/20260928-r23-W05/w05_control_probe 1000 16
w05_control_probe: malloc.c:2617: sysmalloc: Assertion ... failed.        ← 崩

$ LD_LIBRARY_PATH=build/t58/w05only/b/lib build/t58/bin/t58_own_w05only 20 3
t58_own_w05only: malloc.c:2617: sysmalloc: Assertion ... failed.          ← 崩
```

原因 = **ABI 失配**（归档探针按 W05 快照头编译，`sizeof(shm_pub_ipc)` = 与覆写后的头不同）：覆写后的 t58 头给 **352**、现树头给 **336**。round 3 这条命令**是有读数的**（`artifacts/perf/20261001-r78-W05-S4R3/f3/admission6_longwindow.log`）⇒ 这是本轮**新引入**的失效。
⚠️ 该失效恰好落在文档**自己警告过的坑**上（§8：「归档二进制**必须配同一提交的库**」，并点名 `malloc(): invalid size`）。

**(c) §10.2 的「三变体只差门控一行」演示不再成立。** 我用**覆写后**的 t58 树编 ABI 匹配的探针：`on` 变体给出 **两臂皆 400/400**（而不是文档 §1.1 表里的 `0/400`）⇒ 任何人照 §10.2/§10.3 复跑，都会**无法复现文档里的最小反例表**，反而得到"修复前也是 400/400"的错误结论。

### 5.3 未被波及的部分（供承接者省力）

- **W05 快照源码本身没丢**：`build/t58/gatesrc/{src,include}` 里仍是 `256bab86` / `4148d65c`；归档 `artifacts/perf/20260928-r23-W05/shm_pub_sub_ipc.{cc,h}.W05` 亦是原值。
- 我用 §10.1b 的建树配方**自建**了一棵（`build/t85/w05repro/`：`git archive HEAD` + 覆盖归档两个 `.W05` 文件 + HEAD 调度器）⇒ 编出 `hdr=4148d65c`（**与 §10.2 声称的头指纹逐位一致**）、`lib=d07497af`（lib sha 与 `bdad6093` 不同，机制已在 round 3 查明：跨构建路径会改 link/RUNPATH 字节，同路径交换测试曾证明注释编辑 codegen-null）。**归档探针在这棵上跑得通**（`f3/admission6`：`threads_after_all=1002`、`entry_count_initial=2000`、稳态 `overrun=0`）。
- round 3 已采的读数**仍在我的 round-3 日志里**，结论未失。

### 5.4 requiredFix

1. **披露**：在 §7.5/§10 追加一行「t84 复现期间就地重建了 `build/t58/w05only/` ⇒ 该树的 `bdad6093` 已不存在；来源改为 §10.1b 的可重建配方」，并给出**新的库指纹**（`5f5edfcc`）与**它与现树同源**的事实。
2. **修读数引用**：§1.1 表、§8 的两条复算命令、§10.3、§10.2 的 `built gate_case_on ... lib=bdad6093` 回显 —— 一律改为**以 §10.1b 重建**（给出新路径与新 sha），或把 `bdad6093` 标为「历史读数、⛔ 该库已不可取」。
3. **守住纪律**：今后**不得**把「修复后的源码」写进名字里带 `w05only`（= W05 精确快照）的树；本轮修复应另建目录（如 `t84fix/`），并在 README 里登记新增/改动的方法树。
4. 建议顺手把 §10.2 的 `w05only` 更名为语义准确的名字，或在脚本里**先校验头/库指纹**再编译（现在就缺这一步，才会静默编出 ABI 失配的探针）。

---

## 6. 已通过项零回退（逐项复测）

| # | 项 | 本轮读数 | 结论 |
|---|---|---|---|
| 1 | 准入 1 线程数 + 归属证明 | 用**我重建**的 W05 快照树（因 t58/w05only 已被覆写）：默认臂 **22 / 102 / 1002**、兼容臂 **61 / 301 / 3001**（差恰 **2N**）；`wchan` 的 `hrtimer_nanosleep` 默认臂 **0**、兼容臂 **40 / 200 / 1999≈2N**（`f2/admission1.log`）| ✅ 未回退 |
| 2 | 机械证据 | 基线 `e800ccc` → 归档快照：`--numstat` **654 / 192**（.cc）、`.h` **49 / 10**；`return` 多重集**不相等**（34 → 43）；删除 192 行中含代码 **153**；控制流 `if` 86→117、`else` 5→14、`return` 50→68（`f2/mech_r4.txt`）| ✅ 未回退 |
| 3 | 运行期 `kControlTiming` | 调度臂 `pub 50.00 ms` / `sub 10.00 ms`；兼容臂 `50.63 / 10.05 ms`（`f2/timing_r4.log`）| ✅ 未回退 |
| 4 | 极性两侧同一真值表 | `unset`/`""`/`0` ⇒ 新路径；`1`/`compat` ⇒ 回退 —— SHM 与 socket **逐值相同**（`f2/polarity_r4.log`）| ✅ 未回退 |
| 5 | **R-1 机制**（Σ 精确等于回退路径唤醒率）| A 臂 34 线程 `ctx ≈ 11 061/11 157/11 148` per s；**B 臂 2 034 线程 `120 067.9 / 120 071.2 / 120 065.0`**，`scheduler entries=0` ⇒ 控制面 ≈ **120 068/s** vs Σ=120 000 ⇒ **偏差 +0.056%**（`f2/r1_r4.log`）| ✅ 未回退 |
| 6 | 千路两臂可用性 | 默认臂 `threads=34` / `entries=2000` / `handshake 1000/1000` / `rx 3000/3000` / `mismatch 0`；回退臂 `threads=2034` / `entries=0` / 同样全通（`f2/scale_both_arms.log`）| ✅ 未回退 |
| 7 | 回归 | `ctest -j4` 我跑 **2 轮**：**29/29**（59.24 s / 59.05 s）；t84 的 8 轮日志我逐份核对：7 轮 29/29、第 8 轮 `28 - test_dzflat_rx (SEGFAULT)` —— **与其自述一致**（`f1/ctest_r4.log`，`artifacts/perf/20261001-t84-W05-F1/ctest_j4_run*.log`）| ✅ 未回退 |

> ⚠️ 第 1 项我**不得不用自建树**而非文档指定的 `build/t58/w05only` —— 这本身就是 §5 的 R4-N1 的直接后果（文档化的复算入口已不可用）。

---

## 7. N2 / N3 / N4 逐条

| 项 | 核对 | 结论 |
|---|---|---|
| **N2**：§7.5「与 HEAD 零差异」更正 | §7.5（437–441 行）已改口径：对照对象 = `build/t58/w05only/` 副本（`256bab86…`），并明写「HEAD blob `ceab5033…`（已含 W06 池接入）⇒ `--numstat` **79/874**」、「`git status` 不列出是因为与 git 索引一致，不是因为它等于 HEAD」。我复算 `.W05` vs HEAD blob = **79/874** ✅ 逐位一致 | ✅ **闭合** |
| **N4**：§0 的 `27/27 → 29/29` | §0（17 行）已写「**29/29 Passed**（t84 实测，57.88 s；历史 t6 时点为 27/27…）」；我实测 29/29 ✅ | ✅ **闭合** |
| **N3**：§10.5 危险补丁作废 | 第 732 行**已**写「⛔⛔ 已于 t84 全部作废，**严禁照抄**」+ 说明理由（抄回 = 把已证伪结论写回源码）。**但**：① 同一节第 **749** 行仍写「本节 `.cc` 建议补丁**仍然适用**」（= t77 的旧状态行，未删）；② 那段含「在 `peer_count` 回到 >0 之前不会被回收」「**不在 `peer_count()==0` 时调用扫描**」的 diff 块**仍物理内联**在 **758–787** 行。⇒ 文档级"作废"已声明，但**物理上仍可被照抄** | ⚠️ **部分闭合** → R4-N2 |
| N3 附带：活源码三处注释 | ① `on_pub_stale_scan` 头注释已改为「由 `pub_control_tick()` 按统一节拍调用：真⇒每拍；假⇒低频兜底」+ 证伪说明（`:725-733`，旧句「仅当 has_peers() 为真时被调用」已删）✅；② `on_pub_heartbeat` 内「行为等价」段已删，改为指向唯一判据（`:701-706`）✅；③ `compat_control_loop()` 的自带门控已删，改为调 `pub_control_tick`（`:954-959`）✅；`接口责任表.md` 新增 t84 修订行（`:65`、`:83`）✅ | ✅ **闭合** |

---

## 8. 新增 findings 明细

| id | 严重度 | 位置 | 问题 | requiredFix |
|---|---|---|---|---|
| **R1-W05R4-N1** | medium | `build/t58/w05only/`（全树，mtime 2026-10-01 14:32:35）× [`W05/控制面接入_交付.md`](../W05/控制面接入_交付.md) §1.1/§8/§10.2/§10.3（6 处 `bdad6093`、20 处 `build/t58`）× t84 的 `README.md:32` | t84 在复现中**就地覆写**了 `build/t58/w05only/`（现内容 = 本轮修复后的源码，库 `5f5edfcc`），使文档引用的 **W05 精确快照库 `bdad6093` 全仓无副本**；§8/§10.3 的两条**文档化复算命令现在崩溃**（ABI 失配，`malloc(): invalid size`；覆写后头 352 vs 现树头 336），而 round 3 同一命令是有读数的 ⇒ 本轮新引入；§10.2 的「三变体只差门控一行」演示亦失效（`on` 现给 400/400 而非 0/400）。t84 的 README 与 negative/README **零披露**。⛔ 注意：`build/**` 受 `.gitignore` 管辖、并非"既有 run"，故**不构成**对 run 不可变纪律的违反，问题在**复算链与披露** | ① 在 §7.5/§10 追加披露行：「t84 复现期间就地重建 `build/t58/w05only/` ⇒ `bdad6093` 已不存在；改为 §10.1b 可重建配方」，并给出新库指纹 `5f5edfcc` 及"它与现树同源"的事实；② §1.1 表 / §8 复算命令 / §10.2 回显 / §10.3 中所有 `bdad6093` 与 `build/t58/w05only` 引用改为**可重建路径 + 新 sha**，或显式标注「历史读数、⛔ 该库已不可取」；③ 纪律：⛔ 不得把修复后源码写进名字含 `w05only`（= W05 精确快照）的树，修复应另建目录；④ 建议 §10.2 脚本编译前**先校验头/库指纹**，避免再次静默编出 ABI 失配探针 |
| **R1-W05R4-N2** | low | [`W05/控制面接入_交付.md`](../W05/控制面接入_交付.md) 第 **749** 行（与 732 行冲突）；753–787 行的 diff 块 | §10.5 内部自相矛盾且危险内容仍可照抄：第 732 行已声明「已于 t84 全部作废，⛔ 严禁照抄」，第 749 行却仍写「本节 `.cc` 建议补丁**仍然适用**」；含已被证伪结论（"在 peer_count 回到 >0 之前不会被回收"、`/* 不在 peer_count()==0 时调用扫描 */`）的 diff 块仍**物理内联**在 758–787 行 | 删除第 749 行的 t77 旧状态陈述（或改为「该状态已随 t84 作废」），并把 758–787 的 diff 块改为「已作废内容不再内联」的摘要（⛔ 保留原文即等于保留可照抄的坑） |
| **R1-W05R4-N3** | low | §7 的 **R-7** 行（423 行）× `test/test_w05_stale_slot_gate_arm.cpp`；§1.1 第 **84** 行 | ① R-7 只覆盖 `test_w05_stale_slot_gate`，**本轮新增的承重双臂用例** `test_w05_stale_slot_gate_arm` 同样未进 ctest（`ctest -N` 无该项、`test/CMakeLists.txt` 无 `add_test`），却未被任何 R-7 行登记；我实测两条用例都只能手工跑；② §1.1 第 84 行仍写「同目录 `t75_*` 前缀件」——**该前缀件不存在**（round 2 起遗留的悬空引用） | ① R-7 行补登 `test_w05_stale_slot_gate_arm`（并注明两条用例共 3 条判据待 `add_test`；t84 已提交 `add_test_需求.md`）；② 删掉或改写 `t75_*` 引用为实际存在的路径（§10 的 §1.1 内联件） |

---

## 9. 本包**不能支撑**的结论（⛔ 不得外推）

1. **不能**说 (B) 路线有问题：判据唯一化 + 双臂闭合这三点（两臂读数 / 判据唯一性 / 承重消融）**全部独立成立**，round 3 的 N1（high）**关闭**。本轮 `needs_revision` 只针对**可复现性与文档一致性**两项新缺陷。
2. **不能**说 `bdad6093` 被"删除"是有人故意改历史：`build/**` 是 gitignore 的构建场，t84 的 README 也自述用它做本轮构建；问题在**未披露 + 文档引用未同步**，⛔ 不是"篡改交付物"。
3. **不能**据本包给出 W05 的**最终 S4 结论**：由 R1 主体承载。
4. **不能**声称我重跑了全部历史实验：六条准入我重采了主读数（含自建快照树的线程/归属、机械证据、R-1、极性、千路、timing），但**未重跑** t58 的 52 组千路线程矩阵与公平性扫描。
5. **不能**把 §5 的复算失效读成"证据造假"：round 3 的读数**当时真实存在**（我的 round-3 日志与 t84 的 8 轮 ctest 日志都在），失效的是**当下重新复算的入口**。
6. **不能**说 `test_dzflat_rx` 的 SEGFAULT 与本轮改动有关：t84 只需如实登记；我核对 8 轮日志后**不额外指控**，但该用例未进 `RUN_SERIAL` 相关判据的核对**不在本包范围**。
7. **不能**把 `sizeof(PubControlState)==8` 直接推广到"整个公开 ABI 零变化"：本包量到的是**该类**（+ `SubControlState`）尺寸、**导出符号行数/名字集**、以及 `sizeof(shm_pub_ipc)`（336 未变）；⛔ 未做全量 ABI 比对（如所有导出类尺寸的逐一 diff）。

---

## 10. 证据索引（`artifacts/perf/20261001-r84-W05-S4R4/`）

| 文件 | 内容 |
|---|---|
| `fingerprints.txt` | 复核起点全部指纹 |
| `f1/both_arms_mine.log` | **两臂各 3 次**独立读数（默认 400/400 ×3；L1 回退 400/400 ×3） |
| `f1/ctest_r4.log` | 我跑的 2 轮 `ctest -j4`（29/29，59.24 / 59.05 s） |
| `f2/three_ablations.log` | **三次消融**（4 棵自建树 × 双臂 × 2 次）⇒ 承重项「消融唯一判据 ⇒ 两臂同时红」 |
| `f2/prefix_counterexample.log` | 修复前 0/400 的两条独立复现路径（`abl_tick` 树 + 归档 `t58_harm_on`） |
| `f3/arm_test_ablations.log` | 常驻双臂用例在 4 棵库上的臂粒度结果 |
| `f3/resident_tests_both_arms.log` | 三条用例 × 两臂 |
| `f2/admission1.log` | 准入 1（自建快照树）线程数 + `wchan` 归属 |
| `f2/mech_r4.txt` | 机械证据（654/192、`return` 34→43、删除 192 含代码 153） |
| `f2/r1_r4.log` | R-1（B 臂 120 068/s vs Σ → +0.056%） |
| `f2/scale_both_arms.log`、`f2/timing_r4.log`、`f2/polarity_r4.log` | 千路两臂 / 运行期周期 / 极性真值表 |
| `f4/layout.log`、`f4/header_mech*.txt` | ③d 尺寸与符号核对；头文件改动是否纯注释 |
| `f5/t58_tree_clobbered.log`、`f5/n2n3n4.log` | **R4-N1** 的覆写取证与失效命令；N2/N3/N4 核对 |
| `src/gate_case.cpp` | 从交付 §10.1 抽取的用例源码（双臂探针同一份） |
