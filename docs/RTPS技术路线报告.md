# dzIPC 完整 RTPS 技术路线报告

> 状态: **完整 RTPS 未启动**; 第 8 节轻量路径已完成 5 项中的 3 项
> (单元测试 + 端到端性能均已实测通过)
> 编写日期: 2026-08-06
> 最后更新: 2026-08-07 (三项落地并实测; 修复 §7.5 的 CRC 时序缺陷)
> 前置结论来源: 2026-08-05 性能测试与丢包诊断(见文末"数据依据")

**读者请先看 §8.1 的状态表** —— 本报告主体(§1~§7)描述的是完整 RTPS 方案, 尚未
启动; 实际动过的代码只有 §8 轻量路径的前三项。

---

## 0. 先回答一个前置问题: 要不要做

在展开路线之前, 必须先说清楚一件事 —— **当前实现已经能满足既有目标场景**。

2026-08-05 的诊断结论是: 大包丢包的根因是接收端 `net.core.rmem_max` 默认只有
208 KB, 而不是协议缺陷。把它调到 64 MB 之后, 1 MB payload 在 pub-sub 与 ser-cli
上都能跑满 ~110 MB/s 且零丢包。也就是说:

**如果目标场景是"同机 / 同交换机、低丢包、自己人和自己人通信", 那么完整 RTPS
带来的收益接近于零, 而成本是一次重写。**

完整 RTPS 只有在下面任意一条成立时才值得做:

| 触发条件 | 说明 |
|---|---|
| **要与第三方 DDS 互操作** | 与 Cyclone DDS / Fast DDS / RTI Connext 在同一总线上收发 |
| **要跨广域网 / 高丢包链路** | 丢包率 > 1%, RTT > 10 ms, 现有 NACK 列表会退化 |
| **要支持标准 QoS 语义** | Durability(晚加入者补历史)、Deadline、Liveliness 等 |
| **要通过 DDS 合规认证** | 军工 / 车规 / 航空场景的采购要求 |

若四条都不成立, 本报告的正确用法是**作为备选方案存档**, 并优先执行第 8 节的
"轻量替代路径" —— 用约 5% 的成本拿到 60% 的收益。

---

## 1. 现状盘点

### 1.1 已有的 RTPS-like 机制

当前实现借鉴了 RTPS 的分片重传思想, 但不是标准 RTPS。已有部分:

| 能力 | 实现位置 | 说明 |
|---|---|---|
| 消息分片 | `data_rev.cc: chunk_send_ex` | 固定 1472 B / 片, 12 B tail |
| 分片重组 | `data_rev.cc: recv_chunk_common` | 位图 + 空洞扫描 |
| ACK 确认 | `IpcRtpsAckMsg` ("DZAK") | 携带 `payload_crc32c` |
| NACK 重传 | `IpcRtpsNackMsg` ("DZNK") | 显式缺失片列表, 上限 256 |
| RTO 自适应 | `ack_first_wait_ms` | RFC 6298 风格, 按实测 RTT 收敛 |
| 重传节流 | `kFruitlessRoundLimit` | 连续两轮无新片则放弃 |
| 完整性校验 | `crc32c` | CRC32C, 可选 |
| 进程发现 | `IpcInfoPool` | 共享内存信息池, 非 SPDP |

### 1.2 现有线格式

**数据分片 tail (12 B, 位于每片末尾)**

```
偏移   长度  字段              说明
 0     2    total_page_cnt    总片数
 2     2    now_page          当前片号, 从 1 开始, 0 保留
 4     4    total_size        消息总字节数
 8     4    dz_ipc_msg_id     消息类型 ID
```

**ACK ("DZAK")**

```
page_cnt(2) total_size(4) data_msg_id(4) receiver_id(4)
sequence(4) integrity_flags(1) payload_crc32c(4)
```

**NACK 显式列表 ("DZNK")**

```
page_cnt(2) total_size(4) data_msg_id(4) receiver_id(4)
sequence(4) miss_cnt(2) missing_pages[miss_cnt](2 each)
```

**NACK 位图 ("DZNB", 2026-08-06 新增)**

```
page_cnt(2) total_size(4) data_msg_id(4) receiver_id(4)
sequence(4) base_page(2) map_bytes(2) bitmap[map_bytes]
```

`bit i` 置 1 表示页号 `base_page + i` 缺失(与 RTPS `SequenceNumberSet` 同向:
base 是**第一个缺失的**页号, 不是最后一个收到的)。载荷上限 1459 B 以保证单页 ——
分片后的 NACK 到达发送端会被逐 datagram 反序列化出半截垃圾, 所以这是正确性要求。

**Heartbeat ("DZHB", 2026-08-06 新增)**

```
page_cnt(2) total_size(4) data_msg_id(4) sender_id(4)
sequence(4) flags(1) round(2)
```

`flags`: bit0 `Reliable`(会响应 NACK), bit1 `Final`(不再重传),
bit2 `BitmapNack`(认识 `DZNB`)。恒 33 B, 单页。

### 1.3 与标准 RTPS 的结构性差距

| 维度 | 当前实现 | 标准 RTPS | 差距性质 |
|---|---|---|---|
| **编号体系** | 单层 `now_page` + 进程级 `sequence` | 双层 `writerSN`(64位) + `fragmentNumber` | **结构性** |
| **Writer 主动通告** | 无 | `Heartbeat` / `HeartbeatFrag` | **结构性** |
| **NACK 编码** | 显式列表(≤256 片) + 位图(`DZNB`, ≤11496 位), 按线格式字节数自动择优 | `SequenceNumberSet` 位图(base + bitmap) | 已基本对齐 |
| **历史缓存** | 无(发完即弃) | WHC / RHC, 支持 Durability | **结构性** |
| **实体标识** | `receiver_id` (4 B) | GUID (16 B: GuidPrefix 12 + EntityId 4) | **结构性** |
| **发现协议** | `IpcInfoPool` 共享内存 | SPDP + SEDP (标准端口组播) | **结构性** |
| **QoS** | 无 | Reliability / Durability / History / Deadline ... | **结构性** |
| **消息封装** | 裸 payload + tail | RTPS Message: Header + Submessage 序列 | **结构性** |
| **字节序** | 主机序 | 显式 endianness flag (每 Submessage) | 可增量改造 |
| **NACK 抑制** | 无 | `nackSuppressionDuration` | 可增量改造 |

