# N05 资源预算与端口审计

N05 为 v1 pooled 增加启动期资源审计和回滚边界。`GatewayConfig` 新增 `data-socket-cap`、`data-port-range`、`socket-fd-fraction`、`socket-buffer-budget-bytes`、`data-rcvbuf-bytes` 和 `data-sndbuf-bytes`；旧连续 `data-base-port` 仍兼容。数据、控制、发现端点统一设置本进程缓冲，并以 `getsockopt` 返回值记账。

本机只读审计（2026-10-06）：

```text
RLIMIT_NOFILE soft/hard: 1048576 / 1048576
fs.nr_open: 1048576
fs.file-max: 9223372036854775807
fs.file-nr: 36576 0 9223372036854775807
UDP sockets in use: 255
CPU allowed: 0-31 (32 CPUs)
```

默认配置的状态读回示例：`data_sockets=4`、`udp_sockets=6`、`fd_budget_limit=524288`、`fd_reserve=64`、`candidate_ports=29998`、`reserved_ports=6`，六个端点的实际收发缓冲均为 524288B，收发总和各 3145728B，低于 256MiB 总预算。状态字段位于 `resource_budget`，与 UDP 数量分开统计。

确定性验证：

```text
build-shared-net/bin/test_shared_net_config                         # 11/11
build-shared-net/bin/test_shared_net_endpoint                       # 6/6
python3 test/shared_net/cli.py --gateway build-shared-net/bin/dzipc_gateway
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1 # 36/36
```

资源失败测试设置 `data_socket_cap < data_shards` 或总缓冲预算为 1B，在创建任何 UDP socket 前返回 `SocketCap`/`BufferBudget`，进程内 socket 计数保持不变。实际 bind 失败仍由端点构造抛出并由网关启动路径回滚已创建的 `unique_ptr` 端点。
