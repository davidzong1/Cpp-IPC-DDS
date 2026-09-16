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
| **NACK 抑制** | 多订阅者场景下防重传风暴 | 0.5 周 | ✅ **已落地 (2026-08-07)** |
| **WHC 高低水位** | 重定标为闭环自适应限速 (DCTCP, per-node) | 1 周 | ✅ **已实现并完成标定** |

**验证状态 (2026-08-08)**: 
- **单元测试**: 35/35 全部通过 (reliable_crc 4 + dzipc_socket 2 + endpoint_split 18 + nodelet 11), tester 已两次独立复现
- **性能回归**: BestEffort 吞吐无回退，与基线 (commit `0dd0b0a`) 一致
- **Reliable 端到端**: 修复 CRC 时序 bug 后，1 MB 载荷跑到 110 MB/s (与 BestEffort 同量级)，
  p50 延迟 8.8 ms，0% 丢包。pub-sub Reliable 从 1 msg/s 提升到线速。
- **sniffer 兼容**: `chunk_rev_sniff` 已加白名单，控制帧不会污染 `dzipc_topic_cat` 输出

**NACK 抑制的三条组成** (跨轮抑制已落地, 单播重传仍未做):
- ✅ **跨订阅者去重**: 发送端的 `missing_union` 是 `unordered_set`, 一轮内 N 个
  订阅者 NACK 同一分片只会重传一次。
- ✅ **跨轮抑制 (2026-08-07 落地)**: 发送端维护**调用内局部** serving history
  (每页最近一次重传的轮次, K=1 窗口), 上一轮重传过的页本轮不再发。无跨消息
  状态、不进身份表 (D-1 纪律, 表仍 3 字段 + ack_count)。真实 1:N 场景因
  at-least-one 语义**低触发**: 2 订阅者实测 `nack_suppressed=0` —— 多数 peer
  输掉首匹配 ACK 竞争, 其 NACK 是否驱动重传轮具有偶发性。**口径: 确定性场景
  (哑接收端) 证明机制正确 —— 单次会话 `nack_suppressed=689`, 抑制轮 drain 后
  仅剩 2 帧 HB, 证实"抑制数据、照发 HB"路径成立; 真实负载下按需生效。既非
  "已验证普遍生效", 也非未实现; 验收判据锚定确定性场景, 不锚 1:N 计数。**
- ❌ **单播重传**: 重传仍走组播, 已收到该片的订阅者会再收一遍。浪费带宽但不放大。
  **卡在传输层, 不在逻辑层** —— 当前架构在三条链路上都不具备条件: ① 发送端拿
  不到对端单播地址 (ack 通道 `recvfrom` 丢弃源地址, 身份表只存 `receiver_id`,
  没有地址字段); ② 即使拿到, 订阅端 ACK 源端口是临时的 (`ack_tx_` 为 SendOnly
  不 bind, `UDPNode::connect` 让内核分配临时端口, 进程内都不稳定); ③ 订阅端没有
  发送端可知的单播接收端点 (唯一已知端口是组播端口, SO_REUSEPORT 共享, 同机
  多订阅者歧义)。落地需跨三层改造: libipc 传输层 (receive 取源地址、SendOnly
  支持 bind 固定 ACK 端口并通告)、报文层 (ACK/HB 携带单播 locator 字段)、身份表
  (D-1 字段冻结的反转: 需加 addr/port 字段, 并新增 per-peer 页级缺片表 ——
  `highest_acked_seq` 只到消息级)。估计 2–3 个工作日, 且触碰已结项回归面 (身份
  表、drain、抑制、端点分离)。**"身份表是必要不充分条件": 它回答了"谁在报缺",
  回答不了"在哪发" (地址) 与"发什么" (per-peer 页级进度)。**

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

| 能力 | 改动前 | 已落地四项之后(2026-08-08) | 轻量路径全做完 | 完整 RTPS |
|---|---|---|---|---|
| 大包可靠传输 | ✅(靠扩缓冲) | ✅ | ✅ | ✅ |
| pub-sub 可靠模式可用 | ❌ 1 msg/s | ✅ **110 MB/s, 见下方语义边界** | ✅ | ✅ |
| 突发丢包下的 NACK 表达力 | ❌ 上限 256 片 | ✅ 11496 位/帧 | ✅ | ✅ |
| 高丢包链路 | ⚠️ | ⚠️ | ✅ | ✅ |
| 多订阅者不风暴 | ❌ | ✅ 跨轮抑制已落地(1:N 场景低触发, 见 §8.1) | ✅ | ✅ |
| 闭环流控 | ❌(开环限速) | ✅ 自适应限速已实现并完成标定 (DCTCP, per-node, 见 §8.1) | ✅ | ✅ |
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

> **成立条件 (2026-08-08 复核)**: 以上三个吞吐数字在 `net.core.rmem_max ≥ 1 MB`
> 下测得 (测试环境实际为 1048576)。tester 复核同链路: `rmem_max=1048576` 时
> 113.3 MB/s, `rmem_max=212992` (默认) 时 50.2 MB/s —— 吞吐受接收缓冲容量支配,
> **缓冲条件不同的数字不可并列比较**。报告或压测引用的每个吞吐数字必须同时声明
> `rmem_max`。

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

**对端 ACK 记账的边界 (2026-08-08, drain 记账口径)**: 为把"至少一个确认"尽量补全,
发送端拿到首匹配 ACK 后**不额外等待**, 只在返回前顺带收干已到达的 ACK
(`drain_record_acks`)。实测: 54 条消息中发布端捕获到 40 条 (74%) 的部分对端 ACK,
身份表 `ack_count` 合计均值 23.2, 为旧版固定值 9 的 2.6 倍; 但受内核组播唤醒
序列化影响非确定性, 钉核配置下可回落至 12–17。**口径: 这是"到达时已可读的对端
ACK 的尽量捕获", 不是"完整记录所有对端"** —— 首匹配 ACK 之后到达的迟到 ACK 能否
入账, 取决于返回前的 drain 窗口, 该窗口不等待、不保证收干 (wait-for-all 已被
D-3 拒绝, 异步多消息版超出轻量路径范围)。

> **压测时的一个已知噪音 (2026-08-07)**: 若把 `dzipc_perf_benchmark` 的
> `use_blocking` 改成对 socket 也生效, 每个用例会打印 1~2 行
> `Reliable publish failed with status 2`(FailedTimeout)。那是**握手探测期**的
> 正常现象 —— 订阅端是 fork 出的子进程, 加入组播组需要时间, 探测循环本来就靠
> 失败重试来等对端就绪(见 `dzipc_perf_benchmark.cpp` 的 `probe_ok/probe_fail`)。
> 判据: 错误条数是每用例个位数而非每条消息一条, 且稳态吞吐正常, 就不是缺陷。
> 稳态下 1 MB 的 p50 是 8.8 ms, 而默认 `--pub-timeout=100`, 有 10 倍余量。

