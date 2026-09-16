# DZFlat 遗留问题与已知陷阱

> **状态**: 现存问题登记表。DZFlat 本体的设计与落地见 [dzflat_shm.md](dzflat_shm.md)。
> **编写日期**: 2026-09-03
> **怎么用这份文档**: 每条都写明「症状 → 成因 → 现状 → 该怎么办」。**症状**一栏是给
> 排查者看的 —— 本表里的问题大多**不以报错的形式出现**, 而是表现成"消息没到"或"字段是
> 空的", 所以按症状反查比按模块反查快。

---

## 0. 一览

| # | 问题 | 严重度 | 状态 |
|---|---|---|---|
| 1 | Python 侧用错解释器 → 静默用到陈旧 pybind 模块 | 中(易误判为通道故障) | ✅ 已修(构建自动同步 + 清理 + 测试自动切换) |
| 2 | `getattr` 取 C++ 侧对象的字段 → 静默得到默认值 | 低(但是一类模式) | ✅ 已修(补齐封装类的 DZFlat 分支, 三处调用点改正) |
| 3 | chunk 池耗尽 → 64 字节分片重组错乱 → **段错误** | **高(内存安全)** | ✅ 已修(两道独立防线) |
| 4 | 被套圈接收方重复入池 → id_pool 空闲链表自环 | 中 | ✅ 已修(并顺带修掉一个更大的 chunk 泄漏) |
| 5 | 覆写路径的残余竞态窗口 | 低(已收窄) | ✅ 已修(epoch 序列锁) |
| 6 | srv(请求/应答类型)未接入 DZFlat | 低(射程边界) | ✅ 已接入(生成 + 传输 + 回归) |
| 7 | `DzipcLogRotation.QueueDepthTriggersRotation` 不稳定 | 低 | ✅ 已修(6/12 → 0/30) |
| 8 | 接收侧拒收完全静默 → 版本错配查不出来 | **高(可运维性)** | ✅ 已修(两侧计数, 见 §10) |
| 9 | `EnableDzFlat` 宏与函数同名 → 实参被**静默反转** | **高(语义反转)** | ✅ 已修(宏改全大写, 见 §11) |

**九条已全部闭环**。第 3/4/5 条同根(接收方无法察觉自己被套圈), 修复见 §8; 第 1/2/6/7 条见 §9;
第 8/9 条是回答"DZFlat 段如何升级"时暴露出来的 —— 升级流程的每一步都靠**看得见**和
**语义不反转**, 而这两条恰好各打掉一个, 见 §10 / §11。

本表保留下来是因为每一条的**成因与判据**仍然有用: 它们描述的都是"不报错、只是结果不对"的
失效形态, 排查同类问题时按症状反查比按模块反查快。

---

## 1. Python 侧用错解释器 → 静默用到陈旧 pybind 模块 【已修, 见 §9.1】

**症状**

Python 订阅者一条 DZFlat 消息都收不到; `topic_echo` 什么也不打印。看起来像通道没通、
话题名写错或 `msg_id` 不匹配 —— 但把发布端切回 TLV 就一切正常。

**成因**

pybind 模块是按**具体 Python 版本**编译的, 而本仓的构建目标是 CMake 找到的那一个:

```
build/CMakeCache.txt:  _Python3_EXECUTABLE:INTERNAL=/usr/bin/python3.10
```

产物落在 `build/python/_dzipc_core.cpython-310-*.so`。而 `python/dzipc/` 下同时存在
另外几个版本的模块, 来路各不相同:

| 文件 | 实际是什么 | 含 DZFlat 绑定 |
|---|---|---|
| `_dzipc_core.cpython-310-*.so` | 从 `build/python/` 手工拷来 | 是(本次更新过) |
| `_dzipc_core.cpython-311-*.so` | 早先构建遗留的实体文件 | **否** |
| `_dzipc_core.cpython-312-*.so` | **符号链接**, 指向 `build_py/python/` 这个**另一棵构建树** | **否** |

关键在于 CMake **没有**任何步骤把产物放进 `python/dzipc/`:

```cmake
set_target_properties(_dzipc_core PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python)
install(TARGETS _dzipc_core LIBRARY DESTINATION lib/python/dzipc ...)
```

输出目录是 `build/python`, 安装目标是 `lib/python/dzipc` —— **都不是** `python/dzipc/`。
那里的文件全靠手工拷贝或符号链接维持, 于是天然会陈旧。

**为什么症状这么误导**

`import dzipc` 照常成功, `GenericMessage` 照常存在, 而 schema 注册表是**纯 Python**,
所以照常加载 21 条。唯一缺的是 C++ 绑定里的 `has_dzflat` / `dzflat_memoryview`。实测:

