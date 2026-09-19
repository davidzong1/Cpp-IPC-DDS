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
> **未修条目的去处**: §7 排查中又发现几条同类缺陷(分配失败 → 空指针、共享池无崩溃回收、
> 库接管进程退出), 它们**没有**在本文件里展开, 已单独收口到
> [unfixed_defects.md](unfixed_defects.md) —— 要看"还有哪些没修"看那份。

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
| 7 | 大消息 `buff_t` 的析构器**解引用已释放的 conn_info** → 偶发 `SIGSEGV at 0`(写 NULL, 内核日志只剩一个 ip) | **高(偶发且不可复现)** | 低 | ✅ 已修(ASAN 坐实 + 变异验证, 见 §修复记录 7) |

**修复顺序**: 1 → 2 → 3 → 4。第 1 条排头不只因为影响最大, 还因为它是**任何 DDS 门面的
前提**(见 [dds_interface_roadmap.md](dds_interface_roadmap.md) 路 A) —— 一个不隔离 domain 的
`DomainParticipant` 是在宣告传输层并不提供的保证。

> ⛔ **本表的 1–7 号只覆盖"传输层既存缺陷"。** 另有 **§ 修复记录 8**(2026-09-18, 分配失败注入工装
> `UF-000`)—— 它交付的是**工装**而非缺陷修复, 因 [unfixed_defects.md](unfixed_defects.md) 0.4 写回协议
> 第 3 步而落在本文, **不进本表**。同理 **§ 修复记录 12 / 13 / 14 / 15**(`UF-001` / `UF-002` /> `UF-006` / `UF-004`)也都是按同一协议搬进来的 `UF-*` 条目, ⛔ **同样不进本表**。
> 查 `UF-*` 缺陷台账请看那份的 0.1 表; 本文 § 修复记录编号与 `UF-` 编号**不是一一对应**。

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

### 7. 偶发 `SIGSEGV at 0`(写 NULL, in libc): 大消息 buff_t 的析构器解引用已释放的 conn_info(2026-09-17)

**现象**。内核日志里两次、同一签名:

```
python3[1417097]: segfault at 0 ip 00007c53f6fa0b7e sp 00007ffe68548f88 error 6 in libc.so.6
Code: ... c5 fe 6f 4e 20 <c5 fe 7f 07> c5 fe 7f 4f 20 49 89 f8 49 83 e0 3f ...
```

- `at 0` + `error 6`(写、非存在页) + 指令 `<vmovdqu %ymm0,(%rdi)>`(`rdi=0`) —— 这是 glibc
  AVX 拷贝循环在往**空指针**写 32 字节。
- 进程名 `python3`, 两次都落在同一条命令上的 `test/test_dzflat_python.py` 循环里; 而且**该文件
  全部断言都已通过**(一次甚至是在第 `[9]` 节执行中途), 所以从应用侧看它"与本轮改动无关"。
- 频率: 约 30 次运行 2 次 / 另一次 4 次运行 1 次 —— 典型的堆运气依赖。

**先量化、再定位**。内核日志只有一个 ip, 现场不足以反查, 所以分三步收敛:

1. **守门人**: 预加载一个 SIGSEGV/SIGBUS 处理器(打印寄存器 + `backtrace_symbols_fd`)后循环跑真实
   复现脚本, 抓到完整 C 层栈:

```
__memmove_avx_unaligned_erms   ← 写 (nil)
  basic_string<...>::_M_construct<char*>(char*, char*)
  (anonymous)::chunk_handle_t::get_info(conn_info_head*, size_t)   ipc.cpp:324
  (anonymous)::chunk_storage_info(conn_info_head*, size_t)         ipc.cpp:377
  (anonymous)::recycle_storage<flag_t>(...)                        ipc.cpp:504
  detail_impl<...>::recv(...)::{lambda(void*, size_t)#2}::_FUN     ipc.cpp:1304
ipc::buffer::~buffer() → buffer_::~buffer_()
```

2. **符号化**: `addr2line` 解析 libipc 的三个偏移 → `chunk_storage_info` / `get_info` /
   `recv(...)::{lambda(void*,unsigned long)#2}::_FUN`。结论: 崩点在"**大消息(storage 路径)的
   `buff_t` 析构器**"里。
3. **坐实**: 用 ASAN 单独构建 libipc + 一个 30 行探针(见下), 由 ASAN 直接给出 heap-use-after-free
   与 alloc/free 双方栈。

**根因**(两个缺陷叠加。前者是 bug, 后者只负责把它放大成"写 NULL"):

1. **use-after-free(真正的 bug)**。大消息的 `buff_t` 析构时要"归还 chunk", 为此需要 `CHUNK_INFO__<size>`
   段名, 而段名 = **接收方 `conn_info` 里的前缀**。旧实现把 `conn_info_t *inf` 这个裸指针捕进
   `recycle_t`(ipc.cpp:1280 附近), 于是"**消息活过接收方**"这个完全合法的顺序 ——
   接收方析构 → `chan_impl::destroy` → `mem::free(conn_info)`(ipc.cpp:828), 之后消息才析构 ——
   就是在读已释放内存。`conn_info_t` 实测 224 字节, 正好落在 tcache 的尺寸类里: `free()` 时 glibc
   把 `next`/`key` 写进块头(offset 0 / 8), 恰好就是 `prefix_._M_p` / `_M_string_length` 的位置,
   于是"字符串长度"变成**一个指针值**(实测 135193289566176 ≈ 1.35e14)。
2. **放大器**。`allocator_wrapper::allocate` 是 `noexcept` 且失败/越界**返回 `nullptr`**, 而不是按标准抛
   `bad_alloc`。libstdc++ 的 `basic_string::_M_construct` 因此拿不到异常, 直接
   `memmove(nullptr, src, huge_len)` → 写地址 0。换言之, **在这个分配器下, 任何一次"分配返回空"
   都会以"写 NULL 的 SIGSEGV"收场**, 这也是为什么本文件里两类不同缺陷的现象长得一模一样。

**为什么"偶发"**: UAF 只有在"那块内存已经被别人改写"时才崩; 没被改写就只是读到完好的旧值,
一切照常。所以它是**析构顺序 + 堆布局**双重依赖的 —— 这正是"一次性、复现不了"的来源。ASAN 下必报。

**复现(确定性)**。序列就三步: 同进程 tx+rx → 发 8192 字节(走 storage 路径) →
`held = rx.recv()` → **析构 rx** → 用同尺寸类分配毒化刚释放的块 → 析构 `held`。
它已经落在 `test/test_chunk_hold.cpp` 的 `ChunkHold.HeldLargeMessageSurvivesItsReceiver` 里
(带毒化 ⇒ 不需要 ASAN 也有牙)。

下次再遇到"只有一个 ip 的 SIGSEGV"时, 用 ASAN 单建 libipc + 一个几十行的同序探针就能拿到因果;
不必重configure整仓 —— libipc 只有 12 个 TU(注意 `a0_*` 是 C, 要用 gcc 先编成 `.o`):

```bash
gcc -fsanitize=address -g -I src -I src/libipc/platform -I src/libipc/platform/linux -I include \
    -c src/libipc/platform/platform.c -o /tmp/platform.o
g++ -std=c++17 -fsanitize=address -g -DLIBIPC_LIBRARY_SHARED_USING__ \
    -I include -I src -I src/libipc/platform -I src/libipc/platform/linux -I . \
    probe.cpp src/libipc/{buffer,ipc,pool_alloc,shm,sniffer}.cpp src/libipc/socket/udp.cpp \
    src/libipc/sync/*.cpp src/libipc/platform/posix/shm_posix.cpp /tmp/platform.o \
    -o probe_asan -lpthread -lrt
```

| 构建 | 结果 |
|---|---|
| ASAN, 修复前 | `ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 in basic_string::_M_data()` + alloc/free 双栈 |
| ASAN, 修复后 | 无任何报告(探针打印"存活") |

**修法**(`src/libipc/ipc.cpp`, 只动这一条路径):

- `recycle_t` 不再存 `conn_info_t*`, 改存**前缀的拷贝**(接收时拷 —— 那一刻 conn_info 必然活着)。
- `recycle_storage` / `chunk_storage_info` / `chunk_handle_t::get_info` 的入参由 `conn_info_head*` 改成
  `ipc::string const&`(前缀按值) ⇒ **析构路径上不再有任何对 conn_info 的解引用**。
- 其余四个仍收 `conn_info_head*` 的调用点(`acquire_storage` / `find_storage` / `release_storage` /
  `discard_storage`)一律改成"判空 + 传 `inf->prefix_`", 行为不变(它们只在活着的句柄上被调用)。

为什么不加锁/不加引用计数: 这条路只需要一个**值的拷贝**(前缀), 把裸指针换成值拷贝是零成本且无死锁
风险的修法; 引用计数要改句柄的生命周期模型(connect/destroy 的所有权), 风险与收益不成比例。

**验证**。

- **回归用例带牙**: `test/test_chunk_hold.cpp` 新增 `ChunkHold.HeldLargeMessageSurvivesItsReceiver`
  (同顺序 + 用同尺寸类分配把释放块毒化, 不依赖 ASAN)。**变异验证**: 只把 `src/libipc/ipc.cpp` 换回
  HEAD(修复前)重建, 该用例让整个测试二进制当场中止(栈落在 `ipc::buffer::~buffer()`); 换回修复版即 `3/3` 通过。
