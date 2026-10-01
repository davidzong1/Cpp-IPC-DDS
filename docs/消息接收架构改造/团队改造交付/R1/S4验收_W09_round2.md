# S4 独立验收 · W09（chunk 容量、队列与背压联动）—— **round 2**

> **verdict: pass**（5 条验收项全部独立满足）；另附 **2 条 medium + 3 条 low** 的证据卫生缺陷
> —— 均**不阻断**本次验收结论，但须以 **append-only 更正**就地落账（见 §6）。
> 复核者：**验证与性能负责人**（W09/t10、t22、t73 的实现者 = 共享层负责人 ⇒ 独立性成立）
> 被复核对象：**t73**（对我在 `S4验收_W09.md` 提出的 W09-S4-F1 blocker 的修复）
> 复核范围：**只读** + 自建影子库（全部在 `tmp/t74/**`，⛔ 未改产品代码 / W09 交付物 / 既有 run）
> 证据：`artifacts/perf/20261001-t74-W09-S4/`（`summary.md`、`decisive/`、`controls/libswap.log`）

---

## 0. 与 round 1 的关系

round 1（`R1/S4验收_W09.md`，t65）判 **needs_revision**：容量模型四条成立、判决性实验成立、
回退族语义分工成立，但 **W09-S4-F1（blocker）** —— `reclaim_orphan_segment()` 的**前提①**
（"首次 attach 时本进程必然未持有本档 chunk"）在同进程**多话题并发首次借样**（受支持用法）下不成立，
导致同档 chunk 被重复分配（我当时给出最小反例 4/120）。

t73 的修法：把「素净判定 + 空闲链字节快照」**移进 `handles_` 的 `lock_` 临界区**，
`reclaim_orphan_segment()` 改为接收调用方传入的 `orphan_candidate` / `snap_in`。

**本轮结论：修法真实有效，且我用三种独立负控 + 换库对照独立证实其"有牙"。** 但 t73 的证据记录里有
两处**可核验性**缺陷（§6-F1/F2），须更正。

---

## 1. 要求 1：容量模型 —— 四条**全部成立**（代码 + 运行期，当前 tree）

| 项 | 判据 | 我的读数 |
|---|---|---|
| 每档 **40 块** | `include/libipc/def.h:49` `large_msg_cache = 40`；`src/libipc/utility/id_pool.h:47` `max_count = limited_max_count()`（= `min(large_msg_cache, 255)`） | 自建探针 `tmp/t74/chunk_probe.cpp`：`large_msg_cache=40`、`id_pool<>::max_count=40`、`sizeof(id_pool<>)=42`；用例运行期 `[W09-C1] 档 9216 可用块数=40`、`[W09-C8] 借满=0 → 归还后空闲=40（满值 40）` |
| 归属键 `(prefix, chunk_size)` | `ipc.cpp:373-379` `chunk_segment_name()` = `make_prefix(pref, {"CHUNK_INFO__", size, "__C", large_msg_cache})` | 同一 bash 调用内 `ls /dev/shm`：空前缀 ⇒ `__IPC_SHM__CHUNK_INFO__132096__C40`；命名前缀 `w09_c2_0` ⇒ `w09_c2_0__IPC_SHM__CHUNK_INFO__9216__C40`（**两个物理段**） |
| 默认空前缀 ⇒ 同档全机共享 | `src/dzIPC/shm_pub_sub_ipc.cc:861`（与 `:1455`）用 **name-only** 构造 ⇒ `ipc.cpp:1440-1443` `connect(ph, {nullptr}, name)` | prefix **恒为空串** ⇒ 同进程全话题同档共用一段（这正是 F1 反例的前提，属**产线形态**而非边角） |
| 队列上限 | `src/dzIPC/common/nodelet_config.cc:110+` `ViewQueueCap() = max(1, large_msg_cache/4)`；`shm_pub_sub_ipc.cc:1333-1340` `view_cap < queue_size` 时钉住；`:1345` `adopt_cap = view_cap` | 自建探针 `vq_probe`：`ViewQueueCap()=10`、`IsViewQueuePinEnabled()=1`；`[W09-C3]` TLV 档 10240 与 loan 档 11264 分列（TLV 物化**未**被钉） |
| 段字节公式 | `52 + 40 × chunk_size` | 132096 档实测 **5 283 892 B** = `52 + 40×132096` ✅ |

---

