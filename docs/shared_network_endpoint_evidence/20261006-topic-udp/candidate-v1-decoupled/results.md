# N04 v1 pooled S/W 解耦专项结果

本批次验证 v1 pooled 的稳定 socket 到 worker 映射。所有窗口使用当前候选构建、3 秒采样、100 Hz、64B、单订阅者、300 条样本、BestEffort；结果只用于功能和映射验收，不替代 N13 的正式长窗口。

| 配置 | 完整交付 | data sockets | data workers | gateway threads | 备注 |
|---|---:|---:|---:|---:|---|
| S=1,W=1 | 300/300 | 1 | 1 | 4 | 状态读回确认端口和 owner |
| S=4,W=1 | 300/300 | 4 | 1 | 4 | 多 socket 单 owner |
| S=4,W=2 | 300/300 | 4 | 2 | 5 | S>W |
| S=4,W=4 | 300/300 | 4 | 4 | 7 | 默认兼容配置 |
| S=4,W=8 | 300/300 | 4 | 8 | 11 | S<W |
| S=16,W=4 | 300/300 | 16 | 4 | 7 | 多 socket 单低流量话题，不作性能结论 |

准确构建和功能验证命令：

```text
cmake --build build-shared-net --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1
```

结果：构建退出码 0；shared_net 36/36 通过。`S=1,W=1` 冒烟命令使用 `test/shared_net/benchmark.py`，结果目录为执行机上的 `/tmp/topic-udp-n04-s1w1-20261006`，300/300 交付；其余五个 case 的完整短样本已在 N04 首版执行记录中保存。所有正式压测仍需按 N13 新证据目录串行运行。