- **真机复现脚本**: 修复后 40 轮 `test/test_dzflat_python.py`(带守门人)+ 修复前 30 轮 2 次 / 4 轮 1 次对比。
- **全量**: gtest **55/55 二进制通过**(含 `test_chunk_hold` 3/3、`test_ipc`、`test_lap_safety`、`test_loan`、
  `test_dzflat_*` 全族)。

**同一族的就地加固**(与本条同时做的, 都很小): `mem::alloc<T>` 在就地构造**之前**判空(否则构造
函数往地址 0 写)、`make_cache` 判空后**按丢包处理并打一次性告警**、`cache_t::append` 连
`buff_.data()` 一起判、`ipc::buffer` 的 `empty/data/size` 容忍空 impl(它们自己也可能分配失败)。
这几处都只是"把崩溃降级为丢一条消息 + 一句日志", 不改协议、不改行为。

**附带发现(未修)**: 这三条本轮**没有**修, 已单独收口到
[unfixed_defects.md](unfixed_defects.md)(登记表 + 修法选项 + 复现手段), 此处只留摘要与出处,
避免两处各自漂移。

1. **崩溃会在共享段里留永久垃圾**(该文档 §3)。chunk 池(`/dev/shm/__IPC_SHM__CHUNK_INFO__<size>`)是
   **跨进程共享**的, 池里"仍被持有"的位只在持有者归还时清 —— 进程被段错误杀死 = 那些 chunk 永久卡住,
   之后**每个**进程看到的都是残池。实测: 上面那次中止之后, `ChunkHold.OverwrittenChunksAreReclaimed`
   在同尺寸类上稳定失败(20 条只有 10 条能进环, 大消息退化成 128 槽/条分片); 清掉这些段后恢复 3/3 通过。
   **换回修复前代码也是同样的 10/20**, 所以它**不是**本次改动引入的(清理前已确认没有任何活进程映射这些段)。
   这条值得单独记: 一次崩溃会**污染后续所有进程**的行为, 排查时容易误判成"新引入的回归"。
2. **同一族(分配失败 → 读/写地址 0)还有两处未修**(该文档 §1、§2): ① `ipc::shm::handle` 的
   `pimpl<handle_>` 分配失败后全线 `impl(p_)->...` 解引用空指针(定向注入 malloc 失败可稳定复现, 崩在
   `handle::release`), 同族还有 `UDPNode` / `mutex` / `condition` / `semaphore`(`buffer` 已加固);
   ② `allocator_wrapper::allocate` 的 noexcept-nullptr 语义仍在。两者都需要**真实 OOM** 才触发(与本次 UAF
   只需要"析构顺序"不同), 所以这一轮只就地加固了 `mem::alloc<T>` / `make_cache` / `cache_t::append`
   (判空后降级为丢弃 + 一次性告警), 其余登记在 [unfixed_defects.md](unfixed_defects.md)。
3. **`dzIPC::StartShutdownMonitor` 让库接管进程退出**(该文档 §4): 覆盖应用的 `SIGINT`/`SIGTERM`
   处理器、在 detached 线程里 `std::exit(0)`、退出码恒 0。本轮排查中它是首要嫌疑, 后由 ASAN 排除
   因果关系 —— 风险本身未证伪, 且**全部属于推理**, 第一项工作是实验。
   ⚠️ **2026-09-18 订正**: "**全部属于推理**"与"第一项工作是实验"**均已过期** —— 机制链已由 LD_PRELOAD
   **仪器实测坐实**(库在四个工厂入口覆盖应用处理器), 且**已提供 opt-out 开关** `dzIPC::DisableShutdownMonitor()`
   (判定 `🔶部分修`)。但本轮**只**解除"库强装处理器"这一半: `std::exit(0)` / 退出码恒 0 /
   非主线程 `exit` 三条**一条都没消** ⇒ 见 [unfixed_defects.md](unfixed_defects.md) 0.3.12 与本文 **§ 修复记录 15**。

**回归**: 本条不动端口、不动 wire、不动队列, 只把"析构路径要用的数据"从裸指针改成值拷贝;
`test_chunk_hold` 3/3 + 全量 55/55。

### 8. 分配失败注入工装 UF-000(2026-09-18)

> ⚠️ **本条不是本文 §0 一览 1–7 号里的任何一条 SHM 缺陷** —— 它交付的是**工装**(工具), 不是产品修复。
> 落在这里是因为 [unfixed_defects.md](unfixed_defects.md) **0.4 写回协议第 3 步**明确要求"详情搬到本文
> 新增「§ 修复记录 N」一节, 沿用『行为判据 + 变异验证』格式"; 指针留在那份 0.1 表的 `UF-000` 行。

**改了什么**

- ⛔ **零产品码改动**(`src/libipc/**` 一个字节都不动), 全部是**新增**:
  - `tools/alloc_fault_inject/alloc_fault_inject.c` —— `LD_PRELOAD` 拦截器(`malloc`/`calloc`/`realloc`);
  - `tools/alloc_fault_inject/fi_selftest.c` —— 工装自测(裸 `malloc` 摆确定序列, 断言语义与三个计数);
  - `tools/alloc_fault_inject/fi_positive_control.cc` —— 阳性对照(在已知含缺陷路径上跑真实注入);
  - `tools/alloc_fault_inject/build.sh`、`run_acceptance.sh`、`README.md`;
  - `test/test_alloc_fault_inject.cpp` —— UF-001/UF-002 的**注入态判据用例**(3 例, 由
    `test/CMakeLists.txt` 的 `file(GLOB)` 自动收进构建树)。
- 工装**不进 CMake 树**(与 `tools/sercli_live_probe` 同约定: 自带 `build.sh`, 拿仓库已有头与
  `build/lib/libipc.so` 编)。判据用例用 **weak 符号**引用工装接口 ⇒ 不需要 `-ldl`、不改
  `test/CMakeLists.txt`。

**为什么必须先有它**(它解决的问题不是"多一个工具"): 没有"尺寸档拦截计数", **"没注入"与
"注入了但代码正确"不可区分** ⇒ UF-001/UF-002 的判据会退化成"不注入时全绿", 而那是**回归不是判据**。
故工装设了两条硬门: ①未加载 ⇒ `GTEST_SKIP`(不是绿); ②`hits >= 1` 硬断言**先于**一切业务断言。

**判据怎么验的**(命令 / 结果 / 日期; 本节的"行为判据"就是**工装本身的行为**)

日期 **2026-09-18**, 基线 HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
artifact = **`/tmp/uf000_acceptance.log`**(252 行; ⚠️ 在 `/tmp`, **非持久**)。
一键重放: `tools/alloc_fault_inject/run_acceptance.sh`。

| 步 | 命令 | rc | 关键读数 |
|---|---|---|---|
| 1 | `tools/alloc_fault_inject/build.sh` | **0** | 产出 `.so` + `fi_selftest` + `fi_positive_control` |
| 2 | `LD_PRELOAD=…/liballoc_fault_inject.so …/fi_selftest` | **0** | 17 条 `SELFTEST ok`(9+5+3 三阶段) + `SELFTEST RESULT: PASS` |
| 2b | `…/fi_selftest`(**不带** `LD_PRELOAD`) | **2** | `SELFTEST BLOCKED: 注入工装未加载(缺 LD_PRELOAD) —— 未执行任何断言, 不得当通过` |
| 3 | `LD_PRELOAD=… …/fi_positive_control`(dtor, 自标定) | **139** | 自标定 `sizeof(handle_)=64`; 宽档 `[1,4096)` 内构造期分配=1; 注入档 `[64,65)`; **实际拦截=1**; `~handle` 处 **SIGSEGV** |
| 3b | `… fi_positive_control 0 0 0 valid` | 3 | 0 命中 ⇒ 工装**自报**"尺寸档未命中 ⇒ 工装没生效(不得当通过)" |
| 3c | `… fi_positive_control 56 57 0 dtor`(**辅助档**) | 3 | **未命中**(`band_calls=0 hits=0`) —— 见下方"代价与注意" |
| 3d | 直方图步(`DZIPC_FI_HISTOGRAM=1`) | 139 | 崩在**打印直方图之前** ⇒ 该步**未产出直方图** |
| 4 | `cmake -S . -B build` | **0** | GLOB 收进 `test_alloc_fault_inject` |
| 4 | `cmake --build build -j8 --target test_alloc_fault_inject` | **0** | 链接成功 |
| 5 | `LD_PRELOAD=… ./build/bin/test_alloc_fault_inject` | **139** | `DisabledHarnessDoesNotIntervene` OK; `HandlePimplAllocFailureMustNotCrash` **SIGSEGV** |
| 5b | `./build/bin/test_alloc_fault_inject`(**不带**注入) | 0 | `[ SKIPPED ] 3` + `[ PASSED ] 0` ⇒ **不是绿** |
| 5c | 注入态只跑健全性用例 | **0** | `[ PASSED ] 1` |
| 6 | `git diff --check` | **0** | 无空白错误 |

**变异验证**(本文的既定口径: 行为判据 + 变异验证, 缺一不算闭环)

