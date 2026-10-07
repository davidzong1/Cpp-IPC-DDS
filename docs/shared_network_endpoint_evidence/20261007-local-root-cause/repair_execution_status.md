# 本机延迟根因修复执行状态

## 已完成

- P0：A/B 产物、动态库哈希和既有 D02/D03 证据已冻结；六个冒烟和 `test/shared_net` 30 项通过。
- P1：统一 `recv_wait` 诊断 seam 已加入并完成 app-diagnostic-001/002。app-diagnostic-002 的 9 个窗口全部完整，wait trace 无溢出、缺失或乱序。
- 诊断关闭路径已收紧：未安装 hook 时不执行诊断专用 `count_if`、快照和 wait-set 字段采集；wait 的条目、ready、stopped、waiter 快照均在锁内保存后再解锁回调；`pending_interrupt` 在更新 `observed_interrupt` 前计算；通知事件使用 `wake_result`。
- 最小回归：`test_recv_wait_set`、`test_recv_worker`、`test_shm_route_session`、`test_lifecycle_contract` 通过。
- D04：用户授权 sudo 后已恢复调度事件采集，业务进程仍以 UID/GID 1000 运行。独立 A 诊断提交为 `bb2f9bf9`，B 诊断提交为 `3918e999`，正式 A 和 `build-latency-formal` 未修改。
- 同语义接收线程对照 `d04-ab-role-001` 已完成 16/16 窗，16000 次发布、264000 次接收完整；8 个 perf 窗的 132000 条调度链全部匹配，无 LOST、未知事件或已知外部编译/压测重叠。实际接收角色为 A 后台线程、B getter。
- C1 已在独立实验工作区修改生产 `waiter::wake/wait_if`，并完成 `B C C B | C B B C` 的三个场景共 24 窗。24000 次发布、272000 次接收及调度链全部完整；详细相邻配对、平衡块和发布序号聚类见 `c1-abba-l2/candidate-summary.md`。
- C1 原四项登记测试和 8 项功能/生命周期 CTest 通过，但新增旧协议兼容用例失败。同一测试对未改动 B 为 5/5，通过候选为 4/5；结果已保留在 `c1-waiter-validation`。
- C1 被拒绝：4KiB/1MiB p50 两块方向相反，32 SUB p99 两块均增加，且旧进程存在丢唤醒风险。补丁只保留在实验分支，不合入生产。
- D05 调度链解析已收紧：支持 run4 的未压缩 CSV/事件，并按实际接收 TID、等待区间和唯一 `sched_waking → sched_wakeup → sched_switch` 链匹配；多个候选链显式保留为歧义，不按最近事件填配。run4 冒烟 300/300 条严格匹配，无 LOST/未知事件。
- D05 assist A/B 已完成：`d05-assist1-20261007`、`d05-assist0-20261007` 各 8 窗、每批 8000 发布/132000 接收；两批完整性、环境门控和调度事件审计通过。assist=0/1 在 1 SUB/4KiB、32 SUB/64B 没有跨场景稳定方向，未修改默认接收协作策略。
- D05 1MiB placement 对照已完成：`d05-copy-assist1-20261007`、`d05-copy-assist0-20261007` 各 6 窗、每批 6000 发布/接收；发布复制 wall/CPU 与端到端结果随放置波动且不一致，未形成复制候选。
- D06 IPI/call-function 冒烟已完成：当前 build-d05 的 assist=0/1 各 300 条 1 SUB/4KiB 消息严格匹配；两组均观察到 `sched_wake_idle_without_ipi`，assist=1 无目标 IPI/call-function 链，assist=0 仅 1/300 条出现目标 `ipi_send_cpu` 且无对应 entry。该证据不足以支持改通知、去重或调度策略，原始事件见 `d06-ipi-smoke-20261007`。
- D06 当前内核栈采样已完成：`d06-stacks-current-20261007` 的 4 个窗口共严格匹配 66,000 条接收和 132,000 个 waking/wakeup 栈事件，缺失、空栈、LOST、未知行均为 0。1 SUB 两窗的 wakeup 栈均为 100% `remote_pending`；32 SUB 两窗分别为 31,871/32,000 和 31,922/32,000 `remote_pending`，其余为 direct activation。32 SUB 最慢 1% 的 `remote_pending` `waking→wakeup` p50 为 150.264/140.883 µs，栈中可见 `sched_ttwu_pending` 与远程 call-function 激活链。该证据把慢段定位到目标线程激活前，但不能单独证明全部延迟来自 C-state，也不能推出增加通知、去重或复制会改善它；汇总见 `d06-stacks-current-20261007/d06-stack-summary.json`。
- D06 接收线程单独绑核诊断已完成：`d06-receiver-affinity-20261007` 共 16/16 窗、16000 次发布、264000 次接收，264000/264000 条调度链严格匹配，完整性和系统设置审计通过。该工具只固定实际 `recv_once` 接收线程，发布者、网关和同一订阅进程的其他线程保持原 affinity；实际 TID/CPU 集合逐窗核对。1 SUB/4KiB 两个平衡块端到端 p50/p99 分别改善 `-3.494/-83.236 us`、`-52.163/-120.976 us`，但 32 SUB/64B 两块分别退化 `+15.170/+28.579 us`、`+10.549/+41.030 us`；`waking->wakeup` 也没有跨场景稳定同向下降。该诊断不形成生产调度候选，详细汇总见 `d06-receiver-affinity-20261007/summary.md`。

## 当前开放项

- D04 权限及 A/B 角色关联阻塞已解除。D05/D06 已将主要慢段定位到目标线程激活前；IPI 冒烟及接收线程单独绑核均未给出跨目标场景稳定收益。CPU idle/IPI 相关证据不能单独证明整段都属于硬件退出时间，当前没有生产修复候选。
- `test_shm_ready_transition` 连续两次失败于两个链路前提用例（`drain=0`，worker 空闲退出），第三个状态转换用例通过；未将其归因于诊断 seam。
- 1MiB 的当前发布复制段存在窗口波动；D05 placement 对照未显示 assist 或复制实现的稳定收益，不能把它写成已经证明的生产回归。

## 不能关闭的结论

三个历史未通过项仍为“待正式验证”：当前 D02/D04 改善不能替代 D12。C1 已实际执行生产通知修改，但性能与兼容性均未达到准入条件，未形成可接受的生产 C。此次只新增测试专用接收线程亲和性诊断，没有修改生产接收、通知去重、复制或调度策略。未执行 C1 的 L0、OFF、ASan/UBSan 或 D12，未覆盖 `build-latency-formal`。

## 下一步准入

D05/D06 已完成但未形成生产候选；后续只有在出现可验证的机制新证据时，才继续围绕 CPU 类型、迁移及 `sched_ttwu_pending`/IPI 激活路径设计单因素候选。在有稳定 L0 收益及 D11 回归证据前，不修改通知去重、接收默认值、复制或调度策略；不能用被拒绝的 C1 或线程绑核诊断进入 D12。
