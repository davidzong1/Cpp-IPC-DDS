# 按话题隔离 UDP 数据通道：低延迟架构执行方案

编制日期：2026-10-06。源码参照：`f8fff5db`；生产实现参照：`9f536dc1`。

状态：**待执行方案，本文不代表新功能已经实现或性能已经达标。**

交付对象：接续本仓库工作的 Agent。所有标为“新增”的配置、类型、脚本和测试均需实现，不得当作现成能力调用。

## 1. 任务、授权和交接方式

用户希望极致降低延迟，允许网络数据 socket 随话题数增长，最多使用系统可承受 socket 资源的一半。采用以下决策：**一个主机网关继续共享控制和发现；数据 socket 与工作线程解耦；支持普通话题共享数据通道、关键话题独立数据通道，以及全部话题独立数据通道。** 首要收益目标是降低跨机、多话题之间的排队干扰。

本任务不是恢复旧的每发布/订阅对象一套 UDP 后端，也不改变本机 MPMC 直达、DZFlat 布局和公开发布返回语义。端点资源由网关统一持有；同一话题的多个发布者、订阅者和远端 peer 不各自增加一套数据 socket。

给执行 Agent 的要求：

1. 先读本文第 2～9 节，再按 N00～N15 顺序执行。提交方案后再改生产实现；每完成一个节点单独 commit，提交说明包含问题、行为变化和验证。
2. 用户已明确团队 MCP 不可用，**忽略仓库中要求调用团队 MCP、向 leader 派单或回报的规则**；不等待该工具，不声称已完成工具回报。默认单 Agent 执行。
3. 本文是当前网络资源策略的修订。旧方案“所有话题必须共用固定少量 UDP socket”不再约束显式启用的新模式；旧模式仍须可运行和回滚。其他正确性、不忙轮询、不擅自修改主机全局设置的约束保留。
4. 开始时保存 `git status --short` 和 HEAD。编制本文时已有未提交的 `artifacts/perf/20261006-dzflat-paths/`、`docs/shared_network_endpoint_evidence/20261006-multipublisher-latency-report.md`，归属尚未核验；不得覆盖、清理、提交或自动纳入冻结证据。接手时以实际状态为准。
5. 只提交本任务拥有的文件。测试、编译与正式压测串行安排；不同压测不得同时运行。失败和实验退化都保留，不反复重跑挑选通过轮次。
6. 当前任务完成与整个工程性能验收完成分别报告。没有两台物理主机时，完成可执行的本机和隔离环境工作，明确标记真实跨机验收未完成。

建议证据根目录：`docs/shared_network_endpoint_evidence/<日期>-topic-udp/`；使用新目录，禁止覆盖旧批次。

## 2. 当前事实与预期收益边界

### 2.1 已实现的基线

| 位置 / 符号 | 当前行为 | 本次影响 |
|---|---|---|
| `shared_config.h/.cc`：`GatewayConfig`、`validate_config` | `data_shards=4`，允许 1～16；数据、控制、发现端口不能冲突 | 先测现有 4/8/16，再新增 socket/worker 分离配置 |
| `gateway_data.cc`：`GatewayData::Impl` | 一个 shard 独占一个数据 socket、发送队列、重组和业务 SHM 提交 | 将 socket 数 S 与数据工作线程数 W 分离 |
| `datagram_endpoint.h`：`DatagramEndpoint`、`DatagramLoop` | 单 IO owner；已有令牌和先摘除后关闭的契约 | 优先复用，不另造不受生命周期保护的 FD 表 |
| `wire_protocol.h/.cc` | DZMX v1 头 160B，分片 1024B，UDP 载荷最大 1184B；HELLO 公告 base/K | 按话题端口需要显式新协议和目录绑定 |
| `peer_directory.cc`、`reassembly.*` | 完整目录快照、source PUB 验证、target SUB/route epoch 验证、去重和配额 | 端口和端点代次必须进入同一验证边界 |
| `outbox_service.cc`：`OutboxService` | 一个初始化线程、一个 drain 线程；按 session 读取出站 SHM | socket 增多不能自动提高这里的处理能力 |
| `gateway_runtime.cc`：`process_outboxes` | 每条 Record 还经过控制循环校验身份/序号、建立可靠账本，再 `data->submit` | **不能把当前路径描述为 drain 直接送入数据线程**；必须量出这一段 |
| `publisher_endpoint.cc` | 路由已同步且无远端的普通发布直接提交本机 SHM；可靠发布可等待信用和远端确认 | 不把本机耗时或可靠等待都归因于 UDP 数量 |
| `client_runtime.cc` | 同进程共享会话、出站通道及部分锁/账本 | 同一进程多个话题仍可能在出站侧互相影响 |

现有关键文件均位于 `include/dzIPC/net/`、`src/dzIPC/net/`；按符号定位，不依赖本文行号。

### 2.2 证据与不能外推的结论

- [最新完整本机矩阵](shared_network_endpoint_evidence/20261006-execution/results.md)：54 窗口、24/27 逐轮通过、8/9 中位数通过；仍有 1/4KiB、1/1MiB、32/64B 的第 1 轮失败。162000 次发布、2214000 次接收正常。**这些 UDP 不参与本机数据转发的失败不能靠“增加 UDP socket”宣称关闭。**
- [固定负载冷热测试](shared_network_endpoint_evidence/20261005-crc/results.md)：优化后的冷消息可靠完成 p99 三轮中位数，同 shard 为 1.057ms、不同 shard 为 0.720ms。支持隔离处理的方向，但同时改变了 socket 和 worker 归属，不能声称只增加 socket 就快 32%。这是同机隔离环境的真实 UDP，不是物理跨机结论。
- [1000 话题注册证据](shared_network_endpoint_evidence/20261005-t14/scale-1000.json)只证明注册/资源规模，不能当作 1000 个活跃话题的吞吐或延迟测试。
- 多发布者轮流发送能测平均 API 开销，不能测并发争用；并发测试必须保存调用区间并证明真实重叠。54 窗口原矩阵不是多发布者性能矩阵。

预期：多话题冷热混跑的尾延迟最有希望改善；一个话题的全部发布者仍共享一个话题通道，低负载单话题和纯本机路径不预设收益。

## 3. 最终模式与配置契约

### 3.1 三种数据模式

| 新增模式 | 数据 socket 数 S | 资源不足时行为 |
|---|---|---|
| `pooled` | 固定 S0；按 RouteKey 分配共享 socket | 新增 socket 失败则启动失败，不带缺失端点运行 |
| `hybrid` | S0 + 独立话题数 D | 普通话题留在池中；明确要求独立的话题注册失败，不静默取消隔离 |
| `per-topic` | 已网络就绪的不同 RouteKey 数 T | 新话题注册明确失败，已就绪话题继续服务 |

每个数据 socket 同时用于该绑定的收发，可面向多个 peer；总 UDP 数为 `S + 2`，额外两个为控制和发现。T/D 计数包含仅 PUB、仅 SUB、PUB+SUB 话题；最后一个本地角色注销后才可回收专用端点。话题在存续期间保持端口及 owner 不变，不根据瞬时流量自动迁移。

新模式继续由 `DZIPC_NET_BACKEND=shared_v1` 选中网关后端。**应用不创建按话题 UDP socket。** 网关仍按主机单实例运行，不通过多个同身份网关分摊流量。

### 3.2 新增配置（拟定默认值，N03 冻结）

| 配置 | 默认 / 约束 |
|---|---|
| `--network-version` | `1`；新目录绑定模式显式使用 `2`，同一网关本轮只运行一种网络版本 |
| `--data-mode` | `pooled`；v1 只允许 pooled，v2 允许三种模式 |
| `--data-sockets` | `4`；共享池 socket 数，pooled/hybrid 为 1～16；per-topic 不创建共享池 |
| `--data-workers` | `4`；W 与 S 独立，至少 1，不超过本次部署明确分配的 CPU 数 |
| `--data-socket-cap` | `256`；包含共享、专用、正在创建和正在关闭的全部数据端点 |
| `--data-port-range` | `20000:49999`；显式 IPv4 本地地址上的候选端口范围，排除控制/发现和已占用端口 |
| `--socket-fd-fraction` | `0.5`；允许更小，不允许大于 0.5 |
| `--socket-buffer-budget-bytes` | `268435456`；所有网关 UDP socket 的收发缓冲上限总预算，独立于重组/出站预算 |
| `--data-rcvbuf-bytes` / `--data-sndbuf-bytes` | 各 `262144`，只设置本进程数据 socket；读取内核实际返回值后记账 |
| `--topic-policy-file` | 可选 JSON，按完整 topic/domain/msg_id 指定 pooled/dedicated、worker 和缓冲覆盖值 |

