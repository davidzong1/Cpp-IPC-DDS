# W03 运行判定 — 20260928-r04-W03

## 口径

| 项 | 值 | 证据文件 |
|---|---|---|
| 时基 | CLOCK_MONOTONIC | manifest.json / environment.json |
| 计时成本 | 15.3319 ns/call (vDSO=是, 强制 syscall=125.837 ns) | observation_overhead.json |
| 跨进程时钟一致性 | 通过 (fork+pipe 区间 392913 ns) | manifest.json |
| 上下文切换完整口径 | rusage ru_nvcsw/ru_nivcsw（整个线程组生命周期） | cpu.json |
| 上下文切换采样口径 | 逐 TID 增量 + complete/undercount_bound 标注 | cpu.json |
| 观测窗口 | 70.0688 s（要求 >= 60 s） | idle.json |
| 诊断计数 | 开启 | counters.json |
| 采样行数 | 280 (间隔 250 ms) | samples.csv |
| 恢复首包延迟 | count=0, mean=0 us, p99=0 us, lost=0, rereg=0 | idle.json |

## 相位

| 相位 | covered | threads mean/min/max | cpu_cores mean | ctx/s mean |
|---|---|---|---|---|
| startup_peak | yes | 18/18/18 | 0.0419912 | 15245.5 |
| steady_active | yes | 7.82927/1/8 | 0.01948 | 5608.57 |
| idle_fallback | yes | 2/2/2 | 0.00054506 | 49.8727 |

周期任务覆盖: 已声明, ticks=3500, source=w03_thread_workload:periodic tick 50Hz
空闲回落: 已观测 (线程回落比例 0.255452)
已知线程校准: 期望 18, 观测峰值 18 => PASS

## 路径证据

已确认 0 条, 未确认 5 条

- **未确认**: tlv
- **未确认**: dzflat-a
- **未确认**: dzflat-b
- **未确认**: cyclonedds-iox
- **未确认**: compat-fallback

## 未确认项（必须随结论一起报告）

- 未执行/未提供连接-静默-恢复强制用例 => 恢复首包延迟未确认
- 未提供路径证据规格: 所有数据路径按 未确认 登记

## 逐项引用

- 相位数值: idle.json phases.*
- 进程 CPU/上下文切换: cpu.json processes[]
- 采样序列: samples.csv
- 计数开销: observation_overhead.json
- 字段定义: schema.json / schema.csv
- 路径证据: path_evidence.json
- 原始 stdout/stderr: stdout/*.log, stderr/*.log