**结论: 8 项结构性差距中, 6 项无法增量改造。** 这决定了完整 RTPS 是重写而非重构。

### 1.4 关键约束: 组播 loopback 与 ACK

2026-08-05 的实测发现一个对路线设计有决定性影响的事实:

> **在 `IP_MULTICAST_LOOP=1` 的组播 socket 上, 发送端无法在同一 socket 上等 ACK。**
> 自己发出的分片会全部回绕进自己的接收队列, 排在对端 ACK 之前。1 MB = 713 片
> 回绕分片, ACK 永远等不到。

这一条同时否定了 pub-sub 和 ser-cli 的"发送端等 ACK"方案(ser-cli 虽然分了
请求/响应两条通道, 但每条通道内部仍是收发共用, 同样中招)。

**对 RTPS 改造的影响: Writer 与 Reader 必须使用独立的收发端点。** 这是第 3 节
架构设计的硬约束, 不是可选项。标准 RTPS 天然满足(Writer 发数据用组播、收 AckNack
用单播回 Reader 的 unicastLocator), 但我们必须显式实现这个分离。

---

## 2. 目标定义

### 2.1 范围界定

采用 **RTPS 2.3 (OMG formal/2019-04-03) 的最小互操作子集**, 不实现完整规范。

**纳入范围**

- RTPS Message 封装 (Header + Submessage)
- 核心 Submessage: `DATA`, `DATA_FRAG`, `HEARTBEAT`, `HEARTBEAT_FRAG`, `ACKNACK`, `NACK_FRAG`, `GAP`, `INFO_TS`, `INFO_DST`
- Stateful Writer / Reader 状态机
- SPDP (参与者发现) + SEDP (端点发现)
- QoS: `Reliability`, `Durability`(VOLATILE / TRANSIENT_LOCAL), `History`(KEEP_LAST / KEEP_ALL)
- CDR 序列化 (与现有 `IpcMsgBase` 并存)

**排除范围**(明确不做, 避免范围蔓延)

- Security (DDS-Security 插件体系)
- 除上述之外的 QoS(Deadline / LatencyBudget / Ownership / Partition / TimeBasedFilter ...)
- Content-Filtered Topic / Query Condition
- Persistence(TRANSIENT / PERSISTENT durability, 需外部存储)
- TCP / SHM 传输的 RTPS 封装(仅做 UDP)
- Type System (XTypes / DynamicData)

### 2.2 验收标准

| 编号 | 标准 | 验证方式 |
|---|---|---|
| A1 | 与 Cyclone DDS 互通 pub-sub | 本项目发, `cyclonedds` 订阅, 反向亦然 |
| A2 | 与 Fast DDS 互通 pub-sub | 同上 |
| A3 | 1 MB payload 零丢包 | 现有 benchmark, 64 MB rmem_max |
| A4 | 性能不低于当前实现的 80% | 现有 benchmark 对比, 各 payload 档位 |
| A5 | 5% 人为丢包下可靠交付 | 需新增丢包注入 |
| A6 | TRANSIENT_LOCAL 补历史 | 晚加入的 Reader 能收到之前的样本 |
| A7 | Wireshark 可解析 | 用 Wireshark 的 RTPS dissector 抓包验证 |

**A7 值得单列**: Wireshark 内置 RTPS 解析器, 一旦线格式正确, 它能直接告诉你
哪个字段错了。这是整个项目最廉价的调试杠杆, 应在第一个 Submessage 落地时就用上。

---

## 3. 架构设计

### 3.1 分层

```
┌─────────────────────────────────────────────────┐
│  应用层 (现有 API 保持不变)                        │
│  PublisherIPC / SubscriberIPC / Server / Client │
├─────────────────────────────────────────────────┤
│  DDS 层 (新增)                                   │
│  DomainParticipant / Publisher / Subscriber     │
│  DataWriter / DataReader / Topic / QoS          │
├─────────────────────────────────────────────────┤
│  RTPS 层 (新增, 本报告核心)                        │
│  ┌──────────────┐  ┌──────────────┐            │
│  │ StatefulWriter│  │ StatefulReader│           │
│  │  + WHC        │  │  + RHC        │           │
│  │  + ReaderProxy│  │  + WriterProxy│           │
│  └──────────────┘  └──────────────┘            │
│  Discovery: SPDP + SEDP                         │
│  Message: Header + Submessage 编解码             │
├─────────────────────────────────────────────────┤
│  传输层 (改造现有 UDPNode)                        │
│  独立收发端点 / 组播 + 单播 / Locator 抽象         │
└─────────────────────────────────────────────────┘
```

### 3.2 关键数据结构

```cpp
// ---- 标识 ----
struct GuidPrefix { uint8_t value[12]; };   // 参与者唯一, 建议 IP + PID + 随机
struct EntityId   { uint8_t key[3]; uint8_t kind; };
struct Guid       { GuidPrefix prefix; EntityId entityId; };

// 64 位序列号, 全局单调递增, 每个 Writer 独立
struct SequenceNumber { int32_t high; uint32_t low; };

// ---- 缓存 ----
struct CacheChange {
    ChangeKind      kind;          // ALIVE / NOT_ALIVE_DISPOSED / NOT_ALIVE_UNREGISTERED
    Guid            writerGuid;
    InstanceHandle  instanceHandle;
    SequenceNumber  sequenceNumber;
    Timestamp       sourceTimestamp;
    std::vector<uint8_t> serializedPayload;   // CDR
};

class WriterHistoryCache {          // WHC
    std::map<SequenceNumber, CacheChange> changes;
    HistoryQos qos;                 // KEEP_LAST(depth) / KEEP_ALL
    size_t     highWatermark;       // 流控: 超过则阻塞 write()
    size_t     lowWatermark;        // 降到此值恢复
};

// ---- 远端代理 ----
class ReaderProxy {                 // Writer 侧, 每个匹配的 Reader 一个
    Guid                    remoteReaderGuid;
    std::vector<Locator>    unicastLocatorList;    // ACKNACK 回这里
    std::vector<Locator>    multicastLocatorList;
    SequenceNumber          highestAckedSN;        // 已确认到哪
    std::set<SequenceNumber> requestedChanges;     // 待重传
    bool                    expectsInlineQos;
};

class WriterProxy {                 // Reader 侧, 每个匹配的 Writer 一个
    Guid            remoteWriterGuid;
    SequenceNumber  maxAvailableSN;   // 从 Heartbeat 得知
    SequenceNumber  minAvailableSN;
    std::set<SequenceNumber> missingChanges;
    // 分片重组: SN -> 该消息的分片位图
    std::map<SequenceNumber, FragmentBuffer> fragBuffers;
};
```

