# shm chunk 池占用治理 · 执行顺序

> 基线 `2f13665`(dev), 起笔时工作树干净。
> 本文件是**执行顺序**的记录, 每一步做完回写"实际结果"一栏; 未做的步骤保持 ⬜。
> 状态词表同 `unfixed_defects.md` §0.3.4: ⬜未做 / 🔶进行中 / ✅已做 / ⛔不做 / ⏸️待决策。

## 0. 问题陈述

订阅侧有一条**用户态队列**(`view_queue_` / `msg_queue_`), 里头的每个未消费 `Sample`
都把对应的 chunk 钉在池外。队列越深、应用取数越慢, 被钉住的 chunk 越多, 直到池取空。

目标: **让池的占用上界从"队列深度"降到"订阅者数"**, 从而降低满池风险。

## 1. 量纲(这一节决定收益有多大, 先算再做)

| 量 | 值 | 锚点 |
|---|---|---|
| 每档 chunk 池容量 | **32** | `large_msg_cache = 32` → `id_pool::max_count = min(32, 255)` |
| 环槽位数 | **256** | `kRingSlots` |
| 池分档粒度 | 1KB 台阶(`large_msg_align`) | `calc_chunk_size` |
| 段名 | `CHUNK_INFO__<chunk_size>` | `get_info` |
| 默认前缀 | **空** ⇒ 段名无话题/进程区分 | `connect(ph, {nullptr}, …)` + `make_prefix("", …)` |

锚点:

- `include/libipc/def.h:44` — `large_msg_cache = 32`
- `src/libipc/utility/id_pool.h:40-47` — `max_count = limited_max_count()`
- `src/libipc/prod_cons.h:25-26` — `kRingSlots = 256`
- `src/libipc/ipc.cpp:234-241` — `calc_chunk_size` 按 `large_msg_align` 取整
- `src/libipc/ipc.cpp:330-333` — `CHUNK_INFO__<chunk_size>`
- `src/libipc/ipc.cpp:363-380` — `chunk_storage_info(pref, chunk_size)`:按 `chunk_size`
  取 `chunk_handle_t`,再按 `pref` 取 handle ⇒ **同一档位一个段**

**三个承重结论:**

1. **池是硬瓶颈, 环不是。** 池 32 块 vs 环 256 槽 —— 生产者最多只能同时持有 32 条大消息,
   第 33 条就到不了环里(会走降级路径)。环的深度**永远不会**成为大消息的第一约束。
2. **池空是静默的。** 三条路径全都不报:

   | 路径 | 池空后的行为 | 锚点 | 是否报 |
   |---|---|---|---|
   | `send` | 退化成 64 字节分片 | `ipc.cpp:962-990` | ⛔log 被注释(`:962-964`) |
   | `no_member_send` | 同上 | `ipc.cpp:1101-` | ⛔log 被注释(`:1101-1103`) |
   | `loan` | 返回空 `loan_t`, 调用方回退整包 | `ipc.cpp:1435-1440` | ⛔只有注释, 无 log |

   ⇒ 现场无法回答"池是不是被钉干了"。**先做步骤① 是硬前提, 不是可选优化。**(已做, 见 §3)
3. **池是跨话题、跨进程共享的** —— 默认 prefix 为空, 段名里没有任何话题/进程区分,
   所以同一尺寸档的池是**全机一份**。两个后果:

   - **跨话题饿死**: A 话题的慢订阅方钉干 7168 档, B 话题(毫无关系)的大消息跟着
     一起退化成 72 字节分片, 而 B 自己的日志里看不到任何异常源。
   - **诊断不能只看单话题**: 现场看到"某话题大消息异常"时, 元凶可能是**另一个进程**。

   这与 UF-003 的形态相同(`chunk id 经环传递 ⇒ id 空间是跨进程契约`), 也意味着
   步骤① 的报错必须带上"哪一档"与**池的归属键**, 否则跨话题归因无从下手。
   归属键就是 `prefix`(段名 = `make_prefix(prefix, {"CHUNK_INFO__", chunk_size})`),
   而默认它是空串 —— 所以报错里要**把空前缀显式打出来并点明含义**。已落码, 见 §3。


## 2. 池占用的两个来源

chunk 从池里被取走到归还, 中间经过了谁:

```
生产者 acquire_storage ──► 环槽(只存 storage_id) ──► recv 建 buff_t
                                                        │
                                      ┌─────────────────┴─────────────────┐
                                      ▼                                   ▼
                            view_queue_ (Sample, 钉住)          TLV 路径物化后即释放
                                      │
                                      ▼
                              用户 try_get → 读完 → ~Sample → recycle → 还池
```

`chunk->conns()` **不是引用计数**, 是**发送时刻一次性置好的接收方位图**:

- 唯一写入点: `ipc.cpp:536` `chunk->conns().store(conns, relaxed)` —— 在 `acquire_storage`
  内, 即**生产者**在做; `conns` 是 `send`/`no_member_send`/`loan` 三个调用点
  (`:1050`/`:1190`/`:1532`)自己传进来的**发送那刻的在连收方位图**。
- 唯二清除点: 接收方 `buff_t` 析构 → `recycle_storage`(`ipc.cpp:610`) → `sub_rc`
  (`ipc.cpp:579` unicast / `:588` broadcast);写方 `force_push` 覆写 →
  `discard_storage`(`ipc.cpp:664`)。
- 归零才还池: `ipc.cpp:629-637`(`sub_rc` 返回真后 `info->pool_.release(id)`)。

> 锚点订正(2026-09-19): 本节原先记的是 `:438` / `:491-509` / `:600-620` / `:535-540`,
> 及一处 `:936 que->elems()->connections()`。前四者是**步骤① 落码前的行号**
> (加了注释块后整体下移), 最后一条在现树里 `connections()` 已零命中 —— 属于
> 陈旧引用, 已按现树重取。凡引用本节的结论, 用上面的行号。

⇒ 用户态队列持有 `Sample` 的效果是 **pin(钉住不归零)**, 不是 **acquire(加计数)** ——
位在你 `pop` 之前就已经在位图上了。**两个来源**: ①队列里躺着的, ②环里在飞还没 recv 的。

## 3. 执行顺序

### ✅ 步骤① 让"池空"可观测

**做什么**: 在 `acquire_storage` 这个**唯一取块点**上收口, 当 `pool_.acquire()` 返回 `< 0`
时报一次并计数。三条调用路径(`send` / `no_member_send` / `loan`)在此汇合,
一处改动全覆盖。

