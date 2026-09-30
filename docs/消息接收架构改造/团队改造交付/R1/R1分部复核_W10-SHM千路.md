# R1 分部复核 · SHM 千路闭环证据（r25-W10）

> **分部 verdict，非 R1 主体结论。** R1 主体仍待 socket 修复 + W10 socket 臂收口复跑后出具（t14）。
> 任务：`t31`（队长裁决 **D-24**）；attempt_id `9a99002a-4ce7-4ab6-bbac-1866fc7d446f`；复核人：架构负责人。
> 复核对象：`artifacts/perf/20260929-r25-W10/`（12 个顶层子目录）+ `团队改造交付/W10/W10_交付.md` + `W10_任务失败报告.md`（队长出具）。
> 复算件：`R1/复算脚本/verify_w10_ledger.py`（+ `.output.txt`）、`verify_w10_threads.cpp`（+ `.output.txt`）、`verify_w10_chunk_counter.cpp`（+ `.output.txt`）。
> ⛔ 本轮**未修改** r25-W10 任何文件、**未修改**任何产品代码；全部复算产物落在 `build/t31/` 与 R1 目录。

---

## 0. 一分钟结论

| 项 | 独立复算结论 |
|---|---|
| 逐 route 台账 1000/1000（§13.2 #1/#2） | ✅ **成立**，且我独立重跑同源复现 |
| 固定 worker / 线程数 = 34（§13.2 #4） | ✅ **成立**（两种创建顺序的逐 TID 归因双重确认） |
| 关闭与恢复、旧 generation 无投递（§13.2 #5） | ✅ **成立** |
| `fallback_count == 0` **且四类独立计数**（§13.2 #3） | ⛔ **不成立** —— 4 类要求里有 2 类（`chunk_exhausted`、队列淘汰）**在 `src/` 下无写入点**，其 0 是「未接线」而非「实测为 0」；已给**最小反例** |
| 结束回基线 route/token/fd/队列/chunk（§13.2 #6） | ⛔ **不成立（未测量）** —— 实测单元格只有 route/fd，token/queue/chunk 未测；判定仍打 ✅ |
| §10.2 的 CPU 归因（`≈0.51 core 来自 RecvWorker 池`） | ⛔ **与我的反事实相反**（池整体停用后 CPU 由 0.560 → **0.595 core 不降反升**；逐 TID 归因指向 `ShmControlScheduler` 线程） |
| §10.7 三实验组是否分列 | ➖ **该 run 不含 §10.7 任何分组**（samples 无时延字段），**但也未越界称「端到端」** |
| 证据纪律（默认不写仓库 / 非空拒写 / 新子目录 / 指纹） | ✅ 逐条实测成立（两条**文字口径**需收窄，见 F5） |
| t22 池段修复的 A/B 复测 | ✅ **独立复现**：A/B 两臂 3/3 `builder_rc=0`，B 臂 `orphan segment reset` 生效 |
| 越界声明（socket 千路 / G2 / §10.2 计数器级） | ✅ **未发现越界**（三处均显式声明不可声明） |

### 分部 verdict：⛔ **不通过（分部）**

**按 §13.2 的字面判据**（六条须同时满足，任一条不满足只能标「规模试验失败」）：本 run **2/6 条不满足**（#3、#6），故**不能**据此写入「1000 订阅可用」。

**但必须与「闭环实质是否成立」分开陈述**（否则会误伤真实成果）：

- **实质成立**：1000 个独立话题的**真实注册 + 真实逐 route 收发 + 逐字节载荷校验**、固定 worker 归属（`worker_path=1000`、`compat_total=0`）、销毁后归零 —— 这三件事我**独立复算并重跑复现**，不是转述。
- **不成立的是「证据完备性」**：§13.2 要求的**独立计数**与**回基线测量**两项，本 run 拿不出证据，且其中 `chunk_exhausted` 的「0」具有**误导性**（见 F1 的判决性反例）。
- ⇒ 因此本分部 verdict 的准确表述是：**「SHM 千路闭环的收发实质成立，但按 §13.2 全部六条不能结案」**。

---

## 1. 逐 route 台账与计数的独立复算数值（非引用）

复算件：`verify_w10_ledger.py`（退出码 0 = 全部自洽）。以下均为**我重新计算**的值。

### 1.1 `shm-ind-1000/ledger.csv`

| 复算项 | 我的数值 | 结论 |
|---|---|---|
| 行数 | **1000** | ✅ |
| 唯一 route 数 | **1000**（重复 0） | ✅ 无重复 |
| route 下标集合 | **恰为 `0..999`** | ✅ 无遗漏（比「行数=1000」更强） |
| 全条件满足（`registered=1 ∧ rx==planned ∧ dup=0 ∧ corrupt=0 ∧ timeout=0`） | **1000/1000** | ✅ |
| 不满足样例 | **0 条** | ✅ |
| `planned` / `sent` / `rx` 合计 | **3005 / 3005 / 3005** | ✅ 三方恒等 |
| `dup` / `out_of_order` / `corrupt` / `timeout` 合计 | **0 / 0 / 0 / 0** | ✅ |
| `planned` 分布 | **999×3 + 1×8**（首 route 含 5 条恢复消息） | ✅ 与 `--msgs 3 --resume-msgs 5` 自洽 |
| `first_packet_ok` 全 1 | 1000/1000 | ✅ |

