# DZFlat — SHM 专属平坦布局(取消序列化/反序列化)

> **状态**: 设计定稿, **Step 0~4 全部落地并验收**(见 §9.1~§9.6)。
> **目标读者**: 架构评估 + 实施者。
> **结论先行**: 把 SHM 侧的 wire 从「TLV 自描述字节流 + 1460 字节页尾分片」换成「定长根记录 + 段内相对偏移 + 追加式变长区」。序列化退化为一次布局写入, **反序列化整体消失**(读端用 View 直接寻址 chunk)。
> **实测(离线布局, §9.2)**: 10 万点云编码 **40× 更快、堆分配 100,004 → 0、wire 1.92× → 1.00×**, 消费者端到端 **75×**; 5.9 MB 图像编码 7.6× 更快。
> **实测(真实 SHM 链路, §9.4)**: 2 万点云发布 **~59×**(1607→27 µs/条), 0.88 MB 图像发布 **~3.2×**(63→22 µs/条)。
> **实测(B 级就地构造, §9.5)**: 把 A 级那一次"进 chunk"的成本整个消掉 —— 0.88 MB 图像 152~166 → 114~116 µs/条, 余下全是调用方自己填像素。
> **Python(§9.6)**: `topic_echo` 对 DZFlat 消息字段完整; 大数组以 `memoryview` 零拷贝返回, 而非 TLV 路径那种百万元素 list。
> **编写日期**: 2026-09-02
> **取代**: [loaned_blob_borrow.md](loaned_blob_borrow.md) — 该方案是本方案的退化特例(只把最后一个字段搬出 wire), 代价更大而覆盖面更小, **归档不实施**, 理由见 §2.3。
> **遗留问题登记表**: [dzflat_known_issues.md](dzflat_known_issues.md) —— 七条**已全部闭环**(§8 修第 3/4/5 条, §9 修第 1/2/6/7 条); 该表保留是因为每条的成因与判据对排查同类"不报错、只是结果不对"的问题仍然有用。

---

## 0. TL;DR

| 项 | 结论 |
|---|---|
| 替代结构是什么 | 定长 Root 记录(标量/定长数组/嵌套记录原地内联) + `{u32 off, u32 cnt}` 变长引用 + 追加式变长区。全段位置无关(relocatable) |
| 反序列化怎么消失的 | 读端 `View` 直接在 chunk 上寻址; 无字段名解析、无页尾剥离、无 `resize` 零填、无逐元素堆分配 |
| 为什么现在能做 | 页尾格式是 **UDP 的分片格式**(`data_rev.cc` 是真实消费者, §1.4), 全局改不动; 但允许为 SHM 加独立骨干后, SHM 不必再背 UDP 的形状 |
| 最大收益点 | 不是大 blob, 而是**嵌套数组**: 每元素一次堆分配 + 每元素重复写字段名 → 10 万点云 5.3MB/10 万次 `new` 变 2.4MB/0 次 |
| 两级 | A 级: 发端 1 拷贝 / 收端 0 拷贝, 反序列化消失。B 级: loan-and-build, 两端 0 拷贝 |
| 双 wire 如何共存 | 段首 magic 判别。TLV 首 4 字节是首字段名长度(小整数), 与 magic `0x4C465A44` 结构上不可能碰撞 → 混版本可检出而非静默错解 |
| 硬前置 | `force_push`→`clear_message` 的无条件 `release_storage` 必须先改成尊重 chunk `conns`(§5.1)。不修则任何延长 chunk 持有期的方案都会踩 ABA |
| 主要代价 | 双 wire 双维护(generator 出两套、测试矩阵翻倍)。这是长期维护税, 不是技术风险 |
| 兼容性 | 现有 owning struct、`serialize()/deserialize()`、socket/UDP 路径**原样保留**; 新增能力是并存而非替换 |

---

## 1. 成本定位: 三项, 不是一项

### 1.1 页尾交错