**为什么不逐个放开那两行注释**: 池空时**每条大消息**都会命中, 逐条打印会把日志淹掉。
照 `report_cache_alloc_failure`(`ipc.cpp` 内)的先例: **首报 + 计数节流**。

**已落码**:

| 改动 | 位置 |
|---|---|
| `note_pool_exhausted(kind, chunk_size, size, prefix)` | `ipc.cpp:464-503` |
| `acquire_storage` 增 `char const *kind` 形参 + `id < 0` 分支 | `ipc.cpp:505-538`(`id < 0` 在 `:525-531`) |
| 三个调用点传 kind | `ipc.cpp:1050`(`"send"`) / `:1190`(`"no_member_send"`) / `:1532`(`"loan"`) |
| 两处被注释掉的逐条 log → 指向本收口的说明 | `ipc.cpp:1060-1062` / `:1200-1202` |

⛔ **节流粒度是一次真实的自我订正**(由变异臂 C 抓出, 见下): 首版用**进程内单一
计数**, 于是只要 A 档池先报过一次, B 档再饿就是"第 2 次"被节流吞掉 —— 而
"哪一档在饿"恰恰是这条观测唯一要回答的问题。现改为**按 `(kind, chunk_size)` 各自
一份计数**。锁是安全的: 本函数**只在池取空时**才被调用。

**报错必须自带归因字段**(§1 结论 3 的要求): 光有"哪一档"还不够 —— 池段名
= `make_prefix(prefix, {"CHUNK_INFO__", chunk_size})`(`get_info`, `ipc.cpp:332-333`),
所以 **`prefix` 才是这一档池的归属判别键**, 而默认部署下它是**空串**。于是:

- `prefix = '...'` 原样打出。**空前缀不是"没信息", 它本身就是那条信息** ——
  现场读到空字段会当成"没信息", 而它恰恰意味着"这一档池没有归属区分, 元凶可能在
  另一个进程里"。所以空的时候额外拼一句点明, 非空时不拼(点明句是**条件输出**)。
- `count` 标注 **`(本进程)`** —— 池是全机共享的, 一个光秃秃的 `count` 会被读成
  "全机饿了多少次"。**读数能被读错, 与读数错了是同一类问题。**
- ⛔ 没打 pid: 报错本来就是**受害者自己**打的, 打上自己的 pid 对定位元凶没有帮助;
  对归因有用的是"池的键是什么", 那正是 prefix。若要跨进程对账, 用 prefix 而不是 pid。

**判据(必须两个方向都跑)**:

- 阳性对照: 构造池耗尽(深队列 + 慢取数), 计数 **> 0** ← 证明观测面非退化
- 阴性对照: 正常负载, 计数 **== 0**

⚠️ 只有阳性对照能证"可见"; 只有阴性对照**不能**证"没问题"——
那是"没触发"与"没观测"同形。见 RELEASE_NOTES 的证据规范。

**不改**: 不新增公开 API。改动全部落在 `ipc.cpp` 的**匿名 namespace**(`:32`-`:1633`)内,
`acquire_storage` 是内部链接函数, 签名变化**无 ABI 影响**。

**产物**: 代码 + 一段"池空计数 > 0"的实拍 —— 见 §5。

**用例**: `test/test_pool_exhaust_observability.cpp`(两个用例)

| 用例 | 方向 | 构造 |
|---|---|---|
| `ExhaustionIsReported` | 阳性 | 接收方连上但**从不 `recv`** ⇒ 无 `buff_t` 构造 ⇒ 无 `recycle_storage`。前 32 条各取一块, 第 33 条必中 `-1`。**三档池各跑一次**: 7168 / 5120(锁住节流粒度缺陷) / **带非空前缀 `poolobs_a` 的 4096** |
| `NoReportWhenPoolCycles` | 阴性 | 8 轮 send→recv→`buff_t` 析构归还, 池内最多 1 块在外 |

阴性用例带一个 `fprintf(stderr, kProbe)` 探针并先断言"探针必须被捕获" —— 否则
"没报耗尽"与"根本没捕获到 stderr"完全同形, 用例会假绿。

⛔ **为什么要第三档(带前缀)**: 前两档的 prefix 本来就是空的, 于是"正确打出了空
prefix"与"代码里硬编码了一个空串"**完全同形** —— 那两条断言不承重。变异臂 D 实测
坐实了这一点: 把 `prefix.c_str()` 硬编码成 `""`, **只有第三档转红**(红集恰为 1 条),
前两档的 `prefix = ''` 断言照样绿。

**变异验证(每一步都实跑, 驱动 `/tmp/mut_poolobs.py`, 逐臂记红集)**:

| 臂 | 变异 | 期望 | 实测 |
|---|---|---|---|
| A | 删掉 `note_pool_exhausted(...)` 调用 | 阳性转红, 阴性仍绿 | ✅ 阳性红集 13 条, 阴性绿(其探针断言通过 ⇒ 红的是缺报告, 不是捕获面死) |
| B | 在阴性用例捕获窗口内注入一次真耗尽(全新档 6144) | 阴性转红 | ✅ 阴性红集 1 条 = `EXPECT_FALSE(contains(err, kMarker))`, 失败原文里注入的报告(`chunk_size = 6144`)与探针 marker 同时可见 ⇒ 阴性断言承重 |
| C | 把节流退回"跨档共用一个计数" | 阳性第二、三档转红 | ✅ 红集 6 条 = 第二档 4 条 + 第三档 2 条; 第一档仍 `count = 1` ⇒ 缺陷可复现, 断言是判别式 |
| D | `prefix.c_str()` → 硬编码 `""` | 只有第三档转红 | ✅ 红集 1 条(第三档 `prefix = 'poolobs_a'`) ⇒ 前两档断言确实不承重, 第三档是唯一判别式 |
| E | 空前缀点明句改为无条件输出 | 只有第三档转红 | ✅ 红集 1 条(第三档 `EXPECT_FALSE(含有"空前缀")`) ⇒ 点明句确实是条件输出 |

五臂红集**互不相同**且各自落在该落的断言上 —— 这是"每条断言都有独立判别力"的证据,
而不只是"改了代码就会红"。跑完还原后 `ipc.cpp` / 测试文件 md5 与变异前逐位一致。


### ✅ 步骤② 量出实际是谁钉的

