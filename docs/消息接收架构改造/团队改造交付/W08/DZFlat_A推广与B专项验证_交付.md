# W08 DZFlat A 推广与 B 专项验证 —— 交付

> ## 🚨 失效提示（2026-09-28 登记；**2026-09-30 t71 扩大范围 + 换数**）
>
> **失效范围 = §0 / §1.2 / §2.5 / §3 / §5**（2026-09-28 版只写了后三个），**以及本文档中一切由 `20260928-r05..r10-W08-*` 派生的数值与结论**：
> 那 6 个 run 的 `samples.csv` 被**两轮覆盖** —— **20:22:4x** 与 **20:31:18–20:31:21**（时间线取证见 §10.1.1）。
> 原因：本包新增用例的**默认落盘路径**在无 `W08_ARTIFACT_ROOT` 时会回推仓库根、且落盘用
> `fopen(...,"wb")` 截断 ⇒ 任何一次直接运行 `build/bin/test_w08_dzflat_ab` 都会就地覆盖那 6 个 run 的
> `samples.csv`。**六个 run 的原始样本在两轮之后全部不可复原**（r05/r06/r08 丢失，r07/r09/r10 亦被覆盖），
> 其中 r09/r10（DZFlat B）的 transport 中位一度被覆盖成原值的 20~27 倍 —— 若被引用会直接毁掉
> 「B 消除最后一次拷贝」的结论。（⚠️ 这里的"倍"是**证据被污染的程度**，⛔ 不是任何性能倍数声明；本包全文无性能倍数结论。）
>
> ⚠️ **原 banner（2026-09-28 版）只声称失效范围为 §2.5/§3/§5，漏报了 §0 与 §1.2** ——
> 而那两处**正是全文被引用最多的数值**（§0:61 的发布/e2e 路径数、§0:62 的比值、§1.2:80-82 的三档表）。
> 这属于"标了失效却仍让读者能引用失效数"的反模式：**t71 已把 §0/§1.2 的数值整体换成 r11..r16 的现行值**
> （逐值新旧对照见 §10.5），⛔ 不再保留任何 r05..r10 派生数。
>
> **当前有效来源**：`artifacts/perf/20260928-r11..r16-W08-*/`（**新 run_id**，见 §2.5 与 §10），
> 汇总 `perf_summary_r11_r16.{json,md}`，逐值复算方式见 §10.5 的**内联命令块**（自包含，无需外部脚本）。
> **污染前的派生记录**（仅作交叉核对，⛔ 不再作结论来源）：`perf_summary.json` / `perf_summary.md`（19:57:24）。
> **已根治**：默认落盘改为 `build/test-scratch/w08`（不可能写到 `artifacts/`）+ 拒绝覆盖既有 `samples.csv`；
> 反事实验证见 §10.3 —— ⚠️ 该证明的边界是「**20:34:04 之后编译出的二进制**不再写仓库」，
> ⛔ **不是**"历史从未被覆盖"。
> **纪律**：D-12 期间 W08 数值一律不得进入 W11 或交付结论；**t21 已交付后，引用限制改为「只可引用 r11..r16」**；
> **t71 追加**：⛔ 不得从 `perf_summary.json`（19:57 派生记录）反推或用其数值回填本文档任何一处。

| 项 | 值 |
|---|---|
| 工作包 | **W08**（P1，DZFlat A 推广与 B 专项验证） |
| 负责人 | socket与数据面负责人 |
| attempt | 交付 `e0b4ff9b-5887-4dd7-976d-db164d43f883`（task `t9`）；污染收口 `5095b429-fb05-4dd3-af4d-c41ed4ebac92`（task `t21`） |
| 基线 | `e800ccc496ac710b711c9346709e86a148c41241`（2026-09-28） |
| 改动面 | 4 文件、**+127/−4**（`src/dzIPC/shm_pub_sub_ipc.cc` / `.h`、`include/dzIPC/common/nodelet_config.{h,cc}`）+ 用例文件 `test/test_w08_dzflat_ab.cpp` |
| 性质声明 | ⛔ **仅计数器调用点、无发布/接收行为变更**（队长 D-3 有界例外，见 §3）；用例侧新增安全落盘与防覆盖闸（§10） |
| 独立补丁 | `artifacts/perf/20260928-r01-W08/w08_counter_only.patch` |
| 证据等级 | **S3 单项验证通过**；⛔ 未宣称 S4/S5（独立验收属 W10/REVIEW，正式性能结论属 W11） |

改动面指纹（sha256）：

| 文件 | 基线（e800ccc） | 现在 |
|---|---|---|
| `src/dzIPC/shm_pub_sub_ipc.cc` | `e6e53db8ea5891336d1bb1aac4fcbc857315cf7ff6fda9822a0dbdaac2d24634` | `de8ab534c8ab261ae00a9c1670adee074101283159173e9b2a405aee3cd50ebc` |
| `include/dzIPC/shm_pub_sub_ipc.h` | `b9ae81ce44c8b420e7734110371f4b88f471a1e979813cacc1da0ee3c08bad42` | `df08aff972f993565bb64efd2efabc80a930c5b9fd6c0b2e522085effe8fc908` |
| `src/dzIPC/common/nodelet_config.cc` | `cfa2e76a25082c5f02e839ae7b37032db0d53472d4e368f56ea214f9214116b0` | `2ac20889b17334890031ad4fc73b47dff8e2cc4c15594821cc71f2a206836f6c` |
| `include/dzIPC/common/nodelet_config.h` | `10746e06c693e1ba333094a6fd16ae6738190a988fb4bf8e9f30c03330d61084` | `ff8fbaf264e2dab5069d9915ae4802795537477c4f8bccb666fe4c7c312f959f` |

> 前两行与队长 D-3 给定的基线指纹**逐字一致**（`e6e53db8…` / `b9ae81ce…`）。

---

## 0. 一分钟结论

**没有从零造 B**：仓内既有的跨进程位置无关用例（`test/test_dzflat_transport.cpp:463`）与 B 的三条契约用例
（`test/test_dzflat_builder.cpp:105/136/352/380/411/444/471`）**原样复用**，本包补的是三件缺失的东西：

| # | 缺口（队长已核实） | 本包补法 | 判决性证据 |
|---|---|---|---|
| ① | 既有 B 证据**不在回归里**（两个 target 从未登记 ctest） | **已收口**：`test_dzflat_transport` / `test_dzflat_builder` 已由 t17 登记（`ctest -N` 第 16/17 项）；本包新增的 `test_w08_dzflat_ab` **已登记为 `ctest` #26**（`RUN_SERIAL TRUE` + `TIMEOUT 600` + `ENVIRONMENT W08_ARTIFACT_ROOT=.../test-scratch/w08`，见 §8-1）；**当前 `ctest -N` 共 29 项**（`ctest --test-dir build -N` 复核） | §5 + `ctest -N` |
| ② | **A/B 计数不可分**（A/B 共用 `NoteDzFlatPublish`） | 按 W03 `counters.h` 的既定接线口补三路径分流计数（**仅计数器点**） | §3 的"每配置断言该路径=N、另两条=0" |
| ③ | 缺**跨进程 + 全量载荷校验 + 逐样本落盘**的可采信配置 | 新增 `test/test_w08_dzflat_ab.cpp`：6 个 run × 120 样本，落盘到 `artifacts/perf/<run_id>/` | §2 的 run 矩阵与逐样本文件 |

