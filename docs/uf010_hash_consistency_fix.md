# UF-010 修复: 字符串哈希与容器相等性一致性

> 状态: ✅已提交(`90510a5`, 2026-09-19)· 判据全绿 · 变更面仅 `src/libipc/memory/resource.h`
> 关联: `docs/uf010_evidence_registration.md`(证据登记) · `test/test_loan.cpp:203`
> (端到端判据) · `test/test_uf010_hash_semantics.cpp`(语义层聚焦判据)

## 1. 一句话

把 `ipc::hash<ipc::string>` / `hash<ipc::wstring>` 从"按指针哈希"改为"按内容哈希"，
使哈希与 `std::equal_to<Key>`(按内容) 语义一致 —— 消除"同一逻辑键永不命中"导致的
重复 `mmap`/零拷贝地址分裂。

## 2. 根因与修法

`src/libipc/memory/resource.h` 的两个特化返回 `std::hash<char const *>{}(val.c_str())`,
而 `ipc::unordered_map` 的相等性用 `std::equal_to<Key>` —— 哈希按地址、相等按内容。
内容相同、地址不同的 key 哈希不同 ⇒ 落进不同桶 ⇒ `find()/operator[]` 永不命中已有
条目。实测后果(见登记文档 §4): chunk 池句柄缓存对同一 prefix 反复 `emplace` ⇒ 同一
shm 段在 `loan()`/`recv()`/`~buffer()` 各建一次映射, 从不 `munmap`; 多接收者零拷贝
广播拿到不同虚拟地址(`Loan.BroadcastToMultipleReceivers` 稳定红)。

修法(选项 A, 登记文档 §8): 按内容哈希, 用 `std::string_view`/`std::wstring_view`
覆盖 `[data(), data()+size())` —— 不拷贝、不看终止符, 空串/内嵌 `'\0'`/任意长度都
稳定等价。`wstring` 特化是同坑第二处, 一并修(无使用点, 零行为变化)。

未采用: 选项 B(改用 `ipc::map` 绕开哈希) 治标不治本 —— 缺陷留在公共特化里, 任何
后续 `ipc::unordered_map<ipc::string, ...>` 都会再中; 选项 C(维持现状) 让 flaky
用例持续污染回归判读。

## 3. 变更内容(逐处)

| 文件 | 变更 |
|---|---|
| `src/libipc/memory/resource.h` | +`#include <string_view>`; 两个特化体改 `string_view` 哈希 + 简短语义契约注释(12+/2−; 新增行不含尾随空白, `git diff --check` 干净) |

无其它产品码、无接口/构建改动。改动为 header-only 内联, 重编 `libipc.so` 即生效。

## 4. 兼容性与影响面(已核)

- `ipc::hash` / `ipc::unordered_map` 定义在 `src/libipc/memory/resource.h`, **不是安装头**:
  `include/libipc/` 下无任何头引用它(全部引用者都在 `src/` 内); 对库外用户无 API/ABI 影响。
- 全仓 `ipc::hash` 直接使用点: 仅该头自身 + `docs/probe_hash_semantics.cpp`。
- 全仓 `ipc::unordered_map` 使用点: `src/libipc/ipc.cpp:110`(`cc_acc` 的 `handles`)、
  `:301`(`chunk_handle_t::handles_`) —— 正是修复目标; 其余 `unordered_map` 均为
  `std::unordered_map`(整数/指针键, `data_rev.cc`、`local_pub_sub_registry.h`), 不受影响。
- `ipc::map`(有序树)不碰哈希, 不受影响。
- ⚠️ 行为变化(1 处): 运行期**外部删除/重建**池段后, 已缓存 handle 仍指向旧 inode ——
  本进程不会像旧行为那样"下次调用重新 `shm_open` 拿新段"。清残池协议本来就要求
  "先确认无活持有者再删"(见 `unfixed_defects.md` §4), 遵守协议时无差异; 线上误删
  段的恢复手段是重启进程。跨进程池共享/回收(UF-003)不在本修复射程, 行为不变。

## 5. 验证证据

三方指纹(规范见 `RELEASE_NOTES.md`, 0918 起):

