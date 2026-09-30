# W10-R5 §10.2 / 三态复验 — 独立结论

- run_id: `scan-st3-n100-W32-diagon-r5`｜state 3｜n=100｜workers=32｜diag=on｜round=5
- 判据版本: `W10-R5/§6.2-§6.3/v1`
- 判定: **通过**

## 运行时门禁（方案 §6.2）

| 门禁 | 结果 |
|---|---|
| 有route时 Δscan_rounds>0 | ✅ |
| 有route时 Δscanned_routes_total>0 | ✅ |
| 有route时诊断扫描时间有效(>0) | ✅ |
| 0<=Δready_observed<=Δscan_rounds(门控) | ✅ |
| 0<=Δscan_ready_rounds<=Δscan_rounds(常驻) | ✅ |
| deferred 深度非负 | ✅ |
| deferred 深度不超过在册 route 数 | ✅ |
| 常驻/门控同窗口自洽 | ✅ |
| 诊断开启时 Δwait_timeout_count 与 wait_timeouts 同口径 | ✅ |

## 五项派生值（同一窗口差值；零分母 = null + 原因）

| 量 | 值 | null 原因 |
|---|---|---|
| 平均每轮扫描条目 | 3.124799 | - |
| 平均每轮扫描耗时(ns) | 425.325595 | - |
| 平均每条目扫描耗时(ns) | 136.112961 | - |
| 发现就绪的轮次占比 | 0.000000 | - |
| 每秒等待超时（门控 `wait_timeout_count`） | 0.319830 | - |
| 每秒等待超时（常驻孪生 `wait_timeouts`，标注 resident_） | 0.319830 | - |

> ⛔ `scan_time_ns_total` 是**墙钟区间累计**，不得换算为 CPU core。CPU 由独立采样给出。

## 失败清单（0 条）

（无）