### 1.2 `samples.jsonl` × 台账 交叉

| 复算项 | 我的数值 |
|---|---|
| 行数 | **6010** = tx 3005 + rx 3005 |
| 字段 | `direction, path, route, route_idx, run_id, seq`（**无任何时延字段**） |
| 逐 route `sent`/`rx` 与 samples 计数一致性 | **不一致 0 项** |
| 逐 route 序号集合 | **恰为 `0..rx-1`**（对 1000 个 route 全成立） |
| samples 覆盖 route 数 | **1000** |

### 1.3 `counters.json` 自洽（含两条恒等式）

| 复算项 | 我的数值 | 说明 |
|---|---|---|
| `registered_count == expected_count` | 1000 | ✅ |
| `valid_rx_count == expected_count` | 1000 | ✅ |
| `registration_attempts == registration_ok` | **1000 / 1000** | ✅ |
| `tlv_messages` | **3005** = 3×1000 + 5 | ✅ 与台账 3005 条恒等 |
| `tlv_wire_bytes` | **264440** = **88 B/条 × 3005** | ✅ 88 = 64 B 载荷 + 24 B 头，逐字对得上 |
| `failure_classes` 十类 | 全 0；`first_failed_resource = none` | ✅ 内部自洽 |
| `diagnostics_enabled` | **false** | ⚠️ ⇒ `scan_*` 族受门控，本 run 的 0 不能当「开诊断读数」 |
| 我给出的无写入点计数（见 F1） | **`chunk_exhausted=0`、`queue_evicted=0`、`queue_backpressure=0`、`generation_mismatch=0`、`publish_blocked=0`、`rx_timeout=0`、`fd_limit=0` 等 14 项** | ⛔ 0 的来源是「无生产者」 |

**产品代码写入点普查（我的独立统计，非引用）**：`CounterId` 共 **61** 个，其中 **27** 个在 `src/`+`include/` 有写入点，**34** 个**没有**。

### 1.4 其余拓扑（我复算）

| 臂 | 行数 | 复算结论 |
|---|---|---|
| `shm-ind-1` | 1 | 全条件满足 1/1 |
| `shm-ind-100` | 100 | 全条件满足 100/100 |
| `shm-bcast-32` | 32 | 全条件满足 32/32（`registered=32`、`valid_rx=32`、`planned=8`） |
| `shm-hotcold-1000` | 1000 | 冷路 **999/999** 各收 ≥1 条；热路 route0 `planned=40000`、`rx=64` |

### 1.5 我独立重跑千路（同源重编，⛔ 不写 r25 目录）

```
registered_count=1000/1000 threads_after_create=34 create_ms=692.3 fds_open=5 max_fd=4
pool_routes_alive=1000 socket_pool_routes_alive=0 (registered=1000)
valid_rx_count=1000/1000 dup=0 corrupt=0 out_of_order=0
recover silence_ms=3000 rx_after_silence=5 first_packet_ok=1 lost=0
counters fallback_total=0 backend_unavailable=0 capacity_full=0 wait_set_full=0 wait_token_invalid=0
         registration_attempts=1000 registration_ok=1000 registration_failed=0
worker_path_count=1000 compat_total=0 fallback_count=0 path_choice=0
routes_before_destroy=500 routes_after_destroy=0 threads_after_destroy=5 fds_after_destroy=4
rc=0
```

⇒ **同一源码重编后可从零复现**（我的 run 目录：`build/t31/fresh1000/`，指纹 `dc2d5c5a…`，见 F4 的口径说明）。

---

## 2. 复核要点逐项结论（8 项）

### 要点 1：台账三问 —— ✅ 成立

| 子问 | 结论 | 我的证据 |
|---|---|---|
| ① 载荷按 `(route_idx, seq)` **真的逐字节重算**（非固定载荷） | ✅ | 读 `test/perf/w10/w10_matrix.cpp:75-105`：`fill_payload` 写 seq/route_idx/magic/逐字节模式；`verify_payload` **逐字节重算** `(route*31+seq*131+i*7)&0xFF` 并校验 `magic^seq*2654435761`。`broadcast` 臂按发布者下标生成、订阅者各自重算。⇒ 固定载荷能被旧帧/内容损坏骗过，这里不能 |
| ② 台账覆盖**全部 1000 route**（无重复无遗漏） | ✅ | 行数 1000、唯一 1000、**下标恰为 0..999**（我的复算比队长多做了「下标连续性」这一条） |
| ③ `planned` 与实际发送计划**独立于接收结果** | ✅ **（限定速臂）** | 代码路径：定速分支**先**完成 `pub_publish` 全部 n 条，**再**统一对全部 route 赋 `planned=o_.msgs`；`sent` 只在 `pub_publish` 内 +1 ⇒ 与接收无关。⛔ 但 **hotcold 臂的 `sent` 列恒 0**（热路走 `publish_route`，不记账）⇒ 该列在非定速臂不可读，见 F6 |

