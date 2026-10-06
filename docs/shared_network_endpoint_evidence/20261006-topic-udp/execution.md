# 按话题 UDP 数据通道执行台账

本批次对应 `docs/topic_udp_data_channel_execution_plan.md`，证据目录为新目录，不覆盖历史批次。

## 起点

| 项目 | 值 |
|---|---|
| 起点 HEAD | `7663871ed68a0ebb3e77aa84aca0a4ce20e45cb4` |
| 方案提交 | `7663871e` |
| 起点工作区 | `artifacts/perf/20261006-dzflat-paths/` 与 `docs/shared_network_endpoint_evidence/20261006-multipublisher-latency-report.md` 未跟踪；归属外部任务，保留不改、不提交 |
| 旧生产基线 | `f066a82`，shared-net 生产实现基线 |
| 原网络基线 | `docs/shared_network_endpoint_evidence/20261006-execution/`，用途与旧生产基线不同 |
| 物理跨机 | 当前环境未提供第二台物理主机，跨机端到端验收保留未验证 |

## 节点状态

| 节点 | 状态 | 产物/说明 |
|---|---|---|
| N00 | 已完成 | 本台账、环境审计、冻结基线清单 |
| N01 | 已完成 | `test/shared_net/topic_udp_matrix.py`、计划 schema、统计单元测试和冒烟记录 |
| N02 | 已完成（短基线） | `baseline-network/results.md`；K=4/8/16 改造前与 S/W 解耦短窗口均完整交付，正式长窗口仍留给 N13 |
| N03 | 已完成 | `design.md` 冻结 v1 S/W 配置、映射、所有权和 N02 选择依据；v2/资源预算留给后续节点 |
| N04 | 已完成 | `candidate-v1-decoupled/results.md`；v1 pooled S/W 解耦实现、状态读回、参数转发和专项验证 |
| N05-N15 | 未开始 | 依赖前序节点 |

## N00 验证

起点检查命令：

```text
git rev-parse HEAD
git status --short
uname -a
ulimit -Sn; ulimit -Hn
cat /proc/sys/fs/nr_open /proc/sys/fs/file-max /proc/sys/fs/file-nr
cat /proc/net/sockstat
nproc; taskset -pc $$
```

N00 尚未修改生产行为。环境原始输出见 `environment.txt`，计划和代码哈希见 `plans/` 与后续 `binary_manifest.json`。

## N01 验证约定

`topic_udp_matrix.py` 使用显式 JSON 计划，不复用 `performance_matrix.py` 的固定 54 窗口入口。它在 `--validate-plan` 阶段校验 RouteKey、发布者布置、发送时间表、资源预算和 nearest-rank 统计；`--preflight` 只运行短冒烟并标记为非正式证据。正式窗口失败会保留结果并以非零退出，不把基础设施失败归类为性能退化。N01 校验和 preflight 已通过；`preflight2/` 是一次重复验证目录，未作为正式证据使用。

N02 短基线已完成并写入 `baseline-network/results.md`；N03 设计记录已根据短基线冻结默认和后续比较口径。

N04 验证命令：`cmake --build build-shared-net --parallel 4`（退出码 0）；`ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1`（36/36，退出码 0）。S/W=(1,1),(4,1),(4,2),(4,4),(4,8),(16,4) 均完成 300/300 短冒烟，状态读回的 `data_sockets`、`data_workers`、`gateway_threads` 与映射一致，未观察 `wrong_shard` 或漏进展。
