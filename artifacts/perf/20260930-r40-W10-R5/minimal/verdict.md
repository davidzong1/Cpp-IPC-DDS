# W10-R5 最小验证集（方案 §6.2）

- run_id: `r5min`｜workers=4
- 判定: **通过**（失败 0 项）

| 用例 | 名称 | 结果 | 说明 |
|---|---|---|---|
| 1 | "无 route（空表轮扫描）" | PASS | "route_count=0；Δscan_rounds=30（>0 说明空表也在按轮扫描）、Δscanned=0（应为 0）、scan_ready_rounds=0（应为 0）" |
| 2 | "固定 route 无消息" | PASS | "route_count=10（应=10）；Δscanned=130（>0）；Δscan_ready_rounds=0（应=0 —— 无消息）" |
| 3 | "一次已知就绪" | PASS | "一次发布 ⇒ Δscan_ready_rounds=1（≥1）、Δscan_rounds=37；门控 ready_observed Δ=1、scan_rounds Δ=37（常驻/门控逐值一致=1）" |
| 4 | "预算耗尽重入 deferred" | PASS | "灌 64 条（>每轮消息预算 32）⇒ Δbudget_yields=2、Δdeferred_drains=2、实收=64、recv_errors=0（预算耗尽**不是丢弃**）" |
| 5 | "断开/注销" | PASS | "注销前 route_count=10 ⇒ 注销后 0；Δidle_exits=4（>0 = 线程按需归还）、Δthread_restarts=0" |
| 6 | "假就绪（false-ready）" | N/A | "SHM 侧**不适用**：SHM 的事实判据是共享内存 seq（`seq != last_seq`），不存在『内核报了就绪但读不到』的形态；假就绪是 **socket** 侧形态（`fruitless_backoffs`/`rearm_events`，W04-F3 已修）。⛔ 不用其它形态冒充，socket 侧证据另见 W07 交付。" |
| 7 | "后端错误（wait_errors/recv_errors）" | N/A | "**无法在不改产品代码的前提下构成**：SHM 后端的 wait 只在 futex_waitv 返回异常时计`wait_errors`，该路径需要注入失败（属产品侧射程，⛔ 本次不改）。当前读数：recv_errors=0、wait_errors=0（均 0，⛔ 不得当作『已覆盖』）。" |
| 8 | "诊断开关前后对照" | PASS | "diag=off：Δrounds=64 Δscanned=320 gated_scan_time=0（**未采集**，⛔ 非零成本）；diag=on：Δrounds=68 Δscanned=340 gated_scan_time=53732（>0）；常驻量两档量级一致(±20%)=1" |