### 3.3 端点分离(应对 1.4 的约束)

```cpp
class RtpsEndpoint {
    int mcast_recv_fd;    // 加入组播组, 只收
    int ucast_recv_fd;    // 绑定本地端口, 收 ACKNACK / NACK_FRAG
    int send_fd;          // 只发, 且不加入组播组
    Locator unicast_locator;   // 通过 SPDP 通告给对端
};
```

**要点: `send_fd` 不调用 `IP_ADD_MEMBERSHIP`。** 组播的接收资格来自入组, 发送不
需要组成员资格 —— 于是"只发不收"的 socket 天然收不到任何东西, 包括自己发出去被
内核回绕回来的分片。这是 1.4 节问题的根治办法。

> **勘误 (2026-08-06)**: 本节此前写的是"`send_fd` 必须设 `IP_MULTICAST_LOOP=0`"。
> **那是错的, 照做会打断 dzIPC 的核心用例。**
>
> `IP_MULTICAST_LOOP` 是发送端选项, 且作用于**整台主机**而不是单个 socket:
> 置 0 之后本机所有 socket(包括其他进程的订阅者、`dzipc_topic_cat` 这类抓包
> 工具)都收不到这个包。man 7 ip 的原话是 "whether sent multicast packets should
> be looped back to the local **sockets**" —— 复数, 指本机全部。
>
> 而 dzIPC 的主场景恰恰是同机跨进程通信, 关掉回环等于让所有本地订阅者失聪。
> 正确做法是上面的"不入组", `IP_MULTICAST_LOOP` 一律保持 1。
>
> 已按此实现, 见 `include/libipc/udp.h` 的 `NodeRole`。`test_socket.cpp`
> (同进程一收一发, 依赖回环)是防止有人再次改错的哨兵测试。

同进程内的自发自收由 DDS 层的本地匹配处理(类似现有 nodelet 快路径), 不依赖
组播回绕。

---

## 4. 实施路线

分 6 个阶段, 每阶段独立可验证。**允许在任意阶段末尾停止** —— 后文标注了各阶段
的独立价值。

### 阶段 1: 传输层与 Locator 抽象

**周期估算: 2 周**

- [ ] `Locator` 结构 (kind / port / address[16])
- [x] `RtpsEndpoint`: 收发分离 —— **已以 `UDPNode::NodeRole` 的形式落地
      (2026-08-06)**, 但注意实现方式与原文不同, 见下方勘误
- [ ] 标准端口计算 (RTPS 规范 9.6.1.1):
  ```
  PB=7400, DG=250, PG=2, d0=0, d1=10, d2=1, d3=11
  SPDP 组播: PB + DG*domainId + d0
  SPDP 单播: PB + DG*domainId + d1 + PG*participantId
  用户组播: PB + DG*domainId + d2
  用户单播: PB + DG*domainId + d3 + PG*participantId
  ```
- [x] 现有 `UDPNode` 保留, 两套传输并存 —— 当前是同一个 `UDPNode` 加角色参数,
      不是两套实现; 真做 RTPS 时仍需新增 `RtpsEndpoint`

**独立价值**: 即使后续不做 RTPS, 收发分离也能解决 1.4 节的 ACK 阻塞问题, 让现有
的 Reliable 模式真正可用。**这一条已经兑现**(见 §8.1)。

**验收**: 单元测试 —— 两个端点互发, 发送端不收到自己的包。
✅ 已通过 (`test_socket_endpoint_split`, 2026-08-06)。

---

### 阶段 2: RTPS Message 编解码

**周期估算: 3 周**

- [ ] `Header`: magic "RTPS" + version(2.3) + vendorId + guidPrefix (20 B)
- [ ] `SubmessageHeader`: id(1) + flags(1) + octetsToNextHeader(2)
- [ ] Submessage 编解码:
  - [ ] `DATA` (0x15)
  - [ ] `DATA_FRAG` (0x16)
  - [ ] `HEARTBEAT` (0x07)
  - [ ] `HEARTBEAT_FRAG` (0x13)
  - [ ] `ACKNACK` (0x06)
  - [ ] `NACK_FRAG` (0x12)
  - [ ] `GAP` (0x08)
  - [ ] `INFO_TS` (0x09), `INFO_DST` (0x0e)
- [ ] `SequenceNumberSet` / `FragmentNumberSet` 位图编解码
- [ ] endianness flag 处理 (每 Submessage 的 flags bit 0)
- [ ] CDR 序列化基础设施

**⚠ 高风险点**

1. **`SequenceNumberSet` 的位图语义容易搞错**: `bitmapBase` 是**第一个缺失的**
   SN, 不是最后一个收到的。`numBits` 上限 256。bit i 置 1 表示 `base + i` 缺失。
2. **`octetsToNextHeader = 0` 的特殊含义**: 表示该 Submessage 延伸到消息末尾,
   只对最后一个 Submessage 合法。
3. **`SequenceNumber` 是有符号 64 位**, `{high=-1, low=0}` 是 `SEQUENCENUMBER_UNKNOWN`。
4. **对齐**: CDR 要求 4 字节对齐, Submessage 之间要 padding。

**验收**:
- 单元测试覆盖每种 Submessage 的编解码往返
- **Wireshark 抓包能正确解析** —— 这是最硬的验收, 优先做

---

### 阶段 3: Stateful Writer / Reader