## 2. 要求 2：t22 修复 v2 + t73 修法 + UNSOUND_v1 自我证伪记录

### 2.1 修法的**结构核对**（我逐行读，非引用）

`src/libipc/ipc.cpp:540-576`（`get_info()`）：

```cpp
bool orphan_candidate = false;
ipc::id_pool<> orphan_snap;
{
  std::lock_guard<std::mutex> guard{lock_};          // ← handles_ 的锁
  h = &(handles_[pref]);
  newly_attached = !h->valid();
  if (!make_handle(*h, shm_name, chunk_size)) return nullptr;
  if (newly_attached) {
    auto *probe = static_cast<chunk_info_t *>(h->get());
    if (probe != nullptr) {
      probe->lock_.lock();                            // 锁序：handles_.lock_ → info->lock_
      bool const pristine = probe->pool_.invalid();
      if (!pristine) { std::memcpy(&orphan_snap, &probe->pool_, sizeof(orphan_snap));
                       orphan_candidate = true; }
      probe->lock_.unlock();
    }
  }
}                                                     // ← 快照与 valid() **同一临界区**
if (newly_attached) (void)reclaim_orphan_segment(info, pref, chunk_size, "attach",
                                                orphan_candidate, &orphan_snap);
```

`reclaim_orphan_segment()`（`:469-505`）现为：`if (!orphan_candidate || snap_in == nullptr) return false;`
→ 取 `snap = *snap_in` → ① 锁外 `scan_segment_mappers()` → ② `info->lock_` 内 `memcmp(&snap, &info->pool_)`
→ 不等则**放弃**（安全方向：漏而不夺）→ 相等才 `reset_free_chain()`。

⇒ **前提① 已由"取快照的位置"机械保障**：任何其它线程要拿到本段 `info` 都必须先过 `handles_` 的 `lock_`
并看到 `valid()` ⇒ 其借出**必然发生在快照之后** ⇒ 被 ② 的字节镜像复核挡下。✅

**锁序核对（死锁面）**：静态扫描「持 `info->lock_` 期间再取 `handles_` / `chunk_storage_info()` / `get_info()`」
的逆序区间 = **0 处**；`acquire_storage()` 的池清扫（`reclaim_dead_chunks`）也在取 `info->lock_` **之前**完成
⇒ 无嵌套逆序。✅

### 2.2 我用**自建负控**独立证明"有牙"（三种负控 + 换库对照）

库全部由我自建（由当前 `src/` 逐字改，改动面见下），⛔ 不依赖 t73 的 `tmp/t73` 产物：

| 代号 | 内容 | sha256[0:16] | 与主线的差异 |
|---|---|---|---|
| **FIX** | 当前 tree 构建 | `d0f581d19f74c411` | — |
| **NEG** | 只把「素净判定 + 快照」**移出** `handles_` 的 `lock_`（= 最小负控） | `1e3c5e5fe66ca9e3` | 1 处搬移 |
| **V2NEG** | 还原 **t73 之前**的 v2 写法：判定+快照**在 `lock_` 释放后且在 `/proc` 扫描之后**（= 历史原物） | `221d04877883809f` | 2 处 |
| **ABL** | 删掉 `reclaim_orphan_segment` 调用（= 无 v2 修复的基线行为） | `012cd0a02320437d` | 1 处 |
| t73NEG | **t73 自己保存**的负控 | `0186853e1eadb5ea` | — |

**（a）常驻用例换库对照**（同一二进制 `sha256[0:16]=08b803b169852c7e`、实测 `kRounds=1200`，**只换 `libipc.so.3`**）：

| 库 | 3 次读数 |
|---|---|
| **FIX** `d0f581d1…` | `bad_rounds=0` / `0` / `0` |
| **V2NEG** `221d0487…` | `bad_rounds=1` / `1` / `1` |
| **NEG** `1e3c5e5f…` | `bad_rounds=1` / `1` / `1` |
| **t73NEG** `0186853e…` | `bad_rounds=1` / `1` / `1` |

⇒ **判据与被修饰直接对应、稳定可判**（不是我"写了就绿"）。原始输出：`artifacts/perf/20261001-t74-W09-S4/controls/libswap.log`

**（b）我的反例件**（`R1/S4验收_W09_证据/repro_alias.cpp`，对上述各库重编后运行）：

