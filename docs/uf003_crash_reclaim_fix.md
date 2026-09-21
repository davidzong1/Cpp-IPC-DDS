# UF-003 修复: chunk 池无崩溃回收(死持有者清扫)

> 状态: ✅已修。指纹(证据规范, RELEASE_NOTES.md §4): tree_head=`94b01e2`+工作树
> (4 文件修改 + 2 文件新增未提交, 本文档随其一), libipc_md5=`bdefc741`
> (`build/lib/libipc.so.1.3.0`), 判据驱动 md5=`0657b7a7`(`test_uf003_crash_reclaim`)。
> 决策记录(互通性口径 / owner 身份 / 回收时机 / 射程 / 不做清单)见
> `docs/uf003_crash_reclaim_decision.md`; 台账 `unfixed_defects.md` UF-003 行。

## 1. 缺陷

chunk 池(`CHUNK_INFO__<chunk_size>__C<cap>`, 每档 40 块)是**共享段**, 块的占用状态
只由 `id_pool` 的空闲链与 chunk 头部的收方位图共同决定, 而位图里没有任何"持有者还
活着吗"的信息:

- 收方进程被 `kill -9` 后, 它名下的 chunk 位图位**永久悬挂**(没有析构、没有断连);
- `id_pool::release` 是唯一归还路径, 没有 owner/存活检查 ⇒ 那些块再也不回池;
- 池被逐块吃干后, 该尺寸档上**所有**话题的大消息永久退化为 TLV 分片拷贝:
  能跑、不报错、只是从零拷贝退化成逐片拷贝 —— 最坏的一类故障(静默降级)。

## 2. 修法(落点)

| 落点 | 内容 |
|---|---|
| `src/libipc/circ/elem_def.h` | `owner_table`(32 槽 × 8B 原子 u64, 位号=槽号)+ `proc_stat`(`/proc/<pid>/stat` 的 state/starttime 解析)+ `owner_maybe_alive`(三条件判死, 其余一律判活)+ `route_tag_of`(段名 FNV-1a) |
| `src/libipc/circ/elem_array.h` | 广播策略连接成功即 `owners_.claim(bit)`(声明失败回滚位); 断连**先清位后清槽**; 表追加在既有成员之后(不动任何既有偏移) |
| `src/libipc/ipc.cpp` | ① chunk 头部空洞内新增 `published`(1B, 偏移 4)与 `route_tag`(4B, 偏移 8), 配 static_assert 兜底; ② `reclaim_dead_chunks()`: 池穷尽时按"本路由 + 已发布 + 位图全死"三条件整块回收; ③ `acquire_storage` 借出即打标记/清 published, 穷尽时清扫 + 重试一次; ④ send/loan 发布成功后 `mark_published`; ⑤ push 失败 `return_unpublished`(归还未发布的借出块); ⑥ `QU_CONN__` 段名加 `__V2`(布局变了, 新旧二进制不混挂) |
| `src/libipc/sniffer.cpp` | `QU_CONN__` 段名逐字同步(⛔ 跨文件同步点) |
| `test/test_uf003_crash_reclaim.cpp` | 判据(新增) |

**为什么是 (pid, starttime)**: `/proc` 的 pid 会复用, starttime(启动节拍)保证"同号
pid"必是两个不同进程 —— 判死不会把新进程误判成旧持有者(见决策文档 §2/§7)。

**为什么整块放弃而不是清死位留活位**: 那需要新的位图仲裁协议; 池穷尽本身已有 TLV
回退兜底, 保守不发明新协议(决策文档 §4)。

**观测**: 复用 `note_pool_exhausted` 同款计数通道, 新增
`chunk pool reclaim: kind = …, chunk_size = …, reclaimed = N, count = …`(首报/每
1024 次节流), 与池穷尽日志并列。

## 3. 判据(test_uf003_crash_reclaim, 2 用例)

判据点只有 `loan` —— 它直接回答"池里还有没有可借的块"; `send` 在池空时静默退化为
TLV 分片并照样返回 true, 对本案**没有分辨力**。

