# 固定容量 Loan 背压验证证据

实现使用每话题、每尺寸档、每 generation 共用的 10 块池；有界等待保留 1 块信用。成功发布的消息在每个订阅者处必须数量一致，缺失、重复、损坏、发布者内乱序和队列驱逐均为零，测试结束时池须一致并恢复到 10/10。突发场景允许明确的 `pool_exhausted`，不能把信用拒绝计作已接受消息丢失。

## 证据索引

| 路径 | 内容与结论 |
| --- | --- |
| `baseline_runs.json`、`baseline_*.txt` | 修改前 1/4/8/32 发布者基线，保留失败窗口。 |
| `optimized_final/` | 最终修复后的验收矩阵：raw/DZFlat × 1/4/8/32 发布者 × 1/8 订阅者 × 64 B/4 KiB/11 KiB/1 MiB × 0/800 Hz；128/128 配置、384 个窗口、196608 次尝试全部接受，池最大占用 9、最终回满 10/10。 |
| `stress_final/` | 最终修复后的 32 发布者、8 订阅者长压力：4/4 配置、12 个窗口，每窗 8192 次尝试；1 MiB DZFlat 三窗分别接受 8189/8183/8190 条、拒绝 3/9/2 次，其余配置全部接受。已接受消息全部可靠交付，池均回满。 |
| `optimized_verified/` | 最终订阅者变化修复之前的 128/128 配置对照。 |
| `stress_verified/` | 32 发布者、8 订阅者，每窗 8192 次尝试，4 配置均可靠；1 MiB DZFlat 三窗分别拒绝 7/2/6 次。 |
| `stress_rerun_verified/` | 独立长压力对照，4 配置均可靠；1 MiB DZFlat 三窗分别拒绝 3/7/4 次，其余配置全部接受。 |
| `regressions_verified/` | 固定容量、DZFlat、跨进程、覆写回收及共享网络相关聚焦回归。 |
| `comparison/` | 吞吐与延迟趋势。raw 新旧发布入口不同，不能用于证明无性能回退。 |
| `final_review/` | 最终全量构建后完整 CTest 为 74/81；专项 5/5、池回归 35/35、UF-011 严格回满 1/1、覆写安全 3/3、失联回收 2/2。7 项 CTest 失败在修改前库复现；保留此前 1 MiB UDP 失败及额外广播队列挂起记录。 |
| `consumer_final/` | 本轮最终独立消费验收：显式零拷贝/复制、1/4/8/32 发布者、1/8 订阅者、11000 B/1 MiB，另含 32 发布者长压力、2 ms 应用处理和 100 ms 慢启动；88/88 配置、264/264 窗口通过，池均回满。长压力 196608 次尝试、196595 次接受、13 次明确信用拒绝。 |
| `performance_final/` | 同一测量源码与匹配源码/头文件的旧库，raw 使用一致显式 loan 入口；首轮 32 配置为 12 通过、5 超预算、15 不可判定。旧版对象发布有 TLV 回退及 180 条缺失，当前全部 96 窗口无交付错误。 |
| `performance_loaned_final/` | DZFlat B 级直接借样的独立消费对照：16 配置、96 窗口全部交付正确、无 TLV 回退；首轮 15 配置预算通过，1 项超预算。不能替代对象发布入口的不可判定结论。 |
| `performance_final_confirmation_*/` | 按预先冻结的相同阈值对最终首轮 6 个超预算项各确认一次，3 个通过、3 个仍超预算；长窗口结果与首轮同时保留。 |
| `acceptance_review/` | 最终报告 `report.md`、可复核索引 `report.json`、测量源码/编译身份及本轮聚焦 CTest 20/20，其中 `loan_pool_*` 12/12；池 35/35、loan 10/10、严格回满 1/1、覆写 3/3、失联回收 2/2。 |

最终验收使用 `optimized_final/` 和 `stress_final/`；其他矩阵与回归目录保存实施过程中的结果，包括失败结果。每个矩阵目录的 `runs.json` 保存命令、退出码、逐窗结果及判定，`results.csv` 保存消息结果，`occupancy/` 保存每 2 ms 的池占用采样。

上述两个目录是固定容量阶段的完整可靠性矩阵。本轮性能预算及独立消费结论以 `acceptance_review/report.md` 为准，最终消费曲线共 264 条。报告逐条复核原始输出、JSON、预算身份、配置覆盖和池占用曲线；任何交付错误、路径回退或测量格式错误均不会写成通过。

`final_review/wait_subscriber_before_fix.txt` 与 `wait_subscriber_after_fix.txt` 保存信用等待期间订阅者加入/退出的确定性复现与修复验证。槽位实际接收者现在在提交时更新到 chunk 位图，覆盖单发布者和多发布者广播，防止提前归还与信用残留。

