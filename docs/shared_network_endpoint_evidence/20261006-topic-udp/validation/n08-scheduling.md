# N08 多 socket 等待与公平调度验证

## 实现范围

`GatewayData` worker 使用 `SocketWaitSet` 的 epoll 等待数据 FD 和命令唤醒 FD。每轮接收按 socket 与 RouteKey 轮转，并受包数、字节和时间预算约束；deferred 接收工作保留可续跑队列。已就绪 socket 即使 worker 尚有 deferred RouteKey，也继续参与本轮读取，避免一个 RouteKey 的处理队列挡住其它 socket。发送按 socket、RouteKey 轮转，EAGAIN 以有界延后重试。端点 token 使用注册代次；动态端点由 owner worker 确认加入或移除。

话题策略将 `dedicated` socket 与 `exclusive_worker` 分开配置。普通池 socket 仅分配给普通 worker，显式独占 worker 不接受池 socket；状态读回包含 `pooled_socket_workers` 与 `exclusive_workers`。

## 等待、空闲与生命周期

`test_socket_wait_set` 的 `LargeIdleSetAndHighTokenReuse` 覆盖 256 个 eventfd：空闲等待 200ms 阻塞且没有 ready token，进程 CPU 时间小于 50ms；单 FD 置位只返回对应 token；最高 token 注销后可增补新 token，旧 token 不误命中，最终 0 个通道残留。

`test_shared_net_teardown` 的 `ManyIdleGatewayEndpointsBlockAndReturnOwnerSlots` 由 4 个调用线程并发向 4 个 worker 注册 256 个空闲 UDP 端点，每 worker 64 个。全局 socket index 使用原子递增。200ms 采样期间 worker wakeup 计数不变、进程 CPU 时间小于 50ms；注销最高 slot 后重新注册并移除全部端点，最终每 worker socket 数均为 0。

`test_shared_net_v2_exclusive_worker` 读回 dedicated RouteKey 的 owner 为 0、普通池 socket 的 worker 列表不含 0、exclusive worker 列表为 `[0]`，并覆盖端点创建、收发及释放。最后角色关闭的资源检查分别读取 `/proc` FD 和网关状态，因此改为最多等待 3 秒，要求连续两次同时满足路由为空、UDP 数精确回到池 socket 数加控制/发现 2 个、Ready 数等于池 socket 数；超时仍失败。

## RouteKey 公平性

使用现有 `test/shared_net/fairness.py` 在两个同机隔离主机实例中各运行 30 秒。固定热流 1MiB、冷流 64B，冷流 100Hz，未开启诊断打点。每个场景均收到冷流 3000 条，0 失败、0 无效、0 重复：

| 场景 | RouteKey / shard | 热流实际发送 | 冷流最大接收间隔 |
|---|---|---:|---:|
| 同 socket | `fairness-hot` / `fairness-cold-2`，2 / 2 | 4193 条，140Hz | 12.62ms |
| 异 socket | `fairness-hot` / `fairness-cold-0`，2 / 1 | 4116 条，137.2Hz | 10.65ms |

原始样本：[同 socket](n08-fairness-same-socket.json)、[异 socket](n08-fairness-different-socket.json)。这组工装运行网络 v1 pooled；它验证 RouteKey 同/异 socket 的热冷公平性，不作为 v2 exclusive worker 的性能结论。

## 构建与回归

```text
cmake --build build-shared-net --parallel 4
  退出码 0
ctest --test-dir build-shared-net -R '^(test_socket_wait_set|test_shared_net_teardown|test_shared_net_end_to_end|test_shared_net_v2_exclusive_worker|test_shared_net_reliable_end_to_end)$' --output-on-failure -j 1
  5/5 通过，18.41 秒
python3 test/shared_net/fairness.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --same-shard --output validation/n08-fairness-same-socket.json
  退出码 0；冷流 3000/3000
python3 test/shared_net/fairness.py --gateway build-shared-net/bin/dzipc_gateway --probe build-shared-net/bin/shared_net_probe --output validation/n08-fairness-different-socket.json
  退出码 0；冷流 3000/3000
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1
  39/39 通过，27.45 秒
```

完整回归中的一次失败保留如下：收包矩阵在一条 1MiB BestEffort 消息上未收到预期消息（初次输出仅有 `received=[]`），其余 38 项通过。专用 worker 测试随后单独诊断运行通过，调度修正后的聚焦集 5/5、完整集 39/39 通过。该单次丢收作为未复现观察保留，不据此宣称丢包问题已由调度修正解决；后续性能/故障矩阵继续检查大消息 BestEffort。物理跨机未测试，本证据是同机隔离实例。