1. ⛔ **去掉 `LD_PRELOAD` ⇒ 必须 SKIP/BLOCKED, 不得绿** —— 实测步骤 5b `3 SKIPPED / 0 PASSED`;
   工装自测步骤 2b `BLOCKED` + rc=2。⇒ 假绿路径**已堵死**。
2. ✅ **旧态(未修)阳性对照必须崩** —— 步骤 3 `rc=139`; 判据用例本身步骤 5 `rc=139`。两条**独立**
   (前者是专用复现程序, 后者是被测断言)。
3. ✅ **注入可观测性** —— 退出时**总是**打
   `[alloc_fault_inject] FINAL active=… band=[lo,hi) skip_n=… max_fails=… total_calls=… band_calls=… skipped=… hits=…`;
   阳性对照另打 `实际拦截次数=N`。

**代价与注意**

- ⚠️ **"回滚修复态 ⇒ 某已知用例转红"本轮不适用**: UF-001/UF-002 **尚未修**, 没有"修复态"可回滚。
  本轮给的是**等价证据**(未修态上判据用例 rc=139 + 独立复现程序 rc=139)。⇒ **修完这两条后仍须补
  一次真正的回滚变异验证**, 本次不算已做。
- ⚠️ **辅助档 `[56,57)` 未命中**(步骤 3c, rc=3), 如实记录**不夸大**: `56` 是
  `tools/alloc_fault_inject/README.md` §1 示例里写死的 `sizeof(handle::handle_)`(**推算值**), 本轮**实测
  自标定为 64** ⇒ **该 README 示例已过期**(承重路径是**自标定**, 步骤 3 命中且唯一, 故后果有限)。
  ✅ **2026-09-18 已订正**(writer, 单写者): 三处写死尺寸全改为**自标定调用**, 并加了一张**尺寸取值表**
  (`56` 标为 ❌过期·推算·从未实测, `64` 标为 ✅2026-09-18 实测), 另写明 `rc=3`(档未命中)与
  `rc=4`(没崩 ⇒ 可能已修)**含义相反, 别读反**。文件 133 → **159** 行,
  md5 `0ae3ae57413e74efe9be0c3ec3d0ab30`。⚠️ 同一数字仍留在 `run_acceptance.sh:47-52` 与
  `fi_positive_control.cc:22`(**本轮未订正**, 见下条)。
- ⚠️ 步骤 3d 的**尺寸直方图未产出**(进程在退出打印之前就崩)。它是**辅助**自证手段; 拦截计数已由
  FINAL 行提供。⇒ 不得记成"直方图已验证 56"。⛔ **2026-09-18 定位到根因 —— 结构性, 不是运气**:
  `run_acceptance.sh:50-53` **传了 argv** ⇒ `fi_positive_control.cc:93-97` 走显式档分支, 而 `:121` 把
  `max_fails` **写死为 1** ⇒ `DZIPC_FI_ALLOC_MAX_FAILS=0`(只计数不拦截)被**覆盖**, 第 1 次档内分配
  就被拦空而崩, **永远到不了直方图打印**; artifact 里那行 `max_fails=1`(而非 env 给的 0)即实证。
- ⚠️ **工具侧两处"同一过期数字"残留, 本轮未订正**(2026-09-18 读码定位; 本轮写边界只授权改 README):
  `run_acceptance.sh:47-48`(步骤 3c 的 banner 写"期望 rc=139", 档位却是过期的 `56 57` ⇒ **每轮重放
  必得 rc=3**, 读起来像"验收失败", 实为档过期)与 `fi_positive_control.cc:22`(头注释里的显式档示例
  写死 `56 57 0 dtor`, 照抄即踩过期档)。两者**不改变**本轮结论, 但会把"看起来像失败"的读数**传染给
  下一次重放**, 且 3c/3d **同时失效**易被误读成"辅助档全崩 = 工装坏了"。
- **覆盖边界**(README §5): 只拦 `malloc`/`calloc`/`realloc`; **不拦** `aligned_alloc`/
  `posix_memalign`/`memalign`/`valloc`/`mmap`。libipc 的 `static_alloc::alloc` 就是 `std::malloc`
  (`src/libipc/memory/alloc.h:20-27`) ⇒ 该边界**不影响** UF-001/UF-002。并发下"第 N 次命中"归属不确定。
- 该工装是 **0.3 第 5 条「每条必须附变异验证」的唯一例外** —— 它自己是判据的提供者, 故验收字段由
  [unfixed_defects.md](unfixed_defects.md) **0.3.1** 单独补齐(三条), 写回由 **0.3.2** 承载。

---

### 12. allocator `allocate` 的 `noexcept`-nullptr 语义 UF-001(2026-09-18)

> ⚠️ **本条不是本文 §0 一览 1–7 号里的任何一条 SHM 缺陷** —— 它是 [unfixed_defects.md](unfixed_defects.md)
> **0.1 表**登记、**本体未登记在本文**的条目(`UF-001`)。落在这里是因为那份文档 **0.4 写回协议第 3 步**
> 明确要求"详情搬到本文新增「§ 修复记录 N」一节, 沿用『行为判据 + 变异验证』格式"; 指针留在 0.1 表
> 的 `UF-001` 行, 条目块见其 **0.3.6**。
>
> ⛔ **本条与 §8 的关系**: §8 交付的是**工装**(`UF-000`, 判据的提供者); 本条是**第一次真正用它**
> 判掉一条产品缺陷。⇒ "UF-001/UF-002 判据不可执行"这个前置**到此为止**。

**改了什么**

- `src/libipc/memory/allocator_wrapper.h` —— `allocator_wrapper<T, AllocP>::allocate` 不再把失败
  "压成空指针":
  - **去掉 `noexcept`**;
  - `count > this->max_size()` ⇒ `throw std::length_error("...: count exceeds max_size")`
    (原: `return nullptr`);
  - `alloc_.alloc(count * sizeof(value_type))` 得 `nullptr` ⇒ `throw std::bad_alloc()`
    (原: 把 `nullptr` 交给调用方);
  - 补 `#include <new>` 与 `#include <stdexcept>`。
- ⛔ **零其它产品码改动**。这是 **§1「修法选项 1」**, 不是 0.6 第 9 条记的"入口加闸"那个变体 ——
  后者治的是 **dzflat/TLV 反序列化的无界分配**(`test/test_deser_alloc_guard.cpp`),
  **完全不覆盖 allocator 语义**。
- **为什么这算修**: 本条被登记为**放大器** —— 分配失败原本被翻译成"往地址 0 写"的 SIGSEGV,
  崩点**在很久之后、别处**且无诊断(§7 那次崩溃就是它把 use-after-free 放大的)。
  拆掉 `noexcept` 后, "失败"重新成为一个**调用方接得住的异常**。

**判据怎么验的**(命令 / 结果 / 日期; 由 coder 执行, 非本节作者)

```bash
make -C build test_alloc_fault_inject                              # rc=0
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  ./build/bin/test_alloc_fault_inject                              # 注入态: 3/3 PASS
./build/bin/test_alloc_fault_inject                                # 无注入: 3 SKIP / 0 PASS
./build/bin/test_ipc && ./build/bin/test_shm && ./build/bin/test_sync && ./build/bin/test_uf007
                                                                   # 8 + 8 + 5 + 2 全 PASS
```

- **行为判据**: 注入态 **3/3 PASS**, 且承重读数是 **`hits=1` / `band_calls=1`**
  (总分配 `total_calls=23`) ⇒ **注入确实命中**; 断言的是"分配失败必须抛 `std::bad_alloc`",
  而**不是**"没崩"。命中计数先行是 0.3.1 第 3 条的硬纪律 —— 没有它, "没注入"与"注入了但代码对"
  **不可区分**, 全绿会是**假绿**。
- **无注入态 3 SKIP / 0 PASS**: 工装未加载时该用例**绝不绿色通过**(0.3.1 第 2 条)。SKIP = 无判据力,
  ⛔ 不是"通过"。
- **回归**: `test_ipc` 8 / `test_shm` 8 / `test_sync` 5 / `test_uf007` 2 **全 PASS** ⇒ 改动未外溢。
- **日期**: 2026-09-18。**基线** HEAD = `3818899687efefd35f8514acdff565226bec7cce`。

**变异验证**: **已做**。回滚本条改动(恢复 `noexcept` + `return nullptr`)后重跑 ⇒ **rc = 139**(SIGSEGV);
转红的是"分配失败须抛 `std::bad_alloc`、不得以信号终止"这条断言。⇒ 判据**承重**。
⚠️ 注意这与 §8 那条不同: §8 是**工装**, 自己就是判据的提供者, 故它是"行为判据 + 变异验证"这条
通用规矩的**唯一例外**(由 0.3.1 单独补验收字段, 其中"回滚变异"当时仍欠一次); **本条有回滚读数**。

**代价与注意**

- ⚠️ **射程只到 `allocator_wrapper::allocate` 一条入口**: `mem::alloc<T>`
  (`include/libipc/pool_alloc.h:87-97`)仍按"**失败返回 `nullptr` 而不抛**"工作, 本轮**不改** ——
  那一侧由调用方判空收口, 见 §13。
