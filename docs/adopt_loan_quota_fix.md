# UF-012 修复: adopt 借样配额 + 池扩容 40/钉上限 10

> 状态: ✅已修。指纹(证据规范, RELEASE_NOTES.md §4): tree_head=`90510a5`+工作树(24 文件未提交, 本文档随其一),
> libipc_md5=`191287dc`, 判据驱动 md5=`a4de8f72`(test_adopt_loan_quota)。

## 1. 缺陷

schema-less 话题(GenericMessage)收大段 DZFlat 时, `dzflat_adopt` 把**活 chunk** 借进
`msg_queue_`(借样是 Python 侧 memoryview 零拷贝的落点)。而 `msg_queue_` 的深度是用户
配置的 `queue_size` —— 完全不受步骤③对 view 队列的钉制(`ViewQueueCap()`)约束:

- queue_size 配 64/128 ⇒ 理论钉需求 64/128 块, 而池每尺寸档只有 32 块;
- 一个慢消费者就能把整档池钉干 ⇒ 同档位**所有**话题的 loan 失败, 全部退化 TLV 整包拷贝;
- 与 view 路径的差别: view 的钉上限是构造期钉死的 8, adopt 的钉需求随用户配置无界。

构造函数注释"msg 队列(物化, 不钉 chunk)刻意不钉"对 TLV 路径成立, 但对 adopt 分支不成立
—— 这是步骤③护栏("钉有界")上唯一的洞。

## 2. 修法(池侧 → 借样侧)

**决策(用户拍板)**: 防钉穿从池侧挪到借样侧 —— 不扩池救急, 而是给借样设配额;同时按
ROS 2 默认 QoS depth=10 对齐钉上限, 池按比例扩容保持"4 个订阅者满钉"余量。

| 变更 | 文件 | 内容 |
|---|---|---|
| 池容量 32→40 | `include/libipc/def.h` | `large_msg_cache = 40` = 10(钉上限, 对齐 ROS 2 默认 depth)× 4(满钉订阅者余量) |
| 钉上限 8→10 | `src/dzIPC/common/nodelet_config.cc` | `ViewQueueCap()` = 40/4 = 10, view 队列与 adopt 配额同源 |
| **借样配额** | `src/dzIPC/shm_pub_sub_ipc.cc` | `adopt_cap_` = view_cap(同一开关同一上限);配额内 adopt 借样(零拷贝), 配额满 `dzflat_read` 物化(每消息一次拷贝) + `kDzFlatAdoptSpilled` 计数 |
| 计数三路 | 同上 + `circularqueue.h` | 入队 +1 / pop(get_clone/try_get_clone) −1 / **满队挤最老** −1(CircularQueue 新增 `set_evict_cb`, 驱逐在 push 内部发生、不经过 pop, 不钩必漂移) |
| fast-path 防绕过 | `shm_pub_sub_ipc.cc:247` | 发布侧 clone 若自身持借样(订阅后转发的 GenericMessage), 先物化再扇出 —— 队列里的借样只允许来自接收侧配额 |
| 驱逐可见性 | `include/dzIPC/common/circularqueue.h` | `EvictCb` + `set_evict_cb()`, `push_owned` 驱逐分支析构前回调 |
| 分类虚函数 | `ipc_msg_base.hpp` / `generic_message.hpp` | `virtual dzflat_is_borrowed()`(基类 false, GenericMessage override)—— 计数与 fast-path 判别的依据 |
| 观测点 | `nodelet_config.h/.cc` | `DzFlatRxEvent::kDzFlatAdoptSpilled` + `DzFlatRxStats::dzflat_adopt_spilled`(kCount 哨兵前插入) |
| 超限警告 | `shm_pub_sub_ipc.cc` | `warn_view_queue_pinned` 扩展: queue_size 超钉上限时同时说明 view 钉制与 adopt 配额两个钉面 |
| 段名编码容量 | `ipc.cpp` / `sniffer.cpp` | `CHUNK_INFO__<size>__C<cap>` —— 容量决定段布局, 新旧容量版本段天然隔离(兼收 UF-003 档案登记的"无版本标记"缺口, unfixed_defects_full_v1.md:607);⛔ 两处构造逐字同步 |
| socket 腿豁免 | `socket_pub_sub_ipc.cc` | adopt 分支加注: wire 是去帧独立堆块, 借样不占池, 不设配额 |

