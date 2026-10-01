# T06 · W09 独立复核：四类场景同二进制换库验证（方案 §8 / 成员五：验证与性能负责人）

- run_id：`20261001-w12-T06`
- attempt_id：`13e0fbea-b9d3-419a-92b9-92381b188d6a`
- 归属任务：`t7 / T06（方案 §8 成员五：验证与性能负责人，W09 独立复核）`
- 被复核任务：`t4 / T05`（成员四：共享层负责人，W09 修复保护）
- 采集窗口：2026-10-01 21:16 → 21:57 +0800
- 基线 HEAD：`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`（分支 `dev`）；产品源 `src/libipc/ipc.cpp`
  sha256 `9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4`
  （与 `git show HEAD:` 一致；本 run **未触碰**产品源/工装/文档）。
- **复核 verdict：`pass`**（§8 四类场景逐项成立；被测路径确实执行；库绑定只引用本 run 目录 `fingerprint.txt`）。

---

## 0. 一句话结论

以**同一个测试二进制**（`build/bin/test_chunk_capacity_backpressure`，
sha256 `08b803b1…8836f`）**只替换 `libipc.so.3`** 跑完六库矩阵 + 自建四场景布景探针：

| 场景 | FIX（产品库本体 `813fab5b…`） | 有牙证明（非法实验臂/负控） |
|---|---|---|
| 全新段 | **不触发 orphan reset**（10/10 轮 `reset=0`，逐轮 `pre_absent=1` 已核实段真的不存在） | FORCE 臂（去掉素净门槛）**10/10 轮都触发** ⇒ 判据有牙 |
| 段有活持有者 | **不复位**（10/10 轮 `subject_reset=0`）+ 持有者 64B **UNCHANGED**、chunk id 不同 | FORCE 臂（段级探活恒判孤儿）**10/10 轮都复位，持有者 10/10 轮被改写 MUTATED** ⇒ 判据有牙 |
| 段内持有者已死 | **允许复位**（10/10 轮借满 40 块 + 10/10 轮复位日志） | BASELINE/ABLATE（无 reclaim）**借满 0/10、复位 0/10** ⇒ 判据有牙 |
| reclaim 期间另一线程 acquire | **不复位、不别名**（1200 轮 `bad_rounds=0`，`same_id_pairs=0`，`same_data_ptr=0`；复位确实在跑 138–183 次/臂） | V2NEG、FIXNEG **各 3/3 复现 `bad_rounds=1` 真别名命中**（`_ALIAS_HIT`，非崩溃、非超时） |

---

## 一、§8.1 同一个测试二进制，只替换 `libipc.so.3`

- 工装二进制：`build/bin/test_chunk_capacity_backpressure`
  sha256 `08b803b169852c7e5c7bdcdf91060b3c8dd1199cd63578167ecd19d40818836f`，522736 B，
  mtime 2026-10-01 14:33:25（早于本窗口 21:16，全程未被替换）。
- 二进制 `NEEDED libipc.so.3`、`RUNPATH=/home/zwc/cpp_ipc_dds/build/lib` ⇒
  **`LD_LIBRARY_PATH` 优先于 RUNPATH**，换库只需改环境变量（`diag/freeze_and_binding.txt` 有 `ldd`
  与 `LD_DEBUG=libs` 双路留痕）。
- 六臂库（每臂 `fingerprint.txt` 记 `lib_sha256`，且每臂补 `lib_binding.txt` 记录 `ldd` 实际命中路径）：