```
$ python3.12 -c "... dzflat.decode_generic(GenericMessage()) ..."
has_dzflat 属性存在: False
decode_generic -> None          ← 与"这是条 TLV 消息"完全同一个返回值
schema 注册数: 21               ← 看起来一切正常
```

于是 `decode_generic` 返回 `None`, 与"这条消息本来就是 TLV"无法区分。

**现状: 已加告警, 根因未除**

`python/dzipc/dzflat.py` 的 `_check_binding()` 现在能把两种情况分开: `GenericMessage`
在新旧模块里都有 `field_count`, 只有新模块才有 `has_dzflat`。命中旧模块时发一次
`RuntimeWarning`, 明确指出是模块陈旧而非通道故障。

```
RuntimeWarning: dzipc 的 pybind 模块缺少 DZFlat 绑定(has_dzflat) —— 很可能用到了陈旧的 .so。
DZFlat 消息会全部读不到, 且症状像通道不通。请用构建时的那个解释器 ...
```

告警只治症状。根因是"`python/dzipc/` 下的模块靠手工维持", 仍然存在。

**该怎么办**

- 立即可用: 用构建时的那个解释器跑 Python 侧代码。验收脚本
  `test/test_dzflat_python.py` 因此固定 `python3.10`。
- 建议的根治: 在 `python/CMakeLists.txt` 加一个 `POST_BUILD` 拷贝, 把产物落到
  `python/dzipc/`, 并**删掉**那两个陈旧文件(含指向 `build_py/` 的符号链接)。这些 `.so`
  被 `python/.gitignore` 忽略, 属本地产物, 删除无副作用。
- 若确实需要多版本共存, 那就每个版本各有一棵构建树, 并让拷贝步骤按
  `Python3_SOABI` 命名 —— 但要先想清楚多版本共存到底有没有需求, 现在这三个文件更像
  是历史沉积而不是有意设计。

---

## 2. `getattr` 取 C++ 侧对象的字段 → 静默得到默认值 【已修, 见 §9.2】

**症状**

Python 里读某个字段, 永远得到 `getattr` 的默认值(`"N/A"`、`None`、`0`), 而消息其实是
正常送达的。

**成因**

`sub.try_get(td)` 之后 `out.topic()` 返回的是 **C++ 侧对象**(pybind 把
`std::shared_ptr<IpcMsgBase>` 下转成 `GenericMessage`)。它的字段**不是 Python 属性** ——
要用 `get_uint32("width")` 这类访问器。所以:

```python
getattr(msg, "additional_info", "N/A")   # 恒为 "N/A"
```

实测确认(即使消息正常送达):

```
getattr(msg,"additional_info","N/A") -> 'N/A'
有 field_count 吗: True | 有 has_dzflat 吗: False
```

`getattr` 带默认值时不抛异常, 于是错误被吞掉。

**现状: `topic_echo` 已修, 模式仍在**

`python/dzipc_topic_echo.py` 原先正是这么写的 —— 它用 `ipc.Supervisor()` 作模板并
`getattr(msg, "update_time")` / `getattr(msg, "additional_info")`, 因此**对所有类型都只
打印 `N/A`**。这与 DZFlat 无关, 是本次顺带发现并修掉的: 改用 `GenericMessage` 模板 +
`dzflat.dump()`, 一份代码同时覆盖 TLV 与 DZFlat。

但这个**模式**没有被消除: 任何新写的 Python 工具只要对 C++ 侧消息对象用 `getattr`,
都会掉进同一个坑, 而且不报错。

**该怎么办**

- 读 C++ 侧消息对象一律走访问器(`get_*`), 或直接用 `dzflat.dump(msg)` / 生成的
  Python 封装类(`dzipc.gen_msgs.StdImage.from_generic(...)`)。
- 写工具时避免 `getattr(msg, name, default)` 这种"取不到就算了"的写法 —— 至少不要带
  默认值, 让它抛出来。

---

## 3. chunk 池耗尽 → 64 字节分片重组错乱 → 段错误 【已修, 见 §8】

**症状**

发布端连续快发大消息而订阅端不及时取时, **订阅进程段错误**, 栈顶在
`IpcMsgBase::adapt_memcpy_tods` / `Xxx::deserialize`。

**成因**

`send()` 在 `acquire_storage` 失败(chunk 池 32 块/尺寸档位耗尽)时退化成 64 字节分片。
一条 0.88 MB 消息因此变成 14000+ 个分片打进 256 槽的环, `force_push` 覆写 → 接收侧
`recv` 的重组缓存(按 `msg.id_` 索引)拼出错乱的缓冲 → `check_id` 偶然通过 → TLV
`deserialize` 按垃圾长度走偏 → 越界。