### 要点 2：§13.2 六条逐条判定 —— **4 通过 / 2 不通过**

| # | §13.2 条件 | 我的独立结论 | 依据 |
|---|---|---|---|
| 1 | `registered_count == expected` | ✅ | 台账 1000 行 `registered=1`；我的重跑 1000/1000 |
| 2 | `valid_rx_count == expected` + 逐 route 序号/载荷校验 | ✅ | 台账 `rx==planned` 1000/1000；samples 序号集合 `0..rx-1` 全成立；载荷逐字节校验在源码 |
| 3 | `fallback_count == 0` **且** wait-set 满 / token 无效 / chunk 耗尽 / 队列淘汰**均有独立计数** | ⛔ **不成立** | 前两项已接线（`shm_pub_sub_ipc.cc:1891/1894`）；**`chunk_exhausted`、`queue_evicted` 在 `src/` 下 0 个写入点**。最小反例见 **F1** |
| 4 | worker/控制/编排线程数符合冻结配置 | ✅ | 我 **独立证明了组成**（要点 5） |
| 5 | 关闭与恢复在冻结超时内、旧 generation 无投递 | ✅ | 台账 `first_packet_ok=1`、`rx_after_silence=5`；我重跑 `lost=0`；F2 用例（`w10_faults` F2 臂）我独立重跑 `generation_after=2 got=1 has_seq7=1 has_other=0` |
| 6 | 结束时 route/token/fd/队列/chunk 回可解释基线 | ⛔ **不成立（未测量）** | `verdict.md` 实测单元格**只有** `routes 500→0，fd 5→4`；工装无 token/queue/chunk 的回基线测量代码，判定却打 ✅ —— 见 **F3** |

**关于 W08/W09/t19 口径分离是否被正确引用**：本 run 的 `fallback_total=0` 属 W09/t19 §6 规则的**第 1 类**（「没有真回退」，不是「未接线」）—— 该引用**正确**。⛔ 但 t19 只钉死了 `fallback_total` / 原因维度的口径；**`chunk_exhausted` / `queue_evicted` 不在 t19 射程内**（W09 交付 §7 与 W19-F2 均登记其为开放缺口），因此 §13.2 #3 不能借 t19 的结论过关。

### 要点 3：§10.7 三实验组是否分列 —— ➖ 该 run 不含分组，但**未越界**

- `shm-ind-1000/samples.jsonl` 的**全部**字段只有 `direction/path/route/route_idx/run_id/seq`，**没有任何时延字段** ⇒ 该 run **不产出**「传输机制 / 完整读取 / 生产到消费」任一组计时。
- 核查 W10 两份文档：**全文未出现**「实验组 / 传输机制 / 完整读取 / 生产到消费 / 端到端」字样 ⇒ ✅ **没有把第一组称为完整应用端到端延迟**，也没有把该 run 当性能对照表用。
- ⇒ 结论：**§10.7 在本 run 上「未做也不该做」**，写得清楚，**不扣分**；但 W11 的性能对照必须有它，且该 run 的台账**不能**被借去充当 §10.7 证据。

### 要点 4：§10.2 的替代量是否被当成计数器级证据 —— ✅ 未被越界，但**归因错误**（见 F2）

- 交付 §4.2 与失败报告 §3.1 均**显式**写明「计数器未接线 ⇒ 本包**不主张**计数器级证据」，只用 CPU 替代量 ⇒ **口径诚实，不越界**。
- ⛔ 但该替代量的**归因**（「≈0.51 core 来自 RecvWorker 池 `collect_pending()` 全扫」）**与我的反事实相反**：把池整体停用后 CPU 由 0.560 → **0.595 core 不降反升**，逐 TID 归因指向 `ShmControlScheduler` 线程 ⇒ **F2**。这条必须改，否则会污染 R-1 门槛修订（`§11.3`，我作为架构负责人要采纳的输入）。

### 要点 5：反证四件事 —— 3 件成立，1 件**部分成立**

