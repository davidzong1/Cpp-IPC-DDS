# W02 交付说明 —— 统一跨进程基准

> **工作包**：W02（P0，基准/数据适配），依据 `../../团队改造方案_性能证据闭环与SHM规模化.md` §4 W02
> **基线提交**：`e800ccc496ac710b711c9346709e86a148c41241`（工作区含其他成员的未提交改动，实际清单见当轮 `manifest.json:working_tree_diff`）
> **执行者**：基准负责人；**独立评审者**：待队长指派（本工作包成果不得由执行者自审通过）
> **当前等级（方案 §11）**：**S3 单项验证通过**。理由见 §4。
> **本轮性质**：新增基准源码/脚本/自检用例与文档；**未修改任何产品源码**（`src/`、`include/` 的改动属于其他工作包）。

---

## 1. 交付物清单

| # | 文件 | 说明 |
|---|---|---|
| 1 | `test/xproc_benchmark.cpp` | 基准本体：父进程编排 + `--role=pub\|sub\|ddscheck` 角色进程 + 合并与产物输出 |
| 2 | `test/w02_pattern.h` | 逻辑载荷/形态/序号相关模式/时间戳头槽位定义（单一事实来源） |
| 3 | `test/w02_xproc_common.h` | 控制块（跨进程握手与身份登记）、配置枚举、原始样本行定义 |
| 4 | `test/w02_dds_types.idl` / `.c` / `.h` | CycloneDDS 侧同构类型（`idlc -l c` 生成物，已入库，不需要本机再生成） |
| 5 | `test/w02_dds_udp.xml` / `test/w02_dds_iox.xml` | CycloneDDS 两档对照配置（SharedMemory off / on） |
| 6 | `test/w02_benchmark.sh` | 启动脚本（重建 → 仅清理本运行对象 → 拉起 → 汇总） |
| 7 | `test/test_w02_xproc_benchmark.cpp` | 小规模正确性用例（**10 条** gtest：形态/模式/头口径 + 身份/计时边界 + **路径计数单写入者（D-17）** + **未测项写空不写 0（D-19，含真起 DDS 档）** + 失败计数） |
| 8 | `W02_统一跨进程基准_结果格式与使用.md`（本目录） | 结果格式说明、口径、已知限制、证据索引 |
| 8b | `W02_中间轮次留痕与r18去向.md`（本目录） | D-17 要求：中间轮次处置与 r18 去向说明（含逐 run 指纹时间线） |
| 8c | `test/w02_sample_audit.py` | 样本表规范校验器（计时边界 + 空值分类 + 行口径恒等式），把 skipped 判据固化成可执行代码 |
| 8d | `test/w02_summary_schema_check.py` | `summary.csv` 表头/格式串/数据行三方一致性闸（t47：插入新列曾漏改格式串 ⇒ `fprintf` 越界 SIGSEGV） |
| 9 | `test/CMakeLists.txt`（修改） | 新增 `xproc_benchmark` target + `test_w02_xproc_benchmark`（进 CTest）+ `test_w02_summary_schema`（t47 结构闸，进 CTest） |

证据：**现行唯一可引用 run 由当轮 `manifest.json:currently_citable` 指定**（机器可读的单一权威指针）；
`r19`（`*_messages` 因 D-17 双写作废）、`r20`（F2 修复前对照）、`r23`（F3 修复前一代）**均为非现行**，
逐条原因见 `manifest.json:superseded_runs.non_current_runs`，⛔ 不得作为现行结论引用。

---

## 2. 合同七条对照

