# T05 · W09 reclaim 并发修复保护与五库换库矩阵（方案 §7 / 成员四：共享层负责人）

- run_id：`20261001-w12-T05`
- attempt_id：`37e6b190-b12b-4128-b770-917b299297aa`
- 归属任务：`t4 / T05（方案 §7 成员四：共享层负责人，W09 修复保护）`
- 采集窗口：2026-10-01 20:42 → 21:06 +0800
- 基线 HEAD：`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`（分支 `dev`）——
  与 T01 登记一致；⚠️ 方案头部写「当前参考 HEAD：dcaa0d9」，实际已前进到 `f548cd7`（T01 已如实登记）。
- 结论：**五库换库矩阵成立** —— `FIX`（产品库本体）`bad_rounds=0`；`V2NEG`、`FIXNEG` 各 3/3 复现
  `bad_rounds=1`；`BASELINE`、`ABLATE`（无 reclaim / 消融）恒绿 ⇒ 缺口确由 **reclaim 进入 + 快照位置**决定。

---

## 一、§7.3.1 机械检查（rg）

方案给的命令：

```bash
rg -n "orphan_candidate|orphan_snap|reclaim_orphan_segment" src/libipc/ipc.cpp
```

**执行结果（三路互证，逐行一致，原始留痕见 `mech_rg.txt`）**：

| 路径 | 做法 | 结果 |
|---|---|---|
| ① 直接调用 | `rg …`（PATH 上无 rg） | `bash: rg: 未找到命令`，**exit=127** |
| ② 定位二进制 | 本机装有 ripgrep，只是不在 PATH —— codex 打包的 `…/codex-path/rg`，`ripgrep 15.2.0 (rev e89fff89ac) features:+pcre2` | — |
| ③ **按方案原文执行** | 用 run 目录内的 `scripts/rg-forwarder.sh`（exec 到上述 rg 二进制）放进 PATH 后**逐字**执行同一条命令 | **exit=0**，命中 10 行 |
| ④ 等价替代 | `grep -nE` 同正则同文件 | exit=0，命中**同一批行** |

输出（③④ 逐行一致）：

```
$ rg -n "orphan_candidate|orphan_snap|reclaim_orphan_segment" src/libipc/ipc.cpp
469:  bool reclaim_orphan_segment(chunk_info_t *info, ipc::string const &prefix,
471:                              bool orphan_candidate,
477:    if (!orphan_candidate || snap_in == nullptr) return false;
549:        bool orphan_candidate = false;
550:        ipc::id_pool<> orphan_snap;
569:                std::memcpy(&orphan_snap, &probe->pool_, sizeof(orphan_snap));
570:                orphan_candidate = true;
586:         * reclaim_orphan_segment 的论证), 是则整体复位空闲链。⛔ 这一步必须在
591:          (void)reclaim_orphan_segment(info, pref, chunk_size, "attach",
592:                                       orphan_candidate, &orphan_snap);
```

原始留痕：`mech_rg.txt`（含 ①②③④ 四段与转发器路径）。

## 二、§7.3.2 锁序与临界区（机械核对，可离线重放）

脚本：`scripts/check_lock_order.py`（`python3 scripts/check_lock_order.py --run-dir <run>`）。
输出：`lock_order_check.txt`、`mech_check.txt`。五项判定**全部通过**：