**这不是 DZFlat 引入的**: 已用 `git worktree` 在**不含本次任何改动的 HEAD** 上复现。
且开启 DZFlat 后同一场景**不再崩**(146/200 条走了借样, 避开了分片路径) —— 借样反而
降低了触发概率。

与 `dzflat_shm.md` §5.3 记的"池耗尽是性能悬崖"是同一根因的两面: **悬崖底下还有一个内存
安全问题**。

**该怎么办**

- 修法方向: 在 `pop()` 侧增加覆写检测(与第 5 条同族), 让被套圈的重组直接判废而不是
  拼出半条消息。
- 规避: 别让 chunk 池耗尽 —— 遵守 `dzflat_shm.md` §5.3 的持有预算
  (`32 / (帧率 × 订阅者数)`), 或开启 DZFlat 借样。
- 基准脚本 `test/dzflat_tx_benchmark.cpp` 因此必须**发一条取一条**, 已在文件头注明。

---

## 4. 被套圈接收方重复入池 → id_pool 空闲链表自环 【已修, 见 §8】

**症状**

长时间运行后大消息性能突然塌陷(退化成分片), 且 chunk 池"变小"了。

**成因**

被套圈(lapped)的接收方读空时会把同一个环槽位读到两次 —— 游标落后写指针超过 256 时,
`cur` 与 `cur+256` 映射到同一 `index_of`。若该槽位承载大消息, 同一个 `storage_id` 会
产生两个 `buff_t`, 于是 `recycle_storage` 走两次: 第一次把 id 放回 `id_pool`, 第二次在
`conns` 已为 0 的情况下**再放一次**。`id_pool::release` 是 `next_[id] = cursor_` 的头插,
重复入池会把链表接成**自环**并丢掉其后的全部 id。

**现状**: 未修, 既存。已登记在 `test/test_chunk_hold.cpp` 的文件头注释里, 该文件的两个
用例**刻意绕开**这条路径(不 drain 被套圈的接收方), 否则会污染其自身的判据。

**该怎么办**: 与第 3、5 条同族 —— 需要在 `pop()` 侧加覆写/套圈检测。三条一起修比分开修
划算。

---

## 5. 覆写路径的残余竞态窗口 【已修, 见 §8】

**症状**: 极低概率的数据撕裂(读方拿到半新半旧的 chunk 内容)。

**成因**: Step 0 已把"覆写即无条件归还 chunk"改成按 `conns` 位图条件归还
(`ipc.cpp: discard_storage`), 但 `pop()` 是**先把槽位数据拷出、之后才清自己的 rc 位**,
所以一个"已读到 `storage_id` 但尚未清位"的接收方仍会被算进 `rem_cc`。

**现状**: 有意留下。这与 `prod_cons.h` 的 `force_push` 注释里早已登记的"覆写与 `pop`
无互斥 → 数据撕裂"同源。Step 0 把问题从"发布方一绕圈就必然踩"收窄回"与撕裂同源的窄
缝", 没有声称根除 —— 见 `dzflat_shm.md` §5.1 的"残余窗口"段。

**该怎么办**: 在 `pop()` 侧加覆写检测。**B 级借样让发布方在 chunk 上停留更久(构造期间),
更依赖这一条**, 所以若要大规模用 B 级, 这条的优先级应当上调。

---

## 6. srv(请求/应答类型)未接入 DZFlat 【已接入, 见 §9.3】

