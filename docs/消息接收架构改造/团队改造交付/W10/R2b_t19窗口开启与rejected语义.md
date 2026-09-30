# R2b — t19 窗口开启（调用点补丁应用，11 个 ID 激活）+ `registration_rejected` 产品语义裁定

> 任务：`t41`｜负责人：共享层负责人｜attempt `d2b0e58a-7b4c-430f-b14f-cfeeafadf6d7`
> run_id：**`20260929-r41-W10-R2b`**；证据双份放置（`build/**` 与 `artifacts/**` 均可能被清理）：
> ① 工作目录 `artifacts/perf/20260929-r41-W10-R2b/`（只追加）；
> ② **交付目录内副本 `团队改造交付/W10/R2b证据/`（计入 git，durable）**。两处内容逐字一致。
> 上游：t19（本包作者，分类器与语义已实现、调用点补丁待窗口）、t6/t7（已落地 ⇒ 窗口开启）、t35（F1 四类接线闭环 + 27 项未接线清点）、W10-R0（R0-8/R0-11/R0-12 口径与写入安排）
> 边界：**仅** `src/dzIPC/shm_pub_sub_ipc.cc` / `include/dzIPC/shm_pub_sub_ipc.h` 的**计数器调用点**；⛔ 未改 `include/dzIPC/measure/counters.h`（sha256 与开工前逐字一致）、未改行为语义、未改他人 WP。

---

## 0. 一分钟结论

| 验收条款 | 结论 |
|---|---|
| 补丁对当前树**干净应用** | ✅ 两份 patch（t19 原样）`patch -p0 --dry-run` 通过，实测应用成功（offset 464/16 行，因 t6/t7 在其上方插入了代码 ⇒ **不需要重新生成**，见 §2） |
| 前后 sha256 + 「仅计数器点」声明 | ✅ 前后四组 sha256 齐全（§2.1）；「仅计数器点、无行为变更」由 **A–F 六条机械判据**证明（§4），非口头声称 |
| 三语义可分的**同臂读数** | ✅ 同二进制 5 条臂、5 轮逐字一致（§3）：`(a)` 路径选择 `fallback_total=0`；`(b)` 真回退 `fallback_total` 与原因**同时 +1**；`(c)` `borrow_failed_*` 与 `fallback_total` **分列**（三个 (c) 臂 `fallback_total` 恒为 0） |
| 与 t35 已接线的 `chunk_exhausted`/`chunk_alloc_failed`/`queue_evicted` 口径一致、⛔ 不重复计数 | ✅ 机械核对：三者在本改动面 **0 处**（唯一写入点仍在 `ipc.cpp:720/723`、`circularqueue.h:132`）；ARM5 实测两族同时出现且**各记各的**（§3.3） |
| `registration_rejected` 明确裁定（含与 `wait_set_full` 的边界） | ✅ 裁定 **(ii) 不适用**：`RecvRegisterStatus` 的 **8 个取值已有唯一具体归因**（机械提取表 §5.2），generic「rejected」**无剩余语义空间**；若强行新增会造成与 `registered_*` 系列的**双计数** |
| 「11 个 ID」被激活 | ✅ 两个分类器关联 **11 个 ID**（逐数对上任务书）：其中 **9 项**由 t35 的 `table_only` 变为**已接线**，另 **2 项**（`fallback_total`/`fallback_capacity_full`）此前已有 W06 接收路径写入点、本包为其增加**发布路径族**写入点（§6.1 给出两族的区分读法） |
| ⛔ 无重复计数 | ✅ `check_wiring.sh` 5 组机械检查全绿（§4.3） |

**残余（如实登记，不掩盖）**：`borrow_failed_pool_exhausted` 仍**无写入点**（ARM5 实测 `loan_invalid=5` 但该 ID 为 0）—— 即 W19-F2 未闭环，需 `ipc::loan()` 加原因出口（libipc 接口变更，超出本任务授权）。本包**不为收口把它猜成有值**。

---

## 1. 为什么这一步是必需的（不是"补个样式"）

