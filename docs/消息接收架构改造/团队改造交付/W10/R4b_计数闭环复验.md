# W10-R4b §13.2#3 计数闭环复验（t43）

> 任务：`t43`（§13.2#3 计数闭环复验：t35 接线后双臂重跑 + `first_failed_resource` 定位），attempt `7bed9209-682d-4611-9228-5933504143aa`
> 负责人：**验证与性能负责人**｜依据：R1 分部复核（t31）F1 判定 + t35（F1 接线）交付 + t34/R0 冻结条款
> 证据目录：`artifacts/perf/20260930-r46-W10-R4b/`（新 run_id，⛔ 未覆盖 r25/r27）
> 自评等级：**S3**。⛔ S4 独立验收归 `t14`/R1。

**一句话**：§13.2#3 的**四类容量计数在 t35 接线后全部可判**（∅→非零逐类取证），
`first_failed_resource` **可定位**（`none → chunk_exhausted` / `wait_set_full`）；
但**27 项静态未接线 ID 中至少 1 项（`path_selection_dzflat_disabled`）运行期实测非零**
⇒ 静态清单**不能**替代运行期实测，本报告据此给出**静态 ∪ 运行期**的联合覆盖声明。

---

## 0. 速查

| 要求 | 结果 |
|---|---|
| ① 双臂重跑（SHM + socket，各 1000 独立话题，R3 修正工装） | ✅ 双臂 `verdict=PASS`、逐 route 台账 1000/1000、`c1..c5=1` |
| ①' A 腿 / B 腿**分别**取证 | ✅ A 腿（send）`chunk_exhausted 0→1`；B 腿（loan）`chunk_alloc_failed 0→80`；两腿互不冒充 |
| ② `first_failed_resource` 可定位 | ✅ `none → chunk_exhausted`（独立两阶段对照）+ `none → wait_set_full`（规模档 W=1，873 条） |
| ③ 27 项未接线 ID 的 0 解读纪律 | ✅ 写入 `counters.json.counter_coverage` + `manifest.json`；并**修正**：其中 1 项运行期实测非零 |
| ④ §13.2#6 的处置 | ✅ 明确「**部分测量、不可判定**」+ 列出补齐所需产品侧 API |
| ⛔ 未写成「六条全部满足」 | ✅ §5 逐条给出可判定/不可判定 |
| 采集期间发现的**工装缺陷** | ⚠️ **H7**（`samples_` 数据竞争 ⇒ `double free` rc=134）已定位、修复、复跑 3/3 绿并留档 |

---

## 1. 为什么必须重跑（而 r25/r27 不能用）

R1 分部复核（t31）判定 §13.2#3 **不成立**，根因是 `chunk_exhausted`/`queue_evicted` **在 `src/` 下 0 个写入点**
⇒ `counters.json` 里的 0 **不可读作"未发生"**。该根因**已在 t35 接线**：

| ID | 唯一写入点 | 本 run 复核 |
|---|---|---|
| `chunk_exhausted` | `src/libipc/ipc.cpp:723`（`note_pool_exhausted`，非 `loan` 分支） | ✅ 文件在位、行号一致 |
| `chunk_alloc_failed` | `src/libipc/ipc.cpp:720`（`kind == "loan"` 分支） | ✅ |
| `queue_evicted` | `include/dzIPC/common/circularqueue.h:132`（淘汰分支，**不注册回调也计**） | ✅ |
| `wait_set_full` / `wait_token_invalid` | 原有接线 | ✅ 未被破坏（见 §3 臂 D/E2） |
| `counters.h` | — | ✅ sha256 **`1f9c2bd0…`**，与 t35 开工前**逐字一致** ⇒ 未新增任何 CounterId |

但 r25（SHM）/r27（socket）**都跑在接线之前**（库 `43288e5f…` / `81fe91ff…`）⇒ 其 §13.2#3 判定**必须废弃**。

**本 run 的库** = `558f47ede2419e14554766af55e53671e69d05d940b05351b1d4aa0e45d79031`（成对指纹见 `fingerprint.txt`）。

---

## 2. 要求 ①：双臂重跑（R3 修正工装，各 1000 独立话题）

| 臂 | 命令要点 | rc | 判定 | 逐 route 台账 | §13.2 断言 |
|---|---|---|---|---|---|
| **SHM** | `--transport shm --topology independent --n 1000 --msgs 3 --payload 64` | 0 | **PASS** | **1000 行 / 1000 唯一名 / 全条件满足 1000** | `c1..c5=1`，`c6=部分测量` |
| **socket** | `--transport socket --topology independent --n 1000 --msgs 3 --payload 64` | 0 | **PASS** | **1000 行 / 1000 唯一名 / 全条件满足 1000** | 同上 |

