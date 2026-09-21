# UF-010 证据登记(待 writer-claude 落笔进 unfixed_defects.md)

> ⛔ 本文件是**登记材料**, 不是台账本体。`docs/unfixed_defects.md` 是单写者协议, 实际条目由
> writer-claude 落笔。本文件只提供可机械核对的证据链, 不构成对产品码的修改授权。
>
> 状态建议: ~~⏸️待决策~~ **✅已闭环**(2026-09-19 收尾写回)—— 原状态"仅证据·不动产品码";同日决策方授权**选项 A**并落码提交(`90510a5`),
> 修法/判据/复跑见 `uf010_hash_consistency_fix.md`, 台账已落 UF-010 行(✅已修)。本文件保留为证据链原始记录。

## 建议台账条目(一句话 + 指针)

| ID | 标题 | 状态 | 详情 |
|---|---|---|---|
| UF-010 | `ipc::hash<ipc::string>` 按指针哈希 ⇒ shm 句柄缓存永不命中 | ⏸️仅证据, 不授权改产品 | 本文件 |

**速览一句话**: `hash<string>` 特化返回 `std::hash<char const *>{}(val.c_str())`(**哈希指针**),
而容器相等性用 `std::equal_to<Key>`(**比内容**) ⇒ 两者语义不一致 ⇒ 内容相同的两个 `ipc::string`
对象落进**不同桶**、永不命中已有条目 ⇒ 同一 shm 段被反复 `mmap` 且**从不 `munmap`**;
多接收者零拷贝广播因此拿到**不同虚拟地址**(同一物理内存) ⇒ `Loan.BroadcastToMultipleReceivers`
的 `EXPECT_EQ(g1.data(), g2.data())` 失败。**数据正确性不受影响**(两条 `all_bytes_are` 均通过)。

## 三方指纹(证据规范 RELEASE_NOTES.md 0918 起)

| 键 | 值 |
|---|---|
| `tree_head` | `2f13665` (分支 `dev`) |
| `libipc_md5` | `88fb143b56f4ee9c4074fd07ad6b1999` (`build/lib/libipc.so`) |
| 源 `src/libipc/ipc.cpp` | `2336bbea4f131e61e2f143b91812314f` |
| 源 `src/libipc/memory/resource.h` | `50130d80785786439d8d4ab20122efe6` |
| 源 `test/test_loan.cpp` | `665ae18da6085f870a333335d145d4d1` |
| `driver_md5` | `4fd2882dfe441ed507d98f93902fb42f` (`build/bin/test_loan`) |

⚠️ 探针/插桩轮次使用过**临时插桩构建**(已逐字节还原, md5 自证如上 `2336bbea…`);
`strace`/插桩读数采自插桩构建, 隔离对照表与复跑采自上述干净指纹构建。

## 1. 现象

`test_loan` 的 `Loan.BroadcastToMultipleReceivers`(`test/test_loan.cpp:203` 起)失败于
`:224` 的 `EXPECT_EQ(g1.data(), g2.data())`; 同用例 `:213` 的 `ASSERT_TRUE(lo.valid())` **通过**
(池并未耗尽), 两条 `all_bytes_are(...)` 也**通过**(数据内容正确)。地址差实测恒为
**−167936 (0x29000)**。

## 2. 隔离定性(用户 2026-09-19 选定路径: 先隔离定性)

判据(用户原话): "让该用例独占进程/独占池后单跑。若仍稳定红 ⇒ 零拷贝广播在多接收者下确实
不可靠 = 真产品缺陷; 若变绿 ⇒ 纯属用例间时序耦合。"

| 实验 | 结果 |
|---|---|
| 当前树 · 该用例单跑 ×20 | **红 20/20** |
| HEAD 干净 worktree · 该用例单跑 ×10 | **红 10/10** ← 决定性 |
| HEAD 干净 worktree · 全量 ×5 | 绿 5/5 |
| 当前树 · 全量 ×10 | 红 10/10 |
| HEAD · "逐个前置用例 + 该用例"配对 ×6 | 红 30/30 |

**结论**: 隔离后仍**稳定红**, 且在**未含步骤①/③ 任何改动**的 HEAD 树上同样红
⇒ **真产品缺陷, 且是既有缺陷**, 非步骤①(`ipc.cpp` 池空诊断)或步骤③(`shm_pub_queue` 钉)引入。

⛔ **一处已撤回的归因**: 一度把该红归为"用例间时序耦合", 由隔离实验直接**推翻**。
"HEAD 全量跑绿"不代表无缺陷(见 §6)。

## 3. 根因(代码)

