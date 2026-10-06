# N03 配置、协议与所有权冻结记录

## 当前起点

N00/N01 提交为 `f0ed27f7`。本设计对应方案中的网络 v1 pooled 解耦阶段，保留既有 DZMX v1、HELLO v1、连续数据端口和应用 API。网络 v2、按话题端点和资源分配器在 N05/N06/N07 单独实现，不能由本阶段的 worker 配置暗含启用。

## N02 配置选择依据

N02 的短基线在相同 3 秒、100 Hz、64B、单订阅者条件下完成 300/300 交付。原 v1 的 K=8 在短窗口 p99 为 148.557 us，K=4 为 163.904 us，K=16 为 169.838 us；改造后的 S=4/W=4 为 73.997 us、S=4/W=8 为 43.749 us。S=16/W=4 在单低流量话题下 p99 为 182.138 us，且线程数没有增加到 19，说明端点数量和 owner 数必须分开评估。

因此 N04 保留 S=4/W=4 作为默认兼容配置，继续覆盖 S<W、S=W 和 S>W 的专项 case；N13 再用固定冷热流和可靠模式验证短窗口观察是否可重复。短基线只证明映射和功能可运行，不冻结 socket 数的性能结论，也不把 W=8 的额外线程收益归因于 socket 隔离。

## 配置契约

本阶段新增 `--data-workers W`，默认 `W=S`，取值 `1..CPU budget`。旧 `--data-shards S` 仍表示数据 socket 数和 v1 对外的 `data_shards`，默认 4，取值 `1..16`。两者均显式给出时允许，但 `data-workers` 只改变本地 owner 数，不改变 HELLO 中的 `data_shards`；不改变 v1 对端端口计算。

`S` 是 socket 数，`W` 是数据 worker 数。稳定映射为：

```text
socket_index = route_hash(RouteKey) % S
worker_index = socket_index % W
```

该阶段不按 `route_hash % W` 决定端口，也不向对端公告 W。每个 worker 独占其映射到的 socket 集合，所有属于同一 socket 的收发、重组、发送队列和可靠事务仍在同一 owner 中串行执行。

## 所有权和生命周期

`GatewayData::Impl::Shard` 改名语义为数据 owner，但保留内部类型名以缩小改动。构造 `W` 个 owner；每个 owner 接收其 socket 集合。owner 线程退出前停止接受命令、完成或取消队列中的事务、摘除等待 token，再关闭所持 socket。控制线程只通过 wake/command/fence 更新视图，不持目录锁等待 owner 内部同步。

接收仍使用每个 socket 对应的 `ReassemblyShard`，但所有实例共享一个 `ReassemblyBudget`；预算不乘以 S。v1 `wrong_shard` 校验继续使用对端 HELLO 的 `data_shards` 和完整 RouteKey，不能使用本地 W。

## 控制和反馈

控制 socket/发现 socket 继续由网关控制线程持有。ACK/NACK/REJECT 通过控制线程发送，N04 不改变可靠语义。数据 owner 产生 `GatewayDataEvent` 后唤醒控制线程；控制线程继续负责会话结果和 credit grant。N09 才根据分段证据决定是否改变这一链路。

## 兼容和失败边界

- S=4,W=4 的默认行为保持原有线程数和端口。
- S=4,W=1/2/8 不改变 HELLO、RouteKey、消息编码或应用发布返回。
- W 大于 CPU 预算由配置校验拒绝；不自动创建更多线程。
- owner 线程创建失败、socket 映射不一致或关闭 barrier 未完成，网关启动失败；不以部分数据端点宣告 Ready。
- v1 对端仍只能看到 S 个数据端口；v2 endpoint epoch/目录字段不在本阶段引入。

## N04 验收 case

固定 `(S,W)=(1,1),(4,1),(4,2),(4,4),(4,8),(16,4)`。每个 case 必须读回实际 data sockets、owner threads、data ports、发送/接收端口和 `wrong_shard`，并覆盖普通/可靠、同话题多发布及不同话题冷热流。功能先于性能；若 S/W 切分通过但延迟未改善，保留数据并进入 N09 归因。