**验收**（t9 合同）：

| 验收项 | 结果 | 读数 |
|---|---|---|
| A/B 标记与实际拷贝路径一致（有计数证据，三路径可分） | ✅ | 每个配置断言「该路径 = N，另两条 = 0」：`tlv` ⇒ `tlv=60/a=0/b=0`、`dzflat-a` ⇒ `a=60/tlv=0/b=0`、`dzflat-b` ⇒ `b=60/tlv=0/a=0`；DZFlat 两档 `fallback=0`（无静默回退）。**该可分性只在发布侧成立**：消费端在 A/B 两档同形（`counters_consumer.json` 四个 ID 全 0），见 §3.1。**这组数不来自任何落盘 run**：由**活的 gtest** `W08DzFlatAB.CrossProcessPathCountersAreSeparableByConfig` 在子进程里断言，`60 = kSamples`（`test/test_w08_dzflat_ab.cpp:699`；该用例 `write_evidence=false` ⇒ 不写盘）⇒ 复算命令 `ctest --test-dir build -R w08_dzflat_ab`（当前 `ctest` **#26**）；落盘侧的等价核对见 §2.5 的 `counters_publisher.json`（r11/r12 `tlv=120`、r13/r14 `a=120`、r15/r16 `b=120`，对应 120 样本档） |
| 收益可复现且不破坏兼容消费 | ✅（方向性，正式数值归 W11） | **现行值（r12/r14/r16，`permsg` 档，0.88 MB）**：发布路径 TLV **180.1** → A **20.3** → B **1.3** µs；生产到消费 e2e TLV **726.3** → A **384.5** → B **357.8** µs；兼容消费由 `test_dzflat`(13)、`test_dzflat_rx`(8)、`test_dzflat_sercli`(4) 全绿守。⛔ **本行原先的六个数值全部来自已失效的 r05..r10 派生记录，已整体换成上列现行值**（逐值新旧对照见 §10.5；⛔ 本节不复述旧值，避免被继续引用） |
| 不得宣称「B 级提升 N 倍」 | ✅ 已按机制表述 | 见 §2.5：A→B 在 `transport` 上的**比值本身不作为收益**，一律写成「**只等于省掉一次 0.88 MB 拷贝**」（现行 A→B transport 差 **19.0 µs**）；并给出 **e2e 上 B vs A = −6.9%**（现行值，绝对值 −26.7 µs）；⛔ 全文无"提升 N 倍"式结论。⛔ 本行原先的两个数（一个**倍数**与其配套 e2e 百分比）**均来自已失效的 r05..r10 派生记录，已清出**（复核者已由 19:57 派生记录复算并**逐位吻合**地证其为派生值）；逐值对照见 §10.5，⛔ 本节不复述旧值 |
| 跨进程证据落盘到 `artifacts/perf/<run_id>/`，manifest 标 transport / process_model / work_package | ✅ | 6 个 run 目录各含 `samples.csv`、`samples.jsonl`、`manifest.json`（`transport=dzflat-a\|dzflat-b\|tlv`、`process_model=cross-process`、`work_package=W08`）、`counters_publisher.json`、`counters_consumer.json`、`README.md` |

---

## 1. A：启用策略与正确性/收益（要求 1、4）

### 1.1 A 的适用范围（由**类型兼容**决定，不是由尺寸决定）

- 开关：`dzIPC::EnableDzFlat(bool)`，**进程级、默认 OFF**（W00 §6.2 冻结默认值，本包未改）。
- 触发条件（`shm_pub_sub_ipc.cc::try_publish_dzflat`）：开关开 **且** `msg->dzflat_supported()` **且** 该通道有接收方 **且** chunk 池能借到块。任一条不满足 ⇒ 回落整包 TLV。
- `dzflat_supported()` 由 generator 发射的 Flat 类型决定（`StdImage`/`StdPointCloud` 等**有平坦布局的类型**为 true）；**手写类型与 `GenericMessage` 为 false** ⇒ 恒走 TLV。这就是"类型兼容"边界。
- ⛔ 旧对象接口（`publish(std::shared_ptr<IpcMsgBase>)`）与 TLV 回退**一律保留、未改语义**：`test_dzflat` 13 项、`test_dzflat_rx` 8 项、`test_dzflat_sercli` 4 项全绿。

### 1.2 A 的收益随载荷变化（本包 3 档扫描，中位数，跨进程；⚠️ e2e 非单调）

> ⚠️ **本表已于 2026-09-30（t71）整体换数**：原表三行的 12 个数值**全部来自已失效的 `r05..r10`**
> （复核者核出其中 11/14 在 r11..r16 中 **0/6 命中**）。现行值来源：`r12`（TLV）/ `r14`（A），
> 均为 `permsg` 写法、跨进程、每档 40 样本的中位数；复算命令与逐值对照见 §10.5。

| 载荷 | TLV 发布路径 | A 发布路径 | TLV e2e | A e2e | A 相对 TLV（e2e） |
|---|---|---|---|---|---|
| img66k（66,048 B） | 11.0 µs | 6.6 µs | 69.6 µs | 57.5 µs | −17.4% |
| img262k（262,656 B） | 17.4 µs | 16.7 µs | 149.9 µs | 200.9 µs | **+34.1%** ⚠️ |
| img880k（921,600 B） | 180.1 µs | 20.3 µs | 726.3 µs | 384.5 µs | −47.1% |

**读法（⛔ 不是"随载荷单调增长"）**：A 省掉的是 TLV 的"整包 `serialize()` + `send()` 再拷贝进 chunk"，
所以**发布路径**上的收益随载荷增大（`6.6/11.0`、`16.7/17.4`、`20.3/180.1` ⇒ 越大越明显）。
但**端到端**收益在现行数据上**非单调**：−17.4% → **+34.1%** → −47.1%。
其中 **262k 档不成立且带采样歧义**（见下），⛔ 不得据此写"A 的 e2e 收益随载荷单调增长"。

⚠️ **262k 档的采样歧义（双峰，已登记为待 W11 重采项）**：`r14`（A/`permsg`）img262k 的 40 条 `e2e_ns`
不是单峰 —— 下峰 **8 条**中位 **113.7 µs**、上峰 **32 条**中位 **201.0 µs**
（最大间隙 160.8 µs，落在 267.6 与 428.5 之间）；且**与样本序号强相关**：`seq 40..71` 几乎全在 ≈200 µs 上峰、
`seq 72..79` 全在 ≈113 µs 下峰（逐条读数可用 §10.5 的**第二段内联命令**现场复算，`seq 40..79` 打印出来即为上/下峰的分配）。
⇒ 该档"中位数"取决于两峰配比，**在本轮采样下不是稳定量**；`r12`（TLV）同档亦呈双峰（20/20，140.1 vs 154.7）。
**处置**：本档结论**不成立**，⛔ 不进任何默认启用建议；**须由 W11 按 W00 §8 的多轮交替采样重采**，
并记录采集顺序以分辨"热机/相位效应"与"随机双峰"。⛔ 不得用"只取上峰或下峰"人为凑出单调趋势。

因此 A 的**默认启用范围**由"类型兼容（能不能走）× 载荷与拷贝扫描（值不值得走）"共同决定，
⛔ 不是"≥ 某尺寸就自动开"；现行数据只支持**大载荷档（0.88 MB）**有明确 e2e 收益（−47.1%），
小载荷档收益小（−17.4%），**262k 档不成立**（待 W11）。

