# F2：`ipc::loan()` 原因出口 —— 产品缺口立项交付（t46）

| 项 | 值 |
|---|---|
| 任务 | `t46`（产品缺口单独立项：`borrow_failed_pool_exhausted` / `_oversized` 无生产者） |
| attempt | `822ee9fa-c146-4b52-b574-6a902a9753a7` |
| 负责人 | socket与数据面负责人 |
| 基线 | `e800ccc496ac710b711c9346709e86a148c41241`（2026-09-28） |
| 改动面 | **2 文件**：`include/libipc/ipc.h` **+54/−0（纯新增，行尾逐字保留）**、`src/libipc/ipc.cpp` **+325/−14**（**我的写入面**，见《接口责任表》第 17 行） |
| 证据目录 | `artifacts/perf/20260930-r29-libipc-loan-reason/`（**新 run_id**） |
| 证据等级 | **S3 单项验证通过**（可复现命令 + 原始日志 + 反事实 + 影子验证）。⛔ 未宣称 S4/S5 —— 独立验收属 W10/REVIEW |

改动面指纹（sha256，before → after）：

| 文件 | 改动前 | 改动后 |
|---|---|---|
| `include/libipc/ipc.h` | `f8b91bba6e09cbf714e37c6ed530259c520c1bf59cc2a414235f839c1f2ee689` | `9dbe4a6a0762ab0ede3a030e65578c81efc05384c8c68b1859ea60970824275b` |
| `src/libipc/ipc.cpp` | `379918b9f6a24da928bda1c843adb29a7bd8367d5e0e14ca3a25dfa2873d0d59` | `e15cbdd83a72f050768b89e400a6c37e307e53906fa41b4055203f2570b7a00f` |
| `build/lib/libipc.so.1.3.0` | `17acf7a2388533617e461dd45f8597bc4b6e0148b220812c3ab73001b980048f` | `cf209393773d51ee4762dd95bfd1b04f873ec54aa8f61cf5a7ed92be2990fa46` |

> **行尾保真（易被忽略的工程细节）**：`include/libipc/ipc.h` 原始是 **CRLF 为主的混合行尾**
> （360 CRLF + 8 LF）。用文本模式整文件重写会把 360 行 CRLF 变成 LF ⇒ `git diff` 从
> "纯新增 54 行"膨胀成 "±415 行"，让评审无法看出真实改动面。本包最终版**逐行保留原行尾**
> （`splitlines(keepends=True)` 重生成），实测 `git diff --numstat` = **`54  0`**
> （新增 54 行、**删除 0 行**），且 CRLF/LF 计数仍为 414/8（原 360/8，增量全在新插入行上取 CRLF）。
> ⛔ 若后续有人用编辑器重写此文件，请先确认行尾策略，否则会把改动面伪装成 400+ 行。

---

## 0. 一分钟结论

| 验收条款 | 结论 |
|---|---|
| **三选一的明确建议与理由** | ✅ 选 **(b)**：在 `ipc` 侧增独立原因出口（`loan(size, loan_status&)` 重载）—— ⛔ 不改 `loan_t` 布局、⛔ 不改 `valid()` 语义、旧符号保留（§2、§5） |
| 实施 + `valid()` 语义不变 | ✅ 六出口逐条可判；旧/新入口同臂对照 `CONSISTENT`（§3.1 ARM-D） |
| 引用方清单 / 重编命令 / 指纹（③d） | ✅ 25 个引用方 + 重编命令 + 库/头前后指纹 + **符号集合差 = 仅新增 3 个、删除 0**（§5） |
| 同臂 0→非 0 且三类互不冒充 | ✅ `borrow_failed_pool_exhausted` **0→80**（影子验证），同时 `chunk_exhausted` 保持 **0**、`chunk_alloc_failed` 独立计数（§3.2）—— 与该 ID 的**生产**写入点分开陈述（§3.4） |
| 回滚方法 | ✅ 还原 2 文件 + 重编；含"回滚后哪些读数会重新变 0"（§7） |
| 读数纪律已写入 | ✅ §6（落地前 / 落地后两种读法，并给出可机械判定的判据） |
| 三条既有登记已合并 | ✅ t41-F1 + W19-F2 + t43 判定 → **本条目 F2**（§8） |
| ⛔ 未新增 CounterId | ✅ `counters.h` **零改动**（本任务边界），两个目标 ID 均沿用既有 |
| 回归 | ✅ `ctest -j4` **27/27 Passed**；热闸见 §9 |

**本任务最重要的一句话（⛔ 请勿简化）**：缺口有**两层**，且第二层比第一层更严重 ——