（`c1..c6` 为 t37 建立的 §13.2 六条机械断言；`c6` 按设计恒为"部分测量"。）

**四个容量类在双臂上的读数**（`counters.json.capacity_classes`）：

| 类别 | SHM 千路 | socket 千路 | 读法 |
|---|---|---|---|
| `wait_set_full` | **0** | **0** | 千路 + 32 worker ⇒ 容量 4064 ≫ 1000，**结构上不可能触发** ⇒ 0 是**正确值**（另有 §3 臂 D 与 §2.1 的 W=1 档单独取证） |
| `wait_token_invalid` | **0** | **0** | 同上（另有臂 E2 单独取证） |
| `chunk_exhausted` | **3** | 0 | SHM 臂**真实触发**了 send 腿池空（见下）；socket 臂无 chunk 池 ⇒ 应 0 |
| `queue_evicted` | 0 | 0 | 千路定速（每 route 3 条、队列 64）⇒ 不可能满 ⇒ 0 正确（另有 §3 臂 C 与 hotcold 档取证） |
| `fallback_total` | 0 | 0 | ✅ §13.2#3 要求的 `fallback=0` 成立 |
| **`first_failed_resource`** | **`chunk_exhausted`** | `none` | SHM 臂因真实池空而**定位到具体资源**；socket 臂无池 ⇒ `none`（但其 `none` 排除力受 §4 限制） |

**SHM 臂的池空是真实的**（日志原文）：
```
chunk pool exhausted: kind = no_member_send, chunk_size = 1024, size = 88, pool capacity = 40, count = 1 (本进程)
```
⇒ `kind=no_member_send` 属 **A/send 腿**（已交付、降级分片）⇒ 记 `chunk_exhausted` **正确**。

### 2.1 但千路定速档**不足以**证明容量类"贴真实负载" ⇒ 补两档规模压测

| 档 | 构造 | 容量类读数 | `first_failed_resource` |
|---|---|---|---|
| **W=1 + n=1000**（`DZIPC_SHM_RECV_WORKERS=1`，容量 127） | 873 条溢出 ⇒ 显式回退 | `wait_set_full=**873**`、`fallback_capacity_full=873`、`registration_failed=873`、`chunk_exhausted=4` | **`wait_set_full`** |
| **hotcold + hot-msgs=40000** | 热路满速 4 万条压满同 worker 的 64 深队列 | `queue_evicted=**39936**`、`chunk_exhausted=**1965**` | **`chunk_exhausted`** |
| hotcold + hot-msgs=20000 | 同上、量减半 | `queue_evicted=19936`、`chunk_exhausted=0` | `none`（该档池未耗尽 ⇒ 正确） |

⇒ 四类容量计数**在目标规模下贴住了真实负载**（873 / 39936 / 1965 / 0），且**逐档可解释**。
⚠️ W=1 档的 `verdict=FAIL` 是**预期的**：873 条回退使 `fallback_total≠0` —— 该档的用途是**给计数加负载**，
⛔ 不是规模通过档（其台账仍是 1000/1000 全条件满足）。

---

## 3. 要求 ①'：A 腿与 B 腿**分别**取证（⛔ 不得用一条腿的 0 代表另一条）

**同库**重跑 t35 的驱动（`run_all_arms.sh`，本 run 目录内自带，逐臂清 `/dev/shm`）：

| 臂 | 目标 ID | 造法 | BEFORE → AFTER | 关键判据 |
|---|---|---|---|---|
| **A** | `chunk_exhausted` | DZFlat **关** + 订阅者**建了但不消费** ⇒ `send`/`no_member_send` 腿池空 | `0 → **1**` | `publish_ok=200/200`（**已交付**，降级 64 B 分片）⇒ 属"已交付但池空" |
| **B** | `chunk_alloc_failed` | DZFlat **开** + B **借样腿** `loan()` 池空 ⇒ 拒绝 | `0 → **80**`（`loaned=40 rejected=80`） | `loan` 返回**无效**（**未交付**）⇒ 与 A 是**不同类别** |
| **C** | `queue_evicted` | `CircularQueue<int>(2)` push 100（**不注册** evict 回调） | `0 → **98**` | 剩 2 条 ⇒ 淘汰 98 条，**与回调无关** |
| **C2** | `queue_evicted`（**跨二进制**） | 真 pub/sub：队列在 **libipc.so** 内 push，探针在另一二进制读 | `98 → **494**` | 证 `CounterRegistry` 单例在进程内**唯一**（否则必为 0） |
| **D** | `wait_set_full` | `DZIPC_SHM_RECV_WORKERS=1` + 140 订阅 | `0 → **13**`（`fallback_capacity_full=13`、`registration_ok=127`） | 原有接线未被破坏 |
| **E2** | `wait_token_invalid` | 无效 wait token | `0 → **1**` | 原有接线未被破坏 |