**结论(一句话): 用户态 `view_queue_` 是唯一主导者。** 在"每话题 1 发布 / 1 订阅"的默认
拓扑下, 池里的 32 块**全部**由队列钉住, "环内在飞"与"回调中"合计 ≈ 0:

```
L = Qv        (通道 A 外部直读 == 通道 B 直读, 逐点精确相等)
R = L − Qv  ≈ 0
```

⇒ 对照 §4 第一行: 主导者**不是**"环内在飞", **④ 未被否掉**, 仍按"③ 判据成立则不做 ④"的门走。
**步骤③ 的前提成立。**

#### 判别点与扫描表

`queue_size` 是主变量。判别式: 队列主导预测 `L ≈ min(queue_size, 32)`; 环主导预测
`L ≈ 32` 且**与 `queue_size` 无关**。⇒ **`--queue=2` 是唯一判别点** ——
它给出 `L=2` 而不是 32。

插桩构建(通道 A + B, `MODE=instr`, `MSGS=40000`, 每点攒 3 次有效):

| 参数 | 有效/尝试 | `L_p50` | `L_max` | `Qv_p50` | `Qm_p50` | `R_max` | 坏链样本 | 读法 |
|---|---|---|---|---|---|---|---|---|
| `--queue=1024 --drain=0` | 3/3 | 32 | 32 | **32** | 1024 | 0 | 0 | 饱和臂: 32 块全在队列 |
| `--queue=32 --drain=0` | 3/3 | 32 | 32 | **32** | 32 | 0 | 0 | 与池容量相等的边界 |
| `--queue=16 --drain=0` | 3/6 | 16 | 16 | **16** | 0 | 0 | 180 | `L` 跟着 `queue_size` 掉 |
| `--queue=8 --drain=0` | 3/3 | 8 | 8 | **8** | 0 | 0 | 0 | 同上 |
| `--queue=4 --drain=0` | 3/3 | 4 | 4 | **4** | 0 | 0 | 0 | 同上 |
| `--queue=2 --drain=0` | 3/5 | **2** | 2 | **2** | 0 | 0 | 120 | **判别点**: 队列主导预测 2, 环主导预测 32 ⇒ 实测 **2** |
| `--queue=2 --drain=1` | 3/3 | 0 | 0 | **0** | 0 | 0 | 0 | 应用持续取数 ⇒ 池里几乎无钉住 |
| `--queue=4 --hold=4` | 3/4 | 4 | 4 | 0 | 0 | **4** | 60 | 灵敏度正对照(块在应用手里) |
| `--queue=4 --hold=16` | 3/3 | 16 | 17 | 0 | 0 | **16** | 0 | 同上, 更大剂量 |
| `--queue=4 --pub-extra=16` | 3/4 | 4 | 4 | 4 | 4 | 0 | 60 | 环反臂 **不升** |
| `--queue=4 --pub-extra=32` | 3/8 | 4 | 4 | 4 | 4 | 0 | 300 | 同上, 33 个发布线程 |

干净构建(`MODE=clean`, 同样 11 个点, 通道 B 为 NA)的 `L_p50` 逐点对照:

| 参数 | 1024 | 32 | 16 | 8 | 4 | 2 | 2/`drain=1` | 4/`hold=4` | 4/`hold=16` | 4/`pe=16` | 4/`pe=32` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `L_p50` 干净 | 32 | 32 | 16 | 8 | 4 | 2 | 0 | 4 | 16 | 4 | 4 |
| `L_p50` 插桩 | 32 | 32 | 16 | 8 | 4 | 2 | 0 | 4 | 16 | 4 | 4 |

**11 个点逐行全等** ⇒ 插桩对通道 A 无影响(只加了两个 relaxed store, 不碰池路径)。
这条是对"你量的是插桩构建"的正面回答, 不是回避。

#### 交叉验证(用户选定: 两条通道**必须对得上**, 不是"都非零")

| # | 判据 | 结果 |
|---|---|---|
| 1 | 逐样本 `Qv ≤ L` | `qgtl=0` 覆盖全部 11 点 × 全部 rep; 基准内硬计数, 违反即 `exit 7` |
| 2 | 通道 A 扫描反推的队列占用 == 通道 B 直读 | 上表 `L_p50` 与 `Qv_p50` **两列逐行精确相等** |
| 3 | 插桩保真度 | 干净/插桩两表 `L_p50` 11 点全等(上表) |

判据 2 是承重的那条: 只有一条通道时, `L=2` 可以被读成"池没被占住"; 两条同时给出同一个数,
才把"这 2 块**在队列里**"钉死。判据 3 的 `Qm_p50` 列另有一条独立结论 —— 见下文发现 (c)。

#### 非退化与对照

- **量具非盲**: `--queue=1024` 的 `L_max=32 > 0` —— 读数先在已知非零的构造上自证。
- **`R` 是活量而非结构性零**: `--hold=N` 让 N 块**离开队列、留在应用手里**,
  `hold=4 ⇒ R=4`、`hold=16 ⇒ R=16`, 而同期 `Qv=0`。⇒ `R ≈ 0` 是**测出来的**, 不是分母为零。
- **`fresh=1`**(起始空闲链走满 32)在每次采样里仍是硬前提, 不成立即 `exit 3`。
- **环反臂**(本计划书要求"不构造这一臂, `R≈0` 就分不清是结论还是盲区"): **未能构造出来**。
  17 与 33 个发布线程压 1 个订阅线程, `R_max` 仍 ≈0。这不是盲区, 是结论:
  生产端单条成本 > 订阅端, 订阅线程追得上。⚠️ 早期一次独立运行曾见 `R_max=28`,
  但该次 `badloop=17` —— 链持续损坏会让走链提前终止、**少算 `free`、虚高 `L`**;
  8 rep 系统表征下 `pub-extra ∈ {8,16,32}` 的 `R_max` 基本恒 0。该观测已按**测量伪影**处置,
  基准与驱动脚本里的预言也已从"R 可达 28"改为"R 仍为 0"。详见发现 (d)。

#### ⚠️ 本步顺带订正: 池占用的读法公式

§5 步骤① 那版读法写的是 `cursor_ == 32 即池空`。**这在单调场景(只借不还)里成立,
一般情形是错的**: `cursor_` 是空闲链的**头**, 不是"已借出计数"。

正确读法 = 读段内 `[0,33)` 字节, 从 `b[32]` 起沿 `next_[]`(每项 1 字节)走链直到 32,
**步数 = 空闲数**:

```
free = 走的步数(必须步数受限, 自环否则死循环);   L = 32 − free
```