| 反证 | 结论 | 我的证据 |
|---|---|---|
| 有效订阅是否**真的存在** | ✅ | 两条独立证据：① 控制面 `peer_count() >= 1` 逐话题握手（`handshake_ok=1000/1000`，工装里 wait_handshake 的实现）；② 台账 `rx==planned` 且载荷逐字节校验通过 —— **没有真订阅不可能收到并校验通过 3005 条** |
| 预期路径是否**真的执行** | ✅ | seam 计数 `worker_path_count=1000`、`compat_total=0`、`path_choice=0` ⇒ 1000 条 route **全部**走固定 worker 路径，无一条被回退线程顶替。我另在 4-worker 臂上验证 seam 反向可分：`worker_path=508` / `compat_total=492` / `reasons=7:492`（`kWaitSetFull`） |
| 故障场景是否**真的触发** | ✅（F4 尤其） | 我独立重编重跑 `w10_faults`：`F1 idle_exits_delta=31 threads=3`、`F2 generation_after=2 got=1 has_seq7=1 has_other=0`、`F3 routes 1->1`、**`F4 loan_ok=40 loan_rejected=160`**（stderr 打出 `chunk pool exhausted`）、`F5 routes 1->0`、`failures=0` |
| 统计是否**真的覆盖整个系统** | ⚠️ **部分** | 覆盖面的正面：`tlv_messages=3005` = 台账 3005 条（发布侧全量对账）；`registration_*` 1000/1000。**反面**：① `diagnostics_enabled=false` ⇒ `scan_*`/`recv_once_*`/`budget_yields`/`idle_exits` 族**恒 0**，§10.2 所要求的量在本 run **无读数**；② §13.3 十类里 **7 类**（`chunk_exhausted`/`queue_backpressure`/`generation_mismatch`/`publish_blocked`/`rx_timeout`/`fd_limit`/`fallback_pool_exhausted` …）**无写入点** ⇒ `first_failed_resource=none` 的**排除力有限**（见 F1） |

**`threads=34` 是否真等于「32 worker + 1 调度器 + 1 主线程」—— 我做了判决性反证（两次创建顺序对照）：**

`verify_w10_threads.cpp` 用编译期开关切换**创建顺序**，外部脚本按 `/proc/<pid>/task/<tid>/stat` 的 `starttime` 排序读逐 TID CPU：

| 臂 | 创建顺序 | 线程数 | 逐 TID 归因结果 |
|---|---|---|---|
| ① `ORDER_POOL_FIRST=1` | 主(1) → worker(2..33) → **调度器(34)** | 34 | 唯一非零 tid 落在 **#34** = **调度器** |
| ② `ORDER_POOL_FIRST=0` | 主(1) → **调度器(2)** → worker(3..34) | 34 | 唯一非零 tid 落在 **#2** = **调度器** |

另用 `RecvWorkerPool::instance().start(32)` 的单点探针复核：`main 起始=1 → 取调度器单例=2 → 池 start(32)=34 → 池 stop=2`。

⇒ ✅ **`threads = 34 = 1 主线程 + 1 调度器线程 + 32 worker` 成立**，**没有把其它服务线程漏计或错计**。
（口径说明：`RecvWorkerPool::start(32)` 按**固定 32 个** worker 拉起，与 route 数无关，所以 `n=1` 与 `n=1000` 都是 34 —— 这恰好也是「固定线程」目标的正面证据；`gate3` 里 `workers=4/8/16/32 → threads=6/10/18/34` 正是 `1+N_worker+1`。）
⇒ ⛔ 同时暴露：**本 run 稳态下唯一忙的那条线程就是调度器**，不是 worker —— 这正是 F2。

### 要点 6：t22 池段修复的独立复测 —— ✅ **我独立复现了 A/B 双 rc=0**

方法（与 W09 交付 §5.1/§5.2 同法，我自己重编 `tmp/w00_verify/leaker.cpp` 并自己杀进程）：

| 臂 | 步骤 | 轮次 | 我的实测 |
|---|---|---|---|
| **A（published）** | 借满 40 块 132096 档 → **发布** → 外部 `SIGKILL` → 单跑 `test_dzflat_builder` | 3/3 | `loaned=40 mode=published`；**`builder_rc=0`**（无需复位） |
| **B（unpublished）** | 借满 40 块 → **不发布** → 外部 `SIGKILL`（段 `CHUNK_INFO__132096__C40` 杀前/杀后都在=1）→ 单跑 | 3/3 | `loaned=40 mode=unpublished`；**`builder_rc=0`**，并打出 `chunk pool orphan segment reset: kind = attach, chunk_size = 132096, capacity = 40, count = 1 (本进程), prefix = ''  <= 段已无活映射者(崩溃遗留), 空闲链已整体复位` |

⇒ ✅ **方法可复现、结论成立**：`orphan segment reset` 确实在「段已无活映射者」时整体复位空闲链，B 臂从「理应 rc≠0」变成 rc=0。
⚠️ 方法学提醒（我第一版踩到）：`leaker` 需**用真实 PID** `kill -9`（用 `timeout … &` 包裹后会杀到包装进程，残留段反而仍在 ⇒ 得到假读数 `rc=1`）。W09 的日志（`v2_2_after_fix.log`、`cf_2_baseline_after_leak.log`）与我的复现同形，但**四份日志内容几乎逐字相同**（含 `NOT_KILLED` 字样）⇒ 属「同法不同次」，也说明该实验**输出信息量偏少**（只有 3 行），建议 W09 补一份带段号/借样数的结构化输出，别再靠 3 行 stdout 承重。