**A 腿与 B 腿的对拍**（逐字复现 t35 的读法要点）：

| 臂 | `chunk_exhausted` | `chunk_alloc_failed` | 含义 |
|---|---|---|---|
| A（send 腿） | **1** | **0** | 消息**已交付**（降级分片）⇒ 池空归 `chunk_exhausted` |
| B（loan 腿） | **0** | **80** | 借样**未交付**（被拒）⇒ 归 `chunk_alloc_failed` |

⇒ **两腿互不冒充**：A 腿的 0 不代表 B 腿未发生，反之亦然（这正是 W09 §5.1 的两类之分）。

---

## 4. 要求 ②：`first_failed_resource` **可定位**

### 4.1 独立两阶段对照（专门探针 `w10_r4b_firstfail`）
同一进程、同一库、同一张 §13.3 表（与 `w10_matrix.cpp` 同一枚举顺序）：

```
stage1_no_construction      first_failed_resource = none
stage2_send_leg_exhausted   first_failed_resource = chunk_exhausted   (chunk_exhausted=1, chunk_alloc_failed=0)
FIRSTFAIL_LOCATABLE=1 (none → chunk_exhausted)
```
⇒ 从 `none` 变为**具体资源名**，可机械判定（`first_failed_resource.json` 的 `locatable: true`）。

### 4.2 第二条独立路径：`none → wait_set_full`（规模档，见 §2.1）
⇒ 定位能力**不依赖单一类别**：既有 `chunk_exhausted` 也有 `wait_set_full` 两条独立命中。

---

## 5. 要求 ③：`counters.json` 解读纪律（**并修正 t35 清单**）

### 5.1 纪律（已**机器化**写入产物）
`counters.json.counter_coverage` 与 `manifest.json.counter_reading_discipline` 现含：

| 字段 | 内容 |
|---|---|
| `covered_classes` | **13 项**：本 run **可判定**的类别（接线有效且运行期可触发） |
| `unwired_ids` | **27 项**：静态清单判定为未接线者 ⇒ **其 0 = 未采集**，⛔ 不得读作"未发生" |
| `diagnostics_gated_ids` | **4 项**：`scan_time_ns_total` / `ready_observed` / `deferred_depth_last` / `deferred_depth_max`（关诊断时**结构性必为 0**） |
| `reading_discipline` | 上述读法的原文纪律（依据 t35/F1 §6 冻结条款） |
| `diagnostics_enabled` | 本 run 实测值（千路臂为 **false**） |
| `first_failed_resource_caveat` | `none` 的排除力**不完整**（未采集集合内的类别不可据 0 排除） |

### 5.2 ⚠️ 但静态清单**不能**替代运行期实测 —— 本任务实测到 1 项反例

t35 的 27 项清单来自**静态**审计（`audit_counter_wiring.py`）。该审计看不到**经载体变量的间接写**
（`r.inc(c.detail)`，`detail` 由运行期决定）。本任务用 `w10_r4b_runtimetruth` 在三条腿下实测：

| 腿 | t35 27 项中**实测非零**的项 |
|---|---|
| baseline（DZFlat 关 + 正常收发 50 条） | **`path_selection_dzflat_disabled` = 50** |
| A 腿（DZFlat 关 + 池空 200 条） | **`path_selection_dzflat_disabled` = 200** |
| B 腿（DZFlat 开 + 借样） | （无） |

并且 SHM 千路臂的 `counters.json` 里该项为 **4007**。

**结论（对 t35 清单的修正，append-only 性质的本报告内新增）**：
1. `path_selection_dzflat_disabled` **不在**"未采集"集合里 —— 它有**运行期写入点**
   （`note_dzflat_attempt` → `classify_dzflat_attempt` → `r.inc(c.detail)`，`detail` 取值由分类器决定），
   静态审计只因"未出现字面 ID"而判 `table_only`。⇒ **其 0 可读作"本轮未发生"**。