反例就在本机的**孤儿段**上(即 §5 环境事实 1 列出的那四个, 走链全部 `free=32` 即 `L=0`):

| 段 | `cursor_` | 错误公式 `32 − cursor_` 给 | 正确走链 |
|---|---|---|---|
| `…__1024` | 27 | 5 | `free=32` ⇒ **`L=0`** |
| `…__132096` | 6 | 26 | `free=32` ⇒ **`L=0`** |
| `…__2048` | **0** | **32** | `free=32` ⇒ **`L=0`** |
| `…__929792` | 16 | 16 | `free=32` ⇒ **`L=0`** |

第三行是决定性的: 一个**彻底空闲**的池(`L=0`), 错误公式给出 32 —— 即"全部在用",
方向完全反了。⇒ 本步全部读数**走链**取得, 不走 `32 − cursor_`。走链器: `docs/probe_pool_chain.cpp`。

**(仍需注意)**: `queue_size` 配到 1024 而池只有 32 块时, 队列就是**无界钉住** ——
`L_p50=32` 那两行正是它: 池被队列吃满, 此后任何新的借样都拿不到块, 回退 TLV。
这正是步骤③ 要收的口。

### ✅ 步骤③ 先试最便宜的杠杆:钉 `queue_size`

**做什么**: `view_queue_` 满时 drop-oldest **本来就是释放**
(丢 `shared_ptr<Sample>` → refcount 0 → `~Sample` → `recycle_storage` → 还池)。
所以池占用**今天已经被 `queue_size` 界住了**, 只是这个界可以配得比池大。

⇒ 把容量钉到池容量以下, `min(queue_size, ViewQueueCap())`, 锚点 `shm_pub_sub_ipc.cc:454`。

**成本**: 一行。不动 wire, 不破互操作。
**代价**: 队列更早 drop-oldest ⇒ 应用更容易丢样。(⚠️ 本步**未实测量**"应用少拿到多少" ——
`--drain=0` 臂里应用从不取数, 丢样不可观测; 有此顾虑的场景应量 `try_get` 成功率后再裁。)
**判据**: 步骤①的计数从 `> 0` 变 `== 0`(或量级显著下降)。

#### 落法(用户拍板)

**默认钉 + 公开 opt-out API**。`queue_size` 在 `SubscriberIPCPtrMake` 里是**必填无默认**的参数
(`include/dzIPC/dzipc.h:76`), 在库里改写它属于"覆盖调用方的显式输入" ⇒ 覆盖必须**可观测**,
不能静默。

| 项 | 落点 |
|---|---|
| 开关 | `dzIPC::EnableViewQueuePin(bool)` / `IsViewQueuePinEnabled()`(默认 **ON**) |
| 容量 | `dzIPC::ViewQueueCap()` = `ipc::large_msg_cache / 4` = **8** |
| 生效 | `min(queue_size, ViewQueueCap())`, 订阅者**构造时**读一次 ⇒ 只影响之后新建的订阅者 |
| 诊断 | 生效时打一条**一次性** stderr(按被请求的 queue_size 去重), 明写"被钉到 N"与如何恢复 |
| 头文件 | `include/dzIPC/common/nodelet_config.h`(与 `EnableDzFlat` 同一体例) |

**为什么钉到 8(= 池容量/4)**: 32 块里留 24 块头寸给环内在飞、同进程其他话题、同机其他进程
—— 池按尺寸档**全机共享**(建 route 不带 prefix, 见 §5 环境事实 1), 钉满 32 等于把别的使用者挤死。

**射程**(为什么只钉 SHM 的 view 队列):
- `view_queue_` 只在"DZFlat + typed"的借样路径被 push(§2), TLV/schema-less 走**物化**的
  `msg_queue_` ⇒ 钉它对非 DZFlat 话题零影响; `msg_queue_` 刻意**不**钉(缩它只是白减应用缓冲)。
- socket/UDP 侧也有 view 队列, 但其 Sample 持有的是接收层去帧出来的**独立堆块**
  (`socket_pub_sub_ipc.cc:758-797`), 不占 chunk 池 ⇒ 无需钉。

⚠️ **单订阅者钉住 ≠ 全机不耗尽**: 4 个同尺寸档订阅者各钉 8 块仍会用满 32。跨进程隔离要靠
prefix(见 `docs/unfixed_defects.md` UF-003), 不在本开关射程内。

#### 实测落点: 判据成立

同一二进制 A/B(`--no-pin=1` 即钉 OFF), `--queue=1024 --drain=0 --msgs=40000`, 由
`docs/pool_attribution_run.sh` 的 §步骤③ A/B 段机械核对:

| 臂 | 有效/尝试 | 坏链 | `fb`(池空回退) | `rx_acc`(零拷贝收下) | `L_p50` |
|---|---|---|---|---|---|
| off(钉前) | 3/3 | 0 | **39993** / 40000 | 33 | 32 |
| on(钉后) | 3/5 | 120 | **1** | **40025** | 8 |

⇒ 回退 39993 → 1, 零拷贝接收 33 → 40025: 判据("`>0` 变 `==0`, 或量级显著下降")**成立**。

⚠️ **`fb=1` 是首帧瞬态, 不是"几乎修好了"**: 它与 `--msgs` 无关(2000/20000/60000 三档实测均
恒为 1, `rx_acc` 恒为 `msgs+25`), 是首条发布时接收方尚未就绪、借不到块的**固定一条**开销。

⚠️ **钉臂的 `坏链=120` 是既有缺陷被暴露, 不是钉造成的** —— 见下, 这是本步最重要的副产品。

#### ⚠️ 副产品: 空闲链自环抓到了**指纹**, 且它才是残余池空的主导者

步骤② 登记的发现 (a)("池空闲链会间歇性自环")在本步拿到**直接指纹**, 并纠正了它的定位。

**指纹**(`docs/probe_pool_chain.cpp` 外部直读, 在基准运行中抓取):

```
__IPC_SHM__CHUNK_INFO__12288   cursor_=23
next_[0..32] = 23 10 16 11 30 23 26 13 29 25 23 23 19 26 22 28 10 12 23 27 1 10 23 23 31 3 8 10 7 10 15 6 23
走链: ok=0 free=-1 L=-1
```