**轻量路径闭合度 (2026-08-08, 段5 审查补记 2026-08-10): 4.5/5, 不是 5/5。** 端点分离 ✅、
Heartbeat ✅、位图 NACK ✅、闭环限速 ✅ (DCTCP 已实现并完成标定, 见 §8.1) 四项落地;
NACK 抑制 🟡 (跨轮抑制已落地, **单播重传未做**, 卡在传输层, 见 §8.1); 另两条未闭合:
**下限告警②构造不可达未实测** (机制性不可达, 非缺陷, 见下) 与 **吞吐硬判据未达**
(见下「未闭环项补记」)。故 4.5/5。**不再以「WHC 极限环」作为 🟡 理由**。

**闭环自适应限速的标定结果 (2026-08-08, `rmem_max=212992` / `wmem_max=212992`,
1 MB 载荷, 200 条消息 × 开环/闭环对照)**:

| 判据 | 结果 |
|---|---|
| NACK 总量 闭环 ≤ 开环 | ✅ 13 vs 195 (降到 6.7%) |
| 送达率不降 | ✅ 200/200 vs 200/200 |
| 重传字节占比 ≤10% | ✅ 0.28% vs 开环 6.30% (降 22×) |
| 速率收敛 | ⚠️ **未收敛到内部定值: 新×新 5/5 无 100M↔200M 极限环 (旧律固有形态已由 E 消除), 但稳态为天花板 200M 微锯齿、内部不动点未现 (稳态端点仍 {floor, initial})** |

**性能方差带 (2026-08-08, tester 中位数 + min/max, 同条件对照)** —— 上表及段3
的全部数字均为**单次采样**; tester 以 ≥5 次 (开环 ≥3 次) 复测给出以下方差带,
**终稿一律以带宽为准, 段3 单次数字仅供对照、不作结论依据**:

| 指标 | 闭环 | 开环 | 比值 |
|---|---|---|---|
| 吞吐 (msg/s) | 89.81 (中位数) | 40.48 (中位数) | **2.2×** |
| p50 延迟 (ms) | 9.08 (中位数) | 24.69 (中位数) | **2.7×** |
| p99 延迟 (ms) | 29.82 (29.52~31.21, ×5) | 26.12 (25.93~28.40, ×3) | 持平至略差 (0.88×) |
| 重传占比 | 0.28% | 6.30% | 降 22× |

**收益口径 (2026-08-08)**: 闭环限速的真实收益是**吞吐与 p50** —— 吞吐 2.2×、
p50 2.7×、重传占比降 22×, 都是硬收益。**p99 如实写: 持平至略差**(闭环 29.82 ms
对开环 26.12 ms, 0.88×) —— 闭环的收益不在尾延迟。

**段3 开环 p99 222.6 ms 的裁定 (2026-08-08, tester)**: 段3 采到的开环 p99
222.6 ms 与本次**同条件**测得的 26.12 ms 矛盾; tester 复测 3/3 均落在 26 ms 量级
⇒ **222.6 ms 判定为单次长尾异常样本, 非第二个模态(双峰)**。段3 其余数字
(NACK 13 / 重传 0.27% 等)均落在本次方差带内。**此前"p99 改善 7.4× / 延迟收益
显著"的表述已撤销** —— 那是把单次长尾异常当成了开环基准, 闭环 p99 实际略差于
开环; "开环的长尾来自反复撑爆接收缓冲"同样不成立 (222.6 ms 不是常态)。

**已知行为: 闭环限速 (DCTCP) 的形态 (段5 审查补记 2026-08-10 重写, 原「MIMD 极限环」
段不反映当前控制律)**:
- ① **控制律已是 DCTCP 式比例降 + 加性升** (`adapt_rate_dctcp`, data_rev.cc:725;
  :566-587 注释明写 DCTCP) —— 无 ×1.5 增倍 / ×0.5 减半, 段3 的 MIMD 律在当前树
  **已不存在**。
- ② **旧律 100M↔200M 极限环的历史**: 曾用**纯旧二进制 (commit `1431622` 本身)**独立
  复现 (1 MB 载荷, `rmem_max=212992`, 200 条消息分布 **134×200M + 33×100M +
  33×150M**, "3 条 150M → N 条 200M → 溢出减半 100M"循环)。定性「段3 固有行为」作为
  **历史事实**成立, 但只描述段3 实现, **不再描述当前实现**。
- ③ **当前残留形态 (新×新)**: 5/5 轮无 100M↔200M 极限环, 但**内部不动点未现** ——
  稳态端点仍 {floor, initial} 两态, 稳态为天花板 200M-STEADY 微锯齿 (208.5~209.7M,
  NACK>0)。E 消除的是"往复", 没有造出"内部定值"。
- ④ **代价**: 天花板稳态 + rmem=212992 小缓冲 → 持续溢出 NACK → 持续重传与逐轮等待
  → **吞吐硬判据未达** (见下「未闭环项补记」)。
- ⑤ **立项理由已兑现、收敛判据未达成**: 方案 E (段4: 接收端显式拥塞反馈) 已**实现**
  (非"已立项"; data_rev.cc:725 + DZA2 报文, udp_rtps_ack_msg.hpp:371-412), "消除往复"
  已兑现; 但「速率收敛到内部定值」判据**仍未达成**。

**未闭环项补记 (段5 审查补记, 2026-08-10) —— 报告原缺记的两条未闭环项**:

> **未闭环项 1 — 吞吐硬判据未达** (`rmem_max=wmem_max=212992`, 1 MB×200 条, 200M,
> 5 轮, retrans 取 msg199 累计口径, 中位数+min/max 方差带): 闭环 (新×新) 吞吐 msgps
> **中位 84.39 < 判据 85.3** (=89.81×0.95), **4/5 轮低于**; 跨轮佐证: 08-08 任务6 中位
> 79.0 (76.53~94.84), 08-10 复现中位 74.53 (5/5 轮低于)。送达 5/5 轮 200/200 全无损
> (丢的是吞吐判据, 不是数据)。归因: E 以「无内部不动点 → 天花板稳态」为代价消除
> 极限环; 天花板稳态 + 小缓冲 → 持续溢出 NACK → 持续重传 (retrans 中位 2.88MB =
> 3,024,960B, 为旧×旧 0.45MB = 468,096B 的 6.5 倍)。
> **归因订正 (leader 裁定采信, 非已定案; 段5, 2026-08-12)**: **retrans 翻倍是相关
> 而非因果, 不解释吞吐差** —— retrans 字节直接重发耗时仅占 Δwall **~0.8%** (+2~3 ms);
> Δmsgps=10.81 由 wall +0.305s (+13.7%) 完全解释, 而 wall 增长**全部来自等待时间
> +0.562s** (占比 43.4%→60.4%), NACK 事件数持平 (18 v 18)。⇒ 修复方向 = 降等待 /
> 提 ACK 效率 / 允许接收侧间歇排空, 且与 P-1 联动 (修 P-1 让速率离开天花板 → 间歇
> 排空 → msgps 可能自然回升)。重传批同按 rate_limit 限速
> (data_rev.cc:2288-2298) → 逐轮 round_wait 死等 → msgps 被拖低。DZA2 每消息开销
> (全帧仅 ~37B) 是真实但次要的 ACK 通道成本, **不是 retrans 放大来源** (retransmit_bytes
> 只计重传片载荷, data_rev.cc:2317)。