### 1.3 ⛔ 不预设「≥64 KiB 自动使用 B」

B（`loan → 应用原地构造 → publish_loaned`）要求**生产者改写构造方式**（不再先建对象再造消息），
是一种**代码级迁移**，不是运行期按尺寸偷换的开关。本包把它做成显式配置（`dzflat-b`）并给出
适配示例（§2.2），⛔没有、也不应该有任何"按尺寸自动切 B"的路径。

---

## 2. B 专项：跨进程 + 全量载荷校验 + 逐样本落盘（要求 2、3）

### 2.1 复用了什么（⛔ 未重造）

| 既有资产 | 位置 | 本包如何用 |
|---|---|---|
| 跨进程位置无关（fork 子进程发布 + 退出码回报走 DZFlat） | `test/test_dzflat_transport.cpp:463` | **原样保留**为正确性回归；本包另加"逐样本落盘"的跨进程对照（互补，不替代） |
| 指针区间断言（反"临时缓冲假零拷贝"） | `test/test_dzflat_builder.cpp:105` | **原样保留**；本包不做等价重复断言 |
| 就地构造往返 / 未发布借样即归还 / move 后仍可用 / 超预算 / 开关关 / 无订阅者 | `test_dzflat_builder.cpp:136/352/380/411/444/471` | **原样保留**，并在 §4 里映射到"B 失败契约"的每一条 |
| 同进程 A/B 计时（151.6/165.5 vs 113.8/116.1 µs） | `test/dzflat_tx_benchmark.cpp` Step 3 | 只作**方向性对照**；⛔ 不当跨进程验收数据（§2.5） |

### 2.2 B 的适配示例（本包实际写法，逐字来自 `test/test_w08_dzflat_ab.cpp` 的生产端）

```cpp
auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(bytes + 256);   // ① 显式借样(容量上界由调用方给)
if (!lo.valid()) { /* ② 失败是常态: 回退普通 publish(...), 见 §4 契约 */ }
lo->set_width(w); lo->set_height(h); lo->set_step(step); lo->set_encoding("rgb8");
auto px = lo->alloc_data(bytes);                             // ③ span 指向共享内存本身
for (std::uint32_t k = 0; k < bytes; ++k) px[k] = pattern_byte(seq, k);  // ④ 应用**原地**填
pub.publish_loaned(std::move(lo));                           // ⑤ 封口并投递
```

⛔ 这里没有用任何内部 `loan()` 代跑；调用点就是**应用侧**的 `loan → alloc_data → publish_loaned`，
与 `path_evidence.h` 对 DZFlat B 的要求（call site 必须含 `publish_loan`）同源。

### 2.3 协议：**发一条取一条**（规避既存缺陷，⛔ 不是本包修的）

已知既存缺陷（`docs/dzflat_shm.md §9.5` 末尾）：大消息连发且订阅方不取 ⇒ chunk 池（32 块/档）耗尽
⇒ `send()` 退化 64B 分片 ⇒ 环覆写 ⇒ 重组拼错 ⇒ **段错误**。

本包在协议层把在途数钉在 **1**：生产端每发一条，**等消费端完整校验并 ack 之后**才发下一条
（`PubRecord` 经管道回传 + ack 管道回程）。因此本包的读数**不受**该缺陷影响；⛔也**不用**该缺陷解释
任何失败（本包没有失败读数需要解释）。

> **口径统一（t71）**：对该既存缺陷，本包的全部立场是「**规避、未修**」——
> 与 §8「已知限制」第 6 条**同一表述、互相链接**（见该条）：本包以"发一条取一条"**规避**它，
> ⛔ **没有修**、也**不声称已修**。归因与修复属 W09/W10（另注：该缺陷与 W09 的 `note_pool_exhausted`
> 观测点同源，但"有观测"不等于"已修"）。

### 2.4 三个实验组与逐样本字段（方案 §10.7 / W03 `field_schema.h`）

每条样本一行，字段与表头**全部**取自 W03 的 `SampleRecord`/`sample_csv_header()`（未自造并列字段）：

| 实验组 | 派生字段 | 取点 |
|---|---|---|
| 传输机制 | `transport_ns` | `transport_done_ns − publish_enter_ns`（生产端） |
| 通知与交付 | `delivery_ns` | `app_obtained_ns − transport_done_ns` |
| 完整读取 | `app_read_ns` | `fully_consumed_ns − app_obtained_ns` |
| 生产到消费 | `e2e_ns` | `fully_consumed_ns − produced_ns`（**跨进程** CLOCK_MONOTONIC） |

- 载荷模式 `data[i] = (seq*131 + i*7) & 0xFF`：**与序号相关**，每条都变；消费端**整段重算比对**
  （`payload_checksum_ok`），因此"复用固定载荷掩盖旧帧/内容损坏"会被抓成 false。
- 三个尺寸档各 40 条：`img66k` / `img262k` / `img880k`（0.88 MB 与文档同档）。
- 跨进程时基一致性：两端都用 `dzIPC::measure::monotonic_now_ns()`（CLOCK_MONOTONIC），
  跨进程一致性由 W03 的 `test_w03_measurement` 覆盖。

### 2.5 六个 run 的矩阵与读数（中位数，µs）—— **当前有效版：r11..r16**

> ⚠️ **本表已于 2026-09-28 由 r11..r16 重跑替换**（原 r05..r10 表因证据被覆盖而失效，事件登记见 §10）。
> 数值来源：`artifacts/perf/20260928-r11..r16-W08-*/samples.csv`，汇总脚本
> `artifacts/perf/20260928-r01-W08/analyze_r11_r16.py`（产出 `perf_summary_r11_r16.{json,md}`）。

| run_id | transport | 写法 | transport 中位（66k/262k/880k） | e2e 中位（66k/262k/880k） | app_read 中位（66k/262k/880k） |
|---|---|---|---|---|---|
| `20260928-r11-W08-tlv-prebuilt` | tlv | 对象预先构造 | 11.0 / 30.5 / 61.1 | 67.9 / 239.9 / 504.0 | 14.5 / 53.2 / 186.7 |
| `20260928-r12-W08-tlv-permsg` | tlv | 每消息新构造 | 11.0 / 17.4 / **180.1** | 69.6 / 149.9 / **726.3** | 14.6 / 53.1 / 185.2 |
| `20260928-r13-W08-dzflat-a-prebuilt` | dzflat-a | 对象预先构造 | 3.9 / 7.0 / 20.7 | 34.1 / 111.4 / 375.1 | 14.4 / 53.0 / 184.3 |
| `20260928-r14-W08-dzflat-a-permsg` | dzflat-a | 每消息新构造 | 6.6 / 16.7 / **20.3** | 57.5 / 200.9 / **384.5** | 14.6 / 53.1 / 184.7 |
| `20260928-r15-W08-dzflat-b-prebuilt` | dzflat-b | 预置字段 | 2.2 / 2.2 / 2.4 | 36.4 / 121.9 / 408.6 | 14.6 / 53.2 / 186.2 |
| `20260928-r16-W08-dzflat-b-permsg` | dzflat-b | 应用原地构造 | 1.3 / 1.3 / **1.3** | 31.8 / 104.7 / **357.8** | 14.6 / 53.0 / 187.6 |