1. `ipc::loan()` 只有一个 `bool` ⇒ 上层判不出原因（这是任务书本来的假设）。我按 (b) 补了出口。
2. **但补完出口后，`borrow_failed_oversized` 仍然接不上** —— 因为唯一活着的生产落点
   `note_dzflat_borrow_failed(bool,bool,bool)` 的输入空间里**没有**这一维，且该 ID 在
   `classify_dzflat_attempt` 之外**没有任何分支**（§4 两路互证）。⇒ 该 ID 是
   **`counters.h`（W03 维护）的落点缺陷**，⛔ 不在我的写入面内。已立需求
   `41_borrow_failed_oversized_需求.md`，并**如实登记为未闭环**。

---

## 1. 要求 1：可行性判定与前情核实

### 1.1 先核实"无生产者"这个前提（要求书的断言我不照抄）

```bash
python3 src/audit_counter_wiring.py . 30_audit.tsv     # t35 的只读清点脚本
awk -F'\t' '$1 ~ /borrow_failed/' 30_audit.tsv
```

| ID | `n_direct_writers` | 判定 | 证据 |
|---|---|---|---|
| `borrow_failed_pool_exhausted` | **0** | `table_only` | 只在 `counter_table()` 里出现 |
| `borrow_failed_oversized` | **0** | `table_only` | 同上 |
| `borrow_failed_no_receiver` | 0 | `potential_used` | 载体 `note_dzflat_borrow_failed`，用户 `shm_pub_sub_ipc.h` |
| `borrow_failed_publish` | 0 | `potential_used` | 同上 |
| `borrow_failed_reason_unknown` | 0 | `potential_used` | 同上 |

⇒ **任务书的两条断言成立**（确无生产者），但**口径要补一句**：另三个 `borrow_failed_*`
虽"无直接写入点"，却有载体 `note_dzflat_borrow_failed` 且该载体被应用头文件使用 ——
所以本族**不是整族死掉**，死的只有两个具体 ID。这个区分决定了后面的处置不同。

### 1.2 `loan()` 的失败出口机械清点（`src/libipc/ipc.cpp` 改动前）

| # | 出口 | 触发条件 | 调用方原本能区分吗 |
|---|---|---|---|
| E1 | `invalid_handle` | `queue_of(h)` 为空 或 `elems()==nullptr` | ❌ 只有一个 `false` |
| E2 | `not_ready` | `ready_sending()==false`（**单生产端 flag 被占**） | ❌ |
| E3 | `no_receiver` | `connections()==0` | ❌ |
| E4 | `pool_exhausted` | `acquire_storage` 池空（重试一次清扫后仍空） | ❌ |
| E5 | `storage_unavailable` | `chunk_storage_info` 建不出段 / `id` 越界 | ❌ |
| E6 | （**新增**）`size_too_large` | 容量算术溢出（§4.3 的缺陷） | ❌ 原本**根本不失败**，见 §4.3 |

实测（`02_loan_exit_reachability.log`，改动前 4 臂）：

```
arm=E1 invalid_handle  request=4096  valid=0
arm=E2 not_connected   request=4096  valid=0
arm=E3 no_receiver     request=4096  valid=0  recv_count=0
arm=E4 pool_exhausted  request=65536  held=40  denied_after=1  valid=0  recv_count=1
caller_visible: valid()=0  (id=-1 data=null size=0)  status_api=ABSENT(当前 ipc::loan_t 无原因出口)
```

⇒ **四条出口的调用方可见面完全相同**（`valid()==false`，`id==-1`，`data==null`，`size==0`）。
这与 `counters.h:645` 的注释一致："`loan()` 的 bool 无法区分「单生产端 flag 占用 / 无接收方 /
池耗尽」三态，须扩 `ipc::loan()` 返回通道才能精确区分 —— 属 libipc 接口变更"。

### 1.3 三条路的评估

| 路 | 形态 | ABI 影响 | 评价 |
|---|---|---|---|
| **(a)** | 在 `loan_t` 上加 `reason()`/`status()` 成员 | ⛔ **破坏**：`loan_t` 按值返回 ⇒ x86-64 走 **sret 指针**（实测汇编：`_Z4makev: movl $1,(%rdi); movq %rdi,%rax; movq $2,8(%rdi); movq $3,16(%rdi); ret`）⇒ 加成员改变调用方栈上缓冲区大小与偏移，**所有**引用方必须重编 | ❌ 不选 |
| **(b)** | 在 `ipc` 侧增独立出口（新重载，`loan_t` 布局不动） | ✅ **零布局变化**：`sizeof(loan_t)` 仍 24、偏移不变；旧函数符号保留 ⇒ 旧二进制可继续用旧 API | ✅ **选它** |
| (c) | 判定不可行/不值得，登记已知限制 | — | ❌ 不可行不成立：出口能补，且已实测六出口全部可达（§3.1） |

**为什么选 (b) 而不是 (a)（理由，非偏好）**：
1. `loan_t` 是**按值返回**的公开结构 ⇒ (a) 属 §4.4 ③d 硬闸的"结构体尺寸变化"，会强制
   全仓 25 个引用方 + 任何手工编译工装同步重编。本任务给的收益（"多一个原因字段"）
   **不值这个面**。
