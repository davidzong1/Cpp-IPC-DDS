# 添加 DDS 标准接口: 差距分析与路线

> **状态**: **未来事项, 暂不实施。** 本文档只做差距分析与路线比较, 不含施工计划。
> **编写日期**: 2026-09-13
> **基线**: HEAD = `aaa6fd1`(DZFlat 零拷贝分支落地后)。
> **与既有文档的关系**: [RTPS技术路线报告.md](RTPS技术路线报告.md) 分析的是**wire 协议**
> 层面做完整 RTPS 的代价(结论: 重写而非重构, 8 项结构性差距中 6 项无法增量改造)。本文档
> 视角不同 —— 问的是**用户可见的 API/QoS/实体模型**距离 OMG DDS v1.4 有多远, 以及有哪几种
> 到达方式。两份要一起读。
> **一句话结论**: 当前架构与 DDS 的距离, **90% 不在 API 形状, 而在三个结构性前提**
> (§3)。API 形状是可以在几周内套出来的; 那三条不解决, 套出来的门面在关键语义上是空的。

---

## 1. 现状对照表

| DDS 概念 | 现有对应物 | 证据 | 差距 |
|---|---|---|---|
| DomainParticipant | 无 —— `domain_id` 是每次调用重新传的 `size_t` | `include/dzIPC/dzipc.h:66,70` | 全缺 |
| **Domain 隔离** | UDP: 端口含 domain; **SHM: 完全没有** | `src/dzIPC/common/hash.cc:22-34`; `src/dzIPC/shm_pub_sub_ipc.cc:27-30` | 部分 / **全缺** |
| Publisher / Subscriber(作为容器) | 无 —— pimpl 本身就是 writer/reader, 无分组实体 | `include/dzIPC/topic_ipc.h:11-53` | 全缺 |
| Topic 实体 | 一个 `const std::string&` 参数, 每个端点各传一次 | `include/dzIPC/dzipc.h:66,70` | 全缺 |
| 注册类型名 | `typeid` demangle, 仅供 info_pool 展示, 不上 wire 不参与匹配 | `src/dzIPC/shm_pub_sub_ipc.cc:105-110` | 全缺 |
| wire 上的类型判别 | `msg_id`(用户随手给的 int, 默认 0), TLV 在页尾 / DZFlat 在段头 | `include/dzIPC/common/topic_data.h:51,61`; `ipc_msg_base.hpp:48-58`; `dzflat.h:65` | 部分 |
| DataWriter\<T\> / DataReader\<T\> | `publish(shared_ptr<IpcMsgBase>)` / `get_clone(...)`, **无类型** | `include/dzIPC/topic_ipc.h:20-23,42-47` | 部分 |
| Listener(pub/sub) | 无(仅服务端有回调) | `include/dzIPC/shm_pub_sub_ipc.h:99` (`// void sub_listener();`) | 全缺 |
| WaitSet / Condition | 无; 最接近的是 `CircularQueue` 的 `condition_variable` | `include/dzIPC/common/circularqueue.h:40-69,182-183` | 全缺 |
| ContentFilteredTopic / QueryCondition | 无 | `docs/RTPS技术路线报告.md:157` | 全缺 |
| 键 / InstanceHandle / DISPOSE / UNREGISTER | 无; `.msg` 语法没有 `@key` | `generator/msg_generator.py:216-227` | 全缺 |
| 内建发现主题(SPDP/SEDP) | `IpcInfoPool` 共享内存表 + 每 topic `TopicControl` | `include/dzIPC/ipc_info_pool.h:39-63`; `common/control_plane.h:28-51` | 全缺 |
| 远端参与者发现 | 无(纯本机共享内存; 组播 TTL 硬编码 1) | `include/dzIPC/socket_pub_sub_ipc.h:60-64,74-77`; `platform/posix/udp.h:212-213` | 全缺 |
| SampleInfo(时间戳/样本态/实例态) | 无 | `include/dzIPC/pub_sub_base.h:47-52` | 全缺 |
| `read()` vs `take()` | 只有 take(每次读都是破坏性 pop) | `include/dzIPC/common/circularqueue.h:146-176` | 全缺 |
| CDR / XCDR2 | **零支持**(全仓 grep 无命中); 现有是 TLV + DZFlat 两套自有格式 | `generator/msg_generator.py:388-404`; `dzflat.h:56-68` | 全缺 |
| OMG IDL / X-Types | 26 个类型码的 `.msg` 语法, 只接受 `type name` | `generator/msg_generator.py:46-89,216-227` | 全缺 |

