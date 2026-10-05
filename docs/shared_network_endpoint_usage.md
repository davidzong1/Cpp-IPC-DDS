# 共享网络网关使用说明

`shared_v1` 是显式启用的 Linux 64 位后端。默认仍为 legacy；本机采用 MPMC 直达，跨主机经本机 SHM 出站和网关 UDP 单播。网关 UDP 端点固定为 `K+2`，应用不创建共享网络 UDP socket。多个远端主机各接收一份网络流量。

## 构建与安装

```bash
cmake -S . -B build-shared-net -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDZIPC_BUILD_SHARED_NET=ON -DLIBIPC_BUILD_PYTHON=OFF
cmake --build build-shared-net --target ipc dzipc_gateway dzipc_list dzipc_topic_cat --parallel 4
cmake --install build-shared-net --prefix /tmp/dzipc-shared-install
```

Linux 工具使用相对于自身的安装库路径。每台主机只运行一个网关，控制目录须由当前用户拥有且不可由其他用户写入。默认 K=4，数据端口 24000～24003、控制端口 24004、发现端口 24005。网关配置固定到进程生命周期，改变 K 需要重启。

```bash
mkdir -p /tmp/dzipc-gateway-$UID
chmod 700 /tmp/dzipc-gateway-$UID
/tmp/dzipc-shared-install/bin/dzipc_gateway check-config \
  --control /tmp/dzipc-gateway-$UID/control.sock --listen-ip 127.0.0.1 --interface lo
/tmp/dzipc-shared-install/bin/dzipc_gateway serve \
  --control /tmp/dzipc-gateway-$UID/control.sock --listen-ip 127.0.0.1 --interface lo
```

以上是本机示例。实际跨机部署将 IPv4 和网卡替换为本机地址，确保两端发现组 `239.255.250.251:24005` 可达，以及数据/控制端口双向可达。`check-config` 只做配置和网卡检查；`serve` 实际绑定端口才检查占用。

应用启动前设置：

```bash
export DZIPC_NET_BACKEND=shared_v1
export DZIPC_SHM_MPMC=1
export DZIPC_GATEWAY_CONTROL=/tmp/dzipc-gateway-$UID/control.sock
```

既有 C++ / Python `IPC_SOCKET` 公共工厂将选择共享后端。配置在进程中首次使用时固定；改变环境后须启动新应用进程。`IPC_SHM`、`IPC_SOCKET_ONLY`、服务请求响应仍按原路径工作。shared_v1 与 legacy 网络协议不互通。每对象独立 QoS/绑核要求暂不支持，传入非默认值明确失败；网关参数统一配置。

## 返回值与生命周期

- 发布先尝试本机，再接管网络；源网关不回注源主机。网络离线或额度不足不会撤销本机已提交的消息。
- best-effort/prebuilt 任一腿已提交或提交未知，返回 true，禁止据此回退重发。只有两腿都明确未提交，prebuilt 才能安全返回 false 后回退。
- blocking 的 ACK 表示远端网关已提交业务 SHM，不保证订阅回调执行或处理完成。任一要求的目标失败则返回 false，即使另一腿已经交付；不要无条件重试整个业务事件。
- blocking 超时单位毫秒，范围 1～5000；0 无提交，超范围明确失败。信用等待、编码与发送沿用同一截止时间。超时不能撤销已发 DATA，允许晚交付。
- 出站 loan 容量和网络缓存信用分别记账。TX_PROGRESS 只归还出站容量，可靠终结后才释放网络缓存额度。
- SIGSTOP 不允许第二网关接管。SIGKILL/重启后，已初始化旧对象可继续本机收发，旧网络身份失效；新建对象才能获得新网络会话。失败的 reset/InitChannel 会封闭整个重置句柄。
- Sample 在其持有期间保持借样合法。公开对象析构前调用方必须停止发起新的成员调用；已进入的 getter 会被唤醒并收敛。

## 查询和读取

```bash
/tmp/dzipc-shared-install/bin/dzipc_gateway status --control "$DZIPC_GATEWAY_CONTROL" --json
/tmp/dzipc-shared-install/bin/dzipc_gateway status --control "$DZIPC_GATEWAY_CONTROL" \
  --topic camera --domain 0 --msg-id 71 --json
/tmp/dzipc-shared-install/bin/dzipc_list -t camera -v
/tmp/dzipc-shared-install/bin/dzipc_topic_cat -t camera -s false -m 71 --domain 0 --once --timeout-ms 5000
```