### 要点 7：证据纪律 —— ✅ 行为成立（三处文字/口径需收窄 → F5）

| 纪律 | 我的实测 |
|---|---|
| ① 默认不写仓库 `artifacts/` | ✅ **不带 `--out` 跑，`artifacts/perf` 顶层条目 before=30 / after=30（+0）** |
| ② 非空目录拒绝写入 | ✅ **行为实测**：预置一个非空目录后 `--out` 到它 ⇒ 打印 `ARTIFACT_DIR_NOT_EMPTY: … —— 拒绝覆盖（重跑请新建 run_id）`，目录仍只有原 1 个文件（⛔ 注意：**拒绝时进程仍返回 rc=0**，即「拒绝」不体现在退出码上） |
| ③ 各运行各占新子目录 | ⚠️ 顶层确有 **12 个子目录**、无覆盖发生；但**不全是「运行」**，但**只有 10 个是「运行」**（`shm-ind-1/100/1000`、`shm-bcast-32`、`shm-hotcold-1000`、`socket-ind-1/100`、`faults/`、`gate3/` 里的探针、`repro/`、`ctest_rounds/`、`port-conflict/`）；**`socket-ind-1000` 没有目录**（超时被杀、未落盘），三态空闲的落盘物是**34 字节文本文件**而非目录。⇒ 「12 个运行各占新子目录」**表述过宽**，见 F5 |
| ④ 指纹与 `ldd`/RUNPATH | ✅ `fingerprint.txt` 记录库 `43288e5f…`、工装二进制 `c3459700…`(matrix)/`d8fca8aa…`(idle)、`ldd → build/lib/libipc.so.3`、`RUNPATH=/home/zwc/cpp_ipc_dds/build/lib`、机器/governor/继承环境清理声明 | 但 ⚠️ **`build/bin/w10_matrix` 的指纹已变**：`fingerprint.txt`(19:19) 记 `c3459700…`，`gate3/harness_fingerprints.txt`(20:38) 记 `b56e7f39…`，**当前二进制又是 `b56e7f39…`** ⇒ 同一 run 内两类工装读数用**不同二进制**，且当前源码重编后与两者都不同 ⇒ **F4** |
| ⑤ 三态空闲 + 60 s 窗口 | ✅ 我复算 `idle-state3`：`threads=34 cpu=0.583–0.589 core ctx=9040–9101 /s wait_timeouts=19196–19199`、`idle_exits=0`（状态 3 保持接收能力）与 `gate3` 的 `0.58200 core` 同档 |

### 要点 8：越界声明核查 —— ✅ **未发现越界**

逐份检索该 run 的全部文档（`W10_交付.md`、`W10_任务失败报告.md`、`README.md`、各 `verdict.md`）：

| ⛔ 不得声明 | 核查结果 |
|---|---|
| socket 千路有效收发 | ✅ **未声明**。反复写「本轮**不能声明**」；`socket-ind-1000` 只有 `rc=124` 挂起日志 |
| G2 规模闭环完整通过 | ✅ **未声明**。失败报告 §6 明确列入「不能声明」清单 |
| §10.2 计数器级结论 | ✅ **未声明**。明写「不主张计数器级证据，只给 CPU 替代量」 |
| 各 `verdict.md` 的「判定：通过」 | ⚠️ 各臂 `verdict.md` 写「判定: 通过」而 §13.2 六条里 #3/#6 实为不满足/未测量 ⇒ 属**内部不自洽**（见 F3），但**不构成越界声明**（未上升到「1000 订阅可用 / G2 通过」） |

---

## 3. Findings（按严重度）

### F1 · **blocker** · §13.2 #3「四类独立计数」不成立，且 0 具误导性

- **问题**：§13.2 第 3 条要求 `wait-set 满 / token 无效 / chunk 耗尽 / 队列淘汰`**均有独立计数**。实际：
  - `wait_set_full`、`wait_token_invalid`：✅ 有写入点（`src/dzIPC/shm_pub_sub_ipc.cc:1891`、`:1894`），我在 4-worker 臂实测到 `wait_set_full=492` **非零**；
  - `chunk_exhausted`、`queue_evicted`（队列淘汰）：⛔ **`src/` + `include/` 下 0 个写入点**（`grep -rn "CounterId::chunk_exhausted" src/` = 0；`queue_evicted` 同样 0；`CircularQueue` 内部也不写 registry）。