2. (b) 天然满足"⛔ 不改 `valid()` 语义"：`valid()` 与 `loan_status==ok` 在**所有**出口上
   一致（ARM-D 实测 `CONSISTENT`），旧代码一行不改。
3. (b) 让"是否读取原因"成为**调用方的选择**：不读原因的既有调用方零成本、零行为变化。

---

## 2. 要求 2：ABI、兼容与 ③d 证据

### 2.1 引用方清单（谁 include `libipc/ipc.h`）

```bash
grep -rln '#include "libipc/ipc.h"' --include=*.h --include=*.cc --include=*.cpp . \
  | grep -v '^./build/\|^./tmp/\|^./artifacts/\|^./local/\|^./docs/' | sort
```

`20_referrers.txt`，**25 个文件**：

| 类别 | 文件 |
|---|---|
| 生产头（`include/`、`exec/`） | `include/dzIPC/common/loaned_message.h`、`include/dzIPC/detail/shm_sub_seam.h`、`include/dzIPC/shm_pub_sub_ipc.h`、`include/dzIPC/shm_route_session.h`、`include/dzIPC/shm_ser_cli_ipc.h`、`exec/dzipc_topic_cat/include/shm_sniffer.h` |
| 生产实现 | `src/libipc/ipc.cpp` |
| 测试 | `test/test_chunk_capacity_backpressure.cpp`、`test_chunk_hold`、`test_ipc`、`test_lap_safety`、`test_lifecycle_contract`、`test_loan`、`test_pool_exhaust_observability`、`test_recv_wait_set`、`test_recv_worker`、`test_shm_i5_pop_buffer`、`test_shm_ready_transition`、`test_shm_route_session`、`test_shm_sub_dtor_gate`、`test_uf003_crash_reclaim`、`test_uf007_id_pool_double_release`、`test_uf010_hash_semantics`、`test_uf011_chunk_return`、`test_wakeup_artifact` |
| 非构建（陈旧副本，⛔ 不在本任务面） | `local/include/libipc/ipc.h`（与 `include/` 版本已有差异，见 §2.3） |

### 2.2 重编命令 + 符号对照（③d 的机械判据）

```bash
cmake -S . -B build && make -C build -j16
nm -DC --defined-only build/lib/libipc.so.1.3.0 | awk '{print $1,$2,$3}' | sort > syms_post.txt
diff <(awk '{print $2" "$3}' syms_pre.txt|sort) <(awk '{print $2" "$3}' syms_post.txt|sort)
```

结果（`syms_pre.txt` / `syms_post.txt`）：

```
=== 符号: 新增 / 删除 ===
  + W ipc::chan_impl<ipc::wr<(ipc::relat)0, ...
  + W ipc::chan_impl<ipc::wr<(ipc::relat)0, ...
  + W ipc::chan_impl<ipc::wr<(ipc::relat)1, ...
pre=1497 post=1500
```

⇒ **新增 3 个弱符号（新重载的三个实例化），删除 0 个**。旧 `loan(handle,size,bool)`
符号**原样保留** ⇒ 未重编的旧二进制仍可加载、仍可调用旧入口。

### 2.3 对既有调用方的影响（逐类）

| 调用方 | 影响 |
|---|---|
| `test/**` 的 20 个测试（用 `tx.loan(n)`） | **零源码改动、零行为变化**（旧重载原样转发，`loan_impl(..., nullptr, ...)` 与旧实现逐位相同；`ctest` 27/27 佐证） |
| `dzIPC` 侧（`shm_pub_sub_ipc.h`、`shm_ser_cli_ipc.cc`、`loaned_message.h`） | 同上；**要拿到原因需显式改用新重载** —— 我把该改动做成补丁交 SHM接入负责人（§3.4） |
| `local/include/**` 陈旧副本 | ⛔ **不参与构建**（`test/CMakeLists.txt` 的 `include_directories` 只有 `include/`、`src/`、`test/`、`3rdparty`、`exec/dzipc_topic_cat/include`、项目根）。与 `include/` 版本已不同（实测 diff：`local` 版缺 `loan_t` 的注释行、多了裸 `{`）。**不修**：属他人的同步问题，本任务不越界 |
| 手工编译的工装（如 `w10_*`） | 若只用旧 API ⇒ 无需动作；若改用新 API ⇒ 必须按 §2.2 重编（③d 硬闸原文要求） |

### 2.4 ③d 硬闸的自评