### QoS 专项

DDS QoS 的四个性质是: (i) 按实体声明, (ii) 随发现交换, (iii) RxO 匹配, (iv) 不兼容时给
`*IncompatibleQos` 状态。**这四条当前一条都不成立** —— 没有任何 QoS 上 wire, 没有协商,
没有状态通道。

| DDS 策略 | 现有对应物 | 证据 | 差距 |
|---|---|---|---|
| RELIABILITY | `publish()` = BestEffort / `publish_blocking()` = 可靠+CRC。**按调用而非按实体**, 且语义只是"至少一个 ACK"(不是任何 DDS 可靠性等级) | `src/dzIPC/socket_pub_sub_ipc.cc:357-358,385-394`; `include/dzIPC/socket_pub_sub_ipc.h:39-51` | 部分 |
| HISTORY depth | `queue_size` → 读端 `CircularQueue`, 满则丢最旧。**只有读端**, 无写端历史, 无 KEEP_ALL | `include/dzIPC/dzipc.h:71`; `circularqueue.h:102-113` | 部分 |
| RESOURCE_LIMITS | 三个编译期硬顶, 都不可配: chunk 池 32/尺寸档、环 256 槽、接收者 32 | `include/libipc/def.h:44`; `circ/elem_array.h:30`; `circ/elem_def.h:19` | 部分 |
| LIVELINESS | 控制面心跳(订阅 10ms 刷 / 发布 2s 收)+ info_pool `kill(pid,0)`。**纯内部机制**, 不对用户暴露, 无 lease QoS 无状态回调 | `common/control_plane.h:34,95`; `shm_pub_sub_ipc.cc:141-142,566-567` | 部分 |
| TYPE_CONSISTENCY | DZFlat 的 `schema_hash`(FNV-1a 32): 二元的"一模一样或拒收", 不可协商 | `dzflat.h:59,80-117` | 部分 |
| TRANSPORT_PRIORITY | `DispatchPriority`/`CPU_CORE` 是**线程**调度(SCHED_FIFO + 绑核), 不是报文优先级 | `include/dzIPC/type.h:10-33` | 无(名字被占) |
| USER / TOPIC / GROUP_DATA | `RegisterInfo::extra`(≤64 字节), 本机诊断字符串, 不上 wire | `include/dzIPC/ipc_info_pool.h:35,46` | 无 |
| DURABILITY(+SERVICE) | **无。没有写端历史缓存**("发完即弃"), 晚加入者什么也拿不到 | `docs/RTPS技术路线报告.md:111` | 无 |
| DEADLINE | 无 | `docs/RTPS技术路线报告.md:156` | 无 |
| LATENCY_BUDGET | 无(存在的是它的反面: `rate_limit_bps` 限速) | `include/dzIPC/common/data_rev.h:49` | 无 |
| OWNERSHIP(+STRENGTH) | 无 —— 需要每样本的写者身份, 而那不存在(§3 拦路石 2) | `docs/RTPS技术路线报告.md:156` | 无 |
| PARTITION | 无(全仓无此概念) | — | 无 |
| LIFESPAN | 无(wire 上没有样本时间戳; 页尾只有 page/size/msg_id) | `ipc_msg_base.hpp:144-159` | 无 |
| DESTINATION_ORDER | 无(无源时间戳, 顺序就是环/队列给的顺序) | `circularqueue.h:146-176` | 无 |
| TIME_BASED_FILTER | 无 | `docs/RTPS技术路线报告.md:156` | 无 |
| PRESENTATION | 无(没有 Publisher/Subscriber 容器实体可供它作用) | `include/dzIPC/topic_ipc.h:11-53` | 无 |
| WRITER / READER_DATA_LIFECYCLE | 无(没有实例概念) | — | 无 |
| ENTITY_FACTORY | `InitChannel()` 是手工两阶段"构造+启用", 但没有 autoenable 策略对象 | `include/dzIPC/topic_ipc.h:18,39` | 无 |