> **未闭环项 2 — 1% 丢包零回升** (`rmem_max=1048576`, rate=100M, 页级注入 1%,
> 500 条): 速率**单调滑落 100M→38.4M (−61.5%)**, **red=489 / inc=0 全程零回升**。
> ⚠️ **38.4M 是 pre-R1(改前)末值; R1 后同构造末值 = 31.88M** —— 两值之差是**代码态
> 差异, 不是 run 间方差**, 引用勿混 (段5 订正, 2026-08-12)。单向逼近 floor
> (**31.5 MB/s**, 713 页主场景; 31,883,520 是 e_full 722 页场景, 勿并列); 按几何衰减
> 投影 **593 条触底 (推断**: 0.99805^k×100M=31.56M → k≈593, 依赖 α_eff≥1 + inc=0
> 两前提) —— **但 500 条主场景停在 38.4M 并未触底, 产品侧全程零告警**: 告警① 仅在
> 钳到 floor 那一刻触发且每节点一次性, 故本场景不产生任何告警, **这是 P-4 可观测性
> 洞**(详见 §8.2 段5 结论区 P-4 立档); **送达 500/500 全无损**
> —— 丢的是速率自适应能力, 不是数据。机制: `scaled_loss_fraction` 定点截断
> (data_rev.cc:658-665) 在 λ=7.1 时仅 ~2.8% 消息视为 clean; K=3 连续 clean 回升门
> (data_rev.cc:767, clean=:2243) 结构性不可达 → 降主导到 floor 钳制。对照: 0.5% (λ=3.5)
> 缓降 11% 平衡 89M (red=143/inc=4>0) 非滑落 —— **滑落是丢包密度的函数, 阈值在
> 0.5%~1% 之间**。

**口径声明: 以上两条的达标口径待改动前后同批次 A/B 对照复核** —— 报告陈述历史用 84.39
主口径; 后续验证改动收益必须以同批次背靠背 A/B + 每轮 loadavg 埋点, 不得拿跨会话绝对值
当基准。

**僵尸订阅者隔离 (场景 D, `rmem_max=1048576`)**: 全部判据通过 —— 挂一个只发 NACK
永不 ACK 的假订阅者, `rate_reductions=0`(其 NACK 全被 liveness 判据过滤, 零降速),
真订阅者 200/200 交付, 吞吐 112.3 vs 112.5 MB/s (0.998×), p99 1.01×。**僵尸订阅者
无法拉低全组速率**, 这是 D-3 那条纪律(进程活着 ≠ 正在收数据)在限速路径上的兑现。

**下限保护 (场景 E, cgroup 限 15% 单核制造深度 CPU 饥饿)**: 速率恰停在
`window_floor` = 31,883,520 B/s 不再下破、永不为 0, 发布端 60/60 正常退出无挂死,
下限告警触发 1 次。实现值与设计公式(31,486,080)及独立冒烟(30,426,240)三方一致, 差 <5%。

> **一条未能实测的兜底路径 (如实记录)**: 下限告警有两条触发 —— ①下限仍持续溢出 NACK,
> ②下限仍超时。实测**只有 ① 触发**(msg23), ② **无法在本机构造出来**: CPU 饥饿限不住
> 内核收包(softirq 不记账到进程 cgroup), 而接收端组装的用户态成本仅 ~5 ms/条(CRC32C
> 走硬件指令), 15% 单核下 p50 仍只有 34 ms, 远不到组装 deadline, 进不了"静默丢弃"区;
> 丢片主因恒为 rmem 缓冲溢出 → 溢出 NACK 恒在 → ① 恒可达。**② 属于代码路径已审查
> (三处超时点均带 `at_floor_timeout`)但未经实测**, 不记为已验证。



**段5 三条闭口最终结论 (2026-08-12)**

**总述: P-1 / P-2 / P-3 三条闭口全部未闭, P-4 只立档。** 四条判据门槛
(**1.05 / 85.3 / initial×0.60 / 0.579**) 全程未动、未重定标。

> **P-1 内部不动点 — 未闭**
> 判据: 5 轮 max/min ≤ **1.05** 且全判 INTERNAL-FIXED-POINT。
> 现状 (实测): **5/5 INTERNAL-DRIFTING (max/min 1.061~1.195) → FAIL**。2k 态速率已
> 离开 200M 天花板、漂入内部区间但未稳定。
> 已排除的候选机制: ④-A (rmem 变体) 不获支持 —— p 确随 rmem 单调变化, 但带宽不随 p
> 单调缩放 (减半反使带变宽 1.119→1.223); ④-E (α_scaled 量化) 粒度 371K ≪ 带幅 15M,
> 只解释带边阶梯; 支柱① (继电器, 试验A) 拆掉仍 >1.05, 只值约 1 个百分点; 支柱②
> (α EWMA 滞后, 试验B) 排除且拉长 α 有害 (INTERNAL-DRIFTING 换成 200M-STEADY 禁入态)。
> 剩余链条 (未测, 可闭性低): 周期 250~300 > 末50 窗口 ⇒ 振幅口径相位依赖。
> 阻塞原因: 测量窗口径在现有数据上**不可执行** (基线五轮均 200 条, 全程暂态; 唯一
> 500 条 trace 为单条) —— **这本身即未闭原因之一**。
> **幅值口径 (推导, 非方差带)**: 1.0868 / 1.0975 为 single long500 推导值,
> **不作正式基准** (单次采样不得当基准); [250,499] 是修正起点, [289,499] 是旧近似且
> 切掉真局部峰 286 (低估、更易 PASS) ⇒ **不得选用 289 作为唯一口径**。正式幅值需
> 500 条 × 5 轮新实验 (挂账, 未做)。双起点「均 >1.05」的未闭方向不受影响。

