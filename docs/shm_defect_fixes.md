# SHM 侧缺陷修复计划与进度

> **状态**: 施工中。本文档同时是**进度表** —— 每修完一条就回来更新该条的「进度」栏与
> 「§ 修复记录」。
> **编写日期**: 2026-09-13
> **来源**: 对 DZFlat 零拷贝分支落地后的现存问题审计(HEAD = `aaa6fd1`)。与
> [dzflat_known_issues.md](dzflat_known_issues.md) 的分工: 那份登记的是 DZFlat 特性
> **自身**的九条(已全部闭环), 这份登记的是审计中发现的**传输层既存**缺陷 —— 它们与
> DZFlat 无关, 在 DZFlat 之前就在。
> **怎么用**: 每条写明「症状 → 成因(带 file:line 证据) → 判据 → 进度」。症状一栏是给
> 排查者看的 —— 这四条**全都不以报错形式出现**, 表现成"通道明明通了却收不到"或
> "两个本该隔离的域互相看见了"。

---

## 0. 一览

| # | 问题 | 严重度 | 成本 | 进度 |
|---|---|---|---|---|
| 1 | SHM 段名不含 domain_id → **完全没有 domain 隔离** | **高(语义漏洞)** | 低 | ✅ 已修(见 §修复记录 1) |
| 2 | 第 33 个订阅者静默黑洞(连接位耗尽仍宣布握手成功) | **高(静默失败)** | 低 | ✅ 已修(见 §修复记录 2) |
| 3 | UDP 默认 domain 下所有 topic 同端口 + `INADDR_ANY` 绑定 → **跨 topic 串扰** | **高(错收他人数据)** | 低 | ✅ 已修(实测坐实, 见 §修复记录 3) |
| 4 | `get(Sample&)` 无超时重载 → TLV-only 话题上永久阻塞 | 低(易绕开) | 极低 | ✅ 已修(见 §修复记录 4) |
| 5 | 组播**组地址碰撞**: 8192 topic 下 60% 与他人共组(默认 domain 下即完全串扰) | **高(规模相关)** | 低(改端口公式) | ⬜ 未修(**已实测**, 见 §5) |
| 6 | 端口公式**越界抛异常** → domain≥6 起部分 topic 名**建连接直接打死进程** | **高(可用性)** | 低 | ✅ 已修(实测坐实, 见 §修复记录 6) |

**修复顺序**: 1 → 2 → 3 → 4。第 1 条排头不只因为影响最大, 还因为它是**任何 DDS 门面的
前提**(见 [dds_interface_roadmap.md](dds_interface_roadmap.md) 路 A) —— 一个不隔离 domain 的
`DomainParticipant` 是在宣告传输层并不提供的保证。

**一条已撤销的条目**: 审计初期我曾报"known_issues 第 4 条(套圈重复入池)文档与代码矛盾"。
**那是误读**: 该条标题写着"【已修, 见 §8】"(`dzflat_known_issues.md:184`), 一览表也是 ✅;
被我引作"未修"的 `:198` 那句属于该条**成因**段里对**修复前状态**的历史叙述。文档与代码
一致, 无需处理。记在这里是因为"把成因段的历史叙述当成现状声明"是一种会反复犯的读法错误。

---

## 1. SHM 段名不含 domain_id → 完全没有 domain 隔离

**症状**

两个进程分别用 `domain_id=0` 和 `domain_id=7` 发布**同一个 topic 名**, 本该互不可见, 实际
互相收到了对方的消息。UDP 传输下不会(它的端口含 domain), 只有 SHM 会。

**成因**

段名只由 topic 名派生, `domain_id` 没参与:

```cpp
// src/dzIPC/shm_pub_sub_ipc.cc:27-30
std::string shm_name_for_topic(const std::string& topic_name)
{
    return "dz_ipc_" + sanitize_topic_name(topic_name) + "_topic";   // ← 无 domain_id
}
```

`domain_id` 确实被存进了 `domain_id_`(`:51/:380`), 但它只用在两个**不影响通道归属**的地方:
- 进程内 nodelet 快速路径的 `ChannelKey`(`:199/:401`);
- info_pool 的诊断展示记录(`:110`)。

于是 domain 在 SHM 上退化成"只对同进程快速路径有效"的标签, 跨进程完全失效。控制面段名
派生自数据段名(`control_name_for`, `:19-25`), 所以控制面同样不隔离。

**为什么一直没暴露**: 这不是崩溃也不是丢消息, 而是**多收**。测试里没有"同名 topic 跨
domain 应互不可见"这条断言, 而正常使用中很少有人给同名 topic 配不同 domain。

