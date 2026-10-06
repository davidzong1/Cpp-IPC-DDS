# N12 故障矩阵与覆盖边界

本记录对应 N12，所有测试串行执行。当前环境只有一台主机；两个端点使用独立用户、挂载和 IPC 命名空间，并通过 loopback UDP 通信，不能替代物理跨主机故障证据。

## 已验证

| 故障或边界 | 证据 | 结果 |
|---|---|---|
| 端点 cap 耗尽 | `validation/n12-dedicated-20261006-012000/cap256-status.json` | 默认 cap=256 时第 257 个 dedicated 注册收到 `SocketCap`；回收后数据端点回到 0 |
| 缓冲/FD 承受 1000 dedicated | `validation/n12-dedicated-20261006-012000/dedicated1000-status.json` | cap=1024、缓冲预算 2 GiB 时 1000/1000 Ready；注销、重新注册和再次注销均完成 |
| 100 活跃话题 pooled | `validation/n12-active-100-20261007-002200/pooled.json` | 100 RouteKey、4 数据 socket、4 worker；中途额外话题 `4→4` socket 回收；内容无非法包/重复 |
| 100 活跃话题 per-topic | `validation/n12-active-100-20261007-002200/per-topic.json` | 100 RouteKey、100 数据 socket、4 worker；中途额外话题 `101→100` socket 回收；内容无非法包/重复 |
| 网关/peer 重启 | `test_shared_net_restart` | 通过；新网关身份与 epoch 生效，旧句柄保持本地语义 |
| 丢首包、丢分片、丢 ACK | `test_shared_net_reliable` | 通过；重传后只提交一次，错误 ACK 身份/端口/epoch 不完成事务 |
| 乱序、重复、错误 CRC、错误端口/epoch/shard | `test_shared_net_reassembly`, `test_shared_net_wire`, `test_shared_net_dedup` | 通过；错误包在分配重组资源前拒绝，重复不重复提交 |
| 配额耗尽与回滚 | `test_shared_net_quota`, `test_shared_net_failure` | 通过；全局/peer/route/receipt 预算释放，ACK 不早于真实 SHM 提交 |

## 未验证或保留失败

- 未进行物理跨主机网卡、交换机或真实网络丢包/乱序/重复注入；不能据此给出跨机吞吐和尾延迟结论。
- 100 话题 64B、每端 10 个 100 Hz 热话题和 90 个 5 Hz 冷话题的两种模式均保留了短收样本。四线程排空后，热话题接收约 98.4%～98.8%，冷话题约 66.6%～69.3%；两模式接近。网关无 `invalid_packets`、CRC、`wrong_shard` 或 `send_error`，所以该轮只能说明当前总负载下存在接收事实缺口，不能宣称 per-topic 有收益或无收益。
- N11 已知的 1 MiB BestEffort 重组超时仍保留在 `validation/n11-20261006-204455/`，本节点未改写为通过。

## 资源摘要

100 话题运行的每个网关在负载前后均为 9 个线程、应用 UDP 为 0。pooled 为 4 个数据 socket、6 个 UDP socket、FD 131、RSS 约 15.9→17.5 MiB，实际收发缓冲各 3 MiB；per-topic 为 100 个数据 socket、102 个 UDP socket、FD 227、RSS 约 16.1→17.6 MiB，实际收发缓冲各约 51 MiB。完整状态、逐话题发送/接收和失败事件见上述 JSON 原始文件。