**周期估算: 4 周**

- [ ] `WriterHistoryCache` + `HistoryQos` (KEEP_LAST / KEEP_ALL)
- [ ] `ReaderHistoryCache`
- [ ] `ReaderProxy` / `WriterProxy` 状态机
- [ ] Writer 侧:
  - [ ] 周期 `HEARTBEAT` (可配置周期, 默认 100 ms)
  - [ ] 响应 `ACKNACK` → 重传
  - [ ] 响应 `NACK_FRAG` → 重传指定分片
  - [ ] `heartbeatResponseDelay` / `nackSuppressionDuration`
  - [ ] WHC 高低水位流控
- [ ] Reader 侧:
  - [ ] 收 `HEARTBEAT` → 对比本地 → 发 `ACKNACK`
  - [ ] 收 `DATA_FRAG` → 重组 → 收 `HEARTBEAT_FRAG` → 发 `NACK_FRAG`
  - [ ] 收 `GAP` → 标记这些 SN 不再期待
- [ ] `MaxQueuedRexmitBytes` 重传节流

**⚠ 高风险点**

1. **重传风暴**: N 个 Reader 同时 NACK 同一个 SN。必须实现
   `nackSuppressionDuration`, 否则 Writer 会重传 N 次。
   *(轻量路径已顺手解掉一半: 发送端的 `missing_union` 是集合, 一轮内跨订阅者
   自动去重, 所以不会重传 N 次。缺的是跨轮抑制, 以及重传走单播而非组播。)*
2. **Heartbeat 频率权衡**: 太快浪费带宽, 太慢则丢包恢复延迟高。
   建议初值 100 ms, 并在 Writer 有新数据且存在未确认样本时提前触发。
   *(轻量路径的 Heartbeat 不是周期性的, 是"发完即通告"+"每轮重传后补发",
   见 §8.1。周期 Heartbeat 是 Durability 才需要的, 阶段 3 再做。)*
3. **WHC 内存**: KEEP_ALL + 慢 Reader = 无限增长。必须有 `ResourceLimits` 兜底。
4. **状态机的并发**: Writer 的定时 Heartbeat 线程与 write() 调用线程会同时访问
   WHC, 锁粒度设计不当会成为性能瓶颈。建议 WHC 用读写锁, ReaderProxy 各自独立锁。
5. **`ReaderProxy` 是"全体确认"语义的前提**。轻量路径的 `publish_blocking()` 收到
   任意一个订阅者的 ACK 就返回成功(§8.2), 因为没有订阅者集合的概念。要做到标准
   RTPS 的"全部匹配 Reader 都确认才算投递", 必须先有 `ReaderProxy` 逐个跟踪
   `highestAckedSN`。这是阶段 3 相对轻量路径最实质的增量之一。

**独立价值**: 到此为止, 可靠传输已完整。若不需要与第三方互操作, **可以在这里停止**,
用现有的 `IpcInfoPool` 做发现, 跳过阶段 4。

**验收**: A3 (1 MB 零丢包), A5 (5% 丢包下可靠交付)

---

### 阶段 4: 发现协议 (SPDP + SEDP)

**周期估算: 3 周**

- [ ] `SPDP`: 内置 Writer/Reader, 周期组播 `ParticipantBuiltinTopicData`
  - [ ] 默认周期 30 s, `leaseDuration` 100 s
  - [ ] 参数列表 (PID) 编解码
- [ ] `SEDP`: 内置 Writer/Reader, 交换 Publication/Subscription 数据
  - [ ] `PublicationBuiltinTopicData`
  - [ ] `SubscriptionBuiltinTopicData`
- [ ] 端点匹配: Topic 名 + 类型名 + QoS 兼容性检查
- [ ] Liveliness: 租约超时 → 清理远端参与者

**⚠ 高风险点**

1. **PID 参数列表极其琐碎**: 几十个 PID, 各家实现对可选 PID 的处理不一。
   建议先只实现必需 PID, 遇到互操作问题再逐个补。
2. **QoS 兼容性规则**: RTPS 规范定义了 requested/offered 的兼容矩阵,
   写错会导致端点不匹配且**没有任何错误提示** —— 表现为"能发现但收不到数据",
   这是最难查的一类问题。
3. **与现有 `IpcInfoPool` 的关系**: 建议**并存而非替换**。SPDP/SEDP 用于跨实现
   互操作, `IpcInfoPool` 保留用于同进程/同机的快路径优化(nodelet)。

**验收**: A1, A2 (与 Cyclone / Fast DDS 互通)

---

### 阶段 5: QoS 与 DDS 层

**周期估算: 3 周**

- [ ] `ReliabilityQos`: BEST_EFFORT / RELIABLE
- [ ] `DurabilityQos`: VOLATILE / TRANSIENT_LOCAL
- [ ] `HistoryQos`: KEEP_LAST(depth) / KEEP_ALL
- [ ] `ResourceLimitsQos`: max_samples / max_instances / max_samples_per_instance
- [ ] DDS 层门面: `DomainParticipant` / `Publisher` / `Subscriber` / `DataWriter` / `DataReader`
- [ ] 现有 API 适配层(保证向后兼容)

**验收**: A6 (TRANSIENT_LOCAL 补历史)

---

### 阶段 6: 互操作测试与性能调优

**周期估算: 3 周**

- [ ] 与 Cyclone DDS 互操作矩阵(各 QoS 组合 × 各 payload 档位)
- [ ] 与 Fast DDS 互操作矩阵
- [ ] 性能回归: 现有 benchmark 全量对比
- [ ] 丢包注入测试 (1% / 5% / 10%)
- [ ] 长稳测试 (72 h)
- [ ] Wireshark 抓包归档(作为线格式的回归基线)

**验收**: A4 (性能不低于当前 80%), A7 (Wireshark 可解析)

---

## 5. 工作量与风险

### 5.1 周期汇总

| 阶段 | 内容 | 周期 | 累计 |
|---|---|---|---|
| 1 | 传输层与 Locator | 2 周 | 2 周 |
| 2 | Message 编解码 | 3 周 | 5 周 |
| 3 | Stateful W/R | 4 周 | 9 周 |
| 4 | 发现协议 | 3 周 | 12 周 |
| 5 | QoS 与 DDS 层 | 3 周 | 15 周 |
| 6 | 互操作与调优 | 3 周 | 18 周 |