| 用例 | 断言 | 实跑证据(stderr) |
|---|---|---|
| `DeadHolderIsReclaimedAtExhaustion` | ① 持有者活着 ⇒ `loan` 必须失败(绝不夺活人的块); ② `kill -9` 后 ⇒ 清扫让 `loan` 恢复, 且连续 3 轮借样/发布都成功 | `chunk pool exhausted: kind = loan, chunk_size = 21504 … count = 1` → `chunk pool reclaim: kind = loan, chunk_size = 21504, reclaimed = 40, count = 1` |
| `LiveCoHolderBlocksReclaim` | ③ 两个持有者只死一个 ⇒ 仍不得回收(整块放弃); 两个都死 ⇒ 才允许回收 | 两断言绿; 计数日志因首报节流不再打印(同进程内第 2 次) |

"②的因果是封闭的": 池里的块只有两条归还路(最后持有者的 `buff_t` 析构 / 覆写时
`discard`), 被 `SIGKILL` 的子进程两条都走不到, 且 40 条消息压在环里未环绕(不触发
覆写) —— 所以"loan 恢复"只能由清扫解释。

### 3.1 判据自身的构造纪律(踩过的坑, 已写进测试头注释)

1. **本档位池全机共享**(段名按 chunk_size 分档, 不带话题/进程分量): 别路由的残留块
   会让本测试借不到块, 而清扫**按设计只回收本路由的块**(位号只在同路由 owner 表里
   有意义)。故判据用**私有尺寸档**(20480 → 档 21504, 全仓无人使用), 并做池卫生前置
   探测: 可用块数 != 池容量即显式 skip 并说明(不给假绿, 也不给误红)。
2. **loan 与 send 的档位公式不同**: send 是 `calc_chunk_size(P)`, loan 是
   `calc_chunk_size(loan_size_class(P))` —— 只有 P 已是 `large_msg_align(1024)` 整数倍
   时两者同档。第一版取 20007(非对齐)导致"探测的池"与"发送占的池"不是同一个,
   判据以"活持有者没挡住 loan"的假红告终。
3. **不要把消息发得比池子多**: 池空后大消息退化为 TLV 分片(每片 1 环槽), 环灌满后
   `force_push` 覆写会按 rem_cc 归还那些块 —— 池便不再保持"被持有", 判据测不到清扫。
   只发**恰好打满池子**的条数。
4. **`clear_storage` 必须在同进程所有同路由 route 析构之后调用**(见 §7.2)。

## 4. 验证

- 专项 `test_uf003_crash_reclaim` **2/2 × 5 轮绿**(含 fork 子进程崩溃臂)。
- 回归面 **13 驱动全绿**: uf003_crash_reclaim 2、uf007 2、uf010 5、uf011 1、
  chunk_hold 3、lap_safety 3、pool_exhaust 2、loan 10、adopt_loan_quota 5、
  dzflat_transport 9、uf004_timeout_unlink 4、uf009 3、uf004_shutdown_monitor_optout 7。
- 途中两处**环境态**故障已归因并处置, 与代码无关:
  ① `test_loan`/`test_chunk_hold` 一度红 —— 是本判据早期版本(档 9216)留下的孤儿块
  把该档位池占满, **档位共享池的既有脆弱性**(它们假定池是干净的);按残池协议清掉
  `__IPC_SHM__CHUNK_INFO__9216__C40` 后立即全绿。
  ② 早期版本判据的两处假象(档位不匹配 / TLV 分片灌环)已固化成 §3.1 的构造纪律。

## 5. 已知边界(不是缺陷, 是明确的射程与残余泄漏)

1. **死路由孤儿块**(新登记 UF-013): 清扫靠 `route_tag` 校验"这块是不是本路由借出
   的"。若某路由崩溃后其路由段也被销毁(`clear_storage` / 无进程再 attach), owner 表
   随之消失, **其它路由无法验证那些位**, 于是那些块在该档位不能再被回收 —— 直到
   **同名路由再次出现**(同段名 ⇒ 同 tag ⇒ 新路由的第一次池穷尽即可回收它们, 实测
   每次运行本判据的卫生探测都能回收上一轮残留, 故 5/5 稳定绿)。
   实际影响有界(≤ 池容量/档位), 且生产里话题名稳定 ⇒ 下次启动自愈。彻底修法需要把
   owner 表按 tag 放进**池段**(跨路由可查), 属独立设计项, 不在本轮射程。