`final_review/ctest.txt` 保留此前 73/81 结果，`ctest_after_fix.txt` 为最终 74/81 结果。7 项剩余失败与修改前对照断言一致；前次额外失败的 1 MiB UDP 集成用例在修改前对照和最新完整回归中通过，其根因仍未确认。非 CTest 的 `test_queue_run.txt` 广播用例出现重试耗尽并挂起；`baseline_test_queue_run.txt` 在匹配 HEAD 队列头文件的修改前程序上复现相同现象。不能将额外队列验证记为通过。

## 复现

```bash
cmake --build build --target ipc loan_phase_latency_measure test_topic_chunk_pool test_uf011_chunk_return -j4
ctest --test-dir build -R '^loan_pool_' --output-on-failure
build/bin/test_topic_chunk_pool
UF011_EXPECT_LEAK_FREE=1 build/bin/test_uf011_chunk_return

# 普通矩阵要求全部尝试成功；输出到新目录，保留既有证据。
python3 test/perf/loan_backpressure/run_matrix.py --require-all \
  --output test/perf/loan_backpressure/reproduced_matrix

# 32 发布者长压力只允许明确的信用拒绝，已接受消息必须全部可靠交付。
python3 test/perf/loan_backpressure/run_matrix.py --stress --messages 8192 \
  --output test/perf/loan_backpressure/reproduced_stress

# 完整回归要求先全量重新编译，避免库与客户端头文件版本不一致。
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## 独立验收结论

消费验收 88/88 配置通过。复制模式先把 DZFlat 视图复制到预留的 owning `StdImage`，或将 raw 实际消息字节复制到本地缓冲；共享样本释放后才执行完整校验与受控应用处理。raw 的 `sent -> acquired` 单列为 `transport_latency`，DZFlat 的应用队列驻留单列为 `queue_residence`，不把两者混称为接收耗时。

预算在运行前写入 `acceptance_budget.json`：吞吐 >= 基线 90%，发布 p99 <= 基线 120%，单位消息 CPU <= 基线 125%，RSS <= 基线 125% + 8 MiB，单发布者 RSS 硬上限 512 MiB。每窗独立进程、交错运行新旧版本，比较三窗中位数；Linux RSS 使用当前地址空间的 `/proc/self/status` `VmHWM`，避免 `ru_maxrss` 继承父进程高水位。所有旧口径轮次及日志中断轮次均保留，详见最终报告。

一次长窗口确认后的结果为 raw 14/16、DZFlat B 级 15/16、DZFlat 对象入口 1/16 预算通过；对象入口其余 15 配置不可判定。最终三个超预算配置是：raw 1 MiB/8 订阅者的零拷贝与复制模式，以及 DZFlat B 级 4 KiB/1 订阅者复制模式的 p99。最终可比配置的 RSS 预算均通过。保持容量 10、信用 9，性能预算未全部通过，自适应扩容尚未实现且不准入。

本轮没有重跑完整 CTest，前序 74/81 和已复现的 7 项失败仍保留。新增公开结构字段和 V5 队列描述符要求客户端重新编译；旧 V4 队列仅在最后话题租约释放后清理。

## 独立验收复现

```bash
cmake --build build --target loan_phase_latency_measure -j4
ctest --test-dir build -R '^loan_pool_' --output-on-failure

# 导出指定版本到新的临时目录，使用匹配旧头文件/库编译相同测量源码。
python3 test/perf/loan_backpressure/build_baseline.py \
  --revision ea6c11487ca6148862caeaf09fb1ddeea3048438 \
  --directory /tmp/loan_acceptance_reproduced \
  --output test/perf/loan_backpressure/reproduced_performance

python3 test/perf/loan_backpressure/run_acceptance.py --kind consumer \
  --output test/perf/loan_backpressure/reproduced_consumer

python3 test/perf/loan_backpressure/run_acceptance.py --kind performance \
  --baseline /tmp/loan_acceptance_reproduced/build/bin/loan_phase_latency_baseline \
  --output test/perf/loan_backpressure/reproduced_performance

python3 test/perf/loan_backpressure/run_acceptance.py --kind performance \
  --baseline /tmp/loan_acceptance_reproduced/build/bin/loan_phase_latency_baseline \
  --budget test/perf/loan_backpressure/performance_loaned_budget.json \
  --output test/perf/loan_backpressure/reproduced_loaned_performance

# 原有最终证据的独立复核与报告生成。
python3 test/perf/loan_backpressure/report_acceptance.py
```

性能程序存在超预算或不可判定时退出码为 1；它不会更改阈值或把这些状态并入可靠性通过。`performance_final_confirmation_budget.json` 给出本轮仅确认一次的 6 个明确失败项和长窗口长度，`run_acceptance.py --only` 可单独复现它们。性能阈值不纳入普通 CI，32 发布者可靠交付和验收判定器的回归已纳入 CTest。