| 形态 | **FIX** | V2NEG | NEG |
|---|---|---|---|
| 1 轮 × 2 话题 × 60 次（每次清空段） | **0/60** | 0/60 | **6/60** |
| 3 轮 × 8 话题 × 60 次 | **0/60** | 2/60 | 2/60 |
| 40 轮 × 8 话题 × 3（不清场） | **0** | 0 | 0 |

⚠️ 命中是**概率性**的（单次形态波动大，见 V2NEG 的 0/60 与 2/60 并存）⇒ 单次小样本**不能**作为
"修复无效"的证据；但**换库对照（a）在同一二进制下 3/3 vs 3/3** 是稳定判据，二者不矛盾。

**（c）ABA 窄窗口专项攻击（t73 §12.3 自认仍开着）**：我新写 `tmp/t74/aba/aba_probe.cpp`
（一半线程做 `loan → discard_loan` 紧循环制造 A→B→A，另一半**并发首次**借样）：

| 库 | 200 次运行的命中数 | 累计 `same_id` / `same_ptr` |
|---|---|---|
| **FIX** | **0** | 0 / 0 |
| V2NEG | 3 | 8 / 8 |
| NEG | 6 | 6 / 6 |

⇒ 该窄窗口在**本次 200 次攻击下未被打开**（⛔ 不等于"已封死"，仅"本次未命中"）。

### 2.3 UNSOUND_v1 的自我证伪记录 —— **成立**（且已按实物更正）

| 交付声明 | 我的核对 | 判定 |
|---|---|---|
| v1（在池耗尽路径复位）不安全：会把**本进程自己持有**的 id 重新发出 | 逻辑成立：`scan_segment_mappers` 排除自身 ⇒ 判据只否证"他人"，不否证"自己" | ✅ |
| `solo_chunk_capacity.log` 见 2 例 FAILED、`solo_dzflat_transport.log` 见 1 例 FAILED | 实物确认（**7 条中 2 败** / 3 条中 1 败），日志可见 `orphan segment reset: kind = loan, prefix = 'w09_c1_0'` | ✅ |
| 早期"5 FAILED"表述 | **已按实物更正**（`UNSOUND_v1_selfhold.md:16-18`、`D14.md:154`、`:259` 三处均带 ⚠️ 更正块）—— 这是我在 round 1 提的 F4，**已闭合** | ✅ |
| R-3「跨会话不累积」措辞 | **已改为「同一会话内跨进程持续，直至被删除或系统重启」**，并注明"`/dev/shm` 为 0"只在该时点成立（`D14.md:243`）—— 我在 round 1 提的 F5，**已闭合** | ✅ |
| 回退族"写入点唯一" | **已按 `CounterId::` 前缀逐 ID 列表更正**：`fallback_total` = 3（3 个互斥分支）、`fallback_backend_unavailable` = 2、`fallback_capacity_full`/`wait_token_invalid` = 1；并补「9 个 ID 仅经 `c.detail` 间接可达」口径（`D14`/`回退计数…md:64-90`）—— 我在 round 1 提的 F3，**已闭合** | ✅ |

---

## 3. 要求 3：判决性实验**独立复现**（leaker + SIGKILL ⇒ 残留段 ⇒ 单跑 0% ⇒ 清理后 100%）

**自建驱动**：`tmp/t74/decisive.sh`；**自建 leaker**：由仓内源 `test/perf/out/20260928_w00_gate3_calib/src/leaker.cpp`
重建（`tmp/t74/leaker_mine`，⛔ 不依赖 t73 的探针二进制）；**输出**：`artifacts/perf/20261001-t74-W09-S4/decisive/`。

| 臂 | 库 | rc | OK | FAILED | `chunk pool exhausted` | `orphan segment reset` |
|---|---|---|---|---|---|---|
| A1 无泄漏 | FIX | 0 | 9 | 0 | 0 | 0 |
| A2 无泄漏 | NEG | 0 | 9 | 0 | 0 | 0 |
| A3 无泄漏 | ABL | 0 | 9 | 0 | 0 | 0 |
| **C 注入残留段** | **ABL** | **1** | **8** | **1** | **1** | 0 |
| E 只删该段 | ABL | 0 | 9 | 0 | 0 | 0 |
| C 注入残留段 | NEG | 0 | 9 | 0 | 0 | **1** |
| E 只删该段 | NEG | 0 | 9 | 0 | 0 | 0 |
| **C 注入残留段** | **FIX** | **0** | **9** | 0 | 0 | **1** |
| E 只删该段 | FIX | 0 | 9 | 0 | 0 | 0 |
| **F 对照：leaker 活着** | **FIX** | **1** | **8** | **1** | **1** | **0** |