旧 `--data-shards K` 保留为 pooled 的 `S=K,W=K` 兼容简写；与新 S/W 参数同时给出时拒绝，不猜测优先级。旧 v1 默认值和既有 CLI 继续有效。v2 per-topic 模式明确拒绝无意义的池参数；帮助信息、配置输出和错误码必须一致。

v1 沿用 `--data-base-port` 的连续 S 个端口，HELLO 的 K 表示 S，不能改成 W；显式传入仅适用于 v2 的 `--data-port-range` 时拒绝。v2 使用端口分配器，显式传入 `--data-base-port` 时拒绝。未显式提供的旧默认参数不构成冲突。v1 同样受 FD、缓冲和数据 socket 总额度约束；其端口额度按已声明可用的本地 UDP 端口空间审计，不把长度为 S 的连续绑定块当作全部可用空间后再减半。

策略文件在启动时加载；动态话题按已冻结策略注册。第一版不支持在线修改端点模式/worker、迁移活跃话题或运行中切换协议。`dedicated_socket` 与 `exclusive_worker` 分开表达：多个专用 socket 可以共用 worker，不能对外冒称独占 CPU。

示例策略结构（**新增格式，不能直接交给当前程序**）：

```json
{
  "default": {"endpoint": "pooled"},
  "routes": [
    {"topic": "control/status", "domain": "0", "msg_id": 71,
     "endpoint": "dedicated", "worker": 0, "exclusive_worker": true},
    {"topic": "camera/front", "domain": "0", "msg_id": 72,
     "endpoint": "dedicated", "worker": 1, "exclusive_worker": false}
  ]
}
```

domain 以十进制字符串解析为 uint64；重复 RouteKey、越界 worker、普通话题进入独占 worker、独占 worker 被不同话题重复要求均为配置错误。未指定 worker 的普通端点仅在非独占 worker 中分配；没有可用普通 worker 时拒绝该配置。

pooled 模式禁止 dedicated 策略；hybrid 的 default 固定为 pooled；per-topic 的 default 固定为 dedicated。省略策略文件时由模式确定默认值。第一版仅 dedicated 条目允许指定 worker、缓冲覆盖值和 exclusive_worker，避免同一个池 socket 接受互相冲突的话题级配置。exclusive_worker 仅指该数据 worker 不服务其他 RouteKey，CPU 亲和性仍单独配置和审计，不隐含操作系统隔离 CPU。

策略内缓冲覆盖字段为 `data_rcvbuf_bytes`、`data_sndbuf_bytes`，单位为字节，仍受第 4 节总预算约束。所有策略字段严格校验，未知字段拒绝，避免拼错配置后悄悄使用默认值。

## 4. “最多一半资源”的可执行定义

Linux 没有一个能代表可用 socket 数量的统一参数。编制时当前会话 `RLIMIT_NOFILE` 为 1048576，`fs.file-max` 为接近整数上限的值；两者都不能推导出可有效运行几十万个数据 socket。执行时重新采集网关实际继承的限制，不复用这些数值。

启动审计保存：进程 FD 软/硬限制、`fs.nr_open/file-max/file-nr`、`/proc/net/sockstat`、可用内存及 cgroup 限额、CPU 允许集合、网卡队列数和当前 RSS/IRQ 配置（只读）。UDP 独占端口按每个本地 IP/网络命名空间预算，不能拿系统 FD 数当作可用端口数。

数据端点有效上限同时满足：

```text
S_effective <= data_socket_cap
S_effective + F_nondata_peak + F_reserve <= floor(RLIMIT_NOFILE.soft * socket_fd_fraction)
S_effective <= floor(排除保留端口后的候选端口数量 * 0.5)
Σ(每个网关UDP socket实际SO_RCVBUF + SO_SNDBUF) <= socket_buffer_budget_bytes
```

`F_nondata_peak` 包含控制/发现 UDP、Unix 监听/会话、epoll/eventfd、出站/初始化等其他 FD 的预算峰值；默认保留额 `F_reserve>=64`。所有变量用溢出检查后的无符号计算；进程启动或后续会话扩张无法保留这些额度时明确拒绝，不能侵占为已就绪端点预留的资源。系统级剩余 FD 仅作为额外保守检查，不能把巨大 `file-max` 当作实际容量保证。

创建前统一预留 FD/端口/缓冲额度，创建中与 Closing 中也占预算；bind 失败、owner 注册失败、目录安装失败必须完整回滚。实际内核缓冲大小可能倍增或被限制，按 `getsockopt` 返回值核对并输出；不能把设置值当成实值，也不能把缓冲上限求和写成已占物理内存。内核元数据、重组、缓存和 SHM 另外记账并采集实际内存峰值。

不得通过 `SO_REUSEPORT` 让多个逻辑独占话题意外共用端口；发现 socket 的既有复用行为单独保留。不通过关闭别的进程、修改系统 sysctl、全局电源策略、网卡 RSS 或无限扩大内核缓冲获得成绩。扩大本进程配置预算时先记录资源依据，再按本方案执行，无须为已经授权的普通实现步骤反复询问用户。

## 5. 数据 owner、等待与公平性

1. 一个 socket 在整个存活期只能由一个数据 worker 收发；同一 RouteKey 的发送事务、重组、去重和 SHM 提交由其 owner 串行管理。不能让多个线程同时调用同一重组对象。
2. pooled 模式先算 `socket_index = route_hash(RouteKey) % S0`，再查冻结的 `socket→worker` 表；**S 与 W 不相等时不能继续用 `hash % W` 校验收包端口**。v2 则查目录和绑定，不再从端口推断 worker。
3. 每个 worker 使用有界就绪队列和可阻塞的多 FD 等待，优先复用 `DatagramLoop`；大规模端点采用 epoll/等价现有等待后端，避免每轮扫描所有空闲 socket。注册 token 单调递增，不能只凭 FD 数字标识端点。
4. 按 socket 和 RouteKey 轮转，分别设置包数、字节数、时间预算；一次大消息不能连续占满全部分片发送。所有工作队列有上限，预算耗尽的已就绪对象进入 deferred 队列，不能丢失“已有数据但没有下一次通知”的进展。
5. 保留非阻塞批量 IO；不为了凑批等待，不引入忙轮询。EAGAIN 时等待可写/既有有界重试条件，不能无限零超时空转。低流量话题不会被批量阈值拖延。
6. 注册/注销命令通过 owner 执行和确认；控制线程不能持目录锁等待 worker 回执，worker 不能同步回调控制线程形成锁环。关闭 barrier 覆盖已开始的 IO 和提交。
7. 数据工作线程 W、出站 drain 数 R、控制/初始化线程数均独立输出；实际线程还包含 SHM 调度线程，不能只用 `W+3` 代替 `/proc` 实测。
8. 默认保留 R=1。是否分片出站或绕开每消息控制循环，必须按第 8 节取证后实施；增加数据 socket 不能隐藏这部分成本。

未显式指定 worker 的 dedicated 端点，使用 `route_hash(RouteKey) % 普通worker数` 索引启动时冻结的非独占 worker 列表；池 socket 按 socket_index 对该列表取模。相同 RouteKey 注销再注册仍回到原 owner；不做基于瞬时负载的重新分配。退役去重/回执历史由该 owner 按既有留存规则持有，不随 FD 对象销毁；等待集合和空闲 FD 可及时回收，历史内存另外计入有界配额。

### 5.1 必须可读回的状态与计数

扩展现有网关 `status` 输出，提供机器可读的端点和 worker 视图；保留旧 `shards` 字段的兼容含义或明确新增字段，不把 worker 数悄悄改名为 shard 数。至少包括：