| 臂 | 库 sha256 | 大小 | 语义 |
|---|---|---|---|
| `FIX` | `813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a` | 1291248 | **冻结产品库本体**（= `build/lib/libipc.so.1.3.0` 逐字节相同） |
| `FIX_RB` | `c928deaa4f480ee361e5974bf824c85c87658f2a6e9113bc0d3415568264b32d` | 1291248 | T05 逐字重建的 FIX（独立构建路径交叉核对） |
| `V2NEG` | `3380016ba38084d8071d00133096298c733e1ebc8de16dde969a58456353d791` | 1291248 | 负控：快照移出 `handles_` 临界区 |
| `FIXNEG` | `1e7d5d7f12c20919e3994b9bdcd818375ba53afb11d99e8f74cb1a21c69dfc91` | 1291248 | 负控：删除 `info->lock_` 下 `memcmp` 复核 |
| `BASELINE` | `4fc0e95dc316ac01972edd7fe6efde4eb13de8f4793c08b05ea95eae29dd3a71` | 1285504 | 无 reclaim 的历史库 |
| `ABLATE` | `52ccbe9e7e054aada32d5ec28f0ea97085ea7d6e1403e5dc756dc88878dde732` | 1285960 | 保留 reclaim 代码但删除调用点 |
| `FORCE`（自建，非产品臂） | `ea8101d7f50e3dbdc489b6d1ef009ec79b4e5d61a208238c1cbaa8c2299e7c27` | 1291248 | **量具有效性对照臂**：去掉"素净"门槛 + 段级探活恒判孤儿 |

- ⚠️ **库副本命名歧义（沿用队长提示，实测确认）**：`20261001-w12-T05/libs/libipc.so.1.3.0.FIX`
  实测 `c928deaa…`（实为 **FIX_RB**），**不是**产品库本体；`libs/libipc.so.1.3.0.FIX_PRODUCT`
  才是 `813fab5b…`。本 run **未使用** `libs/.FIX`，产品臂取自
  `arms/FIX/libipc.so.1.3.0`（实测 `813fab5b…`）与 `build/lib/libipc.so.1.3.0`（同一 SHA）。
- `FORCE` 臂的**改法与差异**落盘在 `build/T06/13e0fbea-…/src/ipc.cpp.FORCE` 与 `src/FORCE.diff`
  （31 行 diff，两处改动：① 去掉 `if (!pristine)` 门槛；② `return live ? ... : orphaned;`
  改为恒 `orphaned`）。⛔ 该臂**不是**产品代码，也未进入任何交付路径，只用于证明判据有牙。

---

## 二、§8.3 官方 ctest 原始输出落盘

```bash
ctest --test-dir build -R test_chunk_capacity_backpressure --output-on-failure
```

- 落盘：`ctest/product/ctest.log`（另有 `ctest/ctest.log` 副本）与 `ctest/product/ctest.meta.txt`。
- 原始输出（**未加 `LD_LIBRARY_PATH`** ⇒ 走二进制 RUNPATH = `build/lib` 的产品库本体现值）：

```
Test project /home/zwc/cpp_ipc_dds/build
    Start 29: test_chunk_capacity_backpressure
1/1 Test #29: test_chunk_capacity_backpressure ...   Passed    6.53 sec
100% tests passed, 0 tests failed out of 1
Total Test time (real) =   6.53 sec
ctest_exit=0
end=2026-10-01T21:29:44+0800
```

- ⛔ **如实登记**：`ctest` 注册的命令不带 `LD_LIBRARY_PATH`，因此该条**只在产品库环境下跑了一次**
  （不是换库读数）。换库读数来自 `official-matrix/`（同一二进制 + 六个库）与 `scenarios/`
  （自建四场景布景探针）。本条与后两者是**不同证据**，不互相替代。

---

## 三、§8.2 读数记录（`same_id_pairs` / `same_data_ptr` / `bad_rounds` / orphan reset 次数）

### 3.1 官方常驻用例 ×6 库（同一二进制，1200 轮，每臂独立 run 目录）

命令（每臂）：`LD_LIBRARY_PATH=<arm lib> build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*' --gtest_color=no`
表：`official-matrix/matrix.tsv`（每行对应自身目录的 `fingerprint.txt`）。