| 合同要求 | 落点 | 证据 |
|---|---|---|
| (1) 父进程编排 + 独立发布/订阅进程、握手、预热、定速/满速、正常退出、跨进程身份可核验 | `xproc_benchmark.cpp`：`run_one_case` / `spawn_role` / `wait_handshake` / `check_identity` / `SendPlan` / 停止哨兵与 `sub_done` | 每用例 `verdict.md:identity:` 行；`manifest.json:binary_sha256`；`child_killed_total=0` |
| (2) 适配 TLV / DZFlat A / B / CycloneDDS+iceoryx；同一逻辑载荷；记录应用字节与传输字节 | `--path=`；`Shape`（`w02_pattern.h`）；`payload_bytes` / `wire_bytes` / `wire_bytes_source` | `summary.csv` 的 `payload_bytes,header_bytes,wire_bytes_per_msg,wire_bytes_source`；`test/w02_dds_types.idl` 与 `TestMsg` 同构 |
| (3) 三个时刻分别计时，禁混用结束点 | `transport_ns`（发布侧本地）/`delivery_ns`/`app_read_ns`/`e2e_ns`；头里**不**放 `transport_done` | `samples/*.samples.csv` 的派生列；`test_w02_xproc_benchmark` 的 `TimingBoundariesAreConsistent` 逐行验算 |
| (4) 只读时间戳 / 全量读校验 / 原地构造为不同工作负载，各组工作量等价 | `Workload::timestamp` / `crc` / `full`；B 档生产端就地构造（`warmup` 与 `measure` **同形**） | `summary.csv:workload,elements_checked`；B 档的可分性证据在**发布侧**（`wire_bytes_source`，见《结果格式与使用》§7.1） |
| (5) 阻塞事件驱动为主对照，忙等单列组并报 CPU | `WaitMode::blocking` / `busy`；`--smoke` 在 1 KiB 档附 busy 组 | `summary.csv:wait_mode,pub_cpu_s,sub_cpu_s,total_cpu_cores` |
| (6) 逐样本延迟、序号、失败/丢失/重复/乱序、实际速率、队列策略、路径计数、运行元数据 | `samples/` + `summary.csv` + `manifest.json` + `counters.json` + `path_evidence.json` | 同上 |
| (7) 定速用独立于接收完成的发送计划，记录迟发/积压/publish 阻塞 | `SendPlan`（`t0 + k/rate` 绝对排定表） | `summary.csv:plan,attempts,late,late_threshold_ns,backlog_max_ns,backlog_sum_ns,send_blocked` |

---

## 3. 验收三条对照

| 验收 | 判据 | 证据 |
|---|---|---|
| **跨进程身份可核验** | pub/sub 是 `fork+exec` 的新映像；各自 `CLOCK_MONOTONIC` ready 值落在父进程 spawn 前后括号内；pid + `/proc/<pid>/stat` starttime 同登记；实际加载库路径回填控制块 | `verdict.md` 每用例 `identity:` 行；`test_w02_xproc_benchmark::IdentityIsVerifiable` |
| **计时边界一致** | 四列派生量与绝对时刻逐行自洽（`done-enter`/`app-done`/`fully-app`/`fully-produced`），且生产/消费内部顺序单调 | 同上的 `TimingBoundariesAreConsistent`（遍历整个样本表） |
| **异常消息与发送失败不被静默排除** | 六类**必须进机器可读判定**（t47/W02-F3 收紧）：`send_failed/missing/duplicate/out_of_order/checksum_bad/rows_truncated` + `late`/`backlog`/`send_blocked`/`abnormal`/`bad_header`，各自有**显式阈值**（写入 `manifest.failure_thresholds`）并在 `summary.csv` 有对应 gate 列；仅"落列不判定"视为静默排除 | `FailuresAndLossesAreJudged`（新）+ `FailuresAndLossesAreAccounted`；`summary.csv` 的 `*_ok` 列与 `bad_header` 列；现行 run 的 `f3_before_after.md` |
| **含 64B/1KiB/64KiB/1MiB 四档 smoke** | `--smoke` / `--smoke-dds` | 现行 run（`manifest.currently_citable`）= `artifacts/perf/20260928-r34-W02/`（75 用例，四档齐备） |

---

## 4. 为什么现在只能标 S3（而不是 S4/S5）

- **S3 已达成**：可复现命令（`test/w02_benchmark.sh smoke-dds` 或 `command.txt`）、通过条件、
  原始日志与失败计数（`summary.csv` / `verdict.md` / 540 个原始样本文件）齐备，且自检用例进 CTest。
- **S4 未达成**：缺**非实现者**复核。本目录的自检用例覆盖的是"三条验收"，不构成独立评审。
  队长已于 2026-09-28 **批准并指派架构负责人**（经 t14/R1）做 W02 独立复核，复核清单含
  进程模型/计时边界/指纹口径与本节三条验收；复现命令 `test/w02_benchmark.sh smoke-dds`。
  ⛔ 不由实现者自审升 S4。
- **S5 未达成**：本工作包不涉及默认启用。

**未确认项（不得当作已确认）**：

1. `cyclonedds-iox` 的**路径证据未确认**（`path_evidence.json:unconfirmed`）：DDS 侧没有
   dzIPC 的路径计数器，只能给出 `CYCLONEDDS_URI` 指向、RouDi 存在性、实体可建与收发计数。
   "每条消息真走 iceoryx/零拷贝"未取证。
2. DDS 档**不采集 wire 字节**（`wire_bytes_per_msg=null`）。
3. 三条边界里 `transport_ns` 是**发布进程本地**测得（API 入口→返回），不是"消息到达对端"的时间；
   `delivery_ns` 才是通知与交付路径开销。报告不得把 `transport_ns` 当端到端。