> **P-2 吞吐 — 未闭 (FAIL)**
> 判据: 中位 msgps ≥ **85.3** (=89.81×0.95)。
> 实测 (n=5, new×new, `rmem=212992`, 1MB×200, 200M, 全 DELIVERED 200/200 零超时):
> **全 5 轮 median 71.82 (66.02~76.71) → FAIL**; 仅 clean 轮 (r1–r3) median
> **74.58 → FAIL**。**如实定性: 明显未达 (差 15.8%), 不是「踩线」**; 两个口径均 FAIL。
> 逐轮 msgps / loadavg(前→后): r1=76.71(1.50→1.46)、r2=74.58(1.46→1.46)、
> r3=68.95(1.46→1.50)、r4=66.02(1.50→1.62 污染)、r5=71.82(1.62→1.65 污染)。
> **前置 probe_window 门未过, 经 leader 裁定以 `--skip-probe` 放行** (32 核机背景抖动
> rel 41%, 该门在低负载下不可满足), 逐轮标注污染 —— 此项如实记录, 不隐去。
> **观测**: 2k 态吞吐中位 71.82 低于任务6 的 79.0, **成因未隔离** (候选至少三条: 2k
> 运行代价 / 负载差异 / 速率形态由 200M-STEADY 变为 INTERNAL-DRIFTING; 无单变量对照)
> ⇒ **不归因**, 转挂账。
> 闭口关系: 正式门槛是 85.3 msgps, **非「P-1 先闭」**; P-2 闭口在**机制上依赖** P-1
> (残余带未消 ⇒ 吞吐判据的稳态前提不成立), 非门槛硬推论。

> **P-3 稀疏丢包 (1% 构造) — 未闭, 归因改判**
> 判据 (档II condF): 末50 中位 ≥ **initial×0.60** (100M 构造 = 60M; 跨构造须用
> initial×0.60, 不得写裸 60M) 且末段严格递减后缀 ≤25 且送达 500/500。
> 判定 (不变): condF **未达**。平衡点 bps* ∈ **[5.0M, 12.48M]** (端点对应 α_eff=1 的
> **上界** 12.48M / α_eff≈2.56 的实测结构值 5.0M), **全部 ≪ floor 31.5M**, 更 ≪
> initial×0.60。**送达 500/500 全无损** —— 丢的是速率自适应能力, 不是数据。
> **机制结论**: 平衡点低于 window_floor ⇒ 回升门只能造出 **floor 之上的钳制游走**
> (回升 +Δ 后立即被更大降幅拉回), **造不出可及带内的内部不动点**。佐证: 39 条消息
> 触发回升 tick 而 `rate_increments` 仅 +9, 其余 30 次被 floor 钳制吸收 (计数门控
> 「仅实际变化时 ++」的预期行为)。
> **归因改判**: 由「结构性不可达」改为「**采集口径实现窄化, 可行路径存在, 本段未
> 落笔**」。实测: runs/lost 只在**位图 NACK 路径**采集 (data_rev.cc:1121/:2350); 1%
> 构造为独立碎片丢 (713 页消息均值 ~7 页散布), span≈700 ≫ 16×miss_cnt ⇒ 接收端按
> 编码效率选**显式 NACK** ⇒ **498/500 消息的 runs/lost 未被采集** (三轮恒
> bitmap=39 / explicit=498)。⇒ q=0.078 是「**位图捕获率**」而非机制上限; 机制上限
> **≤0.999**, 远超 condF 所需的 0.375。「协议层面不携带 runs 信息」的假设**被证伪**
> —— 显式 NACK 的 `missing_pages` 同样可导出 runs/lost, 只是采集没读。
> **安全性前置已验证 (实测, 15 轮)**: 放宽采集的风险是可能灌入偏向「判随机」的样本,
> 若真拥塞构造存在走显式 NACK 的消息即会误开回升门。实测: 标准 (真拥塞) 构造
> **15 轮 0 显式 NACK, 441 个 NACK 事件 100% 位图**; 饿死构造亦 0 显式; 只有 1% 随机
> 注入走显式。标准构造 ratio 分布 p50=0.069 / p90=0.158, lost 中位 43 (连续大段丢)
> ⇒ 相对阈值 0.579 判拥塞鲁棒。⇒ **该风险不成立, 放宽采集对标准构造新增样本 0、
> 误判随机 0**。⚠️ 三条限定须随此结论一同引用: ①trace 取自非定格 lib, 「函数体跨 lib
> 未变」是**推导非实测**, 落笔后须复验; ②**0/441 不等于概率恒 0**, rule of three 上限
> **0.68%/NACK 事件**; ③阈值 0.579 的重标定**不因此免除** (现值数据源为位图-only)。
> 另: 标准构造已存在 1/229 (0.44%) 单页丢 ratio=1.0 的单条假阳, 由「取最拥塞对端」
> 聚合兜底不翻转 —— **保护机制是聚合, 不是 ratio 本身**。
> **本段处置**: 不落笔 (无落笔授权)。落笔链「实现 → 重标定+安全复验 → 逐行审 →
> 档II 验收」列为下段第一优先, 另需确认 q 提升后若 bps* > initial 的钉顶性质
> (**本段未验**: 不落笔即无 q 提升, 亦无钉顶可测)。
> **结论: 未闭, 路径存在未落笔。**

> **P-4 告警可观测性洞 — 只立档 (独立问题, 结项必报, 不写成「小瑕疵」)**
> **可观测性洞**: 告警① 触发条件为 `bps == floor_bps` (钳到 floor 那一刻,
> data_rev.cc:759) 且每节点一次性 (:761); 告警② 同 floor 门控 (:793-800) ⇒ P-3 主
> 场景 (500 条停在 38.4M) 与 200 条场景 (68.4M) **产品侧全程零告警**。唯一可见性来自
> 测试工具的 rate trace, **不是产品能力** —— 现场部署 (无 rate trace) 会静默滑落数百
> 条, 直到约 593 条触底才打出一行告警。
> **文案误指病因**: 现有提示 (:766-769 「check net.core.rmem_max and subscriber
> health」) 在 1% 随机丢包场景下指错方向 —— 真实病因是**控制律把随机丢包当拥塞**,
> 与 rmem / 订阅者健康无关, 运维照此排查会查错方向。
> **修法**: 新增中间告警通道 (降速幅度 / 无回升时段); **两条既有告警逐字不动** (段3
> 红线)。**定位**: 既有缺陷 (段3 即存在), 非段5 引入, 不阻塞三条闭口。

**代码态锚点**: `data_rev.cc` md5 `56f72e57` / `build/lib/libipc.so.3` md5 `b8f4cc84`
(tester 与 reviewer 双方独立核对一致)。2j 判别器 + 2k 回升门已落笔并通过逐行审四条:
①判别量只进布尔/增益、不进 `st.bps=` 赋值 ②缺数据保守按拥塞 (不 fail-open)
③最小降 ≥bps/512 ④回升幅度逐字未动。段3 六条安全性质逐字保留。本段未 commit。

**段6 P-3 落笔与闭口最终结论 (2026-08-13)**

**总述: 段6 完成 P-3 落笔链（实现 → 逐行审 → 安全复验 → 档II condF 验收 → 同窗口
A/B 吞吐副作用评估），P-3 闭口由 leader 签字。** 四条判据门槛 (**1.05 / 85.3 /
initial×0.60 / 0.579**) 全程未动、未重定标。**P-1 / P-2 仍未闭**（见下）。**段6 闭的是
P-3 一条，不是三条** —— 报告不宣称「已闭环」「完整记录」。