**一个命名冲突值得单独提**: 公共 API 里叫 `Qos` 的那个 enum 是**线程调度开关**, 与 DDS QoS
毫无关系, 却占了这个名字:

```cpp
// include/dzIPC/type.h:22-26
enum Qos:bool { UseQos = true, NotUseQos = false };
```

真做 DDS 门面时这个名字要腾出来。

### UDP 那条路是不是 RTPS

**不是 RTPS wire**, 但 RTPS-*like* 的部分是真的。

| 真有(可复用) | 证据 |
|---|---|
| 分片 + 重组 | `ipc_msg_base.hpp:135-136,161-192`; `data_rev.cc:322-356` |
| 心跳(带 Final/Reliable 标志) | `udp_rtps_ack_msg.hpp:493-582` |
| NACK: 显式列表(≤256) + 位图(≤11496 bit) | `udp_rtps_ack_msg.hpp:8-95,125-274` |
| ACK 带 CRC32C, 及 DZA2 拥塞反馈变体 | `udp_rtps_ack_msg.hpp:276-369,390-474` |
| RFC 6298 RTO 估计 | `data_rev.cc:460-487` |
| 跨轮 NACK 抑制 + DCTCP 式自适应速率 | `include/dzIPC/common/data_rev.h:164-190` |

| 标准结构元素(全缺) | 现实 |
|---|---|
| RTPS Header(`'R','T','P','S'` + 版本 + VendorId + GuidPrefix, 20B) | 无。wire 首字节就是载荷 |
| Submessage 头(`submessageId`/flags/`octetsToNextHeader`) | 无。控制帧是普通 `IpcMsgBase` 子类, 靠页尾 msg_id 区分: `"DZHB"/"DZAK"/"DZA2"/"DZNK"/"DZNB"` (`udp_rtps_ack_msg.hpp:584-595`) |
| DATA / DATA_FRAG / GAP / INFO_TS / INFO_DST / NACK_FRAG | 一个都没有 |
| GUID(GuidPrefix 12 + EntityId 4) | 每进程一个随机 u32, 由时钟读数异或而来(`data_rev.cc:232-240`) |
| Locator | 无。地址由 topic 名 hash 出来(`hash.cc:38-46`) |
| 64 位 SequenceNumber + 独立 fragmentNumber | 单个进程级 `uint32_t sequence` + `uint16_t now_page` |
| 有状态 Writer/Reader(每匹配读者状态) | `PeerState` 只有 `{highest_acked_seq, last_ack_ts, last_nack_ts, ack_count}`, 消息级而非样本/分片级(`data_rev.cc:505-518`) |
| WHC / RHC | 无 |
| SPDP / SEDP | 无 |

---

## 2. 三条路

### 路 A —— DDS 形状的 C++ 门面(API 合规, 无 wire 互操作)

*"DDS 标准"在这里的含义*: 遵守 OMG DDS v1.4 的实体结构与调用语义。两个 dzIPC 进程按 DDS
语义对话; Cyclone/FastDDS 进程**不能**与之通信。

需要从现有代码补的:
- 一个**现在没人持有的状态层**: participant 对象持 `domain_id`, topic 注册表映射
  `(name, type_name) → msg_id`, 以及容器实体。今天 `domain_id` 与 `msg_id` 是每个调用点
  重新传的(`dzipc.h:66-80`), 没有任何对象拥有它们。
- **先修 SHM 的 domain 隔离**(见 [shm_defect_fixes.md](shm_defect_fixes.md) 第 1 条)。
  一个不隔离 domain 的 `DomainParticipant` 是在宣告传输层并不提供的保证 —— 这是路 A 这条
  最便宜的路也绕不开的前提。
- 类型化 `DataWriter<T>`/`DataReader<T>` 包住现有无类型接口。
- Listener + WaitSet/Condition: **有现成落点** —— 每订阅者已有接收线程
  (`shm_pub_sub_ipc.cc:704,732`), `CircularQueue` 已带 `condition_variable`
  (`circularqueue.h:182-183`)。`StatusCondition`/`ReadCondition` 挂那里, `GuardCondition` 平凡。