- 网关版本/模式、配置 S0/W、实际 data sockets、控制/发现 sockets、Creating/Ready/Closing、端点分配失败原因和高水位。
- 每个 RouteKey 的 data_port、endpoint_epoch、pooled/dedicated、owner、PUB/SUB 引用数和 ready 状态；每 worker 的端点数、队列深度/峰值、唤醒、预算让出和 IO 包/字节数。
- FD 软限制、非数据 FD 预算、保留额、实际 FD 数、候选端口总数/预留数、实际 SO_RCVBUF/SO_SNDBUF 求和及剩余预算；统计口径不得混用 FD 与 UDP 数。
- drain 和控制事件队列深度、处理量、失败量；可靠 pending、重组和退役历史占用；错误 epoch/port/token、目录拒绝和重复包计数。

资源失败至少区分 `socket_cap`、`fd_budget`、`port_budget`、`buffer_budget`、`bind_failed`、`owner_failed`。这些是新增诊断原因，公开调用返回映射沿用已有失败语义，不能未经兼容处理新增本地协议枚举。诊断导出不能持全局锁扫描全部话题阻塞数据热路径；完整映射按需读取，性能窗口采用低频快照并保持对照一致。

## 6. 必须保留的不变量

| 编号 | 契约 |
|---|---|
| I01 | 本机发布由应用直接提交 MPMC；源网关不向源主机重复注入 |
| I02 | DZFlat/TLV 有效字节、schema、Sample 生命周期和原 SHM 布局不变；不以少复制或少校验提速 |
| I03 | 话题身份始终是完整 RouteKey；端口号、哈希桶和 worker 编号不能替代它 |
| I04 | 一次已接管消息只能提交一次；失败/未知状态不能被上层自动重发整个业务事件 |
| I05 | 普通发布、prebuilt、可靠阻塞发布保持既有返回和 deadline 语义；ACK 仍表示远端业务 SHM 已提交 |
| I06 | 注册就绪前不公告可收发端点；失效端点不得再接纳新的事务 |
| I07 | 映射、FD、目录快照、重组及回执均被有效 lease/owner 保活；先摘等待再关 FD |
| I08 | 新代次不能接受复用端口上的旧消息；注销不能清掉仍须保留的去重历史来制造重复提交 |
| I09 | 同一共享 socket 上的话题退出，不影响其余话题；多发布者退出一个，不释放整个话题端点 |
| I10 | FD、端口、缓冲、发送信用、出站容量、重组和控制队列分别有界；回收计数恰好一次 |
| I11 | v1/v2 明确区分，不静默改协议、不同时向两个版本重发同一消息 |
| I12 | 资源不足拒绝新增工作，已接管事务仍须终结；“只注册成功、实际没有端点”不是可用状态 |
| I13 | 三类指标分开：发布调用、网络接管/可靠完成、端到端接收；平均值不是 p50 |
| I14 | 所有性能结论携带源码/二进制/配置、原始样本与交付证据；不覆盖历史失败 |

## 7. 网络 v2、目录绑定和端点生命周期

### 7.1 版本边界

保留网络 v1 的编码、解码和 golden vectors，默认继续运行 v1 pooled。新端点目录采用显式网络 v2；v2 pooled 也使用新目录，作为排除协议变化影响的对照。一次网关运行只发所配置版本，发现不同版本的 peer 时输出 `incompatible_version`，不安装可用路由、不静默降级，也不向 v1/v2 双发业务消息。能够解码两个版本不等于允许混合版本互通。

本地 DZLC/DZTX 现有 v2 与本节“网络 v2”是不同版本域。保持本地报文、注册描述和 SHM 布局；应用不携带动态 UDP 地址。若实现发现必须改变本地协议，先补兼容方案并单独提交设计，不能顺手复用网络版本号。

### 7.2 DZMX v2 数据及可靠反馈

所有多字节整数仍按现有 `byte_codec.h` 的**大端**编码，不直接序列化 C++ 结构。偏移从报文首字节 0 开始。v2 头长 176B，分片载荷仍最多 1024B，最大 UDP 载荷 1200B；以下是完整头布局。

| 偏移 | 字节 | 字段 / 约束 |
|---|---:|---|
| 0 | 4 | `DZMX` |
| 4 | 1 | 网络版本：2 |
| 5 | 1 | kind，保留 Data/Ack/Nack/Reject 的值 |
| 6 | 2 | 保留，必须 0 |
| 8 | 2 | header_size：176 |
| 10 | 2 | payload_size：实际报文长度减 176 |
| 12 | 28 | 原 scope 的字节 4～31，解码时补 `DZS2` |
| 40 | 16 | source_id |
| 56 | 8 | source_epoch，网关代次 |
| 64 | 16 | publisher_id |
| 80 | 8 | sequence |
| 88 | 16 | target_id |
| 104 | 8 | target_epoch，网关代次 |
| 112 | 4 | message_size |
| 116 | 4 | fragment_index |
| 120 | 4 | fragment_count |
| 124 | 4 | message_crc |
| 128 | 4 | route.msg_id |
| 132 | 4 | schema_hash |
| 136 | 8 | receiver_route_epoch |
| 144 | 4 | packet_crc；此字段按 0 计算，覆盖整个头和载荷 |
| 148 | 1 | encoding，TLV / DZFlat |
| 149 | 1 | delivery，BestEffort / Reliable |
| 150 | 10 | 保留，必须 0 |
| 160 | 8 | 新增 `data_source_endpoint_epoch`，非 0 |
| 168 | 8 | 新增 `data_target_endpoint_epoch`，非 0 |
| 176 | 0～1024 | 载荷；沿用各 kind 的载荷合法性规则 |

ACK/NACK/REJECT 继续经共享控制 socket 发送，网关 source/target 身份及网关 epoch 按原有规则交换。**两个新增 endpoint_epoch 按原始 DATA 方向原样回显，不交换**，用于关联原事务的发送数据端点与接收数据端点，不代表 ACK 使用的控制 socket。反馈匹配必须同时比较这些字段和既有消息身份；不能仅凭 request_id 或序号匹配。可靠 ACK 仍在远端业务 SHM 提交后生成。

新增版本字段或显式版本 codec 入口，避免一个全局常量让 v1 头长被动变成 176。逐处核对 `ReceivedDatagram::bytes` 的 1184B 容量、`decode_packet` 长度校验、批量 IO 数组、`gateway_data.cc` 中 1184 的字节预算、可靠重传和缓存计费；按实际版本计算长度和配额。截断包必须拒绝，不得把缓冲扩容误当作允许 v1 超长包。1200B 不代表任何网络路径都免分片，部署记录 MTU；本轮不引入新的路径 MTU 协议。

### 7.3 HELLO、catalog 和目录 v2

| 报文 | v2 变化 | 保持不变 |
|---|---|---|
| DZGD HELLO | version@4=2；data_base_port@32 与 data_shards@34 均为 0；capabilities@38 为 3：bit0 表示目录端点，bit1 表示报文 endpoint_epoch；其他能力位当前拒绝 | 总长 64、kind@5=1、header_size@6=64、gateway_id@8、gateway_epoch@24、control_port@36、snapshot_version@40、max_message_bytes@48、CRC@52、56～63 保留 0 |
| DZGC catalog | version@4=2，目录体使用下述 v2 条目 | 84B 头及全部其余偏移、CRC@80、每页体最多 1024B、整快照 body_crc、页数和字节配额 |
| gateway directory | 条目固定区从 52B 扩展为 64B | 起始 4B 条目数；最多 4096 个条目；按完整 RouteKey 严格排序；目录总量上限及完整快照原子安装 |

v2 gateway directory 条目：

| 偏移 | 字节 | 字段 |
|---|---:|---|
| 0 | 32 | 完整 scope |
| 32 | 4 | msg_id |
| 36 | 4 | schema_hash |
| 40 | 8 | receiver_route_epoch：含 SUB 时非 0，仅 PUB 时为 0 |
| 48 | 2 | topic 字节长度，仍不超过 1024 |
| 50 | 2 | role_flags，PUB=1、SUB=2、PUB+SUB=3 |
| 52 | 2 | data_port，非 0，不与该 peer 控制端口冲突 |
| 54 | 2 | endpoint_flags：bit0=dedicated，其余位为 0 |
| 56 | 8 | endpoint_epoch，非 0 |
| 64 | topic 长度 | topic，沿用原身份和字符串校验 |