> **P-3 稀疏丢包 — 本段闭口 (leader 签字)**
> 判据（档II condF，不变）：末50 中位 ≥ initial×0.60（100M 构造 = 60M）+ 末段严格递减
> 后缀 ≤25 + 送达 500/500。**5/5 PASS**：末50 中位 **100M**（min=max=100M，零方差）、
> 严格递减后缀 **1**、送达 **500/500**。judge_condF.sh md5 **54a91e8d**（3a 固化未漂移）、
> lib **17050d67**（跑前重核）、窗口 loadavg 1.40~1.69 无污染轮。
> 六项前置全满足：①落笔无代码缺陷（Z.5）②安全复验 0/175 ③口径可比（机制数学等价 +
> 截断实测未触发）④阈值维持 0.579（AA.3）⑤档II condF 5/5 PASS（AC.0）⑥同窗口 A/B
> 未观测到吞吐恶化（AG.0）。

> **落笔链（实测，全部完成）**
> 落笔（coder，1a）：显式 NACK `missing_pages` 导出 runs/lost，**只扩采集路径、不改任何
> 控制行为**，净改动 **+72/-13**（纯代码 +35 / 注释 +37 / 删除 13 全为注释）。
> 逐行审（reviewer，1d）：**通过，无代码缺陷**。六条安全性质 P1-P6 全过；`st.bps=` 4 写点
> RHS 逐字未变；判别量只进 `runs_random` 布尔门 → alpha_eff 增益，⛔ 不进 `st.bps=` 赋值
> 链；无 `rise=Δ·g(判别量)` 形式（段5 裁定 W 硬禁令守住）。
> 安全复验（tester，3c）：落笔态标准拥塞构造 **explicit_nack = 0/175**（100% 位图），与段5
> 3j 的 0/441 一致 ⇒ U.5 风险不成立支撑成立，W.2 限定①解除。⚠️ 上界**绑定样本量**：
> 0/175 ⇒ rule of three 上界 **≈1.7%**（95%，n=175），⛔ 不得沿用段5 更紧的 0.68%（n=441）。
> 档II condF 正式验收（tester，3d）：**5/5 PASS**（见上）。

> **核心成果：q 由 7.8% → 99.8%（采集窄化实证闭合）**
> 1% 构造 runs>0 样本：**~39/轮 → 499/轮**，q 从 **7.8% → 99.8%**（实测，3c 交付B）。
> 实测确认段5 coder 推导的机制上界 **0.999**；证实段5 裁定 **U.1「q 窄化是采集口径问题、
> 非协议固有」** —— 那 92% 的消息一直携带着 runs 信息，只是从前位图-only 采集没去读。
> 归因完整性：P-3 的「结构性不可达（甲支）」被排除（段5 U.1），「采集口径实现窄化（乙支）」
> 经本段落笔消除。

> **阈值裁定（AA.3）：维持 0.579，不改代码**
> 可行区间 = **(标准 p90=0.1429, 1% min=0.6667)**。0.579 在区间内，距上界 0.088，比 tester
> 报的中点 0.5714 更优（越靠上界 → 标准侧假阳越少），**改到 0.5714 是朝更差方向动，不采用**。
> 假阴核对：1% **0/1497**（min=0.6667>0.579）；饿死假阳 **0/117**（max=0.0211≪0.579）。
> 改阈值消除不了标准侧 ratio=1.0 的假阳（1.0 是比值上限，任何 <1.0 阈值都误判）—— 那是
> 机制问题，靠 T.1 聚合兜底，不是调阈值能解决。

> ⚠️ **形态限定（AC.1，必写）**
> **condF 达标形态是 initial 钳顶 100M，不是内部平衡。** 零方差（min=max=中位=100M）是
> **机制性的**：q≈0.998 ⇒ bps*≈159.7M > initial=100M ⇒ **被钳至 initial**。⛔ **不得表述为
> 「系统达到内部稳态/内部平衡」**。钉顶在 **P-1 五分类里属禁入态** —— **P-3 用 condF 判、
> P-1 用内部不动点判，两个判据不冲突，但绝不可混为一谈**：condF 达标不构成 P-1 相关证据。
> ⛔ 禁写「P-3 闭口证明限速器工作正常」——condF 达标**只证明稀疏丢包场景下速率不再单调
> 下滑到 floor**，这正是 P-3 原本的病征。

> **副作用：同窗口 A/B（n=5+5）未观测到吞吐恶化**
> 背景：3e 跨批观测中位 −13%（71.82→62.44），但两批负载差 ~1.5 倍、批内方差 ~4 倍于中位差
> → **跨批不可比、判不了** → 触发同窗口 A/B。
> 假阳样本（实测）：标准构造 **1/229（段5）→ 2/171（3c）**，**统计上无显著变化**（1→2 个
> 样本的涨落）；**落笔显式路径在标准构造未触发（explicit_nack=0）**，与假阳率**无因果**
> （⛔ 不得写「假阳率升 2.7 倍」——小样本比率倍数无统计意义，AG.1 订正）。2 个假阳均
> ratio=1.0（单页稀疏 pattern，runs=lost=1），ratio 上限=1.0 是机制问题非阈值问题，由
> T.1 取最拥塞对端聚合兜底。
> A/B 结果（tester 3f，同窗口 ABABAB，A=**984ce5af** / B=**17050d67**）：**A 中位 86.86
> （76.15~96.93）vs B 中位 89.58（81.94~105.56）**，中位差 **+2.72（B 高 3.1%）**，**完全
> 落在两组方差带重叠区 [81.94,96.93] 内**，方向 **B>A**。配对差 mean **+2.73**、sd 9.34、
> **paired_t p=0.549**（df=4）、**95%CI [−8.86, +14.32] 含 0**。送达 10/10 轮 rc=0、
> DELIVERED、200/200、0 超时；装载每轮 ldd+md5 断言**零错装**。
> ⛔ **不得写「副作用证否」**（AG.2：p=0.549 不能证否，n=5+5 检验力有限）。✅ 口径：
> **「同窗口 A/B（n=5+5）未观测到吞吐恶化」**。
> 更硬的证据在机制层（与实测互相印证）：**explicit_nack 两侧全 0** ⇒ 落笔采集路径未触发 ⇒
> **采集开销结构性不存在**；**inc 两侧相当、速率形态无漂移** ⇒ **无假阳误回升证据**。3e 的
> −13% 确认由负载差 + rmem 差解释，非落笔效应。