2. 同理，**静态重跑**本任务的工作区还显示 3 项的判定从 `table_only` 变为 `potential_used`
   （`borrow_failed_no_receiver` / `_publish` / `_reason_unknown`，载体 `note_dzflat_borrow_failed`
   现已在 `include/dzIPC/shm_pub_sub_ipc.h:118/126/140` 有调用方）⇒ 它们**也已离开**"未采集"集合的前身形态，
   但本任务的腿未触发其具体出口（`publish_loaned` 失败路径），故仍按**未实测**登记。
3. ⇒ **联合覆盖口径**：`未采集集合 = (静态清单 27 项 − 运行期实测非零项)`，且**任何引用都必须同时给静态判定与运行期读数**。

### 5.3 本 run 对 §13.2#3 的**覆盖范围**（明确声明）

| 判定 | 类别 |
|---|---|
| ✅ **可判定（实测）** | `wait_set_full`（873 @W=1；0 @千路）、`wait_token_invalid`（1 @臂E2；0 @千路）、`chunk_exhausted`（1 @臂A、1965 @hotcold、3 @千路）、`queue_evicted`（98 @臂C、494 @跨二进制、39936 @hotcold、0 @千路）、`fallback_total`（873 @W=1；0 @千路）、`chunk_alloc_failed`（80 @臂B） |
| ⛔ **仍未采集（不可判定）** | `registration_rejected`、`fd_limit`、`queue_backpressure`、`generation_mismatch`、`publish_blocked`、`publish_failed`、`rx_timeout`、`tlv_bytes`/`dzflat_a_bytes`/`dzflat_b_bytes`、`fallback_type_incompatible`/`_oversized`/`_pool_exhausted`/`_reason_unknown`、4 个 `borrow_failed_*`（除实测的非零项）、`seq_*` 5 项、`payload_checksum_*` 2 项 |
| ➖ **门控（关诊断时结构性必为 0）** | `scan_time_ns_total`、`ready_observed`、`deferred_depth_last`、`deferred_depth_max` |

### 5.4 §13.2#3 判定

> **§13.2#3 = 部分成立**：
> · `fallback_count == 0`（千路档）✅ 成立；
> · **四类容量计数各自独立、且在接线后贴真实负载** ✅ 成立（∅→非零逐类取证，见 §3）；
> · **但**：`first_failed_resource == "none"` 的**排除力不完整** —— 上述第 2 行 20+ 类仍在未采集集合，
>   ⛔ 因此**不能**写"本轮无任何失败资源"。
> ⇒ t31 原判「#3 不成立（根因未接线）」**已消解**；但 **#3 的完全形态（十类皆可判）仍未达成**。

---

## 6. 要求 ④：§13.2#6 的处置 —— **部分测量、不可判定**

§13.2#6 要求"结束时 `route/token/fd/队列/chunk` 回到可解释基线"。当前状态：

| 资源 | 状态 | 依据 |
|---|---|---|
| route | ✅ 已测（`routes_after_destroy=0`） | t37 起从**各自池**读（SHM/socket 分取） |
| token | ✅ 已测（**代理**：池内在册数 → 0） | `add_route` 取 token、`remove_route` 同步摘除 ⇒ 一一对应 |
| fd | ✅ 已测（`fds_open→fds_after_destroy`，按 R0-5/R0-6 冻结模型判 4096 上限） | — |
| **queue**（view/adopt 深度） | ⛔ **未测 ⇒ 不可判定** | **无公开读数 API**（`grep queue_depth/pool_used` 命中 0） |
| **chunk**（池内已用块数） | ⛔ **未测 ⇒ 不可判定** | **无公开读数 API**；外部只能看 `/dev/shm` 段文件存在性与总大小，推不出"已用块数" |

**本报告的明确处置**：§13.2#6 记为「**部分测量、不可判定**」，⛔ **不写成满足**。
**补齐所需的产品侧 API**（供后续条目评估，⛔ 本任务不改产品代码）：

1. **队列深度**：`shm_sub_ipc` / `socket_sub_ipc` 增 `queue_usage()`（返回 `{size, capacity, high_watermark}`）——
   数据源在 `SubState::msg_queue`（`CircularQueue`），只需一个 const 访问器；
2. **chunk 池占用**：`ipc::id_pool<...>` 增只读 `used_count()/total_count()`（`max_count` 已是公开常量），
   再经 `shm_pub_ipc::pool_stats()` 暴露 ⇒ 可在结束前后各读一次做基线对账；
3. 两者都需要**接口变更流程**（R0-6：口径与接口冻结）；本任务只在报告中提出，⛔ 不擅自实现。

⇒ **§13.2 六条**：`#1/#2/#4/#5` 在本 run 双千路臂上 ✅；`#3` **部分成立**（§5.4）；`#6` **部分测量、不可判定**。
⛔ **不得写成"六条全部满足"**。