> ⚠️ **上表最右列「污染前同名 run 的 transport 中位」是 §10.1 要求的交叉核对列**（取自 19:57 派生记录），
> ⛔ **不是现行值、不得引用入任何结论**；现行值只看左侧各列（r11..r16）。
> 该列保留的理由是 §10.1「交叉核对源」的追溯义务与 t71 的"不得删除如实标记"纪律。

**机制读数（大档 0.88 MB，`permsg`）**：
- 发布路径：TLV 180.1 → A 20.3 → **B 1.3 µs**。A→B 的差 ≈ **19.0 µs**，就是"对象 → chunk 那一跳 0.88 MB 拷贝"。
- 生产到消费：TLV 726.3 → A 384.5 → **B 357.8 µs**（B vs A ≈ **−26.7 µs**，与上面那一跳同量级）；
  余下全是应用自己填/读载荷的时间。
- **B 的 transport 三档基本恒定**（2.2/2.2/2.4 与 1.3/1.3/1.3，与载荷大小无关）是"省掉最后一次拷贝"的签名；
  而 A 随载荷线性增长（6.6 / 16.7 / 20.3 与 3.9 / 7.0 / 20.7）。
- **完整读取三档基本相等**（880k 均 ≈186 µs）—— 正是"B 可避免复制，但应用填充/读取大载荷仍需时间"的直接读数。
- ⛔ **不得表述为「B 级提升 N 倍」**（t71 起**本文档连那个比值本身都不再写出**）：
  `transport` 是**发布 API 内部**的耗时，A→B 的差 = **19.0 µs**，它**只等于省掉一次 0.88 MB 拷贝**；
  同一批数据里 e2e 上 B 相对 A 只有 **−6.9%**（绝对 **−26.7 µs**）量级。
- ⛔ 本包**单轮**采样且同机有并发负载（load avg 一度 149/665，见 §10）：正式结论必须由 W11 按 W00 §8 的
  5 轮交替 + 中位数判定规则重采，并绑定已通过正确性验收的提交。同进程的 113.8/151.6 µs **不可**与本表换算或互推。

**轮间离散度（本包自检，不是 W11 结论）**：同一配置独立跑第 2 轮（落 scratch，`test_w08_dzflat_ab_r11_r16_round2.log`），
`transport` 中位的轮间差为：B 档 **+0.1 µs（三档全部）**、A 档 880k **+0.1 µs**、TLV 档 880k **+5.1 µs**；
小档（66k/262k）在 A/TLV 配置上出现过 −2.8 ~ −9.8 µs 的漂移，与并发负载一致。
⇒ **B ≤ A ≤ TLV 的排序与"B 与载荷无关"的签名在两轮间稳定**；绝对数值需 W11 重采。

每 run 的生产端路径计数（生产进程自报，`README.md`）与预期一致（r11..r16）：

| run | dzflat_a_messages | dzflat_b_messages | tlv_messages | dzflat_fallback | prebuilt | dzflat_wire_bytes |
|---|---|---|---|---|---|---|
| r11/r12（tlv） | 0 | 0 | 120 | 120 | 0 | 0 |
| r13/r14（dzflat-a） | **120** | 0 | 0 | **0** | 0 | 50,022,720 |
| r15/r16（dzflat-b） | 0 | **120** | 0 | **0** | 0 | 50,022,720 |


---

## 3. 三路径可分计数（D-3 有界例外；⛔ 仅计数器点、无行为变更）

### 3.1 缺口与修法

既有 `dzIPC::detail::NoteDzFlatPublish(bool)` 只分"走了 DZFlat / 回落整包"；A（`shm_pub_sub_ipc.cc`）与
B（`shm_pub_sub_ipc.h`）共用它，另有 `publish_prebuilt_segment` 第三入口 ⇒ **A/B 不可分**，而
W03 已定义 `tlv_messages/dzflat_a_messages/dzflat_b_messages` 且 `path_evidence.h` 要求三路径计数必须分开。

**修法**：在**我的文件** `nodelet_config.{h,cc}` 追加一个**转发层**钩子，把计数直接落到 W03 的
`measure::CounterRegistry` 同名 ID（热路径一次 relaxed `fetch_add`，无锁、无分配、无日志）：

| 入口 | 计数点 | 落到 |
|---|---|---|
| A | `shm_pub_sub_ipc.cc::publish_blocking` / `publish_for_sniffer` 的 DZFlat 成功分支 | `dzflat_a_messages` + `dzflat_wire_bytes` |
| A 回退 | 同两处的 TLV 发送**成功**分支 | `tlv_messages` + `tlv_wire_bytes` |
| B | `shm_pub_sub_ipc.h::publish_loaned` 成功分支 | `dzflat_b_messages` + `dzflat_wire_bytes` |
| 预构造段 | `publish_prebuilt_segment` 成功分支 | 本模块 `DzFlatPrebuiltSegmentCount()` + `dzflat_wire_bytes` |

> ⚠️ **可分性只在发布侧**（t71 明确登记，⛔ 不得外推）：上表四个入口**全部在生产端**。
> 消费端拿到的都是同一形状的"一条消息"，**不区分它当初是 TLV / A / B 来的** ——
> 实测 `counters_consumer.json` 在 `r13/r14`（A）与 `r15/r16`（B）**两档同形**（四个 ID 全 0，
> 逐 run 读数见 §2.5 表下方的 `counters_publisher.json` 对照）。
> ⇒ 「三路径可分」这个结论**只对发布侧成立**；⛔ 不得据此写"A/B 在读侧也可分"。
> 判据命令：`python3 -c "import json;print(json.load(open('artifacts/perf/20260928-r14-W08-dzflat-a-permsg/counters_consumer.json'))['counters'])"`（r15 同理）。

### 3.2 行为不变的证据

- 为计数在 `publish_blocking`/`publish_for_sniffer` 把原来两个 `return` 收敛成一个局部 `bool sent`
  —— 控制流与返回值**逐位不变**（补丁可逐行复核）。
- `publish_loaned` 只在 `release()` 前多取一个 `seg_bytes` 用于计数。
- 既有计数器语义/取值不变：`DzFlatPublishCount/DzFlatFallbackCount` 未改口径（DZFlat 两档 `publish=120/fallback=0`）。
- 相关回归 **61 OK / 0 FAILED**（9 个用例文件，§5），热路径硬闸见 §5。

### 3.3 本包**未**接线的计数（⛔ 不以 0 冒充已测）

- **逻辑载荷字节**（`tlv_bytes`/`dzflat_a_bytes`/`dzflat_b_bytes`）：TLV 侧没有通用的"应用逻辑字节"API，
  拿 `serialize()` 后的大小充数会把 wire 与 payload 混用（W03 明令两者不得混用）⇒ 留 0 并在此登记。
- **回退原因细分**（`fallback_pool_exhausted` / `fallback_type_incompatible` / `fallback_oversized`）：
  需要 `try_publish_dzflat` 返回原因（改签名 = 行为/接口变更），超出 D-3 的"仅计数器点"边界 ⇒ ⛔未做。
- **预构造段入口**在 W03 的三路径 ID 里**没有对应项**（既非 A 也非 B）⇒ 条数记在本模块，
  已在 manifest 的 `w08_scope_note` 与本节登记，⛔ 未并进 A/B 冒充。
- **nodelet 同进程快路径**（`clone + push(shared_ptr)`）不经 wire，不在这三条路径内，未计数。

---

