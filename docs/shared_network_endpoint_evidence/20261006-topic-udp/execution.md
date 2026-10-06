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
| N05 | 已完成 | `resources/n05-budget.md`；FD/端口/缓冲预算、状态读回和确定性失败回滚 |
| N06 | 已完成 | `validation/n06-wire-v2.md`；双版本 DATA/HELLO/catalog/目录 codec 和独立向量 |
| N07 | 已完成 | v2 pooled/hybrid/per-topic 生命周期、角色引用聚合、端点 owner add/remove、资源回滚与端到端覆盖；N08 等待/公平调度尚未实现 |
| N08 | 已完成 | `validation/n08-scheduling.md`；epoll 多 FD 等待、按 socket/RouteKey 轮转、deferred 续跑、exclusive worker 映射及 256 空闲端点验证；保留一次未复现的 1MiB BestEffort 丢收观察 |
| N09 | 已完成（保留失败样本） | `diagnostics/n09-20261006-185715/queue-attribution.md` 与 `queue-attribution.json`；同进程多话题、跨进程多话题均有分段链路，同话题双发布者可靠场景两轮均有 5 秒超时，未伪装成通过 |
| N10 | 已完成，候选优化回退 | `diagnostics/n10-20261006-191735/` 与 `n10-comparison.md`；队列拆分无稳定净收益，保留实验记录并回退生产实现；39/39 shared_net 回归通过；同话题双发布者可靠失败仍存在 |
| N11 | 已完成（保留失败分类） | `validation/n11-20261006-204455/results.md`；普通聚焦 13/13、OFF 聚焦 14/14、legacy 回退 3/3；全量普通 38/39、ASan/UBSan 36/39，失败均为已保留的 1MiB BestEffort 重组超时；生产实现未改变 |
| N12-N15 | 未开始 | N12 规模、资源耗尽、故障和生命周期压力矩阵 |

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

N05 验证命令和资源审计见 `resources/n05-budget.md`。构建与 shared_net 36/36 回归通过；配置、端点预算拒绝和 CLI 状态 JSON 均通过。N05 仅实现 v1 启动期预算，v2 动态端口分配器和按话题端点留给 N06/N07。

N06 验证命令和结果见 `validation/n06-wire-v2.md`。v1 字节兼容保持通过，v2 codec 已独立可测试；端点生命周期和模式接入留给 N07。

## N07 验证

N07 将 v2 pooled、hybrid 与 per-topic 接入网关端点生命周期。每个 RouteKey 聚合 PUB/SUB 注册引用；专用端点仅在首个本地角色注册时创建，在最后角色注销或会话超时时关闭。发送/接收路由通过当前端口绑定定位唯一 worker，v2 DATA 校验源/目标端口及 endpoint epoch；ACK/NACK/REJECT 原样回显 DATA 端点代次。动态端点创建、owner 注册和资源记账失败会回滚，端口冲突会尝试后续候选端口。

验证命令与结果：

```text
cmake --build build-shared-net --parallel 4
  退出码 0
ctest --test-dir build-shared-net -L shared_net -N
  38 tests
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1
  38/38 passed, 退出码 0, 23.20s
```

两机隔离命名空间的端到端附加场景按序运行，网关真实 UDP、应用进程 UDP 数为 0：

```text
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode hybrid --data-sockets 4 --data-workers 4 --dedicated-policy --lifecycle-roles sub
  退出码 0；SUB-only 端点与最后角色回收通过
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode hybrid --data-sockets 4 --data-workers 4 --dedicated-policy --lifecycle-roles pub,pub
  退出码 0；同 RouteKey 双 PUB 引用聚合与逐个注销通过
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode hybrid --data-sockets 4 --data-workers 4 --dedicated-policy --lifecycle-roles sub,sub
  退出码 0；同 RouteKey 双 SUB 引用聚合与逐个注销通过
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode per-topic --data-workers 4 --lifecycle-roles both
  退出码 0；both 对应两个注册引用，最后注销后 dedicated 端点回收
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode per-topic --data-workers 4
  退出码 0；BestEffort 覆盖 64B、1023B、1024B、1025B、4KiB、1MiB，两方向共 12 条，零重复、损坏或错误投递
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode per-topic --data-workers 4 --reliable
  退出码 0；Reliable 覆盖相同尺寸，两方向 6/6 完成，无超时
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode per-topic --data-workers 4 --block-first-dedicated-port
  退出码 0；两网关均跳过首个已占用候选端口并选中下一端口，端到端尺寸矩阵通过
python3 test/shared_net/end_to_end.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --network-version 2 --data-mode per-topic --data-workers 4 --expect-endpoint-failure --socket-buffer-budget-bytes 2097152
  退出码 0；dedicated 注册报告 BufferBudget，路由/数据端点未残留，端口账本恢复到控制/发现基线
```