t35（F1）用只读脚本核实：`note_dzflat_attempt` / `note_dzflat_borrow_failed` 两个分类器**全仓无调用方**。这意味着 t19 的「三语义可分」当时**只有定义、没有调用证据** —— 而方案 §13.2#3 需要的正是调用证据。

更严重的是**失效方式静默**：若无调用方，`counters.json` 里 `fallback_total` 与全部 11 个 ID **恒为 0**；读报告的人会把它读成「本轮没有回退」，而真实含义是「**这一维根本没被观测**」。本包把这一维接上，并在 §9 给出可机械判定的读法纪律。

---

## 2. 补丁对当前树的适用性（要求 1）

### 2.1 前后指纹（要求 3）

| 文件 | 接前 sha256 | 接后 sha256 |
|---|---|---|
| `src/dzIPC/shm_pub_sub_ipc.cc` | `12131d8c354b868faca61204ceb21600a4888109a1dd059442f230358000586f` | `ceab50331f2a2449a7b425afa21c2f9a0e27e6a02016dbacc384a34dfdba0628` |
| `include/dzIPC/shm_pub_sub_ipc.h` | `b12be18e00bc836b6d78d4c0aa9f55354ad01ae8baf8fa1e8450e6a717329f6b` | `40eeb7cc20570ac0e9f968eef4d69d8ede21ff178161295c134b1ada24568e7b` |
| `include/dzIPC/measure/counters.h`（⛔ 不动） | `1f9c2bd0b9419aea1332c9c295e032ef8e5889e40bf48ebdbf79d8659530b6ae` | **同左（逐字一致）** |
| `build/lib/libipc.so.1.3.0` | `81fe91ffa14ee00a9ebfeb8eee23cd1bfae71956545ee79e686919a8e6f8d4c9` | `558f47ede2419e14554766af55e53671e69d05d940b05351b1d4aa0e45d79031` |

原始记录：`artifacts/perf/20260929-r41-W10-R2b/fingerprint_before.txt`（同一文件内按 before/after 两段追加，符合只追加纪律）。

### 2.2 是否仍然干净应用 ⇒ **是，不需要重新生成**

```
$ patch -p0 --dry-run < .../t19_callsite_cc.patch
checking file src/dzIPC/shm_pub_sub_ipc.cc
Hunk #1 succeeded at 1079 (offset 464 lines).
Hunk #2 succeeded at 1203 (offset 464 lines).
Hunk #3 succeeded at 1239 (offset 464 lines).
$ patch -p0 --dry-run < .../t19_callsite_header.patch
checking file include/dzIPC/shm_pub_sub_ipc.h
Hunk #2 succeeded at 114 (offset 16 lines).     # Hunk #1 无 offset（上下文逐字命中）
Hunk #3 succeeded at 134 (offset 16 lines).
```

**为什么 offset 很大仍然可接受**：t6（W05 控制面接入）与 t7（W06 接收池接入）在 `.cc` 顶部一次性插入了约 464 行，三处调用点的**上下文（各 3 行）逐字未变** ⇒ `patch` 按上下文定位成功。**判断依据不是行号而是上下文**，故本包**未重新生成**补丁（重生成会把「t19 交付的补丁」与「t41 应用的补丁」变成两个不同文件，破坏可追溯性）。若 W05/W06 之后再改这三处上下文，补丁会**大声失败**（不是静默错位）—— 这是可接受的失败模式。

⇒ **"不需要重新生成"这一判断是可复核的**：任何人重跑上面两条 `--dry-run` 都能得到同样输出。

### 2.3 落点数

```
$ grep -c 'note_dzflat_attempt'        src/dzIPC/shm_pub_sub_ipc.cc      → 3
$ grep -c 'note_dzflat_borrow_failed'  include/dzIPC/shm_pub_sub_ipc.h   → 3
```

三处 `.cc` 落点：`publish_blocking`（A 侧回退点）、`publish_prebuilt_segment`（预构造段失败点）、`publish_for_sniffer`（sniffer 回退点）。三处 `.h` 落点：`publish_loaned` 的三个出口（对象无效 / `finalize()` 失败 / `publish_loan()` 失败）。

---