**判据**

- 同一 topic 名 + 不同 `domain_id` 的发布/订阅对: 订阅方**收不到**对方消息;
- 同一 topic 名 + 相同 `domain_id`: 照常互通(不得回归);
- 控制面段一并隔离(否则 generation/PeerSlot 表仍会串)。

**该怎么办**

段名纳入 domain。**换名意味着与旧版本进程不互通** —— 这在本仓有先例且写明过理由:
`control_name_for` 的 `_control2` 后缀就是同一手法(结构体变大后必须换名, 顺带隔离新旧
版本进程)。修复时要在注释里同样写明。

**进度**: ✅ 已修 —— 见 §修复记录 1。修的过程中发现段名规则被复刻在五处, 顺带收成了
单一出处; 并暴露出两个既存 bug(一个测试参数写错、dzplot 的控制面段名一直是错的)。

---

## 2. 第 33 个订阅者静默黑洞

**症状**

订阅者进程起来了, `InitChannel()` 没报错, 日志看着一切正常 —— 但**一条消息都收不到**。
只在同一 topic 上已有 32 个订阅者时出现。发布端的 `has_subscribed()`/`recv_count()` 也
看不出异常。

**成因**

libipc 的接收方连接位图是 `cc_t = uint32_t`(`src/libipc/circ/elem_def.h:19-20`), 只有
**32 个位**。位满时 `connect()` 返回 0:

```cpp
// src/libipc/circ/elem_def.h:59-67
cc_t connect() noexcept {
    for (unsigned k = 0;; ipc::yield(k)) {
        cc_t curr = this->cc_.load(std::memory_order_acquire);
        cc_t next = curr | (curr + 1);   // find the first 0, and set it to 1.
        if (next == curr) {
            // connection-slot is full.
            return 0;
        }
        ...
```

而订阅端拿到 `cc_id == 0` 之后**照样宣布握手完成**:

```cpp
// src/dzIPC/shm_pub_sub_ipc.cc:522-534
const uint32_t cc_id = (subscriber_ && subscriber_->valid())
                           ? subscriber_->connected_id() : 0u;
peer_slot_ = control_plane_.acquire_peer_slot(attached_generation, cc_id);
if (peer_slot_ < 0 && verbose_) { /* 只在 verbose_ 下打一行控制面槽位告警 */ }
handshake_completed.store(true, std::memory_order_release);   // ← cc_id==0 也走到这
```

"连接位耗尽"这件事本身**既没有计数也没有告警** —— 上面那行 `verbose_` 告警说的是控制面
`PeerSlot` 表满, 是另一回事。

**一个放大了问题的不匹配**: 控制面 `PeerSlot` 表是 **64** 槽
(`include/dzIPC/common/control_plane.h:36-40` `kMaxPeerSlots = 64`), 传输层只能连 **32** 个。
这个 2× 差额正是黑洞的容量: 第 33..64 个订阅者能在控制面登记成功、`handshake_completed`
置真, 传输层却一个都没连上。

发布端从不校验这个: 它只用 `recv_count()` 判"有没有接收者"(`:288/:322`)和快速路径门控
(`:202`), 没有任何地方把"控制面登记的 peer 数"与"实际连上的连接位数"对比。**两端都看不见。**

**判据**

- 第 33 个订阅者: `InitChannel()` 后**不得**进入 `handshake_completed == true` 的假成功态;
- 该失败必须**可观测**(计数或非 verbose 告警), 不能只靠 `verbose_`;
- 前 32 个订阅者行为不变(不得回归);
- `kMaxPeerSlots` 与连接位上限的关系要么对齐, 要么在注释里写明为何是 64。

**进度**: ✅ 已修 —— 见 §修复记录 2。

---

## 3. UDP 默认 domain 下所有 topic 同端口 + `INADDR_ANY` 绑定 → 跨 topic 串扰

**症状(推测, 待确认)**

同机多个 UDP topic 且都用默认 `domain_id = 0` 时, 一个订阅者可能收到**别的 topic** 的
数据报; 若两个 topic 又都用默认 `msg_id = 0`, `check_id` 会放行, 于是按错误的类型
反序列化。

**成因(代码已确认, 效应未实测)**

`domain_id` 是**乘数**而不是偏移:

```cpp
// src/dzIPC/common/hash.cc:22-34
uint64_t hash_value = static_cast<uint16_t>(fnv1a64(topic_name) % 10000);
uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + domain_id * hash_value;
```

`domain_id = 0`(默认值)⟹ `limited_port == UDP_DISCOVERY_BASE_PORT == 11451`, **对所有
topic 都一样**。组播地址由 topic 名单独算(`:38-46`), 所以此时靠"组"而非"端口"区分。

