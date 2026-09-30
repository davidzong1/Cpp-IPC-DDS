# W02-F3 修复前后对照（失败量是否进判定）

> 修复前 = `20260928-r23-W02`（**非现行**，F3 修复前一代）；修复后 = **现行** `20260928-r30-W02`（`manifest.currently_citable`）。
> 两轮的样本表判据相同（行口径），skipped_rows 逐 run 波动属正常：r23=37 / r30=53。

| 维度 | 修复前 `r23` | 修复后 `r30`（现行） |
|---|---|---|
| `late` 非零 case 数 | 17/75 | 14/75 |
| 其中「`late` 非零但 `failure_reasons` 为空」 | **17 —— 全部漏判**：这 17 条**没有任何判定通路**（既无名点，也无 gate 列），只能靠人翻 CSV ⇒ 静默排除 | **14 —— 都是"已判定但未超预算"**：每条在 `late_ok` 列都有显式判定值（=1），读方一眼可判「未超阈值」而不是「没判」 |
| Σ`late` | 71 | 55 |
| `backlog_max_ns` 非零 case 数 | 75/75 | 75/75（非零≠超预算：预算 = 10 个周期） |
| `late_ok` 列 | **不存在** | 存在（本 run 判失败 0 个 case） |
| `backlog_ok` 列 | **不存在** | 存在（本 run 判失败 0 个 case） |
| `send_blocked_ok` 列 | **不存在** | 存在（本 run 判失败 0 个 case） |
| `abnormal_ok` 列 | **不存在** | 存在（本 run 判失败 0 个 case） |
| `bad_header_ok` 列 | **不存在** | 存在（本 run 判失败 0 个 case） |
| `bad_header` 列 | **完全不存在**（只写进子进程日志 `bad_hdr=`；B 档实测 686–1210/case（跨 r19/r20/r23；见 `r4_bad_header_inventory.txt`） 完全不可见） | 存在（本 run 非零 0 case；gate 判失败 0 case） |
| 阈值声明 | 无（`late_threshold_ns` 只定义"什么叫迟发"） | `manifest.failure_thresholds`：6 个阈值 + `cli_overridden` + `gates_in_judgement`（逐 gate 触发计数）+ `informational_only`（**显式声明**哪几个量只作信息性保留及理由） |

## 负控（阈值压到 0，证明判据真的会亮）

负控的原始输出见同目录 `f3_negctl/`（阈值压到 ~0 的那一条）：

```
$ ./build/bin/xproc_benchmark --path=tlv --payload=1024 --workload=full --duration=1 --warmup=0.2 \
    --rate=1000 --sample-limit=3000 --late-abs-max=0 --late-rate-max=0 --backlog-max-periods=0.0001 \
    --abnormal-max=0 --bad-header-max=0 --purpose=verification --citation-authority=<证据 run> \
    --out-dir=artifacts/perf/<证据 run>/f3_negctl --run-id=<id>
⇒ rc=1；late_ok=0 **且** backlog_ok=0（两个 gate 同轮被判亮）；cli_overridden=true
⇒ failure_reasons: 迟发超出阈值 late=1 (0.1000%) > 阈值[abs<=0 且 rate<=0.0000%]；
                   发送积压超出阈值 backlog_max=… > 阈值 0.000
```
⚠️ 该复核轮的 `manifest.currently_citable` **指向证据 run**（`--purpose=verification` +
`--citation-authority=<证据 run>`）⇒ 复核轮不会被误当成现行权威（这正是 F0 第 2 条的机制）。

### 第三条证据：**真实负载**下这两个闸确实会亮（不是只在人工压阈值时）