固定区原来的 0～51 偏移保持。v2 条目最大 1088B，可以跨 catalog 页边界；先按页协议组装完整目录再解析条目，不能要求每页恰好包含整数个条目。本地注册 `encode_descriptor(..., registration=true)` 仍使用 52B 固定区，建议另建网关目录端点类型，禁止通过扩大公共 descriptor 编码破坏 DZLC。

同一 peer 的多个 pooled RouteKey 可以使用同一 `(port, endpoint_epoch)`；共享同一端口时 epoch 和 pooled 标志必须一致。dedicated 端口不得出现在其他 RouteKey 上，同一 endpoint_epoch 不得指向不同端口。目录重复 key、非法共用、未知标志、零代次、类型冲突及超配额均拒绝整份候选快照，已安装快照保持有效。安装比较必须纳入端口、代次和 flags；更新 `PeerDirectory` 的相等判断与 admission 失效逻辑。

### 7.4 收发验证和冻结事务

接收 DATA 必须在申请重组内存前核对：

1. 本网关运行版本、长度、保留位、CRC、scope/schema、消息及分片边界。
2. 已安装 peer 身份、网关 epoch、来源 IP、已安装 PUB RouteKey；源 UDP 端口及 `data_source_endpoint_epoch` 与该 PUB 目录一致。
3. 收包 endpoint token / lease 确实是当前 owner 的有效绑定；target_id/网关 epoch 正确；`data_target_endpoint_epoch` 与该端点一致。
4. 本地 RouteKey 存在有效 SUB，绑定指向该收包端点，receiver_route_epoch 正确；共享池同端口上的错误话题也不能借此绕过路由校验。

v1 仍用对端 base/S 算源端口、本端 base/S 校验目标 socket；v2 完全由已安装目录和本地绑定校验，不能再要求 v2 HELLO 的 K 非 0。校验入口不可只改 `GatewayData::incoming`，还需覆盖 `ReassemblyShard`、可靠发送 destination 和控制反馈匹配。

一次发送接管时冻结本地 publisher/admission、源端点 lease、每个远端目标的 peer/目录/route admission、目标端口及 endpoint_epoch。首次发送、后续分片及重传用同一冻结目标；中途端点失效按现有 Cancelled/失败/可能已投递语义终结，不把旧事务改投新端口。反馈先验 control 来源 IP/端口、网关 epoch，再交原 owner 匹配事务；迟到反馈不能命中新事务。

去重身份保留原 publisher/sequence/peer 网关代次和原消息一致性检查，新端点字段加入报文一致性判断；**不能单靠换 endpoint_epoch 重置去重来接受同一业务消息第二次**。历史留存时间和配额不缩短。注销后尚需保留的历史可使用有界退役状态，耗尽时拒绝新工作，不能清空历史换取成功注册。

### 7.5 端点与话题绑定生命周期

新增内部对象至少区分：`DataEndpoint`（FD、token、port、epoch、owner、预算 lease）、`RouteDataBinding`（RouteKey、模式、端点 lease、PUB/SUB 引用）、已冻结的 `TxTarget`。它们可以采用其他命名，但所有权与状态必须可审查。

```text
预留额度 → 创建并 bind → owner 加入等待集合并确认
         → 本地绑定就绪 → 发布可公告目录快照 → 回复注册 ready

停止接纳 → 本地 binding / 公告目录失效 → owner barrier
         → 取消或终结在途工作 → 摘除等待并消化旧 token 事件
         → 关闭 FD → 释放端口及额度
```

“发布目录快照”表示它已成为本地可发送的完整快照，不要求所有 peer 确认安装；注册 ready 与远端发现同步是两个状态，保留原路由同步契约。注册失败回滚本次增加的本地角色、桥接和配额；不可返回成功却只剩半个可用注册。

端点 epoch 为本网关存活期间不重复的非零 uint64，递增耗尽时拒绝创建；网关重启由已有 gateway_epoch 隔离。关闭端口后可供新端点重新 bind，但必须分配新 epoch 和新 token。用“先读目录、随后用裸 FD”的方式不能替代 lease；FD 数字相同、旧 epoll 事件或旧 UDP 包均不能命中新代次。

角色引用按 RouteKey 聚合：退出一个 PUB 不影响其他 PUB；最后一个 SUB 退出时使接收 route epoch 失效，若仍有 PUB 则保留端点用于发送；PUB+SUB 全部退出后才关闭 dedicated 端点。pool socket 在网关存活期间保留，单话题退出只删除绑定。重新增加 SUB 必须遵守现有 receiver_route_epoch 递增与历史规则。

## 8. 出站与控制循环：先取证，再决定是否分片

### 8.1 实际链路和观测点

```text
应用 publish → 本机 MPMC 提交
             → 会话出站 SHM → OutboxService::drain
             → OutboxEvent::Record → 控制循环 process_outboxes
             → GatewayData::submit → 数据 worker 队列 → UDP send

远端 UDP receive → 验证 / 重组 → 业务 SHM 提交 → 应用取样
                                            → ACK 控制队列 → 源端可靠完成
```

上述是关键阶段关系，API 返回与两条路径的具体先后以当前实现和发布类型为准，不为打点改变返回时机。N01 建立基线测量，N09 在新架构上补齐同一链路，保存关联 `(gateway_epoch, session, publisher_id, sequence, RouteKey)`。诊断至少区分：应用等待出站锁/信用、出站写入与等待读取、drain 自身处理、事件排队等待控制循环、控制校验与账本、数据队列等待、首包/末包发送、接收验证/重组/SHM 提交、ACK 入队/发出及完成返回。

每段保存调用墙钟耗时；对疑似调度问题另采线程 CPU 时间和切换计数，不能把墙钟都归因于 memcpy 或锁。打点缓冲有界、记录丢弃数量，避免逐消息写文件和全局诊断锁；正式延迟验收关闭分段打点，诊断与无诊断结果分别保存。分位数不能相加，按同一消息关联样本分析慢消息中各段占比。

### 8.2 条件优化的边界

增加 UDP socket 不改变应用同进程共享出站通道，也不自动消除单个 drain、逐消息控制校验或 ACK 控制队列。如果数据 socket 排队已经缩短，但上述区间仍主导慢消息，就继续 N10；否则提交“保留现有出站架构”的证据，不为完成节点强行增加线程。

进入改造的预登记判据：在两个连续诊断轮次中，该区间在最慢 1% 消息中的累计耗时占比至少 20%，且伴随可重复的队列增长或串行处理等待；若采用其他阈值，须在 N03 冻结并说明依据。该判据用于选择改造位置，不替代第 9 节性能验收。

允许两级优化，不能直接跳过所有权设计：

- **多 drain**：R 默认为 1，按 session 固定分配给一个 drain，单个出站 reader 不被多个线程同时消费。R 的增加计入总 CPU/线程预算；初始化线程职责、session 关闭 barrier、事件有序性和信用回收不变。记录同进程热点仍受单出站 reader 限制的结果，不声称 R 能拆开一个 session。
- **减少逐消息控制循环**：先在证据目录提交独立设计，画清 publisher 序号、可靠 pending/results 账本、信用账户、route/peer 快照的唯一写 owner 和关闭流程。可将不可变 admission 快照提供给固定 session owner，由其串行验收序号、建立可靠账本并投递数据 worker，控制线程仅负责生命周期和结果传递。已有请求去重、ID 冲突拒绝、目标冻结、失败结算、重复 Record 不重发都须在新 owner 实现，不能以原子指针读目录替代整套事务边界。

如果仅移出 BestEffort 校验而可靠路径仍留在控制循环，同一 publisher 的序号验收也必须经过同一个 owner；禁止两个路径各维护一套“最后序号”。应用侧新增每话题出站 SHM 涉及本地协议/资源及 API 接管边界，**不在本轮默认实现范围**，证据证明需要后另列后续方案。

## 9. 指标、比较方法与验收门槛