| 判定 | 机械读数 | 结论 |
|---|---|---|
| A 顺序①–⑤全在 `handles_` 的 `lock_` 临界区内 | `lock_guard` 行 552，临界区闭括号行 571；①行 553 ②行 554 ③行 555 ④行 566 ⑤行 569 | 通过 |
| B `memcmp` 复核在 `info->lock_` 持锁区间内 | `lock` 行 486 → `memcmp` 行 487 →（不等则提前 `unlock` 行 489，**不写池**）→ `reset_free_chain` 行 492 → `unlock` 行 493 | 通过 |
| C 锁序始终 `handles_.lock_ -> info->lock_` | 全文件 `handles_` 的 `lock_` 取用点 **1** 处（行 552）；「先持 `info->lock_` 再取 `handles_` 锁」的逆序嵌套 **0** 处 | 通过 |
| D `/proc` 探活在两把锁之外 | 探活行 482；在 `handles_` 临界区内 = False；在任一处 `info->lock_` 持锁区间内 = False；在取锁之前 = True | 通过 |
| E 变体差异局部性 | 允许区间 `[465,496] ∪ [537,595]`；`ABLATE`/`V2NEG`/`FIXNEG` 相对 HEAD 的差异**全部**落在该区间内（区间外行数 = 0） | 通过 |

⇒ **段名构造**（`chunk_segment_name` / `CHUNK_INFO__` / `__C<cap>`，锚点各 2 处、逐字未动）、
**容量布局**（`large_msg_cache`，锚点 2 处）、**`published()` 保护语义**与其余全部产品路径**均未被触碰**。

### 2.1 实验臂的"回退可逆"证明（防止改错对象）

`scripts/make_variants.py` 除生成三个负控臂外，还把每个负控臂的改法**逐字还原**：

```
ABLATE_REV == HEAD 逐字还原: True   sha256=9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4
V2NEG_REV  == HEAD 逐字还原: True   sha256=9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4
FIXNEG_REV == HEAD 逐字还原: True   sha256=9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4
```

即三个负控臂各自都是对 HEAD 的**单点改动**，且**只**改动 reclaim 相关行（差异 hunk 各 1 个，
触及 8 / 21 / 11 行，全部落在 §二.E 的允许区间内）。归档源码见 `src/ipc.cpp.*`。

## 三、§7.3.3 五库（同一测试二进制，只换库）

**唯一变量 = `LD_LIBRARY_PATH` 指向的 `libipc.so.1.3.0`**；工装二进制与探针二进制全程不变
（`ldd` 逐个臂确认命中路径；探针自身在运行时读 `/proc/self/maps` 把**实际加载库的真实路径 + SHA-256**
打进输出，见 `diag/self_fingerprints.txt`）。

| 臂 | 含义 | 源文件 sha256 | 库 sha256（完整） | 大小 B |
|---|---|---|---|---|
| BASELINE | `e800ccc` 的 `ipc.cpp`（**尚不存在** `reclaim_orphan_segment`） | `540b6f4c…edf8` | `4fc0e95dc316ac01972edd7fe6efde4eb13de8f4793c08b05ea95eae29dd3a71` | 1285504 |
| ABLATE | HEAD + **删除 reclaim 调用** | `640d75c3…5742` | `52ccbe9e7e054aada32d5ec28f0ea97085ea7d6e1403e5dc756dc88878dde732` | 1285960 |
| V2NEG | HEAD + **快照移到 `handles_` 的 `lock_` 之外** | `ed434b9f…0e35` | `3380016ba38084d8071d00133096298c733e1ebc8de16dde969a58456353d791` | 1291248 |
| FIXNEG | HEAD + **保留 reclaim 与快照，删除 `info->lock_` 下的 `memcmp` 复核**（破坏正确临界区） | `8f54c77c…12d3` | `1e7d5d7f12c20919e3994b9bdcd818375ba53afb11d99e8f74cb1a21c69dfc91` | 1291248 |
| **FIX** | **冻结的产品库本体 `build/lib/libipc.so.1.3.0`** | — | `813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a` | 1291248 |
| FIX_RB | 我按 `flags.make`/`link.txt` 逐字重建的 FIX（交叉核对臂） | `9f936b25…ebf4` | `c928deaa4f480ee361e5974bf824c85c87658f2a6e9113bc0d3415568264b32d` | 1291248 |