> ⚠️ **A/B 结论的四条限定（AG.3，必须一并引用）**
> ① **射程**：A 侧源 commit=**6143a76**，段5 b8f4cc84 的构建 commit **从未核实**；且 rmem
> 口径不同（P-2 基线实测 1MB vs 本次 212992）。⛔ **不得拿 A/B 绝对 msgps 与段5 的 71.82
> 比较**，本实验**不重判 P-2 基线**，射程仅「落笔是否使吞吐恶化」。② **G12 逐对门 4 PASS /
> 1 REJECT**（r2：B 侧 loadavg 1.38→1.51 爬升，drift 0.0373/s > 0.02）——r2 恰是差异最大轮
> （−13.33），负载爬升推低 B_r2；**即便含该轮，B 中位仍高于 A**。③ **上界绑样本量**：0/175
> ⇒ ≈**1.7%**（n=175），⛔ 不得沿用段5 的 0.68%（n=441）。④ P-2 本身仍未闭，本实验不重判。

> **C4 截断（Z.3/Z.4，已定）**
> 显式 256 截断**机制仍在**：触发边界 = **span > 11496 bit（≈1437 页 ≈2MB 消息）且缺页稀疏
> 散布**（⛔ 非 ~6MB，Z.0 订正）。该分支是 `send_nack_auto` **有意支持**的设计（:1488-1491，
> 注释明写「45000 片丢 300 片均匀散布，位图罩 77 片，显式报满 256 片」）。当前 1MB / 713 页
> 构造未触及，**余量约 2 倍**（从段5 曾以为的 6 倍缩至 2 倍，如实呈现）。实测（3c 交付C）：
> 显式样本 lost max ≤16 ≪ 256；标准/饿死零显式样本 ⇒ **X.5 分路径预案不启用，单阈值路线
> 维持**。⛔ 禁写「不存在截断问题」。

> **编码来源口径（AB.3，强制记载）**
> 1% 构造 NACK 帧计数：显式 **92.7%**（1494/1611）/ 位图 7.3%（117/1611）（实测）。
> ⛔ **per-sample 编码归属不在 trace，runs/lost 分布层面的两路径直接对比未做** —— ⛔ 禁写
> 「已按编码来源拆分验证两路径分布一致」/ 把帧计数说成分布拆分。口径可比性依据：①
> `count_bitmap_runs` 与 `count_explicit_runs` 经逐行审证明同 pattern 同边界下**数学等价**；
> ② C4 截断实测未触发 ⇒ **合并分布视为 pattern 真值**。分布（合并，实测）：标准 n=171
> p10/50/90=0.036/0.060/0.143｜1% n=1497 p10/50/90=1.000（min=0.6667）｜饿死 n=117
> p10/50/90=0.002/0.002/0.005。

> ⚠️ **P-1 / P-2 仍未闭（AG.4，必写）**
> **P-1** 内部不动点：**未闭**（5/5 INTERNAL-DRIFTING，max/min 1.061~1.195，FAIL）。
> **P-2** 吞吐：**未闭 FAIL**（中位 **71.82** < **85.3**）。本段未动其门槛、未重判。
> **段6 闭的是 P-3 一条，不是三条** —— ⛔ 禁写「已闭环」「完整记录」。

**代码态锚点（段6）**: src `data_rev.cc` md5 **a5d15b95**（落笔态，含显式 NACK runs/lost
导出）｜lib **17050d67**（落笔态重建，验收态）｜judge_condF.sh md5 **54a91e8d**｜A/B 两侧
lib：A=`/home/zwc/cpp_ipc_dds_ab_a_side/build/lib/libipc.so.1.3.0`（**984ce5af**，源等价
56f72e57，worktree 保留未删）/ B=主树（**17050d67**）｜采集 `calib_runs_v2_0812_x3c` /
验收 `condF_accept_0813_0000` / A/B `seg6_ab_3f_0813`｜gtest 指定绿集全过（1a 时序 flake
例外已 A/B 证与改动无关）。本段未 commit。


**段8 P-1 正式落笔报未闭 (2026-08-13)**

**总述: P-1 判据（内部不动点：5/5 轮 INTERNAL-FIXED-POINT，末50 max/min ≤ 1.05）到期报
**未闭**。四条判据门槛（**1.05 / 85.3 / initial×0.60 / 0.579**）全程未动、未重定标。本段
仅落笔段7 三轮讨论的归因结论（终裁 AJ + 用户裁答 AK），未改任何判据/门槛、未 commit。
四项总状态不变：**P-1 未闭 / P-2 未闭 FAIL（中位 71.82 < 85.3，段5 实测）/ P-3 已闭（段6）/
P-4 只立档**。

> **P-1 内部不动点 — 未闭（正式落笔）**
> 判据（不变）：5 轮末50 max/min ≤ **1.05** 且全判 INTERNAL-FIXED-POINT。
> 判定（实测，段5 3b 定基）：**5/5 INTERNAL-DRIFTING → FAIL**。官方末50 max/min
> **1.065~1.140**（逐轮 **1.084 / 1.140 / 1.097 / 1.136 / 1.065**），S_MED 中位 **182.9M
> （161.8~188.2M）**（出处=seg5_task3b_p1_baseline.md §1；前提= new×new、rmem=212992、
> 1MB×200、200M、R1 lib d07dd20f）。段5 报告另有 max/min **1.061~1.195**（段5:922 /
> 段6:1094，两处一致）——⛔ 该数是 **2k 代码态（lib b8f4cc84）**独立佐证（V.5，同末50
> 口径），与 R1 定基**非同批数据**，两代码态下 5/5 全 >1.05 不变。
> **FAIL 判定强健，非测量噪声**（tester 3h 滑窗复析）：thr_r2/r4 **151/151 窗全
> >1.05**（实测）；假 5/5 PASS 概率窗口级 **0.0043%**（推导：p̂^5，R1 p̂_pooled=0.1338、
> 101/755 窗）、run 级 rule-of-three 上界 **≤0.15%**（推导：0/11 官方 ≤1.05）。
> ⛔ 前提=滑窗高度自相关、有效独立样本=run 数（200 条<周期 250~300），run 级为诚实口径
> —— 所有诚实口径 <1%。
> ⛔ 周期未测定、振荡 vs 单向漂未分离（n=200 < 周期 250~300，段5 3b 实测）。
> **未闭定性（AK.4 口径原样）**：**「判据形式化过度 + 可达性前提在当前约束下未成立」**
> —— ⛔ 非「目标定错了」，⛔ 非「只差调参」。

> **立项意图（出处 = 用户裁答 2026-08-13，⛔ 不得标为推导或实测）**
> 段4 立项「速率收敛到内部定值」的意图 = **「别只有 floor / 天花板两态端点」**；「内部
> 点收敛」是段5 task3 §1.2 操作化时加上的形式（末50 max/min ≤1.05），**不是立项意图本身
> 要求的**。⇒ **带收敛在原则上与立项相容**（非降标准）；但**缺能预测带幅的机制** —— 判准②
> （带幅下界独立于自设禁区）未过、判准③（阈值独立于被评数据）**直接判 no**（无机制预测 ⇒
> 任何新阈值 X 从观测反推 = 与「抬到 1.07」逐字同构，裁定4 禁令本体）⇒ **不重定义判据、
> 四门槛不动**。⛔ 不得写成「用户同意放宽 P-1 判据」「用户认可带收敛判据」。