### 9.1 不混淆四种耗时

| 指标 | 定义 | 注意事项 |
|---|---|---|
| 发布调用 | `publish` 进入至返回 | 普通/prebuilt/可靠阻塞分别报告；轮流发布等待自己的轮次不计入调用耗时 |
| 网关接管和发送 | 出站可读到接管、接管到首包/末包发送 | 与普通 publish 返回不等价；诊断区间在同一主机时钟域计算 |
| 可靠完成 | 源端调用或请求起点至最终结果，起点写进指标名 | 包含远端 SHM 提交及 ACK；不是“发送 syscall 延迟” |
| 端到端接收 | 消息发送起点至接收应用实际取样 | 多订阅报告每个接收者及最慢接收者，不能只报首个接收者 |

每项提供样本数、**算术平均值**、p50、p95、p99、最大值；分位数算法冻结为 nearest-rank。提供每个发布者结果和按消息加权总体结果，不平均各发布者 p99，也不把 p50 标成平均值。可靠错误、超时和未完成单列，不能从均值样本中默默丢弃。

物理两机的单向耗时不能直接相减各自 CLOCK_MONOTONIC。可靠完成在发送机测量无需跨机同步；单向端到端结论需要 PTP/等效同步、时钟转换和采样期间误差证据。无同步条件时保留接收率、完整性和源端可靠完成，不将 RTT/2 冒充单程延迟。

### 9.2 冻结公平对照

1. N02 先测原实现 K=4/8/16。原实现 K 同时改变 socket 和 worker，必须标出额外 CPU 成本；CPU 不足的配置可记为不可执行，不能在少量 CPU 上过度订阅后断言大 K 更差。
2. 新实现至少包含旧 v1 pooled、解耦后的 v1 pooled、v2 pooled、v2 hybrid、v2 per-topic。主比较采用 N02 中在部署预算内表现最好的旧配置；另保留同 W/同 CPU 预算比较，避免只挑差基线。
3. 区分两个实验：增加 socket 且保持 worker/CPU 预算不变；为关键话题保留专用 worker，但总 W/允许 CPU 不变。增加 W 或 CPU 的结果另列，不能归因于 socket 单一因素。
4. 相同消息大小、编码、发布方式、订阅者数、目的 peer 数、总发送率和 CPU 亲和性策略。冷热测试固定绝对热流速率，不按各版本峰值各取 70% 后宣称等负载。
5. 正式窗口每配置每场景 3 轮，每轮预热 10 秒、采样 60 秒；冷流 100Hz 至少获得 6000 个计划样本。采用预先冻结的轮换次序，保存全部失败；新旧采样窗口串行。样本不足判为未完成，不用短窗口补成一次成功。
6. 开始前冻结 case 清单、源码/构建选项、二进制和实际加载库 SHA256、机器/CPU/内存/MTU/时钟、端口/worker/话题策略、计划发送时间表及随机种子。结束复核二进制未变。基准说明中硬编码的 commit/flags 必须核验真实值，不能直接复制 `performance_matrix.py` 的 manifest 用于新网络基线。

### 9.3 本次新增的工程目标

以下是**拟定的优化验收目标，不是已有测量结论**。N03 随测试配置一并冻结，执行中不得为使结果通过而放宽。

- 正确性先通过：非故障、未过载场景下，计划可靠消息全部终结且成功消息接收完整，零重复、零损坏、零跨话题误投；BestEffort 在约定稳定负载下零观察丢失，故障/过载下另报告丢包和错误，不要求协议变成可靠传输。
- 预登记的关键冷热场景：每轮冷话题可靠完成 p99 相对所配对基线至少下降 20%。有合格时钟证据的物理跨机端到端 p99 同样逐轮比较；没有证据则该项未验收。
- 同场景平均值、p50，以及其他回归场景平均值/p50/p99，不超过基线加 `max(基线 × 10%, 5µs)`。所有比较在相同指标/发布类型中进行；给出绝对微秒值与比例。
- 计划发送率、实际发送率、完成率、接收率一起保存；不能减少热流实际负载来降低冷流尾延迟。未能维持计划负载的窗口判定失败或预先定义的过载实验，不计为延迟通过。
- 纯本机 54 窗口继续用原 `f066a82` 基线、原门槛、原采样口径，不套用以上新网络阈值。历史 24/27 保留；除非新的完整原矩阵逐轮通过，不能宣称原延迟问题已经全部修复。

三轮中位数、置信区间可作辅助分析，不能替代逐轮判据。若功能完成而性能未达标，保留新模式为显式实验配置、默认继续 v1，并按区间证据列出剩余原因。实现完成、资源验收完成、本机旧矩阵通过、真实跨机达标是四个独立状态。

## 10. N00～N15 执行节点

依赖顺序为 N00 → N01 → … → N15；相邻节点可以复用已取得的证据，不能跳过验收。每个节点至少单独一个 commit；N10 若需额外架构设计，先提交设计再提交实现。编制本文时以下节点均未执行。节点完成表示该节点产物和判据满足，不代表全项目性能通过。

每次提交前更新证据目录的 `execution.md`：节点、起止源码、涉及文件、准确命令、退出码、证据链接、通过/失败/受阻项、下一节点。提交后在下一次记录或最终交接中补 commit hash，避免要求一个提交包含自身 hash。提交代码前先完成该节点对应验证，失败不写成通过；重要失败证据可以独立提交。

### N00：冻结起点和方案

- **输入**：本文、实际 HEAD/工作区、旧总方案和第 2 节证据。
- **工作**：确认文件归属；保存只读环境审计；保留当前生产二进制或从冻结源码独立构建；创建本轮证据目录与执行台账。已有本文的独立提交可复用，不需要再做空提交。
- **验证**：确认旧生产源码、旧性能基线 `f066a82` 和本次原网络基线是不同用途；证据目录不覆盖旧文件；无团队 MCP 等待依赖。
- **产物/完成判据**：方案、基线清单和工作区归属记录齐备，计划已提交；尚未修改生产行为。

### N01：补齐发布测量和比较工装

- **输入**：当前 `benchmark.cc/.py`、`multipublisher.py`、`fairness.py`。现有多发布脚本只接受 2/8，现有公平脚本写死 K=4，不能当作新矩阵已经支持。
- **工作**：新增或扩展工装，支持 1/2/8 发布者、轮流/真实并发、同进程/多进程、同话题/不同话题；支持网关参数转发与真实 S/W/端口读回；增加平均值和第 8 节应用侧/现有网关诊断。建立第 11 节新矩阵入口，允许对原二进制测试。
- **验证**：用可核算样本检查均值/nearest-rank、按消息配对、失败样本留存和退出码；并发场景保存起止区间并计算跨发布者重叠；不重叠时明确标记，不能算并发测试通过。短冒烟检查预定总速率一致和载荷身份唯一。
- **产物/完成判据**：工装源码、测量 schema、冒烟原始样本和用法；能正确区分调用耗时与等待轮次，正式性能无打点模式可用。

### N02：先测现有 K=4/8/16 上限

- **输入**：冻结原网关/库、N01 工装和 CPU 预算。
- **工作**：在同总负载下测单话题、多发布和冷热混跑；为 K=8/16 显式选择范围外 control_port，修正测试端口预留器，不能继续使用 `base+4` 导致冲突。保留同/不同 shard 数据及每个配置的实际线程数。
- **验证**：读回 S=K、W=K；确认包从期望端口发出、CPU 未超预算、正式窗口互不重叠、发送率无缩水；记录全部轮次。
- **产物/完成判据**：`baseline-network/` 完整证据及基线选择理由；确定预算内最佳原 pooled 配置。不能以 K=16 已改善为由跳过端点隔离与资源治理，除非用户改变任务。

### N03：冻结配置、协议和所有权设计

- **输入**：第 3～9 节、N01/N02 发现。
- **工作**：将新增配置、错误分类、预算公式、v1/v2 字节表、端点状态机、线程/锁/lease 归属、实验 case 和阈值固化为设计记录。给 RouteKey→endpoint→worker 映射及控制反馈路径画出实现结构；列出全部旧 shard 索引依赖。
- **验证**：检查 S≠W、仅 PUB/仅 SUB、目录安装失败、注销期间 ACK、复用端口和预算回滚的推演；完成一次协议尺寸/偏移核对。设计修订记录原因，不能留下“实现时再决定端点 epoch 如何匹配”。
- **产物/完成判据**：无关键所有权和协议空白的设计 commit；正式 case 与通过门槛已冻结，可进入生产实现。