- 注入读数：`loaned=40 mode=unpublished`，残留段 **5 283 892 B**；段已无活映射者（进程被 SIGKILL）
- **聚焦臂（"单跑 0% passed"形态**，只跑 `DzFlatBuilder.Tier0ElementArrayWrittenInPlace`）：
  **ABL `rc=1 / OK=0 / FAILED=3 / exhausted=1`** vs **FIX `rc=0 / OK=1 / orphan_reset=1`**
- F 臂证明**判据承重**：leaker 活着 ⇒ 段内仍有活持有者 ⇒ **不复位** ⇒ 单跑失败（正确行为）
- ⚠️ 驱动在调试期有**两处我自己的缺陷**（已修并留痕）：① leaker 路径误写为目录；
  ② 未校验二进制存在 ⇒ `rc=127` 曾被计成"命中"（与我在 t65 踩过的坑同类）

---

## 4. 要求 4：回退族写入点唯一性与语义分工（带 `CounterId::` 前缀，域 = `src/`）

```bash
grep -rn 'CounterId::fallback_total' src/ | wc -l                              # → 3
```

| ID | `src/` 内写入点 | 落入分支 |
|---|---|---|
| `fallback_total` | **3**（`shm_pub_sub_ipc.cc:1911/1915/1919`） | `kBackendUnavailable` / `kPoolStartFailed` / `kWaitSetFull`（**互斥 switch 分支**） |
| `fallback_backend_unavailable` | **2**（`:1912/1916`） | 前两支 |
| `fallback_capacity_full` | 1（`:1920`） | `kWaitSetFull` |
| `wait_token_invalid` | 1（`:1924`） | `kInvalidToken`（与 `kWaitSetFull` **不同 case** ⇒ 互斥） |

**语义分工（独立核对，成立）**：

| 组 | 语义 | 判据 |
|---|---|---|
| (a) `path_selection_*` | 开关关闭 ⇒ 走 TLV，**不进** `fallback_total` | `counters.h:620` 只 `r.inc(c.detail)` |
| (b) `fallback_total` + 原因 | 真回退 ⇒ **同时各 +1**（"首个失败资源"语义，总量维与原因维各记一份） | `counters.h:623-624` |
| (c) `borrow_failed_*` | B 借样失败 ⇒ **绝不碰** `fallback_total` | `counters.h:627`；`note_dzflat_borrow_failed()` 全文 **0 次**出现 `fallback_total` |
| 间接可达 | **9 个 ID** 经 `r.inc(c.detail)` 可达（`counters.h` 内 **3 处**调用点）⇒ 纯静态 `grep` 会漏判 | `grep -c 'r.inc(c.detail)'` = 3 |

⇒ 「每事件单落点 + 互斥分支」成立；「每 ID 单个写入点」**不成立**（W09 交付已按此更正）。

---

## 5. 要求 5：与 W06/W10 的边界 + 回归

| 项 | 读数 |
|---|---|
| ⛔ 不得由 W09 容量结论外推"千路可用" | W09 §2.3/§8 两行分列（单进程配额 vs 全机容量）；本验收**不**为规模背书 |
| `queue_backpressure` 无生产者 ⇒ 其 0 不得读作"无背压" | `grep -rn 'queue_backpressure' src/` = **0**；淘汰唯一写入点 = `include/dzIPC/common/circularqueue.h:132` |
| `ctest -j4` | **29/29 Passed**（`Total Test time 59.66 s`）；含 `test_chunk_capacity_backpressure` **6.58 s Passed**、`test_dzflat_fallback_semantics` Passed |
| 查表/复算命令 | 见 `artifacts/perf/20261001-t74-W09-S4/summary.md` 与 `controls/libswap.log` |

---

## 6. Findings（**均不阻断**本次 verdict；须 append-only 更正）