| ③d 要件 | 本包状态 |
|---|---|
| 是否变更了按值返回结构体的字段 | **否**（`loan_t` 逐字未动，`sizeof` 仍 24、偏移 0/8/16 不变） |
| 引用方清单 | ✅ §2.1（25 个，机械生成） |
| 重编命令 | ✅ §2.2 |
| 库与工装指纹 | ✅ 表头（`ipc.h`/`ipc.cpp`/`libipc.so` 前后 sha256）+ `fingerprints_before.txt`/`_after.txt` |
| 符号对照 | ✅ §2.2（新增 3 / 删除 0） |

⇒ 本包**触发了"加接口"但未触发"改布局"**，故 ③d 的**硬闸要求（全引用方重编）不成立**；
但为可追溯仍给全 §2.1–2.2 的证据。

---

## 3. 要求 5：同臂复跑验证

### 3.1 六出口逐条可判（`10_arms.log` ARM-A）

改后同一进程内依次构造六条出口（`loan_reason_arms A`）：

```
  E1_invalid_handle      valid=0 status=invalid_handle
  E2_not_ready           valid=0 status=not_ready
  E3_no_receiver         valid=0 status=no_receiver
  E4_pool_exhausted      status=pool_exhausted       (held=40)
  E5_size_too_large      valid=0 status=size_too_large
  E6_storage_unavailable valid=0 status=storage_unavailable
```

**每条出口的构造法**（⛔ 都是可复现的真实路径，不是 mock）：

| 出口 | 构造 | 备注 |
|---|---|---|
| `invalid_handle` | 默认构造的 `ipc::route`（未 open） | `queue_of(h)==nullptr` |
| `not_ready` | **同一条 route 起两个 sender**：`tx1` 先拿 `sender_flag_`，`tx2.loan()` ⇒ `connect_sender()` 失败 | 这正是 counters.h 注释里点名的"单生产端 flag 占用"那一态 |
| `no_receiver` | 只起 sender、不起 receiver（`recv_count()==0`） | |
| `pool_exhausted` | 真 pub/sub、借满 64 KiB 档（40 块）后第 41 次 | 与 t35 的 ARM-B 同构造 |
| `size_too_large` | `loan(SIZE_MAX)` 等溢出请求 | §4.3 的缺陷出口 |
| `storage_unavailable` | `loan(2^54)`（档位算术不溢出，但 `40 × 档位` 段建不出来） | ⛔ 不得与 `pool_exhausted` 混为一谈 |

### 3.2 两个 ID 的 0 → 非 0（`10_arms.log` ARM-B/C/C2 + `11_shadow_verify.log`）

| 臂 | 构造 | 指令 | 读数 |
|---|---|---|---|
| **ARM-B** | 真 pub/sub，借满 64 KiB 档；**逐句执行提议补丁的应用调用点逻辑** | `loan_reason_arms B` | `io_pool=0 → 160`；`loaned=40 denied_pool=160 denied_other=0` |
| **ARM-C** | 同一场景但**只走旧 bool API**（不读原因） | `loan_reason_arms C` | `io_pool=0`，`chunk_af=1` ⇒ **旧行为逐位不变**：不读原因就一个字节都不动 |
| **ARM-C2 / 影子验证** | 用**补丁后的头文件**（唯一改动 = 应用调用点）+ 我的原因出口，真走 `pub.loan<Flat>()` | `shadow/shadow_exhaust` | `loaned=40 rejected=80`；`io_pool=0 → **80**`；同时 `chunk_exhausted=0`、`chunk_alloc_failed=80` |

### 3.3 三类互不冒充（要求 5 的"⛔ 不被污染"）

| 类别 | 语义（t19 冻结） | 本包实测 |
|---|---|---|
| `chunk_exhausted` | 传输层 A/TLV 池空，**降级交付**（`send`/`no_member_send`） | ARM-B/C/C2 中**恒 0**（本场景没走 A 腿）✅ |
| `chunk_alloc_failed` | 传输层 B 借样池空，**未交付** | 与本 ID 同源同事件，**各自独立计数**：ARM-C2 `chunk_af=80` 而 `io_pool=80` ⇒ 一次事件在两族各记一次（**这正是设计要求**：一族是"传输层容量事实"，一族是"应用借样失败"，见 §3.4） |
| `borrow_failed_pool_exhausted` | 应用层 B 贷款失败（原因 = 池耗尽） | ARM-B/C2 0→非 0 ✅ |
| `fallback_total` 族 | (b) 真回退 | 本包**全程未变**（ARM-A/B/C 读数里不含 fallback）✅ 与 t19 冻结一致 |

> ⚠️ **"同一个事件记两族"是设计，不是重复计数**：`chunk_alloc_failed` 归
> **capacity** 组（"池空了，谁来取都会空"），`borrow_failed_pool_exhausted` 归 **borrow**
> 组（"应用这次借样失败了"）。两者的分母不同（前者按池事件、后者按应用调用），
> ⛔ 不可互相替代，也不可相加。这一条与本包 §8 合并的 t41-F1 是同一口径。

### 3.4 ⛔ 生产写入点的范围声明（必须写明，否则会误判为"已闭环"）