**FIX 臂用的是仓库现成的产品库本体**（sha256 `813fab5b…` = T01 登记值），**不是**我重建的副本
⇒ 本次读数直接承重在被冻结的产品产物上。`FIX_RB` 是**额外的独立构建路径**交叉核对：
两条路径（产品库本体 / 逐字重建）在全部 18 次读数上给出**同一判据结论**（见 §四）。

### 3.1 库的构建方式与产物一致性（如实登记）

- 编译/链接旗标**逐字取自** `build/src/CMakeFiles/ipc.dir/flags.make` 与 `link.txt`
  （含 CMake 原样保留的重复 `-O3 -O3 -DNDEBUG -DNDEBUG -O2 -fPIC -O3` 与完整 rpath 串）。
- **旗标逐字照抄可完全复现既有目标文件**（与编译输入路径无关）：
  `c++ <flags.make 逐字旗标> -c src/libipc/ipc.cpp -o /tmp/fix_exact.o`
  ⇒ sha256 `3cdb81d6fa3226ef50f8b56aabff5ac4eabbc1b5f260bce13dbefea4ec9417c0`，
  与 `build/src/CMakeFiles/ipc.dir/libipc/ipc.cpp.o` **逐字节一致**。
- **旗标不全则不一致**：非逐字旗标（如 `-O2 -DNDEBUG`）编译同一路径得到
  `285bbab3aa84480a…` ⇒ 与既有目标文件不同 ⇒ **旗标是承重的**，本 run 全程只用逐字旗标。
- ⚠️ **如实登记的构建不可复现性**：把**未改动的** `ipc.cpp.o` 用同一 link.txt 重新链接，
  得到的 `libipc.so` 与该库仍**不是逐字节相同**（`.rodata`/`.rela.dyn` 完全相同，
  `.text`/`.dynsym` 与 build-id 不同；`.text` 差异 123373 字节、且集中在中段）。
  原因未定位（链接器版本/序或 `-flto` 之外的因素）；**本任务不声称二进制可复现构建**。
  这不影响本期判据：六个臂的 `nm -D --defined-only` 导出符号集**与产品库完全一致（1500 个）**，
  `ldd -r` 未解析符号 **0**，且每臂的判据结论都与其库 sha256 绑定归档。

## 四、§7.3.4/§7.3.5 五库矩阵读数

### 4.1 我的判据探针（`probe/t05_alias_probe.cpp`，每臂 3 次独立复跑）

探针与仓内常驻用例 `ChunkCapacityBackpressure.OrphanResetDoesNotAliasInflightLoansAcrossTopics`
**同逻辑**（专属前缀 / 8 话题并发首借 / 每轮 fork 种子进程 + 被测进程 / 只在开跑前清一次段），
但**独立成型**：它把「实际加载库路径 + SHA-256」「`same_id_pairs` / `same_data_ptr` / `bad_rounds` /
`skipped_rounds` / `executed` / `alias_hit_rounds` / `crash_rounds` / `first_alias_round`」
直接打在 stdout，使**读数与库的绑定不依赖外部脚本转述**。

判据口径（与仓内一致，⛔ 未放宽）：
- 专属**非空**前缀（`w12t05_<ARM>_<k>`，每臂每轮各不相同）；
- **8 个话题并发首借**，`loan(8000)` ⇒ 档 9216（`calc_chunk_size` 后为 9216）；
- **至少 1200 轮**（`rounds=1200`；`CASES == 1200` 时执行轮数见下表）；
- **⛔ 不每轮清段**：`clear_storage` 只在开跑前执行一次；每轮的"非素净"由种子进程现造
  （种子进程借 3 块后 `_exit`，不归还）；
- 判据：两个**不同话题**拿到相同 `loan_t::id` 或相同 `data` 指针 ⇒ bad round。