| ID | 严重度 | 问题 | 所需修法 |
|---|---|---|---|
| **W09-R2-F1** | **medium** | `D14.md:287`（§12.2）称负控"同一件 40 轮即出现 `same_id_pairs=1066`"。我以 **t73 自己的 `lib_negative`** 对同一件跑 **40 轮 × 8 话题 × 1 = 6 次独立**，全部 `same_id_pairs=0 / same_ptr_pairs=0`；`grep -rn '1066' artifacts/perf/20261001-t73-W09-F1/ tmp/t73/` = **0 命中** ⇒ **该数字无实物出处、且我复现不出** | 要么补出产出该数字的**命令 + 原始输出**，要么改用**可复现**的换库对照读数（我：同一二进制下 `lib_fixed 3/3 bad_rounds=0` vs `lib_negative 3/3 bad_rounds=1`，已存 `artifacts/perf/20261001-t74-W09-S4/controls/libswap.log`）。⛔ 不得保留无出处数字 |
| **W09-R2-F2** | **medium** | §12.2 与 `c9_libswap_matrix.log` 引的指纹 `lib_fixed=dbca161e…` / `lib_negative=0298b1df…` **已不存在于磁盘任何位置**（我全盘 `find` 核对）；现存的实体是 `lib_fixed=**d0f581d1…**` / `lib_negative=**0186853e…**` | 改引**现存**指纹对，并注明 `d0f581d1…` == 当期 `build/lib/libipc.so.3`（我本轮 FIX 用的就是它）。这是 t68 那类"陈旧指针"，第三人按原文核验会**找不到文件** |
| **W09-R2-F3** | low | §12.2「常驻用例」一行有 **三处与实物不符**：① 写成"**空前缀** 8 话题并发首借"，实际用 `kAliasPrefix = "w09c9alias"`（**专属非空前缀**，`test_chunk_capacity_backpressure.cpp:565/575/576`）；② 写成"**×300 轮**"，实际 `kRounds = 1200`（`:653`；我 11:46 观测到二进制曾为 2000，11:49 又被改为 1200 ⇒ 期间有并发改动）；③ 写成"单跑 **≈1.86 s**"，我实测 **7.7 s**（×2 次）。<br>⚠️ **经推算这是"过期"而非"凭空"**：实测每轮 ≈ 6.42 ms ⇒ 300 轮 ≈ **1.93 s**（与 1.86 s 吻合）、1200 轮 ≈ 7.7 s（与实测吻合）⇒ t73 写作时 300 轮/1.86 s **自洽**，之后被并发改到 1200/2000 轮而文档未跟 | 三处按当前实物改写；并注明"轮数/前缀随实现演进，引用须附源码行号 + 二进制指纹" |
| **W09-R2-F4** | low | t73 自留的 `decisive_repro/results.tsv` 有 **4 行 `rc=127` 且 `passed=0`**（二进制缺失，非有效读数）；`decisive_repro_t73/`、`decisive_repro_v4/` 的 ABL/BASE 行全为 **`rc=139`**（工装崩溃）且 `passed=0` —— 按本仓纪律这两类**不得**读作产品失败，但**未标注**，第三人易误读为"0% passed" | 在这三份 `results.tsv` 里给无效行加标注列（`invalid=missing_binary` / `invalid=harness_crash`），或改为只在 `run.log` 中留痕 |
| **W09-R2-F5** | low | §12.3 自认 ABA 窄窗口**仍未封死**（我认同），但**没有登记为带 owner 的未决项**（只写在正文散文里） | 在 §8.3 未决清单里增设一条：`ABA 窗口（快照↔复核之间的借还周期）`，修法 = 引入"本进程本档在飞借样数"（复核建议②），并给 owner（W09/共享层）。附我本轮的攻击读数（FIX 0/200 未打开） |

> ⚠️ **F1/F2 为何仍判 pass**：这两条影响的是 **t73 的"有牙证据"的引用形态**，而非其结论；
> 其结论（负控有牙、判据承重）我已用**更强的独立证据**（同一二进制换库 3/3 vs 3/3）另行确证。
> 但按本仓"无出处数字/陈旧指纹不得保留"的纪律，**必须**就地更正。

---

## 7. ⛔ 不能支撑的结论

1. ⛔ **不得**由本验收外推"W09 容量 ⇒ 1000 路可用"（规模结论归 W10/t11 台账）。
2. ⛔ **不得**声称"t22/t73 的段级复位问题已完全闭环"——**ABA 窗口仍开**（§12.3 自认 + 我 F5）。
3. ⛔ **不得**用单次小样本反例（如 V2NEG 的 0/60）当作"修复无效"或"负控无牙"的证据；
   判据是**换库对照**（同二进制、只换库）。