**总计约 18 周(4.5 个月), 按 1 名有 RTPS 经验的工程师全职估算。**

若团队无 RTPS 经验, 建议**乘以 1.5 ~ 2 倍**(27 ~ 36 周)。规范本身约 180 页,
且大量细节隐含在"与其他实现的实际行为"中而非文字里 —— 这部分只能靠抓包对比补齐。

### 5.2 代码量估算

| 模块 | 新增行数(估) |
|---|---|
| Message 编解码 | ~3000 |
| Stateful Writer/Reader | ~4000 |
| 发现协议 | ~3000 |
| QoS + DDS 层 | ~2500 |
| 传输层改造 | ~1000 |
| 测试 | ~4000 |
| **合计** | **~17500** |

### 5.3 主要风险

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 互操作调试耗时超预期 | **高** | 周期 +50% | 阶段 2 就用 Wireshark 验证, 不等到阶段 6 |
| QoS 兼容性静默失败 | **高** | 难以定位 | 匹配失败时输出详细原因日志 |
| 重传风暴 | 中 | 性能崩塌 | 阶段 3 即实现 nackSuppression + MaxQueuedRexmit |
| WHC 内存失控 | 中 | OOM | ResourceLimits 强制兜底 + 水位告警 |
| 性能低于现有实现 | 中 | 收益倒挂 | 阶段 3 末尾即做性能对比, 不合格就停 |
| 范围蔓延 | **高** | 无法收敛 | 2.1 节的排除清单要严格执行 |

### 5.4 何时应该中止

明确的中止条件, 避免沉没成本效应:

- 阶段 2 结束时 Wireshark 仍无法解析 → 线格式理解有根本偏差, 重新评估
- 阶段 3 结束时性能低于现有实现 50% → 收益倒挂, 中止
- 阶段 4 耗时超过 6 周仍无法与任一第三方实现互通 → 重新评估互操作目标

---

## 6. 与现有实现的关系

### 6.1 并存策略

**不替换, 而是并存。** 现有 `socket_pub_sub_ipc` / `socket_ser_cli_ipc` 保留,
新增 RTPS 传输类型:

```cpp
enum IPCType {
    IPC_SHM,        // 现有, 不动
    IPC_SOCKET,     // 现有, 不动
    IPC_RTPS,       // 新增
};
```

理由:
1. 现有实现在目标场景下性能更好(无 Heartbeat 开销、无 GUID 开销)
2. 迁移风险可控, 出问题能立即回退
3. SHM 路径完全不受影响

### 6.2 可复用的部分

| 现有组件 | 复用方式 |
|---|---|
| `crc32c` | 直接复用 |
| RTT 自适应 (`ack_first_wait_ms`) | 移植为 `heartbeatResponseDelay` 的自适应 |
| 分片丢失诊断 (`FragmentLossStats`) | 直接复用, 换成按 SN 统计 |
| 发送节流 (`rate_limit_bps`) | 演进为 WHC 高低水位流控 |
| 缓冲截断警告 | 直接复用 |
| benchmark 框架 | 扩展 `--transport=rtps` |
| `IpcInfoPool` | 保留, 作为同机快路径 |

### 6.3 不可复用的部分

- `IpcMsgBase` 的 tail 机制 → RTPS 用 Submessage header
- `IpcRtpsAckMsg` / `IpcRtpsNackMsg` / `IpcRtpsNackBitmapMsg` → 换成标准
  `ACKNACK` / `NACK_FRAG`
- `IpcRtpsHeartbeatMsg` → 换成标准 `HEARTBEAT` / `HEARTBEAT_FRAG`
- 现有分片重组逻辑 → 换成基于 `WriterProxy` 的按 SN 重组

> **注**: 轻量路径新增的四个控制帧(`DZAK`/`DZNK`/`DZNB`/`DZHB`)在线格式层面都是
> dzIPC 私有的, 做完整 RTPS 时**一定会被替换**。可复用的是它们背后的**机制经验**:
> 位图 gap 集合的编码方式(→ `SequenceNumberSet`)、能力协商位的做法、以及
> "Heartbeat 是 sequence 的唯一载体"这个教训(§7.4)。§8.3 说轻量路径"不会成为
> 技术债"指的是这一层, 不是指代码能直接搬过去。

---

## 7. 数据依据

本报告的判断基于 2026-08-05 的实测数据:

### 7.1 丢包形态

```
1 MB payload, 默认 rmem_max=208KB:
  期望=50330 缺=255  空洞=1   平均长度=255.00  最长=255
  期望=51049 缺=249  空洞=2   平均长度=124.50  最长=234
```

**结论**: 缓冲溢出型突发丢包, 不是随机丢包。这否定了 FEC 方向(交织 XOR 抗突发
上限 16~32 片)。

> **补充 (2026-08-06)**: 本节此前还写了"**当前的 NACK 列表机制在真实场景下够用**,
> 不需要位图压缩"。那个判断只在 `rmem_max` 已调到 64 MB 时成立, 而**恰恰在没调好
> 缓冲的场景下不成立** —— 也就是用户最指望 Reliable 救场的时候。
>
> 上面这组实测数据自己就说明了问题: 空洞长度 255 和 249, 而
> `IpcRtpsNackMsg::kMaxMissingPages` 正好是 256。**单个突发空洞就几乎顶满整个
> NACK 容量**, 再多一片就要拆成两轮, 每轮多付一个 `round_wait_ms`(713 片时
> 200 ms)。这不是"残余丢包极少所以够用", 而是"刚好卡在悬崖边上"。
>
> 位图把 255 片连续空洞的编码从 510 B 降到 32 B, 并让 713 片全缺也能一帧报完。
> 它的价值是**鲁棒性兜底**, 不是稳态性能 —— 稳态下确实用不上, 但轻量路径把它
> 排在第 3 位的理由应当是这个, 而非原文的"用不上"。

### 7.2 性能基线