## 4. B 失败时的应用处理契约（要求 5）

| 场景 | 契约 | 承重用例 |
|---|---|---|
| 开关未开 | `loan()` 返回**无效**对象；应用回退普通 `publish()`（该次记 TLV） | 既有 `test_dzflat_builder.cpp:444 LoanFailsWhenSwitchOff` |
| 无接收方 | 同上（借样被拒，不是错误） | 既有 `:471 LoanFailsWithoutSubscriber` |
| **超预算** | `alloc_*` 返回**空 span**、Writer 转 `!ok`、`publish_loaned` 返回 false 并**归还 chunk**（⛔ 不写出坏段） | 既有 `:411 ExceedingBudgetFailsCleanly` + 本包 `FailedLoanIsReturnedAndApplicationFallbackStillDelivers`（新增：失败后**能立刻再借到** ⇒ 未漏 chunk） |
| **发布失败/回退清理** | 应用回退普通 `publish()`，消息**不许丢**；这一次回退记在 **A/TLV**（实际路径），⛔ 不记在 B | 本包新增用例：超预算失败 → 回退 `publish` → 送达成功，且 `dzflat_a_messages +1`、`dzflat_b_messages` 不变 |
| 持样跨生命周期 | `LoanedMessage` 是 **move-only RAII**：未投递的借样在析构时归还；`publish_loan` 失败时由其内部 discard（应用不得重复归还） | 既有 `:352 UnpublishedLoanIsReturnedOnDestruction`（连借 160 次不发布全过）与 `:380 MoveKeepsBuilderUsable` |

**一句话契约**：B 的失败**是常态而非异常**；应用的义务是"`loan` 拿不到 / `publish_loaned` 返回 false 时，
回退到普通 `publish()`"，chunk 的归还**由 RAII 保证**，应用不得手工 `discard` 已交给 `publish_loan` 的借样。

---

## 5. 回归与热路径硬闸

| 测试 | rc | OK | FAILED |
|---|---|---|---|
| `test_dzflat_transport`（含跨进程位置无关） | 0 | 9 | 0 |
| `test_dzflat_builder`（B 三条契约） | 0 | 9 | 0 |
| `test_dzflat` | 0 | 13 | 0 |
| `test_dzflat_rx` | 0 | 8 | 0 |
| `test_dzflat_sercli` | 0 | 4 | 0 |
| `test_loan` | 0 | 10 | 0 |
| `test_chunk_hold` | 0 | 3 | 0 |
| `test_pool_exhaust_observability` | 0 | 2 | 0 |
| **`test_w08_dzflat_ab`**（本包新增） | 0 | 3 | 0 |
| 合计 | — | **61** | **0** |

新增用例耗时：`CrossProcessPathCountersAreSeparableByConfig` 1816 ms、
`CrossProcessPerSampleEvidenceIsWrittenAndPayloadFullyVerified` 3628 ms、
`FailedLoanIsReturnedAndApplicationFallbackStillDelivers` 351 ms。

热路径硬闸（本改动在发布热路径上，虽不在 `include/libipc/**` 也一并跑）：`bash docs/hotpath_gate.sh build` **exit 0 过闸**，
读数 `hotpath_gate_after_w08.txt`：

| 门 | 改前（W00 基线时） | W07 后 | **W08 后** | 判据 |
|---|---|---|---|---|
| 门 1：SHM pub/sub 1 MiB 吞吐（下限 700 msg/s） | 1079 | 705 | **722** | 过闸 |
| 门 1：SHM pub/sub 64 B 吞吐（下限 80000 msg/s） | 86051 | 170401 | **172361** | 过闸 |
| 门 2：8 项邻接回归 | 全 OK | 全 OK | **全 OK** | 过闸 |

- 本改动加的是"每条发布 1 次 relaxed `fetch_add`"级别的计数：**64 B 档（发布调用率最高）172361 msg/s
  vs W07 后 170401**，看**不出**计数开销的痕迹。
- 1 MiB 档 722 与 W07 的 705 同档，且低于 W00 改前的 1079 —— 与 W07 交付里记录的**同机并发负载**
  （load avg≈3、7 名成员并行）一致，⛔ 本包不把它当作回归，也不当作收益；正式性能数值归 W11。


---

## 6. 与 W09 共同决定 DZFlat 默认策略（要求 6）

本包给出**输入**，不单方改默认：

1. **全局默认维持 OFF**（W00 §6.2 冻结；本包未改 `g_dzflat_enabled` 默认值）——因为 DZFlat 段对**未升级**的
   订阅方是不可解析的（会按 TLV 读段尾 `msg_id` 而丢弃），必须由部署方在确认两端版本后显式打开。
2. **A 的启用范围**：由类型兼容（`dzflat_supported()`）× 载荷与拷贝扫描（§1.2）决定；本次扫描显示
   A 的端到端收益**并非随载荷单调增长**（现行值：66 KiB −17.4% / 262 KiB **+34.1%（该档双峰，不成立）** / 0.88 MB −47.1%，见 §1.2）；只有**发布路径**上的收益随载荷增大。⛔ 不得据此建议"≥某尺寸自动开"。
3. **B 的启用**：代码级迁移（生产者改写构造），⛔ 不做按尺寸自动切换；是否纳入默认建议需 W09 的容量/背压
   结论（32 块/档的持有预算、慢消费者与 adopt 配额）配合。
4. 已把上述三点与"capacity 再变更须沿用同一段名/版本机制"的约束发给共享层负责人。

---

## 7. 证据索引（`artifacts/perf/20260928-r01-W08/`）

| 文件 | 内容 |
|---|---|
| `command.txt` | 复现命令（构建/用例/落盘位置/汇总/回归/热闸） |
| `w08_counter_only.patch` | 独立补丁（4 文件 +127/−4） |
| `fingerprints.txt` | before/after sha256 + diffstat |
| `test_w08_dzflat_ab.log` | 3 个用例的 gtest 日志 |
| `regression/*.log` | 9 个用例文件的逐项日志（61 OK） |
| `perf_summary.md` / `perf_summary.json` | ⚠️ **污染前的历史派生记录**（19:57:24，对应已被覆盖的 r05..r10）—— 保留作交叉核对，⛔ 不再作结论来源 |
| `perf_summary_r11_r16.md` / `.json` / `analyze_r11_r16.py` | **当前有效**汇总（新一版 run）与可复算脚本 |
| `guard_before_sha256.txt` / `guard_after_sha256.txt` / `bare_run_after_guard.log` | §10 反事实验证：裸跑前后仓库 6 个样本 sha256 逐字不变 |
| `test_w08_dzflat_ab_r11_r16.log` / `..._round2.log` | 新一版 run 的归档轮与独立重复轮日志 |
| `hotpath_gate_after_w08.txt` | 热路径硬闸读数 |
| §10.5 的两段**内联复算命令** | t71 换数与 262k 双峰取证的**自包含**复算方式（heredoc，无新增文件；只读 `r11..r16` 的 `samples.csv`，⛔ 不读 `perf_summary.json`） |
| `artifacts/perf/20260928-r11..r16-W08-*/` | **当前有效**：6 个 run 的 `samples.csv`/`samples.jsonl`/`manifest.json`/`counters_publisher.json`/`counters_consumer.json`/`README.md` |
| `artifacts/perf/20260928-r05..r10-W08-*/` | ⚠️ **已污染、已失效**（两轮覆盖：20:22:4x 与 20:31:18，见 §10.1.1），按 §12 只追加不删除 —— 保留以备追溯，⛔ 不得引用其数值 |