而 socket 是绑 `INADDR_ANY:port` 之后再 `IP_ADD_MEMBERSHIP` 入组:

```cpp
// src/libipc/platform/posix/udp.h:168-193
local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
... ::bind(server_fd, ...) ...
... ::setsockopt(server_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) ...
```

这是 Linux 上典型的组播串扰形态: 绑 `INADDR_ANY:port` 的 socket 会收到该端口上**本机已
加入的其它组**的数据报。加上 `SO_REUSEADDR`/`SO_REUSEPORT`(`:161-163`)允许多进程共用端口,
默认配置下所有 topic 都挤在 11451。

**另一个独立问题**: `domain_id * hash` 让不同 `(domain, topic)` 可能撞到同一端口 ——
例如 `domain=1, hash=100` 与 `domain=2, hash=50` 都得到 11551。

**为什么标"未实测"**: 以上是从代码 + 已知 Linux 组播语义推出的, **我没有跑实验**。
串扰是否真的发生依赖内核版本与是否真有第二个组被本机加入。**先做实验再决定修不修**,
不能凭推理改传输层。

**判据**

实验(先做这个):
- 同机、同 `domain_id = 0`、两个不同 topic 名、都用默认 `msg_id = 0`;
- 各起一个订阅者, 只往其中一个 topic 发;
- 观察另一个 topic 的订阅者是否收到 / 是否 `check_id` 放行。

若成立才修(候选修法: 绑组播地址而非 `INADDR_ANY`, 或把 topic hash 也计入端口);
若不成立, 把实验证据记在本条并收口。

**进度**: ✅ 已修 —— 见 §修复记录 3(实验已做, **串扰实测成立**)。

---

## 4. `get(Sample&)` 无超时重载

**症状**

在只发 TLV 的话题上调用零拷贝视图路径的 `get(Sample&)`, 线程**永久挂住**。

**成因**

`get` 直接调无参 `pop`, 而 `pop` 的无超时分支是 `for(;;) + cv_.wait`:

```cpp
// src/dzIPC/shm_pub_sub_ipc.cc
void shm_sub_ipc::get(Sample& out)
{
    std::shared_ptr<Sample> s;
    view_queue_->pop(s);   /* 阻塞直到有 Sample */
    ...
```

而 `CircularQueue::pop` **本来就支持超时**, 只是没人传:

```cpp
// include/dzIPC/common/circularqueue.h:40
bool pop(MsgPtr &out, uint64_t tm = std::numeric_limits<uint64_t>::max())
```

这不是设计缺陷而是**没接线** —— 现成能力白放着。视图队列在 TLV-only 话题上永远是空的
(见 dzflat_shm.md §3.8 的严格分流), 所以这条路径注定挂死。

**判据**

- 新增带超时的重载, TLV-only 话题上超时返回 false 而非挂死;
- 不改变现有无参 `get` 的语义(仍然阻塞 —— 有调用方依赖它);
- socket 侧同步(它的视图路径恒不可用, 超时重载应立刻返回 false)。

**进度**: ✅ 已修 —— 见 §修复记录 4。

---

## 5. 组播组地址碰撞(8192 topic 实测 60%)

**这条与第 3 条同族但成因不同**, 别混: 第 3 条是"端口相同 + 绑 `INADDR_ANY`"(已修);
本条是"**组也相同**"——修完第 3 条之后它才会显形。

**实测**(探针 [`test/udp_topic_scale_probe.cc`](../test/udp_topic_scale_probe.cc), 直接用
生产函数 `hash.h` 的两个导出函数, 不重写哈希):

| domain | 组碰撞(卷入的 topic) | (组,端口)碰撞 | 最挤的组 |
|---|---|---|---|
| **0(默认)** | 4915–4998 (**60–61%**) | **同样 60–61%** | 9–10 个 topic |
| 1 | ~60% | 2–12 (0.02–0.15%) | 2 个 |
| 7 | 60–61% | 2–12 (0.02–0.15%) | 2 个 |

> **domain=7 那一格 2026-09-15 订正过**: 原值 40–41% 是**探针的伪差**, 不是真差异 ——
> 组地址只由 topic 名决定(`:38-46`), 与 domain 无关, 三个 domain 的组碰撞率**必然相同**。
> 旧值偏低的成因是探针 `measure()` 里那句 `catch (...) { continue; }`(端口越界的 topic
> 不计入): domain=7 时有 **22.7%** 的 topic 名会让端口公式抛异常而被丢掉, 它们只从
> **分子**里消失,**分母**仍是 `n = 8192`, 于是 60% × (1 − 22.7%) ≈ 41%。详见 §修复记录 6。