| 臂 | 库 sha16 | rc | gtest | bad_rounds | skipped | orphan reset 行数 |
|---|---|---|---|---|---|---|
| V2NEG-1 | `3380016ba38084d8` | 1 | FAILED | **1** | 0 | 20 |
| V2NEG-2 | `3380016ba38084d8` | 1 | FAILED | **1** | 0 | 66 |
| FIXNEG-1 | `1e7d5d7f12c20919` | 1 | FAILED | **1** | 0 | 1 |
| BASELINE-1 | `4fc0e95dc316ac01` | 0 | PASSED | 0 | 0 | **0** |
| ABLATE-1 | `52ccbe9e7e054aad` | 0 | PASSED | 0 | 0 | **0** |
| FIX_RB-1 | `c928deaa4f480ee3` | 0 | PASSED | 0 | 0 | 266 |
| **FIX-1** | `813fab5be886ef6e` | **0** | **PASSED** | **0** | 0 | **182** |
| **FIX-2** | `813fab5be886ef6e` | **0** | **PASSED** | **0** | 0 | **138** |
| **FIX-3** | `813fab5be886ef6e` | **0** | **PASSED** | **0** | 0 | **183** |
| BASELINE-polluted（故意污染对照） | `4fc0e95dc316ac01` | 1 | FAILED | 1 | 0 | 0 |

- `bad_rounds` 取自用例自身打印的 `[W09-C9] … bad_rounds=<n> skipped=<n>`；`orphan segment reset`
  次数 = run.log 中该日志行数。**FIX 三臂复位日志 138/182/183 行 ⇒ 被测路径确实在跑**（非早退假绿）。
- **失败形态分类**：红臂的 `bad_rounds` 全部是**真别名命中**（gtest 报 `bad_rounds=1`），
  `crash_rounds=0`、无信号退出、无超时 ⇒ 不是写崩/误计。
- ⚠️ **BASELINE-polluted 与 BASELINE-1 是同库（`4fc0e95d…`）而结论相反**，这是本 run 查出的
  **量具卫生事实**（见 §六.2）：C9 用例的 `clear_storage(prefix, "w09c9_seed")` 只清**控制面**段
  （`AC/CC/WT/RD_CONN__`，`ipc.cpp:210-222`），**不含**池段
  `/dev/shm/w09c9alias__IPC_SHM__CHUNK_INFO__9216__C40`（段名在 `ipc.cpp:373-378`）。
  ⇒ 上一条负控臂留下的重复 id 会被无 reclaim 的库**继承**（它永远修不回来）。
  故绿臂必须在前一条负控臂之后**清一次本用例专属前缀的池段**（μm 级卫生操作，
  ⛔ 不动任何他人段文件、⛔ 不动用例本体：1200 轮内部**一次都不清**）。

### 3.2 自建四场景布景探针（同一探针二进制 `probe/t06_scenario_probe`，sha256 `f70017d2…`）

探针把四类场景**各自隔离布景**（官方用例把它们揉在同一个 1200 轮用例里，只能给总判定）。
段状态直读**不调用任何产品 API**：`chunk_info_t` 首成员 `id_pool<>` 的 `next_[40]` 占文件头 40 字节、
`cursor_` 紧随其后（`seg_probe` 只 `fopen` 段文件）。
表：`scenarios/summary.tsv`，驱动：`logs/matrix.driver.log`。

| 臂 | 库 sha16 | fresh ok/轮 | fresh reset 轮 | live 主体/复位轮 | live 持有者改写 | dead 借满 | dead 复位轮 | race 执行/1200 | race bad | same_id_pairs | same_data_ptr | race 复位轮 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| FORCE（有牙对照） | `ea8101d7` | 10/10 | **10** | 10 / **10** | **10 轮 MUTATED** | 10/10 | 10 | — | — | — | — | — |
| **FIX** | `813fab5b` | 10/10 | **0** | 10 / **0** | **0** | 10/10 | **10** | 1200/1200 | **0** | **0** | **0** | 163 |
| FIX_RB | `c928deaa` | 10/10 | 0 | 10 / 0 | 0 | 10/10 | 10 | 1200/1200 | 0 | 0 | 0 | 160 |
| V2NEG | `3380016b` | 10/10 | 0 | 10 / 0 | 0 | 10/10 | 10 | 169/300 即停 | **1** | **2** | **2** | 45 |
| FIXNEG | `1e7d5d7f` | 10/10 | 0 | 10 / 0 | 0 | 10/10 | 10 | **1**/300 即停 | **1** | **2** | **2** | 1 |
| BASELINE | `4fc0e95d` | 10/10 | 0 | 10 / 0 | 0 | **0/10**（只借到 37） | **0** | 300/300 | 0 | 0 | 0 | 0 |
| ABLATE | `52ccbe9e` | 10/10 | 0 | 10 / 0 | 0 | **0/10**（只借到 37） | **0** | 300/300 | 0 | 0 | 0 | 0 |