## 3. 同臂复跑：三语义可分（要求 2）

**探针**：`artifacts/perf/20260929-r41-W10-R2b/t41_semantics_arms.cpp`（手工编译，与库同一 `build/lib/libipc.so.3`）。
**同一二进制、同一库、同一进程模型**；逐臂 `CounterRegistry::instance().reset()` 后读同一组 ID。

### 3.1 读数（5 轮逐字一致，原始输出 `arms_x5.out.txt` / `arms_after.out.txt`）

| 臂 | 构造 | `tlv` | `a` | `b` | `fallback_total` | `path_selection_dzflat_disabled` | `fallback_type_incompatible` | `borrow_failed_reason_unknown` |
|---|---|---|---|---|---|---|---|---|
| **ARM1** | DZFlat **关**，标准消息 ×30 | 30 | 0 | 0 | **0** | **30** | 0 | 0 |
| **ARM2** | DZFlat **开** + `GenericMessage`（类型不支持）×20 | 20 | 0 | 0 | **20** | 0 | **20** | 0 |
| **ARM3** | DZFlat 开 + `loan<Flat>(256)` 却 `alloc_data(8192)`（超预算）×10 | 0 | 0 | 0 | **0** | 0 | 0 | **10** |
| **ARM4** | DZFlat 开 + 借样成功（对照）×10 | 0 | **10** | 0 | **0** | 0 | 0 | 0 |
| **ARM5** | DZFlat 开 + 同档借满 40 块不发布，再借 5 次（都失败） | 0 | 0 | 0 | **0** | 0 | 0 | **0**（见 §3.3 诚实标注） |

### 3.2 三组互斥的判据

**判据 (a) 路径选择不计入 fallback**：ARM1 中 `tlv_messages==30`（每条走了 TLV）、`fallback_total==0`、6 个 `fallback_*` 原因**全 0**，而 `path_selection_dzflat_disabled==30`。
⇒ 关掉 DZFlat 是**正常路径**，不是回退；它的条数单独成组，用于解释 `tlv_messages` 的来源构成。

**判据 (b) 真回退 total 与原因同时 +1**：ARM2 中 `fallback_total==20` **且** `fallback_type_incompatible==20` **且** `path_selection_*==0`。
⇒ 「DZFlat 开了、类型不支持」是一次**真实降级**（这是队长 D-16 的精确化：开关关时同一事实**不得**计入 fallback，见 ARM1 vs ARM2 的对照）。

**判据 (c) B 借样失败与 fallback_total 分列**：ARM3 中 `borrow_failed_reason_unknown==10` 而 `fallback_total==0`；ARM4（借样成功）中 `fallback_total==0` 且 `borrow_failed_*` 全 0。
⇒ ⛔ **两者不得相加**：`fallback/(dzflat+fallback)` 这个比值只能用 `fallback_total`，`borrow_failed_*` 属应用侧借样失败、与传输路径降级无关。

**端到端旁证（真实 A/B 工装，非本探针）**：应用本补丁后重跑 W08 三配置，`counters_publisher.json` 实测
tlv 两档 `path_selection_dzflat_disabled=120`（而 `fallback_total=0`）、A 两档 `dzflat_a_messages=120`、B 两档 `dzflat_b_messages=120`，且三档 `sel_off/sel_unsup/fb_total` 均符合 §3.1 的表。
⇒ 判据在**真实跨进程 A/B 链路上**同样成立，不只是本探针的合成场景。

### 3.3 ⚠️ 诚实标注：ARM5 暴露的未闭环缺口（W19-F2）

ARM5 借满 40 块后再借 5 次，**每次都失败**（`loan_invalid=5`），但 `borrow_failed_*` **全 0**。原因：
`shm_pub_ipc::loan<Flat>()` 在 `!lo.valid()` 时**直接 `return {}`，不经过任何分类器**；而应用侧"借样失败"要变成计数，必须由**应用**调 `publish_loaned()`（此时 `!lo.valid()` ⇒ 落 `borrow_failed_reason_unknown`）。