**成因**: `ServiceGenerator` 覆写了 `generate_hpp_file`([srv_generator.py:104](../generator/srv_generator.py#L104)),
自己组装文件, 不发射 DZFlat 段。因此它必须把 `emit_dzflat` 置为 `False` —— 否则类里会
留下**已声明未定义**的虚函数, 链接期报 `undefined reference to vtable`(落地过程中真的
撞到过, 7 个测试二进制链接失败)。

**现状**: 射程边界, 非技术障碍。`shm_ser_cli` 同样走 SHM 且共享同一个 chunk 池, 收益机制
与 pub/sub 完全相同。

**该怎么办**: 让 `ServiceGenerator` 也调用 `generate_dzflat_section`, 并把
`emit_dzflat` 改回 `True`; 之后 `shm_ser_cli_ipc` 照 `shm_pub_sub_ipc` 的双 wire 分派
改一遍即可。

---

## 7. `DzipcLogRotation.QueueDepthTriggersRotation` 不稳定 【已修, 见 §9.4】

**症状**: `test_dzipc_log` 间歇失败, 报 `bags.size() >= 2, actual: 1`。

**成因**: 时间敏感的日志轮转用例, 与 DZFlat 无关。

**现状**: 既存。已量化确认**不是本次改动引入的**:

| | 12 次运行失败次数 |
|---|---|
| 未含本次任何改动的 HEAD | 6 / 12 |
| 本次改动后 | 7 / 12 |

同一水平, 差异在噪声内。

**该怎么办**: 不要把它当成回归。全量测试扫描时应把它单列, 其余 32 个 gtest 二进制是
稳定全绿的。

---

## 8. 第 3/4/5 条的修复(2026-09-03)

三条同根: **接收方无法察觉自己被套圈**(游标落后写指针超过环长 256)。修复分四处, 每一处都
只用已有状态, 没有新增共享内存字段。

### 8.1 `pop()`: 被套圈则重同步游标 (`prod_cons.h`, single-multi-broadcast)

落后超过一整圈时, `cur` 指向的格子早被写方甩过多轮, 其当前内容属于更新的世代。旧实现
逐格照读, 于是从被甩过的格子里读出的 `storage_id` 可能已归还并被复用 —— 读方的 `buff_t`
析构时会回收**别人正在用的** chunk(ABA)。

现在直接把游标跳到"环里还留着的最旧一格", 中间全部放弃 —— 这正是 `force_push` 注释里
早已声明的语义(慢订阅者保持连接、只丢失被套圈的消息)。

### 8.2 `pop()`: 本格是否已消费过 (同上)

`el->rc_` 低 32 位是"尚未放行本格的读方位图", 写方落格时置上全部在连位, 读方在 `pop`
末尾清掉自己那位。所以"我的位已清" ⟺ 这一格的当前内容我已取走过 ⟹ 套圈重读, 跳过。

新接入的读方 `cursor_` 取自当时的写指针(`queue.h` 的 `connect()`), 所以不会因"入连前
写入的格子"而误跳。

### 8.3 `pop()`: 拷贝期间被覆写则丢弃 —— 第 5 条的正解 (同上)

只有 `force_push` 能覆写我还持位的格子(普通 `push` 在 `cc & rem_cc != 0` 时直接判满),
而 `force_push` 必然先自增 `epoch_`。于是拷贝前后各读一次 `rc_` 的高 32 位(epoch)就够:
变了说明本格在我拷贝途中被覆写, 数据可能撕裂 → 丢弃。这就把 §5 登记的残余窗口关掉了。

### 8.4 chunk 归还的判据统一到 `rem_cc` (`ipc.cpp: clear_message`)

修 8.1/8.2 时暴露出一个**更大的既存泄漏**, 此前一直被第 4 条的双重入池掩盖着:

`<single,multi,broadcast>::push` 的判满条件是"仍有在连读方持有本格 **且** 该持有属于当前
世代"。一旦此前有过 `force_push` 抬升 `epoch_`, 早于那次抬升写入的格子就带着旧世代 ——
即便读方的位还置着(它被套圈了, 永远不会来取), 判据也不成立, `push` 会直接覆写。而旧实现
只给 `force_push` 传了 `clear_message`, `push` 传的是空回调, 于是**每次这种覆写都漏一块
chunk**。实测洪泛 768 条后 32 块全部漏光, 大消息随即永久退化成 64 字节分片。

两个缺陷此前互相抵消: 被套圈的读方把同一格读两次, 同一 `storage_id` 二次入池, 恰好补上
漏掉的那些 —— 池子于是"看起来"是满的。去掉重复投递后, 泄漏立刻显形(空闲 chunk 0/32)。

修法两半:

1. `push` 也传 `clear_message`(回调签名统一为 `(void*, cc_t)`), 于是被覆写的被套圈消息
   其 chunk 有人归还;
2. **归还的唯一依据是 `rem_cc != 0`**。`rem_cc == 0` 意味着该消息所有该收的读方都已 pop
   过, chunk 生命周期完全归它们的 `buff_t` —— 写方此时无权归还, 否则又是双重入池。
   这一条同时让 unicast 天然安全(其 `push` 只取读方已放行的格子, `rem_cc` 恒 0; 其
   `force_push` 从不调用回调)。

> 少了第 2 半会怎样: 实测 `IPC.1v1` / `1vN` / `Nv1` / `NvN` 四个吞吐用例**直接挂死** ——
> `id_pool::release` 的头插把空闲链表接成自环(`next_[id] = cursor_` 而 `cursor_` 已是
> `id`), 此后每次 `acquire` 都返回同一个 id, 所有大消息共用一块 chunk 互相踩踏。

### 8.5 `deserialize` 的边界检查 —— 第 3 条的第二道防线 (`ipc_msg_base.hpp`)

8.1 去掉的是**触发条件**, 但底下那个洞还在: `adapt_memcpy_tods` 的偏移完全由**缓冲自身
内容**算出(字段名长度、数组元素个数都从 wire 里读), 而它**连 buffer 大小都拿不到**, 根本
无法拦。任何被截断或错乱的缓冲都能把偏移推到任意远处 —— 这比"套圈重组"宽得多。

不需要改任何调用点: 生成的 `deserialize` 第一句就是 `deserialize_data_cut(buffer.size())`,
它把 `_total_size` 设成了缓冲长度。于是:

- `adapt_memcpy_tods` **逐段**校验(不能只在入口算一次总跨度 —— 跨页字段的实际读取会在
  每段之后额外跨过 12 个页尾字节, 物理跨度大于 `local_data_len`);
- 越界即置位 `_deser_overflow` 并且**一个字节都不拷**;
- 新增公开的 `deserialize_ok()`, SHM 订阅循环据此**丢弃整条消息** —— 把"崩进程"降级成
  "丢一条"。

已单独验证这道防线的有效性: 临时关掉 8.1 的游标重同步(让错乱缓冲照旧到达 `deserialize`),
`FloodedSubscriberMustNotCrash` 仍然通过。**两道防线各自独立成立。**

### 8.6 验收

`test/test_lap_safety.cpp` 三个用例, 修复前后对照:

| 用例 | 修复前 | 修复后 |
|---|---|---|
| `FloodedSubscriberMustNotCrash` | **信号 11 打死** | ok |
| `LappedDrainMustNotDeliverDuplicates` | 288 条里 **283 条重复** | ok |
| `LappedDrainMustNotShrinkChunkPool` | 2/20(泄漏显形后) | ok |

chunk 池分阶段实测: 初始 32 → 正常收发 20 条后 32 → 洪泛 768 条不读后 12(20 块被环里
未读消息合法持有) → 套圈读空后 **回到 32**。

33 个 gtest 二进制全绿(含 `IPC.*` 八个吞吐用例), Python 验收全过, DZFlat 传输/B 级基准
数字无变化。

**注意**: 被套圈的读方现在会**明确丢失**中间的消息(实测 768 条里收到 22 条)。这不是新增
的损失 —— 旧实现"收到"的那些本来就是错乱内容, 只是以段错误和池子腐坏的形式表现出来。
需要不丢消息的场景必须遵守 `dzflat_shm.md` §5.3 的持有预算, 或提高订阅端消费速率。

---

## 9. 第 1/2/6/7 条的闭环(2026-09-04)

### 9.1 构建自动同步 pybind 模块并清理其他版本(第 1 条)

根因是 `python/dzipc/`(即 `import dzipc` 实际加载的包目录)下的 `.so` **靠手工维持**:
CMake 的输出目录是 `${CMAKE_BINARY_DIR}/python`, install 目标是 `lib/python/dzipc`,
两者都不是它。

- `python/CMakeLists.txt` 加 `POST_BUILD` 步骤, 构建完把产物 `copy_if_different` 到
  `python/dzipc/`;
- 再跑 `python/prune_stale_modules.cmake` 删掉**其他 Python 版本**的 `_dzipc_core*.so`
  —— 同一包目录里并存多个版本时, 用哪个解释器就加载哪个, 而只有本次构建的是新的。
  实测清理掉了一个 311 的实体文件和一个指向另一棵构建树(`build_py/`)的 312 符号链接。
- `test/test_dzflat_python.py` 不再硬编码解释器: 它从 `.so` 文件名里的 ABI 标签
  (`cpython-312` → 3.12)推出该用哪个解释器, 不匹配就 `os.execv` 切过去。
  **硬编码本身就是同一个坑的另一种形态** —— 构建目标换了解释器(本次就从 3.10 变成了
  3.12), 硬编码的脚本会跟着失灵。

`dzflat.py` 里的 `_check_binding()` 告警保留: 它覆盖"用了别处安装的旧包"这种构建步骤
管不到的情形。

### 9.2 封装类补齐 DZFlat 分支, 并改正三处调用点(第 2 条)

问题的本质是"C++ 侧消息对象的字段不是 Python 属性", 而 `getattr(obj, name, default)`
会把这个错误吞成默认值。真正的闭环不是提醒大家小心, 而是**让正确的读法可用且唯一**:

- generator 给每个封装类的 `from_generic()` 加了 DZFlat 分支(先试
  `dzflat.decode_generic`, 不是 DZFlat 段才走原 TLV 路径), 并新增
  `_from_dzflat_dict()` 递归构造嵌套封装对象。此前对 DZFlat 消息调 `from_generic`
  会走到 `g.get_uint32()` 并抛 `Field not found` —— 响亮但无用。
- `dzipc_topic_echo.py` 改用 `GenericMessage` 模板 + `dzflat.dump()`(上一轮已做)。
- `ipc_demo.py` 修两处: 订阅侧原先是
  `if not hasattr(msg_obj,"data3"): msg_obj = msg_template` 加一串
  `getattr(..., 默认值)` —— `hasattr` 恒为假, 于是**每次都退回读模板对象**(空值), 而
  `getattr` 的默认值把错误吞掉, 打印出来永远是空列表、也永远收不到 `exit`。现在走
  `TestMsg.from_generic(msg_obj)`。发布侧另有一个独立缺陷: `publish()` 收的是 C++ 侧
  消息对象而非 Python 封装类, 必须 `.to_generic()` —— 注意这与 `make_topic_data`
  **不对称**(那个会自动转换), 直接传封装对象会得到一条 pybind 的类型错误。
  两处修完 demo 才第一次真正跑通(实测收到 4 条真实数据并正常退出)。

验收: `test_dzflat_python.py` 新增第 [4] 组, 断言 `StdImage.from_generic()` 在 DZFlat
与 TLV 两种 wire 下都给出正确值, 且嵌套字段是 `StdHeader` 对象而非 dict。

### 9.3 srv 接入 DZFlat(第 6 条)

- `ServiceGenerator.emit_dzflat` 改回 `True`, 并在 `generate_class` 闭合 `};` 之后
  追加 `generate_dzflat_section` —— 虚函数的**声明在类内、定义在类外**必须成对, 少任何
  一半都是链接期 `undefined vtable`(这正是当初把它关掉的原因)。
- `shm_ser_cli_ipc.cc` 按 pub/sub 的双 wire 分派镜像了四个点: 客户端发请求 / 服务端收
  请求 / 服务端发响应 / 客户端收响应。共用两个辅助: `try_send_dzflat()`(借样发送, 失败
  即回退整包)与 `accept_wire()`(按段首 magic 判别, 并对 TLV 路径检查
  `deserialize_ok()`)。
- ser/cli 用的是 `ipc::server`(single-single-unicast), 与 pub/sub 的 `route` 不同: 其
  recv 侧 recycle 无条件归还。对借样无影响 —— 一条消息只有一个接收方, 其 `buff_t`
  析构即归还。`RepeatedRoundTripsDoNotExhaustPool` 用 100 次往返(远超 32 块池容量)钉住
  这一点。

验收: `test/test_dzflat_sercli.cpp` 三例。**断言有牙**: 临时禁掉 `try_send_dzflat` 的
判据后, 两个用例立刻报 `一条也没走 DZFlat(dzflat=0 fallback=2)`。

### 9.4 日志轮转用例去随机化(第 7 条)

原写法用 64 KB 负载 + 每轮 `sleep(50ms)`, 是在拿生产者线程赛跑写线程:

- `RecordPublish` 会把负载 memcpy 进事件, 所以 64 KB 让**生产者**每次 push 的成本与写
  线程每次 pop+落盘的成本相当, 谁快谁慢全看调度;
- 每轮之间的 sleep 又给写线程排空队列的机会, 下一轮从 0 重新爬。

改成**小负载(64 B) + 不间断紧循环 400 次**: 写线程每个事件有固定开销(序列化、索引项、
ofstream 调用), 而生产者只是一次小 memcpy + 入队, 于是生产者稳定跑赢, 队列必然爬过
`max_queue`(10)从而 arm 住轮转。超过容量的推送被丢弃是正常的 —— arm 只需要队列**触到**
10 一次。

实测: 单用例 **30 次 0 失败**(原 12 次失败 6 次), 整个 `test_dzipc_log` 连跑 10 次全过。
全量扫描不再需要把它单列。

### 9.5 现状

**35 个 gtest 二进制全绿**(含此前 flaky 的 `test_dzipc_log`), Python 验收全过, 生成器
幂等, DZFlat 传输/B 级基准数字无变化。

---

## 附: 与其他文档的关系

- [dzflat_shm.md](dzflat_shm.md) — DZFlat 的设计、四个 Step 的落地与实测。本表第 3/4/5
  条在其 §5.1 / §5.3 / §9.4 有更详细的机制描述。
- [loaned_blob_borrow.md](loaned_blob_borrow.md) — 已归档的前身方案, 不实施。

---

## 10. 第 8 条: 接收侧拒收完全静默

**症状**

消息量对不上 —— 发布端计数在涨, 订阅端收到的条数少一截或干脆是 0。**两端都不报错**,
日志里什么都没有。切回 TLV 就正常。

**成因**

订阅侧的四条拒收出口原本都是裸 `continue`([`shm_pub_sub_ipc.cc`](../src/dzIPC/shm_pub_sub_ipc.cc)
的双 wire 分派, 以及 `shm_ser_cli_ipc.cc` 的 `accept_wire`) —— 不记日志、不抛错、无计数。
既有的 `DzFlatPublishCount` / `DzFlatFallbackCount` 是**发布侧**的, 看不见接收端丢了多少。

于是"版本错配"这种一定会发生的部署事故没有任何抓手。DZFlat 是定长布局, schema 一变指纹
就变, 老读端只能拒收(见 [dzflat_shm.md §3.8](dzflat_shm.md)); 拒收本身是正确的, **不可观测**
才是缺陷。

**现状**

已修。`AcceptWire()`([`wire_accept.cc`](../src/dzIPC/common/wire_accept.cc))成为双 wire 判别的
唯一实现, pub/sub 与 ser/cli 共用 —— 原先两条路径各写了一遍同样的逻辑, 两份实现意味着两份
埋点, 迟早漂移成"一条路径的丢弃看得见, 另一条看不见"。

六个量, 关键在于**把"正常过滤"与"真实缺陷"分开**: `msg_id` 不符是同一通道多种消息混跑时的
常态, 也是这里唯一的高频量, 不单独拿出来就会把真信号淹到看不见。同理 `dzflat_header_bad`
必须与 `tlv_id_skipped` 分开 —— 一个**损坏的 DZFlat 段**若任其落进 TLV 分支, 尾部校验必然
失配, 看起来和"这条不是我的话题"一模一样。

各量含义与判读见 [dzflat_shm.md §3.8](dzflat_shm.md)。

**Python 进程的那一半在别处**: `GenericMessage` 是无 schema 的直通体, 对任何格式合法的段都
收下, 所以 C++ 的 `dzflat_schema_drop` 在 Python 进程里恒为 0。版本错配要到按指纹查表时才
暴露, 计数在 `dzipc.dzflat.rx_stats()`。两侧合起来看才完整。

**该怎么办**

- 排查"消息量对不上": 先看 `DzFlatRxCounters().defects()`。非 0 就按上表定位;
- `dzflat_schema_drop > 0` ⇒ 两端 msg 定义不是同一份, 按 §3.8 的顺序重滚(先关 DZFlat);
- Python 侧另看 `rx_stats()["schema_unknown"]` —— 它非 0 就等于"该重跑 generator 了";
- 回归: `test_wire_accept`(9 例) + `test_dzflat_python.py` 第 [6] 组。每例断言**恰好那一项
  +1 且其余不动**; 只断言"目标项涨了"是没有牙的 —— 把所有事件记到同一项、或者返回常量的
  实现都能过。变异验证: 埋点改空操作 → 9 例全挂; 把 `schema_drop` 归并进 `id_skipped` →
  对应那一例挂。

---

## 11. 第 9 条: `EnableDzFlat` 宏与函数同名 → 实参被静默反转

**症状**

调用 `dzIPC::EnableDzFlat(false)` 之后 `IsDzFlatEnabled()` 返回 **true**。编译**不报错**。
只在包含了 [`dzipc.h`](../include/dzIPC/dzipc.h) 的翻译单元里发生 —— 而那正是使用者该包含
的伞形头文件。

**成因**

```c
#define EnableDzFlat EnableDzFlat(true);   // 与函数同名!
```

对象式宏, 且宏名与函数名相同。C 预处理的"蓝漆"规则使替换列表里的同名标识符不再展开, 于是

```
dzIPC::EnableDzFlat(false);   →   dzIPC::EnableDzFlat(true);(false);
```

**两条合法语句** —— 语法没问题, 编译器无话可说, 而实参被静默换成了 `true`。

比编译错误糟得多: 报错会立刻被发现, 这个不会。而 `EnableDzFlat(false)` 恰好是灰度升级的
第一步([dzflat_shm.md §3.8](dzflat_shm.md)), 反转的后果是**把 DZFlat 段推给未升级的订阅方**,
表现正是第 8 条那个静默丢消息。两条缺陷叠在一起: 一个制造事故, 另一个让事故不可见。

之所以一直没暴露: 邻近的 `ENABLENODELET` / `DISABLENODELET` 是全大写, 不与函数碰撞; 而
DZFlat 的两个宏写成了混合大小写, 其中 `EnableDzFlat` 正好撞上函数名。所有实际调用方(测试、
基准)都只包含 `nodelet_config.h`, 不包含伞形头, 所以谁也没踩到。

**现状**

已修: 宏改名为 `ENABLEDZFLAT` / `DISABLEDZFLAT`, 与既有的 `ENABLENODELET` 一致。原混合大小写
的两个宏**无任何使用者**(全仓 grep 确认), 改名无兼容负担。

**该怎么办**

- 给便捷宏起名时, 全大写不是风格偏好而是**必须** —— 与函数同名的对象式宏会静默改写调用点;
- 回归: `test_dzflat_python.py` 第 [7] 组钉住"`EnableDzFlat(False)` 必须真的关掉"。这条看着
  像废话, 但它守的正是这个已经发生过的缺陷。

---

## 12. 第 3 条的另一半: count 未校验 → 无界分配(实测 4.3GB 提交, exit=134)

**症状**

`ipc_benchmark` 在 262144B 档偶发 `terminate called after throwing an instance of
'std::length_error'`, `what(): vector::_M_default_append`, **exit=134**(abort)。间歇性 ——
不是每次必现, 取决于损坏缓冲里恰好是什么值。

**成因**

第 3 条的修复(adapt_memcpy_tods 越界即止、置位、不拷)把"越界读 → SIGSEGV"堵住了, 但
生成的 TLV 反序列化长这样:

```cpp
int32_t data1_count;                                 // 未初始化
this->adapt_memcpy_tods(&data1_count, buffer, ...);  // 越界 → 一个字节都不拷
data1.resize(data1_count);                           // ← 分配, 跑在拷贝之前
```

于是出了两条**独立的**路径, 都能绕过那道读边界检查 —— 因为它们不是读越界而是**无界分配**:

- **① count 读失败**: 越界即止"不拷任何字节" → `data1_count` 保持栈上未初始化垃圾 →
  `resize(垃圾)`。垃圾值恰好很大时就是 length_error / bad_alloc;
- **② count 读成功但值荒谬**: 套圈覆写/篡改把 count 字节改成巨大值 →
  `resize(0x20000000)` 提交 4.3GB(`vector::resize` 值初始化, 是真写下去的)。内存一紧就
  bad_alloc, 或 OOM killer。

两者都发生在 `adapt_memcpy_tods` 的检查**之前**(resize 先分配), 所以那道检查轮不到;
抛出的异常这条路径上无人捕获 → abort。

**为什么没早点暴露**: 它藏在 TLV 反序列化里, 只在订阅端收到**损坏的 TLV 重组缓冲**时触发
(套圈产物的典型形态), 且依赖垃圾值恰好落在"能通过, 但巨大"的范围 —— 概率性。

**现状**

已修, **两道独立的闸**(各自单独被变异测试验证过):

- **分配前 count 闸**(generator 发射): 新增 `IpcMsgBase::tods_count_ok(at, count,
  min_elem_size)` —— `count × 最小元素字节数 > 缓冲剩余` 就置位并 `return`, 在
  `resize`/`reserve`/`构造 string` **之前**拦下。只拦荒谬值, 合法消息恒能通过(逻辑长度
  恒 ≤ 物理剩余)。覆盖 5 种形态: 嵌套数组、字符串字段、字符串数组(+逐元素 str_size)、
  bool 数组、标量数组。
- **失败路径清零**(基类, 纵深防御): `adapt_memcpy_tods` 越界时把目标剩余字节清零。这层
  保护**所有** deserialize —— 包括手写的、以及还没重新生成的头文件(它们没有 count 闸,
  清零保证 count 失败时读到的是确定的 0 而非栈垃圾)。

实测: 把 count 篡改成 `0x20000000` 后修复前 `data1.size = 536870912`, 修复后 `0`。

**该怎么办**

- 回归: `test_deser_alloc_guard`(5 例, 全确定性 —— 间歇性崩溃不能靠"跑通一次"验收):
  荒谬 count、负数 count、count 读失败(截断在 count 字段中间, 不能补页尾否则读取会成功)、
  直接测清零层(TodsProbe 越过生成代码)、未被篡改的同一条必须仍解得开。
- 变异验证: 去掉 count 闸 → 前三条挂; 去掉清零 → 只有直接测清零那条挂(证明两条防线各自
  有覆盖, 而非互相掩盖)。

**已知残余(未修)**: 嵌套元素的溢出 flag 是**子对象自己的成员**, 父对象与 AcceptWire 看不见
(alias 路径里 `{field}[i].deserialize(...)` 置的是元素自身的 `_deser_overflow`)。所以嵌套
缓冲内部被篡改时可能**部分解析但整条不丢**。不做内存安全风险(count 闸已挡分配、alias 路径
已用 region_is_contiguous 限界读取), 只是静默部分解析 —— 属正确性疣, 留待需要时把嵌套
flag 向上传播。
