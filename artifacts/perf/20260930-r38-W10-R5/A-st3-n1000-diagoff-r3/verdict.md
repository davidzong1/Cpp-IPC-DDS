# W10-R5 §10.2 / 三态复验 — 独立结论

- run_id: `A-st3-n1000-diagoff-r3`｜state 3｜n=1000｜workers=32｜diag=off｜round=3
- 判据版本: `W10-R5/§6.2-§6.3/v1`
- 判定: **不通过**

## 运行时门禁（方案 §6.2）

| 门禁 | 结果 |
|---|---|
| 有route时 Δscan_rounds>0 | ❌ |
| 有route时 Δscanned_routes_total>0 | ✅ |
| 0<=Δready_observed<=Δscan_rounds(门控) | ✅ |
| 0<=Δscan_ready_rounds<=Δscan_rounds(常驻) | ✅ |
| deferred 深度非负 | ✅ |
| deferred 深度不超过在册 route 数 | ✅ |
| 常驻/门控同窗口自洽 | ✅ |

## 五项派生值（同一窗口差值；零分母 = null + 原因）

| 量 | 值 | null 原因 |
|---|---|---|
| 平均每轮扫描条目 | null | 分母 Δscan_rounds = 0（诊断关闭 ⇒ 门控量**未采集**（⛔ 不得解释为零成本）） |
| 平均每轮扫描耗时(ns) | null | 分母 Δscan_rounds(门控) = 0（诊断关闭 ⇒ 门控量**未采集**（⛔ 不得解释为零成本）） |
| 平均每条目扫描耗时(ns) | null | 分母 Δscanned_routes_total(门控) = 0（诊断关闭 ⇒ 门控量**未采集**（⛔ 不得解释为零成本）） |
| 发现就绪的轮次占比 | null | 分母 Δscan_rounds = 0（诊断关闭 ⇒ 门控量**未采集**（⛔ 不得解释为零成本）） |
| 每秒等待超时 | 0.000000 | - |

> ⛔ `scan_time_ns_total` 是**墙钟区间累计**，不得换算为 CPU core。CPU 由独立采样给出。

## 失败清单（1 条）

- 门禁失败: 有route时 Δscan_rounds>0