- ⛔ **本条已于 2026-09-18 过期(订正; 勿再引用旧文)**: 原文写 `src/libipc/buffer.cpp:59`
  (`~buffer()` 的 `p_->clear()`)仍是**无条件解引用 `p_`** —— 与 §13 修的五类同型, 但 `buffer`
  **不在**那五类清单内 ⇒ **本轮未改**。⚠️ **该行今日已被 coder 改动** ⇒ "未改"**已不成立**;
  ⛔ **但改动不构成修复(回滚变异阴性)**。准确现状与裁决口径见 §13"代价与注意"第 1 条与
  [unfixed_defects.md](unfixed_defects.md) **0.3.8(7)**。
- ⚠️ 本节数字**来自 coder seq135**; 本节作者只做**静态复核**(`rg` + `git diff`), **未重跑**该批命令。

---

### 13. 五类 pimpl 入口的失效态判空 UF-002(2026-09-18)

> ⚠️ **同上**: 本条也是 0.1 表登记的条目(`UF-002`), **本体未登记在本文**。指针留在 0.1 表的
> `UF-002` 行, 条目块见 [unfixed_defects.md](unfixed_defects.md) **0.3.7**。
> 两条**必须一起读**: §12 拆掉了"函数级"那层放大器, 本条拆掉"构造期"那层。

**改了什么**

`p_ == nullptr` 是**失效态**, 不是不可能发生的状态: `pimpl<T>` 走"不舒服"分支时 impl 在堆上,
`mem::alloc<T>` **失败返回 `nullptr` 而不抛**(`include/libipc/pool_alloc.h:87-97`), 构造函数不检查。
**0.6 第 3 条已确证"光加判空不够"** —— `handle::release()` 首句是 `impl(p_)->id_ == nullptr`,
**先解引用 `p_` 再判 `id_`**, 而 `~handle()` 首句即 `release()` ⇒ `p_ == nullptr` 时**必崩**。
故本轮改的是**解引用次序**, 并按 §2「修法选项 1」= **入口判空 + 失效态空转**收口, 五类逐处覆盖:

| 文件 | 类 | 失效态处置(节选) |
|---|---|---|
| `src/libipc/shm.cpp` | `shm::handle` 族 | `valid()` 假 / `size()` `0` / `name()` 空 / `ref()` `-1` / `detach()` 空转 / `release()` `-1` / `acquire()` 报错返回 |
| `src/libipc/socket/udp.cpp` | `UDPNode` | `create()` 报错返回 / `connect()`·`send()` `false` / `role()` 缺省 `SendRecv` |
| `src/libipc/sync/mutex.cpp` | `mutex` | `open()` 报错 `false` / `native()` `nullptr` / `valid()` `false` / `close()`·`clear()` 空转 |
| `src/libipc/sync/condition.cpp` | `condition` | 同上(同族同口径) |
| `src/libipc/sync/semaphore.cpp` | `semaphore` | 同上(同族同口径) |

- ⛔ **析构也是入口**: `~mutex()` 旧实现的第二句 `p_->clear()` 与首句 `close()` 一样是**崩点**;
  `~udp::UDPNode()` 走 `delete p_`(安全)但其上一句 `close()` 在旧实现里要解引用 `p_`;
  `~handle()` 见上。故五类的**析构一并判空**。
- ⛔ **不动全局错误模型**: 失效态由"取原生句柄得 `nullptr` / 加解锁得 `false`"暴露;
  ⛔ **刻意不新增枚举值** —— 加值会改枚举的取值范围(ABI/语义), 而失效态本身已由 `connect()`/`send()`
  的 `false` 暴露, 不需要靠 `role()` 说谎(理由原样写在 `udp.cpp` 内注释)。
- ⛔ **`utility/pimpl.h` 本身未改**: 仍是"失败返回空"的约定 —— 即**保留**"失败可被观察到",
  而不是改成"失败即抛"(那就是另一条修法了)。

**判据怎么验的**(与 §12 **同一次运行**; 同一份注入态批次里 `HandlePimplAllocFailureMustNotCrash`
就是本条的路径)

- **行为判据**: 注入态 **3/3 PASS**, 承重读数 **`hits=1` / `band_calls=1`**; 在该前提下断言的是
  **失效态语义**(`valid()` 为假、`size()` 为 `0`、`detach()` 得 `nullptr`、`release()` 得 `-1`),
  而**不是**"没崩"—— 后者单独不成判据。
- **无注入态 3 SKIP / 0 PASS**;**回归** `test_ipc` 8 / `test_shm` 8 / `test_sync` 5 / `test_uf007` 2 全 PASS。
- **日期**: 2026-09-18。**基线** HEAD = `3818899687efefd35f8514acdff565226bec7cce`。

**变异验证**: **已做**。回滚本条改动后重跑 ⇒ **rc = 139**(SIGSEGV); 转红的是"分配失败后
`valid()` 必须为 `false`"与"`release()` 应返回失败码**而非崩**"这两条断言 —— 即回到 0.6 第 3 条
记载的崩溃形态(读 `0x8`)。⇒ 与 §12 **各自独立回滚, 两项分别 rc139**。

**代价与注意**

- ⛔ **遗留一栏已于 2026-09-18 过期(订正; 勿再引用旧文)**: 原文写 `src/libipc/buffer.cpp:59`
  (`~buffer()` 的 `p_->clear()`)仍是**无条件解引用 `p_`** —— 与本节五类**同型**, 但 `buffer`
  **不在**清单内 ⇒ **本轮未改**。⚠️ **该行今日已被 coder 改动**(仅析构那一行 + 注释, `+11/−1`)
  ⇒ "本轮未改"**已不成立**, 旧文作废。
  ⛔ **但改动不构成修复 —— 回滚变异为阴性**: 还原该行后重跑同一注入探针 **rc = 0, 不崩**; 成因是
  `pimpl<T>::clear()` 只做 `clear_impl(static_cast<T*>(this))`, 而"不舒服"分支即 `mem::free(p)`,
  传空指针**立即返回、从不读 `this` 的内存**(gcc 反汇编自出 `test rbp,rbp; je <ret>`)。
  ⇒ ⛔ **不满足 0.3 第 5 条("行为判据 + 变异验证, 缺一不算闭环"), 本节不得据此宣称 `buffer`
  已加固**; **保留 / 回退待 leader 裁**。台账侧同步见 [unfixed_defects.md](unfixed_defects.md)
  **0.3.7 限定 1** 与 **0.3.8(7)**。
  ⛔ 0.9 一览第 2 行原文"仅 `buffer` 已加固"今日**已反转**, 且**反转后又被这次落码搅乱一次**
  —— 现状是"**改过但无变异效力**", 与"没改"和"已加固"**都不同**, 三者别混。
- ⚠️ **语义裁决未闭环**: 各入口失效态的**返回值选择**(尤其 `role()` 取缺省值而非新增枚举值)
  是**实现侧的自洽选择**, ⛔ 尚未经 reviewer-claude 语义复核 ⇒ 若复核要求改动, 本条的变异验证
  需**重做**。0.1 表 `负责人` 列写的是"coder(实现)+ reviewer(语义)", **后半未闭环**。
- ⚠️ 本节数字**来自 coder seq135**; 本节作者只做**静态复核**, **未重跑**该批命令。

---

### 14. `ipc_info_pool` 表满路径回收死条目 UF-006(2026-09-18)

> ⚠️ **同上**: 本条也是 0.1 表登记的条目(`UF-006`), **本体未登记在本文**。指针留在 0.1 表的
> `UF-006` 行, 条目块见 [unfixed_defects.md](unfixed_defects.md) **0.3.11**(写回) /
> **0.3.10**(验收证据登记) / **0.3.9**(落码登记) / **0.3.8(4)**(回收边界裁定)。
> ✅ **本文先前那句"仍不新增对应节"已被本条取代** —— 当时缺的是**解禁条件**(产物不在树 / 回报未落 /
> 用例授权未拍), 不是证据; 三条件已于 2026-09-18 全数兑现(逐条见 0.3.11(a))。

**改了什么**

`register_entry`(`src/dzIPC/ipc_info_pool.cc:505`)在"扫不到空槽"**之后**, 在**同一把 `ScopedShmLock` 内**
按与 `gc_dead()` **逐字同构**的谓词(`pid_alive`)回收死条目, 最多**一次扫 + 一次重试**;
**空扫**后 250 ms 内不再扫(节流); 三条失败原因(池未就绪 / 取锁失败 / 表满)各给**限流诊断**(1 条/秒/原因)。

- ⛔ **只动 `register_entry` 的表满分支 + 新增匿名命名空间内的 helper**: `snapshot` 与 `gc_dead` 的
  **函数体不在 diff 内** ⇒ `gc_dead=false` 语义与**判定路径零改动**。
- ⛔ **零 ABI / 零布局**: 头文件 `include/dzIPC/ipc_info_pool.h` md5 与 HEAD **逐字节相同**
  (`e9763c22…`); `PoolEntry` / `kRegionSize` / `kMaxEntries`(=512) / 段名 `dz_ipc_info_pool_v1` 零变更;
  `register_entry` 的 `-1` **返回码不变**(只补日志)。段尺寸 151612 前后一致, 修后写 / 修前读双向解码逐条一致。
- ⛔ **回收谓词不看 `heartbeat_ns`**(与 `gc_dead()` 同口径): pid 复用导致的"假活条目"仍**不可解**,
  评审已裁**只接受有界保守**, ⛔ 不得称"根治表满"。