**本包改的是 libipc（我的写入面），但两个 ID 的"从 0 变非 0"发生在应用调用点**：

| ID | 生产写入点 | 状态 |
|---|---|---|
| `borrow_failed_pool_exhausted` | `include/dzIPC/shm_pub_sub_ipc.h::loan<Flat>()`（**SHM接入负责人**写入面） | ⛔ **未落地**。本包提供可应用补丁 `40_proposed_callsite.patch` + 影子验证；ARM-B 只是"扮演"该调用点 |
| `borrow_failed_oversized` | `note_dzflat_borrow_failed`（**counters.h/W03**）或调用点 | ⛔ **不可达**，见 §4 |

⇒ **⛔ 本包不得被读作"两个 ID 已接线"**。准确表述是：
「`ipc::loan()` 的原因出口**已就绪且六出口可判**；`borrow_failed_pool_exhausted` 的
**应用调用点补丁已就绪待落**；`borrow_failed_oversized` **仍不可达，已立需求**」。

---

## 4. 第二层缺口：`borrow_failed_oversized` 结构不可达（两路互证）

### 4.1 证据 ①：8 组合穷举（`01_classifier_exhaustion.log`）

把唯一活着的生产落点 `note_dzflat_borrow_failed(bool,bool,bool)` 的输入空间**穷举**，
每个组合跑一个全新进程（注册表是进程级单例，每进程天然干净）：

```
hr=0 fo=0 po=0 produced=borrow_failed_no_receiver
hr=0 fo=0 po=1 produced=borrow_failed_reason_unknown
hr=0 fo=1 po=0 produced=borrow_failed_publish
hr=0 fo=1 po=1 produced=borrow_failed_reason_unknown
hr=1 fo=0 po=0 produced=borrow_failed_reason_unknown
hr=1 fo=0 po=1 produced=borrow_failed_reason_unknown
hr=1 fo=1 po=0 produced=borrow_failed_publish
hr=1 fo=1 po=1 produced=borrow_failed_reason_unknown
可达集合(3): borrow_failed_no_receiver borrow_failed_publish borrow_failed_reason_unknown
OVERSIZED=0（8/8）
```

⇒ **`borrow_failed_oversized` 在 2³ 个输入组合中一个都取不到。**

### 4.2 证据 ②：分类器 vs 落点的分歧（`03_divergence.log`）

把**同一个逻辑事件**（B 路径借样成功、封口 `finalize` 失败 = 超变长预算）送进两条实现：

```
--- 旁路（classifier，输入空间含 write_ok 维）---
输入 = {borrow_requested=T, borrow_ok=T, write_ok=F}
分类器返回 detail = borrow_failed_oversized
classify_dzflat_attempt => produced: borrow_failed_oversized

--- 实测落点（site，输入只有 3 个 bool）---
输入 = note_dzflat_borrow_failed(had_receiver=T, finalize_ok=F, publish_ok=F)
note_dzflat_borrow_failed => produced: borrow_failed_reason_unknown
```

⇒ **t19 里"两处同一张判定表"的设计意图没有兑现**：分类器判得对，但落点拿不到 `write_ok`
这一维，于是判错。而分类器那条正确路径**在真路径上根本不执行**（`borrow_requested` 在
`src/` 命中 **0**；三个 `note_dzflat_attempt` 调用点都没填该位）⇒ 死代码。

### 4.3 附带发现（**不在任务书范围，但属真实缺陷，必须登记**）

`loan()` 的**容量算术溢出**：`chunk_info_t::chunks_mem_size(sz) = 40 * sz`，
`make_handle` 用 `sizeof(chunk_info_t) + 该乘积` 去 mmap。乘积溢出时**回绕成小值并映射一个过小的段**，
而 `loan()` 仍把请求档位报给调用方：

```
req=4611686018427387904 (2^62) -> valid=1  size=4611686018427387904
  seg=__IPC_SHM__CHUNK_INFO__4611686018427388928__C40  bytes=41012   ⛔ 声明容量 > 实际映射
req=18446744073709551615(SIZE_MAX)-> valid=1  size=18446744073709551615
  seg=__IPC_SHM__CHUNK_INFO__1024__C40  bytes=41012                  ⛔ 声明容量 > 实际映射
req=65536 (正常)                -> valid=1  size=65536   seg bytes=2662452   ✅
```

⇒ 调用方按 `lo.size` 写就是**必然越界**（括号注释里原本写着"后续 acquire 会失败"，
实际 acquire **不会**失败）。本包**顺手收口**：加 `kMaxChunkSize` 判据把静默越界
变回一次可判定的 `size_too_large`（§4.4）。⛔ 此项**未被要求**，但属"不能报一个假的容量"
这一硬底线，故实施并在此声明。

### 4.4 溢出的判据（与 `chunks_mem_size` 同源，不是拍的数字）