4. 跨档比较时等待策略**不同构**：DZFlat 档用视图队列定时阻塞（`get(Sample&, tm=20ms)`），
   TLV 档用 msg 队列条件变量阻塞（收尾阶段退化为短睡眠轮询 + `try_get_clone`）；两者
   `wait_mode` 都标 `blocking`，这是**同一个标签下的两种机制**，跨档比较时必须引用本条限制。
   （TLV 档不能用视图队列等待：TLV 话题上视图队列恒空，那等于换了对照组。）
5. 性能数字只来自单次运行，**没有置信区间/轮间离散度**；正式对照需按方案 §6.2 的多轮交替由
   W11 在独占窗口采集。

---

## 5. 使用约定（**后人误用防错表**，队长 2026-09-28 裁决）

| 约定 | 内容 | 依据 |
|---|---|---|
| **不得改历史 manifest 的 `work_package`** | `manifest.json:work_package` 恒为 `"W02"`。若其他工作包（W08/W10/W11）需要复用本基准出**自己**的证据，正确做法是**以新 `--run-id` + `--out-dir` 重跑**，并在当轮改工作包号；⛔ 禁止改写既有 `artifacts/perf/<run_id>/` 下的产物把它"署名"成别的工作包 | 方案 §11「不得从一项事实推导另一项事实」：同一产物被两个工作包署名会造成证据归属混淆 |
| **本工作包不做参数化** | 队长已**否决**把 `work_package` 做成命令行参数。各工作包已/应有自己的结果目录（如 `artifacts/perf/20260928-r1x-W08-*`），不需要借用 W02 的 run | 同上 |
| **冒烟 ≠ 性能结论** | 现行 run（`20260928-r34-W02`）是**基准可用性 + 正确性**证据（75 用例、单次运行、无置信区间/轮间离散度）。⛔ 不得被 W11 当作正式性能结论引用 | 方案 §6.2（≥5 轮交替、保留逐轮结果）与 §10.8（不得用单次读数替代验收） |
| **正式采集必须独占窗口** | W11 串行独占测试窗口；本基准只提供可复现入口与结果格式，不做"最优轮挑选"（保留逐轮独立 run 目录，不合并汇总） | 方案 §3 协作规则 4、§4 W11 |
| **引用纪律** | 任何引用都要带：`run_id` + 进程模型（恒 `cross-process`）+ 等待方式（`blocking`/`busy`）+ 计时边界（`transport`/`delivery`/`app_read`/`e2e` 中的哪一个）+ 消费工作量（`timestamp`/`crc`/`full`） | 与 W01 证据索引纪律一致；W01 UF-01/UF-12 的教训 |
| **⚠️ `r19` 的 `*_messages` 已作废；`r20`/`r23` 亦非现行 —— 只可引用 `currently_citable`**（D-17 + D-19 + t47/F0） | `artifacts/perf/20260928-r19-W02` 的 `counters.json` / `*.counters.json` 里 `*_messages`（tlv/dzflat_a/dzflat_b）**被双写**（传输层钩子 + harness），数值约 2×，且**在回退场景下把"期望路径"记成"实际路径"**（`r15_dzflat-a…`：`a=3208` 而真实路径是 A:2208 + TLV:2）—— 这会让 W03 `path_evidence` 的 `confirmed` 判定失去区分力。该 run 的逐样本表、`summary.csv` 的 plan/attempts/sent_ok/recv_measure、计时边界分布不受影响，但**一律以 `currently_citable` 指向的 run 为准**（当前 = `artifacts/perf/20260928-r34-W02/`，F3 修复后重跑） | D-17 + D-19 + t47/F0 裁决；方案 §13.3「路径证据不足却标已确认」 |
| **路径计数写入者口径（自证，不推给读方）** | `*_messages` / `*_wire_bytes` **只由传输层钩子写**（`nodelet_config.cc:NoteDzFlatPathDelivered`；调用点 `shm_pub_sub_ipc.cc` 的 A/B/预构造段入口 + `shm_pub_sub_ipc.h` 的 `publish_loaned`）⇒ 语义是**实际交付路径**，含探测/预热/停止帧，DDS 档恒为 0（DDS 发布分支不经 `CounterRegistry`）；`*_bytes` 由 **harness** 写，语义是**测量窗口内成功发布的应用逻辑载荷**（= `sent_ok × payload_bytes`），⛔ 与 `*_wire_bytes` 口径不同、不得相加；用例级判定一律用 `summary.csv` / `results.json`（不经 CounterRegistry）。`counters.json:counter_scope` 逐 ID 声明写入者与可加性 | D-17 追加② |
| **中间轮次留痕** | 我为省磁盘清理过 `r01…r19` 的**目录**，其中 **r18 属未留痕清理**（性质与 W08 证据覆盖同类）。已登记：`W02_中间轮次留痕与r18去向.md` + 控制台日志归档（随现行 run 的 `superseded/`）+ `manifest.json:superseded_runs`；并由 W12 收录进《运行与回滚》已知限制 | 方案 §12「只追加」；D-17 追加要求 |
| **⚠️ 复核轮必须带 `--purpose=verification` + `--citation-authority=<证据 run>`**（t50/R-3） | **认领权威是显式动作**：只有 `--purpose=evidence` 才让 `manifest.currently_citable` 指向本 run；**不带参数**（或 `--purpose=verification/experiment` 且未给 `--citation-authority`）⇒ `currently_citable` 写 **`null`** 且 `run_identity.authority_undeclared=true`。⛔ 修复前的「默认抢指针」已移除 —— 它为**第 1 类缺陷（引用链断裂）的复发形态**：一次手滑重跑就悄悄换掉权威指针。**可机械核对**：① `python3 -c "import json;print(json.load(open('<run>/manifest.json'))['run_identity'])"` 里 `purpose_explicit` 必须为 `true`；② 复核轮跑完后证据 run 的 `manifest.currently_citable` 与 `manifest.json` 的 sha256 **必须不变**。脚本入口：`W02_PURPOSE` / `W02_CITATION_AUTHORITY` 环境变量 | t48/R-3 + t50 收口 |
| **⭐ 任何引用必须是 `currently_citable` 指向的 run**（t47/W02-F0） | `manifest.json:currently_citable` 是**唯一权威指针**；`current_run_is_evidence=false` 的 run（复核/实验轮）会把指针指回证据 run。引用旧 run（`superseded_runs.non_current_runs` 里逐条登记了原因）必须**同时标「非现行 + 原因」**。⛔ 反面教材：t25 复核合同引用了 r19 的 `cells=269896/skipped=26`，而现行已是 r23（269852/37）——**引用链断裂会让下游拿错数字** | t47 教训；方案 §11「不得从一项事实推导另一项事实」 |
| **三条纪律**（t47） | ① **现行 run 唯一权威指针**（见上）；② **同一事实只有一处判定逻辑**（`failure_reasons` 只在 `run_one_case()` 产生，summary/results/verdict 只读）；③ **零值三态可区分**：`0`=已采集且为零 / `null`=未采集或分母为零（CSV 空字段）/ `未接线`=有定义无生产者（其 0 不可读作「未发生」）。三条均写入 `manifest.evidence_discipline` | t47 纪律要求；W10-F7 的同型教训 |