旧端口和旧 source/target endpoint epoch 在 `test_shared_net_reassembly` 中于申请重组内存前被拒绝；错误可靠反馈代次在 `test_shared_net_reliable` 中不能完成事务。端点引用测试会分别区分 PUB-only、SUB-only、双角色和多角色退出。

执行中保留的测试修正/异常：首次 per-topic BestEffort 运行在暂停网关超过客户端 3 秒健康超时后，误把已由会话失效回收的端点当作“最后角色注销”失败；脚本现记录会话超时回收或主动注销回收，并由独立 lifecycle case 检查主动注销。`both` 的初次断言把一个 PUB+SUB probe 算作一个引用，已改为按两个角色登记计数。端口冲突场景最初有一次额外 SUB 注销后 UDP 计数 3→2 的断言失败；不改变行为代码的诊断运行和随后全量 CTest 均通过，现将此单次不一致保留为未复现观察，N08/N11 回归时继续关注。

本节点只完成功能接入与同机隔离验证；未测试物理跨机互通。`GatewayData::Shard` 当前仍用每轮 `poll` 构造并扫描本 worker 所有 socket 的等待集合，未宣称满足 N08 对大规模多 FD epoll 等待、公平预算和 deferred 续跑的要求。

## N08 验证

N08 将数据 worker 的逐轮 `pollfd` 构造改为 `SocketWaitSet`；Linux epoll 以固定 64 项批次等待，代际索引将内核事件关联至当前 token。worker 维护动态 endpoint owner 映射、ready socket 队列、按 socket/RouteKey 轮转的发送与接收队列；EAGAIN 按 socket 有界延后，接收 deferred 队列同时受包数和字节数约束，重组 tick 按 deadline 调度。发现 deferred 队列仍有 RouteKey 时，worker 仍读取预算内的其他已就绪 socket，避免热 RouteKey 挡住其它 socket。

策略映射将池 socket 固定给普通 worker；`exclusive_worker` 只接收明确绑定到该 worker 的 dedicated RouteKey。专用 worker 不足以承载普通话题、普通话题指定独占 worker、重复独占要求等配置会被拒绝；状态输出读回池 worker 映射与独占 worker 列表。

N08 构建与完整 shared_net 回归：`cmake --build build-shared-net --parallel 4` 退出码 0；最终 `ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1` 为 39/39 通过，27.45 秒。调度和 exclusive worker 变更后的聚焦等待集/端点 teardown/v2 e2e/exclusive worker/reliable e2e 为 5/5；之后并发 owner-slot 注册用例单独通过。256 个空闲 eventfd 和 256 个动态 UDP endpoint 的 CPU/wakeup/owner slot 结果、冷热同/异 socket 30 秒样本与原始文件、失败保留说明见 `validation/n08-scheduling.md`。

此前完整集曾有一次 exclusive worker 场景的 1MiB BestEffort 消息接收为空；同一测试的后续诊断运行通过，调度修正后的完整回归也通过。该失败保留为未复现观察，未归因为测试采样问题，也不声称调度修正已证明消除此丢收。最后角色释放时 `/proc` 与状态命令的不同时间点读数曾短暂显示额外 UDP FD；脚本现在限时轮询两个读回源直至连续两次一致，并仍对泄漏超时失败。

## N09 验证

N09 使用同一有界 message-trace schema 关联 `(gateway_epoch, session, publisher_id, sequence, RouteKey)`，覆盖应用 publish、outbox/drain、控制循环、数据 worker、首/末包、接收提交、ACK 和可靠完成。诊断窗口为 2 秒、每发布者 20 Hz、64B、可靠发布，按“轮次 → v1 pooled → v2 pooled → v2 per-topic”串行执行；共 20 个样本文件（两轮 × 三配置 × 三布置，加两轮 trace-off 开销配对），物理跨机标记为未验证。

验证命令及结果：