2. **published=0 的借出即暴死**: 借出后、发布前发布者整体死亡 ⇒ 该块不被清扫处置
   (published=0 保护发布者不踩 use-after-free), 悬挂到进程重启。上界: 每进程每档位
   至多 1 块(在飞只有一条)。
3. **单播不参与**: 单播的 cc 是计数语义, 位号无意义 ⇒ 不登记、不清扫(决策文档 §6)。
4. **`/proc` 隔离环境**(hidepid 等)整体退化为"不回收", 行为等于修复前, 无回归风险。
5. **用户态长期持有**(`try_get` 后的 Sample / `msg_queue_` 借样)与 UF-012 同一契约:
   不设上限, 靠池观测与 spilled 计数兜底感知。

## 6. 顺带收口

- `send`/`no_member_try_send` 的 push 失败路径此前**直接丢掉已借出的 chunk**(消息没
  进环 ⇒ 没有接收方会回收它 ⇒ 永久漏一块)。本轮加 `return_unpublished`: 位图归零 +
  还池, 语义与 `discard_storage` 同族(位图归零是还池的唯一授权)。
- `QU_CONN__` 段名的 `__V2` 版本分量, 顺带把档案里登记的"CHUNK_INFO/QU_CONN 无版本
  标记 ⇒ 跨版本混挂"缺口一起收口(UF-012 已用 `__C<cap>` 处理容量分量)。

## 7. 本轮附带的两项非本缺陷发现

### 7.1 尺寸档口径陷阱(测试侧, 非缺陷)

`loan` 与 `send` 算 chunk 档的公式不同(§3.1 第 2 条), 这是既有 API 的口径事实: 借样
请求的尺寸会被 `loan_size_class` 先对齐、再经 `calc_chunk_size` 加头对齐一次, 于是
非 1024 对齐的请求会**落到下一档**。借样与发送因此可能"同尺寸不同档"。已在判据文件
头注明, 供后续写池类判据的人避坑。

### 7.2 `route::clear_storage` 在路由存活时调用 ⇒ 该路由析构段错误(既有缺陷, 已登记 UF-014)

- 现象: `ipc::route tx{name, sender}; … ipc::route::clear_storage(name);`(tx 仍存活)
  ⇒ tx 析构时 `chan_impl::destroy → disconnect → waiter::wake → a0_mtx_lock` SIGSEGV。
- 矩阵实测(**当前构建**): `peer ∈ {none, alive, dead} × payload ∈ {small, big}` 全组合
  —— **只有 `clear=yes` 一列崩**; 与对端是否存在、是否有大消息、是否 `kill -9` 均无关。
- **归因: 干净基线复现** —— 从 `HEAD` 导出源码到 `/tmp/pristine` 单独构建(`libipc.so`
  与工作树版本相互独立), 同一矩阵结果逐格相同 ⇒ **不是本轮 UF-003 改动引入**, 是既有
  行为。
- 为何一直没被发现: 仓库里的测试一律在**测试开头**(此时无同路由活路由)调用
  `clear_storage`, 从不"先建路由 → 清段 → 再析构"。而档案里本来就写着
  `clear_storage` 会"打断任何正在用该段的进程"(test_pool_exhaust_observability.cpp 的
  注记) —— 本次判据第一版正好踩在这个未成文的纪律上(见 §3.1 第 4 条)。
- 处置: 本轮只把它**登记为 UF-014**并写进使用纪律(判据按纪律重排作用域); 是否修
  (例如 `clear_storage` 后把本进程已 attach 的 handle 标为失效并让析构短路)留给决策。

## 8. 边界与提交状态

- 本轮工作树改动与另一 Agent(UF-011 归还幂等守卫)完全分离: 其工作已随 `94b01e2`
  提交, 本轮 `git diff` 中 `note_double_return` 命中数为 **0**, 22 处 `UF-003` 标记
  全部属于本轮(4 改 + 2 新, 未提交)。
- 未提交: `src/libipc/{circ/elem_array.h, circ/elem_def.h, ipc.cpp, sniffer.cpp}` +
  新增 `test/test_uf003_crash_reclaim.cpp`、`docs/uf003_crash_reclaim_decision.md`、
  本文档。