其余 `next_[5]=next_[10]=next_[11]=next_[18]=next_[22]=23` 都是"23 当头时被正常 release 的
节点"(`release(id)` 把头写进 `next_[id]`), **唯一异常的是 `next_[23]==23`**。而
`next_[id]==id` 的成因只有一条: `release(id)` 发生时 `cursor_` **已经是 `id`** ⇒ **同一 id
二次入池**。后果不是"容量缩水"而是: `acquire()` 从此**永远返回 23**(`cursor_ = next_[23] = 23`),
池只剩一块且**反复发放同一块** ⇒ 两个发布者会同时持有同一 chunk。

**它不是钉造成的**(控制臂: 钉 OFF + 小队列以留出空闲块):

| 臂 | q | 3 次 rep 的 `badloop` | 对应的 `fb` |
|---|---|---|---|
| 钉 OFF | 4 | 0 / **100** / 0 | 1 / **742** / 438 |
| 钉 OFF | 8 | 0 / **100** / 0 | 1 / **129** / 1 |

⇒ 与钉无关。**但 `--queue=1024` 时 OFF 臂的池是满排空的**(`cursor_=32`) ⇒ 走链一步即终止 ⇒
**自环对 OFF 臂不可见**, 只有钉留出了空闲块才走得到它。这正是步骤② 时"坏链只在低 queue 出现"
的原因 —— 在**钉之前**的构建上, 低 queue 本来就留空闲块。

**相关性是 1:1 的**: 干净链 ⇒ `fb=1`; 坏链 ⇒ `fb` 跳到 129~11856。⇒ **步骤③ 之后的残余池空
不是"环内在飞", 而是这个既有缺陷** —— §4 的"环主导 ⇒ ④ 全白做"那道门**仍未触发**。

**机制在源码里已被登记, 闸也加了, 但漏了一条路径**: `ipc.cpp:676` 与 `:726` 两处注释写明了同款
机制("`next_[id] = cursor_` 而 `cursor_` 已是 id ⇒ 自环 ⇒ 每次 acquire 返回同一 id ⇒ 所有大消息
共用一块 chunk 互相踩踏"), 并在 `discard_storage` 里加了 `rem_cc == 0 → return` 的闸。
**闸在而自环仍在** ⇒ 还有未受闸保护的二次入池路径, 而 `discard_storage` 自己的注释已点出残余窗口:

> 残余窗口(本函数未关闭): pop() 先把槽位数据拷出、之后才清自己的 rc 位, 所以一个"已读到
> storage id 但尚未清位"的接收方仍会被算进 rem_cc … 一旦让接收方长期持有 chunk(DZFlat 的
> Sample), 该窗口会被拉成秒级并必现。

DZFlat 的 view 队列**正是**"接收方长期持有 chunk" ⇒ 与实测吻合。**归属登记, 不在本步射程**
(它是 libipc 的回收竞态, 既不是"队列配得深"造成的, 也不是 wire 设计问题)。

**若判据不成立** ⇒ 主导者不是队列, 回步骤② 重新归因, 不要继续走④。(本步**判据成立**, 见上。)

### ⬜ 步骤④ 只有当③钉到 1 还不够, 才做「解钉 + 世代校验」

这是**一次 wire 级重设计**, 不是"加个字段"。四道坎:

**(a) API 形态** —— 当前 `get(Sample& out)` / `try_get(Sample& out)` 是**输出参数不是回调**
(`pub_sub_base.h:69-84`)。`Sample` 活过 `try_get`, pin 就只能跟到 `Sample` 析构。
要把 pin 窗口缩到"回调期间", 必须**新增回调形态**:

```cpp
bool try_get(std::function<void(Sample const&)> cb);
```

**(b) 竞态(这道是硬的)** —— 解钉之后:

```
读方: recv(id) ─────────────────────► try_get: 读 gen
写方:        acquire(id) gen++ ─► memcpy ─► push
```

读方读 gen 时可能已是**新一代** ⇒ 校验通过但载荷是新消息 ⇒ **静默错数据**。
要堵只能做 seqlock(读 gen → 用载荷 → 再读 gen → 比对), 而载荷是零拷贝的,
没有"再读一次"这回事 ⇒ 只能在 `try_get` 内部重试。

**(c) gen 字段放得下, 但必须拍互操作** —— `chunk_t` 真实布局:

```
offset  0..3   std::atomic<cc_t>     ← conns()
offset  4..15  12 字节 padding(无人使用)
offset 16..    payload               ← data() = this + make_align(16, 4)
```

`make_align(16, 4) = 16`, 所以 **offset 8 放 `atomic<uint64_t> gen` 是 8 字节对齐的,
payload 偏移不变**。锚点: `ipc.cpp:243-256`(布局)、`ipc.cpp:284-285`(源码注释自己写了 `+ 16`)、
`utility.h:59-62`(`make_align`)。

⛔ **互操作**: chunk 段每 prefix 一份、跨进程共享。新旧混跑时老生产者不 bump gen
⇒ 新读方读到恒 0 ⇒ `0 == 0` 校验通过 ⇒ **保护完全失效且无任何迹象**。
同 UF-003 的模式(`chunk id 经环传递 ⇒ id 空间是跨进程契约`)。**先拍互通性, 再动字段。**

**(d) 契约①要重写** —— `sample_message.h:9-13` 的三条契约里第 ① 条:

| | 今天 | 改后 |
|---|---|---|
| `try_get` 返回 true | 保证可读 | 保证**取到后**到 `~Sample` 可读 |
| 队列非空时 | 必返回 true | 取到的那条可能已过期 ⇒ 可能 false, 要重试循环 |
| 丢谁 | 队列满 ⇒ 丢最旧(确定性) | 生产者复用 ⇒ 丢随机(时序决定) |

**安全性没退**(`Sample` 活着时仍钉着), 退的是「**队列里的一定能取到**」。

## 4. 什么时候不做

| 条件 | 结论 |
|---|---|
| 步骤② 显示主导者是"环内在飞" | ⛔④ 全部白做, 转去治环 |
| 步骤③ 判据成立 | ⛔不做④ |
| 步骤① 阴性对照跑不出阳性对照 | ⛔观测面不可信, 先修观测面 |
| 互通性拍不了 | ⛔④ 不做(会静默分裂 id 语义) |

