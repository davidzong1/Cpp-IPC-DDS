# 共享网络网关

当前完成配置检查、协议编解码及底层 UDP IO。网关会话与业务数据面仍在后续任务卡中；
`serve`、`status` 当前返回 `NotImplemented`。默认网络后端仍为 `legacy`。

构建配置检查程序：

```bash
cmake -S . -B build-shared-net \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_DEBUG_INFO=OFF \
  -DDZIPC_BUILD_SHARED_NET=ON -DLIBIPC_BUILD_PYTHON=OFF \
  -DUPDATA_MSG_SRV_GENERATOR=OFF
cmake --build build-shared-net --target dzipc_gateway --parallel 4
build-shared-net/bin/dzipc_gateway check-config \
  --listen-ip 127.0.0.1 --interface lo \
  --control /tmp/dzipc-gateway-test/control.sock
```

实际部署需填写指定网卡上的本机 IPv4 地址，控制路径也可由 `DZIPC_GATEWAY_CONTROL` 提供。
检查程序校验地址归属、端口、路径和配额；端口占用由后续 `serve` 实际绑定时确认。

| 选项 | 默认值 |
|---|---|
| `--data-base-port` / `--data-shards` | 24000 / 4 |
| `--control-port` / `--discovery-port` | 24004 / 24005 |
| `--discovery-group` | 239.255.250.251 |
| `--io-batch-max` | 32 |
| `--io-round-packets` / `--io-round-bytes` / `--io-round-us` | 64 / 65536 / 200 |
| `--nack-delay-ms` / `--nack-interval-ms` / `--retry-initial-ms` | 2 / 2 / 2 |
| `--retry-max-ms` | 100 |
| `--control-rate` / `--peer-control-rate` / `--control-burst` | 10000 / 1000 / 64 |
| `--max-message-bytes` / `--outbox-bytes` | 16777216 / 33554432 |
| `--send-bytes` / `--send-records` | 268435456 / 4096 |
| `--session-send-bytes` / `--session-send-records` | 33554432 / 512 |

数值使用无符号十进制整数，拒绝重复和未知选项。配额首版可下调至契约允许的范围，
不能高于方案默认硬上限。完整配额、协议与任务状态见
[执行方案](../../docs/shared_network_endpoint_execution_plan.md)。

共享后端构建开关仅在 64 位 Linux 默认开启。关闭构建时，网络模式解析和失败分支仍可链接。
显式选择 `shared_v1` 时，缺少构建支持报告 `BackendNotBuilt`，缺少 MPMC 报告
`MpmcRequired`；当前数据面未完成时报告 `NotImplemented`。