- 探针自指纹：每臂每场景头部打印 `T06_PROBE … loaded_lib=<真实路径> loaded_lib_sha256=<实测 SHA>`
  ⇒ 读数与库的绑定**不依赖外部脚本转述**（`scenarios/*/fresh.log` 第一行）。
- `T06_MEAS_ERR`（量具自检）在**所有臂、所有场景均为 0 次** ⇒ 没有"子进程没跑出来却被判绿"的情形。
- ⚠️ **参与度差异（必须与"绿"一起读）**：五种库在 race 场景里的"每轮实际借到几块"并不相同 ——
  `FIX` 1200/1200 轮 `held=8`；`V2NEG` 169 轮全 `held=8`；`FIXNEG` 首轮即命中；
  而 `BASELINE`/`ABLATE` 300 轮里只有前 10 轮 `held=8`，其余 **287 轮 `held=0`**
  （无 reclaim ⇒ 空闲链在跨轮复用中退化到借不出块）。⇒ 这两臂的 `bad_rounds=0` 部分是
  **"没有牙"**（判据要求每轮实际持有两块以上才能比较 id），本报告**不把它们当作强负控**，
  只当作"无 reclaim"的结构参照；有牙性由 FORCE（场景有牙）与 V2NEG/FIXNEG（并发有牙）承担。
- 扩展剂量：`FIX` 再加 **3000 轮 × 16 话题**并发首借（`scenarios/extended/`）⇒ 每轮 `held=16`、
  `bad_rounds=0`、`same_id_pairs=0`、`same_data_ptr=0`、`executed=3000/3000`、每轮 `held=16`、
  复位 215 轮；`topics=1/2/4/8 × 200 轮` 剂量响应（`scenarios/dose/`）亦全绿。

---

## 四、§8.4 四类场景逐项判定

### ① 全新段 → 不触发 orphan reset：**成立**
- `fresh` 场景每轮先 `unlink` 池段（`pre_absent=1` 逐轮记账），再首次 attach + 借 1 块。
  FIX：**10/10 轮 `reset_lines=0`**；每轮可外部直读 `chain_after_borrow=39 cursor=1`、
  归还后 `chain_after_discard=40`（段真的建起来了、块真的借到了）。
- ⚠️ **口径说明**：官方 1200 轮用例**不隔离**"全新段"这一场景（它每轮都先 fork 种子进程造
  "已用过"的段），其 `orphan reset` 计数（FIX 138–183 行）**不能**当成"全新段不触发"的证据；
  官方侧的 `BASELINE/ABLATE` 计数为 0 也只说明"这段代码不存在"，不是早退正确。
  ⇒ 本条判定的**唯一直接证据**是本 run 的 `fresh` 场景（+ FORCE 有牙对照）。
- **有牙证明**：FORCE 臂（去掉素净门槛）**10/10 轮都触发 reset**（`reset_lines_total=8`，
  首轮因日志基线不含）⇒ 该场景的"0"不是"路径根本没执行"的假绿。
- ⚠️ **如实登记（本 run 实测）**：`fresh` 场景**不能**区分"有 reclaim 且早退正确"与
  "无 reclaim"，因为 `BASELINE` 在该场景也是 `reset=0`。有牙性由 FORCE 臂单独给出，
  **不能**用 BASELINE 代替。