| 场景 | rmem_max=208KB | rmem_max=64MB |
|---|---|---|
| ser-cli 64 KB | 903 msg/s, 112.9 MB/s | 同左 |
| ser-cli 256 KB | 224 msg/s, 111.9 MB/s | 同左 |
| ser-cli 1 MB | **66.67% 丢包** | **~110 MB/s, 零丢包** |
| pub-sub 1 MB | 高丢包 | 零丢包 |

**结论**: 系统参数是瓶颈, 不是协议。这是"要不要做 RTPS"这一决策的核心依据。

### 7.3 组播 loopback 约束

```
pub-sub 启用 Reliable: 吞吐塌到 1 msg/s, 丢包率 0.00%
```

数据面通、确认面坏 —— 自发分片回绕堵在 ACK 之前。ser-cli 改用
`chunk_send_reliable` 后全尺寸失败, 同一原因。

**结论**: 这是第 3.3 节"端点分离"的直接依据。

### 7.4 Reliable 失效的第二个原因: ACK 的 sequence 恒为 0

实施端点分离时发现的独立缺陷, 之前一直被 loopback 掩盖:

- 发送端 `chunk_send_ex` 用逐条自增的计数器填 `meta.sequence`, 并要求
  `ack_msg.sequence == meta.sequence`。
- 接收端在 `wait_first_data_chunk` 里构造 `chunk_meta{page_cnt, total_size,
  msg_id}` —— **只有三个字段, `sequence` 默认为 0**。12 字节 tail 里根本没有
  承载 sequence 的位置。

于是 `send_ack` 永远回 `sequence = 0`, 校验只有进程发出的**第一条**消息
(计数器恰好为 0)能通过, 之后永远对不上。

**这意味着只做端点分离修不好 Reliable。** Heartbeat 因此不只是延迟优化 ——
它是线格式里唯一能把 sequence 送到接收端的载体, 是可靠投递的必要条件。
两项改动必须一起上。

### 7.5 Reliable 失效的第三个原因: 发送端 CRC 算在了页号纠正之前

前两层修好、ACK 真正能到达之后暴露出来的第三层。症状是 Reliable + CRC32C 的
**多页**消息 100% 报 `FailedIntegrity`(status 4), 而丢包率 0.00%、分片全部收齐。

链路是这样的:

1. `IpcMsgBase::adapt_memcpy_tos` 写 tail 时是**先 `++page` 再 `add_tail_msg`**,
   于是 `serialize()` 产出的页号是 `[2,3,4,...,N,N]` —— 差一。
2. `chunk_send_ex` 把 `publish_data` 切成 chunk, 而 chunk 是
   **`publish_data` 的非拥有视图**(`buffer(ptr, size)` 构造)。
3. 随后的 `write_now_page(chunks[i], i+1)` 就地把页号纠正成 `[1,2,...,N]` ——
   它改的是 `publish_data` 本身。
4. 而 CRC 原先算在第 2 步**之前**, 校验的是一份"从未上过线"的字节流。

接收端 `place_page` 把整片(含 tail)原样拼进 `assembled` 再 CRC, 拿到的是线上真实
字节。两端在每片 2 个字节上不同, N 片差 N-1 处, 必然不等。

**为什么一直没被发现**: 端点分离之前 ACK 到不了发送端, 代码在比较 CRC 之前就
`FailedTimeout` 返回了; 单页消息则因为 `page` 从未自增而无需纠正, 天然一致 ——
`test_socket_reliable_crc` 原有的两个用例都走这两条路径, 所以一直是绿的。

**修复**: 把 CRC 计算移到 `write_now_page` 循环之后。回归用例
`SocketReliable.ReportedCrcMatchesWireBytes` 利用"`chunk_send_ex` 就地改写入参"
这一点, 直接断言 `report.crc32c == crc32c(调用后的 data)` —— 不依赖页号是否差一,
将来 `adapt_memcpy_tos` 被修正了本用例依然成立。

**这三层的共同规律值得记下来**: 每修好一层, 下一层才第一次有机会执行。所以
"改完就能用"的判断在这类分层协议里不成立, 必须靠端到端实测逐层剥。

---

## 8. 轻量替代路径(推荐优先评估)

如果 2.1 节的四条触发条件都不成立, 建议先做这条路径 —— **约 4 周, 拿到完整
RTPS 60% 的收益**。

### 8.1 内容

| 项 | 说明 | 周期 | 状态 |
|---|---|---|---|
| **端点分离** | 阶段 1 的核心部分, 解决 ACK 阻塞 | 1 周 | ✅ **已落地 (2026-08-06)** |
| **Heartbeat 机制** | 发送端发完主动通告 `(page_cnt, sequence)`, 接收端据此判断是否需要 NACK | 1 周 | ✅ **已落地 (2026-08-06)** |
| **NACK 位图压缩** | 缺失片数多时切换位图编码 | 0.5 周 | ✅ **已落地 (2026-08-07)** |
| **NACK 抑制** | 多订阅者场景下防重传风暴 | 0.5 周 | 🟡 **部分完成** |
| **WHC 高低水位** | 把现有 `rate_limit_bps` 演进为闭环流控 | 1 周 | 未做 |

**验证状态 (2026-08-07)**: 
- **单元测试**: 18 个全部通过 (`test_socket_endpoint_split` 12 个 + 6 个位图择优判据用例)
- **性能回归**: BestEffort 吞吐无回退，与基线 (commit `0dd0b0a`) 一致
- **Reliable 端到端**: 修复 CRC 时序 bug 后，1 MB 载荷跑到 110 MB/s (与 BestEffort 同量级)，
  p50 延迟 8.8 ms，0% 丢包。pub-sub Reliable 从 1 msg/s 提升到线速。
- **sniffer 兼容**: `chunk_rev_sniff` 已加白名单，控制帧不会污染 `dzipc_topic_cat` 输出

**NACK 抑制标"部分"而非"未做"的原因**: 
- ✅ **跨订阅者去重**: 已作为端点分离的副产品存在 —— 发送端的 `missing_union` 是
  `unordered_set`, 一轮内 N 个订阅者 NACK 同一分片只会重传一次。