4. ⛔ **不得**引用 `queue_backpressure == 0` 作为"无背压"。
5. ⛔ **不得**把 `fallback_total` 的 3 个写入点读作"重复计数"（互斥分支）；
   反之也⛔不得声称"每 ID 单写入点"。
6. ⛔ **不得**在未标注 `invalid` 的前提下引用 t73`decisive_repro*/results.tsv` 里的 `rc=127`/`rc=139` 行。

---

## 8. 复现命令（可在单次 bash 调用内执行；`/dev/shm` 不跨调用保留）

```bash
cd /home/zwc/cpp_ipc_dds

# ① 容量模型（运行期）
./build/bin/test_chunk_capacity_backpressure 2>&1 | grep 'W09-C'
./tmp/t74/chunk_probe        # large_msg_cache=40 / id_pool<>::max_count=40
./tmp/t65/exp/vq_probe       # ViewQueueCap()=10

# ② 判决性实验（三库 A/C/E + F 对照；⛔ 单次调用内）
bash tmp/t74/decisive.sh "$PWD/artifacts/perf/20261001-t74-W09-S4/decisive"

# ③ 常驻用例换库对照（同二进制，只换 libipc.so.3）
BIN=build/bin/test_chunk_capacity_backpressure
F='*OrphanResetDoesNotAliasInflightLoansAcrossTopics*'
for L in tmp/t74/lib_fix tmp/t74/lib_v2neg tmp/t74/lib_neg; do
  printf '%s : ' "$L"
  for i in 1 2 3; do
    LD_LIBRARY_PATH=$PWD/$L $BIN --gtest_filter="$F" --gtest_color=no 2>&1 | grep -oE 'bad_rounds=[0-9]+' | head -1
  done
done
# 期望：lib_fix=0/0/0；lib_v2neg=1/1/1；lib_neg=1/1/1

# ④ 反例件（我的）
for L in fix v2neg neg; do rm -f /dev/shm/t65rep* /dev/shm/__IPC_SHM__CHUNK_INFO__9216__C40
  ./tmp/t74/ra/ra_$L 3 8 1 2>&1 | tail -1; done

# ⑤ 回退族（带前缀 + src/ 域）
grep -rn 'CounterId::fallback_total' src/ | wc -l                       # 3
for id in fallback_backend_unavailable fallback_capacity_full wait_token_invalid; do
  echo "$id $(grep -rn "CounterId::$id" src/ | wc -l)"; done            # 2 / 1 / 1
grep -c 'r.inc(c.detail)' include/dzIPC/measure/counters.h              # 3
grep -rn 'queue_backpressure' src/ | wc -l                              # 0

# ⑥ 回归
cd build && ctest -j4                                                   # 29/29
```

---

## 9. verdict

**pass**。

- 要求 1（容量模型四）**全部成立**，代码 + 运行期双证据；
- 要求 2（t22 v2 + t73 修法 + UNSOUND_v1 记录）**成立**：我逐行核对修法的结构，并用**自建三种负控 + 同一二进制换库对照**独立证实"有牙"（FIX 3/3 绿 vs 三种负控各 3/3 红）；
- 要求 3（判决性实验）**独立复现**（自建 leaker 与驱动，10 臂 + 聚焦臂给出"单跑 0% passed"形态）；F 对照证明判据承重；
- 要求 4（回退族）**语义分工与互斥成立**，逐 ID 写入点数与间接可达口径均与代码一致；
- 要求 5（边界 + 回归）**成立**：`ctest -j4` = **29/29**。

⇒ round 1 的 **W09-S4-F1（blocker）已闭合**；我对 round 1 另提的 F3/F4/F5 亦已在 W09 交付物中闭合。
本轮新增 5 条**证据卫生**缺陷（2 medium + 3 low）**不阻断**结论，但须 append-only 更正（建议在 t73 的
原任务上补 `evidence_note`，⛔ 不改既有 run 的数值，⛔ 不重开 t73 的终态）。

⚠️ **本次为只读复核**：⛔ 未改 `test_chunk_capacity_backpressure.cpp`、⛔ 未改 W09 任一交付物、
⛔ 未改产品代码、⛔ 未改既有 run（影子库与探针全部在 `tmp/t74/**`，证据写在
`artifacts/perf/20261001-t74-W09-S4/`）。