### ② 段有活持有者 → 不复位：**成立**
- `live` 场景：另一个**始终存活**的进程借 1 块并写 64 B `0xA5`，本进程随后首次 attach 该段。
  FIX：**10/10 轮 `subject_reset_lines=0`**；持有者 10/10 轮回报 `verdict=UNCHANGED`；
  主体拿到的 chunk id 与持有者 id **从不相同**（`same_id_rounds=0`）。
- **有牙证明**：FORCE 臂 **10/10 轮都复位**，且持有者 **10/10 轮 `MUTATED`**、`same_id_rounds=10`
  ⇒ 这个场景对错误复位是**确定性**敏感的，不是概率性的。
- ⚠️ **如实登记**：官方 1200 轮用例**不覆盖**本条（它每轮 fork 的种子进程在 attach 之前就已 `_exit`，
  段内不存在活持有者）⇒ §8 第二项的**唯一证据来自本 run 的 `live` 场景**。

### ③ 段内持有者已死 → 允许复位：**成立**
- `dead` 场景：种子进程借 3 块后 `_exit`（不归还）⇒ 外部直读 `pre_chain=37`（非素净），
  被测进程首次 attach。FIX：**10/10 轮借满 `kCap=40` 块**（复位前最多只能借 37）
  且 **10/10 轮打出复位日志**。
- **有牙证明**：BASELINE / ABLATE（无 reclaim）**借满 0/10**（只借到 37）、复位 0/10
  ⇒ "借满 40"这个行为判据确实把"复位发生"与"没发生"分开了。

### ④ reclaim 期间另一线程 acquire → 不复位、不能别名：**成立**
- 布景：每轮先造"已用过"的段，再在本进程内让 **8 个话题的 8 个线程**在栅栏后**同时**首发
  `loan(8000)`（档 9216），与 `get_info()` 的首次 attach 判定竞争。
- FIX：**1200/1200 轮执行**，`bad_rounds=0`、`same_id_pairs=0`、`same_data_ptr=0`，
  且 worker 侧复位行数 163（**路径确实在跑**）；官方用例侧同库 1200 轮同样 `bad_rounds=0`
  且复位 138/182/183 行。
- **有牙证明**：V2NEG（快照移出临界区）`bad_rounds=1 same_id_pairs=2 same_data_ptr=2`；
  FIXNEG（删 `memcmp` 复核）**首轮即命中** `bad_rounds=1 same_id_pairs=2 same_data_ptr=2`；
  两条负控各自独立有牙（分别对应 §7.2 的两条保护）。探针与官方用例**两条独立判据路径**同判。
- ⚠️ **如实登记**：V2NEG 在自建探针里第 168 轮命中（概率性），FIXNEG 第 0 轮命中（确定性）；
  T05 记录 V2NEG 3/3、FIXNEG 3/3 复现，本 run 各复现 2 次与 1 次，**同向**。

---

## 五、§8.5 库绑定只引用 run 目录 `fingerprint.txt`

- 本报告所有库 SHA 均来自 `artifacts/perf/20261001-w12-T06/` 内**本 run 自己产出**的
  `fingerprint.txt`（`official-matrix/*/fingerprint.txt`、`scenarios/*/fingerprint.txt`）。
- ⛔ **未使用** W11 manifest 的生成时 `binary_sha256` 快照；⛔ 也未把 T05 报告正文的数字当绑定依据
  （T05 的库副本仅作**输入料**，其 SHA 在本 run 内**重新实测**后才引用）。
- 双重绑定证据：① 每臂 `fingerprint.txt` 记 `lib_sha256` + `lib_size` + `cmdline` + `exe_rc` +
    `gtest_verdict` + `reading` + `orphan_segment_reset_count`；② 每臂 `lib_binding.txt` 记
    `ldd` 实际命中路径（`LD_LIBRARY_PATH` 与默认 RUNPATH 两路）；③ 探针自建时运行时读
    `/proc/self/maps` **自报**实际加载库的真实路径 + SHA-256。
