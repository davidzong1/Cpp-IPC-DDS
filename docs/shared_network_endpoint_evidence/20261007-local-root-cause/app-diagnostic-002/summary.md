# B 等待路径诊断（app-diagnostic-002）

本批在 B 的新 wait-trace 构建上完成 9 个短窗：worker assist=1、worker assist=0、强制 compat，各覆盖 1 SUB/4KiB、1 SUB/1MiB、32 SUB/64B。每窗 5 秒正式窗口、100 Hz、500 条发布；32 SUB 共 16000 条接收。所有发布和订阅完整，丢失、重复、损坏、trace 溢出、trace 行数不匹配均为 0。

wait trace 事件文件位于各窗口目录下的 `sub*.csv.wait_trace.csv`。worker 路径出现 `set_add/remove/interrupt/wait_begin/wait_scan/wait_end/consume_ready`；assist=1 还出现 `local_wait_begin/end` 与 `set_enable`。compat 路径主要出现 `local_notify` 和 `route_notify`，没有 wait-set 事件，说明它没有进入 worker wait-set。

事件只说明应用等待协议和通知路径，不能证明 `notify→wakeup→scheduled→recv_return` 的内核调度因果。CSV 按回调写入顺序保存，32 SUB 多线程文件不要求全局时间单调；采集器自身校验的是行数、溢出和事件完整性。D04 仍受 `perf_event_paranoid=4`、tracefs/debugfs 无权限和无免密 sudo 阻塞；在能力恢复前不形成生产候选，也不修改通知去重、复制实现或调度策略。JAX 任务按用户要求未读取、停止或修改。

新构建的诊断 seam 关闭时保持原等待协议；本批之后的最小回归中 `test_recv_wait_set`、`test_recv_worker`、`test_shm_route_session` 和 `test_lifecycle_contract` 通过。`test_shm_ready_transition` 的两个链路前提用例连续两次 `drain=0`，worker 全部报告空闲退出；该失败独立记录为既有就绪链路/环境回归，不能归因于 wait-trace seam。