- **最小反例（我写的判决性实验）**：`verify_w10_chunk_counter.cpp` —— 真实建 pub/sub、真实借样直到池耗尽：

  ```
  BEFORE registry: chunk_exhausted=0 chunk_alloc_failed=0 fallback_pool_exhausted=0
  chunk pool exhausted: kind = loan, chunk_size = 132096, size = 131072, pool capacity = 40, count = 1 (本进程), prefix = ''
  loaned=40 rejected=160 (池容量 40/档)
  AFTER  registry: chunk_exhausted=0 chunk_alloc_failed=0 fallback_pool_exhausted=0
  VERDICT: 池已明确耗尽(rejected=160) 而 CounterRegistry::chunk_exhausted=0
  ```
  ⇒ **池已明确耗尽（160 次 loan 被显式拒绝、stderr 打了 `chunk pool exhausted`），而 `chunk_exhausted` 仍为 0**。故 `counters.json` 里的 `chunk_exhausted=0` **不能**被读作「本轮没有 chunk 耗尽」。
- **影响**：`first_failed_resource = none` 的**排除力被高估**（十类里有 7 类无生产者）；§13.2 #3 无法按字面结案；该 run 也**不能**用于「回退/容量」的判定。
- **归属与现状**：W09-F5 / W19-F2 已登记该缺口（原计划由 t19 接线），但 **t19 交付的是「已就绪待应用的 hunk patch」+「接线口径」**，其中**不含** `chunk_exhausted`/`queue_evicted`；t19 交付 §7 明写「未写入任何生产源文件」，且其验证是**影子头编译**，未改产品代码。
- **要求修法（⛔ 不得靠文字说明绕过）**：把 `chunk_exhausted`（`ipc.cpp` 池耗尽唯一出口 `note_pool_exhausted`）、`queue_evicted`（`CircularQueue` 淘汰回调 / `adopt` 溢出处）接到 `CounterRegistry`；接线后**重跑本 run 的同一臂**，并让 `counters.json` 明确标出「本臂是否真的触发过该资源」。
- **在接线前的正确表述**：本 run 关于 §13.2 #3 只能写「`fallback_count=0`、`wait_set_full=0`、`wait_token_invalid=0` 为实测；**chunk 耗尽与队列淘汰两类本 run 无独立读数，不得据此判为 0**」。

### F2 · **high** · §3.2 的空闲 CPU 归因与反事实相反（真凶是控制面调度器，不是 RecvWorker 池）

- **问题**：交付 §3.2 断言「0.58 core 中 **≈0.51 core 来自 RecvWorker 池**（32 worker × 100 ms 超时唤醒 × `collect_pending()` O(route) 全扫）」，依据 `w10_threadscan`。
- **我的反事实（4 格 2×2，`win=8 s` 稳态，外部逐 TID 读 `/proc/<pid>/task/*/stat`）**：

  | 格 | 调度器 | 接收池 | 线程数 | 逐 TID 实测总 CPU |
  |---|---|---|---|---|
  | A | ON | ON | 34 | **0.560 core**（唯一忙线程 = 调度器） |
  | B | OFF（compat 控制面） | ON | 2033 | ~0.00 core |
  | **C** | **ON** | **OFF（`DZIPC_SHM_RECV_COMPAT=1`）** | 1002 | **0.595 core** |
  | D | OFF | OFF | 3001 | ~0.00 core |

  ⇒ **把 RecvWorker 池整体停用（`pool running=0 workers=0 route_count=0`，即完全没有 `collect_pending()`）后，CPU 不降反略升（0.560 → 0.595）**。
- **两次创建顺序的逐 TID 判决**（要点 5 表）：无论先建池还是先取调度器，**唯一非零的 tid 都是调度器那条线程**。
- **一致的旁证（四条，互相独立）**：
  1. **决定性标定（接收池完全不参与）**：`--state 2` 下**只建订阅、不建发布端** ⇒ `pool running=0 workers=0 route_count=0`、全进程**仅 2 条线程**（主 + 调度器）。CPU 随**在册控制项数**增长：

     | 在册控制项 | CPU (core) | 每项成本 (core/项) | 进程线程数 | 接收池 |
     |---|---|---|---|---|
     | 100 | **0.00600** | 6.0e-5 | 2 | 未启动 |
     | 500 | **0.07900** | 1.6e-4 | 2 | 未启动 |
     | 1000 | **0.31700** | 3.2e-4 | 2 | 未启动 |

     ⇒ **完全没有接收池时，1000 项就已耗 0.317 core**。而状态 3（`entries=2000` = 1000 sub + 1000 pub）实测 0.583 core ≈ **0.317 × 2**（按 `entries` 线性外推）—— 与「控制面项数 × 每项成本」的归因**逐值吻合**。
  2. `w10_idle` 自己打印的调度器唤醒率随在册项数单调增长：`entries=0 → 0 wakes/s`（状态 1，CPU **0.000**）；`entries=1000 → 14223 /s`（状态 2，CPU **0.317**）；`entries=2000 → 8705 /s`（状态 3，CPU **0.583–0.589**）⇒ **CPU 与 `entries` 一一对应**。
  3. 池侧自己的读数在稳态几乎不动：`wait_timeouts` 增量 = 32 worker × 60 s / 100 ms ≈ **19200**（每 worker 每 100 ms 只唤醒 1 次）、`recv_once_calls=0`、`budget_yields=0`；每次唤醒的 `collect_pending()` 只遍历**本 worker 的 ~31 条 route**（1000/32）⇒ 量级 ≈ 10³ 次 seq 读/秒，**远不足以撑起 0.5 core**。
  4. 逐 TID 归因（要点 5 的两序对照）唯一非零 tid 恒为调度器线程；池的 32 条 worker 线程 CPU **全部为 0**。

