# 本机延迟根因测试执行台账

## D00

- A：`f066a82c1b4b10ea28322f86042bf6da5e122318`，`/tmp/dzipc-root-cause-baseline`，baseline/IPC_SHM。
- B：`94856f7bcef69e054ce679420dd8b7ee8e6b0c1e`，`/tmp/dzipc-local-latency-root-cause`，shared_v1/本机 MPMC。
- C：尚不存在。
- 本批证据：`docs/shared_network_endpoint_evidence/20261007-local-root-cause/`。
- 工作区、版本和环境清单见 `plan.json`、`provenance.json`、`environment.json`。
- 当前只使用独立工作区，不改主工作区和整合交付工作区。

## 计划

D01 先完成双版本构建、工装自检和能力矩阵；D02 运行三失败场景的 8 窗 ABBA|BAAB L0 同期对照。采样、解码和回归串行执行。

## 开放项

真实正式 54 窗、生产候选 C、D10-D13 尚未执行；三项失败的根因需由新证据确认。

## D01

- B 配置/构建：`cmake -S . -B build-latency-formal -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDZIPC_BUILD_SHARED_NET=ON -DLIBIPC_BUILD_TESTS=ON -DLIBIPC_BUILD_PYTHON=OFF -DLIBIPC_BUILD_DEMOS=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF`，构建退出码 0。
- A 生产库构建：冻结 A 不含 shared-net；首次全量 CMake 生成因忽略的 `test/perf/w10/w10_rebuild_crash.cpp` 缺失而失败，随后以 `LIBIPC_BUILD_TESTS=OFF` 构建 `ipc` 目标成功。A 的同一 benchmark 工装使用 A 头文件/生成消息头和 A `libipc.so` 手工链接，未复制 B 生产代码。
- 解析器自检：D01 原记录中的“10 项”计数不准确；当前 8 个 `test_*.py` 脚本共 22 个用例全部通过。增加逐窗环境检测后，当前共 27 个用例通过；能力清单见 `tool-validation/capability.json`。
- 六个 L0 冒烟：`tool-validation/d01-smoke-002/`，A/B 各覆盖 1 SUB/4KiB、32 SUB/64B、1 SUB/1MiB，3 秒正式窗口、100 Hz、2 秒预热；6/6 退出码 0，发布/接收完整、无重复/损坏/丢失，业务进程 UDP FD 为 0。中断的首次尝试 `d01-smoke-001/` 保留，未计入通过样本。
- 运行器：`test/shared_net/run_local_latency_experiments.py`，串行窗口、20 秒静置检查、每窗哈希和环境快照；采样期间每秒记录进程 CPU 增量，同一进程实例累计 CPU 达 1 秒且工作目录不属于测试 worktree 时标记为竞争，并停止受影响的平衡批次。低于阈值、短于采样间隔的进程和内核活动仍可能漏检，不能声称系统绝对空闲。

D02 进入条件满足：A/B 双侧工装完整性成立，开始三失败场景的 24 窗 L0 `ABBA|BAAB` 对照。

## D02 后续批次

- `d02-l0-pairs/005/` 在第 2 窗停止。JAX 负载位于用户声明的 `/home/zwc/MPC_GPU`，已按要求不终止；实际触发停止的是 `/home/zwc/.vscode-server` 的代码索引进程（工作目录 `/home/zwc`，不属于 JAX 根目录）。该批次保留但不拼入统计。
- 为继续取得可比的完整块，用户环境活动根在启动参数中声明为 `/home/zwc`，其中包含 `/home/zwc/MPC_GPU`。所有该根下活动仍记录在每窗 `environment.json` 的 `ambient_activity`，不触发中止；根外进程仍按原 1 秒 CPU 阈值检测。
- `d02-l0-pairs/006/` 完成 24/24 窗，顺序为每场景 `ABBA|BAAB`，10 秒正式、100 Hz、L0。三场景共 272000 条接收样本，完整性错误 0，未声明竞争者窗口 0；环境根活动累计 70.310 CPU 秒，因此该批次是“声明环境负载下的同期对照”，不是原静置门控意义上的绝对空闲批次。
- D02 汇总见 `d02-l0-pairs/006/summary.md`；全部订阅者原始 CSV 先合并后计算总体分位数。相对 A，B 的总体端到端 p50/p99 分别为：1/4KiB `16.205/162.890` 对 `29.603/217.458` µs，1/1MiB `68.760/148.875` 对 `94.610/241.881` µs，32/64B `26.053/167.999` 对 `40.023/209.785` µs。

## D03

- 驱动扩展为 `d03`，固定顺序 `A0 B0 B1 A1 | A1 B1 B0 A0`；仅开启接收应用缝，A 不强开其能力矩阵中不存在的发布分段。D03-001 的 16/16 窗均退出码 0，264000 条接收样本完整，无丢失、重复、错误或 trace 溢出。
- A 的 L1 只提供 `recv_return`；`recv_begin/enqueue/dequeue/wait/assist` 字段为能力缺失，汇总中保持未采样。B 的 L1 完整路径分为发布起点→recv 返回、recv 返回→入队、入队→出队、出队→API 返回。
- D03 L1−L0 观测开销方向不稳定：1/4KiB A 的 p50 增加 66.912 µs、B 减少 36.354 µs；32/64B A 增加 2.407 µs、B 增加 10.467 µs。因此 L1 绝对端到端值不能作为正式性能结论；仅使用同窗完整段的定位信息。
- D03 汇总见 `d03-app-001/summary.md`。D03 显示 B 的 4KiB/32SUB L1 主要耗时在发布起点→recv 返回，B 的后续应用段 p99 分别约为 4.880/7.844/2.157 µs 和 6.890/25.489/3.006 µs（各段独立分位数，不相加）。

## D04 能力预检

- `tool-validation/d04-scheduler-capability.json` 已保存本机预检。当前用户 `perf_event_paranoid=4`，普通用户 `perf stat` 被内核拒绝；`sudo -n` 需要密码，tracefs/debugfs 事件格式对当前用户不可见。
- 因此没有启动伪造的 D04 调度窗口，也没有把历史 `waking→wakeup` 栈证据冒充当前 A/B 调度链。D05/D06/D08 条件实验等待可用的提权采集器或用户提供等价原始事件证据。

## D02 中断批次

- `d02-l0-pairs/001/` 已启动但被外部 JAX CPU 密集任务打断；只完成前 9/24 窗。结果内容计数完整（9000 次发布、72000 次接收），但 4KiB 窗静置记录已出现外部竞争进程，32SUB 场景期间又观察到高负载任务。采样窗没有结束快照，不能按新检测逻辑完整判定环境。
- 本批整体标记为中断且不用于 A/B 性能比较；原始 CSV、日志、结果及静置记录完整保留，具体观察见 `d02-l0-pairs/001/status.md`。不从该批抽取有利窗口。
- `d02-l0-pairs/002/` 在启动第一窗前未能取得连续 20 秒无可见竞争任务的静置窗口，未启动业务采样；该批按静置门控失败记录并保留，详见 `d02-l0-pairs/002/status.md`。

## D02 运行器与环境失败批次

- `d02-l0-pairs/003/` 的第一窗业务采样完成，但旧运行器在监控收尾调用 `activity_delta()` 时传入错误的数据形状，因 `KeyError: 'processes'` 退出；该批不用于比较，详见 `d02-l0-pairs/003/status.md`。
- `d02-l0-pairs/004/` 的第一窗完成后发现外部 JAX 进程在窗内累计 CPU 超过阈值，按规则停止；JAX 未被本任务终止，该批不用于比较，详见 `d02-l0-pairs/004/status.md`。