> **本步实测落点(步骤② 已出结论)**: 第一行**未触发** —— 主导者是用户态队列, 不是"环内在飞", 所以 ④ **不因本行被否掉**, 仍挂在"③ 判据成立则不做 ④"这道门上。
> 但准确表述附一条限定: "环不主导"是在**本基准可构造的负载范围内**成立(33 发布线程对 1 订阅线程仍 `R_max≈0`), 见 §3 步骤② 与 §5 发现 (d)。
>
> **步骤③ 已出结论(2026-09-19)**: 第二行**触发** —— 判据成立(`fb` 39993 → 1, 零拷贝接收 33 → 40025)
> ⇒ **⛔ 不做步骤④**。④ 是 wire 级重设计(新增回调形态的 `try_get` + 世代校验), 既然最便宜的杠杆
> 已经把回退打掉三个数量级, 就不该再动 wire。
> ⚠️ 留一条**重新评估的入口**: 若将来要治那个**空闲链自环**(步骤③ 的副产品, 见 §3 步骤③), 那是 libipc
> 的回收竞态, 与 ④ 的 wire 重设计**不是同一件事**, 不要把它当作"回去做 ④"的理由。

## 5. 实际结果(做完回写)

### 三方指纹(证据规范, RELEASE_NOTES 0918 起)

| 键 | 值 |
|---|---|
| `tree_head` | `2f136658aa092f18bb5b157a729bed51331fea75` (dev; 工作树脏: `M src/libipc/ipc.cpp` + 三个新文件) |
| `libipc_md5` | `979ac73fd94319df470f50f82aefad0e` (`build/lib/libipc.so.1.3.0`) |
| `driver_md5` | `e3da6b7518c21864eaf7133b98a5d28d` (`build/bin/test_pool_exhaust_observability`) |
| 源码 md5 | `src/libipc/ipc.cpp` = `2336bbea4f131e61e2f143b91812314f`;`test/test_pool_exhaust_observability.cpp` = `923a01b3bd7e5f4fe799e1dc7c5f14d0` |

> 前一版指纹(`libipc_md5 2ccafc5c…` / `ipc.cpp 838a2cc0…` / `test a0937e49…`)对应的是
> **补 prefix 字段之前**的状态, 已被本版取代;那一版的唯一实测结论(节流粒度缺陷可复现)
> 在本版由变异臂 C 重新坐实, 未丢。变异驱动留档: `docs/mut_poolobs.py`。

### 运行

```bash
cmake -S . -B build && cmake --build build -j8 --target test_pool_exhaust_observability
grep -l CHUNK_INFO /proc/[0-9]*/maps   # 前置: 必须为空(段名全局, 见 §1 结论 3)
LD_LIBRARY_PATH=$PWD/build/lib:$PWD/build/bin ./build/bin/test_pool_exhaust_observability
```

### 实拍(三档池, 连跑多次逐字节一致)

```
[ RUN      ] PoolExhaustObservability.ExhaustionIsReported
[pool-obs] captured stderr:
chunk pool exhausted: kind = send, chunk_size = 7168, size = 6144, pool capacity = 32, count = 1 (本进程), prefix = ''  <= 空前缀: 本档池无话题/进程区分, 全机共享, 归因须查其他进程

[pool-obs] captured stderr (2nd size class):
chunk pool exhausted: kind = send, chunk_size = 5120, size = 4096, pool capacity = 32, count = 1 (本进程), prefix = ''  <= 空前缀: 本档池无话题/进程区分, 全机共享, 归因须查其他进程

[pool-obs] captured stderr (prefixed pool):
chunk pool exhausted: kind = send, chunk_size = 4096, size = 3072, pool capacity = 32, count = 1 (本进程), prefix = 'poolobs_a'

[       OK ] PoolExhaustObservability.ExhaustionIsReported (2 ms)
[ RUN      ] PoolExhaustObservability.NoReportWhenPoolCycles
[       OK ] PoolExhaustObservability.NoReportWhenPoolCycles (1 ms)
[  PASSED  ] 2 tests.     exit=0
```

三行读法: 前两行是**默认(空前缀)**部署 —— 两个不同话题、两个不同尺寸档, 报出来的
`prefix` 都是空的, 这就是"同尺寸档全机一池"的可见形式; 第三行是**带前缀**的池,
`prefix = 'poolobs_a'` 原样报出, 且**没打**那句"空前缀"点明(点明句是条件输出)。

### 邻接回归(libipc 改动必跑)

| 用例 | 结果 |
|---|---|
| `test_chunk_hold` | 3/3 绿 |
| `test_lap_safety` | 3/3 绿 |
| `test_uf007_id_pool_double_release` | 2/2 绿 |


### 步骤② 三方指纹(量测工具链)

| 键 | 值 |
|---|---|
| `tree_head` | `2f136658aa092f18bb5b157a729bed51331fea75` (dev) |
| `libipc_md5` | `979ac73fd94319df470f50f82aefad0e` (`build/lib/libipc.so.1.3.0`) — **与步骤① 同值**, 即本步未动 libipc 行为 |
| `bench_md5` | `60420f0445dca21ebbab72c356b25b74` (`build/bin/pool_attribution_benchmark`) |
| `bench_src` | `test/pool_attribution_benchmark.cpp` = `323bda784e03038f0651742d1a377eb3` |
| `instr_py` | `docs/pool_attribution_instr.py` = `7fd50ffbd0f4e397a5edc25c2fc4ab57` |
| `run_sh` | `docs/pool_attribution_run.sh` = `3ba412dc4ae31c0c413be256725c913d` |
| `cmakelists` | `test/CMakeLists.txt` = `4a31235e33ad6c9a7b6232303685c3df` |
| 产品码 | `src/dzIPC/shm_pub_sub_ipc.cc` = `f47164b955d3934ddef509b77d02d307` (= HEAD); `src/libipc/ipc.cpp` = `2336bbea4f131e61e2f143b91812314f` |

### 步骤② 插桩还原证据

通道 B 是**临时**改动, 量完必须逐字节复原。两层自证, 缺一不可:

| 层 | 证据 |
|---|---|
| 内容 | `python3 docs/pool_attribution_instr.py off` 打印 `md5 f47164b9… vs 打补丁前 f47164b9… -> ✅ 逐字节一致` |
| **版本控制** | `git status --porcelain src/dzIPC/shm_pub_sub_ipc.cc` **输出为空** ⇒ 与 HEAD 逐字节相同 |

第二层比第一层强: md5 一致只证明"与**我记下的那个数**相同", 而 `git status` 空证明
"与**版本库里的字节**相同"。同时 `libipc_md5` 也回到 `979ac73f…`, 与步骤① 记录逐位相同。

打补丁期间的**符号非退化正对照**(防"通道 B 静默降级成通道 A"这种读起来像同意的情况):