> **机制结论（终裁 AJ）**
> 带幅由**周期闭合条件**决定：**闭周期内降侧总扫掠 = 升侧总扫掠 =（每周期回升条数）× Δ**
> （AJ.1.a，coder 2f 守恒律推导）⇒ **带幅不由任何单侧参数独立决定**。历次单侧调参（方案2
> 改回升形状 / 拉长 α / 缩小 inc）失败是**共同原因** —— 单侧调整被平衡重定位吃掉，不是
> 各自没调对。实测佐证（tester 3j）：假想 inc→0 只剩降侧，`r_bound` 中位 **1.0769**、
> **11/11 run 中位 >1.05**（降侧单独已顶穿判据线）；但**判据窗末50 相位依赖**：11 内部 run
> 中 **7 条末50 `r_bound` < 1.05（1.008~1.040）、6 条升侧主导** —— ⛔ 两口径必须同时引用，
> 不得只引一半（AJ.6 限定4）。⚠️ AJ.2 限定：缩小 inc 对升侧主导的 run 有效，但平衡下移 +
> P-3 恢复 ∝1/inc + P-2 85.3 红线 ⇒ **「有效但代价在别处」与「无效」分开写**，不作为 P-1
> 杠杆。支柱②（α EWMA 滞后）：**拉长方向排除且有害**（试验B，g_inv 16→32 **单变量
> （相对 testA 方案2 基线）**，4/5 轮钉天花板且负载更低仍钉顶）；**缩短方向未测**。方案2（试验A）：**0/5 FAIL**（干净轮
> 1.052~1.055，优于 R1 最佳 1.065 但未跨 1.05），**不作 P-1 主解**。
> 系统形态（tester 置换检验，实测）：**真在漂/振荡，非不动点噪声** —— 16/16 显著异于白
> 噪声零分布、H0 校准 11/11 不拒绝、自相关 lag1=0.87~0.99；支撑=**大幅值（内部态全窗
> 166~209M）+ 平滑 + 排除瞬态/触顶/负载三外因**，⛔ 不得简化为「拒绝了白噪声」。

> **⛔ 三条未测路径 —— 未测 ≠ 已排除**
> 「可闭性低」**≠**「已证不可闭」。以下三条既不削弱安全性质、也未被实测排除：
> ① **④ 传输时滞**（NACK→α 更新跨一轮的相位滞后）：从未单变量测过，架构级（改 adapt
> 触发点）；⚠️ **修复项身份已机制级注销（AM.1），诊断项身份不变** ——『④ 是否驱动
> 带幅』仍未测，仅『调 τ 修 P-1』的修复杠杆已关闭（见下）；
> ② **α 门阈值上调**（`kRateRecoverAlpha` 1→2~4）：仅以「天花板钉顶风险」的分析拒绝，
> **未实测**；
> ③ **缩短 α**（`g_inv` 16→8）：试验B 只测拉长方向，对称补测**未做**。
> ⛔ 禁写「已穷尽」「已证不可闭」「已闭环」「完整记录」—— 未闭是当前状态，不是终局。
> 
> **E2④ 传输时滞：修复杠杆已机制级关闭，驱动因子仍未测（裁定 AM，2026-08-14）**
> ⛔ **两命题分开写、不得合并**（AM.7）：关闭的是**修复杠杆**（有硬证据），不是**驱动
> 因子命题**（仍未测）。
> **修复项**：调 τ 修 P-1 的**可达收益上界 = 0 步**（推导，出处=seg8_task2b_coder_
> upperbound.md）。τ 已贴**结构性下限 1 步**（消息 N 的反馈最早只能影响 N+1；代码三证：
> 发送端速率在消息起点固定并整条用绝对时间表 / adapt 触发点全在消息末 / 接收端产生 N 的
> F 只能在 N 发完之后）⇒ 可减步数 s=**0**（干净基线）~1（迟到反馈瞬态）；每步收益上界
> r_step≈**0.54%~1.51%**，再给 ×2 非线性乐观放大 ⇒ **≤3.0%**（前提=上界论证、非实测）。
> P-1 官方缺口 **g=r0−1.05 ∈ [1.5%,9.0%]**（逐轮 1.065/1.140/1.097/1.136/1.065，报告
> :1117 实测）典型 **4.7%** ⇒ s=0 提供降幅 0、任何 g>0 必跨不过；即便给不可达的 s=1+×2
> 双重乐观（≤3%），对典型 4.7%/最差 9.0% 仍**远不够**，且 P-1 要 **5/5 全过** ⇒ **④ 不
> 构成 P-1 的修复路径**。
> ⛔ 方向不对称永久禁令（AM.2）：τ<1 步是**空集**（无定义域），**不得**从 τ↑ 实验斜率
> 外推 τ↓ 修复收益。
> **诊断项**：**④ 作为驱动因子 —— 仍未测**。『④ 是否驱动带幅』无答案；本段只关闭修复
> 杠杆，不触及驱动因子命题。⛔ **不得**写成「④ 已排除」「④ 不是带幅成因」「④ 已穷尽」
> 「P-1 已证不可闭」「已闭环」。
> ⛔ 口径限定（AM.3/AM.6）：新口径（逐周期 r）基线**无法从已落盘数据回填**（34 条权威
> 标准构造 trace 全为 msgs=200、整条落首循环暂态 msg19→275 内 ⇒ **0 完整周期**；唯一
> long500 弃首 300 后剩 ~200 条 <1 慢周期，仅得单弧 r=**1.098**，实测、**n=1 无方差带**）
> ⇒ 需 **≥900 条新长跑**（口径基线本身也是 E2 成本项）。末50 max/min ≤ 逐周期 r 仅为
> **上界关系**（推导：窗口⊆周期；波形=周期不匀的随机弛豫振荡，无解析映射），⛔ **无确定
> 性数值换算** ——「逐周期带 ≤1.05」是 P-1 通过的**充分条件**，但当前逐周期带（估计
> ~1.10，⛔ 非实测）不满足，⛔ 不可反推末50 必过。⇒ E2 若开跑须**同 run 双量同测**
> （逐周期 r=响应/诊断量，末50 max/min=判据量），上界论证仅做结论级衔接、非数值换算。
> 
> 续攻候选实验（E1' 判据形式杠杆探针 / E2 ④ 时滞（**纯诊断，AM.8**）/ 实验 G 保幅降频 /
> E3 缩短 α）均只给设计、⛔ 未开跑（动控制律 / 冻结项须 leader 授权）。