- ⚠️ **`--require-lib-sha256` 的如实登记**：本 run 实测该参数**不被本工装支持**
  （`diag/require_lib_sha256_check.txt`：传 `deadbeefdeadbeef` 后 `exit=0`、
  二进制内**不含**该串（`strings` 计数 0）、测试源内计数 0；该参数只出现在
  `test/perf/w10/`、`test/perf/w11/` 的驱动器里）⇒ **不声称该参数已获接受**，
  也**不声称"已同库独立复跑"**；同库的多次读数（FIX ×3、V2NEG ×2）均如实标为**同一批库的重复读数**。

---

## 六、失败、未覆盖项与量具缺陷（如实登记）

1. **T05 归档目录的副本命名歧义（沿用队长提示，实测确认）**：`libs/libipc.so.1.3.0.FIX`
   实测 `c928deaa…` = FIX_RB，不是产品库本体；产品库本体在 `libs/….FIX_PRODUCT` 与
   `arms/FIX/libipc.so.1.3.0`。本 run 未误用，但**该命名歧义本身是 T05 的归档缺陷（low）**。
2. **C9 用例的跨臂段污染（本 run 查出的量具卫生缺陷，非产品缺陷）**：
   `clear_storage` 不清 chunk 池段 ⇒ 负控臂留下的重复 id（实测 `PROBE_CHAIN_STATS
   cycle_closed=1 reachable_ids=6`）会被**无 reclaim** 的库继承，使 BASELINE/ABLATE
   在紧接负控臂之后误报 `bad_rounds=1`；干净态下同库 1200 轮全绿。本 run 的处置：
   每臂前只删本用例专属前缀段，并**逐臂记录清空前残留段状态**（`official-matrix/matrix.driver.log`
   的 `[pre]` 行），另设 `BASELINE-polluted` 故意污染对照（`bad_rounds=1`）把该事实钉死。
   ⛔ 用例本体（产品/工装/文档）一字未改。
3. **`--require-lib-sha256` 不适用**（见 §五）⇒ 库绑定改用上述三重证据；不声称该参数被接受。
4. **`live` 场景跨进程只能比 chunk id，不能比 data 指针**：不同进程地址空间里 `data` 指针不可比
   （自建探针只比 id + 持有者 64 B 是否被改写）；官方用例的 `same_data_ptr` 是**同进程内**两个
   话题借样之间的比较，两者口径不同，本报告分别标注。
5. **ABA 窄窗口未封死**（W09 自认，`ipc.cpp:456-460` 登记）：复核判据是"字节镜像相等"，
   若某线程在"快照 → 复核"之间完成一次**借出并归还**（A→B→A），字节可能回到相同值而复核通过。
   本 run（官方 1200 轮 ×3、探针 1200 轮、扩展 3000 轮 ×16 话题、剂量 200×4）**均未命中**该形态，
   但**这只是"未命中"，不是"已封死"**。彻底修法（引入可直接判定的在飞计数）不在本任务 inScope。
6. **未覆盖**：`/proc` 探活不可判的退化路径（`opendir` 失败 / maps 权限不足 ⇒ 一律放弃复位）
   **未构造**（本机 3 个 PID 的 maps 全部可读，`unshare --mount-proc` 在本环境被拒
   `不允许的操作`）⇒ 该分支只用**代码核对**（`ipc.cpp:394-423`，`complete=false ⇒ unknown ⇒ 不复位`）
   支持，**没有**运行时读数。
7. **`/dev/shm` 只读事件的区分**：T05 报告登记 21:08 曾短暂只读。本 run 窗口 21:16 起
   `/dev/shm` 全程可写（21:12:54 写入探针通过、每臂段文件正常创建）；本 run **未观察到**
   共享段相关异常，**未把任何环境异常计入 `bad_rounds`**。