- **影响**：① 交付 §3.2/§3.3 与失败报告 §4.3 的归因需改写；② §3.3 给 R-1 的两项输入里，「池项 = `N_worker×1000/wait_timeout`」这一项不再是主成本，而「控制面项被高估 13–36×」的结论需要**在真实回调负载下重算**（`w10_tickcost` 用的是**空回调**，与 2000 个真控制项的成本不可比）；③ **R-1 门槛（§11.3，本人负责采纳）不得据此冻结**。
- **要求修法**：把 `w10_threadscan` 的逐 TID 行**落盘**（r25 只存了汇总行 `n=… threads=34`，**没有逐 TID 明细**，无法从证据目录自证归因）；并把 2×2 反事实（尤其 C 格）作为归因判据写入 W10 报告。

### F3 · **medium** · §13.2 #6 未测量却判 ✅

- `verdict.md` 第 6 行实测单元格只有 `routes 500→0，fd 5→4`；条件原文要求 `route/token/fd/队列/chunk` 五类回基线（±5%）。工装里**没有** token/queue/chunk 的回基线测量代码（`grep` 只命中 verdict 生成器那一行文本）。
- ⇒ 该行应改为「**部分测量**：route/fd 已测，token/queue/chunk **未测**」，判定不得打 ✅。

### F4 · **medium** · 本 run 的工装二进制已不可原样复跑（且现二进制会 SIGSEGV 退出）

- `fingerprint.txt`(19:19) 记 `w10_matrix` = `c3459700…`；`gate3/harness_fingerprints.txt`(20:38) 记 `b56e7f39…`；当前 `build/bin/w10_matrix` 亦为 `b56e7f39…`，而库在 21:17 被再次重建（`src/dzIPC/threepools/recv_worker.cc` 21:16 有改动、`recv_worker.h` 21:14 有改动）⇒ **工装二进制(20:24) 早于库(21:17)**。
- **我的实测**：用当前 `build/bin/w10_matrix` 复跑任意 SHM 臂 ⇒ **6/6 rc=139（SIGSEGV）**，栈为 `__GI___libc_free ← main()`，发生在 `run()` 返回**之后**、进程退出前；而用**同一源码重新编译**的二进制 ⇒ 1000 路 **rc=0**，各项读数与 r25 同档。
- **本 run 内部还横跨两个工装世代**（我从指纹与 mtime 复原）：

  | 时段 | 产物 | `w10_matrix` 指纹 |
  |---|---|---|
  | 19:19–19:32 | 4 个拓扑臂 + `faults` + 三态空闲 | `c3459700…` → **本 run 的台账证据全部出自此二进制，内部自洽** |
  | 20:35–20:53 | `port-conflict`、`gate3`、`repro`、`ctest_rounds`、`README.md` | `b56e7f39…` |
  | 当前工作区 | `build/bin/w10_matrix` | `b56e7f39…`（且库已于 21:17 再重建） |

- ⇒ 结论：① **台账那批证据（19:19）自洽且我复现得出来**（用同源重编的二进制）；② 但**整个 run 目录不能整体原样重放**：它混了两个工装世代，且当前树上的二进制与库不配对，`w10_repro.sh` 今天跑会产出 rc=139 的产物 ⇒ 证据可追溯性受损。
- **要求修法**：把工装纳入 CMake 构建（现在**不在** `test/CMakeLists.txt`，批跑用的是手工/独立编译的二进制）、把**工装二进制指纹与库指纹成对**写进 run 的 `fingerprint.txt`，并在 `w10_repro.sh` 首步做「二进制 mtime/指纹 vs 库指纹」一致性检查，不一致即报错退出。

### F5 · **low** · 三处文字/口径过宽（含一处会实际误导复算）

