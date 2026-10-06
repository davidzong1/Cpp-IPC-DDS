# 取得 sudo 授权后的 perf 采样方案

本轮沿用独立分支和原生产库。用户已明确授权 sudo 采样；不保存凭据，不改动 sysctl、CPU 电源/空闲态或另一 Agent 的代码。

1. 冒烟：1 SUB/4KiB，3 秒，校验线程 ID、CLOCK_MONOTONIC 对齐、tracepoint 过滤器和事件丢失。
2. 调度对照：1 SUB/4KiB、32 SUB/64B，每项两轮，第一轮 perf OFF→ON、第二轮 ON→OFF；100Hz，预热 2 秒、正式 10 秒，接收/发布 CPU 诊断始终开启。所有 CPU0～31 可用；不是正式性能验收。
3. 复制对照：1 SUB/1MiB，P/P（PUB0/SUB4）、P/E（PUB0/SUB16）、E/P（PUB16/SUB4），网关 CPU2；两轮逆序，perf 开启，其他参数相同。固定核仅用于诊断，不替代原门槛。
4. 按序号关联 notify→sched_waking→sched_wakeup→sched_switch(next=reader)→wait_end。找不到完整链的样本单列，不能填零。关联目标 CPU 在 waking 时的空闲态及 idle exit；只陈述相关性，不把公布的退出延迟当作实测耗时。
5. 检查 perf lost events、原始 CSV 守恒、实际 UID、链接库、CPU 集合、二进制指纹和采样进程活动。全部窗口保留，分别提交工装、证据与结论；不因单个有利窗口关闭旧失败。

仅 perf 使用 root，业务工装通过 setpriv 降回原用户，在原有隔离 SHM 命名空间内运行。采样范围为 benchmark 名称的调度事件及 CPU 空闲事件，不读取其他进程的业务数据。