⇒ **本包不把它写成"已闭环"**：`borrow_failed_pool_exhausted`（预留 ID）与"loan 直接失败"这一格仍无人写。要精确区分「单生产端 flag 占用 / 无接收方 / 池耗尽」三态，须给 `ipc::loan()` 增加原因出口 —— **libipc 接口变更**，超出 t41 授权（登记为 **t41-F1**，与 t19 的 W19-F2 同一事项）。

> 这一格**不影响**本任务的验收：要求 (b)/(c) 的三组互斥判据已由 ARM1–ARM4 完整给出，ARM5 只是把**已知缺口**的边界测出来并如实登记。

---

## 4. 「仅计数器点、无行为变更」的机械证明（要求 3 的实质）

**不靠声称，靠判据。** 用反向应用补丁重建"接前"文件（`build/t41/before/`，其 sha256 与 §2.1 接前值**逐字一致**，证明重建可信），再做六条机械比对。完整输出：`artifacts/perf/20260929-r41-W10-R2b/no_behavior_change.txt`。

| 判据 | 内容 | 结果 |
|---|---|---|
| **A** | `return` 语句**多重集**逐字不变（返回值语义） | `.cc` 117/117 相等；`.h` 7/7 相等 ✅ |
| **B** | 新增/删除的 **51 行**逐行归类，白名单外必须为 0 | 注释/空行 12、结构花括号 8、计数器调用 9、栈上证据填充 22、**白名单外 0** ✅ |
| **C** | 新增 `if(!ok)` 分支体内无 `return`/`break`/`continue`/睡眠/加锁 | 2 个分支，体内越界语句 **0**，无一命中 ✅ |
| **D** | 既有控制流关键字计数变化 | `.cc` 仅 `if` +1、`.h` 仅 `if` +1（即 C 判据里那两个分支），其余全不变 ✅ |
| **E** | 新增点不引入等待/加锁/I/O | `CounterRegistry::inc` = 无锁原子自增，无分配、无日志、无系统调用；调用点位于**原有 `return` 之前** ✅ |
| **F** | 删除行内容清单 | **仅 4 行，全部是注释**（3 行 `.cc` 旧注释被改写、1 行 `.h` 注释被扩写），**无一行代码被删** ✅ |

**A–F 全绿** ⇒ 「仅计数器点、无行为变更」成立（沿用 EX-1 惯例）。

**编译面**：`cmake -S . -B build && make -C build -j16` **exit 0**；本次改动面 `grep -iE "warning|error"` 命中 **0** 条（无新增告警）。

### 4.1 `check_wiring.sh`（可复跑，5 组机械检查）

```
[1] 调用点落数        .cc note_dzflat_attempt=3 ✅   .h note_dzflat_borrow_failed=3 ✅
[2] 11 个 ID 可达     全部 ✅（分类器为唯一写入者）
[3] counters.h 未越界  sha256 与 t35 记录一致 ✅（零改动）
[4] t35 家族不重复计数 chunk_exhausted / chunk_alloc_failed / queue_evicted 在本改动面 0 处 ✅
[5] B 借样失败不进 fallback  note_dzflat_borrow_failed 函数体不含 fallback_total ✅
CHECK_WIRING: PASS
```

---

## 5. `registration_rejected` 产品语义裁定（任务 2）

### 5.1 裁定：**(ii) 已定义但当前架构无对应语义**

**理由（三条，均可复核）**：

1. **枚举已无剩余语义空间**：`RecvRegisterStatus` 有 8 个取值，其中除 `ok` 外的 **7 个全部**已有唯一具体归因（§5.2 机械表）。"rejected" 若定义为"注册被拒"，它与 `wait_set_full`/`busy`/`duplicate`/`stopped`/`invalid_token`/`invalid_route`/`backend_unavailable` 是**包含关系而非并列关系** ⇒ 会造成**双计数**。
2. **与 `wait_set_full` 的边界在语义上本就清晰，不需要新 ID 来区分**：
   `wait_set_full` = **容量满**（该 worker 的 127 token 用尽；W04 已实测第 128 个 ⇒ `wait_set_full` 且 owner 归还）；
   其余 6 个 = **前置条件不满足**（后端不可用 / 别人在收 / 自己重复注册 / 池已停 / token 无效 / route 空）。
   两者已分别由 `wait_set_full` 与 `registration_{duplicate,busy,stopped,invalid_token,invalid_route}` 承载 —— **「容量满 vs 策略拒绝」这个区分已经存在，只是不叫 rejected**。