`src/libipc/memory/resource.h:69-73`:

```cpp
template <> struct hash<string> {
    std::size_t operator()(string const &val) const noexcept {
        return std::hash<char const *>{}(val.c_str());   // ⛔ 哈希的是指针
    }
};
```

`src/libipc/memory/resource.h:51-54`:

```cpp
template <typename Key, typename T>
using unordered_map = std::unordered_map<
    Key, T, ipc::hash<Key>, std::equal_to<Key>, ipc::mem::allocator<std::pair<Key const, T>>
>;                                        // ↑ 相等性按**内容**
```

**哈希按地址 + 相等按内容 ⇒ 二者不一致**。两个内容相同但地址不同的 `ipc::string` 对象
哈希值不同 ⇒ 落进不同桶 ⇒ `find()` **永远不命中**已存在的条目 ⇒ 每次都 `emplace` 新条目。

## 4. 证据链(三条独立)

### 4.1 插桩: 同一个 key 被反复当新 key 插入

在 `get_info`(`ipc.cpp:330-355`)的 `h = &(handles_[pref]);` 后插入一次性诊断
(打印 `pref` / `handles_.size()` / `h->valid()`), 门控 `chunk_size == 5120`:

```
单跑:  pref=[] hsz=1 hvalid=0   ← 条目 1(新)
       pref=[] hsz=1 hvalid=1   ← 命中
       pref=[] hsz=2 hvalid=0   ← 条目 2: pref 明明恒为空串, 却仍是新条目!
       pref=[] hsz=2 hvalid=1
       pref=[] hsz=3 hvalid=0   ← 条目 3
全量:  hsz 1→2→3→4 同构
```

`pref=[]` **恒定不变**, 而 `handles_.size()` **单调增长** ⇒ 同一内容反复插入,
与 §3 的机制预测一致。

### 4.2 独立小程序: 相等却不同哈希

`docs/probe_hash_semantics.cpp`(只链 libipc 的 hash, **不经过被测路径**):

```bash
g++ -std=c++17 -I src -I include docs/probe_hash_semantics.cpp -o /tmp/probe_hash && /tmp/probe_hash
```

```
空串:  a==b? 1   hash(a)=140727456028904  hash(b)=140727456028952  hash相等? 0
长串:  c==d? 1   hash(c)=140727456029000  hash(d)=140727456029048  hash相等? 0
两个内容相同的空串插入后 map.size()=2  (期望 1)
两个内容相同的长串插入后 map.size()=2  (期望 1)
```

(哈希值为运行期地址, 逐次不同; **承重的是 `hash相等? 0` 与 `map.size()=2`** 这两个结论, 不是具体数值。)

### 4.3 `strace`: 重复 mmap 且零 munmap

段 `/dev/shm/__IPC_SHM__CHUNK_INFO__5120`(163884 B = `sizeof(chunk_info_t) + 32×5120`):

| 场景 | mmap 次数 | munmap 次数 |
|---|---|---|
| 单跑 | 3 | **0** |
| 全量 | 4 | **0** |

映射发起点的回溯(LD_PRELOAD `mmap` 拦截)落在 `loan()`、`recv()`、`buffer::~buffer()` 三处
—— 正是持有 `ipc::string` prefix 的三个不同对象。对齐映射基址后可见
`g1` 在 loan/sender 建的映射内、`g2` 在 rx1 建的映射内。

## 5. 影响面(边界已核)

受影响的判据 = **`ipc::unordered_map` + `ipc::string` 作 key**:

| 位置 | 是否受影响 | 理由 |
|---|---|---|
| `src/libipc/ipc.cpp:110` `cc_acc` 的 `static ipc::unordered_map<ipc::string, ipc::shm::handle> handles` | **是** | 判据命中 |
| `src/libipc/ipc.cpp:301` `chunk_handle_t::handles_` | **是** | 判据命中 |
| `src/libipc/ipc.cpp:224` `thread_local ipc::unordered_map<msg_id_t, cache_t> tls` | 否 | key 为整数, 走默认 `std::hash` |
| `src/libipc/platform/linux/mutex.h:111` `ipc::map<ipc::string, shm_data> mutex_handles` | 否 | `ipc::map` = `std::map` + `std::less<Key>`(`resource.h:56-59`), 有序树不碰 hash |
| `src/libipc/platform/posix/mutex.h:41` 同上 | 否 | 同上 |
| `sniffer.cpp:199/204`、`ipc.cpp:357` 的 `ipc::map<...>` | 否 | 同上(且 key 非 string) |

