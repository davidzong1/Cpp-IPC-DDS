# W10-R5 §10.2 / 三态复验 — 独立结论

- run_id: `B-st1-n100-diagon-r1`｜state 1｜n=100｜workers=32｜diag=on｜round=1
- 判据版本: `W10-R5/§6.2-§6.3/v1`
- 判定: **通过**

## 运行时门禁（方案 §6.2）

| 门禁 | 结果 |
|---|---|
| 无route时 Δscanned_routes_total 可为 0 | ✅ |
| 常驻/门控同窗口自洽 | ✅ |
| 诊断开启时 Δwait_timeout_count 与 wait_timeouts 同口径 | ✅ |

## 五项派生值（同一窗口差值；零分母 = null + 原因）

| 量 | 值 | null 原因 |
|---|---|---|
| 平均每轮扫描条目 | null | 分母 Δscan_rounds = 0 |
| 平均每轮扫描耗时(ns) | null | 分母 Δscan_rounds(门控) = 0 |
| 平均每条目扫描耗时(ns) | null | 分母 Δscanned_routes_total(门控) = 0 |
| 发现就绪的轮次占比 | null | 分母 Δscan_rounds = 0 |
| 每秒等待超时 | 0.000000 | - |

> ⛔ `scan_time_ns_total` 是**墙钟区间累计**，不得换算为 CPU core。CPU 由独立采样给出。

## 失败清单（0 条）

（无）