- ❌ **跨轮抑制**: RTPS 的 `nackSuppressionDuration` 未实现，同一分片 ACK 超时后，
  第二轮仍会响应 NACK 重传。但这只影响失败率高的场景，与"多订阅者 N 倍放大"不同。
- ❌ **单播重传**: 重传仍走组播，已收到该片的订阅者会再收一遍。浪费带宽但不放大。

**NACK 位图压缩补充 (2026-08-07)**:
- **择优判据修正**: 原逻辑"缺片数 > 256 就无条件用位图"在稀疏散布 + 超大消息时会失效
  (45000 片消息丢 300 片均匀散布，位图窗口只罩 77 片，列表能报 256 片)。改为
  **先比覆盖率、再比字节数**，6 个单元测试钉住所有边界形态。
- **可观测性**: 新增 `nack_explicit_sent` / `nack_bitmap_sent` / `*_truncated` 四个计数器，
  解决"零丢包与位图坏了"无法区分的问题。`--frag-stats` 输出示例:
  ```
  [NACK] 显式列表=12 位图=47  截断[列表=0 位图=3]
  ```
- **诊断维度**: `*_truncated` 持续有量说明该考虑分多帧发，而不是等下一轮 (每轮 200 ms)。

**已落地部分的实现要点**(与原设想的差异):

- 端点分离用的是"**发送专用 socket 不加入组播组**", 不是原文写的
  `IP_MULTICAST_LOOP=0` —— 后者是主机级开关会误伤其他进程, 详见 §3.3 勘误。
- ACK/NACK 移到独立端口(`+3` / `+4`), 数据端口 `+0` / `+1` 保持不变, 以免
  `dzipc_topic_cat` 失聪。
- Heartbeat 顺带修掉了 §7.4 的 sequence 缺陷 —— 这是当初没预料到的, 但它让
  这两项从"优化"变成了"Reliable 可用的必要条件"。
- NACK 位图用**独立 msg_id `DZNB`**, 不是在 `DZNK` 里加格式位。原因见下。
- 位图/列表的切换判据是**线格式字节数**(`span/8` vs `2N`), 不是原文写的
  "缺失片数 > `page_cnt/8`"。密度判据在稀疏丢包时会误判: 713 片丢 100 片密度
  14% 已过阈值, 但若那 100 片散布全域, 窗口跨度就是 713 位 = 90 B, 与显式列表
  的 200 B 差别不大。直接比较两种编码的实际字节数即可, 不必用密度去近似。

**NACK 位图的两个非显然约束**(都是正确性要求, 不是优化):

1. **必须用独立 msg_id。** 若在 `DZNK` 里加一个格式位, 旧版发送端的
   `IpcRtpsNackMsg::deserialize` 会把该位当作 `miss_cnt` 的高字节, 读出一个巨大的
   数, clamp 到 256 之后仍会从一个只有几十字节的 buffer 里读 512 字节 ——
   `adapt_memcpy_tods` 不做边界检查, 那是一次真实的越界读。独立 msg_id 让旧版
   `check_id` 直接失配, 安全地忽略整帧。
2. **必须协商能力, 不能无条件发。** 旧版发送端收到 `DZNB` 会丢弃, 那一轮等于没发
   NACK —— Reliable 从"能重传"退化成"必然超时", 比多花几百字节严重得多。
   协商位是 Heartbeat 的 `kFlagBitmapNack`; 收不到该标志就用显式列表。

**位图放大了重传突发, 因此重传批次也必须限速。** 显式列表时代一轮最多重传 256 片
(约 377 KB); 位图把上限提到 11496 位, 713 片的消息现在可以一轮全报。若仍无节制地
灌回去, 就会把当初造成丢包的那个接收缓冲(§7.1 的溢出型突发)再撑爆一次, 重传本身
变成下一轮丢包的成因。实现上复用了首轮的 `rate_limit_bps`。

### 8.2 收益对比

**注意: "轻量路径全做完"一列是目标态, 不是当前状态。** 当前进度见 §8.1 的状态列。

| 能力 | 改动前 | 已落地三项之后(2026-08-07 实测) | 轻量路径全做完 | 完整 RTPS |
|---|---|---|---|---|
| 大包可靠传输 | ✅(靠扩缓冲) | ✅ | ✅ | ✅ |
| pub-sub 可靠模式可用 | ❌ 1 msg/s | ✅ **110 MB/s, 见下方语义边界** | ✅ | ✅ |
| 突发丢包下的 NACK 表达力 | ❌ 上限 256 片 | ✅ 11496 位/帧 | ✅ | ✅ |
| 高丢包链路 | ⚠️ | ⚠️ | ✅ | ✅ |
| 多订阅者不风暴 | ❌ | 部分(轮内去重) | ✅ | ✅ |
| 闭环流控 | ❌(开环限速) | ❌(仍是开环) | ✅ | ✅ |
| 晚加入者补历史 | ❌ | ❌ | ❌ | ✅ |
| 第三方 DDS 互操作 | ❌ | ❌ | ❌ | ✅ |
| 标准 QoS | ❌ | ❌ | ❌ | ✅ |
| **周期** | — | 已花约 2.5 周 | **4 周** | **18 周** |

**实测数据 (2026-08-07, pub-sub socket, Reliable + CRC32C)**:

| payload | 吞吐 | p50 延迟 | 丢包 |
|---|---|---|---|
| 65536 B | 112.7 MB/s | 334 us | 0.00% |
| 262144 B | 113.4 MB/s | 1306 us | 0.00% |
| 1048576 B | 109.9 MB/s | 8800 us | 0.00% |

改动前同一路径是 ~1 msg/s。BestEffort 吞吐与基线(commit `0dd0b0a`)一致, 无回退。

**"pub-sub 可靠模式"仍有一个语义边界** —— 轻量路径固有, 不是漏做:

`chunk_send_ex` 的 ACK 等待循环收到**任意一个**订阅者的 ACK 就返回
`DeliveredAcked` (`data_rev.cc` 置位后 `break`, 随即 return)。
N 个订阅者时它的含义是"**至少一个**收到", 不是"全部收到"。