wire 每 1460 字节插一条 12 字节尾记录([ipc_msg_base.hpp:68-69](../include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp#L68-L69)), 于是 `adapt_memcpy_tos` / `adapt_memcpy_tods`([:94-152](../include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp#L94-L152)) 必须把每个字段切成多段 memcpy。4MB 图像 ≈ 2874 段, 发收各一遍。

### 1.2 多余的整包搬运

- `serialize_data_cut` 每次 `new uint8_t[total_size]`([:162](../include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp#L162));
- `send` 再把整包 memcpy 进 chunk([ipc.cpp:765](../src/libipc/ipc.cpp#L765));
- 收端 `data.resize(n)` 先零填一遍再被覆写;
- 收端每帧 `topic_msg_->clone()` 克隆一次消息模板([shm_pub_sub_ipc.cc:561](../src/dzIPC/shm_pub_sub_ipc.cc#L561))。

4MB 图像单帧 blob 相关内存写入 ≈ 4M(序列化) + 4M(进 chunk) + 4M(resize 零填) + 4M(反序列化) = **16MB**, 另加 2 次 4MB 级堆分配。

### 1.3 嵌套数组: 每元素一次堆分配(最严重)

```cpp
// std_point_cloud.hpp:41-46 —— 每个点一次 serialize() → 一次 new uint8_t[]
for (auto& nested_msg : points) {
    points_serialized.emplace_back(nested_msg.serialize());
    ...
}
```

`StdVector3d` 是 `float64[3]`(24 字节负载), 序列化后:

| 组成 | 字节 |
|---|---|
| `int32 name_size` | 4 |
| `"data"` | 4 |
| `uint8 type` | 1 |
| `int32 count` | 4 |
| 负载 | 24 |
| 自身尾记录(每个嵌套 `serialize()` 都会 `add_tail_msg`) | 12 |
| 父消息为该元素记的 `int32 nested_size` | 4 |
| **合计** | **53** |

**2.21× 膨胀, 且每元素一次堆分配、10 万个 `ipc::buffer` 同时存活。** 10 万点云单次 publish: 5.3MB wire + 10 万次 `new` + 20 万+ 次 memcpy。

> 注: 收端的逐元素分配已在上一步(嵌套反序列化借父 buffer, `region_is_contiguous`)修掉了; **发端仍是原样**。
>
> **实测已坐实**(§9.2): 10 万点云 `serialize()` 触发 **100,004 次堆分配**、耗时 7130 µs、wire 6,150,309 B(1.922× 膨胀)。

### 1.4 一条必须纠正的事实

页尾**不是**自产自销的。[data_rev.cc:255-262](../src/dzIPC/common/data_rev.cc#L255-L262) 直接按字节读 `page_cnt / now_page / total_size / msg_id`, [:330](../src/dzIPC/common/data_rev.cc#L330) 用它们做分片落位与一致性校验, 后续用于去重和 NACK 位图。**1460+12 是 UDP/RTPS 的分片格式, 不能全局替换。**

这正是本方案范围限定为 SHM 的原因: SHM 不需要 1472 字节 MTU 分片, 它被迫背着一套广域网形状。

---

## 2. 为什么是「改布局」而不是「绕过布局」

### 2.1 两种思路

| | 绕过(loaned_blob) | 改布局(DZFlat) |
|---|---|---|
| 手段 | 把单一大字段从 wire 里摘出去, 单独占一个 chunk | 整条消息换成可直接寻址的布局 |
| 覆盖 | 一个 `uint8[]`/`string` 字段 | 全部字段类型, 含嵌套数组 |
| §1.3 的 10 万次分配 | **治不了** | 归零 |
| 跨语言 | 该字段在 wire 中消失 → Python/`topic_echo` 丢字段 | 布局更简单, Python 用 `struct.unpack` + memoryview 直读, **变好** |
| chunk 头 | 需加 `refs` 字段 → 改共享内存布局 | 不动 |
| 投递原子性 | 拆成 header + chunk 两条消息 → 非原子 + ABA | 单条消息 |

### 2.2 现有代码里已有的两处先例

1. **接收侧本来就是零拷贝的**: `recv` 拿到 `msg.storage_` 后直接返回指进共享 chunk 的 `buff_t`([ipc.cpp:1102-1113](../src/libipc/ipc.cpp#L1102-L1113)), 回收挂在其析构上。缺的只是"让它活过订阅循环那一轮"。
2. **借用父缓冲已有实现**: 嵌套子消息在同页内时直接别名父 buffer([ipc_msg_base.hpp:186-190](../include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp#L186-L190))。blob 借不到的唯一原因就是 1460 分页让它永不连续。

DZFlat 做的事就是把第 2 条从"同页内的小字段"推广到"整条消息", 并让第 1 条的生命周期跟着消息走。

### 2.3 `loaned_blob_borrow.md` 归档理由

除 2.1 表格中的覆盖面与代价差异外, 该文档另有若干实现级问题(`std::span` 需 C++20 而本项目为 C++17; `chunk != 0` 判据错——有效 `storage_id_t` 从 0 开始、无效是 -1; `LoanedBlob` 只声明析构违反 Rule of Three 而收端每帧 `clone()`; `shm_map(id)` 缺 `size` 参数无法定位段; §4 的 `refs` 方案会丢掉现有 `conns` 位图的自愈性)。这些都可以逐条修, 但即便全修完, 它仍是 DZFlat 的一个覆盖面更小、代价更大的特例。**结论: 不实施。**

---

## 3. 布局规范 (layout_ver = 1)

### 3.1 段结构

```
chunk->data()  ← 8 字节对齐 (ipc.cpp:222-227, 由 static_assert 固定, §5.3)
┌──────────────────────────────────────────────────────────────┐
│ SegHeader  (32 B, 8 对齐)                                     │
│   u32 magic        0x4C465A44  ('D','Z','F','L' 小端)         │
│   u32 schema_hash  字段结构哈希, 收端一次性校验 (§3.5)          │
│   u32 root_off     = 32                                       │
│   u32 root_size    sizeof(XxxRoot)                            │
│   u32 total_size   全段字节数(含本头)                          │
│   u16 layout_ver   = 1                                        │
│   u16 flags        bit0 = 变长区已封口                         │
│   u32 msg_id       沿用 dz_ipc_msg_id 语义                     │
│   u32 reserved     = 0                                        │
├──────────────────────────────────────────────────────────────┤
│ Root record  (定长, 编译期已知, 8 对齐)                        │
│   标量           → 原地内联, 自然对齐                          │
│   定长数组 T[N]  → 原地内联                                    │
│   嵌套消息       → 原地内联其 Root(**任意深度都内联**)          │
│   string         → VarRef{off, byte_len}                      │
│   T[] (T 标量)   → VarRef{off, elem_cnt}                      │
│   string[]       → VarRef{off, cnt} → cnt 个 VarRef 表         │
│   Msg[]          → VarRef{off, cnt} → cnt 个连续 MsgRoot        │
├──────────────────────────────────────────────────────────────┤
│ Varlen area  (追加式, 每块 8 对齐)                             │
└──────────────────────────────────────────────────────────────┘
```

`VarRef = {u32 off; u32 cnt;}`, `off` 是**相对段首(SegHeader 起点)** 的字节偏移, `off == 0` 表示空。

两条由此得到的性质:

- **位置无关**: 全段无指针, 可整体 memcpy 到任何地址、写文件、发 socket, 语义不变。
- **嵌套零间接**: 嵌套 Root 一律内联, 只有它自己的变长负载落到变长区。所以任意嵌套深度的**记录部分**都是一次指针加法, 不存在逐层解析。

### 3.2 性能分级(不是支持门槛)

| 级 | 判据 | 例 | 后果 |
|---|---|---|---|
| **Tier-0** | 递归无任何 VarRef(纯标量/定长数组) | `StdColor`, `StdVector3d`, `StdQuaternion` | 作为数组元素时是**纯 C 数组**, 可整体 memcpy; 读端得到 `span<const TRoot>` |
| **Tier-1** | 有 VarRef, 但可达负载只有裸字节或 Tier-0 记录 | `StdHeader`, `StdImage`, `StdPointCloud`, `StdMarker` | 完全平坦 |
| **Tier-2** | 含元素自身带 VarRef 的嵌套数组 | `test_nested/robot_state`(`pose[]`, `pose` 含 `string`) | 布局同样支持: 先占 `cnt × sizeof(PoseRoot)` 连续块, 再追加各元素负载并回填其 VarRef |

三级**都由同一套布局与同一份 View 代码覆盖**, 分级只用于说明收益大小。§1.3 的灾难性开销正是 Tier-0 元素数组造成的, 因此收益最大的恰好是最简单的一级。

### 3.3 worked example — `StdImage`

```
StdHeader header      # string frame_id; float64 stamp
uint32 height
uint32 width
string encoding
uint32 step
uint8[] data
```

```cpp
struct alignas(8) StdHeaderRoot {   //  0..7   VarRef frame_id
    dzflat::VarRef frame_id;        //  8..15  double  stamp
    double         stamp;           //  sizeof = 16
};
struct alignas(8) StdImageRoot {
    StdHeaderRoot  header;          //  0..15
    uint32_t       height;          // 16..19
    uint32_t       width;           // 20..23
    dzflat::VarRef encoding;        // 24..31
    uint32_t       step;            // 32..35
    dzflat::VarRef data;            // 36..43
};                                  //  sizeof = 48 (尾部 4 字节对齐填充)
```

`view.data()` = `seg + root->data.off`, 一次加法。`view.height()` = 一次定长读。**没有任何解析步骤。**

### 3.4 worked example — 10 万点 `StdPointCloud`

`points` 是 `StdVector3d[]`(Tier-0, `sizeof(StdVector3dRoot) == 24`), 变长区里就是 `100000 × 24 = 2,400,000` 字节的连续 `double[3]` 数组。

| | 现状 TLV | DZFlat |
|---|---|---|
| wire | 5.3 MB (2.21×) | 2.4 MB (1.00×) |
| 发端堆分配 | 100,000 次 | 0 次 |
| 发端 memcpy 次数 | ~200,000+ | 1 次(整块) |
| 收端 | 逐元素 `deserialize` | `span<const StdVector3dRoot>`, 0 拷贝 |

### 3.5 schema_hash

规范化 schema 串 → FNV-1a 32:

```
canon(msg)  = ";".join(f"{name}:{tok(type)}" for each field in declared order)
tok(scalar) = IDL 名           # "uint32", "float64", "bool", ...
tok(str)    = "string"
tok(T[N])   = f"{tok(T)}[{N}]"
tok(T[])    = f"{tok(T)}[]"
tok(Msg)    = "{" + canon(Msg) + "}"
tok(Msg[])  = "{" + canon(Msg) + "}[]"
```

由 generator 计算, 以 `static constexpr uint32_t kSchemaHash` 常量烧进 C++, 运行期零成本。收端在 View 构造时比一次。

**当前 `dz_ipc_msg_id` 是用户手填的整数**([dzipc.h:66](../include/dzIPC/dzipc.h#L66), [srv_data.h:17](../include/dzIPC/common/srv_data.h#L17))**, 没有任何 schema 校验** —— 这个哈希是必须新补的骨干, 不是可选项。TLV 靠字段名自描述所以能容忍字段增删; DZFlat 是定长布局, 没有哈希就会静默错解。

### 3.6 对齐契约与自证

- 全部 Root `alignas(8)`; generator 按声明顺序排布并计算显式填充。
- generator 为每个字段发射 `static_assert(offsetof(...) == N)`、每个记录发射 `static_assert(sizeof(...) == M)`。**任何布局漂移都是编译错误, 不是运行期错数据。**
- 变长区每块起点对齐到 8(浪费 ≤7 字节/块, 换取 `double` 原地读的无条件安全 —— x86 容忍非对齐, ARM 不一定)。
- `chunk->data()` 的 8 对齐由 `chunk_info_t` 的 `alignof` 决定, 当前是 40 字节恰好满足, 但**没有任何东西保证**; §5.3 补 `static_assert`。

### 3.7 双 wire 判别

TLV 段首 4 字节是首字段名长度(`int32`, 实测取值 1~64), DZFlat 段首是 magic `0x4C465A44`。**结构上不可能碰撞**, 故:

```
if (size >= 32 && *(u32*)p == kMagic)  → DZFlat 路径 (再校验 schema_hash)
else                                    → 既有 TLV 路径
```

混版本组网时旧订阅者会走 TLV 分支并因解析失败而**丢弃**, 而不是静默错解 —— 值是安全的, 但**不会告警**: 订阅循环只是 `continue`。这个静默是 §3.8 的接收计数要解决的问题。这条判别式让 DZFlat 可以灰度上线。

### 3.8 段如何升级

**DZFlat 没有字段级演进机制, 而且不打算有。** 这是定长布局的必然: wire 里没有字段名、没有 per-field tag, 读端唯一的定位手段就是编译期算出的偏移。偏移一错, 读出来的不是错误而是**能通过所有校验的垃圾值**。所以宁可拒收。TLV 能容忍增删字段, 代价正是那 40× 的编码开销(§1)。

段头里有两个版本字段, 各管一件事:

| 字段 | 粒度 | 谁在检查 | 不匹配的后果 |
|---|---|---|---|
| `layout_ver` | 全局 wire 格式 | [`dzflat.h` `looks_like_dzflat`](../include/ipc_msg/ipc_msg_base/dzflat.h) | 判别不通过 → 计入 `dzflat_header_bad` → 丢弃 |
| `schema_hash` | 每类型结构 | `bind_segment` | Reader 无效 → `dzflat_read` false → 计入 `dzflat_schema_drop` → 丢弃 |

`flags` 与 `reserved` 目前恒为 0、无人读取 —— 它们是留给"老读端可以安全忽略"那类可选特性的位置。

#### 什么会翻 `schema_hash`

指纹的原料是 `canon`(§3.5) 加各嵌套类型的 hash。以下**任何一项**都会翻:

| 改动 | 翻指纹 | 为什么 |
|---|---|---|
| 加 / 删字段 | ✅ | canon 串变了 |
| **改字段名** | ✅ | name 进了 canon —— 即便布局一字节没动 |
| 改声明顺序 | ✅ | canon 按声明顺序拼 |
| 改字段类型 | ✅ | token 变了 |
| `string[3]` → `string[]` | ✅ | token 带长度, 故意的: owning 容器是 `std::array` vs `std::vector` |
| 嵌套类型改了 | ✅ | 逐层向上传播 |

#### 升级流程: 借道 TLV

利用两个已有性质 —— 发布开关只管发布侧, 而**订阅侧永远同时认两种 wire**(§3.7):

1. 发布方 `EnableDzFlat(false)` → 全链路退回 TLV;
2. 滚 schema。TLV 自描述, 新老混跑期间多出的字段被忽略、缺的字段留默认值, **安全**;
3. 两端都到新 schema 后, 发布方 `EnableDzFlat(true)`。

不需要停机, 也不需要原子部署; 代价只是切换窗口内失去 DZFlat 的收益。

**反过来不行**(先开 DZFlat 再滚 schema): 新发布方发出新指纹, 老订阅方 `bind_segment` 拒收。

Python 侧要一起滚: `SCHEMA_BY_HASH` 也是按指纹查表, `_dzflat_schema.py` 没重新生成就查不到, `decode()` 返回 `None` —— 消息在 Python 里等于**消失**。

#### 接收侧计数: 让违反顺序可见

上面的顺序靠人遵守, 而违反它的后果是**静默丢消息**。所以拒收必须可数 —— 见 [`nodelet_config.h`](../include/dzIPC/common/nodelet_config.h) 的 `DzFlatRxCounters()`:

| 量 | 含义 | 判读 |
|---|---|---|
| `dzflat_accepted` / `tlv_accepted` | 收下的条数 | 分母。拒收数没有分母无法解读 |
| `dzflat_id_skipped` / `tlv_id_skipped` | `msg_id` 不是本话题 | **正常**。同一通道多种消息混跑时本就该跳过, 且是这里唯一的高频量 |
| `dzflat_header_bad` | magic 对但段头自相矛盾, 或 `layout_ver` 不认识 | 段被截断 / 覆写, 或对端换了 wire 格式 |
| `dzflat_schema_drop` | `msg_id` 对上但结构不符 | **一定是版本错配** —— 违反了上面的顺序 |
| `tlv_corrupt_drop` | TLV 反序列化读越界 | 缓冲损坏(known_issues 第 3 条那道拦截的命中数) |

`defects()` 是后三项之和, 非 0 就该去查。

把"正常过滤"单独拿出来不是为了整齐: 它是唯一的高频量, 不分开就会把另外三个真信号淹到看不见。**同理, `dzflat_header_bad` 也必须与 `tlv_id_skipped` 分开** —— 一个损坏的 DZFlat 段若任其落进 TLV 分支, 尾部校验必然失配, 于是看起来和"这条不是我的话题"一模一样。

计数埋在 [`wire_accept.cc`](../src/dzIPC/common/wire_accept.cc) 的 `AcceptWire()` 里, pub/sub 与 ser/cli 共用同一份实现 —— 两份实现意味着两份埋点, 迟早漂移成"一条路径的丢弃看得见, 另一条看不见", 而看不见的那条恰好就是这套计数要解决的问题。

**Python 进程的那一半在别处。** Python 侧的载体 `GenericMessage` 是无 schema 的**直通体**, 对任何格式合法的段都收下 —— 所以 C++ 的 `dzflat_schema_drop` 在 Python 进程里恒为 0。版本错配要到按指纹查表时才暴露, 计数在 `dzipc.dzflat.rx_stats()`:

| 量 | 含义 |
|---|---|
| `accepted` | 解出来了 |
| `not_dzflat` | 段首 magic 不符 —— **正常**, TLV 消息就是这样 |
| `header_bad` | magic 符但段头自相矛盾 —— 段被截断 / 损坏 |
| `schema_unknown` | 指纹不在注册表里 ⇒ **msg 定义变了但没重跑 generator** |
| `schema_mismatch` | 指纹对上但 `root_size` 与本地 schema 不符 ⇒ 两侧 schema 不是同一份 |

两侧合起来看才是完整的接收侧图景。验收见 `test/test_wire_accept.cpp`(9 例, 每例断言**恰好那一项 +1 且其余不动**)与 `test/test_dzflat_python.py` 第 [6][7] 组。

---

## 4. A 级 / B 级

### 4.1 A 级 — 平坦布局 + 只读 View

发端: `serialize` 换成"按布局往 chunk 里写一遍"(用户容器 → chunk, **1 次拷贝**, 0 次堆分配)。
收端: `recv` 返回的 `buff_t` 已指进 chunk, 把它移进 `Sample<T>` 并用 View 访问 → **0 拷贝, 反序列化消失**。

用户代码零改动即可受益(旧写法照用, 只是 wire 变了); 想要零拷贝读则改用 `Sample<XxxView>`。

### 4.2 B 级 — loan-and-build

发端先 `loan` 拿到 chunk, builder 直接往共享内存里写 → **两端 0 拷贝**, 严格零拷贝。已实现, 落地形态(见 §9.5):

```cpp
auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(h * step + 256);  // 参数 = 变长区预算上界
if (!lo.valid()) { /* 无接收方 / 池耗尽 / 开关未开 → 回退 A 级 publish */ }
lo->set_width(w);
lo->set_height(h);
lo->header().set_frame_id("camera");      // 嵌套子 builder, 共用同一个 Writer
auto px = lo->alloc_data(h * step);       // span<uint8_t>, **直接指向 chunk**
camera.read_into(px.data(), px.size());   // 相机可直接 DMA 进共享内存
pub.publish_loaned(std::move(lo));
```

代价: 变长字段必须先声明尺寸上界。图像/点云天然满足(`h*step` 已知), 不是所有消息都舒服。

### 4.3 读端形状

```cpp
Sample<StdImageView> s;                   // 持有 buff_t, 析构才归还 chunk
if (sub->take(s)) {
    auto px  = s->data();                 // span<const uint8_t>,  0 拷贝
    auto pts = other->points();           // span<const StdVector3dRoot>, 0 拷贝
    auto enc = s->encoding();             // string_view, 0 拷贝
}
StdImage owned; s->copy_to(owned);        // 兼容桥, 老代码一行接上
```

---

## 5. 需要新增的骨干

### 5.1 前置(硬阻塞): 覆写路径无条件归还 chunk

`force_push` 覆写槽位时调 `clear_message` → **`release_storage`(无条件 `pool_.release`)**([ipc.cpp:449-466](../src/libipc/ipc.cpp#L449-L466)), 而非尊重 `conns` 位图的 `recycle_storage`([:420-447](../src/libipc/ipc.cpp#L420-L447))。后果:

> 订阅方持有某 chunk 的 `buff_t` → 发布方绕圈覆写该槽位 → chunk id 立刻回池 → 被下一帧重新 acquire → **持有者的指针指向正在被写入的新帧**; 其析构再 `recycle_storage` 一次 → 同一 id 二次入池 → 两条消息拿到同一块。

今天订阅循环拿到 `buff_t` 后立刻拷出就丢, 窗口是微秒级, 属潜在竞态。**任何延长 chunk 持有期的方案(A 级、B 级、loaned_blob 全都算)都会把它拉成秒级、必现。** `prod_cons.h:278-283` 的注释另已自承覆写与 `pop()` 之间的撕裂尚未消除。

**修法(不需要给 chunk 头加任何新字段)。** 关键在于用已有状态区分"还没读到"和"读了还握着":

`pop()` 的顺序是**先把槽位数据拷出、再清掉自己的 rc 位**([prod_cons.h:317-329](../src/libipc/prod_cons.h#L317-L329))。因此在 `force_push` 覆写的那一刻:

- `rem_cc = cur_rc & ep_mask` = **尚未 pop 该槽位**的接收方 → 它们永远看不到这条消息, 其位必须从 `chunk->conns()` 清掉, 否则位图永不归零、chunk 永久泄漏(这正是旧代码无条件 release 的原因);
- `cc & ~rem_cc` = **已经 pop 过**的接收方 → 其中尚未释放 `buff_t` 的那些, 在 `chunk->conns()` 里的位仍然置着。

于是覆写时的正确动作是: 清掉 `rem_cc` 的位, **结果为 0 才归还 id**; 非 0 说明仍有持有者, 交由最后一个持有者的 `buff_t` 析构(`recycle_storage`)归还。既不泄漏, 也不提前归还。

实现: `prod_cons.h` 的两个 broadcast `force_push` 把 `rem_cc` 传给覆写回调, `clear_message` 据此调用新增的 `discard_storage` 而非 `release_storage`。unicast 策略的 `force_push` 从不调用该回调, 以 `if constexpr` 保持原路径不变。

**残余窗口** —— Step 0 时只收窄未关闭, **已于 2026-09-03 关闭**(见 [dzflat_known_issues.md](dzflat_known_issues.md) §8.3): 只有 `force_push` 能覆写读方还持位的格子, 而它必然先自增 `epoch_`, 所以 `pop()` 在拷贝前后各读一次 epoch 就能判定"本格在我拷贝途中被覆写" → 丢弃。同一批修复还补上了 `push` 覆写被套圈格子时的 chunk 归还(§8.4)与 `deserialize` 的逐段边界检查(§8.5)。

### 5.2 libipc 公共原语(SHM) — 已实现, 见 §9.4

| 原语 | 说明 |
|---|---|
| `loan(size) → loan_t{id, data, size}` | 包装 `acquire_storage`, 按**容量档位**(`loan_size_class`)而非精确 size; 无接收方或池耗尽时返回无效, 调用方须回退 |
| `publish_loan(lo, tm)` | 包装 `push`/`force_push`, **单条消息**, 不拆包; 失败时自行归还 chunk |
| `discard_loan(lo)` | 放弃未投递的 chunk, 立刻归还 |
| — | 收端不需要新原语: `recv` 的 `buff_t` 已经零拷贝 |

容量档位是必须的: `calc_chunk_size` 按 1024 对齐且段名含 chunk_size([ipc.cpp:206-213](../src/libipc/ipc.cpp#L206-L213)), 变长 blob 会每 1KB 开一个新共享段。

### 5.3 容量与对齐

- `static_assert(alignof(chunk_info_t) % 8 == 0)` 固定 §3.6 的对齐前提。
- **32 槽天花板**: `id_pool::max_count = large_msg_cache = 32`([def.h:39](../include/libipc/def.h#L39), [id_pool.h:40-48](../src/libipc/utility/id_pool.h#L40-L48)), 每个 size class 全进程共享。
  持有预算 ≈ `32 / (帧率 × 订阅者数)`; 30fps × 2 订阅者 ≈ 每 sample 平均 < 0.53 s。
  超限会 `acquire_storage` 失败 → 退化成 64 字节分片([ipc.cpp:770-798](../src/libipc/ipc.cpp#L770-L798)) → 4MB 消息变 65536 次 push 打进 256 槽的环([circ/elem_array.h:31](../src/libipc/circ/elem_array.h#L31))。**这是性能悬崖, 不是优雅降级**, 必须在文档里作为使用约束写明。

### 5.4 generator

每个消息类型额外发射(现有 owning struct 与 `serialize/deserialize` **原样保留**):

| 产物 | 用途 |
|---|---|
| `XxxRoot` | POD 记录, 布局即 wire; 带 `static_assert` 自证 |
| `XxxView` | 只读访问器 + `copy_to(Xxx&)` 兼容桥 |
| `kSchemaHash` | §3.5 常量 |
| `dzflat_size(const Xxx&)` / `dzflat_write(...)` | A 级写入 |
| `XxxBuilder` | B 级写入(Step 2) |

### 5.5 dzIPC

- publish 按传输分支: SHM → DZFlat, socket/UDP → 既有 TLV;
- 订阅侧把 `buff_t` 移进 `Sample<T>`(落点: [shm_pub_sub_ipc.cc:543-568](../src/dzIPC/shm_pub_sub_ipc.cc#L543-L568) 那个每轮销毁 `raw_data` 的循环);
- 顺带去掉每帧的 `topic_msg_->clone()`(View 不需要克隆模板)。

### 5.6 Python — 已实现, 见 §9.6

不能在 C++ 侧给 `GenericMessage` 加解码: 它没有 schema, 而 schema 只在 generator 产出的物件里, **且没有任何 TU 会编译那些生成头文件**。所以落地形态是: C++ 侧段**原样直通**, generator 另发射一份 Python schema, 由 `python/dzipc/dzflat.py` 解码。大数组用 `memoryview.cast()` 零拷贝直读。

---

## 6. 方案对比(4MB 图 / 10 万点云)

| | 发端 blob 拷贝 | 收端拷贝 | 堆分配/帧 | wire 膨胀 | Python | 新骨干 | 风险 |
|---|---|---|---|---|---|---|---|
| 现状 | 2 | 1 | 2 + N(嵌套元素数) | 1.008× / 2.21× | 完整 | — | — |
| `loaned_blob` | 1 | 0(仅该字段) | 1 + N | 同上 | **丢字段** | 中 | 非原子投递 + ABA |
| **DZFlat A** | 1 | **0** | **0** | **1.00×** | **变好** | 中 | 双 wire 维护 |
| **DZFlat B** | **0** | **0** | **0** | 1.00× | 变好 | 大 | 撕裂窗口拉长 |
| iceoryx 式定容 POD | 0 | 0 | 0 | 固定容量常驻 | 需另写 | 大 | IDL 须声明上界; 4MB × 32 槽 = 128MB/topic |

---

## 7. 代价与卡点

1. **双 wire 双维护是主要账单。** SHM 走 DZFlat、socket/UDP 走 TLV, generator 出两套代码, 测试矩阵翻倍。这是长期维护税而非技术风险, 决策时按此计价。
2. **大 blob 的收益要下游改用 view 才拿得到。** 实测(§9.2)`copy_to` 相对 `deserialize` 只快 2 倍, 端到端 1.24× —— 5.9 MB 的遍历本身就是内存带宽瓶颈。只换 wire 不改读法, 大 blob 场景的收益就只剩编码那 7.6×。**嵌套数组场景不受此限**(编码 40×、分配归零是无条件的)。
3. **B 级改变书写方式**: 变长字段要先声明尺寸上界, 且构造过程 move-only。已落地(§9.5), 但它是**并存的第二套写法**而非替换 —— A 级 `publish(shared_ptr)` 完全不变。
4. **schema_hash 不匹配的处置**要定策略(拒收+告警 / 降级 TLV)。加上校验后会暴露一批本来就在错配的用法。
5. **32 槽 + 持有预算**(§5.3), 越界是悬崖。
6. **C++17**([CMakeLists.txt:52](../CMakeLists.txt#L52)) 无 `std::span`, View 自带轻量 `span`。
7. **撕裂**: §5.1 前置已修(Step 0), 其残余窗口亦已于 2026-09-03 关闭(known_issues §8.3)。
8. **Tier-2 的 builder 复杂度集中在回填 VarRef**, 是唯一需要两段式写入的地方。
9. **不受信输入的边界校验必须覆盖"计数"而不只是"解引用"** —— 实测抓到过 `copy_to` 拿未校验的 `cnt` 去 `resize` 的无界分配(§9.3)。任何新增的访问器都要守这条。

---

## 8. 不做什么

- 不动 socket/UDP 的 wire 与 `data_rev.cc` 的分片/NACK 逻辑(§1.4)。
- 不动 chunk 头布局(不加 `refs`, 复用现有 `conns` 位图及其自愈性)。
- 不动现有 owning struct 的公开成员与 `serialize()/deserialize()` 签名 —— 全部用户代码与既有测试不受影响(实测 31 个测试二进制无回归)。
- 不引入 FlatBuffers/Cap'n Proto 依赖: 布局规范只有 §3 一页, 由 generator 直出, 无第三方 schema 编译器。
- ~~服务(srv)暂不覆盖~~ —— **已接入**(2026-09-04, 见 [dzflat_known_issues.md](dzflat_known_issues.md) §9.3): 生成器发射 DZFlat 段, `shm_ser_cli_ipc` 按 pub/sub 的双 wire 分派镜像了四个点。注意它用的是 `ipc::server`(single-single-unicast)而非 `route`。

---

## 9. 推进顺序与验收门

| Step | 内容 | 验收门 | 状态 |
|---|---|---|---|
| **0** | §5.1 覆写路径修正 | 慢读者持有 `buff_t` 期间发布方覆写该槽位, chunk 不得提前归还; 覆写"无人 pop 过"的大消息其 chunk 必须回收; 既有测试全绿 | ✅ 见 §9.1 |
| **1** | `dzflat.h` 运行时 + generator 发射 Root/View/size/write + 离线基准 | `std_image` 与 `std_point_cloud` 的 wire 大小 / 堆分配次数 / 耗时三项对比; 点云分配次数须为 0 | ✅ 见 §9.2 |
| **2** | libipc `loan/publish_loan` + dzIPC 双 wire 分支 | 跨**进程** DZFlat 往返值一致; 同一订阅者交替收下两种 wire; 四种回退路径均不丢消息 | ✅ 见 §9.4 |
| **3** | B 级 builder(`alloc_data` 就地直写) | `alloc_*` 返回的 span 必须落在 chunk 内(零拷贝的实质); 未投递的借样析构即归还; 超预算不得写出坏段 | ✅ 见 §9.5 |
| **4** | Python DZFlat 读路径 | `topic_echo` 对 DZFlat 消息字段完整; TLV 路径不受影响; 段不可信 | ✅ 见 §9.6 |

Step 1 先离线做(不碰传输), 是为了在承诺传输改造之前先把 §1.3 那个数量级坐实。

### 9.1 Step 0 落地

改动: `prod_cons.h` 两个 broadcast `force_push` 把 `rem_cc` 传给覆写回调; `queue.h::force_push` 的回调加一个带默认实参的形参(兼容 `<multi,multi,broadcast>` 退化调用 `push()` 的那条路径); `ipc.cpp` 新增 `discard_storage`, `clear_message` 按 policy 以 `if constexpr` 分派; 另补 `static_assert(sizeof(chunk_info_t) % 8 == 0)` 钉住 §3.6 的对齐前提。

回归: `test/test_chunk_hold.cpp` 两个用例。

**一条实现层的关键发现**: 这个缺陷**单接收方构造不出来**。覆写回调只在 `push()` 失败退化到 `force_push()` 时才被调用, 而 `push()` 失败的条件是"该槽位仍有接收方没 pop 过"——没 pop 过的接收方不可能持有这条消息的 chunk。所以单接收方时它 pop 过的槽位会被普通 `push()` 静默覆写(无回调, 什么都不释放), 缺陷不显形。**必须两个进度不同的接收方**: 快读方持有 chunk, 慢读方卡住同一槽位。这也说明该缺陷在多订阅者部署下是常态而非边角。

用例已双向验证: 退回旧行为(无条件 `release_storage`)时 `HeldChunkSurvivesOverwriteWithLaggingPeer` 必然失败(持有的段首字节从 `0xA5` 变成后来消息的 `0x5B`), 修复后连续三次运行全绿; 31 个测试二进制无回归。

**顺带发现的另一个既存缺陷(未修, 不在 Step 0 射程)**: 被套圈(lapped)的接收方读空时会把同一个环槽位读到两次 —— 游标落后写指针超过 256 时, `cur` 与 `cur+256` 映射到同一 `index_of`。若该槽位承载大消息, 同一个 `storage_id` 会产生两个 `buff_t`, `recycle_storage` 因此走两次: 第二次在 `conns` 已为 0 的情况下再次 `id_pool::release`, 空闲链表出现重复项(头插会把链表接成自环并丢掉其后的全部 id)。与 §5.1 的残余窗口同族, 需在 `pop()` 侧加覆写检测。已登记在 `test_chunk_hold.cpp` 文件头。

### 9.2 Step 1 实测

`test/dzflat_benchmark.cpp`(每项 5 次取平均, -O2, 32 核机)。口径: TLV 的 `serialize()` 自带一次整包 `new`; DZFlat 写进调用方给的缓冲(生产里就是 chunk), 故不计入分配。

**StdPointCloud 100k 点** (`StdVector3d[]`, Tier-0 元素)

| | TLV | DZFlat | |
|---|---|---|---|
| 编码耗时 | 7130 µs | **176 µs** | **40×** |
| 编码堆分配 | **100,004 次** | **0 次** | — |
| wire | 6,150,309 B (1.922×) | **3,200,136 B (1.000×)** | 1.92× |
| 解码(拷回 owning struct) | 2769 µs / 3355 次分配 | 185 µs / 0 次 | 15× |
| 解码(零拷贝 bind) | — | **O(1)** | — |
| 消费者端到端(解码+读全量) | 2839 µs | **38 µs** | **75×** |

§1.3 预测的"10 万次堆分配"完全坐实(实测 100,004 = 100k 个嵌套 `serialize()` + 4 次容器分配), 归零。

**StdImage 1920×1080 rgb8** (5.93 MB blob)

| | TLV | DZFlat | |
|---|---|---|---|
| 编码耗时 | 1047 µs | **137 µs** | **7.6×** |
| 编码堆分配 | 2 次 | **0 次** | — |
| wire | 6,272,090 B (1.0082×) | **6,220,912 B (1.0000×)** | — |
| 解码(拷回 owning struct) | 285 µs | 139 µs | 2.0× |
| 消费者端到端(解码+读全量) | 1399 µs | **1129 µs** | 1.24× |

**三条必须诚实说明的读数**:

1. **大 blob 的收益集中在编码侧, 不在"更快的拷贝"。** `copy_to` 只比 `deserialize` 快 2 倍, 端到端只有 1.24× —— 因为 5.9 MB 的遍历本身就是内存带宽瓶颈(1120 µs), 把它算进来就摊薄了一切。真正的收益是**消费者可以不拷**: 用 view 时那 285 µs 的 `deserialize` 整个消失。所以 A 级对大 blob 的价值取决于**下游是否改用 view**; 只换 wire 不改读法, 收益就只剩编码那 7.6×。
2. **点云端到端的 75× 里有一部分不是布局带来的。** `sizeof(StdVector3d)` 是 **64 字节**, 而 `sizeof(StdVector3dRoot)` 是 **24 字节** —— 因为每个生成消息都继承 `IpcMsgBase`(40 字节: vptr + `enable_shared_from_this` + 三个尺寸字段)。于是用户自己的 `std::vector<StdVector3d>` 承载 24 字节负载要占 64 字节, 缓存密度差 2.67×, 遍历也就慢 2 倍(70 µs vs 38 µs)。这是 owning struct 的固有开销, 与 wire 无关, 但它恰好也是 DZFlat 视图更快的一半原因。
3. **TLV 的 `deserialize` 在稳态下不分配**(图像那一栏 0 次): `tlv_back` 被复用, `resize` 命中已有容量。首次调用是要分配的。点云那 3355 次来自逐元素 `ipc::buffer` 的 pimpl 走 `ipc::mem::alloc` 池化槽。

### 9.3 Step 1 的安全性验收

`test/test_dzflat.cpp` 13 个用例, 分三类: 往返一致(覆盖全部七种字段形态, 含 Tier-2 的 `pose[]`)、schema 指纹与双 wire 判别、**不信任段内容**。第三类另用 ASAN+UBSAN 在精确大小的堆缓冲上重放篡改路径(偏移推到段外 / 紧贴段尾 / 长度溢出 / 接近 `u32` 上限 / `cnt` 巨大), 无一例越界读。

**该组测试抓到一个真实缺陷**: `copy_to` 对 `Msg[]` 和 `string[]` 原本用段里的 `cnt` 直接 `resize` —— 把 `cnt` 篡改成 100 万就真的分配 100 万个元素(每个 ≥64 字节), 是不受信输入驱动的无界分配。修法: `_count()` 改为经 `Reader::as_span` 校验后再返回(越界则为 0), 于是 `copy_to` 自动收敛。**教训: 边界校验必须落在"计数"上, 只校验"解引用"是不够的** —— 计数会流入分配。

### 9.4 Step 2 落地: 收益进入真实传输路径

**改动面**

| 层 | 内容 |
|---|---|
| `libipc/def.h` | `storage_id_t` 上提到公共头(loan API 要指名它) |
| `libipc/ipc.h` + `ipc.cpp` | `loan_t` + `loan()` / `publish_loan()` / `discard_loan()`; `loan_size_class()` 档位取整 |
| `ipc_msg_base.hpp` | 四个 DZFlat 虚接口(默认实现 = 不支持) + `dzflat_peek_msg_id` / `check_dzflat_id` |
| generator | 发射四个虚函数的类外定义; `emit_dzflat` 开关 |
| `nodelet_config` | `EnableDzFlat` / `IsDzFlatEnabled` + 发布计数器 |
| `shm_pub_sub_ipc` | 发布侧 `try_publish_dzflat`; 订阅侧按段首 magic 双 wire 分派 |

**开关默认 OFF**, 理由写在 `nodelet_config.h`: DZFlat 段对未升级的订阅方不可解析(它会按 TLV 读段尾的 msg_id, 几乎必然失配而丢弃 —— 不会错解, 但会静默丢消息)。订阅侧则**无条件**同时认两种 wire, 于是"先升级订阅方、再升级发布方"是安全的灰度顺序。

**为什么容量按档位取整**: `calc_chunk_size` 只按 1024 取整且共享段名含 chunk_size, 变长负载每 1KB 就开一个新段。借样把 chunk 持有期拉长后同时活着的段更多, 所以 `loan_size_class` 对 ≤64KB 用 1KB 台阶(与既有 send 同粒度, 不引入新段)、更大用 2 的幂(段数对数增长)。代价是大消息最坏浪费近一半容量 —— 但 chunk 是 tmpfs 稀疏映射, 只有真正写到的页才占物理内存, 而只写 `total_size` 那一段。**可见后果**: 接收方拿到的 `buff_t` 大小是容量而非负载长度, 真实长度由段头的 `total_size` 承载。

**传输层实测**(`test/dzflat_tx_benchmark.cpp`, 真实 SHM 链路, 发一条取一条, 200 轮):

| 消息 | TLV publish | DZFlat publish | |
|---|---|---|---|
| `StdImage` 640×480 rgb8 (0.88 MB) | 62.5 ~ 85.6 µs/条 | **21.6 ~ 24.3 µs/条** | **~3.2×** |
| `StdPointCloud` 2 万点 | 1607 ~ 1667 µs/条 | **27.1 ~ 28.9 µs/条** | **~59×** |

计数器证明每一轮确实走了预期路径(dzflat=205/fallback=0 与 dzflat=0/fallback=205)。
**口径限制**: 表里只有发布侧。解码发生在订阅者的后台线程(`subscribe_thread_`), `try_get` 只是队列 pop, 所以这个基准测不出解码差异 —— 那一半在 §9.2 的离线数里。

**回归**: `test/test_loan.cpp` 10 例(借样原语契约)+ `test/test_dzflat_transport.cpp` 9 例(传输层)。31 个测试二进制中 30 个稳定全绿。

> `test_dzipc_log` 的 `DzipcLogRotation.QueueDepthTriggersRotation` 是**既存的不稳定用例**(时间敏感), 与本方案无关: 在未含本次任何改动的 HEAD 上 12 次跑失败 6 次, 改动后 12 次失败 7 次 —— 同一水平。不要把它当成回归。

### 9.5 Step 3 落地: B 级就地构造

**改动面**

| 层 | 内容 |
|---|---|
| `dzflat.h` | 就地构造原语: `alloc_array` / `put_string` / `alloc_string_table` / `put_string_at` / `elem_ptr` / `varlen_start` |
| generator | 发射 `XxxBuilder`; `XxxFlat` 补 `builder_t` / `loan_size(budget)` / `finalize()` |
| `dzIPC/common/loaned_message.h`(新) | `LoanedMessage<Flat>` —— move-only 的 RAII 持有者 |
| `shm_pub_ipc` | `loan<Flat>(varlen_budget)` / `publish_loaned(&&)` |

**Builder 的字段映射**(与 §3.1 的七种形态一一对应):

| 形态 | Builder 接口 | 是否零拷贝 |
|---|---|---|
| 标量 | `set_<f>(v)` | 是(Root 就在 chunk 里) |
| 定长数组 | `<f>()` → 可写 span | 是 |
| `string` | `set_<f>(string_view)` | 否, 一次 memcpy(字段短, 源在调用方堆上) |
| `T[]` 标量 | `alloc_<f>(n)` → **可写 span** | **是 —— B 级的落点** |
| `string[]` | `alloc_<f>(n)` + `set_<f>_at(i, sv)` | 否(同 string) |
| 嵌套消息 | `<f>()` → 子 builder | 是 |
| 嵌套数组 | `alloc_<f>(n)` → 可写元素 span; 非 Tier-0 再用 `<f>_at(i)` | 是 |

**实测**(`test/dzflat_tx_benchmark.cpp`, 0.88 MB 图像, 150 轮, 两种写法都要自己填满像素):

| | 构造 + 发布 |
|---|---|
| A 级(负载先填到 `std::vector`, 再 `publish`) | 151.6 / 165.5 µs/条 |
| **B 级(直接填进 chunk)** | **113.8 / 116.1 µs/条** |

节省的 38~49 µs 与 §9.4 里 A 级 `publish` 本身的 22~36 µs 量级吻合 —— **B 级把"把负载搬进 chunk"这件事整个消掉了**, 剩下的 114 µs 全是调用方遍历 0.88 MB 填像素的成本。真实相机做 DMA 时这部分不由 CPU 付, 所以 B 级对硬件直采链路的意义比这张表显示的更大。

**三条契约**(写在 `loaned_message.h` 顶部, 逐条有用例):

1. **容量由调用方给上界。** 变长负载是就地写的, 借样时还不知道最终长度, 所以 `loan(varlen_budget)` 的参数是"变长区最多要多少字节"。超预算时 `alloc_*` 返回空 span、`Writer` 转 `!ok`、`publish_loaned` 失败并归还 chunk —— **不会写出坏段**。
2. **生命周期 RAII。** 借到的 chunk 要么被 `publish_loaned` 交给队列, 要么在 `LoanedMessage` 析构时归还。move-only, 禁拷贝。用例 `UnpublishedLoanIsReturnedOnDestruction` 连借 160 次(32 块 × 5 轮)不发布, 只有每次析构都归还才能全过。
3. **借样会失败, 且失败是常态。** 无接收方 / 池耗尽 / 开关未开都得到无效对象, 调用方必须能回退 A 级 `publish`。

**两处实现层要点**

- **`move` 之后必须重指 Builder 里的 `Writer*`。** `Writer` 是 `LoanedMessage` 的成员, 地址随对象走; 直接拷 Builder 会让它指向已移动对象的成员 —— 悬垂。而 `publish_loaned(std::move(lo))` 这个签名让 move 成为必经之路, 所以这是唯一需要手工维护的不变式, 用例 `MoveKeepsBuilderUsable` 钉住它。
- **`alloc_array` 必须挡 `n * sizeof(T)` 溢出。** `n` 常来自调用方的计算结果(`h * step`), 溢出后会算出一个很小的字节数并"成功"返回一个远小于 `n` 个元素的 span, 后续写入即越界。

**验收断言的选择**: B 级唯一无法用"值一致"替代的断言是 `alloc_*` 返回的地址**落在 chunk 区间内** —— 若实现偷偷给一个临时缓冲再拷进 chunk, 值照样一致而零拷贝是假的。故 `AllocReturnsSpanInsideTheLoanedChunk` 直接比对指针区间, 并额外要求负载起点在 `SegHeader + Root` 之后。

ASAN + UBSAN 另行覆盖了越界与溢出路径(超预算 `alloc`、`n*sizeof(T)` 溢出、`string[]` 表外下标、Tier-2 元素越界), 无一例报告。

**两处方法论要点**:

1. **静默回退会让测试假绿。** DZFlat 的回退是设计上静默的(类型不支持 / 无接收方 / 池耗尽都回落 TLV 并照常送达), 所以"用例通过"根本不能证明 DZFlat 被走到了 —— 把发布分支整个短路掉, 最初的 8 个用例仍然全过。因此加了进程级计数器 `DzFlatPublishCount/FallbackCount` 并让用例断言它, 短路后 3 个用例立刻转红。计数器同时是线上排查手段: `fallback/(dzflat+fallback)` 持续偏高说明 chunk 持有预算被突破(§5.3)或链路混着未升级的类型。
2. **同进程测试证明不了位置无关。** 段里全是相对偏移、没有指针, 这一条只有在**两个地址空间**里才检验得到 —— 若布局混进绝对地址, 同进程会照常通过。故 `CrossProcessRoundTripProvesPositionIndependence` 用 `fork()` 让子进程发布、父进程订阅, 并用子进程退出码回报"它确实走了 DZFlat"。

**发现一个既存缺陷(与 DZFlat 无关, 未修)**: 发布方连续快发大消息而订阅方不取时, chunk 池(32 块/档位)耗尽 → `send()` 退化成 64 字节分片 → 一条 0.88 MB 消息变 14000+ 个分片打进 256 槽的环 → `force_push` 覆写 → 接收侧 `recv` 的重组缓存(按 `msg.id_` 索引)拼出错乱的缓冲 → `check_id` 偶然通过 → TLV `deserialize` 按垃圾长度走偏, **段错误**。

已用 git worktree 在**未含本次任何改动的 HEAD** 上复现, 确认是既存问题; 且开启 DZFlat 后同一场景不再崩(146/200 条走了借样, 避开了分片路径)。这与 §5.3 记的"池耗尽是性能悬崖"是同一个根因的两个面 —— 悬崖底下还有一个内存安全问题。修它需要在 `pop()` 侧加覆写检测(与 §5.1 的残余窗口同族), 不在 Step 2 射程内。基准脚本因此必须发一条取一条, 已在文件头注明。

---

### 附: 与现有文档的关系

- [loaned_blob_borrow.md](loaned_blob_borrow.md) — **归档不实施**, 理由见 §2.3;
- [shm_nodelet.md](shm_nodelet.md) — 同进程快速路径(`clone` + `push(shared_ptr)`), 与本方案正交; 但注意 View/Sample 类型进入快速路径时的 `clone()` 语义须显式定义;
- [RTPS技术路线报告.md](RTPS技术路线报告.md) — UDP/RTPS 路线, 其分片格式即 §1.4 所述页尾, 本方案不触碰。

### 9.6 Step 4 落地: Python / topic_echo 的 DZFlat 读路径

**为什么不能照搬 C++ 的做法**

Python 侧的通用消费者是 `GenericMessage` —— 一个**自描述 TLV** 走查器, 靠 wire 里的字段名工作。DZFlat 是定长布局, wire 里没有字段名, 所以它解不了: 缺 schema。而 schema 只存在于 generator 产出的物件里, 关键在于 —— **没有任何 TU 会编译那些生成头文件**(`interface.cc` 只 include `generic_message.hpp`)。所以"C++ 侧注册一个 schema_hash → 解码函数的表"这条路走不通: 在 Python 进程里一条也注册不上。

结论: **schema 必须以数据形式送到 Python**。

**分工**

| 侧 | 做什么 |
|---|---|
| C++ `GenericMessage::dzflat_read` | 段**原样直通** —— 留存字节 + schema 指纹, 不解析。`has_dzflat()` / `dzflat_schema_hash_rx()` / `dzflat_memoryview()` 三个绑定暴露给 Python |
| generator | 除 C++ 物件外, 另发射 `python/dzipc/gen_msgs/_dzflat_schema.py`(字段名/形态/wire 类型/Root 偏移/嵌套引用), 由 `gen_msgs/__init__.py` import 时登记进注册表 |
| `python/dzipc/dzflat.py`(新, 手写) | 按 schema 解码段 → dict; 外加 `dump()` 通用渲染 |
| `dzipc_topic_echo.py` | 改用 `GenericMessage` 模板 + `dzflat.dump()`, 一份代码覆盖两种 wire |

**Python 侧反而更好**: 没有页尾交错要剥, 而变长标量数组用 `memoryview.cast()` **零拷贝**直读 —— TLV 路径的 `get_uint8_array()` 会把一张 1 MB 的图变成一个百万元素的 Python list。变长区每块按 8 对齐(`dzflat.h` 的 `kAlign`)恰好满足 `cast()` 的对齐要求。

**指纹必须逐位一致**: Python 用 `compute_schema_hash` 复刻了 `dzflat.h` 里那个 constexpr FNV-1a。不一致的后果不是变慢, 而是查表全落空 → DZFlat 消息在 Python 里等于消失。已对 `StdHeader`/`StdVector3d`/`StdImage`/`StdPointCloud` 四个类型与 C++ 常量逐位比对通过。

**布局模型的交叉校验(这一步的方法论收获)**: Python 要解码就必须知道 Root 的字段偏移, 于是 generator 里补了一套 Itanium ABI 布局推导(`compute_layout`)。同一组偏移随即以**字面量**形式写进 C++ 的 `static_assert` —— 原先那里是从 `offsetof` 符号推回来的表达式, 自证性弱。改成字面量后, 编译期同时校验两件事: ① 编译器没插入意外填充; ② **generator 的布局模型与真实 ABI 一致**, 也就是替 Python 解码器把偏移钉死了。

**顺带修掉一个潜伏缺陷 + 一条覆盖缺口**

把布局字面量铺开时才发现: `std_matrix_3d` 有 `StdVector3d[3]`(定长嵌套数组), 对应的 owning 成员是 `std::array<>`, 而生成的 `copy_to` 调了 `resize()` —— **根本编译不过**。前三个 Step 全绿是因为**没有任何 TU 编译它**: 生成头文件只有测试用到的那几个类型才被实例化。

- 修法: `copy_to` 对定长容器不 `resize`, 并把段里多出的元素截断到固定长度(`string[N]` 同理)。
- 顺带修: schema 指纹的 token 原先对定长/变长的嵌套与字符串数组都发 `{}[]` / `string[]`, 于是 `string[3]` 与 `string[]` 同指纹 —— 而两者的 owning 容器类型不同。现在定长会带上长度。
- 覆盖缺口: 新增 `test/test_generated_headers.cpp`(**由 generator 维护清单**), 把全部 21 个 msg + 6 个 srv 头文件都拉进来编译一遍。于是每个类型的 Root 布局 `static_assert`、每个字段形态的 View/Builder/`copy_to`、`kSchemaHash`/`kRootTight` 的编译期可求值性都被覆盖。**这类 bug 以后不会再潜伏。**

**验收**

`test/test_dzflat_python.py`(C++ `dzflat_py_publisher` 发布 + Python 订阅), 33 项断言全过:

- DZFlat 图像: 字段集完整(6 个)、嵌套 `header.frame_id`/`stamp` 正确、`data` 96 字节逐字节一致、且以 `memoryview` 而非 list 返回;
- DZFlat 点云: 嵌套数组 `points` 逐元素一致、`float64[] channels`、`string[] channel_names` 全对;
- 开关关闭时 TLV 路径不受影响(`field_count()==6`、`get_uint32`/`get_string` 照常);
- 段不可信: 空缓冲 / 全零 / 垃圾字节 / 长度不足 / 指纹不符 / 未注册指纹, 六种都返回 `None` 而不是错解。

`topic_echo` 实测对同一话题分别显示:

```
[wire=DZFlat schema=StdImage hash=0xB470A3E0]      [wire=TLV 字段数=6]
  header:                                            header: <嵌套, ftype=25>
    frame_id: 'camera_optical_frame'                 height: 4
    stamp: 1234.5                                    width: 8
  height: 4  width: 8  encoding: 'rgb8'              encoding: 'rgb8'
  data: <96 个元素> [0, 3, 6, …] …                    data: <96 个元素> [0, 3, 6, …] …
```

**测试有牙**: 把 `GenericMessage::dzflat_read` 退回旧行为(返回 false)后, 用例立刻报 `FAIL 收到了 DZFlat 消息(不是被静默丢弃)` —— 复现的正是这个缺口的真实形态: **不是退化成慢路径, 而是静默丢消息**(订阅循环在 `dzflat_read` 返回 false 时 `continue`)。

**两条遗留** —— 详见 [dzflat_known_issues.md](dzflat_known_issues.md) 第 1、2 条

1. **用错解释器 → 静默用到陈旧 pybind 模块**。构建目标是 CMake 找到的那个解释器(当前 `python3.10`), 而 `python/dzipc/` 下另有陈旧的 311/312 模块(其中 312 还是指向另一棵构建树 `build_py/` 的符号链接), 不含新绑定。误导性极强: `import dzipc` 成功、schema 注册表照常加载 21 条, 只有 `decode_generic` 静默返回 `None` —— 与"这是条 TLV 消息"同一个返回值。已在 `dzflat.py` 加 `_check_binding()` 把两种情况分开并发一次 `RuntimeWarning`; 根因(那几个 `.so` 靠手工维持)未除。
2. **`getattr` 取 C++ 侧对象的字段会静默拿到默认值**。`out.topic()` 返回的是 C++ 对象, 字段不是 Python 属性。`topic_echo` 原先正因此对**所有**类型都只打印 `N/A`(本步已修), 但这个模式没被消除。