```
[on] 动态符号表: ✅ B _ZN5dzIPC3shm8pa_instr10g_pa_msg_qE; B _ZN5dzIPC3shm8pa_instr11g_pa_view_qE
```

驱动在符号数 ≠ 2 时 `exit 1`; 运行时若 `instr != 1`, `pool_attribution_run.sh`
一律把该次判为**无效**并单独计数 —— 不允许 `instr=0` 的数据混进表里。
⚠️ 踩坑留痕: 插桩计数器最初放在 `dzIPC::shm::detail`, 会**遮蔽**本 TU 既有的
`detail::NoteDzFlatRx` 限定名查找(6 处编译失败); 改写成 `namespace dzIPC { namespace detail {`
得到的是 `dzIPC::shm::dzIPC::detail`(更错), 而 `namespace ::dzIPC::detail` 不是合法文法。
最终用自命名子空间 `dzIPC::shm::pa_instr`。两条坑都写在 `instr.py` 的 `HUNKS` 注释里。

### 步骤② 邻接回归(libipc / dzIPC 路径改动必跑)

| 用例 | 结果 |
|---|---|
| `test_chunk_hold` | 3/3 绿 |
| `test_lap_safety` | 3/3 绿 |
| `test_uf007_id_pool_double_release` | 2/2 绿 |
| `test_dzflat_rx` | 8/8 绿 |
| `test_dzflat_transport` | 9/9 绿 |

### 步骤② 登记的四项发现(**不属步骤② 的射程**, 单独挂账)

#### (a) ⚠️ 池空闲链会**间歇性自环**(可复现, 未修)

**现象**: 走链时遇到 `next_[X] == X`(或等价地走满 32 步仍不回 32) ⇒ 链损坏。
判据可机械核对: 基准 64 次尝试里**过半数**走不出链 ⇒ 报 `badloop`, 单列计数。

**实测**: 与参数无关、**随机**发生(同参数换一次跑就变), 低位 `queue_size` 更易命中。
本轮 11 点里 7 点出现过, `badloop` 累计 60~300 样本。**不是**由回退 TLV 的量驱动:
`--queue=4` 曾出现 `badloop=60` 而同期 `fb=1`。

**影响(若为真)**: `acquire()` 会永远返回同一个 id ⇒ 多个生产者拿到**同一块 chunk**,
数据互相覆盖且无报错。形态与 `test_chunk_hold.cpp` 头注释里登记的那个缺陷同型。

**处置**: 本步**只登记不修** —— 它是量测的噪声源(会把 `L` 虚高), 但不是步骤② 的题目。
已在驱动里用"重试到攒够有效次数 + 有效性单列上报"隔离, 不做任何"失败当 0"的补偿。

**⚡ 步骤③ 补充(2026-09-19): 已抓到直接指纹, 上面那个"若为真"坐实了** —— 见 §3 步骤③ 的
"副产品"段: 运行中外部直读到 `cursor_=23` 且 `next_[23]==23`, 即**同一 id 二次入池**; 后果
确为"`acquire()` 永远返回同一个 id"。并且它**与钉无关**(钉 OFF 的小队列臂同样复现: `q=4`/`q=8`
各 3 次里各有 1 次 `badloop>0`), 只是在 `queue_size ≥ 池容量`(池被满排空, `cursor_=32`)时
**不可见** —— 走链一步即终止, 走不到那个自环。此处"低位 `queue_size` 更易命中"的现象由此得到
解释: 低位才留空闲块。**仍不在步骤②③ 的射程, 只登记不修。**

#### (b) 尺寸类的**双重取整**推导(读错池会静默取到另一个池)

借样档位**不是** `calc_chunk_size(dzflat_size())`。`loan()` 与 `acquire_storage()` 各取整一次:

```
class = calc_chunk_size( loan_size_class( dzflat_size() ) )
```

实测: `dzflat_size()=7600` ⇒ `loan_size_class` ⇒ `cap=8192` ⇒ `calc_chunk_size` ⇒ **`class=9216`**。
而 `calc_chunk_size(7600)` 给 8192 —— 那是 **TLV 回退路径**的档位, 同一个进程里**两个池并存**。
⇒ 读错档位时读数**不会报错**, 只会静默读到另一个池。本步基准的 `--payload=11000`
对应档位 `12288`, 段名写死在驱动脚本的 `CLS` 里, 并由 `fresh=1` 前提兜底。

#### (c) `msg_queue_` **不钉** chunk, `dzflat_adopt` 才钉

`dzflat_adopt`(`generic_message.hpp:171`)确实钉住借来的缓冲区
(`dzflat_borrow_ = std::make_shared<ipc::buffer>(std::move(buf))`), 但那只在
**无 schema 话题**路径上才会走到; TLV 回退路径经 `AcceptWire` **物化**(把段字节拷进
消息自己的 vector) ⇒ **不钉**。实测坐实: `--queue=16` 时 `L_p50=16` 而 `Qm_p50=16` ——
若 `Qm` 也钉块, `L` 应当是 32。⇒ 池容量只与 `view_queue_` 有关, `msg_queue_` 是旁观量。
(这也是为什么本步把 `Q = Qv` 而不是 `Q = Qv + Qm`; 早先按后者算会得到
`Q = 2 × queue_size`、`R` 恒为负数, 是错的。)

#### (d) 环反臂**未成立** + `R_max=28` 伪影订正

见正文"非退化与对照"末条。要点: 33 发布线程对 1 订阅线程仍 `R_max≈0`;
唯一一次 `R_max=28` 伴随 `badloop=17`, 归因于链损坏导致的 `free` 少算/`L` 虚高,
**不是**环内积压。基准与脚本里的预言据此从"R 可达 28"改为"R 仍为 0"。
⇒ 结论的准确表述是"**在本基准可构造的负载范围内**队列是唯一主导者",
而不是"环在任何负载下都不主导"。

### 步骤③ 三方指纹与邻接回归(2026-09-19)