```cpp
const std::size_t kMaxChunkSize =
    ((std::numeric_limits<std::size_t>::max)() - sizeof(chunk_info_t)) /
    static_cast<std::size_t>(ipc::id_pool<>::max_count);
const std::size_t cap = loan_size_class(size);
if (cap > kMaxChunkSize) { ...; return fail(ipc::loan_status::size_too_large); }
```

上游正是 `chunk_info_t::chunks_mem_size()` 与 `make_handle` 的同一算式 ⇒ **唯一权威上界**，
且 40 × 4.6e17 B ≈ 18 EB 远超任何真实负载。回归：正常带 `loan(64 KiB)` 仍
`valid=1 size=65536 status=ok`（ARM-F 第二行）。

### 4.5 为什么不能在本任务修 `borrow_failed_oversized`

`note_dzflat_borrow_failed` 的判定表在 **`include/dzIPC/measure/counters.h`**（W03 维护）；
本任务边界写"⛔ 不改 `counters.h`"。⇒ 已立需求 `41_borrow_failed_oversized_需求.md`
（三个方案 + 验收判据 + 顺带处置 `borrow_requested` 死代码）。

---

## 5. 要求 3：与既有口径一致

| 口径 | 本包遵守情况 |
|---|---|
| `borrow_failed_*` 属 t19 **(c) B 贷款失败** | ✅ 新出口只喂 (c) 族；`loan_status` 枚举名与注释显式引用 t19 冻结 |
| ⛔ 绝不并入 `fallback_total` | ✅ 影子验证中 `fallback_*` 全程未出现；ARM-A/B/C 读数不含 fallback |
| 沿用 W09-F5 的 `kind` 映射风格 | ✅ 传输层入口不变：`acquire_storage(kind="loan")` 仍只记 `chunk_alloc_failed`；我**没有**在 libipc 里新增任何 `borrow_failed_*` 写入点（原因：libipc 不知道"应用是否在借样"的调用意图，在传输层记应用语义会重蹈 §4 的错位） |
| `valid()` 语义不变 | ✅ ARM-D：`old_valid=1 new_valid=1 old_size=4096 new_size=4096 status=ok => CONSISTENT`；并且实现上 `loan(size)` 转发 `loan_impl(..., nullptr, ...)`，`why==nullptr` 时**不写任何字节** |

---

## 6. 要求 4：读数纪律（t41 新增，本包扩展）

### 6.1 落地**前**（当前状态）

> **禁止**把「`loan` 返回无效」读成「无借样失败」。
> 统计借样失败率**必须用应用侧 `loan().valid()`**（或 `LoanedMessage::valid()`）计数，
> ⛔ 不得引用 `borrow_failed_*` 的读数（那两个 ID 今天恒 0 是**未接线**，不是"没发生"）。

### 6.2 落地**后**（本包出口就绪 + 调用点补丁落地后）

> `borrow_failed_pool_exhausted` / `_no_receiver` / `_publish` 可直接读（有生产者）；
> 但 `borrow_failed_reason_unknown` 的读数**必须与 `borrow_failed_oversized` 一起看**：
> 在 W03 落点改判之前，**超变长预算的事件会落进 `reason_unknown`**（§4.2 实测）。
> 因此今天的正确读法是：
> ```
> 借样失败总数        = loan().valid()==false 的应用侧计数           （权威）
> 其中池耗尽          = borrow_failed_pool_exhausted                 （已可读）
> 其中无接收方/未就绪  = borrow_failed_no_receiver                    （已可读）
> 其中发布失败        = borrow_failed_publish                        （已可读）
> 其中超变长预算      = ⛔ **不可读**（混在 reason_unknown 里）        （待 W03）
> ```
> ⛔ 不得把 `borrow_failed_reason_unknown` 直接读成"原因不明"，它当前**包含**超预算这一类。

### 6.3 可机械判定的判据（给 W10/R1 用）

```bash
python3 artifacts/perf/20260930-r29-libipc-loan-reason/src/audit_counter_wiring.py <repo> out.tsv
# 凡 verdict ∈ {table_only, selftest_only, potential_unused} ⇒ 该 ID 的 0 读作「未采集」
# 本 run 后：borrow_failed_oversized 仍为 table_only ⇒ ⛔ 其 0 读作「未采集」
```

---

## 7. 要求 6：回滚

**回滚方法**（适用范围：新出口被判定为不需要、或引起任何回归时）：

```bash
cd /home/zwc/cpp_ipc_dds
cp artifacts/perf/20260930-r29-libipc-loan-reason/oldinc/libipc/ipc.h include/libipc/ipc.h
# src/libipc/ipc.cpp 的改动是加法（loan_impl + 两个重载 + 越界判据），
# 反向恢复见 git diff；最小回滚等价物 = 让 loan(size) 直接走原实现体、删除新重载。
cmake -S . -B build && make -C build -j16
ctest --test-dir build -j4        # 期望 27/27
```