1. 「12 个运行各占新子目录」：实际只有 10 个运行臂有目录；`socket-ind-1000` 无目录（超时），三态空闲落盘物是 **34 字节文件**而非目录；`faults/gate3/repro/ctest_rounds/port-conflict` 属探针/批跑产物。
2. **各臂 `.log` 与证据目录不是同一轮**（我做了逐臂判决性对照）：`.log` 里含 `ARTIFACT_DIR_NOT_EMPTY …` ⇒ 该 `.log` 记录的是**第二次被拒绝的运行**。逐臂数值对照亦证实：

   | 臂 | `.log` 的 `first_packet_us` | 目录内 `phases.csv` / `manifest.json` |
   |---|---|---|
   | `shm-ind-1000` | `95844262506` | `95200476310` / `9.52005e10` |
   | `shm-ind-100` | `95839283178` | `95195498677` / `9.51955e10` |
   | `shm-bcast-32` | `95853171007` | `95209386735` / `9.52094e10` |

   ⇒ **两者相差约 6.4×10⁸ µs（≈644 s ≈ 10.7 min），即 .log 与目录确为不同次运行**。目录与 `.log` 的 mtime 差 10 min 也与此吻合。⛔ 因此**不能**用 `.log` 去复算目录里的读数；正确做法是「**目录为准**，`.log` 仅作参考」，或把被拒运行的日志改名（如 `*.rejected.log`）。
3. `--out` 在两个工装里语义不同（`w10_matrix` 要目录、`w10_idle` 要文件名），批跑脚本用同一变量 `$dir` 传给两者 ⇒ 极易误用（r25 已实际产生 34 字节「伪目录」文件）。

### F6 · **low** · hotcold 臂的 `sent` 列恒 0，易被误读

- `w10_shm_hotcold_3020_0`：`planned=40000 sent=0 rx=64`；冷路 999 行同样 `sent=0`（`publish_route` 不记 `sent`）。
- ⇒ 「`planned` 与发送计划独立于接收结果」这一条**只在定速臂成立**；台账应在非定速臂把 `sent` 写成空/`n/a`，或补 `sent` 记账。

---

## 4. 明确列出**不可声明**项（本分部结论新增/收窄）

| ⛔ 不得声明 | 理由 |
|---|---|
| 「SHM 千路**满足 §13.2 全部六条**」 | #3、#6 不成立（F1/F3） |
| 「`counters.json` 证明本轮无 chunk 耗尽 / 无队列淘汰」 | 该两类计数无写入点，0 不可读（F1） |
| 「空闲 CPU 的主成本是 RecvWorker 池的 O(route) 全扫」 | 与 2×2 反事实相反；主成本在控制面调度器线程（F2） |
| 把 §10.2 的 CPU 替代量写进 §11.3 门槛修订依据 | 归因错误 + 该 run `diagnostics_enabled=false`，无计数器读数（F2） |
| 「本 run 可原样重放」 | 工装二进制/库指纹不配对，当前重跑 rc=139（F4） |
| socket 千路有效收发 / G2 规模闭环完整通过 / §10.7 性能结论 | 沿用原 run 的边界声明（我核查**未越界**，此处仅为汇总重申） |

**可以声明**（我独立复算/复现支持的部分）：

- SHM **1000 个独立话题**的**真实注册 1000/1000 + 真实逐 route 收发 1000/1000 + 逐字节载荷校验**；
- **固定 worker 归属**：`worker_path=1000`、`compat_total=0`，`threads = 1 主 + 1 调度器 + 32 worker`；
- 销毁后 `routes → 0`；恢复首包 `first_packet_ok=1 / lost=0`；旧 generation 无投递（F2 臂）；
- §13.2 **#1 / #2 / #4 / #5 四条通过**（独立复算）；
- 1 / 100 / 1000 三档 + 1×32 广播 + 1 热 999 冷四拓扑的台账自洽；
- **t22 池段修复有效**（A/B 双 3/3 rc=0，`orphan segment reset` 生效）；
- 证据纪律「默认不写仓库」「非空拒绝写入」**行为实测成立**。

---

## 5. 复算件清单（可一键重放，⛔ 只读）

| 文件 | 内容 |
|---|---|
| `R1/复算脚本/verify_w10_ledger.py`（+ `.output.txt`） | 台账/samples/counters/其余拓扑/条件 6 单元格的独立复算与退出码（当前 exit 0） |
| `R1/复算脚本/verify_w10_threads.cpp`（+ `.output.txt`） | 两种创建顺序的逐 TID CPU 归因（证明 34 的组成 + 忙碌线程是调度器） |
| `R1/复算脚本/w10_perthread.py` | 外部逐 TID 读取器（按 `starttime` 排序 = 创建顺序） |
| `R1/复算脚本/verify_w10_chunk_counter.cpp`（+ `.output.txt` / `.command.txt`） | §13.2 #3 的**最小反例**：池真实耗尽而 `chunk_exhausted` 仍为 0 |
| `R1/复算脚本/verify_w10_idle_attribution.command.txt`（+ `.output.txt`） | F2 的判决性归因：2×2 反事实 + `--state 2`（池完全不参与）下 CPU 随控制项数线性标定 |
| `R1/复算脚本/verify_w10_threads_and_counters.command.txt` | 上述件的编译/运行命令（含 `-DORDER_POOL_FIRST=0/1` 两序） |

**R1 主体（t14）仍需**：socket 端口冲突修复后的 W10 socket 臂收口复跑、F1 的计数接线后的同臂重跑、F2 结论纳入 §11.3 R-1 修订。