> ⛔ **依赖声明（t50/R-2）**：本层证据的**唯一实物**是 `superseded/w02_r24.out`（r24 那一轮的控制台，
> sha256 `0ff3503009d5eb0dddc82907c3c2256316f4de758cb22fc789b66f817053cb96`）。
> 该档位**不可按需重跑复现**：R1 复核者按同档位亲跑 **3 轮 + 4 路并发未能复现任何名点**（当时机器负载偏高）。
> ⇒ 本层只证明「判据**在该负载条件下**会亮」，⛔ 不得写成「任意负载下都会亮」。
> 详细适用条件见 `r2_archive_note.md`。

阈值取 0.5%/1 周期档的那一轮（`r24`，**非现行**，控制台归档在**本目录** `superseded/w02_r24.out` ——
⛔ 早前文档写的是 `tmp/w02_r24.out`，那违反方案 §12「不得依赖 tmp」，已改正）里，
真实 smoke 负载下的名点**按量分列**（⛔ 早前含混写作「40 处名点」，实际是两类之和）：

| 名点类型 | 行数 | 涉及 case 数 |
|---|---|---|
| `迟发超出阈值` | **40** | 20 |
| `发送积压超出阈值` | **32** | 16 |
| **合计** | **72** | **23**（去重；13 个 case 两类同时亮） |

例如：

```
[?] path=tlv workload=busy payload=1024B rate=1000
    -> FAIL plan=1000 attempts=1000 ok=1000 recv=1000 missing=0 dup=0 ooo=0 crc_bad=0 dzflat=0 fallback=2185
       失败原因:
         - 迟发超出阈值: late=13 (1.3000%) > 阈值[abs<=5 且 rate<=0.5000%]
         - 发送积压超出阈值: backlog_max=3479416 ns = 3.479 个周期 > 阈值 1.000
```

⇒ **三层证据齐了**（按强度排序，⛔ 层② 依赖归档件）：
① **人工负控**（阈值→0 ⇒ 两个 gate 必亮，低负载下**可稳定复现**，实物 `f3_negctl/`）；
② **真实负载**（0.5%/1 周期档 ⇒ 迟发 40 + 积压 32 = **72 处名点**、23 个 case，**依赖归档件**）；
③ **默认档**（5%/10 周期 ⇒ 0 case 亮，说明也不是"随便就判失败"，可稳定复现）。
阈值被覆盖时 `manifest.failure_thresholds.cli_overridden=true`。
⛔ 修复前同样输入 rc=0、`failure_reasons` 为空 —— 这正是「仅落列 = 静默排除」。

## 阈值取值的依据与敏感度（⛔ 不是"取刚好不亮"的数字）

| 阈值 | 默认值 | 依据 | 敏感度（60-case 冒烟） |
|---|---|---|---|
| `late` | `>20` **且** `>5%` | `sleep_until` 抖动几十 µs ⇒ 要求 0 等于把调度噪声判成失败；两条件同时成立才说明"结构性跟不上声明速率" | 5% 档：0 case 亮；0.5% 档：5/75 亮；0% 档：全亮 |
| `backlog_max` | `>10` 个周期 | 峰值积压含一次调度量子 + 唤醒链开销，以周期为单位跨速率档可比 | 1 周期 ⇒ 16 case 亮；5 周期 ⇒ 1 case 亮（真实排队 5.171）；10 周期 ⇒ 0 case 亮 |
| `send_blocked` / `abnormal` / `bad_header` | `>0` | 有界等待耗尽与结构异常**本来就该**为 0 | 本轮全 0（修前 B 档 `bad_header` 会亮 15/75） |

## 判定逻辑唯一性（纪律②）

`failure_reasons`（含 5 个 gate）只在 `run_one_case()` 的**唯一一处**产生；
`summary.csv` / `results.json` / `verdict.md` 只**读**该结果与其布尔位，⛔ 不各自重算。
机械判据：`test/w02_summary_schema_check.py`（表头列数 == 格式串字段数 == 数据行列数；且逐行「gate=0 ⇒ 名点非空」），
`test/test_w02_xproc_benchmark.cpp::FailureCountersAreJudged`（进 CTest）。