外部既有证据（只引用不复制）：`test/dzflat_tx_benchmark.cpp`（同进程 Step 3）、
`test/test_dzflat_transport.cpp:463`、`test/test_dzflat_builder.cpp:105/136/352/380/411/444/471`、
`docs/dzflat_shm.md §9.5`（含既存池耗尽缺陷）、`include/dzIPC/measure/{counters.h,field_schema.h,path_evidence.h}`。

---

## 8. 已知限制（⛔ 不假装闭环）

1. **`test_w08_dzflat_ab` 已登记（#26）—— 缺口已闭合**（队长 2026-09-28 复核后更正；⛔ 本条 2026-09-30 由 t71 把"待登记"改成"已登记"）：
   `test_dzflat_transport` / `test_dzflat_builder` 已由 **t17 落地**，"现有 B 证据不在回归里"这个缺口**已收口**；
   本包新增用例已由架构负责人登记（**ctest #26**，
   带 `RUN_SERIAL TRUE` + `TIMEOUT 600` + `ENVIRONMENT W08_ARTIFACT_ROOT=.../test-scratch/w08`）。
   ⚠️ 队长实测更正：共享段类用例必须用 **`RUN_SERIAL`**，⛔ 不是 `RESOURCE_LOCK`（后者挡不住**未声明该锁**的邻居）。
   ⚠️ 但该 `ENVIRONMENT` **只保护 ctest 路径**，不保护直接运行二进制 —— 这正是 §10 污染事件的入口，已由本包根治。
2. **回退计数族当前无人写入**（队长全仓 grep 确认 `fallback_total` 及五个原因 ID 在 `src/` 下 0 个调用点）：
   后果是 tlv 档出现 `counters_publisher.json` 的 `fallback_total=0` 与 README 自报 `dzflat_fallback=120` 的
   **含义并列而非矛盾**（后者是既有 nodelet_config 口径的"回落整包条数"，前者是 W03 原因族、未接线）。
   已在 §3.3 与 manifest 的 `w08_scope_note` 声明"其值恒 0，不代表已测"；**收口口径由 W09（t19）定义**
   （DZFlat 显式关闭时不存在"回退"，只是路径选择）。⛔ 本包不改 W03/未接线计数。
3. **正式性能数值不属本包**：本包单轮、同机并发负载；W11 必须按 W00 §8 规则重采，且绑定已通过正确性验收的提交。
   D-12 生效期间本包数值一律不得进入 W11 或交付结论。
4. **逻辑载荷字节与回退原因计数未接线**（§3.3），需要 W03/W09 决定是否扩 API。
5. **预构造段入口没有 W03 路径 ID**（§3.3）：条数记在本模块，报告里不与 A/B 混算。
6. **既存池耗尽缺陷未修**（`docs/dzflat_shm.md §9.5` 末尾）：本包靠"发一条取一条"**规避**它，
   ⛔ **未修、也未声称修好** —— 与 §2.3 的口径**逐字一致**（该节末尾有同一表述与互链）。
7. **nodelet 同进程快路径未计入三路径**（不经 wire）。
8. **B 的 `prebuilt` 写法与 `permsg` 在结构上等价**（都是应用写进 chunk），两者读数差 ≈ 0 属预期；
   该轴只对 TLV/A 有意义——这一点已在 §2.5 的表格里如实呈现，⛔ 不用它造"差异"。
9. **r05..r10 的原始样本不可复原**（§10）：污染前的**派生**记录（`perf_summary.json`）仍在，
   但它不是原始样本，⛔ 不能用它反推逐样本数据。

---

## 9. 回滚

```bash
# 计数器分流是本包唯一改动，回滚即整体还原（发布/接收行为从未变过）
git checkout e800ccc496ac710b711c9346709e86a148c41241 -- \
    src/dzIPC/shm_pub_sub_ipc.cc include/dzIPC/shm_pub_sub_ipc.h \
    src/dzIPC/common/nodelet_config.cc include/dzIPC/common/nodelet_config.h
cmake --build build --target ipc -j"$(nproc)"
# 新增用例文件与证据目录可独立保留（test/test_w08_dzflat_ab.cpp、artifacts/perf/20260928-r*-W08-*）
```

⚠️ 回滚后三路径计数回到"不可分"状态：`path_evidence.h` 的 DZFlat B 路径将**只能标"未确认"** ——
这正是本包要修的东西，故 ⛔ 不建议单独回滚 `nodelet_config` 而保留调用点（会编译失败）。

```bash
# 用例侧增量（安全落点 + 防覆盖闸）可单独回滚 —— ⛔ 不建议：回到"默认写仓库"正是本次污染的成因
# （用例文件不在 HEAD 里，回滚 = 手工把 artifact_dir_for()/防覆盖闸摘掉，或写回旧版本）
# 证据侧：⛔ 不要删 r05..r10（按 §12 保留以备追溯），也不要改 perf_summary.json（污染前派生记录）
```


## 10. 证据污染事件与根治（D-12 收口）

### 10.1 事件

| 项 | 内容 |
|---|---|
| 发现者 | 架构负责人在登记 `test_w08_dzflat_ab` 时发现落盘路径风险；队长独立复核后确认范围更大并建 **t21** 指派本包收口 |
| 危险机制 | `run_id` 硬编码 `r05..r10` + `repo_root()` 在**无 `W08_ARTIFACT_ROOT`** 时由 `/proc/self/exe` 回推**仓库根** + 落盘 `fopen(...,"wb")` **截断** ⇒ 任一次直接运行即就地覆盖 |
| 覆盖时间点 | 见 §10.1.1：**共两轮** —— **20:22:4x** 与 **20:31:18–20:31:21**（仓库 6 个 `samples.csv` 的 mtime 现值即第二轮） |
| 受影响文件 | `artifacts/perf/20260928-r0{5,6,7,8,9,10}-W08-*/samples.csv`（各 run 的 `samples.jsonl`/`counters_*.json`/`README.md` 同批被重写） |
| 不可复原范围 | **r05/r06/r08/r07/r09/r10 六个 run 的原始样本全部不可复原**；其中 r09/r10（DZFlat B）的 transport 中位一度被覆盖成原值的 20~27×（⚠️ **污染程度**，非性能倍数声明） |
| 交叉核对源 | `artifacts/perf/20260928-r01-W08/perf_summary.json`（19:57:24）—— 覆盖前的**派生**中位（r05 6.62/17.15/62.71、r06 6.35/17.15/179.80、r07 3.85/6.96/20.24、r08 3.78/6.91/20.10、r09 1.35/1.36/1.38、r10 1.39/1.38/1.42）。⛔ 保留不改，但只作历史对照 |
| 违反条款 | 方案 §12「结果目录只追加、重跑生成新目录」 |

#### 10.1.1 时间线取证：**污染共两轮，我的 guard 起点晚于第二轮**（队长复核补正）