### N04：在 v1 pooled 中解耦 S/W

- **输入**：N03，`GatewayConfig`、`GatewayData::Impl::Shard`、`ReassemblyShard`。
- **工作**：引入稳定的 socket→worker 映射和独立 W；保留 v1 base/S 寻址。初期可为每个 socket 保留独立重组对象，但共享总配额，不能将原全局额度乘 S；队列、view 更新和注销 fence 按真实 owner 分发。
- **验证**：S/W=(1,1)、(4,1)、(4,2)、(4,4)、(4,8)、(16,4)，其中 W 不超过环境预算；对端不同 S 交叉测试；旧 `--data-shards` 行为、可靠重传、注销 barrier 和本机直达无回归。
- **产物/完成判据**：v1 解耦实现及专项测试；默认路径可用，S≠W 时无错端口/漏进展/配额翻倍。

### N05：统一资源预算和端口分配器

- **输入**：第 4 节，endpoint 创建/关闭入口、已有 quota 与状态 CLI。
- **工作**：实现 FD/候选端口/实际缓冲总额度预留与回滚；v2 端口分配器；记录 Creating/Ready/Closing 数量、高水位和失败原因。使用只属于测试进程的低 RLIMIT/小端口池模拟耗尽。
- **验证**：cap 精确边界、进程 FD 半数边界、被占端口、端口耗尽、内核缓冲实际值偏差、bind/等待注册失败及并发注册/注销。泄漏检查覆盖 FD 和额度账本，不能仅看 UDP 数。
- **产物/完成判据**：分配器、资源状态输出、确定性故障测试；全部失败可回滚，已有就绪话题继续运行，资源计数最终恢复。

### N06：实现网络 v2 codec 和目录

- **输入**：第 7 节，wire/peer directory/reliable/reassembly 代码和 v1 golden vectors。
- **工作**：实现双版本显式 codec、v2 HELLO/capabilities、v2 gateway directory、endpoint epoch 字段及目录一致性检查；新 golden vectors 与旧向量并存。所有 UDP 容量和计费跟随实际版本。
- **验证**：v1 字节不变；v2 双向编解码由独立 Python 向量核对；长度/CRC/保留位/版本错配、跨页条目、乱序分页、非法共用端口及超配额整快照拒绝；不同版本不互通且可诊断。
- **产物/完成判据**：codec/目录实现、向量和测试；未完成端点生命周期前不把 v2 模式暴露为可用生产模式。

### N07：接入三模式及端点 ready/close

- **输入**：N04～N06，local directory、gateway runtime、数据发送与接收入口。
- **工作**：实现 RouteDataBinding、聚合角色计数、冻结 TxTarget、v2 源/目标端点验证、可靠反馈匹配；接通 pooled/hybrid/per-topic 注册和注销状态机。
- **验证**：仅 PUB、仅 SUB、双角色、多 PUB/多 SUB 同话题；注销一个角色仍可服务；最后角色关闭；复用同端口的新代次拒绝旧 DATA/ACK；目录撤销期间重传终结且不重复业务提交。
- **产物/完成判据**：三模式端到端可运行；默认仍 v1；应用 UDP 数不随话题增长；数据端点数量符合第 3 节公式。

### N08：多 socket 等待、调度与关键话题隔离

- **输入**：N07，`DatagramLoop`、数据 worker 的发送/接收/重试队列。
- **工作**：实现可扩展的多 FD 等待、socket 与 RouteKey 双层公平预算、deferred 就绪续跑和稳定 owner；接入 dedicated 与 exclusive_worker 策略。队列上下限和字节预算按真实报文计。
- **验证**：大量空闲端点不出现忙轮询；一个热 socket、多热 RouteKey、EAGAIN、重传风暴不饿死冷流和控制命令；S 增加时 W 不变；独占 worker 不接收其他 RouteKey 工作；注销高 FD/旧 token 不误命中。
- **产物/完成判据**：调度实现、空闲 CPU/唤醒及公平性专项证据；仍无默认忙轮询和无限线程增长。

### N09：定位新架构剩余排队

- **输入**：N01 与 N08 二进制，完整出站/入站/反馈链路。
- **工作**：使用同一诊断 schema 比较原配置和新模式，补齐 drain→控制循环、可靠完成反馈排队；分同进程多话题、多进程多话题和同话题多发布。
- **验证**：同消息关联分段完整，丢弃计数明确；最慢 1% 消息归因基于原始记录而非 p99 相加；对照无诊断窗口估算观测开销；区分 socket、worker、出站及 ACK 控制等待。
- **产物/完成判据**：`queue-attribution.md` 与样本，明确 N10 哪一级满足进入条件或全部不采用；不得写“可能是共享网关”作为唯一结论。

### N10：有证据地优化出站 owner

- **输入**：N09 决策与第 8.2 节约束。
- **工作**：按证据实现多 drain 或移出逐消息控制工作；先提交具体所有权设计。若不满足进入条件，仅提交不采用的证据和理由；本节点仍有可审查产物，不做空提交。
- **验证**：可靠 request_id 冲突、重复 Record、发布者序号、信用恰好一次、同一 session 混合可靠/普通发布、慢消费者、关闭竞争和异常线程退出。改造后复测相同慢区间，并以相同总 CPU 预算衡量净收益。
- **产物/完成判据**：经专项验证的优化或保留决定；错误/重复不增加，改造没有收益则回退该改动并保留实验记录。

### N11：完整功能与构建兼容回归

- **输入**：N04～N10 最终候选，全部新旧功能测试。
- **工作**：运行 shared_net 功能集、与本机直达/DZFlat/MPMC/生命周期相关回归；独立 ASan+UBSan 构建；独立 `DZIPC_BUILD_SHARED_NET=OFF` 构建和相应配置/旧后端测试；实际启动 v1 回退配置。
- **验证**：先列出 CTest 测试清单，0 tests 不算通过；不得用 Release 构建替代 sanitizer 验证；记录平台/隔离权限限制和明确失败。测试环境按用例设置 MPMC，不能全局强制破坏旧语义测试。
- **产物/完成判据**：构建与测试日志、用例数量、退出码和 v1 回退记录；出现正确性缺陷先修复，再继续正式性能验收。

### N12：规模、资源耗尽、变更和故障

- **输入**：N11，11.1 的资源/故障矩阵。
- **工作**：完成 100/1000 话题注册规模、100 个活跃小消息话题、冷热混跑期间注册注销、端口/FD/缓冲耗尽、peer/网关重启、丢包乱序与错误包测试。1000 dedicated 需要显式提高 cap 并证明 FD/缓冲/内存可承受；默认 cap=256 时必须先证明第 257 个或更早有效额度边界明确拒绝。
- **验证**：分别报告注册数、就绪话题数、活跃数、socket/FD/线程、实际缓冲和内存峰值；资源回收后可继续注册；独占话题失败不静默回池。故障下可靠完成结果与接收事实一致，未知/可能已投递状态不触发业务自动重发。
- **产物/完成判据**：资源曲线、故障原始记录和泄漏结论。不能承受的规模保留为未验证并记录可运行上限，不通过突破“半数”预算完成测试。

### N13：冻结候选并完成性能比较

- **输入**：N11/N12 合格候选、N02 基线、N03 冻结 case 和阈值。
- **工作**：构建结束后冻结文件哈希，正式矩阵串行运行；覆盖 11.2 的必测场景和第 9 节全部比较；执行原完整本机 54 窗口。物理两机条件具备则完成跨机；缺少环境则保留待执行计划与明确缺口。
- **验证**：逐轮统计，提供计划/实际负载、每发布者均值和尾延迟、完整性、资源和时钟证据；三模式收益不可合并成一个百分比；不能用注册规模代替活跃负载。
- **产物/完成判据**：完整性能报告及原始证据，逐项“通过/失败/未验证”。未达阈值时继续已定位的必要修复，新的候选另开证据批次；不得重复无改动压测筛选最好成绩。

