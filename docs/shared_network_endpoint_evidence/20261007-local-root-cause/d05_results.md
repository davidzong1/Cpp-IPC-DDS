# D05 目标线程激活与 assist/复制对照结果

日期：2026-10-07
构建：`build-d05`，源码分支 `fix/local-latency-root-cause`

## 结论

当前构建未形成可进入生产的接收、通知去重、复制或调度候选。调度链严格按接收 TID、等待区间和时间顺序配对后，1 SUB/4KiB 与 32 SUB/64B 的主要可见长尾仍位于 `sched_waking -> sched_wakeup`，而 `sched_wakeup -> sched_switch` 和 `sched_switch -> wait_end` 较短。assist=0/1 的平衡短窗没有跨场景稳定方向；1MiB placement 对照显示发布复制成本随 CPU 放置变化，但 assist=0/1 与端到端结果不一致，不能据此改变复制实现或默认策略。

## 调度链冒烟

`d05-probe-smoke-20261007/run4`：300 发布、300 接收，严格匹配 300/300；无丢事件。p99（微秒）为：`notify->waking 5.052`、`waking->wakeup 67.584`、`wakeup->scheduled 1.547`、`scheduled->wait_end 3.175`、`wait_end->recv_return 5.456`。该窗口只验证采集和分段链路，不作为性能门槛。

## assist A/B

`d05-assist1-20261007` 和 `d05-assist0-20261007` 各 8 窗，包含两个 ABBA/BAAB 平衡块；每批 8,000 发布、132,000 接收，完整性和调度事件均通过。assist=1 perf 窗的端到端 p50/p99：1 SUB/4KiB 为 126.159/178.702、22.622/177.942；32 SUB/64B 为 38.555/186.578、41.337/174.654。assist=0 对应为 150.108/247.976、101.417/217.072；32 SUB 为 55.227/211.747、49.722/198.005。无 perf 窗方向也不一致，且 `waking->wakeup` 长尾仍存在。因此不修改 `receive_assist` 默认值，不引入通知合并/去重候选。

逐窗的原始分段汇总见 `d05_assist_comparison.json`。

## 1MiB placement/复制对照

`d05-copy-assist1-20261007` 与 `d05-copy-assist0-20261007` 各 6 窗，固定 1 SUB/1MiB、发布/订阅/网关 CPU 放置，全部 6,000 发布/接收完整。assist=1 的发布 p50 在各放置为 67.413、40.155、43.167、77.817、37.436、61.427 微秒；assist=0 为 73.999、74.240、44.820、67.858、38.051、49.893 微秒。复制 wall/CPU 和端到端没有一致同向改善，不能合入预取、分块或减少复制改动。

逐窗的复制与接收分段汇总见 `d05_copy_comparison.json`。

## D06 IPI/call-function 冒烟

当前 build-d05 的 assist=0/1 各运行 300 条 1 SUB/4KiB 消息，两个窗口的 300/300 调度链均严格匹配且没有 LOST；`ipi_send_cpumask` 在 `perf script` 中有内核格式化警告，未用于样本分类。两组样本都在每条链上观察到目标 CPU 的 `sched_wake_idle_without_ipi`；assist=1 的 300 条链没有目标 CPU `ipi_send_cpu`、`ipi_entry` 或 call-function entry，assist=0 只有 1 条目标 `ipi_send_cpu` 且没有匹配的目标 IPI entry。`waking→wakeup` p50/p99：assist=1 为 2.577/95.035 µs，assist=0 为 95.483/119.016 µs。该短窗说明当前长段多数走 idle polling/无 IPI 路径，不能通过增加通知、去重或复制来缩短；assist=0/1 的中位数差异也受窗口状态影响，不能形成调度候选。原始 IPI 事件和按样本分类见 `d06-ipi-smoke-20261007/d06-ipi-summary.json`。

当前 build-d05 的 4 窗内核栈采样（1 SUB/4KiB、32 SUB/64B，各两轮）共严格匹配 66,000 条接收和 132,000 个 waking/wakeup 栈事件，缺失、空栈、LOST、未知行均为 0。1 SUB 两窗的 wakeup 栈 100% 为 `remote_pending`；32 SUB 两窗分别为 31,871/32,000 和 31,922/32,000 `remote_pending`，其余为 direct activation。32 SUB 最慢 1% 的 `remote_pending` `waking→wakeup` p50 为 150.264/140.883 µs，说明长尾位于待处理激活路径，而不是 getter/队列复制段。栈采集仅作机制证据，汇总见 `d06-stacks-current-20261007/d06-stack-summary.json`。

## 代码与工具改动

`summarize_perf_scheduler.py` 现在支持 D05 未压缩事件和 CSV，并要求唯一的 waking/wakeup/switch 链，避免最近事件误配；严格链结果保留缺失原因。调度解析单测 7/7、共享网络 Python 测试 36/36、四个 D05 批次审计均通过。

## 后续

保留全部原始 CSV、perf、环境快照、哈希和汇总；不执行 D12。后续若继续，应针对 `waking->wakeup` 的内核路径做更细的 IPI/call-function 观测，并在有稳定机制证据后才设计生产改动。