| 时点 | 仓库 6 个 `samples.csv` 聚合 sha256 | 来源 / 事实 |
|---|---|---|
| 19:57:24 | （派生值记录，非样本）`perf_summary.json` 生成 | 我的第一次归档（当时 r05..r10 样本还是**原版**） |
| **20:22:4x** | r05–r10 六个 `samples.csv` mtime = 20:22:41/42/43 | **第一轮覆盖**（直接调用旧二进制；晚于 20:18 的 ctest 重定向配置） |
| **20:29**（队长快照） | `5a11365318d2aedd…` | 队长上一轮取证时采样 |
| **20:31:18–20:31:21** | 六个文件 mtime **全部更新**（20:31:18.29 / 18.95 / 19.56 / 20.17 / 20.78 / 21.38） | **第二轮覆盖**（旧二进制、默认写仓库根）—— 现值即这一轮 |
| 20:32:28–20:32:31 | （新 run r11..r16 落仓库） | 我用 `W08_ARTIFACT_ROOT=<repo>` 显式归档新一版 |
| 20:33:57 | 改 `test/test_w08_dzflat_ab.cpp`（安全落点 + 防覆盖闸） | 修复代码 |
| 20:34:04 | 重编 `build/bin/test_w08_dzflat_ab` | 修复后二进制 |
| **20:34:13**（我的 guard_before） | `3febe45958a5a894…` | ⚠️ 取样时刻**已在两轮污染之后** |
| 20:34:19 / 20:37:55（guard_after ×2） | `3febe459…`（与 before 逐字相同） | 修复后零覆盖 |

**结论（完整表述）**：**修复前共两轮覆盖**（20:22:4x、20:31:18），**修复后零覆盖**。
⚠️ 我的 `guard_before`（20:34:13）晚于 20:31 那一轮，因此它证明的是"**20:34:04 之后编译出的二进制不再写仓库**"，
⛔ **不能**用作"历史未被覆盖"的证据 —— 20:29 与 20:34 两个聚合值不同，正是 20:31 那一轮造成的。
⛔ 本节不写"仅一轮"，也不暗示 guard 覆盖了全过程；r05..r10 的原始样本在两轮之后**均已不可复原**。

### 10.2 根治（代码级，`test/test_w08_dzflat_ab.cpp`）

1. **默认落点不再写仓库**：新增 `artifact_dir_for(run_id)` —— ① `W08_ARTIFACT_ROOT` 显式给定时用
   `<dir>/artifacts/perf/<run_id>`；② **未设环境变量 ⇒ `build/test-scratch/w08/artifacts/perf/<run_id>`**
   （由可执行文件路径推导 `<build>`，不依赖 CWD）；③ 两条都推不出来 ⇒ **拒绝落盘**（不让调用方静默退化成写当前目录）。
   选"scratch 优先"而不是"直接拒绝运行"的理由：默认值落在 `.gitignore` 的 scratch 区后，
   **任何一条运行路径都不可能碰到 `artifacts/`**，同时测试仍可反复跑（不牺牲可用性）；
   而"拒绝运行"会让 ctest 之外的一切调用都失败，收益相同但代价更高。
   代码注释已写明"此前该默认值已造成 W08 证据污染"。
2. **拒绝覆盖闸**：目标 run 目录已有 `samples.csv` 且未授权 ⇒ **不覆盖、不静默跳过**，断言失败并给出三条出路
   （改 run_id / 设 `W08_ARTIFACT_ROOT` / 显式 `W08_OVERWRITE_EVIDENCE=1`）。
3. `repo_root()` 降级为**只用于 manifest 的 build_dir 字段**，⛔ 不再决定落盘位置。

### 10.3 反事实验证（`W08_ARTIFACT_ROOT` **不设**，直接跑二进制）

> **判据的时间边界（⛔ 不要扩大解读）**：本反事实证明的是
> **「2026-09-28 20:34:04 之后编译出的二进制不再写仓库」**；
> ⛔ **不是**"历史样本从未被覆盖"。`guard_before` 取样于 20:34:13，已在 20:22:4x 与 20:31:18 两轮覆盖之后（见 §10.1.1）。
> 修复前 + 修复后的完整结论是：**修复前共两轮覆盖、修复后零覆盖**。

```
修复后二进制: build/bin/test_w08_dzflat_ab (mtime 20:34:04)
跑前聚合(6 个仓库样本 sha256 | sha256sum) = 3febe45958a5a894a3981862c595883f91ec1c57b5faa45781cae7ecb5d2f99c
$ ./build/bin/test_w08_dzflat_ab                       → 3/3 PASSED
跑后聚合(同上)                              = 3febe45958a5a894a3981862c595883f91ec1c57b5faa45781cae7ecb5d2f99c
diff before/after = 空 ⇒ REPO_EVIDENCE_UNTOUCHED=YES（自 20:34:13 起）
落点确认: build/test-scratch/w08/artifacts/perf/20260928-r1{1..6}-W08-*  ← 新样本写这里
```

逐文件 sha256 见 `guard_before_sha256.txt` / `guard_after_sha256.txt`，运行日志 `bare_run_after_guard.log`
（20:34:19）与 `bare_run_after_guard2.log`（20:37:55，第二轮独立复核，聚合值同为 `3febe459…`）。
另：`ctest -R w08_dzflat_ab` 亦通过（走 `ENVIRONMENT` 的 scratch 落点）。
⇒ 满足队长要提升为硬闸的判据：**默认路径不可能写到 `artifacts/`**（该判据是关于**修复后二进制**的，与 §10.1.1 的历史污染范围互不冲突）。

### 10.4 新一版 run（当前有效来源）

| 新 run_id | transport | 写法 | 旧（已失效）run_id |
|---|---|---|---|
| `20260928-r11-W08-tlv-prebuilt` | tlv | 对象预先构造 | ~~r05~~ |
| `20260928-r12-W08-tlv-permsg` | tlv | 每消息新构造 | ~~r06~~ |
| `20260928-r13-W08-dzflat-a-prebuilt` | dzflat-a | 对象预先构造 | ~~r07~~ |
| `20260928-r14-W08-dzflat-a-permsg` | dzflat-a | 每消息新构造 | ~~r08~~ |
| `20260928-r15-W08-dzflat-b-prebuilt` | dzflat-b | 预置字段 | ~~r09~~ |
| `20260928-r16-W08-dzflat-b-permsg` | dzflat-b | 应用原地构造 | ~~r10~~ |

- 归档方式：`W08_ARTIFACT_ROOT=<repo>` 显式指定落点后运行（**追加**，不复用旧 run_id）。
- 每个新 run 目录含 `samples.csv` / `samples.jsonl` / `manifest.json`（`transport` / `process_model=cross-process` /
  `work_package=W08`）/ `counters_publisher.json` / `counters_consumer.json` / `README.md`。
- 新值与污染前派生值的对照（**只作量级核对，⛔ 不作结论**）：B 档 transport 现行 `1.3/1.3/1.3`（r16）
  与 `2.2/2.2/2.4`（r15），与 19:57 派生记录的 `1.35~1.42` **同量级** ⇒ 污染只毁掉了被覆盖的**旧目录**，
  未伤及测量机制；A/TLV 档同样同量级（逐值对照见 §10.5）。⚠️ 这里的 `1.35~1.42` 属**历史派生记录**，
  ⛔ 不得回填进 §0/§1.2 或任何结论句。
- ⛔ 旧目录按 §12 **只追加、不删除**（保留以备追溯），仅在文档里标注失效并指向新 run。

### 10.5 t71：§0/§1.2 逐值新旧对照（r05..r10 派生 → r11..r16 现行）

**复算命令**（⛔ 不读 `perf_summary.json`，直接从现行 `samples.csv` 重算）：