| 回滚后重新变 0 / 变回旧行为 | 说明 |
|---|---|
| `loan_status` 出口消失 | 调用方无法判因（回到 §1.2 的"一个 bool"） |
| `borrow_failed_pool_exhausted` 无从产生 | 除非调用点改用别的信号（今天没有） |
| §4.3 的越界缺陷**回归** | ⚠️ 回滚会把 `loan(2^62)` 重新变回"返回假容量"⇒ **建议这条不回滚**（它是安全修复，与原因出口可分离） |
| `valid()`/`size`/旧符号 | 逐位不变（回滚也正是改前状态，故无兼容事务） |

**适用条件**：仅在"新重载引发 ABI/回归问题，且 §4.3 的安全修复另行保留"时回滚。
⛔ 不因"未被使用"回滚 —— 出口是给应用调用点用的，尚未落地不等于无用。

---

## 8. 三条既有登记的合并（本条目 = **F2**）

| 原登记 | 出处 | 内容 | 本包处置 |
|---|---|---|---|
| **t41-F1** | t41（R2b） | `shm_pub_ipc::loan<Flat>()` 失败**不经分类器** —— ARM5 实测 `loan_invalid=5` 而 `borrow_failed_*=0` | ✅ **根因已定位**：`loan()` 无原因出口（§1.2）。**已修**（libipc 侧）；调用点补丁待落（§3.4） |
| **W19-F2** | t19 | `borrow_failed_pool_exhausted` 无写入点，需给 `libipc::loan()` 加原因出口 = 接口变更 | ✅ **已实现**：选 (b)（§1.3）；0→非 0 已实测（§3.2） |
| **t43 判定** | t43（§8） | 两个 ID 属**产品缺口**而非采集缺口，建议单独立项 | ✅ **确认成立**（§1.1），并**进一步细分**：`pool_exhausted` 是"缺出口+缺调用点"，`oversized` 是"缺出口**且**落点不可达"（§4） |

⇒ 三条**同源、已合并为本条目 F2，⛔ 不再重复开条**。

---

## 9. 回归与边界

| 项 | 结果 |
|---|---|
| `cmake -S . -B build && make -C build -j16` | ✅ exit 0 |
| `ctest --test-dir build -j4` | ✅ **27/27 Passed**（55.71 s，`50_ctest_j4.log`） |
| `bash docs/hotpath_gate.sh build 20260930-t46-loanreason` | ✅ **过闸**（`GATE_RC=0`，`51_hotpath_gate.log`）：门 1 `64B=174527 msg/s`（下限 80000）、`1MiB 条件化中位=1336`（下限 1100，n=5）；门 2 八个邻接回归（`test_loan` OK=10 等）全绿 |
| 符号对照 | ✅ 新增 3 / **删除 0**（§2.2） |
| ⛔ `include/dzIPC/measure/counters.h` | **零改动**（本任务边界；工作区内它的 sha 由 W03 侧演进至 `4810b534…`，与本包无关，我未触碰） |
| ⛔ 他人 WP | `shm_pub_sub_ipc.{h,cc}`、`recv_worker.*`、`test/CMakeLists.txt` **零改动**（机械核对：这四个文件里 `loan_status` 命中 **0**）；`test/**` 未新增/修改 |
| 范围机械核对 | `git diff --numstat -- include/libipc src/libipc` 只列出 **`ipc.h`(+54/−0)** 与 **`ipc.cpp`(+325/−14)** 属本任务；同目录另有 `src/libipc/platform/posix/udp.h`、`src/libipc/utility/id_pool.h` 属我**更早的工作包**（W07 `D-22` / W09 `D-14`，文件时间戳 09-28，本任务未触碰） |
| t35 接线未回归 | `chunk_exhausted`/`chunk_alloc_failed` 写入点仍在（`ipc.cpp` 命中 4）、`queue_evicted` 仍在（`circularqueue.h` 命中 2） |

**改动面（2 文件）**：

| 文件 | 相对 HEAD 的累计 | **其中本任务（t46）** | 改动 |
|---|---|---|---|
| `include/libipc/ipc.h` | +54 / −0 | **+54 / −0**（t35 未动此文件） | +`loan_status` 枚举 + `loan_status_name()`；+`chan_impl::loan(h,size,loan_status*,bool)` 声明；+`chan_wrapper::loan(size, loan_status&)`；+注释（`loan_t` 本体逐字未动） |
| `src/libipc/ipc.cpp` | +325 / −14 | **+86 / −12** | +`#include <limits>`；`acquire_storage` 加 `why` 出参（匿名 namespace 内 ⇒ 无符号影响）；`loan` → `loan_impl` + 两个薄重载；+溢出拒绝；+out-of-line 新重载定义 |