**明确不做**: 运行时扩池(容量烧死在段布局 + 全机共享需编译期共识, 已论证不可行);
用户手持期设限(与 view 路径同一契约)。

## 3. 判据(test/test_adopt_loan_quota.cpp, 5 用例)

| 用例 | 判据 |
|---|---|
| WarnsWhenQueueExceedsPinCap | ViewQueueCap()==10、large_msg_cache==40;queue_size=64 构造时 stderr 出现"queue_size = 64 超过钉上限"且说明 adopt 配额面 |
| QuotaCapsBorrowAndSpillsMaterialize | queue_size=64 不消费连发 12 条大段(60/61 宽交替, 同档 10240): 前 10 条 borrowed==true、后 2 条 false;spilled==2;借样与物化同源内容逐字节一致 |
| DrainRestoresZeroCopy | 配额打满后排空, 下一条回到借样(pop 递减缺失 ⇒ 必红) |
| EvictionKeepsQuotaAccurate | queue_size=4 灌 12 条触发驱逐: 全部借样、spilled==0(驱逐递减缺失 ⇒ 第 11/12 条物化必红);排空后再发仍借样 |
| SteadyStateStaysZeroCopy | 消费跟得上时 20/20 借样、spilled==0 |

⛔ 用例自身踩过的坑(记录防复发): ① `try_get_clone` 的 sink 必须传订阅者自身 td
(空 TopicData → `update()` 解引用段错误);② warm-up 必须"连续两轮排空"才净
(单轮排空不夠, 握手期积压混入计数, 实测 total 24/20);③ cap 用例的 spill 决策
必须先于任何 pop —— 边收边排会提前释放配额, 把本该物化的尾部变成借样(真竞态,
用 `dzflat_accepted` 连续两读稳定判"环已吞完"后再统一排干)。

## 4. 复跑记录(指纹见头部)

- 专项: test_adopt_loan_quota **5/5 绿 × 3 轮**(首两轮含修复过程)。
- 回归面 13 驱动全绿: pool_exhaust 2、chunk_hold 3、loan 10、lap_safety 3、
  dzflat_transport 9、shm_nodelet 13、uf004 7、uf009 3、uf010_hash 5、uf007 2、
  adopt_loan_quota 5、**dzflat_python 109 断言全过**(借样零拷贝/借样钉住不被改写/
  memoryview 均在场; vtable 变更后 Python 绑定无损; queue_size=32 场景下新警告自然触发)。
- 容量提升牵出的测试侧魔数(全部改为由 `ipc::large_msg_cache` 导出, 吸收 uf007
  教训): test_loan/test_chunk_hold/test_lap_safety 的 `kChunkPoolSize=32`、
  test_pool_exhaust 的 `"pool capacity = 32"` 字符串断言 ×2、
  pool_attribution_benchmark 的 `kPoolCap=32`/`b[33]`/段名、probe_teardown 的
  `cursor_` 偏移 32→40 与段名。
- /dev/shm 终态 0 段(清池协议: 先确认无活持有者)。

## 5. 已知边界

- spilled 计数非零不是错: 是"消费者慢到配额不够用"的可见信号, 指向消费侧调优。
- UF-011(二次入池)是归还侧缺陷, 与本修复正交;其判据魔数 4×kCap 已由 max_count
  导出, 容量 40 自动适配(实测 uf007 用例绿)。
- 段名含容量后, 升级部署首次启动会新建 `__C40` 段;旧 `__C32` 段成为孤儿可按
  清池协议清理(不会混挂)。