```text
cmake --build build-shared-net --parallel 4
  退出码 0
ctest --test-dir build-shared-net -R '^(test_shared_net_metrics|test_shared_net_end_to_end)$' --output-on-failure -j 1
  2/2 passed，退出码 0
python3 -m py_compile test/shared_net/end_to_end.py test/shared_net/topic_queue_diagnostic.py test/shared_net/analyze_queue_attribution.py
  退出码 0
python3 test/shared_net/topic_queue_diagnostic.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --output docs/shared_network_endpoint_evidence/20261006-topic-udp/diagnostics/n09-20261006-185715 --seconds 2 --rate 20 --payload-bytes 64
  退出码 1；20 个样本中 8 个失败，全部保留在 manifest.json 和 runs/，未用重跑结果替换
python3 test/shared_net/analyze_queue_attribution.py --input docs/shared_network_endpoint_evidence/20261006-topic-udp/diagnostics/n09-20261006-185715 --output docs/shared_network_endpoint_evidence/20261006-topic-udp/diagnostics/n09-20261006-185715/queue-attribution.json
  退出码 0；N10 进入判据在 5 个配置/布置组合上满足
```

失败集中在 `same_topic_multipublisher`：v1 pooled、v2 pooled、v2 per-topic 的两轮均出现两个发布者各发送 36 条、各 1 条可靠调用等待 5 秒后失败；这不是 trace 丢弃或内容校验被静默忽略。其它布置的接收内容校验通过，但有少数轮次出现未完成链路或 trace 丢弃，分析器按严格门槛排除这些轮次。合格轮次中 `ack_receive_to_gateway_result` 在连续两轮最慢 1% API 耗时占比约 35%～39%，因此 N10 需要先针对 ACK 结果回传 owner 设计；该结论不覆盖已失败的同话题多发布者可靠路径。

诊断工装同时修正了宿主响应解析：`load_all`/`recv_all` 返回 JSON 数组，`Process` 现在与对象响应一样入队，已用独立最小回归验证。N09 只定位排队区间，不替代 N13 的正式 3×60 秒窗口，也不构成物理跨机单向延迟证据。

## N10 验证与回退

N10 根据 N09 的 `ack_receive_to_gateway_result` 进入条件实现了 Result/Feedback 双 FIFO 队列和有限 Result 突发公平策略。候选实现未增加线程、socket、协议字段或可靠事务 owner；总事件记录数和字节预算保持不变。

验证命令及结果：

```text
cmake --build build-shared-net --parallel 4
  退出码 0
ctest --test-dir build-shared-net -R '^(test_shared_net_metrics|test_shared_net_end_to_end)$' --output-on-failure -j 1
  2/2 passed，退出码 0
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1
  39/39 passed，退出码 0
```

与 N09 相同的 20 个诊断样本按相同顺序串行执行。两批均保留 8 个同话题双发布者 Reliable 超时；每个发布者发送 36 条，各有 1 条调用等待约 5 秒后失败。对 12 个内容校验通过且 trace 链完整的同轮样本，N10 API p99 两轮中位数相对 N09 在 v1 pooled 跨/同进程分别为 -10.4%/-6.3%，v2 per-topic 分别为 -8.0%/-11.9%，v2 pooled 跨进程为 +7.5%、同进程为 -5.5%。逐轮方向混合，且 ACK 区间占比没有稳定下降；完整数据和计算口径见 [`n10-comparison.md`](diagnostics/n10-20261006-191735/n10-comparison.md)。

按照方案中“改造没有收益则回退并保留实验记录”的判据，N10 生产实现已回退到 N09 的单一事件队列。回退后的构建和 N11 回归作为下一节点验证对象；N10 设计、20 个原始样本、失败样本及分析 JSON 均保留。物理跨机仍未验证。

## N11 验证

N11 在 N10 回退后的生产实现上完成普通、shared-net OFF、ASan/UBSan 三套构建，修正 OFF 配置测试在未构建 shared-net 时的条件编译，并验证安装目录 legacy 回退。结果和原始日志见 [`validation/n11-20261006-204455/results.md`](validation/n11-20261006-204455/results.md)。

普通构建的 MPMC、DZFlat、生命周期和信息池聚焦集为 13/13；OFF 构建为 14/14；sanitizer 聚焦集为 12/13，唯一失败是已有的生命周期负向测试 `GenerationRebuildRequiresRemoveRouteFirstOrCrashStall`，ASan 记录预期的失效对象读取。安装目录回退脚本三项均通过，旧模式未连接共享网关。

本轮普通 shared-net 全量为 38/39，sanitizer 全量为 36/39。失败均集中于 1 MiB BestEffort 场景，接收端在约 500～630 KiB 后重组超时；失败状态没有 `wrong_shard`、CRC、非法包或发送错误，且 sanitizer 全量没有新增 ASan/UBSan 报告。该结果保留为 N12 压力矩阵的输入，不宣称 N11 已关闭大消息丢收问题。物理跨主机仍未验证。
