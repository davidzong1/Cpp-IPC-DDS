# t74 · W09 S4 复审 round 2 —— 独立证据汇总（只读复核；⛔ 未改产品代码/W09 交付物/既有 run）

复核者：验证与性能负责人（W09/t10、t22、t73 实现者 = 共享层负责人 ⇒ 独立性成立）

## 一、库指纹（全部我自建，除 FIX）
| 代号 | 内容 | sha256[0:16] | 来源 |
|---|---|---|---|
| FIX | 当前 tree 构建（含 t73 修法） | d0f581d19f74c411 | `build/lib/libipc.so.3` |
| NEG | 我自建负控：**只把「素净判定 + 快照」移出 `handles_` 的 `lock_`** | 1e3c5e5fe66ca9e3 | 由当前 `src/` 逐字改 1 处 |
| V2NEG | 我自建负控 2：还原 **t73 之前**的 v2 写法（判定+快照在 `lock_` 释放后、**且在 /proc 扫描之后**） | 221d04877883809f | 由当前 `src/` 改 2 处 |
| ABL | 我自建消融：**删掉 `reclaim_orphan_segment` 调用** | 012cd0a02320437d | 由当前 `src/` 改 1 处 |
| t73NEG | t73 自己保存的负控 | 0186853e1eadb5ea | `tmp/t73/lib_negative/libipc.so.3` |

## 二、判决性实验独立复现（自建 leaker + 自建驱动，单次 bash 调用内）
驱动：`tmp/t74/decisive.sh`；输出：`artifacts/perf/20261001-t74-W09-S4/decisive/`
| 臂 | 库 | rc | OK | FAILED | exhausted | orphan_reset |
|---|---|---|---|---|---|---|
| A1 无泄漏 | FIX | 0 | 9 | 0 | 0 | 0 |
| A2 无泄漏 | NEG | 0 | 9 | 0 | 0 | 0 |
| A3 无泄漏 | ABL | 0 | 9 | 0 | 0 | 0 |
| C 注入残留段 | **ABL** | **1** | **8** | **1** | **1** | 0 |
| E 只删该段 | ABL | 0 | 9 | 0 | 0 | 0 |
| C 注入残留段 | NEG | 0 | 9 | 0 | 0 | **1** |
| E 只删该段 | NEG | 0 | 9 | 0 | 0 | 0 |
| C 注入残留段 | **FIX** | **0** | **9** | 0 | 0 | **1** |
| E 只删该段 | FIX | 0 | 9 | 0 | 0 | 0 |
| F 对照 leaker **活着** | FIX | **1** | **8** | **1** | **1** | **0** |

- 注入读数：`loaned=40 mode=unpublished`，残留段 **5 283 892 B** = `52 + 40×132096` ✅
- 聚焦臂（只跑 `Tier0ElementArrayWrittenInPlace`）——**「单跑 0% passed」形态**：
  ABL **rc=1 / OK=0 / FAILED=3 / exhausted=1**；FIX **rc=0 / OK=1 / orphan_reset=1**
- 驱动脚本的两处自身缺陷（已修并留痕，同 t65 教训）：① leaker 路径写成目录；② 未校验二进制存在 ⇒ rc=127 被计成命中

## 三、F1 窗口（t73 修法）的独立定位与有牙证据
1. **结构核对**：`src/libipc/ipc.cpp:542-576` —— 快照 `memcpy(&orphan_snap, ...)` 与 `h->valid()` 赋值同在
   `std::lock_guard<std::mutex> guard{lock_}` 作用域内；`reclaim_orphan_segment()`（`:469`）改为接收
   `orphan_candidate` / `snap_in`，自身**不再**读池做判定 ✅
2. **我的反例件**（`R1/S4验收_W09_证据/repro_alias.cpp`）重编后：
   - 1 轮 × 2 话题 × 60 次：FIX 0/60、V2NEG 0/60、**NEG 6/60**
   - 3 轮 × 8 话题 × 60 次：FIX 0/60、V2NEG 2/60、NEG 2/60
3. **常驻用例换库对照**（同一二进制 `kRounds=2000`，只换 `libipc.so.3`）：
   FIX `0/0/0` vs V2NEG `1/1/1` vs NEG `1/1/1` vs t73NEG `1/1/1`