标准 RTPS 靠 `ReaderProxy` 逐个跟踪 `highestAckedSN`, 只有全部匹配的 Reader 都
确认才算投递完成 —— 而 `ReaderProxy` 在 §3.2 里属于阶段 3, 明确不在轻量路径内。
当前架构也没有按 topic 的订阅者集合信息(`IpcInfoPool` 有进程列表, 但没有端点
匹配), 所以这是轻量路径能力的天花板。

影响面: ser-cli 是 1:1, 不受影响; pub-sub 是 1:N, `publish_blocking()` 的返回值
应理解为"至少一个订阅者已确认"。若业务需要"全体确认"语义, 只能走完整 RTPS 的
阶段 3。

> **压测时的一个已知噪音 (2026-08-07)**: 若把 `dzipc_perf_benchmark` 的
> `use_blocking` 改成对 socket 也生效, 每个用例会打印 1~2 行
> `Reliable publish failed with status 2`(FailedTimeout)。那是**握手探测期**的
> 正常现象 —— 订阅端是 fork 出的子进程, 加入组播组需要时间, 探测循环本来就靠
> 失败重试来等对端就绪(见 `dzipc_perf_benchmark.cpp` 的 `probe_ok/probe_fail`)。
> 判据: 错误条数是每用例个位数而非每条消息一条, 且稳态吞吐正常, 就不是缺陷。
> 稳态下 1 MB 的 p50 是 8.8 ms, 而默认 `--pub-timeout=100`, 有 10 倍余量。

### 8.3 建议

**先做轻量路径, 同时观察是否真的出现互操作需求。**

轻量路径的每一项都是完整 RTPS 的子集或前置(端点分离就是阶段 1, Heartbeat 和
NACK 抑制是阶段 3 的组成部分), 不会成为后续做完整 RTPS 的技术债 —— 是"提前完成
一部分", 而不是"走了弯路"。

---

## 9. 决策建议

```
需要与第三方 DDS 互操作?
├── 是 → 执行完整路线(18 周)
└── 否 → 需要跨广域网 / 高丢包?
         ├── 是 → 执行轻量路径(4 周), 之后重新评估
         └── 否 → 需要 Durability(晚加入者补历史)?
                  ├── 是 → 单独实现历史缓存, 不必做完整 RTPS
                  └── 否 → 维持现状 + 扩缓冲即可, 本报告存档备查
```

**当前(2026-08-07)的判断**: 依据 7.2 的数据, 扩缓冲后现有实现已满足目标场景,
完整 RTPS 仍建议**维持现状**。8.1 的前三项(端点分离 + Heartbeat + NACK 位图压缩)
已落地并实测通过 —— pub-sub 的 Reliable 模式从 1 msg/s 变成 110 MB/s 线速,
这是现有实现最明显的功能空缺, 现已填上。

**已完成的验证 (2026-08-07)**:

1. ✅ BestEffort 吞吐无回退, 与基线 commit `0dd0b0a` 一致。
2. ✅ pub-sub Reliable 实测 110 MB/s / 0% 丢包 (基线 1 msg/s), 三个 payload 档位见 §8.2。
3. ✅ 单元测试 18 + 4 全绿。

**验证过程中剥出的第三层缺陷**: 发送端 CRC 算在页号纠正之前(§7.5)。这一层只有在
前两层都修好之后才第一次有机会执行 —— 前两层没修时代码走不到 CRC 比较那一行。

**仍未验证的一项**: 位图 NACK 在真实丢包下的触发。刚加的
`nack_bitmap_sent` / `nack_explicit_sent` 计数器就是为此 —— 但要制造丢包,
`--rate-limit` 是**减少**丢包的, 应该反过来把 `rmem_max` 调回默认值:

```bash
sudo sysctl -w net.core.rmem_max=212992   # 默认值, 会造成缓冲溢出
./build/bin/dzipc_perf_benchmark --cases=pubsub_socket \
    --payloads=1048576 --duration=5 --frag-stats
```

期望看到 `[NACK] 位图=` 有量, 且空洞平均长度显著大于 1(§7.1 的突发形态)。

**剩下两项的启动顺序**: NACK 抑制与 WHC 闭环流控**都依赖同一个当前不存在的前置 ——
对端身份表**。`receiver_id` / `sender_id` 字段虽然四处填写, 但发送端从未读取
(ACK/NACK 校验只比 `page_cnt / total_size / data_msg_id / sequence`)。没有它:

- 无法做跨轮 NACK 抑制(不知道 NACK 来自谁)
- 无法单播重传(只能继续组播, 已收到的订阅者会再收一遍)
- 无法按对端算"未确认字节数", 闭环流控没有计算基准
- `publish_blocking()` 只能是"至少一个确认"(§8.2)

所以剩下两项不是并列的两块工作, 而是 **1 个前置 + 2 个建立其上的功能**。
建议先花 2~3 天把 `receiver_id` 读起来、维护一张 `topic → 已知订阅者` 的表,
再动后两项。这张表在 RTPS 里就是 `ReaderProxy`, 属阶段 3 —— 也正是轻量路径与
完整 RTPS 之间最实质的那道坎。

---

## 附录 A: 参考资料

- **OMG RTPS 2.3 规范**: https://www.omg.org/spec/DDSI-RTPS/2.3/
- **OMG DDS 1.4 规范**: https://www.omg.org/spec/DDS/1.4/
- **Eclipse Cyclone DDS 源码**: https://github.com/eclipse-cyclonedds/cyclonedds
- **eProsima Fast DDS 源码**: https://github.com/eProsima/Fast-DDS
- **Wireshark RTPS dissector**: 内置, 过滤器 `rtps`

## 附录 B: 关键规范章节索引

| 主题 | 规范章节 |
|---|---|
| Message 结构 | 8.3.3 |
| Submessage 定义 | 8.3.7 |
| SequenceNumberSet | 8.3.5.5 |
| Stateful Writer 行为 | 8.4.9 |
| Stateful Reader 行为 | 8.4.12 |
| SPDP | 8.5.3 |
| SEDP | 8.5.4 |
| 端口计算 | 9.6.1.1 |
| QoS 兼容性 | DDS 1.4 §2.2.3 |