3. **当前架构没有"策略层拒绝"这一概念**：全仓检索 `add_route` 的拒绝出口，**没有任何**基于配额/权限/白名单/优先级/配置的策略判定；所有拒绝都由容量或前置条件产生。若为此**新造**一个策略层，属产品功能新增，超出本任务授权与验收范围。

⇒ **`registration_rejected` 保持"已定义、零写入"不变**，并**明确登记**为「已定义但当前架构无对应语义」，供 R1 复核（不再作为"待接线"项挂在进度表上）。

### 5.2 机械覆盖表（证明"无剩余语义空间"，可复算）

提取方式：从 `include/dzIPC/threepools/recv_worker.h` 读枚举、从 `src/dzIPC/shm_pub_sub_ipc.cc` 读 `sub_status_reason_code()` 与 `fallback_to_compat()`，**不做人工映射**。原始输出 `artifacts/.../rejected_coverage.txt`。

| `RecvRegisterStatus` | `RecvPathReason` | 实际写入的计数 ID |
|---|---|---|
| `ok` | —（不映射） | —（成功，`registration_ok`） |
| `backend_unavailable` | `kBackendUnavailable` | `fallback_total`, `fallback_backend_unavailable` |
| `duplicate` | `kDuplicate` | `registration_duplicate` |
| `busy` | `kBusy` | `registration_busy` |
| `stopped` | `kStopped` | `registration_stopped` |
| `invalid_token` | `kInvalidToken` | `wait_token_invalid`, `registration_invalid_token` |
| `invalid_route` | `kInvalidRoute` | `registration_invalid_route` |
| `wait_set_full` | `kWaitSetFull` | `fallback_total`, `fallback_capacity_full`, `wait_set_full` |

**未映射到任何计数的取值：`['ok']`**（唯一一个，且它不进 `registration_failed`）。
⇒ 每个失败取值都有**唯一具体**归因；另由 `fallback_to_compat` 统一对**所有**非 ok 出口 +1 `registration_failed`，并由 `capacity_first_failed_resource_recorded` 置位"首个失败资源"。

**为什么不选 (i)（新增 `rejected` 值）**：需要改动 2 个公开头（`recv_worker.h` 的枚举、`shm_sub_seam.h` 的原因码）、24 个 `switch` case 站点（其中 76 处 `RecvPathReason::` 引用需同步），且新值与既有 7 值**语义重叠** ⇒ 用一个**含混的新口径**换取"把 `§13.3` 首项填上非零"。本包判定这**降低**证据质量，故明确裁定 (ii)。

**对 R1 的复核要点（本包不自审）**：
1. 重跑 `rejected_coverage.txt` 的生成脚本，核对 8 行表与"未映射 = ['ok']"；
2. 全仓 `grep -rn "registration_rejected" src/ include/` ⇒ 应只有 `counters.h` 的定义与元数据行（**零写入点**）；
3. 若 R1 认为需要"策略拒绝"这一产品概念，则它属**新功能**，应另立工作包（含产品需求与配额来源），**不应**由本 ID 的接线来充当。

---

## 6. 与 t35 清点的衔接：两个分类器让 **9 个** ID 从"零写入"变为"已接线"

**先给机械事实**（不采信转述）：从 t35 的只读清点表 `artifacts/perf/20260929-r33-W03-counterwiring/13_counter_inventory.tsv` 逐行提取本包涉及的 ID。

**由两个分类器可达的写入 ID 集合 = 12 个**（机械提取，见 §4.1 检查 [2]）。逐项与 t35 原判对照：

