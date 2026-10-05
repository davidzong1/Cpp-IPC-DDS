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
- 没有收到 ACK（acked_count=0）也不代表远端没有交付；DATA 已发送后失败仍可能标记 possible_remote_delivery。发布者注销会在屏障确认前取消其未完成网络事务，已发包仍不承诺撤回。
- 新订阅者在 SUB_READY 后开始参与本机分发，可能收到该路由代次下已经在途但尚未提交的消息；最后一个 Ready 订阅者退出后重建会产生新 route epoch，旧代次分片不能进入新订阅。
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

应用进程可用 `ClientRuntime::diagnostics_json()` 查看本机/网络提交结果、部分提交、信用等待和出站容量。这些值不能从网关入站提交计数推算。计数快照不是跨线程事务快照，采样期间可能相差一个在途更新。细分指标使用 `status --metrics counters|quota|latency|shards` 分页查询，不能与 `--topic` 合并。单话题详情包含 `metrics`，其生命周期跟随已登记的路由；不会根据未核验报文创建统计表。

延迟字段是固定 64 个 log2 纳秒桶的统计：`count/sum_ns/max_ns` 与 `p50_upper_ns/p95_upper_ns/p99_upper_ns`。分位数是桶上界；`count=0` 表示没有采样。精确端到端分位数另由 benchmark 原始 CSV 计算。`quota` 给出实际预留峰值与拒绝次数，当前占用仍查汇总；一个申请同时违反多个配额时各拒绝计数均递增。计数是进程累计值，不能直接比较运行时长不同的两个进程。

| 阶段 | 采样范围 |
|---|---|
| encode | 应用执行编码/预构造段校验复制；纯本机 prebuilt 没有编码，count 为0 |
| local_commit | 应用实际调用 MPMC publish 的耗时；loan/复制耗时包含在 api_return 中 |
| credit_wait | 应用实际阻塞等待出站资源，每次等待一个样本 |
| outbox_submit | 单次尝试写出站SHM，排除信用阻塞；含失败尝试 |
| gateway_queue_wait | 网关取出并复制记录后，到所属 shard prepare；不是应用 enqueue→网关 pull |
| network_first_send | 从上述网关取出时刻到首个 DATA 被内核接受 |
| remote_commit | 接收网关一次业务SHM提交尝试，含失败重试 |
| ack_wait | 源网关首发到可靠终结；应用侧为等待 SEND_RESULT 的时段，可能以超时/拒绝终结，须结合可靠结果计数 |
| api_return | 通过基本调用校验后的发布处理到返回，含编码、信用和可靠等待 |

每话题 `queue_wait` 与 `gateway_queue_wait` 同口径；tx_bytes 是实际 DATA wire 字节（含重传），rx_bytes 是首次接纳的分片 payload 字节，commits 是确定提交次数。源端本机直达不经过网关，因此该路由网关计数可以为0。shard的wakeups统计poll返回次数（含超时与立即就绪轮转），不是内核调度唤醒事件；上下文切换另查采样线程的/proc状态。`shm_committed_bytes` 仅累计确定提交的 payload，encode_copy_bytes表示编码出口产出字节，未穷举用户编码器内部复制或写零；local/outbox/reassembly_copy_bytes按已接线的payload复制位置累计。remote_target_copies计接管时冻结的逻辑远端副本数，实际发送量查tx_bytes。所有计时仅使用各自进程的 steady_clock，跨物理主机的时钟不可相减。

当前没有 DZTX enqueue 时间字段；完整应用出站排队、网络单程时间和跨主机时钟误差仍不可从这些直方图推算。协议头未挪用保留字段。

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
| Cancelled | 发布者注销取消尚未完成的网络事务；仍需检查可能部分交付 |

## 回滚

1. 正常停止需要切换的应用，让持有的 Sample 与发送事务结束。
2. 停止该实例网关（SIGTERM），保留日志和状态证据。
3. 新应用进程设置 `DZIPC_NET_BACKEND=legacy` 或删除该变量；双方网络进程采用相同协议模式。
4. 仅在确认本实例已经退出后处理其控制目录和已确认不再使用的独占出站段。无需全局清理 SHM，不使用 `rm /dev/shm/*`、`killall` 或 dzipc_list 全局 reset。

正确性和性能验收参见 [执行计划](shared_network_endpoint_execution_plan.md) 与其证据表；单机命名空间验证不能替代两台物理主机的最终验收。

当前验收结论见[等待/指标修复实测](shared_network_endpoint_evidence/20261005-optimization/results.md)和[CRC固定负载实测](shared_network_endpoint_evidence/20261005-crc/results.md)。同shard冷消息可靠完成p99中位数从4.988ms降至1.057ms；已测纯本机版本仍有13/27逐轮配对未达到门槛，CRC版本没有重复该本机矩阵。物理跨机、完整阶段指标与若干负载矩阵仍缺项，维持实验性显式启用。旧[T14](shared_network_endpoint_evidence/20261005-t14/results.md)和[T15](shared_network_endpoint_evidence/20261005-t15/results.md)记录保留。legacy 的 blocking 固定采用 TLV，应使用对象读取接口；shared_v1 的 ACK 则在目标 SHM 确定提交后发送，不将两种返回语义等同。