- ⛔ **红线已避开**: `reap_dead_locked`(`:195`)**不自抢锁** —— 代码注释原文写明"不能调 `gc_dead()`:
  非递归 mutex 同线程自死锁"; 且**只扫一次、非循环**。

**判据怎么验的**

| 命令 | 结果与关键读数 |
|---|---|
| `test_ipc_info_pool`(修复库) | **`10/10 PASSED`, `rc=0`**(连跑 3 次恒定 ≈ 1.41 s, **无 flake**) |
| 满载 512 全死 → 下一次 `register_entry` | 修复前 **`-1`** → 修复后 **`slot=0`**; `in_use` 512→1 ⭐承重 |
| 满载 511 死 + 1 活 | 修复前 **FAIL** → 修复后 **PASS**, 且活条目 `slot=511` **原样存活** ⭐承重 |
| 满载 512 全活 | 两臂**均为 `-1`**(返回码不变); 修复臂**留下诊断行**, 变异臂此处**无输出** |
| `snapshot(false)` 单次调用 | 段字节 md5 **逐字节不变**(`BYTE-INVARIANT: PASS`) |
| 既有回归 | `test_sercli_auto_path` 15 / `test_dzipc_log` 20 / `test_dzipc_pub` 16 全 `rc=0` |
| 并发 8 进程撞满载死池 | 8/8 `rc=0`, slot 唯一 `0..7`, 无损坏; 收尾 `/dev/shm` info_pool 残留 **0** |

- **日期**: 2026-09-18。**基线** HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
- **验证对象 = 树内落码版**: 源 `ff125bb3…`(24157 B) / 测试 `4df1b1fa…`(19539 B, 新增 3 条用例) /
  库 `c54bf472…` / 测试二进制 `ee3ee95d…` —— 四者与留档 `build/uf006_verify/rev1621_*` **逐字节同一**,
  且**均 ≠ HEAD**(`989b16e2…` / `d800e3b8…`)。⚠️ **必须显式指定臂目录**
  (`LD_LIBRARY_PATH=build/uf006_verify/armF3`) —— 不设会静默解析到旧装 `/usr/local/lib/libipc.so.3`
  (07-09, `6204ee77…`)⇒ 读数无效。

**变异验证**: **已做, 三臂各自转红**(不是"跑绿了就算"):
- **臂 A**(整文件回滚到 HEAD)⇒ `rc=1`: `test/test_ipc_info_pool.cpp:444`(`EXPECT_GE(slot,0)` 实测
  **`-1 vs 0`**)与 `:447`(死条目仍在表中)转红, `:475`("满载失败不得静默")亦红;
- **臂 B**(只把回收块改为空)⇒ 红点与 A **完全相同**(`:444`/`:447`), 而"不静默"用例**仍绿**
  ⇒ 该用例确实咬在**回收**上, 不是被诊断顺带带红;
- **臂 C**(只去掉诊断调用)⇒ **只有 `:475` 红**(实测 `stderr=[]` 空捕获), 回收用例**仍绿**
  ⇒"可观测"这条判据**独立承重**。
- ⇒ 三条断言**各自**被**独立的**回滚方向咬住; 变异臂跑完**已还原**, 终树与留档修复版逐位一致。
  ⚠️ **行号必须连测试版指纹一起引**: 上述属**最终版** `4df1b1fa…`;
  [unfixed_defects.md](unfixed_defects.md) 0.3.10(a) 记的 `:422`/`:434`/`:462` 属更早的 `4fdcabb0…`,
  **两组不可混用**。

**代价与注意**

- ⛔ **措辞订正(tester seq149/151 两次点名, 必读)**: 回收**只在表满路径**。池**未满**时,
  死条目**不会**被"下一次 `register_entry`"回收(臂 A 逐字证明 100→100)⇒ 凡引用本修复必须写成
  "**表已满时**的下一次 `register_entry`", ⛔ 不得写成"下一次注册会回收死条目"。
- ⛔ **G6 = 唯一真实可观测的行为差异(⛔ 不得写"任何消费者都看不到差异")**:
  `src/dzIPC/logger/dzipc_log.cc:848-856` 与 `:1001` 的 `RecordEndpointMeta` **只过滤 `!e.in_use`、
  不看 `alive`** ⇒ 回收会让**死条目提前从 endpoint-meta 日志消失**。全仓 `snapshot(false)` 消费方
  已扫尽, 另两处(`auto_ser_cli_ipc.cc:47/92` 判定路径、`exec/dzipc_pub/src/main.cc:43/382`)
  都过滤 `!alive` ⇒ **等价**。
- ⚠️ **节流 250 ms 的代价**: 只对**空扫**计时(`:217`), 一旦扫到东西**立即解除节流**;
  满载 + 刚空扫 + 此刻才出现死条目 ⇒ 注册最多被**推迟一个窗口**且需**再来一次**注册。
  修复前该场景**恒失败** ⇒ **单调改善**, 但**窗口边界未独立量化**(节流分支确被走到:
  stderr 里"未找到死条目"与"被空扫节流跳过"两条**不同原因**同时出现过)。
  诊断限流 `1 s`/原因(`:221`), 三个失败原因各有独立文案。
- ✅ **"诊断持锁写"这条遗留已在**落码版**上闭合(⛔ 该风险只对更早的 `e21e92e3…` 成立)**:
  落码版把诊断输出移到**解锁之后** —— `:552` 结束锁作用域, `:555-556` 才输出, `:553-554` 的注释原文
  写明理由("限流只管频率, 不管单次时长")⇒ `std::cerr` 阻塞**不会再拖住跨进程锁**。
  ⚠️ 但**单次输出的时长仍无上界**(限流只管频率, 不管单次时长)。
- ⚠️ **判据的环境前提(实测假堵过一次)**: 池段名固定 ⇒ **全机共享**。宿主池若被更早的运行留在
  "满且 pid 复用后被判活"态, **修复臂也会红**, 但**签名不同**: `fill_table` 未能填到表满的
  `ASSERT`(`test_ipc_info_pool.cpp:408` / `:465`)= **环境脏**; `:444` / `:447` / `:475` = **真变异信号**。
  ⇒ 跑判据前**必须先 `uf006_probe reset`** 并核对池占用(夹具 `run_arms.sh` / `reverify.sh` 每臂自带
  reset 故免疫)。干净池下两臂跑完 `total=0`, **不毒化宿主状态**。
  另: 本修复的**端到端场景(真产品进程崩死 → 池满 → 再注册)未测**。
- ⚠️ **本节数字来自 coder seq150 / tester seq149+151 / reviewer seq147**; 本节作者做的是
  **静态复核 + 指纹独立重取**(树内两文件与 `rev1621_*` 逐字节比对、`armF3` 库**含** `整表回收`
  而变异库 `armR` **不含**、测试二进制 `ee3ee95d…` 在修复臂与变异臂日志中**同值**、诊断确实在
  `:555-556` 解锁之后), **未重跑**该批命令。

---

### 15. `dzIPC` 库接管进程退出 UF-004: 新增 opt-out 开关(2026-09-18)

> ⚠️ **本条与 §1–§7(传输层缺陷)和 §8–§14(SHM / palloc / 池 / 工装)都不同** —— 它修的是
> **`dzIPC` 工厂路径的进程退出语义**。因 [unfixed_defects.md](unfixed_defects.md) 0.4 写回协议第 3 步而落在本文,
> 条目块与五条限定在那份的 **0.3.12**。
> ⛔ **判定是 `🔶部分修`, 不是 `✅已修`**: 默认路径**按设计逐位不变**, 所以"默认观测量消失"**没有发生**;
> 本次解除的是**强制性**。⛔ 也**不能**把它读成"修好了 `std::exit` 那条路径" —— 那条路径**逐字节未动**。

**改了什么**(`src/dzIPC/dzipc.cc` md5 `ae2dd301209d5e3bc792da1b5d4f7a83`, 208 行 / +33 −0;
`include/dzIPC/dzipc.h` md5 `743cf3bc0bf421f2db1e52db93991c03`, 112 行 / +13 −0)

- 新增进程级原子 `shutdown_monitor_disabled` 与导出函数 `bool dzIPC::DisableShutdownMonitor() noexcept`;
- 判定点落在 `EnsureShutdownMonitorStarted()` **开头**、`shutdown_monitor_started.exchange(true)` **之前**;
  ⛔ **没有**放进 `StartShutdownMonitor()` 的 `std::call_once` —— 在 `call_once` 里提前 `return` 会把
  `once_flag` **消费掉**, 使此后**任何显式** `StartShutdownMonitor()`(公开 API 与 Python 绑定)
  **永久静默失效**(这条由变体 M2 + 用例 `Explicit` 臂守住);
- 返回 `true` = 生效; 返回 `false` = **太晚**(监控已启动, 不可撤销), **且不改任何行为**;
- ABI: `nm -D` 符号 **980 → 981**(恰 +1: `_ZN5dzIPC22DisableShutdownMonitorEv`), **删除 0**,
  SONAME 仍 `libipc.so.3` ⇒ 纯加性变更;