### N14：部署、兼容和回滚文档

- **输入**：最终配置、N12 资源边界、N13 实际结论。
- **工作**：补 CLI 帮助与用户文档，给出三模式示例、容量计算、PUB-only 也占端点、socket/worker 不等价、同话题多发布无独占增益、动态端口防火墙范围、v1/v2 不互通及回滚步骤。
- **验证**：示例通过参数解析与独立启动冒烟；同机器不重复启动同身份网关；回滚停止新注册、收敛在途工作、重启全部参与 peer 到 v1，再恢复应用；旧端口及控制路径可用。
- **产物/完成判据**：文档、帮助与回滚演练记录一致；明确默认模式与经证据推荐的部署配置，不无条件推荐最大 socket 数。

### N15：最终审计与交付

- **输入**：N00～N14 提交和证据。
- **工作**：核对 I01～I14、节点提交链、原始样本/汇总一致性、测试遗漏、预算与实际 socket 映射；确认未提交无关文件保持原状。写最终交接摘要、推荐配置、收益边界和仍未达标项。
- **验证**：从提交记录可找到实现和对应测试；从任一关键性能数字可定位原始样本、二进制和命令；v1 回退可执行；本机/跨机结论不混淆。
- **产物/完成判据**：最终审计 commit 和可独立阅读的交接报告。存在性能失败或外部环境缺口时明确保留，不把审计提交等同全部验收关闭。

## 11. 验证矩阵、工装接口和执行命令

### 11.1 功能、资源与故障必测矩阵

以下是必测组合，不要求将所有维度做无意义的全笛卡尔积；N03 在机器可承受范围内冻结具体 case。

| 维度 | 必测内容 | 判断重点 |
|---|---|---|
| 模式 / 协议 | v1 pooled；v2 pooled/hybrid/per-topic；v1 对 v2 | 默认兼容；错版本明确不可用；同版本双方允许不同模式/S/W |
| 角色与对象 | PUB-only、SUB-only、双角色；1/2/8 PUB；1/8 SUB；一个 PUB 退出再恢复 | 一话题一端点引用聚合；不随对象/peer 数倍增 |
| 编码与交付 | TLV、DZFlat、prebuilt；BestEffort、Reliable；分片 0/1/1024 边界及允许最大消息 | 有效字节/schema/返回语义不变；零长等边界按原协议允许性接受或拒绝 |
| socket 与 owner | S 小于/等于/大于 W；共享端点多个话题；独占 worker | 端口校验与 W 无关；数据队列和重组单 owner |
| 注册规模 | 0/1/100/1000 话题；默认预算边界及提高预算后的上限 | pooled：S0；hybrid：S0+D；per-topic：T；总 UDP 为 S+2 |
| 活跃规模 | 100 个 64B 话题，总 10000 条/秒，每话题 100Hz；1000 个注册中固定 100 个活跃 | 注册与负载分开；实际发送率与公平性；必要时记录过载 |
| 注册注销 | 热流期间每秒 10 次角色注册/注销，连续 60 秒；随机种子固定；角色数最终归零 | 冷流无饿死；端口/FD/内存回落；历史不被错误清除 |
| 资源失败 | FD 半数预算、cap、缓冲预算、候选端口耗尽；外部进程占用候选端口 | 明确错误、全回滚、已有事务能终结 |
| 生命周期 | 关闭时收包/发包/重传/提交；旧等待 token；端口强制复用；网关和 peer 重启 | 无 UAF、跨代次误投、重复提交和句柄泄漏 |
| 网络故障 | 丢包、重复、乱序、截断、错误 CRC/epoch/port/RouteKey；目录分页缺失和延迟 | 有界重组/重试；过期包提前拒绝；保留已安装完整目录 |
| 控制压力 | 大目录变更、可靠 ACK/NACK 密集、状态查询和出站队列同时活跃 | 共享控制路径进展与关闭时限，不能只测数据 socket |

故障注入只在测试拥有的网络/IPC/挂载命名空间或测试进程内执行，使用现有隔离工装；不对宿主机正常业务接口施加丢包。v2 同机双网关共享网络命名空间时必须使用不重叠的数据候选范围、不同控制端口及独立网关控制路径/身份。

### 11.2 性能必测场景

| 编号 | 场景 | 必须回答的问题 |
|---|---|---|
| P01 | 纯本机原 54 窗口，原载荷与 1/8/32 订阅 | 是否保持原直达路径；旧 24/27 失败是否仍存在 |
| P02 | 真实 UDP，1 话题 1 PUB/1 SUB，64B/4KiB/1MiB，100Hz | 新协议和映射在无争用时增加多少均值/尾延迟 |
| P03 | 1 话题 1/2/8 PUB，轮流发布，总 1000Hz；64B/4KiB | 同话题多个发布者平均调用与单发布相差多少 |
| P04 | 1 话题 1/2/8 PUB，真实并发；同进程与多进程，总 1000Hz；64B/4KiB | 发布调用确有重叠时，争用来自共享出站还是网络队列 |
| P05 | 1/2/8 话题分别配 PUB，重复 P03/P04 的总负载 | 按话题独立 socket 是否改善多话题发布与尾延迟 |
| P06 | 热 1MiB×70Hz，冷 64B×100Hz，1 个远端 SUB/话题 | 关键冷话题 p99 是否逐轮改善至少 20% |
| P07 | P06 中冷热共 worker / 独立 worker，总 W/CPU 预算一致 | 收益来自 socket 队列隔离还是 worker 执行隔离 |
| P08 | 100 个活跃小话题；1000 个注册、100 个活跃；P06 加注册注销 | 规模与目录变更是否抵消延迟收益 |
| P09 | 冷热消息分别发给 2 个 peer，可靠模式 | fan-out、目标冻结及共享 ACK 通道成本 |

P02/P06/P07 必须覆盖 BestEffort 与 Reliable、TLV 与 DZFlat；P03～P05 必须覆盖普通调用与可靠阻塞发布，首先用 TLV，至少对 8 PUB 的关键场景补 DZFlat。prebuilt 做功能和低负载延迟回归，不把编码差异混进 socket 收益。其他组合在 N03 依据诊断补入 case 表。

P04 的并发证据以调用区间为准。低负载下调用过短而无重叠时，保留该事实，另加入同步 burst 争用场景；单发布对照必须采用相同 burst 大小、间隔和总率，轮次等待时间单独记录。开放式发送时间表与实际开始时间同时保存，队列积压/晚发不能被隐藏为较低“API 延迟”。

P06 除了对照 N02 最佳原 pooled，还测 v2 pooled 的冷热共享/不同 socket，并读回实际映射。不能更换冷热消息内容、热流绝对速率或只选择有利的哈希碰撞。P09 需要第三个隔离实例或第三台机器；条件不足时标记该项未验证。

### 11.3 新增矩阵入口与证据结构

新增 `test/shared_net/topic_udp_matrix.py`，在 N01 实现，后续节点扩展；不得把它写成现有 `performance_matrix.py` 的别名。约定：

- `--plan <JSON>`：读取冻结的测试计划；`--output <目录>` 必须不存在或为空。
- `--validate-plan`：仅验证 schema/组合/预算字段，不启动进程；`--preflight`：额外核对二进制、实际配置/端口/隔离环境并运行短冒烟，不计入正式结果。
- 省略上面两个检查参数时顺序运行计划内全部正式窗口。`--diagnostics` 只写诊断批次，manifest 标记不可用于正式门槛；输出路径不能与正式批次相同。
- 任一功能失败、窗口失败或必须项缺失，退出码非 0；同时保留每 case 的结果。基础设施故障不得被当作该配置性能退化，应标记 `infrastructure_error` 并停止受影响后续窗口。

计划 schema 至少包含 `schema_version`、基线/候选源码与二进制/库路径、按 argv 数组保存的网关参数、主机角色与时钟说明、CPU 预算和允许集合、完整 RouteKey/策略、计划速率/消息大小/编码/发布类型、发布者进程布置与发送调度、预热/采样/轮次、case 次序和阈值。source revision 和 SHA256 来自实际文件，不能硬编码“current”版本；64 位标识在 JSON 中使用十进制字符串。

