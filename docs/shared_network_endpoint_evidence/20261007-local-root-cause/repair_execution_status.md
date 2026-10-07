# 本机延迟根因修复执行状态

## 已完成

- P0：A/B 产物、动态库哈希和既有 D02/D03 证据已冻结；六个冒烟和 `test/shared_net` 30 项通过。
- P1：统一 `recv_wait` 诊断 seam 已加入并完成 app-diagnostic-001/002。app-diagnostic-002 的 9 个窗口全部完整，wait trace 无溢出、缺失或乱序。
- 诊断关闭路径已收紧：未安装 hook 时不执行诊断专用 `count_if`、快照和 wait-set 字段采集；wait 的条目、ready、stopped、waiter 快照均在锁内保存后再解锁回调；`pending_interrupt` 在更新 `observed_interrupt` 前计算；通知事件使用 `wake_result`。
- 最小回归：`test_recv_wait_set`、`test_recv_worker`、`test_shm_route_session`、`test_lifecycle_contract` 通过。

## 当前阻塞

- D04 仍因 `perf_event_paranoid=4`、tracefs/debugfs 不可读和无免密 sudo 阻塞。因此尚无当前 A/B 的 `sched_waking`、`sched_wakeup`、`sched_switch`、迁移和 idle/IPI 因果证据。
- `test_shm_ready_transition` 连续两次失败于两个链路前提用例（`drain=0`，worker 空闲退出），第三个状态转换用例通过；未将其归因于诊断 seam。

## 不能关闭的结论

三个历史未通过项仍为“待验证”：当前 D02 短窗改善不能替代正式平衡块，应用 wait trace 不能替代 D04 调度链。未执行通知去重、memcpy、调度策略或生产接收逻辑修改，也未覆盖 `build-latency-formal`。

## 下一步准入

D04 能力恢复后，先在 A/B 的 1 SUB/4KiB 和 32 SUB/64B 上完成调度链最小窗口，再按 W1/W2/W3 单因素方案决定是否创建候选 C。候选必须保留完整性、生命周期和关闭 seam 后的回归证据。