加 `--peer-id`（32 位十六进制 gateway_id）查询指定远端的该路由，输出已安装目录版本、完整话题名、角色、目标 route epoch、`source_verified` 和 `reachable`。`ready=false` 表示不能据此放行发送。远端未安装的完整来源描述也不能被猜测为已核验。查询状态不登记业务订阅。

`topic_cat` 在 shared_v1 下通过公共 Subscriber 注册，能触发远端订阅发现；`--once` 读取后退出。TLV 显示字段，未知 schema 的 DZFlat 输出完整 `segment_hex`、字节数和 CRC，可交给 Python `dzipc.dzflat` 按 schema 解析。显式 `--transport shm` 观察本机 SHM；要抓 legacy socket-only 协议，应在单独工具进程中设置 `DZIPC_NET_BACKEND=legacy`。缺网关时报告 GatewayUnavailable，不转到旧哈希端口。

`dzipc_list` 中一条记录代表一个公共发布或订阅句柄，extra 显示 shared_v1 和网关 epoch。内部 bridge 不计业务发布者。池展示名称最大 127 字节，可能截断时有提示；网关查询保留完整名称和 64 位域。

汇总状态列出真实 socket 缓冲容量、活跃/历史配额、命令积压、入站提交与错误计数、重传和实际配置。`gateway_threads` 是网关专有线程数，SHM 公共线程另查 `/proc/<pid>/task`；socket 缓冲容量不是已占内存。`allocated_send_bytes` 包括未用授予与在途缓存，`send_inflight_bytes` 是其中实际在途部分。计数为 JSON uint64 数值，使用能保存整数精度的解析器；身份、epoch 和域使用十进制字符串。

应用进程可用 `ClientRuntime::diagnostics_json()` 查看本机/网络提交结果、部分提交、信用等待和出站容量。这些值不能从网关入站提交计数推算。计数快照不是跨线程事务快照，采样期间可能相差一个在途更新。延迟分位数与阶段耗时由测试驱动单独采集；当前未提供所有第 14.2 节细分错误/每话题延迟指标，交付审计保留该缺项。

## 错误与退出码

网关 CLI：0 成功；2 参数/配置错误；3 运行时不可用（目录、绑定、独占锁、会话或协议失败）。topic_cat 共享分支额外使用 4 表示单次读取超时。

| 错误 | 含义 |
|---|---|
| InvalidBackend / InvalidOption / InvalidNumber | 配置值、参数或数值无效 |
| UnsupportedPlatform / BackendNotBuilt / MpmcRequired | 平台、编译或 MPMC 前置未满足 |
| InvalidAddress / InvalidInterface / InvalidControlPath / PortConflict / InvalidLimit | 地址、网卡、目录、端口或配额配置不合法 |
| GatewayUnavailable / GatewayStopped | 网关不可连接、初始化失败或循环异常退出 |
| RegistryFull | 本机诊断池无法预留逻辑端点，初始化撤销 |
| DZLC Error 1 / 2 / 3 | 未实现请求 / 会话或重复状态不合法 / Busy 或登记拒绝；附有界原因 |
| Completed / NoSubscribers | 全部冻结网络目标确认 / 没有远端订阅（须结合本机结果） |
| Busy / TimedOut / UnsupportedTimeout | 配额或队列不足 / 原期限已过 / 不支持该超时值 |
| PeerGone / PeerRestarted / GatewayLost / Rejected | 对端失联 / 对端换代 / 本机会话丢失 / 验证或提交拒绝 |

## 回滚

1. 正常停止需要切换的应用，让持有的 Sample 与发送事务结束。
2. 停止该实例网关（SIGTERM），保留日志和状态证据。
3. 新应用进程设置 `DZIPC_NET_BACKEND=legacy` 或删除该变量；双方网络进程采用相同协议模式。
4. 仅在确认本实例已经退出后处理其控制目录和已确认不再使用的独占出站段。无需全局清理 SHM，不使用 `rm /dev/shm/*`、`killall` 或 dzipc_list 全局 reset。

正确性和性能验收参见 [执行计划](shared_network_endpoint_execution_plan.md) 与其证据表；单机命名空间验证不能替代两台物理主机的最终验收。