顺序命名 / ROS 风格命名 / `ns×node` 三种生成方式结果一致(60%、61%、59%), 说明不是命名
方式的问题。

**成因(两层)**

1. **组空间小**: 组地址 = `239.255.<fnv(t+"_mid")%254>.<fnv(t+"_end")%254>`, 名义
   254×254 = **64,516** 个。
2. **两个坐标不独立**(这一层是实测才发现的): 互信息 **I(X;Y) = 2.0 nat**, 40 万个 topic
   只触达 **12,188 个组合(18.9%)** —— 有效空间比名义值又小 5.3 倍。

   对照实验把账算清:

   | 假设 | 卷入碰撞的 topic |
   |---|---|
   | 均匀随机到 64,516 个组 | 12% |
   | 均匀随机到**真实可达**的 12,188 组 | 49% |
   | **实测** | **60–61%** |

   **坐标相关的机制**: 254 = 2 × 127, 而 FNV-1a 的最后一步是乘法。模 2 下乘以奇数等于
   恒等, 于是**最低位退化成输入字节的奇偶校验**; `_mid` 与 `_end` 的奇偶差是常数, 两个坐标
   的最低位被锁成固定关系。这解释了一半的空间损失。

**后果分两级(这个区分是判据的关键)**

- **msg_id 不同** ⇒ 只白烧带宽。收到的包被 `check_id` 挡掉 —— 它是 socket 接收侧**唯一**
  的判别(`data_rev.cc:1345`)。
- **msg_id 相同** ⇒ **真正错收**: 对方的载荷被按你的类型反序列化。而 `msg_id` 的
  **默认值就是 0**。

所以默认配置(domain=0 + 不设 msg_id)下, 60% 的 topic 处于"共组"状态, 其中相当一部分
会落到"错收"。

**建议的修法(按性价比)**

1. **把 topic hash 纳入端口**(最划算)。实测支持: domain=1 时端口参与区分, (组,端口) 碰撞
   从 60% 掉到 **0.15%** —— 要同时撞组和撞端口才算漏, 强度是平方级。改法是把
   `11451 + domain_id * hash` 换成无冲突形式(如 `base + domain*STRIDE + hash%STRIDE`)。
   不动 wire、不动组的分配。
2. **换掉组哈希**(第二优先): 消除坐标相关性, 或直接用更大空间(239.0.0.0/8 有 2^24 个组)。
   端口一改之后, 它从"致命"降级为"轻微浪费带宽"。

**注意**: 改端口公式会改变寻址, **影响与旧版本进程的互通** —— 与第 1 条同类代价。

**进度**: ⬜ 未修(已实测, 修法已定, 待决策)

---

## § 修复记录

### 1. SHM domain 隔离(2026-09-13)

**改了什么**

- **段名纳入 domain**, 并把规则收成**单一出处**: 新增
  `shm_topic_segment_name(topic, domain)` 与 `shm_service_prefix(topic, domain)` 到
  [`include/dzIPC/common/name_operator.h`](../include/dzIPC/common/name_operator.h)。
  pub/sub 段名变为 `dz_ipc_d<domain>_<sanitized>_topic`, 服务通道前缀变为
  `dz_ipc_d<domain>_<topic>`(其 `_ser_r`/`_ser_w`/`_ser_control2` 随之隔离)。
- **ser/cli 一并修**: 它的通道名同样不含 domain(`shm_ser_cli_ipc.cc` 原 `:261-262`、
  `:570-571`), 是同一缺陷的另一半。
