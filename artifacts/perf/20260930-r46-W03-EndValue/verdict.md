# W03 · R0-9/R0-10 结束值接口落地 —— 运行判定

> run_id: `20260930-r46-W03-EndValue` · 任务 t44 · 方案 §12 只追加目录。

## 结论

接口与字段**已落地**（`finish()` 出参 + 4 个新 `CounterId` + 每 worker `ScanRoundResult`/
`ScanRoundAccumulator` + 全池 Σ 聚合）。`ctest` **27/27**、W03 白盒判据 **24/24**、
接口自测 **PASS**。`recv_worker.cc` 的**接线**按 R0-11 归共享层负责人（本包未改，sha256 见指纹）。

## 逐项引用

| 验收要求 | 证据 |
|---|---|
| `finish()` 出参 + 4 个新 ID（含语义/单位/nullable） | `include/dzIPC/measure/counters.h`（枚举 + `counter_table()`）；`interface_selftest.json` 全 true |
| 每 worker 出参、全池总量 = Σ | `B_pool_depth_is_sum_of_workers=true`（12 = 5+7）；`B_sums_match_global=true` |
| 两个 gauge 不互相覆盖且不可相加已声明 | `C_before_after_gauges_distinct=true`；`field_schema.h` 的 `counter_semantics_guards` 三条 |
| `scan_time_ns_total` 区间已冻结 + 含诊断开销 + 落地前不得作判据 | 头文件「区间定义 —— 冻结」段；`overhead_microbench.json` 的分档实测 |
| 引用方清单 + 重编命令（③d） | W03/结束值接口_落地.md §6；`PAIRING: OK` |
| 诊断关闭输出为「未采集」非 0 | `A_diag_off_uncollected/uncollected_elapsed/no_counter_write` 全 true；`diagnostics_collection="uncollected"` |
| `ctest -j4` 全绿 + `test_w03_measurement` 未破坏 | `ctest_27.log`（27/27）、`gtest_w03_measurement.log`（24/24） |

## 未确认/未自证

- 接线未做（R0-11 边界）：新 4 个门控计数在当前库里恒 0 且为**未采集**语义 ⇒ ⛔ 不得读作"深度为 0"。
- `RecvWorkerStats` 未追加字段 ⇒ ③d 的实际触发点在 W06/t42；本包已给命令与记录项。
- 本包不主张 S4/S5。