| # | ID | t35 原判（清点表逐字） | t41 后 | 归属族 |
|---|---|---|---|---|
| 1 | `fallback_oversized` | `table_only`（0 写入点） | **新接线** | 发布路径降级 |
| 2 | `fallback_type_incompatible` | `table_only` | **新接线** | 发布路径降级 |
| 3 | `fallback_reason_unknown` | `table_only` | **新接线** | 发布路径降级 |
| 4 | `path_selection_dzflat_disabled` | `table_only` | **新接线** | 路径选择（**非回退**） |
| 5 | `path_selection_type_unsupported` | `table_only` | **新接线** | 路径选择（**非回退**） |
| 6 | `borrow_failed_no_receiver` | `table_only` | **新接线** | 应用借样失败 |
| 7 | `borrow_failed_oversized` | `table_only` | **新接线** | 应用借样失败 |
| 8 | `borrow_failed_publish` | `table_only` | **新接线** | 应用借样失败 |
| 9 | `borrow_failed_reason_unknown` | `table_only` | **新接线** | 应用借样失败 |
| 10 | `fallback_total` | **`wired`**（`shm_pub_sub_ipc.cc:1881/1885/1889`，W06 接收路径） | 再增一族写入点（发布路径） | 两族共用 |
| 11 | `fallback_capacity_full` | **`wired`**（`:1890`，W06） | 同上 | 两族共用 |
| — | `dzflat_a_messages` | `wired`（W08） | 未改（分类器只读，不写） | 路径条数 |

⇒ **两个分类器关联的 ID 共 11 项**（与任务书"涉及 11 个 ID"逐数对上 = 上表第 1..11 行），其中：
· **9 项**从"零写入"变为"已接线"（t35 清点 `table_only 30 → 27` 中，本包贡献 **9** 项；另 2 项属 t35 自己接的四类）；
· **2 项**（`fallback_total` / `fallback_capacity_full`）**此前已有**写入点（W06 的接收路径注册/回退），本包为它们**增加了发布路径这一族**写入点。

### 6.1 ⚠️ 必须点明：`fallback_total` 现在有**两族**写入点（不是重复计数，但读法要变）

| 写入族 | 位置 | 语义 | 判据 |
|---|---|---|---|
| 接收路径（W06 既有） | `shm_pub_sub_ipc.cc:1911-1912` `kBackendUnavailable`、`:1915-1916` `kPoolStartFailed`、`:1919-1921` `kWaitSetFull` | 接收侧**无法走固定 worker**，回退兼容收包线程 | 与 `wait_set_full`/`fallback_backend_unavailable`/`fallback_capacity_full` 同时 +1 |
| 发布路径（本包新增） | `note_dzflat_attempt` 的 **3 个 `.cc` 调用点**（`publish_blocking` / `publish_prebuilt_segment` / `publish_for_sniffer`）；实际自增发生在分类器 `classify_dzflat_attempt` 的 `fallback` 分支里 | 发布侧**无法走 DZFlat**，回退 TLV | 与 `fallback_type_incompatible`/`fallback_oversized`/`fallback_reason_unknown` 同时 +1 |

⇒ 二者都是**真回退**（符合 `fallback_total` 的定义），**不构成重复计数**（同一次事件只被其中一族记一次：接收侧回退与发布侧回退是**不同事件**）。
⇒ 但**读报告时必须分族**：`fallback_total=20` 在 ARM2 里全部来自发布路径（接收侧那一族计数为 0，因为 ARM2 的订阅者正常接入了固定 worker）。W10/W11 若要归因到具体路径，须**同时**读配套的原因 ID（`fallback_type_incompatible` ⇒ 发布侧；`fallback_capacity_full`/`fallback_backend_unavailable` ⇒ 接收侧）。
⇒ 本包**未改动**接收路径那一族的任何调用点：机械核对 `.cc` 内 `CounterId::fallback_total` 的字面写入点仍**只有 3 处**（行 `1911/1915/1919`，均在 `fallback_to_compat` 内，属 W06 接收族）；`CounterId::fallback_capacity_full` 仍只有 **1 处**（行 `1920`）。
⇒ 发布族是**间接**写：`.cc` 的 3 个 `note_dzflat_attempt` 调用点 → 分类器 `classify_dzflat_attempt()` 的 `fallback` 分支 → `fallback_total` + 原因。
⚠️ 因此**`.h` 的 3 个 `note_dzflat_borrow_failed` 调用点不写 `fallback_total`**（属 (c) 借样族，§3.2 判据 (c) 已实测 `fallback_total==0`）—— 不要把"6 个新调用点"整体读成"6 处都会 +1 fallback_total"。