| 臂 | 库 sha16 | k | rc | executed | **bad_rounds** | same_id_pairs | same_data_ptr | alias_hit_rounds | first_alias_round | orphan reset 次数 |
|---|---|---|---|---|---|---|---|---|---|---|
| BASELINE | `4fc0e95dc316ac01` | 1 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| BASELINE | `4fc0e95dc316ac01` | 2 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| BASELINE | `4fc0e95dc316ac01` | 3 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| ABLATE | `52ccbe9e7e054aad` | 1 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| ABLATE | `52ccbe9e7e054aad` | 2 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| ABLATE | `52ccbe9e7e054aad` | 3 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 0 |
| V2NEG | `3380016ba38084d8` | 1 | **1** | 367 | **1** | 3 | 3 | 1 | 366 | 65 |
| V2NEG | `3380016ba38084d8` | 2 | **1** | 32 | **1** | 1 | 1 | 1 | 31 | 9 |
| V2NEG | `3380016ba38084d8` | 3 | **1** | 910 | **1** | 1 | 1 | 1 | 909 | 166 |
| FIXNEG | `1e7d5d7f12c20919` | 1 | **1** | 1 | **1** | 1 | 1 | 1 | 0 | 1 |
| FIXNEG | `1e7d5d7f12c20919` | 2 | **1** | 1 | **1** | 2 | 2 | 1 | 0 | 1 |
| FIXNEG | `1e7d5d7f12c20919` | 3 | **1** | 1 | **1** | 2 | 2 | 1 | 0 | 1 |
| **FIX** | `813fab5be886ef6e` | 1 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 148 |
| **FIX** | `813fab5be886ef6e` | 2 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 232 |
| **FIX** | `813fab5be886ef6e` | 3 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 164 |
| FIX_RB | `c928deaa4f480ee3` | 1 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 240 |
| FIX_RB | `c928deaa4f480ee3` | 2 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 138 |
| FIX_RB | `c928deaa4f480ee3` | 3 | 0 | 1200 | **0** | 0 | 0 | 0 | -1 | 164 |

- 每行对应自己的 run 目录 `arms-run/<ARM>-<k>/fingerprint.txt`（18 个，读数与该目录的
  `lib_sha256` / `probe_sha256` / `binary_sha256` 逐项绑定）；
  表汇总：`summary_matrix.tsv`；原始驱动日志：`arms-run/matrix.driver.log`、`arms-run/matrix.raw.tsv`。
- **失败形态已分类**：负控臂的 bad round 全部是 **`T05_ALIAS_HIT`（真·别名命中）**，
  `crash_rounds=0`、异常退出 0 —— 不是写崩、不是超时、不是 rc=127 之类的误计。
  例如 `V2NEG-1`：`T05_ALIAS_HIT round=366 rc=1 same_id_pairs=3 same_data_ptr=3 held=8`。

### 4.2 仓内常驻用例（官方工装，同一二进制换库）

`build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*'`，
`kRounds=1200`（源内常量，⛔ 未改测试文件；`test/CMakeLists.txt` 亦未动）。
每臂 run 目录 `gtest-arm/<ARM>/`（含 `run.log` 与 `fingerprint.txt`）：

| 臂 | 库 sha16 | 退出码 | gtest 判定 | 读数 |
|---|---|---|---|---|
| BASELINE | `4fc0e95dc316ac01` | 0 | PASSED | `bad_rounds=0 skipped=0` |
| ABLATE | `52ccbe9e7e054aad` | 0 | PASSED | `bad_rounds=0 skipped=0` |
| **V2NEG** | `3380016ba38084d8` | **1** | **FAILED** | `bad_rounds=1 skipped=0` |
| **FIXNEG** | `1e7d5d7f12c20919` | **1** | **FAILED** | `bad_rounds=1 skipped=0` |
| **FIX** | `813fab5be886ef6e` | 0 | PASSED | `bad_rounds=0 skipped=0` |

各臂 `orphan segment reset` 计数（证明复位路径**确实在跑**）：
BASELINE 0 / ABLATE 0 / V2NEG 186 / FIXNEG 2 / FIX 149。