- **QoS 三分处理**:
  - 直接映射: RELIABILITY(选 `publish` 还是 `publish_blocking`)、HISTORY.depth(`queue_size`)、
    RESOURCE_LIMITS(对着 32/256/32 三个硬顶做校验);
  - 门面内用定时器**本地模拟**(不需要 wire 支持): DEADLINE、LIFESPAN、TIME_BASED_FILTER、
    LATENCY_BUDGET;
  - **诚实拒绝为 UNSUPPORTED**: `DURABILITY != VOLATILE`(要写端历史缓存, 不存在)、
    OWNERSHIP(要每样本写者身份)、PARTITION/PRESENTATION(要发现字段)、
    `DESTINATION_ORDER != RECEPTION`(要源时间戳)。
- 键/实例: 要么改 generator(加 `@key` → 生成 `get_key()` + `InstanceHandle` = 键字段 hash),
  要么就是没有。DISPOSE/UNREGISTER 可以在 `0x445A****` 控制帧保留段里加两个 msg_id ——
  可行但是私有扩展。
- **RxO 匹配与 `*IncompatibleQos` 状态无法诚实实现**: 没有任何 QoS 上 wire, 不兼容根本
  检测不到。门面只能在同进程内匹配, 或者干脆不匹配。

传输层改动: 约 60–70% 的 API 面几乎为零。DZFlat 的 loan 路径已经给了 `loan`/`return_loan`
的对应物。

### 路 B —— 真 DDS-RTPS wire 互操作

需要: RTPS 消息框架(Header + `DATA/DATA_FRAG/HEARTBEAT/HEARTBEAT_FRAG/ACKNACK/NACK_FRAG/
GAP/INFO_TS/INFO_DST` 各 submessage)、GUID 分配、Locator 抽象、标准端口上的 SPDP+SEDP 内建
端点、有状态 Writer/Reader(64 位 `writerSN` + `fragmentNumber`)、支撑 DURABILITY+HISTORY 的
WHC/RHC, 以及**在 generator 里新增一个 CDR 后端**(与现有 TLV、DZFlat 两个后端并列)。

从现有代码看这是**平行栈而非重构**: 报告把 8 项结构性差距中的 6 项判为不可增量改造
(`RTPS技术路线报告.md:118`), 预算 18 周(`:463`), 仅 QoS+DDS 层约 2500 行(`:478`)。
可复用: `NodeRole` socket 层、RTT 估计、限速器、CRC32C、分片丢失遥测、以及端点分离那个发现
(`:120-135`)。不可复用: 整个 `IpcMsgBase` 序列化契约、页尾格式、`msg_id` 匹配、`IpcInfoPool`。

### 路 C —— 绑到现成 DDS 实现, dzIPC 保留为本机快路径

需要: generator 加 `.msg → .idl` 发射器(26 个类型码是 IDL 的严格子集, 正向转换是机械的)、
厂商 IDL 编译器产 CDR 类型、门面里做分派(本机走 dzIPC SHM/DZFlat, 跨机走厂商栈)。

代价: 每消息**两套类型表示**(TLV/DZFlat **和** CDR), 要么双份 codegen 要么加一次转换;
DZFlat 的 `LoanedMessage`/`Sample` 零拷贝契约无法直接映射到厂商 loan API; 且破坏 README
声明的"除 STL 无其他依赖"。收益: 标准 QoS、DURABILITY、键、内建发现、认证过的互操作性
**全部白拿**, 一行都不用自己写。

---

## 3. 三个最硬的拦路石

### 拦路石 1 —— 载荷不是连续字节块, 因此无法塞进 DDS/RTPS 的 payload

`IpcMsgBase::serialize()` 产出的不是"一条序列化消息", 而是**每 1460 字节插一个 12 字节
控制尾的分页流**, 且收发两侧的偏移算术都在跨这些尾:

```cpp
// include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp:277
offset += copy_size + TAIL_MSG_SIZE;   // 跨过数据长度外，还要跨过那 12 个尾部特征字节
```

更麻烦的是那个尾**同时是**框架**和**类型判别符: `check_id` 从最后 4 字节读 `msg_id`
(`ipc_msg_base.hpp:48-58`), 而这是订阅端唯一的匹配谓词(`topic_data.h:51`)。所以:
**不能剥掉尾巴去得到可 CDR 化的载荷而不同时替换类型匹配机制**, 也不能留着 —— RTPS 的
`SerializedPayload` 必须连续且带 4 字节封装头。