---

## 7. 采集期间发现的**工装缺陷 H7**（如实登记，按 t37 的 H1–H6 同例）

| 项 | 内容 |
|---|---|
| 现象 | `hotcold × n=1000 × hot-msgs∈{20000,40000}` ⇒ **2/2** `double free or corruption (!prev)`，**rc=134** |
| 根因 | `Harness::samples_`（`std::vector<std::string>`）被**两线程并发 `push_back`**：`hot_thread`→`publish_route()`（tx）与主线程→`publish_route()`/`drain_once()`（tx/rx） |
| 为何 r25 未崩 | r25 的 hotcold 臂 `publish_route()` **不写样本**（t31-F6 缺陷）；t37 修 F6 时让它也写样本，**激活了潜伏竞态** |
| 修法 | `samples_mtx_` + `push_sample()` 单点写入（3 个写点全部改走） |
| 修后 | **3/3 rc=0 verdict=PASS**（`scale-capacity/hc-fixed-{1,2,3}.log`） |
| 留档 | `h7-race/before_double_free.log` + `h7-race/README.txt`（原始证据保留，不删不覆盖） |

> ⚠️ 这是**工装自身**的缺陷，会让"规模运行失败"被误判为产品问题；同一来源的归因纪律须重申：
> **工装崩溃（rc=134/139）不得计入产品失败率**。

---

## 8. 交付物 / 边界 / 未决

| 路径 | 内容 |
|---|---|
| `W10/R4b_计数闭环复验.md` | 本文件 |
| `artifacts/perf/20260930-r46-W10-R4b/fingerprint.txt` | 库/工装/源文件**成对指纹**（R0-A1） |
| `.../dual-arm/{shm,socket}-ind-1000/` | 双臂逐 route 台账 + `counters.json`（含覆盖声明）+ `phases.csv` |
| `.../arms/` | A/B/C/C2/D/E2 六臂 BEFORE/AFTER + 汇总（含外侧三探针源码） |
| `.../scale-capacity/` | W=1（873 回退）、hotcold 20000/40000（19936/39936 淘汰）、H7 修后 3 轮 |
| `.../firstfail/` | `first_failed_resource` 两阶段对照（`locatable: true`） |
| `.../runtime-truth/` | **运行期真相表**三条腿（静态清单对账，发现 **1 项静态误判**：`path_selection_dzflat_disabled` 实测 50/200/4007 非零） |
| `.../h7-race/` | H7 修前证据与说明 |
| 工装 | `test/perf/w10/w10_r4b_firstfail.cpp`、`w10_r4b_runtimetruth.cpp`、`w10_matrix.cpp`（H7 修复 + 覆盖声明） |

**边界**：⛔ 未改 `recv_worker.*`（t42）、`ipc.cpp`/`circularqueue.h`（t35 已终态）、`counters.h`（`1f9c2bd0…` 未变）、他人 WP；
⛔ 未覆盖 r25/r27；按 §12 新建 run_id `20260930-r46-W10-R4b`。

**与 t35 清单的差异（append-only 上报，⛔ 不重开 t35 终态）**：

| ID | t35 静态判定 | 本次静态重跑 | 本次运行期实测 | 正确读法 |
|---|---|---|---|---|
| `path_selection_dzflat_disabled` | `table_only` | `table_only` | **50 / 200 / 4007**（非零） | **可实测**，0 可读作"未发生" |
| `borrow_failed_no_receiver` / `_publish` / `_reason_unknown` | `table_only` | `potential_used`（载体已在 `shm_pub_sub_ipc.h` 有调用方） | 本任务腿未触发其出口 ⇒ **未实测** | 仍按**未采集**登记 |
| `borrow_failed_pool_exhausted` / `_oversized` | `table_only` | `table_only` | 0（**全仓无生产者**） | **产品缺口**，⛔ 不是采集缺口 |
| `scan_rounds` / `scanned_routes_total` | `selftest_only` | `selftest_only`（`proc_sampler.h`） | — | 由 t30 的**常驻孪生**覆盖（见 R5 报告 §3） |

**未决（供后续条目）**：① `path_selection_dzflat_disabled` 等经载体间接写的 ID 需**重新冻结静态清单口径**
（静态 ∪ 运行期）；② §13.2#6 的 queue/chunk 读数 API 需走接口变更流程；③ `borrow_failed_pool_exhausted`/`_oversized`
两项**全仓无生产者**（`grep` 命中 0）⇒ 属产品缺口而非采集缺口，须单独立项。