**仍无写入点**（如实保留，不合并、不猜值）：`fallback_pool_exhausted`、`borrow_failed_pool_exhausted`、`registration_rejected`（§5 已裁定不适用）。

## 7. 与 t35 家族的口径一致性（要求 4）

| 计数 | 唯一写入点 | 与本次改动的关系 |
|---|---|---|
| `chunk_exhausted` | `ipc.cpp:723`（`note_pool_exhausted`，唯一池穷尽出口） | 本次改动面 **0 处** ⇒ 无重复计数 |
| `chunk_alloc_failed` | `ipc.cpp:720`（`kind=loan`） | 同上 |
| `queue_evicted` | `circularqueue.h:132`（淘汰分支） | 同上 |

**两族同时在场的实测**（ARM5）：`chunk_alloc_failed=6`（池耗尽 ⇒ 大消息降级为 64B 分片的既有语义）而 `fallback_total=0`、`borrow_failed_*=0`。
⇒ `chunk_alloc_failed` 属**容量/池**族（t35），`fallback_*` 属**路径降级**族（t19），`borrow_failed_*` 属**应用借样**族（t19）—— 三族**各记各的、互不折算**，与 W09 §5.1 的分类一致。

**未重复计数的另一条机械证据**：本改动面**不含** `chunk_exhausted`/`chunk_alloc_failed`/`queue_evicted` 的任何字符串（检查 [4]）⇒ 结构上不可能重复。

### 7.1 端到端旁证：W08 三配置的 `counters_publisher.json`

| run | `tlv_messages` | `dzflat_a_messages` | `dzflat_b_messages` | `fallback_total` | `path_selection_dzflat_disabled` |
|---|---|---|---|---|---|
| `20260928-r11-W08-tlv-prebuilt` | 120 | 0 | 0 | **0** | **120** |
| `20260928-r12-W08-tlv-permsg` | 120 | 0 | 0 | **0** | **120** |
| `20260928-r13-W08-dzflat-a-prebuilt` | 0 | 120 | 0 | 0 | 0 |
| `20260928-r14-W08-dzflat-a-permsg` | 0 | 120 | 0 | 0 | 0 |
| `20260928-r15-W08-dzflat-b-prebuilt` | 0 | 0 | 120 | 0 | 0 |
| `20260928-r16-W08-dzflat-b-permsg` | 0 | 0 | 120 | 0 | 0 |

⇒ **tlv 档不再有歧义**：`fallback_total=0`（正确值：没有真回退）与 `path_selection_dzflat_disabled=120`（这 120 条都是"关掉 DZFlat"的正常路径）**同时可见、语义各自明确**；A/B 档路径条数仍可分。

> ⚠️ **证据完整性声明**：上表读自既有 `artifacts/perf/20260928-r1{1..6}-W08-*`（mtime 仍为 **20:34**，**未被本轮覆盖**）。本轮重跑 W08 时因设置了 `W08_ARTIFACT_ROOT=$PWD/artifacts/perf` 而产生了一个**多余的嵌套目录** `artifacts/perf/artifacts/perf/...`（工具在给定根下再拼 `artifacts/perf/<run_id>`）；该嵌套目录**已删除**，正确的 r11..r16 原目录**零改动**（上表即从原目录读取）。

---

## 8. 全量回归

| 命令 | 结果 |
|---|---|
| `cmake -S . -B build && make -C build -j16` | exit 0，0 error，改动面无新增告警 |
| `ctest -j4`（含 19 项 RUN_SERIAL） | **27/27 Passed, exit 0**（原始日志 `ctest_after.log`） |
| `test_w08_dzflat_ab`（三配置 A/B 分测） | 3/3 PASS（`FailedLoanIsReturnedAndApplicationFallbackStillDelivers` 含"失败 B 不得记成成功"断言，仍通过） |
| `test_dzflat_fallback_semantics`（t19 的 6 条分类器判据） | 6/6 PASS |
| `test_w03_measurement`（20 条，含计数存在性/分离性） | 20/20 PASS |
| 三语义同臂 ×5 轮 | 逐字一致（`arms_x5.out.txt`） |
| `check_wiring.sh` | `CHECK_WIRING: PASS` |