- ⛔ **未动**: 两条 `std::signal`、100 ms 轮询、`StopDzipcLog()`、`CleanupIpcInstances()`、
  `std::exit(0)`、`detach`、`once_flag` —— `git diff -U0` 中这些串的**±行数为 0**(= "不改 UF-009"的**可机械判定**形式)。

**判据怎么验的**(库 `build/lib/libipc.so.3` md5 `b7e3c9caba65ba6ff6c37af995abf2f8`; 一键
`bash tools/sercli_live_probe/uf004_optout_acceptance.sh`)

| 门 | 命令 | 读数 |
|---|---|---|
| 新用例(**7 臂**) | `./build/bin/test_uf004_shutdown_monitor_optout` | **7/7 PASSED**(408 ms), 连跑 5 次全绿 |
| 验收汇总 | `uf004_optout_acceptance.sh` | **PASS 28 / FAIL 0** |
| **A0 默认不变门** | `exit_semantics_matrix.sh`(不传 `--optout`) | direct 4/4 `app_handler_alive` + `residue_own=0`; factory 4/4 `library_exit0_fast` + **`residue_own=17`** —— 与 14:30 基线**逐字段一致** |
| **A1 opt-out 生效门** | 同上 + `--optout early` | factory **翻成** `app_handler_alive` + SUMMARY + **`residue_own=0`**(差集 17 → 0) + `optout_ret=1` |
| A1③ 变红方向(⛔ 不可省) | opt-out + 应用**未装**处理器 | **rc=143 / `by_signal=15`** / 无 SUMMARY ⇒ ⛔ **不是"残留已修"** |
| A3 晚调用 | 先构造再 opt-out | `ret=0` + 行为**逐位同默认** |
| A2 / A5 | opt-out 后**显式** `StartShutdownMonitor()` | **仍照装** ⇒ 公开 API 无回退; `RequestShutdown()` 只剩置位语义 |
| 回归 | `./build/bin/test_dzipc` | **8/8 PASSED**(含 `CtrlCSignalCapture`) |
| `exit_elapsed_ms` | — | ⛔ **全程不作判据**: 库路径 1–5 ms 与本记录 47–83 ms **同量级不可分** |

**变异验证**

- **M1(承重, ⛔ 不动产品码)**: LD_PRELOAD 把 opt-out 判定**钉死为 `false`** ⇒ factory 4/4 **回退**成
  `library_exit0_fast` / 残留 17, 而无处理器的臂由 rc=143 **机械翻转**为 rc=0 ⇒ **正反两向的判据都承重**;
  新用例 **4 条转红**。
- **用例侧 M1a–M6**: 每个变异打红**各自**的断言; **M2**(判定搬进 `call_once`)**只打红 `Explicit` 臂**
  ⇒ "公开 API 永久静默失效"这条只有该臂能抓; **M6**(让 `StartShutdownMonitor` 也置位)**只打红臂 7**。
- **第二变异**: 树构建窗口里出现的库 `12ae64ca`(反汇编成功路径 = `xor eax,eax` **恒 `false`**)
  ⇒ 验收侧 3 条红 + 用例侧 4 条红。
- **阴性对照(方向正确性)**: 两条**默认路径**用例在两种变异下**都保持绿** ⇒ 变异没打到无关断言。

**代价与注意**(⛔ 五条口径必须**分开读**, 展开见 0.3.12(c))

① **强制性已解除**(应用可在首个 IPC 构造前自行接管退出); ② **opt-out 路径实测归零**(`app_handler_alive` + 残留 0);
③ **默认路径逐位不变**, **残留 17 属预期**(= 承载 topic 映射的段未 unlink, 根因是 detached 线程里
`std::exit(0)` 不展开栈), ⛔ **不是泄漏**; ④ **晚调用返 `false`** 且行为不变;
⑤ **无自有处理器的应用开 opt-out = `SIG_DFL` 硬杀**(`by_signal=15` / rc=143), **比现状更差**
⇒ 修复是**按应用分类有条件**的。

三条边界: ⛔ **不改 UF-009**(默认残留**不归零**; `std::exit(0)` 那条路径逐字节未动);
⛔ **Python 面不可达** —— 该 API **没有 Python 绑定**(`python/src/interface.cc` 不在允许改的文件内),
`dzviz` / `dzplot` / python 示例**无法 opt-out**, 仍走默认路径被库 `std::exit(0)` 收场;
且即便从 C++ 层 opt-out, `RequestShutdown()` / `IsShutdownRequested()` 也**只剩置位语义**
(没有监控线程去消费) ⇒ **opt-out 后应用自负退出**。
⚠️ **顺序语义**: `DisableShutdownMonitor()` 的返回值只承诺"**此后不再隐式安装**", ⛔ **不承诺"库当前没在控制"**
—— 先**显式** `StartShutdownMonitor()` 再调它仍**返回 `true`** 而库**已接管**(A3b 反例, 已写进头注释 + 臂 7 覆盖);
判"当前是否已被接管"请自查 `sigaction(SIGINT, nullptr, &old)`。
⚠️ **未闭项**: 默认路径 17 段残留**照旧**; §4 的 `OnShutdown(callback)` **未实现**; 退出码恒 0 / 非主线程 `exit` /
`CleanupIpcInstances` 名不副实三条风险**一条未消**; `local/` 安装树**未刷新**(看不到该 API);
A3b 返工**未再经 reviewer 二次复查**(按其**预置选项** (a) 收口, 由 tester seq159 独立确认)。

**与 §14 的差别**: §14 的产品改动**在 0.1 表 `UF-006` 行 `修改边界` 列之外**还动了 `test/test_ipc_info_pool.cpp`;
本条同样扩到 `test/` + `tools/`(**三件**: 新用例、驱动 `--no-shutdown-monitor` 开关、验收脚本)
—— 由 leader 于 2026-09-18 在子任务里明确授权。

**本节数字来自 coder seq156/158、tester seq159、reviewer seq155/157**; 本节作者做的是
**指纹独立重取**(写回时刻在树内重取上表全部对象, 与三方报告值**逐一对齐**)与**静态复核**, 并
**重跑了承重的那一条命令**: 写回时刻 `bash tools/sercli_live_probe/uf004_optout_acceptance.sh` 再次得到
**PASS 28 / FAIL 0**, 四个探针臂逐字段复现(`early` rc=0 / `by_signal=0` / `optout_ret=1` / term=app / residue=0;
`late` `ret=0` / term=other / residue=17; `noapp` rc=143 / `by_signal=15` / residue=23; `M1noapp` rc=0 / `sigcgt=1`;
`M1early` residue=17)⇒ ✅ **"28/0" 已从"引用"升级为"独立复现"**。⚠️ 其余三条(新用例 7/7、
`exit_semantics_matrix.sh` 默认臂、`test_dzipc` 8/8)⛔ **未由本节作者重跑**, 引用时以 0.3.12(b) 的指纹表为准。

### 16. 订阅队列容量 ≤ 1 时连发 2 条即永久挂死(2026-09-18; 登记, 已修，改为最小队列为2)

**起因**: 排查“`SubscriberIPCPtrMake` 的 `queue_size` 能否传 0”。结论: **传 0 不报错也不会崩, 但比传 1 更糟糕的事还在后面 —— 容量 1 的队列存在序列号混叠缺陷, 连发 2 条即永久挂死**, 与传 1 完全同症。

**queue_size=0 的直接处置(修复后)**: `CircularQueue` 构造把 ≤2 一律钳成 2 ——
`include/dzIPC/common/circularqueue.h` `capacity_(capacity <= 2 ? 2 : capacity)`(2026-09-18 修复,
见下文“已修记录”; 修复前是 `capacity == 0 ? 1 : capacity`, 即 0 静默变 1)。
dzIPC 层没有任何校验/告警/下限声明; `queue_size` 在两条传输里**只**喂给两条 `CircularQueue`
(`shm_pub_sub_ipc.cc:453-454` / `socket_pub_sub_ipc.cc:572-574`), 不参与池大小、线程数或共享段命名
(dzviz `subscriber.py:104` 注释里 `QU_CONN__...__<queue_size>__` 的 “queue_size” 是误标 ——
那两个数是 libipc 的消息尺寸档模板参数 `DataSize/AlignSize`, 见 `src/libipc/ipc.cpp:702`),
ser-cli 的请求队列更是硬编码 16(`shm_ser_cli_ipc.cc:185`)。

**真正的缺陷(容量 1 混叠)**: 同文件 `try_dequeue` 释放槽位时写
`cell->sequence.store(pos + capacity_, ...)`(circularqueue.h:165), 而入队完成后写的是
`cell->sequence.store(pos + 1, ...)`(:128)。**capacity ≥ 2** 时两者不同值、语义正确(生产者期望
`seq == pos`, 消费者期望 `seq == pos + 1`); **capacity == 1** 时 `pos + 1 == pos + capacity`,
两条写入重合 —— 槽位在“已被消费、可再生产”与“已生产、待消费”两个状态间不可区分, 第二条消息
入队后消费者永远等不到 `seq == dequeue_pos + 1`, 队列死锁。

**实测(活体, 两条传输同症, 同进程 pub+sub)**:
- `queue=2`: 连发 2 条 → `got=[0, 1]` 正常(SHM 与 UDP 同)。
- `queue=1` / `queue=0`: 连发 2 条 → `try_get_clone` **永久自旋**, faulthandler 12 s 超时转储
  栈钉死在 Python 侧行 18(`try_get_clone`); 20 s/40 s/60 s 均不返回。SHM 与 UDP 完全一致。