⛔ **同一个坑的第二处**: `resource.h:75-79` 的 `hash<wstring>` 特化
(`std::hash<wchar_t const *>{}(val.c_str())`)有**完全相同**的缺陷。全仓当前无
`ipc::unordered_map<ipc::wstring, ...>` 使用 ⇒ 暂无实体, 但任何后续使用都会中。
修 `hash<string>` 时应一并处理。

## 6. 后果与严重性

| 维度 | 判定 |
|---|---|
| 数据正确性 | ✅ **不受影响**(两条 `all_bytes_are` 均通过; §7 复现输出一致) |
| 零拷贝语义 | ❌ **退化为拷贝**: 同一物理内存拿到不同虚拟地址 ⇒ 多接收者零拷贝广播的地址共享前提不成立 |
| 资源 | ❌ **映射泄漏**: `munmap` 次数为 0, 段随调用次数累积重复映射 |
| 性能 | ❌ 每次 `loan`/`recv`/`~buffer` 都可能触发一次新 `mmap`(系统调用 + 页表) |

**为什么以前一直没暴露(推断, 未直接测量)**: 绿/红取决于两个 key 是否**偶然**落进同一个哈希桶。
步骤① 的 `note_pool_exhausted` 在被故意借空池的 `PoolExhaustionIsRecoverable`(排在红例之前)里
做一次 stderr I/O + 分配 ⇒ 移动了后续 `rx1`/`rx2` 的 `prefix_` 对象地址 ⇒ hash 变 ⇒ 不再同桶 ⇒ 红。
**间接佐证**: 插桩版**全量跑也红**, 说明"全量红"并不依赖步骤①的存在。
⇒ **该用例本质是 flaky 的, HEAD 全量跑绿是桶碰撞的巧合**, 不是"HEAD 无缺陷"。
⚠️ 本条是**推断**: 要坐实需在 HEAD 树上插桩打印两把 key 的桶索引做一次直接测量。
**它不改变 §2/§3/§5 的任何结论**, 只影响"为什么以前没暴露"的叙述。

## 7. 复现命令

```bash
cd /home/zwc/cpp_ipc_dds
cmake --build build -j8 --target test_loan
LD_LIBRARY_PATH=$PWD/build/lib ./build/bin/test_loan \
    --gtest_filter=Loan.BroadcastToMultipleReceivers      # 稳定红

# 隔离定性(判"是否用例间耦合")
for i in $(seq 1 20); do
    LD_LIBRARY_PATH=$PWD/build/lib ./build/bin/test_loan \
        --gtest_filter=Loan.BroadcastToMultipleReceivers >/dev/null 2>&1
    if [ $? -eq 0 ]; then echo "第 $i 次: 绿"; else echo "第 $i 次: 红"; fi
done
```

⚠️ **池段前置**: 池段名 `__IPC_SHM__CHUNK_INFO__<class>` 全机共享, 清残段前必须先确认无活持有者:
`grep -l CHUNK_INFO /proc/[0-9]*/maps` 为空(协议见 `unfixed_defects.md` §4)。

## 8. 修法选项(⛔ 未授权, 仅供排期参考)

| 选项 | 内容 | 代价/风险 |
|---|---|---|
| A | 把 `hash<string>`/`hash<wstring>` 特化改成**按内容哈希**(如 `std::hash<std::string_view>{}(std::string_view{val})`) | 改动小, 但 `ipc::hash`/`ipc::unordered_map` 是 libipc 全局容器语义 ⇒ 需全套回归 + 重取树内指纹 + 评估是否让别的用例由绿转红 |
| B | 只把 `ipc.cpp:110`/`:301` 两处改用 `ipc::map`(有序树) | 局部, 不动公共 hash 语义; 但治标, `wstring` 的坑仍在 |
| C | 维持现状 | 数据正确, 仅性能/语义退化; 但 flaky 用例会持续污染回归判读 |

⛔ 本登记**不含**任何产品码改动, 也未授权任何选项。落笔后由决策方另行拍板。

## 9. 待办(给 writer-claude)—— 2026-09-19 全部办结

1. ✅ 台账任务总表已新增 UF-010 行(终态 `✅已修`, 不同于本文件原建议的 `⏸️待决策` ——
   决策方当日授权选项 A 并提交 `90510a5`);
2. ✅ 台账 §3 本体速览已补一句话说明(指向 `uf010_hash_consistency_fix.md`;
   本文件作为证据链原始记录保留);
3. —(§6 的"为什么以前没暴露"属推断, 未被要求坐实; 该推断不改变 §2/§3/§5 结论, 且修复落地后缺陷态已不存在, 坐实失去现实意义。)