⇒ 与 §4.1 的自建探针**逐臂同判**：两条独立判据路径、同一批库，结论一致（不互相替代）。

### 4.3 判读（§7.3.5）

1. `FIX`（**产品库本体**）：3/3 `bad_rounds=0`，1200 轮**全部执行**（`executed=1200`），
   且 `orphan segment reset` 计数 148/232/164 ⇒ **复位路径确实在跑**，不是"路径未执行"的假绿。
2. `V2NEG`：3/3 `bad_rounds>0`（别名命中）⇒ 「快照移出 `handles_` 临界区」**直接承重**，缺口可复现。
3. `FIXNEG`：3/3 `bad_rounds>0`（**首轮即命中**）⇒ 「删除 `memcmp` 复核」同样直接承重
   ⇒ 两条保护（快照取点 + 持锁复核）**各自独立有牙**。
4. `BASELINE`：`orphan segment reset=0`、`bad_rounds=0`、`executed=1200`
   ⇒ 该代码根本**没有**这段 reclaim（结构佐证：二进制内不含
   `chunk pool orphan segment reset` 日志串、无 `opendir/readdir` 未解析符号）⇒ 缺口由 reclaim 引入。
5. `ABLATE`：源码里保留了整段 reclaim 代码但**删掉调用点**（编译器随之把无人引用的
   匿名命名空间函数做 DCE 掉 ⇒ 产物里 `orphan segment reset` 日志串计数为 0，
   与 BASELINE 的产物形态一致）⇒ `orphan reset=0`、`bad_rounds=0`
   ⇒ 进一步把因果**收敛到「调用点」**：不是段格式/容量/其它路径，而是这一次 reclaim 调用本身。

**因果闭环**：无 reclaim（BASELINE）恒绿 → 有 reclaim 但未调用（ABLATE）恒绿 →
调用且快照位置/复核正确（FIX）恒绿 → 只破坏快照位置（V2NEG）或只破坏复核（FIXNEG）恒红。

## 五、§7 原式的"不得破坏"逐条核对

| 方案要求 | 现状 | 证据 |
|---|---|---|
| 不得把快照移出 `handles_` 临界区 | 未破坏：`memcpy(&orphan_snap,…)` 行 569，在行 552–571 临界区内 | `lock_order_check.txt` §A |
| 不得删除 `memcmp` 复核 | 未破坏：行 487，`info->lock_` 行 486–493 区间内 | `lock_order_check.txt` §B |
| 不得把 `/proc` 扫描放进自旋锁 | 未破坏：探活行 482，两把锁之外 | `lock_order_check.txt` §D |
| 不得改变 `published()` 保护语义 | 未触碰：变体差异局部性检查 0 处越界；`published()` 读写点不在允许区间内 | `lock_order_check.txt` §E |
| 不得改变段名 | 未触碰：`chunk_segment_name` / `CHUNK_INFO__` / `__C<cap>` 各锚点计数不变 | 同上 |
| 不得改变容量布局 | 未触碰：`large_msg_cache` 锚点不变；档 9216 与池 40 块在 18 次读数中稳定复现 | 同上 + §四 |

## 六、库副本归档与第三方离线重放

`libs/` 目录归档全部六个库副本（含 `FIX` 与产品库本体的对照名 `libipc.so.1.3.0.FIX_PRODUCT`，
sha256 `813fab5b…` 与产品库一致）；`arms/` 目录给出可直接 `LD_LIBRARY_PATH` 的六套
三件套（`libipc.so.1.3.0` + `libipc.so.3` + `libipc.so` 符号链接）。

一键重放（本次已在 200 轮下实跑通过，输出见 `replay.log`）：

```bash
bash artifacts/perf/20261001-w12-T05/scripts/replay.sh 200    # 或 ROUNDS=1200
```