**下游必须逐条引用的四条限制**（队长已要求 W11 落实）：① `cyc-iox` 路径证据"未确认"；
② DDS 档不采集 wire 字节；③ `blocking` 档两种内部等待机制不同构；④ 单次运行无置信区间。

---

## 5bis. 撤回与更正（t47 收口；含队长错误记录的撤回）

| # | 原表述 | 更正 | 依据 |
|---|---|---|---|
| 1 | `via_view > 0` 是 **B 档专属判据** | **撤回**：消费者侧 A/B **同形**（各 `via_view=13500`、`via_object=0`，10 轮 full 合计）⇒ 可分性**只在发布侧**（`wire_bytes_source` / W03 路径条数）。⛔ 不得用 `via_view` 作 B 的唯一判据 | 队长独立复算 + 本轮实测对照，见《结果格式与使用》§7.1 |
| 2 | 「本仓**只存在一套** W02 基准」 | **撤回**：实际**两套** —— W02 `xproc_benchmark`（合并 `pub_dzflat`）与 W08 `test_w08_dzflat_ab`（A/B 分开计数，`grep xproc_benchmark` 零命中）。⛔ 不得用 W02 的合并读数冒充 A/B 可分 | 队长已记录该错误；《结果格式与使用》§7.3 给出两套基准的口径差异与跨包引用规则 |
| 3 | 失败量「只落列」即可视为"未静默排除" | **改实现**（⛔ 不接受只改措辞）：`late`/`backlog`/`send_blocked`/`abnormal` 进判定通路 + `bad_header` 补列 + 六个阈值写入 manifest | 队长裁决；前后对照见现行 run 的 `f3_before_after.md` |
| 4 | B 档 warmup 帧的形态（本轮新发现） | **修复**：warmup 帧与测量帧**同形**（借样 → 三段分配 → 写时间戳头）。修前每条 warmup 帧都被记 `bad_header`+`abnormal`（实测 **686–1210/case**（跨 r19/r20/r23 共 45 条 dzflat-b 记录；见 `r4_bad_header_inventory.txt`）），而该量在补列之前**只在子进程日志里**，对判定完全不可见 | t47 新发现；《结果格式与使用》§7.2 |

---

## 6. 回滚

本工作包全部为**新增**文件 + `test/CMakeLists.txt` 追加段：回滚 = 删除新增文件并撤销
`test/CMakeLists.txt` 的追加段（或直接 `git checkout test/CMakeLists.txt`）。
不影响产品运行路径，不需要重启服务。