- **五处复刻改为转调唯一出处**: 传输层 pub/sub、传输层 ser/cli、C++ sniffer
  (`exec/dzipc_topic_cat/src/shm_sniffer.cc` —— 它的注释原本就写着"sniffer 必须复刻完全
  相同的方案", 那正是该导出而非复刻的信号)、`tools/dzplot/dzplot.py`、
  `tools/dzplot/test/integration_pub_restart.py`。

**判据怎么验的**

新增 [`test/test_shm_domain_isolation.cpp`](../test/test_shm_domain_isolation.cpp) 三例:
同名 topic 跨 domain **必须收不到**、同 domain **仍互通**、两个 domain 各自成对时各收
自己的载荷。

**绿不算证据, 所以做了变异验证**: 把段名退回修复前(去掉 domain), 三例中有 **2 例立刻挂**
(`SameTopicDifferentDomainsDoNotSeeEachOther` 与 `TwoDomainsEachDeliverOwnPayload`), 确认
用例真的咬住了这个行为而不是恒绿。随后恢复。

**顺带暴露的两个既存 bug**(都是被这次隔离生效"照出来"的, 与本修复无因果):

1. `test/test_dzipc_shm.cpp` 的 `PubSub` 用例**参数写错**: 订阅方写成
   `shm_sub_ipc(msg, "TestMsg2", 10, 1, true)` —— 参数序是
   `(msg, topic, domain_id, queue_size, verbose)`, 即 domain=10/queue=1, 而发布方是
   domain=1。此前段名不含 domain 才碰巧连上; 隔离生效后它们正确地互不可见, 订阅线程
   永远等不到消息而**挂死**(表现为整个二进制超时, 不是断言失败)。已改为 domain=1/queue=10。
2. `tools/dzplot/dzplot.py` 的 `_control_plane_name_for_topic` 拼的是
   `..._topic_control`, 而 C++ 侧是**数据段名 + `_control2`**(`control_name_for`)。
   这个名字**从来就没对上过** —— 属于既存缺陷, 只是控制面打不开只会让"发布方重启检测"
   静默失效, 没人注意。已按 C++ 的派生方式改正。

**回归**: `test_dzipc_shm` 10/10 恢复; 全量 gtest 见下方结论行。

**代价与注意**: 段名变了 ⟹ **与旧版本进程不互通**。这是有意的 —— 旧进程段名不带 domain,
若还能互通就等于隔离没生效。手法与 `_control2` 同源, 注释里已写明。

### 2. 第 33 个订阅者静默黑洞(2026-09-13)

**改了什么**

订阅线程在**宣布握手完成之前**先检查连接位是否真的拿到了:

- `cc_id == 0`(= 连接位耗尽)时**不置** `handshake_completed`, 退掉已登记的 peer
  (`remove_peer`)并 100ms 后重试 —— 有订阅者退出让出位时就能接上;
- 告警**不受 `verbose_` 约束**(这是静默失败, 不该要求先开调试开关);每进程只打一次,
  避免重试循环刷屏。告警文本直接说明后果: 该订阅者收不到任何消息;
- 顺带把 `kMaxShmReceiversPerTopic = 32` 提为具名常量并注明它与控制面
  `kMaxPeerSlots`(64)的 2× 差额 —— 之前这个上限只以位宽的隐含形式存在。

信号一直都在: `acquire_peer_slot` 本来就在 `cc_id == 0` 时返回 -1
(`control_plane.cc:170-173`), 只是订阅端把它丢掉了。

**判据怎么验的**

新增 [`test/test_shm_receiver_cap.cpp`](../test/test_shm_receiver_cap.cpp): 起 33 个订阅者,
断言**控制面登记的 peer 数不超过连接位上限**。

**这里有一段值得记下的过程**: 第一版用例只断言"前 32 个收得到、收到数不超过 32" ——
**变异测试证明它没有牙**: 把修复退回原样, 用例照样全绿。原因是第 33 个订阅者修与不修都
收不到消息, 而"收不到"正是当时的判据, 两种实现下表现相同。

真正的差别在**可观测性**: 修复前那个连不上的订阅者**留在控制面登记表里冒充在线**,
`peer_count()` 会计到 33。改用这条判据后, 变异测试立刻抓到:

```
控制面登记了 33 个 peer, 超过连接位上限 32 —— 连不上的订阅者仍在冒充在线
```

这条教训是普适的: **"修复前会挂"必须实测, 不能假定**。判据要落在"修复改变了什么"上,
而不是落在"现象是否还在"上 —— 后者常常在修复前后一模一样。

**回归**: 全量 40/40 通过。

---

## 附. 审计中的一条误报(记录以免重复排查)

审计曾报"TLV 的大数组反序列化路径缺少分配前 count 闸, 会让分配出错直接 SIGABRT"。
**误报**。生成的代码里闸紧贴在 `resize` 上一行:

```cpp
/* 分配前闸: count 来自 wire, 未校验就 resize 会变成无界分配 */
if (!this->tods_count_ok(offset, pose_history_count, 4)) { return; }
pose_history.resize(pose_history_count);
```

误报的成因是检索时只看了 `grep -n "resize("` 的输出 —— 它显示 `resize` 行而看不到紧邻
上一行的守卫。全部生成头里闸都在(实测 `test_msg.hpp` 4 处、`robot_state.hpp` 3 处、
`std_image.hpp` 2 处)。记在这里是因为**这个误报模式会重复出现**: 单行 grep 看不见上下文,
而"守卫在上一行"正是防御性代码最常见的写法。

### 3. UDP 同端口跨 topic 串扰(2026-09-13)

**先做了实验**。探针: 同 `domain_id = 0`、两个不同 topic、**都用默认 `msg_id = 0`**, 只往
topic A 发布, 两个 topic 各挂一个订阅者。

先算地址确认机制:

```
probe_topic_A  port=11451  group=239.255.60.47
probe_topic_B  port=11451  group=239.255.159.106
```

组不同、端口相同。然后实测:

```
只往 A 发了 593 条;
  A 收到 = 593
  B 收到 = 593          ← B 收到了 A 的**全部**数据
串扰判定: ★ 串扰成立
```

**成因**

端口公式是**乘性**的:

```cpp
// src/dzIPC/common/hash.cc:24-25
uint64_t hash_value = fnv1a64(topic_name) % 10000;
uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + domain_id * hash_value;
```

`domain_id = 0`(默认)时整个第二项恒为 0, 端口退化成常数 **11451** —— 与 topic 名无关。
而接收端绑的是 `INADDR_ANY:port`, 于是同机所有 topic 的 socket 挤在同一端口上互相收包。
(即使 `domain_id != 0` 也有问题: `domain_id * hash` 让不同 `(domain, topic)` 可能撞同一
端口, 例如 `1×100` 与 `2×50` 都得到 11551。)

**为什么既有测试没抓到**: socket 测试里每个 topic 用了**不同的 msg_id**, 于是收到对方的包
也会被 `check_id` 挡掉 —— 串扰真实存在但被掩盖。而 `msg_id` 的默认值就是 0, 生产里大量
topic 并不设它。

**改了什么**

绑**组播组地址**而不是 `INADDR_ANY`(src/libipc/platform/posix/udp.h):

```cpp
local_addr.sin_port = htons(port);
if (::inet_pton(AF_INET, ip, &local_addr.sin_addr) != 1) { ... }
::bind(server_fd, ...);
```

由内核按组过滤, 同端口不同组不再互相投递。**寻址方案(端口/组公式)完全不动**, 所以不影响
互操作 —— 只是把"谁该收"的判断从"端口相同"纠正成"组相同"。

**判据怎么验的**

同一探针重跑:

```
只往 A 发了 592 条;
  A 收到 = 592
  B 收到 = 0            ← 串扰消失
```

**回归**: 全量 40/40 通过。

**回归固化**: 探针是一次性的, 所以把判据落成了常驻用例
[`test/test_socket_topic_isolation.cpp`](../test/test_socket_topic_isolation.cpp) ——
两个 topic **都用默认 `msg_id = 0`**(缺陷的真实形态; 既有 socket 测试因为每个 topic 用了
不同 msg_id, 收到对方的包会被 `check_id` 挡掉, 于是串扰被掩盖了这么久), 断言 B 收到 0 条,
同时断言 A 自己收到 > 0 条(否则"B 收不到"可能只是因为链路不通, 是假绿)。

变异验证: 把绑回 `INADDR_ANY` 退回修复前 —— 用例立刻抓到
`topic B 收到了 615 条本不属于它的数据`, 确认判据咬住了行为。随后恢复。

**未处理但已登记**: 端口公式的乘性冲突(`domain*hash` 可撞)未动 —— 它需要改寻址方案,
会影响互操作, 属于另一件事。若将来要动, 应改成 `base + domain*STRIDE + hash%STRIDE` 这类
无冲突的形式。

### 4. `get(Sample&)` 超时重载(2026-09-13)

**改了什么**

新增 `bool get(Sample& out, std::uint64_t tm_ms)`(基类纯虚 + shm/socket/pimpl 三处实现):

- **shm**: 接线到 `CircularQueue::pop(s, tm_ms)` —— 底层**本来就支持超时**
  (`circularqueue.h:40`), 只是从来没人传。超时返回 false。
- **socket**: 立刻返回 false。socket 永远没有 DZFlat 视图, 不该让调用方等一个不会来的东西。
- 原无参 `get(Sample&)` **语义不变**(仍阻塞)—— 有调用方依赖它。

**判据怎么验的**

[`test/test_dzflat_rx.cpp`](../test/test_dzflat_rx.cpp) 新增两例:

- `TimedGetReturnsOnTlvOnlyTopic`: TLV-only 话题上 200ms 预算内返回 false, 且判据是**时间窗**
  而非单纯返回值 —— `elapsed >= 150ms` 排除"立刻返回"(那种实现在有数据时会漏消息),
  `elapsed < 2000ms` 排除"挂死"。只断言 `got == false` 是没牙的: 一个直接 `return false`
  的实现也能过。
- `TimedGetDoesNotDisturbTheClonePath`: 空等一次之后物化路径照常工作 —— 证明上一条的
  false 不是通道故障。

**变异验证**: 把 `tm_ms` 忽略掉、退回永久阻塞(`(void)tm_ms; view_queue_->pop(s);`)——
**两个用例都没跑完就被外部 timeout 杀掉**(测程 143 = SIGTERM), 即挂死如期发生。修复是
承重的, 判据真的咬住了行为。随后恢复源码并复跑: 两例通过。

**为什么这个缺陷值得修而不只是"文档提醒一下"**: 它不是设计取舍, 而是**没接线** ——
现成能力白放着。而未接线造成的表现是"调用方永久挂住", 这是最难从症状反推成因的一类
(进程没崩、没报错、CPU 也不忙)。

---

### 6. 端口公式越界抛异常(2026-09-15)

**先量化**。探针 = 40000 个真实风格的 topic 名 × 一串 domain, 直接调生产函数:

| domain | 0–5 | 6 | 7 | 8 | 10 | 32 | 64 | 100 | 232 | 541+ |
|---|---|---|---|---|---|---|---|---|---|---|
| 抛死比例 | **0%** | 9.9% | 22.7% | 32.5% | 46.1% | 83.4% | 91.8% | 94.8% | 97.8% | **≈100%** |

**成因**

```cpp
// src/dzIPC/common/hash.cc:24-25 (修复前)
uint64_t hash_value = static_cast<uint16_t>(fnv1a64(topic_name) % 10000);
uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + domain_id * hash_value;
if (limited_port + dzIPC::common::kUdpPortOffsetMax > 65535)
    throw std::runtime_error("Calculated port number exceeds the maximum allowed value of 65535...");
```

端口值域是 `base + domain × hash`, 而 `hash ∈ [0, 9999]`、`domain` 是 `int` —— 乘积上限
`10000 × 2147483647` 比端口窗口(**54081** 个取值)大 4 亿倍。**越界是常态, 不是边界情况**:
`domain ≥ 6` 起就有 topic 名落进去, `domain ≥ 541` 几乎全落进去。而"越界就抛"只是把
"端口算不出来"翻译成了"**进程死**"。

**为什么这条比看上去严重**: 抛出的调用点全是 **socket 传输层的构造函数**
(`socket_pub_sub_ipc.cc:30/456`、`socket_ser_cli_ipc.cc:75/537`)与嗅探器
(`socket_sniffer.cc:23`、`handshake_probe.h:152`)。构造函数没有调用方接得住的余地 ——
`PublisherIPCPtrMake` / `SubscriberIPCPtrMake` / `topic_cat` 都是**用户 main 里的普通调用**:

```
$ # 修复前的库, 一行调用, 没有 catch
$ ./demo   # udp_discovery_port_calculate("g25786", 6)
terminate called after throwing an instance of 'std::runtime_error'
  what():  Calculated port number exceeds the maximum allowed value of 65535...
已中止 (核心已转储)          ← exit 134
```

而报错信息把用户指向"换一个 topic 名或 domain"—— 在多 domain 部署里 domain 是**产品语义**
不是可调参数, 于是这条路径实际上是"某些 domain 完全不可用"。

**改了什么**

`offset = domain_id × (fnv(topic) % 10000)`, 越界时把 **offset 折回**
`[0, kUdpPortWindow)` 而不是抛:

```cpp
const uint64_t folded = offset < window ? offset : offset % window;
return static_cast<uint16_t>(base + folded);
```

折回方式是被两件事同时约束的, 不是随手取模:

1. **既有合法输入的端口一个字节都不许变**。取模恰好满足: 旧实现抛异常的条件是
   `offset ≥ window`, 所以旧版本**能算出结果**的输入恒有 `offset < window`, 取模对它们
   是恒等变换。(若换成"夹到上界", 越界输入会全挤到 65531 上 —— 那是把"崩"换成"静默串扰"。)
2. **段尾也要在 65535 内**: 一个 topic 占 `base .. base+4`(offset 3/4 是 ACK 通道),
   所以基址上界是 `65531`, 窗口长度 `65531 − 11451 + 1 = 54081` —— 取模的模数就是这个数。

**兼容策略(与旧版本进程的关系)**

| 输入类别 | 旧版本 | 新版本 | 互通 |
|---|---|---|---|
| `domain ≤ 5` 的**全部**输入 | 返回端口 | **逐位相同** | 不受影响 |
| `domain ≥ 6` 且 `offset < 54081` | 返回端口 | **逐位相同** | 不受影响 |
| `domain ≥ 6` 且 `offset ≥ 54081` | **崩** | 折回窗口 | 旧版本**没有**在跑的实例(两边都在构造期崩), 无互通可言 |
| `domain < 0`(仅窄化可产生) | 多数崩; 少数无符号回绕给出**基址以下**的端口 | 折回窗口 | 见下方"如实记录的一处改变" |

所以本修复**不产生任何互通回归**: 能跑通的配置端口全不变, 唯一改变的是"旧版本必然崩"的
输入 —— 那些输入没有可互通的对端。**wire 格式、组播组公式、kUdpPortOffset\* 五条偏移全部不动。**

**怎么验的(四条独立证据)**

1. **二进制级 A/B**: 同一份调用表分别链修复前(`build_anchor/lib`, 仍是旧公式)与修复后的
   `libipc` 打表 —— 除那一行 `THROW → port` 外**完全一致**。
2. **大规模逐位对账**(4 万 topic × 601 个 domain): 旧实现能算出结果的 **1,232,421 例全部
   逐位相同**(0 例不一致), 旧实现抛死的 23,049,400 例全部折回窗口内, 段尾越界 0 例。
3. **常驻用例** [`test/test_udp_port_boundary.cpp`](../test/test_udp_port_boundary.cpp):
   8 例。承重的两条互为正反 —— `LegacyInputsAreBitIdentical`(旧能算出结果 ⇒ 逐位相同)与
   `FoldIsNotIdentityOnIllegalInputs`(旧抛死 ⇒ 必须**真的被改动且不退化成分夹取**)。
   金标值全部录自**修复前的库**, 不是现算现抄。末例是端到端: 一个修复前必然打死进程的
   `(topic, domain=6)` 现在真的能收发(`msg_id` 取默认 0)。
4. **变异验证**: 夹取修法 → 2 例红; 保留抛异常 → 6 例红; 改公式(`% 10000` → `% 9999`)→
   4 例红(含逐位对账那一条)。判据咬的是行为, 不是"代码里出现了某个常量"。

**如实记录的一处行为改变**: `domain_id` 是 `int`, 而公共 API 传的是 `size_t` —— 窄化后可能
为负。旧实现下负数**多数崩、少数**靠无符号回绕静默返回一个**基址以下**的端口(`domain=-1`
时恒为 `11451 − hash`, 甚至能算出 0)。新实现一律折回窗口: 确定、在界内、不抛。这属于
"把静默垃圾改成确定值", 只可能影响**显式传负数 domain、且两端都用同一份旧库**的调用方
(公共 API 不产生负数)。判据在 `NegativeDomainFoldsInsteadOfWrappingBelowBase`。

**顺带订正了一处测量伪差**: §5 表里 `domain=7` 那一格原为 40–41%, 该值是探针
`catch (...) { continue; }` 把 22.7% 的越界 topic 从**分子**剔除、分母却仍是 8192 造成的。
修复后探针不再丢样本, 该格回到 60–61% —— 与"组地址与 domain 无关"的机理一致。

**与第 5 条的关系**: 本条是**兜底**, 不预设第 5 条的端口重设计。若第 5 条按建议改成
`base + domain*STRIDE + hash%STRIDE`, 越界输入会天然消失(那时本条的历史映射保证需要随
发布决策一起作废 —— 第 5 条本来就要改所有端口)。

**回归**: 端口相关用例 33/33(隔离/端点分离/CRC/借用/握手探针) + 本文件 8/8;共用同一个
`libipc` 的核心 SHM 用例不受影响(本条只改端口派生, 不碰 SHM 路径)。

---

## 附: 当前状态

**第 1–4、6 条已闭环**, 每条都有**行为判据 + 变异验证**(第 3 条另有实测实验数据);全量
gtest **41/41** 通过(见 §修复记录各条的"回归"行 —— 该数取自第 1–4 条的复跑;第 6 条另加
8 例, 未重跑全量)。

**第 5 条已实测、修法已定、待决策** —— 它比第 3 条更值得优先处理: 第 3 条影响的是
"同机不同 topic 的默认配置", 而第 5 条在 **8192 topic 的规模下 60% 的 topic 落在共组
状态**, 且默认 `msg_id = 0` 时直接是静默错收。修法(端口纳入 topic hash)成本与第 1 条
同级, 但**同样有"与旧版本不互通"的代价**, 所以要和发布节奏一起决定。

顺带记一条方法论: 第 5 条我最初给的是**生日模型估算**(256 topic 时 39.7%), 实测是
**60%** —— 高了 5 倍。差异来自两处: 名义空间(64516)本身就不大, 而**两个坐标还不独立**
(有效空间只剩 18.9%)。后者是**只有实测才能发现**的。凡是拿概率模型支撑决策, 都该有
一次实测兜底。

---