> **`ipc.cpp` 的 +325/−14 里含我 t35（F1）的接线改动**（`note_pool_exhausted` 里的
> `chunk_exhausted`/`chunk_alloc_failed`）。用 t35 留档的改动前版本
> （`artifacts/perf/20260929-r33-W03-counterwiring/counterfactual/ipc.cpp.FIX` = `379918b9…`）
> 做差分，本任务**专属**改动面是 **+86 / −12**，其中 12 行删除全是被重构掉的
> 函数签名与 `return {}`（`acquire_storage` 的参数行 + `loan` 的三个早退），
> 即"把同一个出口改成带原因的出口"，⛔ 无行为删除。

---

## 10. 残余（⛔ 未闭环项，如实登记）

| # | 残余 | 影响哪个验收条件 | 处置 |
|---|---|---|---|
| R-1 | **`borrow_failed_oversized` 仍不可达** | 该 ID 的 0 至今只能读作"未采集"；§13.3 的 B 腿"超尺寸"归因不可判 | 需求 `41_borrow_failed_oversized_需求.md` → **W03**（`counters.h` 落点改判） |
| R-2 | **`borrow_failed_pool_exhausted` 生产写入点未落地** | 同上（0 读作"未采集"直到补丁落地） | 补丁 `40_proposed_callsite.patch` → **SHM接入负责人**（`shm_pub_sub_ipc.h`，属其单一写入面） |
| R-3 | `classify_dzflat_attempt` 的 (c) 分支是**死代码**（`borrow_requested` 在 `src/` 命中 0） | 将来改分类器的人会误以为该路径有效 | 并入 R-1 的需求（二选一：调用点补填 `borrow_requested`，或注释显式登记"仅供单测"） |
| R-4 | §4.3 的越界缺陷**只做了拒绝**，未做"请求即校验"式的前置约束 | 调用方仍可用大请求触发一次失败（而非提前被接口挡住） | 若 W10 认为需要，另立条目（属接口策略，不是正确性） |
| R-5 | `local/include/libipc/ipc.h` 陈旧副本未同步 | ⛔ 不参与构建，无实际影响 | 不修，仅登记（属他人的副本同步问题） |

---

## 11. 证据索引（`artifacts/perf/20260930-r29-libipc-loan-reason/`）

| 文件 | 内容 |
|---|---|
| `01_classifier_exhaustion.log` | 8 组合穷举 ⇒ `oversized` 不可达（证据 ①） |
| `02_loan_exit_reachability.log` | 改动前 4 出口可达性 + `status_api=ABSENT` |
| `03_divergence.log` | 分类器 vs 落点分歧（证据 ②） |
| `10_arms.log` | 六臂终态读数（A/B/C/D/E/F） |
| `11_shadow_verify.log` | 补丁后头文件的影子验证 ⇒ `io_pool 0→80`、`chunk_exhausted=0` |
| `20_referrers.txt` | 25 个引用方（③d） |
| `30_audit.tsv` | 两个 ID 的生产者机械清点 |
| `40_proposed_callsite.patch` | **交 SHM接入负责人**的调用点补丁（已 dry-run + 应用核对） |
| `41_borrow_failed_oversized_需求.md` | **交 W03** 的落点改判需求（三方案 + 验收判据） |
| `50_ctest_j4.log`、`51_hotpath_gate.log` | 回归与热闸 |
| `syms_pre.txt`、`syms_post.txt`、`fingerprints_before/after.txt`、`lib_pre/`、`oldinc/` | ABI 与回滚基线 |
| `patchwork/{a,b}/shm_pub_sub_ipc.h`、`shadow/` | 补丁的前后版本与影子编译副本 |
| `src/*.cpp`、`classifier_exhaustion`、`divergence_classifier_vs_site`、`loan_exit_reachability`、`loan_reason_arms`、`src/audit_counter_wiring.py` | 全部探针源码与二进制（可复算） |
| `command.txt` | 逐条复现命令 |

---

## 12. 评审要点（给非实现者）

1. **别把本包读成"两个 ID 已接线"**：§3.4 的范围声明是硬要求（出口就绪 ≠ 已闭环）。
2. **请独立复核 §4 的两路互证**（8 组合穷举 + 分类器/落点分歧）—— 它把缺口从"采集问题"
   重新定性为"落点缺陷 + 死代码"，这会改变 R-1 的归属（W03 而非 libipc）。
3. **③d 的适用性判断**：本包加接口但不改布局（§2.4）；若判定仍要求全引用方重编，
   请在 REVIEW 里给出理由（我会补跑重编）。
4. **§4.3 的越界缺陷是否应单独成条**：它在任务书外，但属真实正确性问题；
   我做了拒绝式收口（§4.5），如需更强的前置约束请另立条目。
5. **§6.2 的读法**是本次最容易误读的点：`borrow_failed_reason_unknown` 当前**包含**超预算。