射程之广是这条最贵的原因: 落在生成代码(约 30 个生成头)、运行期基类、SHM 接收路径
(`wire_accept.cc`)、UDP 重组路径(`data_rev.cc:322-356`)、以及 sniffer 工具
(`exec/dzipc_topic_cat/`)。**挡住路 B 与路 C 的互操作部分。**

### 拦路石 2 —— 没有端点身份, 也没有可挂在它上面的每读者状态

RTPS/DDS 里 RELIABILITY、DURABILITY、OWNERSHIP、per-writer LIVELINESS、实例生命周期
**全都**要挂在稳定的 per-endpoint GUID 上。现有的是**每进程一个、从时钟读数异或出来的
随机 u32**:

```cpp
// src/dzIPC/common/data_rev.cc:232-240
uint32_t local_node_id()
{
    static const uint32_t id = [] {
        const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return static_cast<uint32_t>((now >> 32) ^ now ^ 0xA5'3C'9E'17u);
    }();
    return id;
}
```

代码注释自己写着后果: "receiver_id 是进程级身份(local_node_id, 每进程一个), 同进程内多个
订阅者会合并为同一条目"(`data_rev.h:228-230`)。每 peer 状态表被有意冻结在消息粒度
(`data_rev.cc:512-518`), 且 `:509` 注明 locator 与 inline QoS 被决策 D-1 显式排除。

这就是为什么 `publish_blocking` 只能承诺**"至少一个读者 ACK 了"**
(`socket_pub_sub_ipc.h:39-51`) —— 那不是任何一个 DDS 可靠性等级。报告还记录了连**单播
重传**都被三层挡住(`recvfrom` 丢掉源地址、ACK 源端口是临时的、没有 per-subscriber 单播
端点), 要解开就得推翻 D-1 的字段冻结(`RTPS技术路线报告.md:684-697`)。
**所有 ownership/durability/per-reader 可靠性 QoS 都堵在这后面。**

### 拦路石 3 —— 发现是本机共享内存表, 且 topic↔传输绑定是算出来的而非协商出来的

发现就是一个 shm 段 `dz_ipc_info_pool_v1`, 512 个固定槽, 靠 `kill(pid,0)` 判活
(`ipc_info_pool.cc:34,309`; `:105-123`)。按构造就跨不了机器, 而 socket 发布端的头文件自己
写明 UDP 组播没有反向发现通道(`socket_pub_sub_ipc.h:74-77`)。

同时传输绑定是**从 topic 名 hash 出来**而不是交换来的: `239.255.<fnv%254>.<fnv%254>` 与
`11451 + domain*(fnv%10000)`(`hash.cc:22-46`)。三个后果:
1. 没有端点公告 ⟹ 承载不了类型名/QoS/partition ⟹ **上面任何一层都实现不了 RxO 匹配与
   `RequestedIncompatibleQos`**;
2. `has_subscribed()` 结构性地看不见远端与非 dzIPC 读者(`socket_pub_sub_ipc.h:60-64`),
   撑不起 `PublicationMatched`;
3. 组播 TTL 钉死为 1(`platform/posix/udp.h:212-217`), 连数据面都只在单个 L2 段内。

再加上 SHM 完全忽略 `domain_id`(`shm_pub_sub_ipc.cc:27-30`), 一个 `DomainParticipant` 门面
会在宣告传输层并不提供的隔离 —— **这条即使走最便宜的路 A 也必须先修。**

---

## 4. 如果要动, 建议的取舍

- **目标是"用起来像 DDS、团队好上手"** → 路 A。先修 domain 隔离, 然后实体层 + 类型化
  reader/writer + Listener/WaitSet, QoS 按上面三分法处理, 并且**在文档里明确列出哪些
  policy 是 UNSUPPORTED** —— 假装支持比不支持坏得多。
- **目标是"跟别家 DDS 互通"** → 按三个拦路石看, **路 C 比路 B 划算得多**。路 B 是 18 周
  的平行栈, 而路 C 用一个 `.msg → .idl` 发射器加分派层就能拿到全部标准语义, 代价是引入
  依赖和双份类型表示。
- **无论哪条路**, 拦路石 1(页尾兼作类型判别符)是最贵的一项, 因为它的射程覆盖生成代码。
  如果将来有任何互操作打算, **越早把"框架"与"类型判别"解耦, 代价越低**。