8. **官方用例的参与度不对称（读数解释上的坑）**：见 §3.2 的"参与度差异"——`BASELINE`/`ABLATE`
   在跨轮复用下 300 轮中 287 轮根本借不到块（`held=0`）⇒ 它们的 `bad_rounds=0`
   不能读成"论证了修复不必要"，只能读成"这段代码里没有 reclaim 这段结构"。
   另：官方用例同一库的读数**受段历史影响**（§六.2），故本 run 对每一条臂都记录了
   `[pre]` 段状态与 `segment_policy`。
9. **本 run 的量具自身踩过的坑（已修，留痕）**：
   ① 探针 stdout 曾与 `tee` 共用同一文件 ⇒ "本轮新增复位行"计数基线被推动（已改为驱动转录到
      `.driver.log`，并加 `T06_MEAS_ERR` 自检）；
   ② race 场景曾缺"每轮必打一行"⇒"无命中"与"进程没跑"不可区分（已加 `T06_RACE_ROUND` 行）；
   ③ 驱动脚本 `mode` 参数曾误取 `${2}`（目录名）⇒ 除首臂外全走污染分支（已修，见 §六.2 前两条记录）；
   ④ `run_official_arm.sh` 曾末条 `grep` 吞掉 gtest 退出码（已 `exit $RC` 透传）。
   以上均属**本 run 量具**，不涉及产品/工装/文档。

---

## 七、证据索引

| 路径 | 内容 |
|---|---|
| `artifacts/perf/20261001-w12-T06/fingerprint.txt` | 本 run 总指纹 |
| `artifacts/perf/20261001-w12-T06/ctest/product/ctest.log` | §8.3 要求的 ctest 原始输出 |
| `artifacts/perf/20261001-w12-T06/official-matrix/<arm>-<k>/` | 官方用例换库单臂：`run.log` + `fingerprint.txt` + `lib_binding.txt` |
| `artifacts/perf/20261001-w12-T06/official-matrix/matrix.tsv` | §3.1 表（含 `same_id_pairs` 所在的 `reading` 行） |
| `artifacts/perf/20261001-w12-T06/scenarios/<arm>/{fresh,live,dead,race}.log` | 四场景布景读数 |
| `artifacts/perf/20261001-w12-T06/scenarios/summary.tsv` | §3.2 表 |
| `artifacts/perf/20261001-w12-T06/scenarios/extended/` | FIX 3000 轮 × 16 话题扩展 |
| `artifacts/perf/20261001-w12-T06/scenarios/dose/` | topics=1/2/4/8 × 200 轮剂量响应 |
| `artifacts/perf/20261001-w12-T06/probe/` | 自建量具源码与二进制（探针 + 段直读器） |
| `artifacts/perf/20261001-w12-T06/scripts/` | 可重放驱动（scenarios / official / 汇总） |
| `artifacts/perf/20261001-w12-T06/libs/<arm>/` | 本 run 实际使用的六个库副本 + FORCE 臂引用 |
| `artifacts/perf/20261001-w12-T06/diag/` | 冻结与绑定、`--require-lib-sha256` 实测、§5.4 锚点 |
| `artifacts/perf/20261001-w12-T06/diag/arm_pollution.md` | 跨臂池段污染的量具缺陷登记 + 最小复现三连 |
| `artifacts/perf/20261001-w12-T06/official-arm/` | 卫生修正前的早期读数（已被 `official-matrix/` 取代，含 POLDEMO 三连） |
| `build/T06/13e0fbea-b9d3-419a-92b9-92381b188d6a/` | FORCE 变体源码/目标文件/库 + `src/FORCE.diff` |

## 八、一键重放