| 键 | 缺陷态 | 修复态(本次) |
|---|---|---|
| `tree_head` | `8700215` | `90510a5`(修复已提交) |
| 源 `resource.h` | `50130d80785786439d8d4ab20122efe6` | `44815addce998b596ba4455625baa026` (注释精简/行尾收口定稿; 哈希实现代码逐字节不变, 编译产物与前一落码版相同) |
| `libipc_md5` | `88fb143b56f4ee9c4074fd07ad6b1999` | `fe0a983f85ba1b954261556b1d8c062a` |
| `driver_md5` `test_loan` | `4fd2882dfe441ed507d98f93902fb42f` | 同左(源未变) |
| `driver_md5` `test_uf010_hash_semantics` | — | `0e0f893eecf39595564d9e685390e162` |

命令与读数(`LD_LIBRARY_PATH=$PWD/build/lib`):

| 实验 | 缺陷态 | 修复态 |
|---|---|---|
| `test_loan --gtest_filter=Loan.BroadcastToMultipleReceivers` 单跑 | 红 10/10 | **绿 20/20** |
| `test_loan` 全量(10 用例) | 红 3/3 | **绿 5/5** |
| `test_uf010_hash_semantics`(5 用例: 4 语义 + 1 映射计数) | 红 4/5(变异态; 见下) | **绿 5/5** |

- **变异验证**: 把两个特化临时改回 `std::hash<char const *>{}(val.c_str())`(与 HEAD
  原实现逐字节相同) ⇒ `test_uf010_hash_semantics` 4/5 红(含映射计数用例:
  loan/recv 后新增 2 个映射而非 1 个)、`test_loan` 广播用例红 3/3; 恢复后全部转绿。
  两项判据均有判别力, 非假日绿。
- **回归面(全部绿, 修复态)**: `test_loan` 10、`test_uf010_hash_semantics` 5、
  `test_chunk_hold` 3、`test_lap_safety` 3、`test_pool_exhaust_observability` 2、
  `test_circularqueue` 5、`test_ipc` 8、`test_mem` 0、`test_dzipc` 8、
  `test_dzipc_shm` 10、`test_dzflat` 13、`test_dzflat_transport` 9、`test_dzflat_rx` 8、
  `test_dzipc_larger_data_shm` 1、`test_shm` 8、`test_shm_nodelet` 13、`test_complex_msg` ✔。
- 探针佐证: `docs/probe_hash_semantics.cpp` 在修复态输出 `hash相等? 1` 与
  `map.size()=1`(登记文档 §4.2 的缺陷态读数 `hash相等? 0` / `size()=2` 由此订正)。

**提交态复跑对账(2026-09-19 收尾闭环, `90510a5`)**: 三方指纹与登记值逐位一致
(源 `resource.h` = `44815add…`, `libipc_md5` = `fe0a983f…`, 两驱动 md5 同上表)。
上表"回归面"18 个驱动全量复跑, 通过数逐个与登记值相同, 全绿;
`Loan.BroadcastToMultipleReceivers` **单跑 10/10 绿**(缺陷态曾红 20/20)。
台账已落笔: `unfixed_defects.md` 任务总表新增 UF-010 行(✅已修)与新登记 UF-011
(池空闲链二次入池 —— 与本修复无关的既有回收竞态, 见 shm_chunk_pool_occupancy_plan.md
§3 步骤③ 副产品);UF-007 行已补 UF-011 交叉注记。

## 6. 回归器(防回退)

- 语义层: `test/test_uf010_hash_semantics.cpp` —— 同名内容哈希一致、不同内容散开、
  `unordered_map` 按内容去重、wstring 同语义、同一 CHUNK_INFO 段 loan→recv 只映射一次。
- 端到端: `test/test_loan.cpp` 的 `Loan.BroadcastToMultipleReceivers`
  (`g1.data() == g2.data()`), 缺陷态稳定红、修复态稳定绿。
- ⛔ 任何让 `hash<string>`/`hash<wstring>` 回到指针语义的改动, 上述两组必须转红。

## 7. 风险与残余

1. 外部运维误删在用池段 ⇒ 本进程与新进程段视图分裂(§4 行为变化条); 遵守清池协议
   或重启进程即可, 属既有运维约束。
2. 哈希算法不对外承诺保值: 无落盘键、无跨版本哈希(容器为进程内缓存), 升级无需迁移。
3. 残余不修: `ipc::unordered_map` 之外的哈希使用(无); `hash<string>` 语义变更影响
   的其它容器(无使用点)。