```bash
cd /home/zwc/cpp_ipc_dds
# 逐值复算：只读现行 run 的 samples.csv（⛔ 不读 perf_summary.json）
python3 - <<'EOF'
import csv, statistics as st
B='artifacts/perf'
RUNS={'r11':'20260928-r11-W08-tlv-prebuilt','r12':'20260928-r12-W08-tlv-permsg',
      'r13':'20260928-r13-W08-dzflat-a-prebuilt','r14':'20260928-r14-W08-dzflat-a-permsg',
      'r15':'20260928-r15-W08-dzflat-b-prebuilt','r16':'20260928-r16-W08-dzflat-b-permsg'}
SZ=['img66k','img262k','img880k']
def med(k,size,col):
    rs=[r for r in csv.DictReader(open(f'{B}/{RUNS[k]}/samples.csv',encoding='utf-8'))
        if r['route'].endswith('/'+size)]
    return st.median(int(r[col])/1000.0 for r in rs)
print("§1.2 (permsg: r12=TLV, r14=A)  transport/e2e/相对")
for s in SZ:
    t,a=med('r12',s,'transport_ns'),med('r14',s,'transport_ns')
    te,ae=med('r12',s,'e2e_ns'),med('r14',s,'e2e_ns')
    print(f"  {s}: {t:.1f} {a:.1f} {te:.1f} {ae:.1f} {(ae-te)/te*100:+.1f}%")
print("§0:61/62 (img880k: r12/r14/r16)")
t,a,b=med('r12','img880k','transport_ns'),med('r14','img880k','transport_ns'),med('r16','img880k','transport_ns')
te,ae,be=med('r12','img880k','e2e_ns'),med('r14','img880k','e2e_ns'),med('r16','img880k','e2e_ns')
print(f"  发布 {t:.1f}/{a:.1f}/{b:.1f}  e2e {te:.1f}/{ae:.1f}/{be:.1f}  A-B差={a-b:.1f}µs  BvsA={(be-ae)/ae*100:.2f}%")
EOF

# 262k 双峰取证：逐样本 seq ↔ e2e（与 §1.2 登记的 8/32 分配一致）
python3 - <<'EOF'
import csv
rs=sorted((r for r in csv.DictReader(open('artifacts/perf/20260928-r14-W08-dzflat-a-permsg/samples.csv',encoding='utf-8'))
           if r['route'].endswith('/img262k')), key=lambda r:int(r['seq']))
print(' '.join(f"{r['seq']}:{int(r['e2e_ns'])//1000}" for r in rs))
EOF
```

**§1.2 三档表（12 个数）**：

| 位置 | 量 | 旧值（r05..r10 派生，已失效） | **新值（r11..r16 现行）** | 新值来源 |
|---|---|---|---|---|
| §1.2:80 | img66k TLV 发布 | 6.3 µs | **11.0 µs** | r12 `transport_us_med` |
| §1.2:80 | img66k A 发布 | 3.8 µs | **6.6 µs** | r14 |
| §1.2:80 | img66k TLV e2e | 41.3 µs | **69.6 µs** | r12 |
| §1.2:80 | img66k A e2e | 34.4 µs | **57.5 µs** | r14 |
| §1.2:80 | img66k A 相对 TLV | −16.7% | **−17.4%** | 由上行两值复算 |
| §1.2:81 | img262k TLV 发布 | 17.1 µs | **17.4 µs** | r12 |
| §1.2:81 | img262k A 发布 | 6.9 µs | **16.7 µs** | r14 |
| §1.2:81 | img262k TLV e2e | 152.5 µs | **149.9 µs** | r12 |
| §1.2:81 | img262k A e2e | 114.5 µs | **200.9 µs** ⚠️ 双峰 | r14 |
| §1.2:81 | img262k A 相对 TLV | −24.9% | **+34.1%** ⚠️ **方向反了** | 由上行两值复算 |
| §1.2:82 | img880k TLV 发布 | 179.8 µs | **180.1 µs** | r12 |
| §1.2:82 | img880k A 发布 | 20.1 µs | **20.3 µs** | r14 |
| §1.2:82 | img880k TLV e2e | 744.6 µs | **726.3 µs** | r12 |
| §1.2:82 | img880k A e2e | 384.5 µs | **384.5 µs**（同值） | r14 |
| §1.2:82 | img880k A 相对 TLV | −48.4% | **−47.1%** | 由上行两值复算 |

**§0:61（发布/e2e 路径，0.88 MB）**：

| 位置 | 量 | 旧值（已失效） | **新值（现行）** | 来源 |
|---|---|---|---|---|
| §0:61 | TLV 发布路径 | 179.8 µs | **180.1 µs** | r12 |
| §0:61 | A 发布路径 | 20.1 µs | **20.3 µs** | r14 |
| §0:61 | B 发布路径 | 1.4 µs | **1.3 µs** | r16 |
| §0:61 | TLV e2e | 744.6 µs | **726.3 µs** | r12 |
| §0:61 | A e2e | 384.5 µs | **384.5 µs**（同值） | r14 |
| §0:61 | B e2e | 355.2 µs | **357.8 µs** | r16 |

**§0:62（比值与 e2e 差）**：

| 位置 | 量 | 旧值（已失效） | **新值（现行）** | 来源 / 处理 |
|---|---|---|---|---|
| §0:62 | A→B `transport` **比值** | 一个由 r05..r10 派生的倍数（复核者已从 19:57 派生记录复算、**逐位吻合**地证其为派生值） | **不采用**（比值一律不作为收益表述，t71 起连数值也不再写出） | ⛔ 现行可复算的等价量 = A→B transport **绝对差 19.0 µs**（`20.3065 − 1.3415`）；正文只写「只等于省掉一次 0.88 MB 拷贝」 |
| §0:62 | e2e 上 B vs A | ≈−7.6%（由 `(355.24−384.55)/384.55` 复算） | **−6.9%**（`(357.756−384.4735)/384.4735`，绝对 **−26.7 µs**） | r14/r16 |

**无法从现行 run 得到 / 已删除的表述**：
- 旧 §1.2 的「-17% → -48% 单调增长」这一**趋势句**：现行数据不支持（262k 档 **+34.1%**）⇒ **删除并改写为非单调**（§1.2 读法）。
- 旧 §0:62 的那个**倍数**与其配套 e2e 百分比：作为**结论性数值**删除；只保留机制表述与现行绝对差（19.0 µs / −6.9%）。
- ⛔ **倍数类数值一律不再写出**：本包 A→B 的 `transport` **比值**（无论旧派生值还是现行值）**全文不作为结论出现**，
  一律换成"只等于省掉一次 0.88 MB 拷贝" + 绝对差 µs（§0:62、§2.5、本节）。
  （本节上面那张表的"旧值"列是为满足"逐值新旧对照"而保留的**失效标记**，⛔ 不得被读成性能声明。）
- 262k 档的**中位数**在现行数据下**不是稳定量**（双峰，见 §1.2）⇒ 该档的"收益"结论**标记为不成立**，
  须 W11 重采；⛔ 不保留任何该档的收益结论。

⛔ **保留但只作历史对照、不得引用**：`perf_summary.json` / `perf_summary.md`（19:57:24）与
`artifacts/perf/20260928-r05..r10-W08-*/`（§12 只追加）—— 它们仍是"污染前派生记录"与"被覆盖的原始目录"，
按纪律保留以备追溯。

---