**代码态锚点（段8）**: 本段零代码、零占机；`data_rev.cc`=a5d15b95 / `data_rev.h`=e3adf7ef /
lib=**17050d67**（与段6 结项逐字一致，seg7_closure_index.md §4）。判定工具统一（Y2 挂账：
floor×1.20 vs analyze_cell×1.02）**已清**（段5 裁定 M.1：×1.02 正确 = `analyze_cell.sh:89`
实现是事实；×1.20 为判据文档误写、无任何工具含此值，判据文档已对齐 ×1.02；段9 tester
复核确认 P-1 全工具链仅 analyze_cell 一处实现、judge_p1 委托之、档I/II 映射按 M.6）。P-1
判据引用仍须标代码态。本段未 commit。

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

**当前(2026-08-08)的判断**: 依据 7.2 的数据, 扩缓冲后现有实现已满足目标场景,
完整 RTPS 仍建议**维持现状**。8.1 已落地的四项(端点分离 + Heartbeat + NACK 位图压缩 + NACK 跨轮抑制)
已落地并实测通过 —— pub-sub 的 Reliable 模式从 1 msg/s 变成 110 MB/s 线速,
这是现有实现最明显的功能空缺, 现已填上。

**已完成的验证 (2026-08-08)**:

1. ✅ BestEffort 吞吐无回退, 与基线 commit `0dd0b0a` 一致。
2. ✅ pub-sub Reliable 实测 110 MB/s / 0% 丢包 (基线 1 msg/s), 三个 payload 档位见 §8.2。
3. ✅ 单元测试 35/35 全绿 (tester 独立复现, 非实现者自测)。
4. ✅ 位图 NACK 真实丢包触发 (见下)。
5. ✅ 闭环自适应限速标定完成: 场景 C(开环对照) / D(僵尸隔离) / E(下限保护) 全部通过, 详见 §8.2 ——
   判据仍是 **4 条过 3 条**: NACK 总量、送达率、重传占比三条通过; **速率收敛判据未达成**
   (新×新无 100M↔200M 极限环, 但稳态为天花板 200M 微锯齿、内部不动点未现, 是"失败的
   形态变了"而非"判据过了", 见 §8.2); 兜底告警②构造不可达 (**机制性不可达, 非缺陷**),
   未记为已验证。另: 两条此前缺记的未闭环项 —— **吞吐硬判据未达** (中位 84.39 < 85.3,
   4/5 轮低于, `rmem_max=wmem_max=212992`) 与 **1% 丢包零回升** (100M→38.4M, 其中
   **38.4M 为 pre-R1 末值、R1-only=31.88M**; red=489/inc=0 全程零回升), 见 §8.2
   「未闭环项补记」与「段5 三条闭口最终结论」。(段5 审查补记, 2026-08-10; 订正 08-12)

**验证过程中剥出的第三层缺陷**: 发送端 CRC 算在页号纠正之前(§7.5)。这一层只有在
前两层都修好之后才第一次有机会执行 —— 前两层没修时代码走不到 CRC 比较那一行。

**位图 NACK 在真实丢包下的触发 —— 已验证 (2026-08-08)**: 原文列为"仍未验证的一项",
验证手段正是下面这条(把 `rmem_max` 调回默认值制造缓冲溢出, 而不是用 `--rate-limit`,
后者是**减少**丢包的):

```bash
sudo sysctl -w net.core.rmem_max=212992   # 默认值, 会造成缓冲溢出
./build/bin/dzipc_perf_benchmark --cases=pubsub_socket \
    --payloads=1048576 --duration=5 --frag-stats
```

段3 的场景 B / C / E 标定都跑在这个条件下(`rmem_max=212992`, 1 MB 载荷), 结论:

- ✅ **位图 NACK 确实被触发**: 单订阅者场景实测 `bitmap_nack=20 / explicit_nack=0`
  (20 条消息全部走位图路径), 且送达 20/20 —— 触发的同时重传闭环有效。
- ✅ **空洞形态符合 §7.1 的突发预期**: 缺片跨度远大于 1, 这正是段3 自适应限速用
  "单帧 NACK 缺片跨度 ≥4"作溢出信号判据的实测依据(§8.1)。
- ℹ️ **计数器读取前提**: `nack_bitmap_sent` / `nack_explicit_sent` 等诊断计数受
  `set_fragment_loss_tracking(true)` 门控, 未开启时读到的是 0。**功能本身不受该门控**,
  别把"计数为 0"误读成"位图没工作"。

**前置已就位, 剩下两项的实际结局 (2026-08-08 更新)**: 本节原先论证"NACK 抑制与
WHC 闭环流控都依赖一个当前不存在的前置 —— 对端身份表"。**该前置已实现**(发送端现在
读取 `receiver_id`, 维护 per-node 的 `receiver_id → PeerState` 表), 两项功能均已在
其上落地, 故此处改写为完成时。

身份表实际解锁了什么:

- ✅ **跨轮 NACK 抑制**: 已落地(§8.1)。但注意它并不靠身份表做主判据 —— 抑制键是
  `(message, page)`("这个分片我最近发过没有"), 因为重传走组播, 记到 per-peer 粒度
  也不减少任何发送字节。身份表在这里只承担一件事: 判定 NACK 来自**未知 peer**(中途
  加入的新订阅者)时旁路抑制、立即补发。
- ✅ **闭环流控**: 已重定标为闭环自适应限速并落地(§8.1)。这里出现了一个预期外的转折:
  原设想的"按对端算未确认字节数"**用不了** —— `highest_acked_seq` 只在该 peer 赢得
  首个匹配 ACK 时才推进(at-least-one 语义, §8.2), 恒输竞争的 peer 其记录永久陈旧,
  "最慢 live peer"这个判据没有可信数据源。改为**事件驱动的溢出信号**(单帧 NACK 缺片
  跨度 ≥4 且来自租期内 live peer), 身份表在这里只用于 liveness 判定(`last_ack_ts`),
  不用于进度判定。
- ❌ **单播重传**: 仍未做, 且**身份表并不足以解锁它** —— 卡在传输层, 见 §8.1 的三条
  链路(拿不到对端地址 / ACK 源端口是临时的 / 订阅端无可寻址的单播接收端点)。
- ⚠️ **`publish_blocking()` 仍是"至少一个确认"**: 这不是漏做, 是轻量路径的语义天花板
  (§8.2)。全体确认需要阻塞等所有 Reader, 与"任一僵尸订阅者可挂死发布端"是同一件事,
  已明确拒绝。

**结论修正**: 原文把"1 个前置 + 2 个建立其上的功能"看作串行依赖链, 实际结果是
**身份表是必要不充分条件** —— 它回答了"谁在报缺"(liveness、未知 peer 识别), 回答不了
"在哪发"(单播地址)与"发到哪了"(per-peer 页级进度)。这张表在 RTPS 里对应 `ReaderProxy`
的一个**真子集**; 补齐剩下部分(locator + 页级 requestedChanges)才是轻量路径与完整
RTPS 之间那道实质的坎, 而不是这张表本身。

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