4. **ABA（t73 自认仍开着的窄窗口）攻击**：我新写 `tmp/t74/aba/aba_probe.cpp`
   （借还紧循环 vs 并发首借）：FIX 0/200；V2NEG 3/200；NEG 6/200 ⇒ 该窄窗口在 200 次攻击下**未被打开**
   （不代表已封死，仅本次攻击未命中）
5. **锁序**：静态扫描「持 `info->lock_` 再取 `handles_` 锁」的逆序区间 = **0** 处 ⇒ 无嵌套死锁风险

## 四、容量模型（逐条，代码 + 运行期）
| 项 | 判据 | 读数 |
|---|---|---|
| 每档 40 块 | `include/libipc/def.h:49`；`id_pool.h:47 max_count = limited_max_count()` | 探针：`large_msg_cache=40`、`id_pool<>::max_count=40`；用例 `[W09-C1] 档 9216 可用块数=40`、`[W09-C8] 借满=0 → 归还后空闲=40` |
| 归属键 `(prefix, chunk_size)` | `ipc.cpp:373-379` + `ls /dev/shm` | 空前缀 `__IPC_SHM__CHUNK_INFO__132096__C40`；命名前缀 `w09_c2_0__IPC_SHM__CHUNK_INFO__9216__C40` |
| 默认空前缀 ⇒ 同档全机共享 | `shm_pub_sub_ipc.cc:861` name-only 构造 ⇒ `ipc.cpp:1440-1443 connect(ph,{nullptr},...)` | prefix 恒空串 |
| 队列上限 | `nodelet_config.cc:110+`（`max(1, 40/4)`）；`shm_pub_sub_ipc.cc:1333+` pin | 探针：`ViewQueueCap()=10`、`IsViewQueuePinEnabled()=1` |

段字节公式：`52 + 40×chunk_size` ⇒ 132096 档 = 5 283 892 ✅（与实测一致）

## 五、回退族（带 `CounterId::` 前缀，域 = `src/`）
| ID | 写入点 | 落入分支 |
|---|---|---|
| `fallback_total` | **3**（`:1911/1915/1919`） | kBackendUnavailable / kPoolStartFailed / kWaitSetFull（**互斥 switch 分支**） |
| `fallback_backend_unavailable` | **2** | 前两支 |
| `fallback_capacity_full` | 1 | kWaitSetFull |
| `wait_token_invalid` | 1 | kInvalidToken（与上者不同 case ⇒ 互斥） |
- 间接可达：`r.inc(c.detail)` **3 处**（`counters.h`），覆盖 9 个 ID
- 分类器三支互斥（`counters.h:611-629`）：`path_selection` 只 inc detail；`fallback` 同时 inc total+detail；
  `borrow_failed` 只 inc detail，`note_dzflat_borrow_failed()` 全文**0 次**碰 `fallback_total`

## 六、回归
`ctest -j4` = **29/29**（含 `test_chunk_capacity_backpressure` 3.35 s Passed、`test_dzflat_fallback_semantics` Passed）

## 七、t74 复核结论与残留缺陷
- verdict: **pass**（5 条验收项全部独立满足）；round 1 的 W09-S4-F1 blocker **已闭合**
- 复审报告：`docs/消息接收架构改造/团队改造交付/R1/S4验收_W09_round2.md`
- 新增 5 条**证据卫生**缺陷（不阻断）：F1 `same_id_pairs=1066` 无出处且复现不出；F2 引的库指纹
  `dbca161e…`/`0298b1df…` 已不存在（现存 `d0f581d1…`/`0186853e…`）；F3 「300 轮/1.86 s/空前缀」三处过期
  （实测 1200 轮/7.7 s/专属前缀；⛔ 经推算 300↔1.93 s 自洽，属**过期**非凭空）；F4 `decisive_repro*/results.tsv`
  的 rc=127/139 无效行未标注；F5 ABA 窄窗口未登记为带 owner 的未决项
- ⛔ 未改产品代码 / W09 交付物 / 既有 run；影子库与探针全在 `tmp/t74/**`

## 八、我自己的两处过程缺陷（留痕）
1. 反例件命中率统计曾把**不存在二进制的 rc=127** 计为"命中"（与 t65 同类坑），已改为按 `rc==1` 计数并前置存在性校验
2. 首版 `decisive.sh` 把 leaker 路径写成目录 ⇒ 注入未生效、全部臂假绿；已修正并重跑