```bash
# ① 四场景布景矩阵（串行，含 FORCE 有牙对照）
T06_ATTEMPT_DIR=build/T06/13e0fbea-b9d3-419a-92b9-92381b188d6a \
  bash artifacts/perf/20261001-w12-T06/scripts/run_all.sh
# ② 官方工装换库矩阵（含跨臂段卫生 + 故意污染对照）
bash artifacts/perf/20261001-w12-T06/scripts/run_official_matrix.sh \
  artifacts/perf/20261001-w12-T06/official-matrix
# ③ 汇总
python3 artifacts/perf/20261001-w12-T06/scripts/summarize_scenarios.py \
  artifacts/perf/20261001-w12-T06/scenarios
bash artifacts/perf/20261001-w12-T06/scripts/summarize_official.sh \
  artifacts/perf/20261001-w12-T06/official-matrix
```

## 九、§13 九项回报

1. **任务编号与负责人**：`t7 / T06`，负责人 = `verify-perf`（成员五：验证与性能负责人，W09 独立复核）；
   attempt_id = `13e0fbea-b9d3-419a-92b9-92381b188d6a`；被复核任务 = `t4 / T05`（`shared-layer`）。
2. **实现/复核角色**：**独立复核角色**（非 T05 实现者）。本 run **只读**产品源码，**未修改任何产品/工装/文档**
   （见 §十）；新增全部落在 in-scope 的 `build/T06/13e0fbea-…/` 与 `artifacts/perf/20261001-w12-T06/`。
   复核 verdict = **pass**。
3. **基线 HEAD**：`f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`（dev）；
   产品源 `src/libipc/ipc.cpp` sha256 `9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4`。
4. **实际修改文件**：无产品/工装/文档文件被改（`src/`、`include/`、`test/CMakeLists.txt`、
   W05/W11/W12 文档、T05 既有证据目录均未触碰）。新增仅 `build/T06/13e0fbea-…/` 与
   `artifacts/perf/20261001-w12-T06/`（创建前均不存在）。
5. **新增证据目录**：`build/T06/13e0fbea-b9d3-419a-92b9-92381b188d6a/`、
   `artifacts/perf/20261001-w12-T06/`（364 个文件）。
6. **产品库指纹**：`build/lib/libipc.so.1.3.0`（= `libipc.so.3` = `libipc.so`）
   sha256 `813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a`，1291248 B；
   与 T01/T05 登记值一致；FIX 臂即此库本体。
7. **工装指纹**：`build/bin/test_chunk_capacity_backpressure`
   sha256 `08b803b169852c7e5c7bdcdf91060b3c8dd1199cd63578167ecd19d40818836f`；
   自建量具 `probe/t06_scenario_probe` sha256 `f70017d2e8c5519fee28cac14ecc49eb3cda17a44c71b553c449423b75e66b11`
   （源码 `e78d5fb1e57f0bdcb0d0bf10d1f80b735d26c78ec4fd1ad4f057899bacba6f31`）、
   段直读器 `probe/seg_probe` sha256 `94718a6252e4e1344cea19dd93fe8432eef5a77dd497d2e072f001decd9f11cb`。
8. **执行命令**：见 commandsRun（ctest ×1、官方换库 ×10 臂次、四场景矩阵 ×7 臂（含 FORCE）、
   扩展 3000 轮 ×16 话题、剂量响应 4 档、段直读与绑定核对若干，全部只读产品库/源码）。
9. **关键结果**：四类场景逐项**成立**（§四）：全新段不触发复位（10/10 轮 0 次，FORCE 对照 10/10 触发）；
   活持有者不复位且持有者 64 B 未被改写（10/10 UNCHANGED，FORCE 对照 10/10 MUTATED）；
   死持有者允许复位（借满 40/40，无 reclaim 库只借 37）；reclaim 期间并发 acquire 不复位不别名
   （官方 1200 轮 ×3 与探针 1200 轮皆 `bad_rounds=0 same_id_pairs=0 same_data_ptr=0`，
   复位确实在跑 138–183 次/臂），两条负控各自 3/3（本 run 2/2 与 1/1）复现真别名命中。
   **⇒ §8 独立复核通过（verdict = pass）**，另登记 4 条量具/归档缺陷与 2 项未覆盖（§六）。