物理多机模式由各主机运行相同版本 probe，协调器汇总原始结果；N01/N03 明确角色命令和产物路径，避免要求执行者临时手工拼接跨机时间戳。若当前环境不能实现或执行物理两机工装，交付可检查的角色计划并将该项保留未完成，不能用同机两个 IPC 命名空间替代“跨机已通过”。

建议产物结构：

```text
<日期>-topic-udp/
  execution.md                  # 节点、提交、命令与结果索引
  design.md                     # N03 冻结设计与修订理由
  plans/                        # 冻结 case/机器/配置/门槛
  baseline-network/             # 原 K=4/8/16 完整窗口
  diagnostics/                  # 打点样本，明确观测成本
  validation/                   # CTest、sanitizer、OFF、故障日志
  resources/                    # FD/socket/线程/缓冲/内存曲线
  performance/                  # 新矩阵所有轮次与原始 CSV 或无损压缩
  local-54/                     # 原本机矩阵，单独汇总
  results.md                    # 逐项结果及未达标原因
  handoff.md                    # 最终交接与推荐配置
```

每个性能批次含 `manifest.json`、`cases.json`、`binary_manifest.json`、样本文件及完整日志。原始样本保留 publisher/sequence、计划时间、调用起止、完成/接收时间、状态和关联标识；只保存直方图不够。诊断字段和时钟域必须在 schema 中声明。

### 11.4 已有构建与回归入口

以下命令只供接手 Agent 执行；编制本文不执行。使用新的构建目录，接手时若同名目录已有其他任务则另取名字。构建完成后再测试和压测，不并行正式负载。

```bash
cd /home/zwc/cpp_ipc_dds
git rev-parse HEAD
git status --short
cmake -S . -B build-topic-udp \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_DEBUG_INFO=OFF \
  -DLIBIPC_BUILD_TESTS=ON \
  -DLIBIPC_BUILD_PYTHON=OFF \
  -DUPDATA_MSG_SRV_GENERATOR=OFF \
  -DDZIPC_BUILD_SHARED_NET=ON
cmake --build build-topic-udp --parallel 4
ctest --test-dir build-topic-udp -L shared_net -N
ctest --test-dir build-topic-udp -L shared_net --output-on-failure -j 1
ctest --test-dir build-topic-udp \
  -R 'test_(shm_mpmc|shm_multi_publisher|dzflat|lifecycle_contract|ipc_info_pool)' \
  --output-on-failure -j 1
```

实际构建并行度不得超环境预算；正式比较核验 compile_commands 和实际加载的库，不能凭 RelWithDebInfo 名称保证优化级别相同。新增测试在 `test/CMakeLists.txt` 登记，先确认 GLOB 是否已创建 target，避免重复 `add_executable`；设置 `shared_net` 标签、时限和隔离资源。不存在 `test/shared_net/CMakeLists.txt`。

sanitizer 使用独立目录，相同源码配置加 `-fsanitize=address,undefined -fno-omit-frame-pointer` 编译及对应 executable/shared linker 选项，执行生命周期、端口复用、目录和可靠故障相关测试；日志记录实际选项及 sanitizer 输出。若采用其他已有 sanitizer 配方，先核验确实检测到目标代码，不仅设置环境变量。

OFF 构建另外配置 `-DDZIPC_BUILD_SHARED_NET=OFF`，检查配置测试及相关旧后端回归；不能对 OFF 构建要求不存在的网关目标，也不能把不再注册的新功能测试算成通过。

新矩阵入口在 N01 实现并由 N03 生成计划后运行，示例中的计划文件是该节点产物：

```bash
python3 test/shared_net/topic_udp_matrix.py \
  --plan docs/shared_network_endpoint_evidence/YYYYMMDD-topic-udp/plans/local-network.json \
  --validate-plan
python3 test/shared_net/topic_udp_matrix.py \
  --plan docs/shared_network_endpoint_evidence/YYYYMMDD-topic-udp/plans/local-network.json \
  --output docs/shared_network_endpoint_evidence/YYYYMMDD-topic-udp/preflight --preflight
python3 test/shared_net/topic_udp_matrix.py \
  --plan docs/shared_network_endpoint_evidence/YYYYMMDD-topic-udp/plans/local-network.json \
  --output docs/shared_network_endpoint_evidence/YYYYMMDD-topic-udp/performance
```

以上 `YYYYMMDD-topic-udp` 是待替换为本轮证据目录名的占位符。原 54 窗口继续使用 `test/shared_net/performance_matrix.py` 的真实 `--baseline/--current/--gateway/--output` 入口，核验其固定基线和库路径假设；不删减 cases 假称完整复测。已有 `fairness.py` 与 `scale.py` 需要 N01/N12 扩展参数和资源读回后才能验证新模式。

## 12. 最终交付、回滚与接手提示词

### 12.1 交付清单

- N00～N15 提交索引；实现、测试、使用文档、CLI 帮助与配置示例。
- 一个默认兼容 v1 的候选，三个显式模式的行为、实际资源上限及失败返回清楚；未达性能目标的功能不自动成为默认模式。
- 原始样本、汇总、源码/二进制/动态库哈希、准确命令、环境/时钟/CPU/资源证据；所有失败和未执行项可追溯。
- 结论分别回答：轮流多发布平均延迟差、真实并发争用、独立 socket 的净收益、独立 worker 的额外收益、共享出站/ACK 控制循环的剩余成本、100/1000 话题可承受范围。
- 原本机 24/27 状态单列，真实跨机验证状态单列，不能合并成“全部通过”。

### 12.2 回滚步骤

1. 停止接纳新的网络发布/注册，按既有 deadline 等待或明确终结在途可靠事务；记录可能已经远端投递的结果，应用不得自动重发整个业务事件。
2. 保存状态、指标、执行版本和配置；正常停止新模式网关，等待 owner barrier 和端点关闭。只处理本次任务所属进程/资源，不清理其他任务的 SHM 或文件。
3. 将所有参与互通的网关配置恢复到冻结的 v1 pooled 配置和匹配二进制；按已验证的旧 base/K/control/discovery 地址启动。v1 与 v2 不能混跑并当作滚动互通升级。
4. 让应用重新建立会话/注册，等待路由发现同步；小流量验证本机直达、跨 peer 接收与可靠完成后恢复计划负载。
5. 记录回滚原因与证据。优先回退具体无收益优化；不通过回滚删除失败样本或改写历史结论。

### 12.3 延期范围

本轮不默认引入单话题多 lane、多网关同身份分片、每话题应用出站 SHM、DPDK/AF_XDP/io_uring 全面替换、忙轮询、CPU 全局隔离或网卡 RSS 调参。按话题端点数量增加可能改善网络栈流散列，但能否分散到不同 NIC 队列取决于真实 RSS 配置和流量，必须取证，不能预设线性加速。

如果测量显示平均发布耗时主要来自本机编码/复制、应用共享锁或接收调度，本次 socket 隔离不能替代相应修复。将证据和下一步设计写入最终报告，避免继续盲目增大 socket 数。

### 12.4 可复制给接手 Agent 的任务

> 阅读 `docs/topic_udp_data_channel_execution_plan.md`，按 N00～N15 顺序执行按话题 UDP 数据通道改造。先冻结基线和完整方案，再修改生产实现，每完成一个节点单独提交 commit。保持本机 MPMC/DZFlat 语义、默认 v1 兼容、网络 v2 明确隔离和最多半数资源预算。团队 MCP 不可用，忽略相关调用要求，默认单 Agent 执行。不要提交工作区里归属不明的报告和 artifacts；正式压测不得并行其他压测或编译。先测原 K=4/8/16，再测同 worker/CPU 预算下的 pooled/hybrid/per-topic；根据分段证据决定是否优化出站 drain/逐消息控制循环。保留每轮原始数据和失败，原本机 24/27 与网络新目标分开验收。缺少物理多机或时钟证据时明确标记未验证，继续完成不依赖该环境的工作。交付实现、测试、资源与性能证据、回滚说明及未达标原因，不以功能通过代替性能达标。
