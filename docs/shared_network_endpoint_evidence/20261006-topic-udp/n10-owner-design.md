# N10 结果事件 owner 设计

## 证据与范围

N09 的合格轮次显示 `ack_receive_to_gateway_result` 在最慢 1% 可靠发布调用中连续两轮占 35%～39%。该区间从数据 worker 处理 ACK 到网关控制 owner 将 `SendResult` 放入会话输出结果队列；同一 `GatewayData::events` 队列还承载接收侧 Feedback。N09 的同话题双发布者可靠超时是独立的失败路径，本设计不把它改写为通过。

本节点保持数据 socket、worker 数、CPU 预算和应用侧协议不变。优化目标是减少 Result 在共享事件队列中的队头等待，不改变可靠事务的所有权边界。

## 所有权不变量

```text
数据 worker（每个 RouteKey 固定 owner）
  ReliableSession、重传、ACK/NACK 验证、事务终结
       |
       | 有界 Result/Feedback 事件队列（GatewayData mutex 保护）
       v
网关控制循环（唯一 owner）
  Session::pending_sends、send_results、result_order、session.output
```

数据 worker 仍是 `ReliableSession` 的唯一写 owner；控制循环仍是本地会话表和控制回复的唯一写 owner。worker 不直接访问 `Session`，控制循环不读取或修改 worker 的可靠状态。每个 Result 仍带原始 `request_id`、publisher、sequence 和 `OutboxHeader`，现有 `pending_sends.erase` 继续提供重复 Record/Result 的恰好一次结算。

## 队列策略

`GatewayData` 将原来的单一事件 deque 拆成 FIFO 的 `result_events` 和 `feedback_events`。两者共用原有 `command_records` 与 `command_bytes` 总预算；Result 队列耗尽会保持现有的 worker 停止/失败语义，Feedback 队列耗尽继续保持现有的有界丢弃计数。入队仍唤醒同一个网关控制循环，不新增线程或 socket。

`pop` 在两个队列之间采用有限突发：最多连续取 8 个 Result 后让出一个待处理 Feedback；没有 Feedback 时立即取 Result，没有 Result 时取 Feedback。这样 ACK 完成可以越过不相关反馈的队头，同时控制反馈不会被可靠结果流永久饿死。控制循环每轮总处理上限仍为 64，路由/控制 token、目录发送和关闭屏障不变。

## 关闭、异常与信用

停止时清空两个事件队列并归零同一总字节账本。Result 队列满、事件内存异常、worker 异常退出仍使网关进入原有失败路径；Feedback 丢弃仍只增加 `dropped_feedback`。Result 的 `finish_send`、`SendResult` 缓存、信用回复和应用 `SendWaitTable` 不改变，因此 request_id 冲突、重复 Record、发布者序号、混合 Reliable/BestEffort、慢消费者和注销竞争沿用现有契约。

## 验收

先运行构建、`test_shared_net_metrics`、`test_shared_net_end_to_end` 和 shared_net 相关回归，再用与 N09 相同的诊断 schema、配置、顺序和 CPU 预算复测。报告 Result/Feedback 队列的内容正确性、trace 丢弃、可靠完成、超时和 `ack_receive_to_gateway_result` 分段；若该区间没有可重复改善或出现公平/关闭回归，保留设计记录并回退实现。