| 键 | 值 |
|---|---|
| `src/dzIPC/shm_pub_sub_ipc.cc` | `6b70d30a0e76dedf1df160f3bdfaa0da`(钉的落点, 含一次性诊断 `warn_view_queue_pinned`) |
| `src/dzIPC/common/nodelet_config.cc` | `190fd742d785f35bb5a97d90a77bf575` |
| `include/dzIPC/common/nodelet_config.h` | `9c54b7c01edf29d10a7ca36a7b006ec9`(新增三个公开 API) |
| `test/pool_attribution_benchmark.cpp` | `f75b9cd0f18c391f8832d2067d2cd4a3`(新增 `--no-pin=1` A/B 开关 + RESULT 增 `pin`/`vcap` 列) |
| `docs/pool_attribution_run.sh` | `82403d82ed0a24edf2d58a9c8adf3e55`(新增步骤③ A/B 段; 扫描臂硬编码关钉) |
| `src/libipc/ipc.cpp` | `2336bbea4f131e61e2f143b91812314f`(**步骤①** 的池空观测点, 本步未动) |
| `build/lib/libipc.so` | `88fb143b56f4ee9c4074fd07ad6b1999` |
| `build/bin/pool_attribution_benchmark` | `fb4e6d1944564d706bc7d1c1812f7e94` |

**邻接回归(25/25 全绿)**: `test_chunk_hold` 3/3, `test_lap_safety` 3/3,
`test_uf007_id_pool_double_release` 2/2, `test_dzflat_rx` 8/8, `test_dzflat_transport` 9/9。

⚠️ `test_uf007_id_pool_double_release` **是绿的** —— 它覆盖的是 `discard_storage` 那道
`rem_cc == 0` 闸**已生效**的路径; 本步实测到的自环走的是该函数注释自陈"**本函数未关闭**"的
残余窗口。两者不矛盾, 且正好划出了该用例的覆盖边界。

#### ⚠️ 扫描臂的可复现性依赖: 必须显式关钉

步骤② 的归因扫描(`L ≈ min(queue_size, 32)`)是在**钉之前**的条件下量出来的。步骤③ 落地后钉
默认 ON, 若照默认重跑该扫描, `queue > 8` 的点会被压到 8 而**预言列仍写着 32** —— 表看起来是
绿的, 实际早已失效(实测: 未加固前 11 点**全部**读出 `L_p50=8`, 而脚本报"失败 0")。

⇒ `docs/pool_attribution_run.sh` 的扫描臂已**硬编码 `--no-pin=1`**, 并新增一条 `pin != 0` 即判
该点无效的断言(而不是让它带着失效的预言继续报绿)。复现步骤② 的结论**必须**走这个脚本,
不能手敲参数。

### 步骤表

| 步骤 | 状态 | 结果 |
|---|---|---|
| ① | ✅ | 已落码 + 双向判据 + 变异五臂(见 §3)。两处自我订正: 节流粒度从"进程内单计数"改为"按 (kind, chunk_size)"; 报错补 `prefix` 归因字段(含空前缀点明、count 标注本进程), 并用带前缀的第三档把该字段证成承重断言 |
| ② | ✅ | 队列主导(`L == Qv == min(queue_size,32)`, `R≈0`), 两条通道逐点精确相等。工具: `test/pool_attribution_benchmark.cpp` + `docs/pool_attribution_run.sh` + `docs/pool_attribution_instr.py`。⇒ **③ 前提成立**。顺带订正了池占用读法公式(`32 − cursor_` 是错的, 须走链), 并登记四项发现(见 §5) |
| ③ | ✅ | 判据成立: `fb`(池空回退)39993 → **1**, 零拷贝接收 33 → **40025**。落法 = 默认钉 + 公开 opt-out API(`EnableViewQueuePin` / `IsViewQueuePinEnabled` / `ViewQueueCap` = 池容量/4 = **8**), 生效时一次性 stderr 诊断。**副产品**: 抓到空闲链自环的**直接指纹**(`next_[id]==id` 且 `cursor_==id`), 证明钉后残差来自**既有**二次入池缺陷而非"环内在飞" ⇒ **④ 不做**(见 §4) |
| ④ | ⛔ | 不做 —— 步骤③ 判据成立(§4 第二行触发) |

### 步骤① 顺带确认的两条环境事实(供步骤②③ 用)

1. **池段在进程正常退出时被 unlink** —— 跑完 `/dev/shm` 里不剩本文件用到的任何一档
   (`7168` / `5120` / `3072` / `4096`, 含带前缀的 `poolobs_a__…__4096`; 变异臂 B 临时
   用的 `6144` 也同样不剩)。残留的是 `1024` / `2048` / `132096` / `929792`, 来自
   **异常终止**(且当时无活持有者)。所以用例里的 `reset_chunk_pool()` 是给异常终止
   兜底的, 不是每次都需要。段是**首次取块时懒创建**的(建 route 本身不建段)。

2. **进程内销毁 route 不归还它钉住的 chunk** —— 直读池的空闲链表头坐实:

   | 时刻 | `cursor_` | 含义 |
   |---|---|---|
   | 段刚清掉 / 建 route 后 | (段不存在) | 懒创建, 首次取块才建 |
   | 钉干 33 条后 | **32** | 池空, 32 块全在池外 ← 阳性对照, 证明读法非退化 |
   | **route 已销毁后** | **32** | **仍然全在池外 ⇒ 销毁不归还** |

   ⚠️ **本表第三列的"含义"只在"只借不还"的单调场景成立** —— `cursor_` 是空闲链的**头**,
   不是已借出计数。一般情形须**走链**, 见 §3 步骤② 末的订正小节(附四个孤儿段的实测反例,
   其中 `cursor_=0` 的池实际 `L=0`)。

   读法: `chunk_info_t` 首成员是 `id_pool<>`, 其 `cursor_` 在段内偏移 32
   (`next_[32]` 占 0..31)。段是 tmpfs 文件, 进程 mmap 的同时可按文件读同一份内存。
   探针: `docs/probe_teardown.cpp`(一次性, 属本文件, 不进 `test/` —— CMake 的
   `test/*.cpp` 是 GLOB, 放进去会多出一个非 gtest 的目标)。构建:
   `g++ -std=c++17 -I include docs/probe_teardown.cpp -o /tmp/probe_teardown -L build/lib -lipc -Wl,-rpath,$PWD/build/lib`

   ⇒ 对步骤②③ 的意义: 池的占用**不由连接生命周期界住**, 只由
   `Sample`/`buff_t` 的析构界住。想在步骤③ 靠"队列容量"收口, 前提是这个析构
   真的会发生 —— 步骤③ 必须先把这条量出来。

   ⚠️ 这条事实第一次做实验时读反了: 当时"销毁后再发一条, 没报耗尽"被读成"池恢复了",
   实际是节流把第二条报告吞了(见 §3 的变异臂 C)。直读 `cursor_` 才是与节流无关的读法。