重放脚本会打印工装/探针 sha256 期望值、六个臂的归档库指纹，然后依次跑
A. 自建探针 ×6 臂、B. 仓内常驻用例 ×5 臂，最后再确认两个二进制全程未被替换。

## 七、§13 九项回报

1. **任务编号与负责人**：`t4 / T05`，负责人 = `shared-layer`（成员四：共享层负责人，W09 修复保护）；
   attempt_id = `37e6b190-b12b-4128-b770-917b299297aa`。
2. **实现/复核角色**：实现角色 —— **保护性验证**：产品代码 `src/libipc/ipc.cpp` **零改动**（只读核对 + 归档），
   新增全部为实验臂源码、库副本、探针与驱动（均在 in-scope 的 `build/T05/` 与 `artifacts/perf/20261001-w12-T05/`）。
   本次**不对自己的实现作复核判定**（W09 独立复核属成员五 / §8）。
3. **基线 HEAD**：`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`（dev）。
   产品源 `src/libipc/ipc.cpp` sha256 `9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4`
   —— 工作区与 `git show HEAD:src/libipc/ipc.cpp` **一致**（`git diff --stat` 为空）。
4. **实际修改文件**：**未修改任何产品源**（`src/dzIPC/`、`include/dzIPC/`、W05 相关的
   `shm_control_scheduler.*`/`shm_pub_sub_ipc.*`、`test/CMakeLists.txt`、W11/W12 文档**均未触碰**，
   见 `git status --short` 与 §八的范围清单）。
5. **新增证据目录**：`build/T05/37e6b190-b12b-4128-b770-917b299297aa/`、
   `artifacts/perf/20261001-w12-T05/`（创建前均不存在；未覆盖任何既有 run / 报告 / 快照）。
6. **产品库指纹**：`build/lib/libipc.so.1.3.0`（= `libipc.so.3` = `libipc.so`）
   sha256 `813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a`，1291248 B；
   与 T01 登记值**完全一致**。
7. **工装指纹**：`build/bin/test_chunk_capacity_backpressure`
   sha256 `08b803b169852c7e5c7bdcdf91060b3c8dd1199cd63578167ecd19d40818836f`；
   自建探针 `artifacts/perf/20261001-w12-T05/probe/t05_alias_probe`
   sha256 `764366d22ce2954035bd6e436781e78b462577a6f8086294c9e4fcdbb44256ad`
   （源码 sha256 `7b8123f4…e695`）。
8. **执行命令**：见 §九 commandsRun（含 `rg` 不可用的如实登记与等价替代、六库构建、
   18 次矩阵、5+5 次常驻用例换库、`ctest`）。
9. **关键结果**：见 §三/§四/§五；一句话 —— **FIX 3/3 `bad_rounds=0`（1200/1200 轮全执行、
   复位确实在跑）；V2NEG 3/3、FIXNEG 3/3 `bad_rounds>0`（真别名命中）；BASELINE/ABLATE 恒绿
   ⇒ 缺口由 reclaim 调用点 + 快照/复核临界区直接决定。**

## 八、失败、未覆盖项与残留缺口（如实登记）

1. **`rg` 不在 PATH 上**（`command -v rg` 为空 ⇒ 裸调用 exit=127）。本机确实装有 ripgrep 15.2.0
   （codex 打包的 `…/codex-path/rg`），本 run 用 `scripts/rg-forwarder.sh` 把它放回 PATH 后，
   §7.3.1 的命令**已按方案原文逐字执行**（exit=0），命中与 `grep -nE` 等价替代**逐行一致**；
   三路过程全部留痕在 `mech_rg.txt`。
2. **二进制不可复现构建**：同一 link.txt 重链 `libipc.so` 与产品库不逐字节相同（§3.1）。
   已用「FIX 臂直接取产品库本体」规避该因素，另加 FIX_RB 交叉核对臂；
   **不声称可复现构建、不声称两个 FIX 库逐字节相同**。