库指纹：`build/lib/libipc.so.1.3.0` = `558f47ede2419e14554766af55e53671e69d05d940b05351b1d4aa0e45d79031`。
（W00 §1.4 冻结的 `97504a74…`/`65608d92…` 均已失效；按 R0-A1，所有读者须**随库重编** —— 本 rebase 后指纹变化即属该纪律的适用场景。）

---

## 9. 读数纪律（写入交付，供 W10/W11/W12 引用）

```
读 counters.json 的三条规则（t19 §6 的延续，本包补充第 4 条）：
 1. fallback_total == 0 且 fallback_reason_unknown == 0
      ⇒ 该档**没有真回退**（不是"未接线"）。tlv 档（DZFlat 关）**就应该**是 0。
 2. fallback_total > 0 ⇒ 逐项读原因；若 fallback_reason_unknown > 0，报告**必须**
      写出"其中 N 次原因不可判定"，⛔ 不得摊到其它原因上、不得省略。
 3. borrow_failed_* > 0 ⇒ 应用侧借样失败，与 (b) 回退无关；⛔ **不得与 fallback_total 相加**。
 4. 【本包新增】loan<Flat>() 返回无效 而 borrow_failed_* == 0
      ⇒ **不是"没有借样失败"**，而是"该失败发生在应用拿到无效对象那一刻，
        产品路径不在此处分类"（t41-F1/W19-F2 未闭环）。
        统计"借样失败率"必须用**应用侧**的 loan().valid() 观测，不能只看 counters.json。
```

---

## 10. 残余与回滚

| # | 严重度 | 项 | 处置 |
|---|---|---|---|
| t41-F1 | medium | `borrow_failed_pool_exhausted` 仍无写入点；`shm_pub_ipc::loan<Flat>()` 的失败不经分类器（ARM5 实测 `loan_invalid=5`、`borrow_failed_*=0`） | 需给 `ipc::loan()` 加原因出口 = **libipc 接口变更**，超出本任务授权；与 W19-F2 同一事项，建议另立工作包。⛔ 本包未猜值填充 |
| t41-F2 | low | `fallback_pool_exhausted` 仍无写入点（t35 清点 `table_only`） | 需与 t41-F1 同批处理（同属"libipc 池失败原因出口"这一接口变更）。**已核实**：`fallback_backend_unavailable` 与 `fallback_capacity_full` **有**写入点（`shm_pub_sub_ipc.cc:1912/1916/1921`，W06 接收路径族）—— 本行原先把前者误列入"无写入"，已按 t35 清点表逐字更正 |
| t41-F3 | info | `registration_rejected` 保持零写入（§5 裁定 (ii)） | 已明确登记「当前架构无对应语义」；若 R1 认定需要"策略拒绝"，属新功能、另立工作包 |
| t41-F4 | info | `src/dzIPC/shm_pub_sub_ipc.cc.orig`（mtime 9-29 18:11）为**他人工具留下**的中间文件，非本包产物 | 本包**未删除**（不属本人写入权）；建议由该文件所有者清理，避免 `file(GLOB)` 误纳入 |

**回滚**（本包改动极小且完全可逆）：
```
patch -R -p0 < docs/.../W09/证据/t19_callsite_cc.patch
patch -R -p0 < docs/.../W09/证据/t19_callsite_header.patch
```
反向应用后 `shm_pub_sub_ipc.{cc,h}` 的 sha256 应回到 §2.1 的"接前"值（本包已在 `build/t41/before/` 实测复现：反向应用后 sha256 = `12131d8c…` / `b12be18e…`，与接前**逐字一致**）。
⇒ 回滚后 11 个 ID 重新变为零写入，**不触碰任何行为**（因为一开始就没有行为改动）。