- 对照: 每发一条随即取走的场景, `queue=0/1/4` 单条都能收到 —— 所以**单条/低速率场景看不出来**,
  这正是它危险的地方(测试全绿, 上线突发即挂)。
- 复现: 任意脚本, `SubscriberIPCPtrMake(td, topic, dom, 1 或 0, IPC_SHM 或 IPC_SOCKET, False)` +
  `InitChannel()` 后**不取**连发 2 条同 msg_id 消息, 再 drain 即挂。探针留档 `/tmp/qz_burst.py`
  (会话结束后删除)。

**附带观察(排查途中, 与 queue 无关)**: 同进程内创建“订阅者先于发布者”的 pub+sub 组合时,
本会话观察到一次启动期段错误与多次无限自旋(打印 `=== SHM ===` 后无输出, R 状态 100% CPU),
复现率不稳定 —— 疑与既有登记的 libipc 启动期/握手期问题同族, 未深挖。

**已修(2026-09-18, 最小方案: 构造处钳制 capacity ≥ 2)**: `circularqueue.h` 构造改为
`capacity_(capacity <= 2 ? 2 : capacity)` —— 容量 1 从此不可达, 混叠条件
`pos + 1 == pos + capacity_` 被根除, 属根除而非绕过; Vyukov 序列语义本体未动(方案②不再必要,
上面的机制分析保留作容量 1 序列号混叠的档案)。修法选项①的“一次性告警”未做(见语义变化①)。

**修好判据已满足(全部实跑, 2026-09-18)**:
- 重建确认: 全部受影响产物(`shm_pub_sub_ipc` / `socket_pub_sub_ipc` / `shm_ser_cli_ipc` 的 `.o`
  与 `_dzipc_core` pybind 模块)均新于 `circularqueue.h`, 构建零错误零告警。
- 活体复测(连发 2 条不取, 同进程 pub+sub): 修复前 q=1 在 `try_get_clone` 永久自旋
  (faulthandler 12 s 转储钉死在该调用, exit=1) —— **这同时就是变异验证**(旧实现 = 转红);
  修复后 **SHM 与 UDP 两传输、q=0/1/2 全部 `got=[0, 1]`**, 零挂死; q=2 行为与修复前一致。
- 回归: `test_circularqueue` 5/5、`test_shm_ser_cli_nodelet` 5/5、`test_dzflat_python` 全部通过。

**语义变化(无害, 但需知情)**:
① `queue_size=0/1` 现在**静默**变成 2, 队列满走既有 drop-oldest —— 无任何告警(可选跟进:
   钳制发生时打一次性警告)。
② 两个既有 capacity=1 显式调用点核查无影响: ser-cli 每请求应答队列
   (`shm_ser_cli_ipc.cc:765`, 作用域仅限单次调用、只等一条应答、超时即弃, 容量无关)与
   `test_shm_ser_cli_nodelet.cpp:433` 的 `dummy_queue(1)`(只用于把 `server_count` 顶到 2,
   从不 push/pop)。均由对应测试覆盖。
③ `local/` 副本要吃到此修复, 须随 `_dzipc_core`/`libipc.so` 一同刷新(由用户手动管理)。

### 17. factory 路径残留 17 段 + 缺 SERVER SUMMARY UF-009: 链式接管 + 宽限退出(2026-09-18)

**症状**(0.3.4/0.3.5 已坐实, 判定实验 artifact `20260918_143034`): 走公共工厂
(`*IPCPtrMake`)的应用收到 SIGINT/SIGTERM 后, `SERVER SUMMARY` 永不打印、
`/dev/shm` 残留 17 段/腿, 但退出码为 0 —— 即"库接管退出"形态。

**根因**: `StartShutdownMonitor()`(`dzipc.cc`)用 `std::signal` 直接**覆盖**应用在 main 里
装的处理器; 监控线程醒来后 `StopDzipcLog → CleanupIpcInstances → std::exit(0)`。
`std::exit` **不展开栈** ⇒ main 里的 IPC 实例 `shared_ptr` 不析构 ⇒ 段不 unlink,
应用自己的收尾打印也永不执行。

**修法(有条件, leader 已拍板"尝试修复")**: `src/dzIPC/dzipc.cc`
① **链式保存**: 安装库处理器前用 `sigaction` 保存既有处置(SIG_DFL/SIG_IGN/真处理器三类);
② **回放**: 信号到来时监控线程先调既有处理器(⛔ SIG_DFL/SIG_IGN 跳过 —— 回放 SIG_DFL 会把
"应用从未装处理器"的 UF-004 臂 2 场景退化成信号硬杀, "默认路径逐位不变"即被打破);
③ **复权**: 应用处置装回、库处理器退出;
④ **宽限 500ms**: 等应用自行退出 —— 优雅路径上 main return ⇒ 栈展开 ⇒ 实例析构 ⇒
段 unlink ⇒ 应用收尾恢复; 驱动实测退出 20~40ms, 500ms 留 10 倍裕量;
⑤ **超时才走原库收尾**(栈不展开, 残留语义与改前一致 —— 保留面, 不强拆实例);
⑥ `RequestShutdown()` 的库内部退出路径不回放不宽限, 行为不变。
已知边界: 同进程内 SIGINT、SIGTERM 接连到来的链式序列, 第二个信号可能赶不上回放
(复权完成后处置已归应用, 只有复权完成前的窗口例外); 单信号场景不受影响。

**验证(全部实跑)**:
- 矩阵复跑(`build/uf009_fix_verify/000247`, 指纹自动绑定: driver `f295bbb0` /
  libipc `eb6fbccf` / tree `38188996`): **factory 腿 2/2 `app_handler_alive`** ——
  残留 17→**0**、SERVER SUMMARY 0/2→**2/2**; direct 腿逐位不变(2/2 app_handler_alive)。
- 新回归套件 `test/test_uf009_graceful_exit.cpp` **3/3**: ①优雅路径带牙用例
  (工厂 pub/sub + 真实 publish 建出 13 段 → raise → 处理器被回放 → 实例析构 →
  逐名 stat 验证段全部消失; **变异验证**: 换回改前代码该用例立刻转红); ②SIG_DFL
  不回放(仍库收尾 exit 0); ③RequestShutdown 路径行为不变。
- UF-004 验收 **7/7 不回归**; 全量 gtest 55 二进制全过; Python 侧
  `test_dzflat_python` 全部通过、`test_dzplot` **129/129**。

**语义变化(如实登记)**: 默认路径下信号到退出增加最多 ~600ms(500ms 宽限+轮询抖动);
应用有自有处理器时进程由 main return 收尾(退出码由应用决定), 不再恒为 0。
`local/` 副本须随库刷新方生效(用户手动管理)。

## 附: 当前状态

**第 1–4、6、7 条已闭环**, 每条都有**行为判据 + 变异验证**(第 3 条另有实测实验数据);全量
gtest **55/55** 通过(第 1–4 条的"回归"行取自当时复跑 —— 41/41; 本次(第 7 条)完整重跑构建与全部
测试二进制, 55/55)。

**另: § 修复记录 8(2026-09-18)不是 SHM 缺陷修复** —— 它交付的是**分配失败注入工装**(`UF-000`,
`tools/alloc_fault_inject/`), 闭环了"没有它则 UF-001/UF-002 的判据不可执行"这个前置。它有**六条
限定**(见记录 8 的"代价与注意", 其中"回滚变异验证"仍欠一次)。

**§ 修复记录 12 / 13(2026-09-18)把 §8 交付的工装第一次用在了产品缺陷上** —— `UF-001`(allocator 的
`noexcept`-nullptr 语义)与 `UF-002`(五类 pimpl 入口的失效态判空)双双落码: 注入态 `3/3 PASS`
(`hits=1` / `band_calls=1`), 无注入态 `3 SKIP / 0 PASS`, 两条**各自独立回滚后分别 rc=139**,
回归 `test_ipc` 8 / `test_shm` 8 / `test_sync` 5 / `test_uf007` 2 全 PASS。⇒ 至此 **`UF-001`/`UF-002`
不再是"未修"**; 0.3.2 末段那句"故 `UF-001`/`UF-002` 本身仍未修"是**当时**的记账, 已被本条取代。

**§ 修复记录 14(2026-09-18)是本文第一条**不靠注入工装**的闭环** —— 它修的是 `ipc_info_pool` 的
**表满路径**: 满载 512 条全死时, 回收前**恒 `-1`**(注册永远失败、池对后来者永久卡死), 回收后
**下一次注册即可成功**。判据 `10/10 PASS`、**三臂变异各自转红**(见 §14)。
⚠️ 三条必须与结论一起读: ①回收**只在表满路径**(池未满时死条目不会被下次注册回收); ②`G6` 日志差异
(`RecordEndpointMeta` 不看 `alive` ⇒ 死条目提前从 endpoint-meta 日志消失); ③250 ms 空扫节流 +
池脏时的**假红签名**(`:408` / `:465`)与**真变异签名**(`:444` / `:447` / `:475`)不同。
⚠️ 与 §12/§13 的差别: 本条的产品改动**在 0.1 表 `UF-006` 行 `修改边界` 列之外**还动了
`test/test_ipc_info_pool.cpp`(新增 3 条用例)—— 该落盘由 leader 于 2026-09-18 明确授权(见 0.3.11(a) 第 ④ 条)。