3. **ABA 窄窗口仍未封死**（W09 自认、T73 注释行 456–460 登记）：复核判据是"字节镜像相等"，
   若某线程在快照与复核之间完成一次"借出并归还"，字节可能回到相同值而复核通过。
   本次 18 次读数 + 200 轮重放**均未**命中该形态（`FIX` 全绿），但**这只是"未命中"，不是"已封死"**。
   彻底修法（引入"本进程本档在飞借样数"等可直接判定的量）**不在本任务 inScope**。
4. **`--require-lib-sha256` 不适用于本工装**：该参数属 W10/W11 的 perf 驱动器
   （`test/perf/w10/w10_matrix.cpp` 等），**本测试二进制不支持**（实测传 `--require-lib-sha256 deadbeef`
   被 gtest 忽略、rc=0，二进制内也不含该串）。
   故本任务的"库↔读数绑定"改用**更强**的两条机制：① 探针运行时读 `/proc/self/maps`
   自报加载库路径 + SHA-256；② 每臂 run 目录的 `fingerprint.txt`。
   ⛔ 因此**不声称**"已同库独立复跑并获 `--require-lib-sha256` 接受"；本次也**没有**做"
   另一次独立复跑"，而是同一批库的多次重复读数（3 次/臂）。
5. **`/proc` 探活的可判性依赖环境**：`opendir` 失败 / `maps` 读不出时判据退化为"放弃复位"
   （保守），本次未构造该退化场景（超出 §7 范围）。
6. **进程名/邻居污染**：判据用专属前缀隔离（方案要求），但 `/dev/shm` 中**其它进程的探针段**
   （本次运行末尾观察到 234 个段）与本次判据无关；⛔ 我**未**清理他人段文件。
7. **采集窗口内 `/dev/shm` 曾短暂变只读（环境事件，如实登记）**：21:08 观察到
   `tmpfs on /dev/shm type tmpfs (ro,...)`，此前后的读数（20:42–21:06 的 18 次矩阵、
   `ctest`）均在该事件**之前**完成；事件后 `/dev/shm` 恢复可写，
   已复跑 V2NEG 300 轮确认判据仍按预期变红（`bad_rounds=1`，`T05_ALIAS_HIT round=24`）。
   正式读数（`arms-run/*/fingerprint.txt`）**不受**该事件影响（时间戳可核对）。

## 九、命令与验证清单

见任务回报中的 `commandsRun`。补充说明：
- §7.3.1 的 `rg` 命令：裸调用在本机 exit=127（PATH 上无 rg），本 run 通过
  `scripts/rg-forwarder.sh`（exec 到 codex 打包的 ripgrep 15.2.0，路径见 `mech_rg.txt`）
  把 `rg` 放回 PATH 后**按方案原文逐字执行**，exit=0；四路留痕见 `mech_rg.txt`。
- 18 次矩阵由 `scripts/run_matrix.sh` 顺序串行执行（正式性能采集串行纪律）；
- 每次读数都写入该臂 run 目录的 `fingerprint.txt`（读数与库绑定的唯一凭据）；
- `ctest --test-dir build -R test_chunk_capacity_backpressure --output-on-failure` ⇒
  `1/1 Passed 6.47 s`（产品库环境下，未换库）。

## 十、是否影响其他成员 / 下一步依赖

- **不影响**：产品源零改动、无构建缓存变更（未执行 `cmake`/`make` 全量构建）、
  未占用默认共享池前缀、未删除 `/dev/shm` 中他人段文件。
- **供下游使用**：`artifacts/perf/20261001-w12-T05/` 可直接作为 **成员五（§8 W09 独立复核）**
  的输入物：五库副本 + 双臂判据 + 每臂 `fingerprint.txt` + 一键重放脚本。
- **下一步依赖**：无阻塞项；W09 的独立复核（§8）应由**非本实现者**执行。