**§ 修复记录 15(2026-09-18)是本文第一条"解除强制性"而非"消除现象"的记录** —— `UF-004`(库接管进程退出)
新增 opt-out 开关 `bool dzIPC::DisableShutdownMonitor() noexcept`: 验收 **28 PASS / 0 FAIL**、新用例 **7/7**、
**M1 + 六臂变异各自转红**、且**两条默认路径用例在两种变异下都保持绿**。
⚠️ **它不是 `✅已修`**: 默认路径**按设计逐位不变**(factory 仍"无 SUMMARY + 残留 own=17"), opt-out 只把
"装不装处理器"交还应用; `std::exit(0)` 那条路径(= `UF-009`)逐字节未动; 无自有处理器的应用开 opt-out
得 `SIG_DFL` 硬杀(rc=143); 该 API **没有 Python 绑定** ⇒ `dzviz` / `dzplot` 无法 opt-out。
五条口径与全部限定见 [unfixed_defects.md](unfixed_defects.md) **0.3.12**。**§ 修复记录 12–15 的实现都在 `UF-*` 台账里**,
本文只存详情。
⛔ 仍未闭: pid 复用假活(不看 `heartbeat_ns`)与"真产品进程崩死 → 池满 → 再注册"的端到端未测。

⚠️ **两条遗留必须一起读(均已于 2026-09-18 订正)**: ①`src/libipc/buffer.cpp:59`(`~buffer()` 的
`p_->clear()`)与 §13 修的五类**同型**却**不在清单内** —— 今日该行**已被 coder 改动**, 但**回滚变异
阴性**(还原后同一探针 rc = 0, 不崩)⇒ ⛔ **不算修复、不满足 0.3 第 5 条**, 保留/回退**待 leader 裁**
(详见 §13"代价与注意"第 1 条与 [unfixed_defects.md](unfixed_defects.md) **0.3.8(7)**);
②`UF-002` 的**语义复核**(reviewer 侧)未闭环 ⇒ 若复核要求改动返回值选择, §13 的变异验证需重做。
⚠️ 另**连带订正**一条: 曾称该行与 `shm`/`mutex`/`condition`/`semaphore` 四处"**逐字同型**"——
就 `buffer` 而言**已被实测证伪**; 那四处的崩点是 `impl(p_)->成员` 类**读**, 但若其析构同样只是
`p_->clear()`, 则同样无变异效力 ⇒ 建议**各自补一次回滚变异**后再决定是否宣称修复(⛔本轮未动那 4 个文件)。

✅ **原先那处跨文档的悬挂引用已订正**(2026-09-18, writer-claude): [unfixed_defects.md](unfixed_defects.md)
的 **0.3.3 / 0.3.4 / 0.3.5**(分别判 `UF-007` / `UF-004` / `UF-009`)原先都声明"详情已搬到本文
**§ 修复记录 9 / 10 / 11**", 但本文**没有这三节**(§8 之后直接是 §12) ⇒ 那三处指针**指向空号**。
现已把那三处改写为"本文**无**对应章节, 其**自身的条目块即唯一详情**", 指向空号的引用**已清零**。
⚠️ 但这**不等于**搬移被取消: 0.4 协议第 3 步对这三项**仍欠** §9–§11 三节, 只是**不再有指向空号的引用**;
补写之日应同时把那三处的措辞改回"详情已搬到 § 修复记录 N"。

**第 7 条的额外价值**: 它把"偶发/不可复现"这一类问题的定位手段固化了 —— 守门人处理器(拿栈)→
符号化(拿函数)→ ASAN + 确定性探针(拿因果)→ 带牙回归用例(卡住不复发)。同时暴露了一个排查陷阱:
崩溃残留在共享段里的垃圾会影响后续所有进程, 很容易被误读成新引入的回归。

**第 5 条已实测、修法已定、待决策** —— 它比第 3 条更值得优先处理: 第 3 条影响的是
"同机不同 topic 的默认配置", 而第 5 条在 **8192 topic 的规模下 60% 的 topic 落在共组
状态**, 且默认 `msg_id = 0` 时直接是静默错收。修法(端口纳入 topic hash)成本与第 1 条
同级, 但**同样有"与旧版本不互通"的代价**, 所以要和发布节奏一起决定。

顺带记一条方法论: 第 5 条我最初给的是**生日模型估算**(256 topic 时 39.7%), 实测是
**60%** —— 高了 5 倍。差异来自两处: 名义空间(64516)本身就不大, 而**两个坐标还不独立**
(有效空间只剩 18.9%)。后者是**只有实测才能发现**的。凡是拿概率模型支撑决策, 都该有
一次实测兜底。

---

⛔ **本文与 [unfixed_defects.md](unfixed_defects.md) 的边界须知(2026-09-18 新增)**:
后者新增了 **0.3.8「剩余项裁定登记」**, 集中登记 `UF-003`/`UF-004`/`UF-005a`/`UF-005b`/`UF-006`/`UF-008`
与 `DF-001`–`DF-005` 的**设计态裁定 / 源码级判据 / 一条阴性变异结果**。
⚠️ **本节(本文)与之的分工**: 本文只放**已闭环**条目的详情(**0.4 协议第 3 步**的搬移目标);
**0.3.8 明确不是 0.4 写回, 不含任何实测 PASS, 也不新增任何「§ 修复记录」**。
⇒ ⛔ 查上述剩余项**只看 0.3.8**, 不要因为本文没有对应节就判"没登记"; 反之 ⛔ **不得**把
0.3.8 的裁定读成"已修复"(尤其 `UF-004` 上限仍是 `🔶部分修`, `buffer.cpp:59` 那次改动**不算修复**)。

✅ **同日订正: `UF-006` 已闭环, 本文**已**新增对应节 § 修复记录 14(2026-09-18 最终写回)**
- **改动内容**(树内落码版 = 留档 `build/uf006_verify/rev1621_ipc_info_pool.cc`, md5 `ff125bb35e69b631c0d250ea3837f962`, 24157 B)
  = **表满时在同一把锁内回收死条目 + 重扫恰好一次 + 空扫节流 250 ms + 三条失败原因限流诊断(1 条/秒)**;
  `include/dzIPC/ipc_info_pool.h` md5 `e9763c22…` **与 HEAD 逐字节相同**(未改)。
- ✅ **实测**: 修复臂 **10/10 PASSED ×3 连跑 `rc=0`**(无 flake); 变异臂(改动前库)**8 PASSED / 2 FAILED**;
  既有面回归 20/20; 臂夹具 **armE `snapshot(false)` 字节不变 PASS**。
  ⇒ ✅ **0.3 第 5 条已满足**(此句取代本文件先前那句"回收分支从未被执行过", 以及 0.3.9 的 "G9 BLOCKED";
  两者在当时是事实, 现已被实测推翻)。
- ✅ **三条解禁条件已于 2026-09-18 全数兑现**(此前挡住写回的正是它们, 不是证据不足):
  ①树内源已还原为 `ff125bb3…` **且稳定**(⛔ ≠ HEAD `989b16e2…`); ②已在**当前指纹对**
  (源 `ff125bb3…` + 测试 `4df1b1fa…`)上重跑全绿 —— 上一版 10/10 跑的是测试 `4fdcabb0`, **故不能算数**;
  ③coder 回报已落 `results.jsonl`(seq150)且**改动面未越界**; ④leader 已拍"**用例落进 `test/` 在授权内**"。
  ⇒ `0.1` 表 `UF-006` 行状态列已翻 **`✅已修`**, 详情见本文 **§ 修复记录 14**, 条目块见
  [unfixed_defects.md](unfixed_defects.md) **0.3.11**; 证据登记保留在 **0.3.10**, 落码过程在 **0.3.9**,
  回收边界裁定在 **0.3.8(4)**。
- ⛔ **与结论必须一起读的三条限定**(已随写回搬入 § 修复记录 14): ①回收**只在表满路径**
  (池未满时死条目**不会**被下一次注册回收 —— 措辞订正, tester 两次点名);
  ②`G6`: `dzipc_log.cc:848-856/:1001`(`RecordEndpointMeta`)**只过滤 `!e.in_use`、不看 `alive`**
  ⇒ 回收会让**死条目提前从 endpoint-meta 日志消失** —— 这是本改动**唯一**真实可观测的行为差异,
  ⛔ 不得写成"任何消费者都看不到差异"; ③250 ms 节流(只对空扫计时)+ 池脏时的**假红签名**
  (`fill_table` 未填满: `test_ipc_info_pool.cpp:408` / `:465`)与**真变异签名**(`:444` / `:447` / `:475`)不同。
- ⛔ **仍未闭**(⛔ 不得读成"表满问题根治"): `heartbeat` 陈旧度未纳入 ⇒ pid 复用假活仍可致表满
  (需另行授权 + 阈值来源); **真产品进程崩死 → 池满 → 再注册的端到端未测**。
- ⛔ **"已落码" ≠ "已验收" ≠ "已修"** —— 三者不得混读; 本条是三者**同时成立**后的落笔(0.4 写回)。