# 未修缺陷登记(待处理)

> **状态**: 逐条状态见下方「冻结任务清单与执行协议」0.1 表(取值 `⬜未修` / `🔶部分修` /
> `⏸️待决策` / `✅已修` / `❌已作废`)。本文档只登记与派工, **本阶段不动代码**。
> **编写日期**: 2026-09-17
> **来源**: 2026-09-17 偶发 `SIGSEGV at 0` 排查(修复见
> [shm_defect_fixes.md](shm_defect_fixes.md) §7)过程中发现、但**没有**随那一轮修掉的问题,
> 加上同族扫描后新确认的几处。每条都是"当时判断不该顺手改", 不是"没看见"。
> **与 [shm_defect_fixes.md](shm_defect_fixes.md) 的分工**: 那份是**已修**条目的施工记录
> (每条都有行为判据 + 变异验证); 这份是**未修**条目的登记表 + 修法选项。两份都要各自维护,
> 修完一条就从这里删掉、到那份加一节。
> **怎么用**: ⛔ **先读「冻结任务清单与执行协议」(紧接本块之后的 `## 冻结任务清单与执行协议`)** ——
> 那是 2026-09-17 冻结的派工与验收台账(`UF-000`–`UF-009` 缺陷 + `DF-001`–`DF-005` 设计待办, 含状态/
> 负责人/依赖/修改边界/验收命令/
> 变异验证/写回字段与执行顺序)。下面 §1–§5 是**缺陷本体**(症状 → 成因(file:line) → 触发条件 →
> 为什么当时没修 → 修法选项 → 判据(怎么算修好了) → 复现方法)。第 6 节是共用的排查手段
> (分配失败注入 / ASAN 探针 / 残池清理), 第 7 节是建议的处理顺序(**已被 0.2 取代, 仅作理由记录**)。
> **引用约定**: 本文中单独出现的 **§7** 一律指 [shm_defect_fixes.md](shm_defect_fixes.md) 的
> 第 7 条(2026-09-17 那次偶发 SIGSEGV 的修复记录), 不是本文档自己的第 7 节。

---

## 冻结任务清单与执行协议(2026-09-17 冻结)

> **基线**: HEAD = `3818899`, 冻结时工作区干净。**行号为本轮快照, 落地后须按 md5 重核**
> (冻结时未取 md5 —— 标 `待核` 的锚点不得当已验证引用)。
> **本节的地位**: 它是**唯一权威的派工与验收台账**; 下面 `## 1.`–`## 5.` 是缺陷本体
> (诊断与修法论证), **编号保持不变**(全文对 "§1"…"§5" 的交叉引用继续有效)。
> 本节用**两个**独立命名空间: **`UF-*`**(未修**缺陷**, `UF-000`–`UF-009`)与 **`DF-*`**
> (**设计待办**, `DF-001`–`DF-005`)—— `DF-*` 不是缺陷, 分开编号的理由见 0.6 第 12 条。
> 两者都与 [shm_defect_fixes.md](shm_defect_fixes.md) 的 1–7 号 **不重号**
> (那个文档的 "§7" 已被占用为固定引用)。
> **状态取值(固定五种)**: `⬜未修` / `🔶部分修` / `⏸️待决策` / `✅已修` / `❌已作废`。
> **单写者**: ⛔ 任何人**不得**直接编辑本文档 —— 条目变更一律提交 `writer-claude` 落笔。
> 上一轮已有并发编辑致结论作废的先例(`server_ipc.cc` / `test_dzipc_log.cpp`, 2026-09-17)。

### 0.1 冻结任务表

| ID | 标题 | 状态 | 严重级 | 负责人 | 依赖 | 可并行 | 修改边界(路径) | 本体章节 |
|---|---|---|---|---|---|---|---|---|
| **UF-000** | 分配失败注入工装(harness) | ✅已修(**工装可用**; 写回见 0.3.2) | 高(**前置**) | tester-claude | 无(它是别人的前置) | ✅ 与 UF-004/009 实验并行 | 仅新增(`tools/**` + `test/**`); ⛔禁改 `src/libipc/**` 与任何产品码 | 本文 §6.1/§6.2 **+ 0.3.1(验收字段补齐)** + 0.3.2(写回) |
| **UF-001** | allocator `noexcept`+返回 nullptr 放大器 | ✅已修(**选项 1 已落码**: 去 `noexcept` + 越界 `length_error` + 底层失败 `bad_alloc`; 写回见 0.3.6) | 高 | coder-claude | ~~**UF-000**~~ ✅ 已满足(2026-09-18, 见 0.3.2) ⇒ **判据已解锁** | ✅ 与 UF-004/006/005 并行; ⛔与 UF-002 串行 | `src/libipc/memory/allocator_wrapper.h`(+`include/libipc/pool_alloc.h`) | §1 **+ 0.3.6(写回)** |
| **UF-002** | pimpl `make()` 空解引用(含 `valid`/`release`) | ✅已修(**失效态判空已落码**: 五类 pimpl 入口 + 析构判空; ⚠️ `buffer.cpp:59` 已于 2026-09-18 被改动但**回滚变异阴性** ⇒ ⛔**不计入修复**, 且该文件**不在本行 `修改边界` 列** ⇒ 保留/回退**待 leader 裁** —— 见 0.3.7 限定 1 与 0.3.8(7); 写回见 0.3.7) | 高 | coder-claude(实现)+ reviewer-claude(语义) | ~~**UF-000**~~ ✅ 已满足(2026-09-18) ⇒ **判据已解锁**; 与 UF-001 串行 | ✅ 与 **UF-007**/UF-004/UF-006 并行; ⛔与 UF-001 串行 | `src/libipc/{shm.cpp, socket/udp.cpp, sync/mutex.cpp, sync/condition.cpp, sync/semaphore.cpp, utility/pimpl.h}` | §2 **+ 0.3.7(写回)** |
| **UF-003** | chunk 池无崩溃回收 → 残池污染 | ⬜未修(⛔reviewer 裁定**不可先做**, 见 0.3.8(1)) | 中高 | 待裁(先拍互通性) → coder | **互通性决策**(⛔未拍 ⇒ 阻断) + ~~**UF-007 必须先修**~~ ✅ **已于 2026-09-18 消解**(UF-007 判 `❌已作废`, 不再是前置, 见 0.3.3) | ✅ 与 UF-001/002/004/006 并行; ~~⛔与 UF-007 串行且在**其后**~~ ⛔**该串行约束已随 UF-007 作废而失效**(2026-09-18 订正, 见 0.3.8(1)) | `src/libipc/utility/id_pool.h` + `src/libipc/ipc.cpp`(chunk_info_t **:258**) + **`src/libipc/sniffer.cpp`**(chunk_info_t **第二份定义 :89-101**, 2026-09-18 补 —— 原边界**漏了它**, 见 0.3.8(1)) —— ⛔改共享段布局 = 与旧版本不互通 | §3 |
| **UF-004** | 库接管进程退出(信号 + `std::exit(0)`) | 🔶部分修(**opt-out 开关已落码并验收 2026-09-18** —— ⛔ **不是** `✅已修`; 五个口径必须分开读: **①强制性已解除**(应用可在首个 IPC 构造前自行接管退出) **②opt-out 路径实测归零**(`app_handler_alive` + `residue_own=0`) **③默认路径逐位不变**(factory 仍 `library_exit0_fast`, **残留 own=17 属预期而非回归**) **④晚调用返 `false`**(已接管则不可撤销且行为不变) **⑤无自有处理器的应用开 opt-out = `SIG_DFL` 硬杀**(rc=143/`by_signal=15`, 比现状**更差**) —— ⇒ 修复是**按应用分类有条件**的; ⛔ **不改 UF-009**(默认残留**不归零**); ⛔ **Python 面不可达**; 写回见 0.3.12) | 中(设计风险, **已实测坐实** —— 机制链已由 LD_PRELOAD 仪器实测钉死) | coder-claude(落码 ✅ seq156/158) + tester-claude(验收 ✅ 28/0 seq159) + reviewer-claude(架构门 ✅ seq155 / 实现门 ✅ seq157) | ✅ 已满足(reviewer 唯一推荐的 **(b) opt-out** 已由 leader 拍定并落地; 实现 + 独立验收均已完成) | ✅ 与 UF-009 同批; 与其余全并行 | `src/dzIPC/dzipc.cc` + `include/dzIPC/dzipc.h`(+`test/test_uf004_shutdown_monitor_optout.cpp`、`tools/sercli_live_probe/{sercli_live_driver.cc,exit_semantics_matrix.sh,uf004_optout_acceptance.sh}`); ✅ **`docs/dzipc_log.md:100` 已于本轮同步** | §4 **+ 0.3.8(2)(裁定) + 0.3.12(🔶写回; 指针行)** |
| **UF-005a** | 组播组地址碰撞(8192 topic 60% 共组) | ⏸️待决策(**先拍兼容性**) | 高 | 待裁(先拍兼容性) → coder | **兼容性决策**(与 005b 同一次) | ⛔ 与 UF-005b 同批决策 | 端口公式; ⛔不动 wire、不动组分配 | §5 第 1 行 |
| **UF-005b** | 端口公式乘性冲突 | ⏸️待决策(**口径打架**) | 中高 | 待裁 | 同 UF-005a 的决策 | ⛔ 与 UF-005a 同批 | 同 UF-005a | §5 第 2 行 |
| **UF-006** | `ipc_info_pool` 表满静默 `-1` + "无周期性回收"(⛔**该措辞已订正**, 见 0.3.8(4)) | ✅已修(**表满路径回收死条目**已落码 `ff125bb3…` 并**实测 10/10 PASS + 变异三臂转红**; ⚠️**措辞订正**: 回收**只在表满路径**, 池未满时死条目**不会**被下一次注册回收; 详情已按 0.4 第 3 步搬到 `shm_defect_fixes.md` **§ 修复记录 14** —— 见 0.3.11) | 中高(静默降级) | coder-claude(落码 ✅ seq150) + reviewer-claude(回收边界 ✅ 0.3.8(4); 验收门 ✅ `PASS` 0.3.10(c)) + leader(**✅ 2026-09-18 已拍"用例落进 `test/` 在授权内"**) | ✅**前置全解除**(0.3.10(e) 四条件 2026-09-18 **全满足** —— 逐条见 0.3.11(a)); ⚠️ 0.5 第 2 条**未单独拍板**(以写回指令为事实认可, 见 0.3.11(d)) | ✅ 独立性最高 | `src/dzIPC/ipc_info_pool.cc`(+`include/dzIPC/ipc_info_pool.h`); ⛔不改判定路径的 `gc_dead=false` 语义; ⛔**不改段布局/ABI**: 不新增不重排 `PoolEntry` 字段、不改 `kRegionSize`/`kMaxEntries`(=512)、不换 `kShmName`; ⛔不改 `register_entry` 返回码(只补日志) —— 本轮实测头文件 md5 与 HEAD 同为 `e9763c22…` | 0.3.9(落码登记) + 0.3.10(验收证据) + **0.3.11(✅写回; 指针行)** |
| **UF-007** | 重复归还 → `id_pool` free-list 自环 | ❌已作废(**登记点过期 / 触发路径不可达 / §8 守卫已成立**; ⛔**不是"已修"**, 见 0.3.3) | 高(**污染 UF-003 判据**) | coder-claude + reviewer-claude | 无(~~必须先于 UF-003~~ ⇒ 已作废, **不再是 UF-003 的前置**, 见 0.3.3) | ✅ 与 UF-001/002/004/006 并行; ~~⛔与 UF-003 串行~~ ⛔**已作废 ⇒ 该串行约束失效**(与 UF-003 行同步订正, 2026-09-18) | `src/libipc/ipc.cpp` 的 `pop()` 侧 + `test/test_chunk_hold.cpp`; ⛔优先不动 `id_pool` 结构 | **本节新增**(仅登记在测试注释与 dzflat_shm.md §9.1) |
| **UF-008** | sniffer 判据缺陷 | ⏸️待决策(**仅证据, 不授权改产品**) | 中(影响判据判别力) | analyst-claude(证据)→ reviewer-claude(判) | 无 | ✅ | ⛔本轮不授权改**产品码**(证据采集为只读); 若判成立, 限 `exec/dzipc_topic_cat/**` + `test/test_handshake_probe.cpp` | 见共享区 `a1_a2_criterion_defect_sniffer_residue_analyst-claude.md` |
| **UF-009** | factory 路径残留段 + 缺 `SERVER SUMMARY` | ⏸️待决策(**证据✅完成 / 机制已观察; 仍不授权改产品码**; 写回见 0.3.5) | 中(**已判定: 非崩溃 / 非挂起**; 残留 17 段与缺 `SERVER SUMMARY` 已坐实) | tester-claude(复现+采集退出码 ✅) | 与 UF-004 同链、同批 | ✅ 与 UF-004 并行 | ⛔不授权改**产品码**; ✅**授权**改工具脚本/测试(`tools/sercli_live_probe/**`、`scripts/**`)以采集退出码与残留段计数 | 见 §4 活体指纹 + `build/live_runs/20260916_153546` |
| **DF-001** | DZFlat 库级默认 **OFF** 的灰度约束(未升级订阅方静默丢段 ⇒ schema 演进由"向前兼容"变"硬同步") | ⏸️待决策(**设计待办**) | 中(发布节奏/互通) | 待裁 → coder | **兼容性决策**(建议并入 UF-005a/b 同一次, 见 0.5 第 7 条) | ✅ 与全部缺陷项并行 | ⛔本轮不进产品修复; 立项前须出路径清单(**UNVERIFIED**) | §5 第 3 行 |
| **DF-002** | 无 per-topic / per-publisher 的 DZFlat 开关(只有进程级 `EnableDzFlat`) | ⏸️待决策(**设计待办**) | 低中(能力缺口) | 待裁 → coder | 同 DF-001 的决策 | ✅ | 同上(⛔本轮不进产品修复) | §5 第 4 行 |
| **DF-003** | 订阅侧**无**反向开关(恒双 wire): 无法表达"这个订阅者不要借样" | ⏸️待决策(**设计待办**) | 低中(能力缺口) | 待裁 → coder | 同 DF-001 的决策 | ✅ | 同上(⛔本轮不进产品修复) | §5 第 5 行 |
| **DF-004** | 同机多订阅者共享一个 SHM 入口 | ⏸️待决策(**设计待办**) | 中(容量/扇出, 未测) | 待裁 → coder | 其设计文档**未实施**(`local_shm_fanout.md` 第 3 行) | ✅ | 同上(⛔本轮不进产品修复) | §5 第 6 行 |
| **DF-005** | Python 发布端仍有**一次 memcpy**(段在 Python 地址空间生成) | ⏸️待决策(**设计待办**) | 低(性能里程碑) | 待裁 → coder | 无 | ✅ | 同上(⛔本轮不进产品修复) | §5 第 7 行 |

> ⚠️ **`DF-*` 是"设计待办", 不是缺陷** —— 与 `UF-*`(未修缺陷)分开编号, 理由见 0.6 第 12 条。
> 它们**本轮不进产品修复**, 状态一律 `⏸️待决策`; **后续入口 = 0.5 第 7 条**(须与 UF-005a/UF-005b 的
> 兼容性拍板同批)。**不进台账 ≠ 不在范围**: 本节 0.9 一览第 5 行与本表**一一对应**, 两处必须同步。

### 0.2 执行顺序(冻结)

```
清单冻结(本文) ──→ UF-000 ✅已修(2026-09-18, 见 0.3.2)
   ├─ 并行: UF-004 / UF-009 判定实验(同批次, 成本最低, 可解锁 UF-009)
   │     ✅ 2026-09-18 已完成(见 0.3.4 / 0.3.5): UF-004 假设(C)坐实, **缺陷本体仍未修**;
   │        UF-009 证据完成、机制已观察, **产品码仍不动**。
   ├─ UF-001 ──→ UF-002   ← 前置 UF-000 已满足 ⇒ **判据已解锁**, 可开工
   ├─ ~~UF-007 ──→ UF-003~~  ← UF-007 判 `❌已作废`(触发路径不可达, 见 0.3.3) ⇒
   │     **UF-003 不必再等它**, 唯一前置只剩「互通性决策」
   ├─ UF-006(独立, 随时)
   ├─ UF-005a / UF-005b: 先拍兼容性决策, 再实现
   └─ DF-001–DF-005(§5 第 3–7 行): 设计待办, ⏸️待决策, ⛔本轮不排工
      后续入口 = 0.5 第 7 条(与 UF-005a/b 的兼容性拍板同批)
```

**与本文 §7 旧顺序的差别(旧顺序已被本节取代)**: §7 写 "§1(放大器)先做", 理由是"让后面所有分配
失败变成可观测错误"。该理由在 §7 的 UAF 修掉后**触发面已缩小到只剩真 OOM**(见"订正记录"第 9 条),
且**没有 UF-000 时 UF-001/UF-002 的判据根本不可执行** —— 只能验"不注入时全绿", 那是**回归不是判据**。
故 §1/§2 排到 UF-000 之后, 而 UF-004/UF-009 的判定实验**不需要任何产品改动**、今天就能跑, 前置成本最低。

### 0.3 横切约束(影响每一条的验收命令, 冻结)

1. ⛔ **验收命令不得假定 CTest 已注册**。实测 `test/CMakeLists.txt` 内 `add_test` **仅 1 条**
   (`test_udp_port_boundary`, `:125`); `enable_testing()` 已在根 `CMakeLists.txt:129` 开 ——
   是**没登记**而不是没开。统一命令形态:
   ```bash
   cmake -S . -B build && cmake --build build -j8 --target <target> \
     && ./build/bin/<target> --gtest_filter='<Pattern>'
   ```
   且 `test/CMakeLists.txt:23-33` 用 `file(GLOB)` ⇒ **新增/删除 `test_*.cpp` 后必须先重跑
   `cmake -S . -B build`**, 否则改了源码跑的是旧二进制(本仓已吃过这个亏)。
2. ⛔ **`test/test_mem.cpp` 是空二进制**(三段 `TEST` 全被注释, 零活跃用例) ⇒ **不得**用它当
   内存类改动(UF-001/UF-002)的验收项。
3. ⛔ **F4 红线**: UF-001 / UF-002 / UF-007 **全部落在 `src/libipc/**`**, 与 F4 契约
   (公共 libipc 默认 `open_or_create` 不得变更)的既有"零改动"观测会冲突 ⇒ 每条 libipc 改动后
   **重跑 F4 红线检查**。
4. **池类判据(UF-003 / UF-007)前置**: 先按本文 §6.3 清残池建立基线, 否则红绿不可解释。
   `test_chunk_hold.cpp` 的 `:175` 刻意吃干池子 ⇒ **红灯不自动等于修复失败**。
5. **每条必须附变异验证**(把修复回滚后哪条断言转红)。这是 [shm_defect_fixes.md](shm_defect_fixes.md)
   的既定口径: **行为判据 + 变异验证, 缺一不算闭环**。

### 0.3.1 UF-000 的验收字段(补齐; 评审 seq120 B4)

UF-000 交付的是**工装**, 不是产品改动 ⇒ 0.3 第 1 条的统一命令模板(`./build/bin/<target>`)对它**不适用**
(§6.2 的 `probe_asan` 是 CMake 树外手工编译)。它是 0.3 第 5 条「每条必附变异验证」的**唯一例外**,
所以必须**自带**下面三条; 缺任一条即按 0.3 第 5 条判**未闭环**:

1. **工装自身的构建/运行命令(可直接粘贴的两条)** —— ①工装的构建命令; ②**被注入进程**的启动命令
   (含尺寸档 `[lo,hi)` 与起始调用计数 `N` 的实际取值)。⛔ **不得**写"用 §6.2 的办法编译"这类转述,
   必须给出实际落盘路径与形参。UF-001/UF-002 的验收命令**都引用这两条**, 不是各自重写。
2. **阳性对照 —— 工装有效性的唯一证据** —— 在**已知含缺陷的旧态**上跑同一条命令, 目标用例
   **必须崩**(或必须出现预期的注入报错); **崩不了 ⇒ 工装没生效**, 此后所有"注入后全绿"都是**假绿**。
   团队先例: analyst 已实测 followup §B 的 A1 **恒绿** —— `exec/dzipc_topic_cat/main.cc:133-152`
   在 `sel.found==false` 时直接 `return`, 根本走不到被测代码, 故"A1 绿"**无判据力**。
   ⇒ 工装与后续判据用例**都**要先过阳性对照。对照对象的选取见 **0.5 第 8 条**(待裁)。
3. **注入可观测性(尺寸档拦截计数)** —— 工装须自报"尺寸档 `[lo,hi)` 拦下 **N 次** / 本进程累计拦下 N 次",
   并写到用例输出或 stderr。⛔ **没有这个计数, "没注入"与"注入了但代码正确"不可区分** ——
   UF-001/UF-002 的判据会退化成"不注入时全绿", 而那是**回归不是判据**(同 0.2 的理由)。

> 这三条是**交付内容**, 不是新的产品改动 —— 落实位置仍是 0.1 表 UF-000 的修改边界
> (`tools/**` + `test/**`, ⛔禁改 `src/libipc/**`)。UF-000 完成时按 0.4 协议写回:
> 状态列改 `⬜未修 → ✅已修` 前, 必须先贴**阳性对照的实测红**(命令 + 输出)。
> ✅ **2026-09-18 已兑现** —— 阳性对照实测红(rc=139)与三条交付内容见 **0.3.2**。

### 0.3.2 UF-000 写回(2026-09-18; 0.3.1 三条已交付, 残余见末段)

> 本节是 0.4 协议第 2 步要求的"条目块"(命令 / 结果 / 日期 / 变异验证)。详情已按第 3 步搬到
> [shm_defect_fixes.md](shm_defect_fixes.md) **§ 修复记录 8**; 0.1 表 UF-000 行保留指针。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`(log 步骤 0)。
**产物**: 工装 `tools/alloc_fault_inject/`; 判据用例 `test/test_alloc_fault_inject.cpp`(3 用例)。
**本轮**没有改任何产品码(`src/libipc/**` 零改动), 也没有改测试 —— 全是**新增**。

**执行的命令(可直接粘贴)**

```bash
# ① 工装的构建(0.3.1 第 1 条之①)
tools/alloc_fault_inject/build.sh
# ② 被注入进程的启动(0.3.1 第 1 条之②) —— 不给档位则**自标定**
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  tools/alloc_fault_inject/bin/fi_positive_control
#    显式档形式: fi_positive_control <lo> <hi> [skip_n] [mode]   (mode = dtor|valid|acquire)
# ③ 判据用例(先重配以让 file(GLOB) 收进新 .cpp, 见 0.3 第 1 条)
cmake -S . -B build && cmake --build build -j8 --target test_alloc_fault_inject
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so ./build/bin/test_alloc_fault_inject
# ④ 一键重放上面全部 + 阴性对照 + 直方图 + git diff --check(artifact = /tmp/uf000_acceptance.log)
tools/alloc_fault_inject/run_acceptance.sh
```

**结果(逐条 rc, 取自 artifact)**

| 步 | 动作 | rc | 关键读数 |
|---|---|---|---|
| 1 | `build.sh` | **0** | 产出 `.so` + `fi_selftest` + `fi_positive_control`(1 条 `-Wcomment` 告警, 无害) |
| 2 | 带注入跑 `fi_selftest` | **0** | 17 条 `SELFTEST ok`(9+5+3 三个阶段) + `SELFTEST RESULT: PASS` |
| 2b | **不带**注入跑 `fi_selftest` | **2** | `SELFTEST BLOCKED: 注入工装未加载(缺 LD_PRELOAD) —— 未执行任何断言, 不得当通过` |
| 3 | **阳性对照** `fi_positive_control`(dtor, 自标定) | **139** | 自标定 `sizeof(handle_)=64`; 宽档 `[1,4096)` 内构造期分配=1; 注入档 `[64,65)`; 实际拦截=1; `~handle` 处 SIGSEGV |
| 3b | 显式档 `[0,0)` mode=valid | 3 | 0 命中 ⇒ 工装**自报** `尺寸档未命中 ⇒ 工装没生效(不得当通过)` |
| 3c | 显式档 `[56,57)` skip_n=0 mode=dtor(**辅助档**) | 3 | **未命中**(`band_calls=0 hits=0`) —— 见下方"限定与遗留" |
| 3d | 直方图步(`DZIPC_FI_HISTOGRAM=1`, 自证 56=sizeof) | 139 | 崩在**打印直方图之前** ⇒ 该步**未产出直方图**, 尺寸自证实际来自步骤 3 的自标定行 |
| 4 | `cmake -S . -B build` | **0** | GLOB 收进 `test_alloc_fault_inject`, 新用例入树 |
| 4 | `cmake --build … --target test_alloc_fault_inject` | **0** | 链接成功 |
| 5 | **注入态跑判据用例** | **139** | 3 用例中 `DisabledHarnessDoesNotIntervene` OK; `HandlePimplAllocFailureMustNotCrash` **SIGSEGV** ⇒ **UF-001/UF-002 的实测红** |
| 5b | **不带注入**跑判据用例 | 0 | `[ SKIPPED ] 3` + `[ PASSED ] 0` ⇒ **不是绿**(0.3.1 第 2 条) |
| 5c | 注入态只跑健全性用例 | **0** | `[ PASSED ] 1`(证明注入态下进程本身可正常运行) |
| 6 | `git diff --check` | **0** | 无空白错误 |

**变异验证(0.3 第 5 条要求的那一条)**

1. ⛔ **去掉 `LD_PRELOAD` ⇒ 判据用例必须 SKIP/BLOCKED, 不得绿** —— 实测步骤 5b: `3 SKIPPED / 0 PASSED`;
   工装自测步骤 2b: `BLOCKED` + rc=2。⇒ **"注入后全绿"这条假绿路径已被堵死**。
2. ✅ **旧态(未修)阳性对照必须崩** —— 实测步骤 3: `rc=139 SIGSEGV`; 判据用例本身步骤 5: `rc=139`。
   两条互相独立(前者是专用复现程序, 后者是被测断言)。
3. ✅ **注入可观测性(0.3.1 第 3 条)** —— 工装退出时**总是**打一行汇总:
   `[alloc_fault_inject] FINAL active=… band=[lo,hi) skip_n=… max_fails=… total_calls=… band_calls=… skipped=… hits=…`;
   阳性对照额外打印 `实际拦截次数=N`。⇒ "没注入"与"注入了但代码正确"**可区分**。

**限定与遗留(⛔ 不得当已全部闭环读)**

1. ⚠️ **0.3.1 第 2 条字面要求的"回滚修复态 ⇒ 某已知用例转红"本轮不适用**: UF-001/UF-002 **尚未修**,
   没有"修复态"可回滚。本轮给出的是**等价证据** —— 在**未修态**上判据用例本身 rc=139(实测红) +
   独立复现程序 rc=139。⇒ **修完 UF-001/UF-002 后仍须补一次真正的回滚变异验证**, 本次不算已做。
   ✅ **2026-09-18 已兑现**(coder seq135; writer 写回): UF-001/UF-002 双双落码(`UF-001` 选项 1、
   `UF-002` 失效态判空), **两项各自独立回滚后分别 rc=139** ⇒ 本条要求的"真正的回滚变异验证"
   **已补, 不再是遗留**。写回见 **0.3.6** / **0.3.7**, 详情见 [shm_defect_fixes.md](shm_defect_fixes.md)
   § 修复记录 12 / 13。⚠️ 本项**只**解掉本条遗留, **不**动 0.3.1 的其余各条(限定 6 的两处过期数字
   `run_acceptance.sh:47-48`、`fi_positive_control.cc:22` **仍在**)。
2. ⚠️ **辅助档 `[56,57)` 未命中(步骤 3c, rc=3)** —— 如实记录, **不夸大**: `56` 是
   `tools/alloc_fault_inject/README.md` §1 示例里写死的 `sizeof(handle::handle_)`(**推算值**), 而本轮
   **实测自标定为 64** ⇒ 该示例**已过期**。后果有限: 承重路径是**自标定**, 步骤 3 命中且唯一。
   ✅ **2026-09-18 已订正**(writer, 单写者): README §1/§2/§3 三处写死尺寸全部改为**自标定调用**,
   并加了一张"尺寸取值表"(旧值 `56` 明确标为 **❌过期·推算·从未实测**, 新值 `64` 标为 **✅2026-09-18 实测**);
   同时点明 `rc=3`(档未命中)与 `rc=4`(没崩 ⇒ 可能已修)**含义相反, 别读反**。
   文件 133 → **159** 行, md5 `0ae3ae57413e74efe9be0c3ec3d0ab30`。⚠️ 同一数字在
   `run_acceptance.sh` / `fi_positive_control.cc` 里**仍在**(见限定 6)。
3. ⚠️ 步骤 3d 的**尺寸直方图本轮未产出**(进程在退出打印直方图之前就崩了)。它是 0.3.1 第 3 条的
   **辅助**自证手段; 拦截计数本身已由 FINAL 行提供。⇒ 不得把 3d 记成"直方图已验证 56"。
   ⛔ **2026-09-18 已定位根因 —— 它不是"运气", 是结构性的**: `run_acceptance.sh:50-53` 给该步**传了
   argv** ⇒ `fi_positive_control.cc:93-97` 走**显式档**分支, 而 `:121` 把 `max_fails` **写死为 1**,
   于是 `DZIPC_FI_ALLOC_MAX_FAILS=0`("只计数不拦截"的宽档)被**覆盖**; 第 1 次档内分配就被拦空 ⇒
   当场崩, **永远走不到退出时的直方图打印**。artifactId 里那行 `max_fails=1`(而不是 env 给的 0)
   就是这次覆盖的实证。⇒ 该步**在修好之前, 每轮重放都产不出直方图**。
4. ⚠️ **工装覆盖边界**(README §5): 只拦 `malloc`/`calloc`/`realloc`; **不拦** `aligned_alloc`/
   `posix_memalign`/`memalign`/`valloc`/`mmap`。libipc 的 `static_alloc::alloc` 就是 `std::malloc`
   (`src/libipc/memory/alloc.h:20-27`) ⇒ 该边界**不影响** UF-001/UF-002。并发下"第 N 次命中"归属不确定。
5. ⚠️ **artifact 在 `/tmp`, 非持久** —— `/tmp/uf000_acceptance.log`(252 行)。需长期留证须另存。
   **行号为 2026-09-18 快照; 本轮未取文档 md5 之外的新锚。**
6. ⚠️ **工具侧另有两处"同一过期数字"残留, 尚未订正**(2026-09-18 writer 读码定位; 本轮写边界只授权
   改 README, 故这两处**未动**):
   - `run_acceptance.sh:47-48` —— 步骤 3c 的 banner 写"**期望 rc=139**", 档位却写死过期的 `56 57`
     ⇒ **每轮重放必得 rc=3**, 读起来像"验收失败"; 实为**档过期**(与限定 2 同源)。
   - `fi_positive_control.cc:22` —— 头注释里的显式档示例写死 `56 57 0 dtor`, 照抄即踩过期档。
   ⇒ 这两处**不改变**本轮结论(承重的步骤 3 走自标定), 但会把"看起来像失败"的读数**传染给下一次
   重放**, 且 3c/3d 两处**同时失效**会让读者误判"辅助档全崩 = 工装坏了"。建议派工一并订正。

### 0.3.3 UF-007 写回(2026-09-18; 判定 `❌已作废` —— 登记点过期, ⛔**不是修好了**)

> 本节是 0.4 协议第 2 步要求的"条目块"(命令 / 结果 / 日期 / 变异验证)。⚠️ **第 3 步的"搬移"尚未执行**:
> [shm_defect_fixes.md](shm_defect_fixes.md) 里**没有** § 修复记录 9(该文件 §8 之后直接是 §12)⇒
> **本节即 `UF-007` 的唯一详情**, 索引与回溯一律以本节为准; 0.1 表 `UF-007` 行保留指针。
>
> ⛔ **本项的状态是 `❌已作废`, 不是 `✅已修`** —— 它**不是**被修好的。作废的依据是"**登记点过期、
> 触发路径在 HEAD 上不可达**、§8 的守卫已成立", 三者都有实测证据; 缺陷的**后果链本身是真的**
> (下面用例 ①), 只是**今天走不到**。⛔ 任何人不得把本节读成"UF-007 已修复"。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`(与 0.3.2 同)。
**产物**: 取证用例 `test/test_uf007_id_pool_double_release.cpp`(2 用例, **新增文件, 未提交**)。
**本轮没有改任何产品码** —— `src/libipc/**` 零改动, 尤其**没有动** `src/libipc/prod_cons.h`
(分析员预先为变异验证锁定的那个文件, **至今一个字节都没改**)。

**执行的命令(可直接粘贴)**

```bash
# 前置: 清掉 UF-003 的残池(否则基线门会红, 而那不是 UF-007 的效应)
ls /dev/shm | grep CHUNK_INFO
ls /dev/shm/__IPC_SHM__*CHUNK_INFO__* | xargs -r rm -f

# ① 取证用例本体(2 用例)
./build/bin/test_uf007_id_pool_double_release
# ② 指定用例隔离复跑
./build/bin/test_uf007_id_pool_double_release \
  --gtest_filter='UF007LappedDrain.LappedDrainMustNotShrinkChunkPoolNorShareOneChunk'
# ③ UF-003 的判据用例(套圈不重投) —— 0.5 第 6 条点名的决定性验证
./build/bin/test_lap_safety --gtest_filter='LapSafety.LappedDrainMustNotDeliverDuplicates'
# ④ chunk 池的既有回归(3 用例)
./build/bin/test_chunk_hold
```

| 步 | 命令 | rc | 关键读数 |
|---|---|---|---|
| ① | `test_uf007_id_pool_double_release` | 0 | **2/2 PASS**; `Consequence_DoubleReleaseSelfLoopsAndLosesAllOtherIds` 0 ms; `LappedDrainMustNotShrinkChunkPoolNorShareOneChunk` **363 ms** |
| ② | 同①的指定用例 | 0 | 同一档 363 ms 复跑, 无 flake |
| ③ | `test_lap_safety --gtest_filter=…LappedDrainMustNotDeliverDuplicates` | 0 | **1/1 PASS**, 61 ms |
| ④ | `test_chunk_hold` | 0 | **3/3 OK**: `HeldChunkSurvivesOverwriteWithLaggingPeer` 1 ms; `OverwrittenChunksAreReclaimed` 122 ms; `HeldLargeMessageSurvivesItsReceiver` 0 ms |

**结果**

- 用例 ① `Consequence_…` 是**表征用例**(characterization): 它记录的是 `id_pool::release` **今天不幂等**
  这个事实 —— `release()` 是头插(`next_[id] = cursor_; cursor_ = id;`, `src/libipc/utility/id_pool.h:83-88`),
  同一 id 归还两次 ⇒ `next_[X] = X` ⇒ 链表指向自己。用例断言: 单次归还的**阴性对照**必须完整恢复
  `kCap` 个 id(通过), 之后重复归还 **`acquire()` 4×kCap 次都取不到负值**、可达 id **恰好 1 个**、
  且就是被重复归还的那个(全部通过)⇒ **后果链真实**: 自环形成后池里其余 `kCap-1` 个 id 永久不可达。
  这条**今天应当通过**; 若哪天有人把 `release` 改成幂等的, 它会转红, 那时 §4/§8.4 的整条后果链要重写。
- 用例 ② `LappedDrain_…` **真的走到了套圈路径**(连发 `256×3 = 768` 条 12288B 大消息、全程不读, 再读空),
  结果: 读空后**连发 20 条探针全部读回**, **重复三轮**都成立(池不缩水); 两条不同内容(0xA5/0x5B)的
  大消息也都能原样读到(无共块)⇒ **触发路径在 HEAD 上不可达**。
- ③ 是 0.5 第 6 条点名的决定性验证(`dzflat_known_issues.md` §4 自陈矛盾的裁决手段): **1/1 PASS**
  ⇒ 套圈读空**不再重复投递**。④ chunk 池既有 3 用例全绿 ⇒ 无回归。
- 分析员(seq125)的**预注册规则**: "全绿 ⇒ UF-007 可判 `❌已作废`; 若第二个用例红而基线门绿 ⇒ UF-007 是真缺陷"。
  **实测落在"全绿"分支** ⇒ 判定 `❌已作废`。同一份报告的另一句同样承重: "⛔ 因此**不得**把本报告当作
  『UF-007 已作废闭环』" —— 那是在**没有运行证据**时不得宣布; 本节补上的正是运行证据, 故可以宣布,
  但**范围严格限定**在"登记点过期 / 触发路径不可达"。

**变异验证**: ⛔ **未执行 —— 这是本节的已知缺口。**

- 0.3 第 5 条要求"每条必须附变异验证(回滚后哪条断言转红)"。本项**没有可回滚的对象**: 本轮
  **未改一字节产品码**, 造不出"回滚"这个动作。为变异验证而**预先锁定**的 `src/libipc/prod_cons.h`
  (分析员 seq125 声明"为变异验证预留, **尚未被改动一个字节**")**至今未被改动**。
- **替代证据(⛔ 明确不等于变异验证)**: (a) 用例 ① 里的**单次归还阴性对照** —— 把"release 的幂等性"
  当作被测机制时, 这条对照就是它的反向观测; (b) 用例 ② 的**基线门**(开跑前连发 `kProbe` 条大消息
  必须全部读回) —— 它把"池缩水"与"池本来就不干净(UF-003 的效应)"分离开, 使红绿**可解释**。
  这两条能排除"用例自己坏了", 但**不能**替代"改一处产品码看哪条断言转红"。
- **若要补上真正的变异验证**: 最小变异点是 `prod_cons.h` 的套圈重同步(`dzflat_known_issues.md:264`,
  8.1 "`pop()` 被套圈则重同步游标")—— 把它回退成旧逻辑, 用例 ② 的 ①/② 两组断言应转红。
  **该动作需先拿授权**(动了 `src/**` ⇒ 超出本轮"仅改两份文档"的写边界)。

**限定(⛔ 不得当已全部闭环读)**

1. ⚠️ **artifact 在 `/tmp`, 非持久**: `/tmp/uf007.log`(用例本体 2/2)、`/tmp/uf007_lap.log`(③)、
   `/tmp/uf007_hold.log`(④)、`/tmp/uf007_build_extra.log`(14:04:59 重链接, Built target 100%)。
   需长期留证须另存。
2. ⚠️ **跑前必须干净残池**: 用例 ② 的基线门在残池下会红, 而**那是 UF-003 的效应, 不是 UF-007 的**
   —— 见 §6.3 清残池步骤。
3. ⚠️ **池容量由模板导出, 不是写死的 32**: `kCap = ipc::id_pool<>::max_count`, 探针条数 `kProbe` 与
   自环探测次数 `4×kCap` 都从它导出。容量一变(改 `large_msg_cache`, 或换尺寸档), 写死魔数会让判据
   在**错误前提上照样给结论**。
4. ⚠️ **四份日志里 `grep -c 变异` 全为 0** ⇒ 与上面"变异验证未执行"一致, 不矛盾, 也**不构成**变异验证。
5. ⚠️ **`docs/dzflat_known_issues.md` §4 的 `:198` 仍写"未修, 既存"**(它与 §8 的自陈矛盾) —— 本项的
   作废**不自动**修掉那份文档的措辞; 该文档**不在**本轮写边界内。

### 0.3.4 UF-004 写回(2026-09-18; 判定实验 ✅ 完成 —— ⛔**缺陷本体未修**)

> 本节是 0.4 协议第 2 步要求的"条目块"。⚠️ **第 3 步的"搬移"尚未执行**:
> [shm_defect_fixes.md](shm_defect_fixes.md) 里**没有** § 修复记录 10(该文件 §8 之后直接是 §12)⇒
> **本节即 `UF-004` 的唯一详情**, 索引与回溯一律以本节为准; 0.1 表 `UF-004` 行保留指针。
>
> ⛔ **本项状态仍是 `⬜未修`** —— 判定实验**已完成**(假设 (C) 坐实), 但**缺陷本体一个字节都没改**。
> ⛔ 也**不是** `⏸️待决策`: 本文档里 `⏸️待决策` 的固定含义是"未授权改产品码 / 仅证据"
> (见 0.1 表 UF-005a/b、UF-008、UF-009、DF-*), 而 UF-004 **是有授权改产品的**(缺的只是三个修法里
> 挑一个) ⇒ 停在 `⬜未修` 才不失真。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
**产物**: `build/uf004_exit_semantics/20260918_143034_582610/`
(`direct_r1 direct_r2 factory_r1 factory_r2 matrix.csv results.json`)。
**执行**: tester-claude(seq133, 跑实验并出结论); **只读复核**: reviewer-claude(seq134, 逐格复算
`matrix.csv` 与 `results.json` 的自洽性, 未改一个字节)。**写回人**(本节作者)**未运行**该脚本 ——
本节只**转述并限定**两位成员的成果。**本轮没有改任何产品码**。

**执行的命令(可直接粘贴; 由 tester 执行, 非本节作者)**

```bash
bash tools/sercli_live_probe/exit_semantics_matrix.sh \
  --out build/uf004_exit_semantics/20260918_143034_582610 \
  --rounds 2 --grace-ms 10000 --hold-ms 6000 --lib-fast-ms 1000
```

**结果(`matrix.csv` 8 行, 逐格)**

| 轮 | via | role | rc | 信号 | 挂起 | 退出耗时 ms | `SERVER SUMMARY` | 本 topic 残留 | 判定 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | direct | client | 0 | 无 | 否 | 34 | 有 | 0 | `app_handler_alive` |
| 1 | direct | server | 0 | 无 | 否 | 98 | 有 | 0 | `app_handler_alive` |
| 2 | direct | client | 0 | 无 | 否 | 49 | 有 | 0 | `app_handler_alive` |
| 2 | direct | server | 0 | 无 | 否 | 56 | 有 | 0 | `app_handler_alive` |
| 1 | factory | client | 0 | 无 | 否 | 48 | **无** | **17** | `library_exit0_fast` |
| 1 | factory | server | 0 | 无 | 否 | 83 | **无** | **17** | `library_exit0_fast` |
| 2 | factory | client | 0 | 无 | 否 | 47 | **无** | **17** | `library_exit0_fast` |
| 2 | factory | server | 0 | 无 | 否 | 83 | **无** | **17** | `library_exit0_fast` |

- **预注册期望 vs 实测**: `results.json` 的 `preregistered_expectation` 写 direct → `app_handler_alive`、
  factory → `library_exit0`。实测 **4/4 : 4/4 一致** ⇒ 假设 **(C)「库接管进程退出」坐实**:
  `--via factory` 走公共工厂 ⇒ `EnsureShutdownMonitorStarted()` 覆盖 `main()` 自己装的处理器 ⇒
  detached 线程里 `std::exit(0)` ⇒ 栈不展开 ⇒ `main()` 的 `shared_ptr` 局部量不析构 ⇒ 段不 unlink。
  聚合读数: direct `rounds_with_server_summary 2/2`、`rounds_with_zero_residue 2/2`、`rc_set "0"`;
  factory `rounds_with_server_summary 0/2`、`rounds_with_zero_residue 0/2`、`rc_set "0"`。
- **判定顺序** hang → 信号 → rc **一行都没触到** `crash` / `hang` / `killed_ext` / `signal_other`
  ⇒ **非崩溃、非挂起**。reviewer 的 D1(原 `decision_rule.crash` 会把看门狗自己的 `kill -9` 读成崩溃,
  即 `by_signal=9` / `rc=137`)已修, 现要求 `hang=0`。
- ⛔ **"每角色残留 17"不等于"零清理"**, 也**不等于**"17 个段泄漏到进程死亡之后": 残留 17 =
  16 个 `__IPC_SHM__{AC,CC,QU,RD,WT}_CONN__…_ser_{r,w}[_WAITER_COND_ / _WAITER_STATE_ / __64__16]`
  \+ 1 个 `dz_ipc_d3_…_ser_control2`; 运行期增量 23 = 17 + 6 个 `_WAITER_LOCK_`。
- ⚠️ **`elapsed` 一栏无路径判别力**: `results.json` 的 `corroboration` 声称应用侧约 20–40 ms、
  库侧约 100–200 ms, 而**实测反证**它 —— factory 是 47/48/83/83 ms, 落在该带**之下**且与 direct 的
  34–98 ms **重叠** ⇒ 该字段**不得**用作判别依据, 只登记、不作为判据。
- ⚠️ **修法仍未拍板**(⛔ **只对 2026-09-18 16:2x 落笔时点成立, 已被 0.3.12 取代**): §4 的"修法选项"1/2/3
  (默认不装处理器 / 保留默认 + `DisableShutdownMonitor()` 与 `OnShutdown(callback)` 开关 / 只修细节如退出码
  用 `128+signo`)**当时三选一尚未选**, 故本节停在 `⬜未修`, 负责人栏写作 `tester-claude(实验✅) → coder(改法待拍)`。
  ⇒ ✅ **同日 17:1x 已拍 (b) opt-out 并落码验收**(见 **0.3.12**): 该格现为 `🔶部分修`。

**变异验证**: 本项交付的是**判定实验**而非修复, 故 0.3 第 5 条意义上的"回滚后哪条断言转红"**不适用**:
没有产品码变动就没有回滚对象。**替代证据(⛔ 不等于变异验证)**: (a) **预注册期望 vs 实测的
4/4 : 4/4 比对** —— 期望在跑之前就写进 `results.json`, 事后逐腿核对, 不是事后编的;
(b) **两条 via 互为对照腿** —— direct 腿"有 `SERVER SUMMARY` / 残留 0"与 factory 腿"无 / 残留 17"
构成同批次对照, 排除了"环境整体出了问题"这一类解释。**真正的变异验证**要等修法落地后才做得出
(把三个选项之一落码, 再回退看哪条断言转红)。

**限定(⛔ 不得当已全部闭环读)**

1. ⚠️ **每 via 只有 2 轮**: 4/4 一致是 2 轮 × 2 角色的结果, 不是多轮统计, **无方差估计**。
2. ⚠️ **构建指纹与 §4 正文的锚点不同**: 本 artifact 产在 13:35 之后(库侧有编辑)且经 14:26 重建,
   **不是** §4 正文引用的 2026-09-16 构建锚点(`build/live_runs/20260916_153546`)
   ⇒ 只能作**机制判定**, **不可**当作"§4 原读数被复现"。
3. ⚠️ **同批还有更早的一次**: `build/uf004_exit_semantics/20260918_142130_551147/` 是**不完整的一次**
   (factory 腿全部 `startup_fail`、rounds=0)⇒ ⛔ **不是**独立复现, 不得与本案并列计数。
4. ⚠️ **`matrix.csv` 客户端行第 14/15 列(标签 `residue_ser` / `residue_cli`)与写入值对调** ——
   `residue_own` 列**正确**, `classify` 判定**不受影响**; 属**报告层瑕疵**, reviewer 依"停止脚本编辑"
   约束**仅登记未改**。
5. ⚠️ **同批须同步 `docs/dzipc_log.md:100`** —— 本节落笔时该文件**不在**当轮写边界内, 故**未动**;
   ✅ **2026-09-18 已随 0.3.12 同步完毕**(opt-out 后监控线程**不启动** ⇒ 库**不再**替你
   `StopDzipcLog()`, 须应用自己显式调用)。

### 0.3.5 UF-009 写回(2026-09-18; 证据 ✅ 完成 / 机制已观察 —— ⛔**未改产品码**)

> 本节是 0.4 协议第 2 步要求的"条目块"。⚠️ **第 3 步的"搬移"尚未执行**:
> [shm_defect_fixes.md](shm_defect_fixes.md) 里**没有** § 修复记录 11(该文件 §8 之后直接是 §12)⇒
> **本节即 `UF-009` 的唯一详情**, 索引与回溯一律以本节为准; 0.1 表 `UF-009` 行保留指针。
>
> ⛔ **产品码仍然一个字节都没改** —— 本项与 UF-004 同链、同批、共用同一 artifact, 但它**始终**属于
> "仅证据, 不授权改产品"那一类; 本轮把**证据**这一半做完(机制已观察、退出码已判定),
> **不改它的授权边界**。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
**产物**: 与 0.3.4 **同一个** artifact `build/uf004_exit_semantics/20260918_143034_582610/`。
**执行**: tester-claude(seq133); **只读复核**: reviewer-claude(seq134)。

**执行的命令(可直接粘贴; 由 tester 执行, 非本节作者)**

```bash
# 退出码与残留段计数由矩阵脚本一并采集(与 0.3.4 同一次运行)
bash tools/sercli_live_probe/exit_semantics_matrix.sh \
  --out build/uf004_exit_semantics/20260918_143034_582610 \
  --rounds 2 --grace-ms 10000 --hold-ms 6000 --lib-fast-ms 1000
```

| 步 | 命令 | rc | 关键读数 |
|---|---|---|---|
| 1 | (上); factory 腿 | 0 | rc 全 `0`, `by_signal` 全 `0`, `hang` 全 `0`, 残留 **17/腿**, `SERVER SUMMARY` **无** |
| 2 | (上); direct 腿(对照) | 0 | rc 全 `0`, 残留 **0/腿**, `SERVER SUMMARY` 2/2 **有** |

**结果**

- **判定(此前是"崩溃/挂起未判定")**: **非崩溃、非挂起** —— 8 腿 `rc` 全为 `0`、`by_signal` 全为 `0`、
  `hang` 全为 `0`; 判定从未触到 `crash` / `hang` / `killed_ext` / `signal_other`。
- **机制已观察**: factory 腿 4/4 判为 `library_exit0_fast` = 库接管退出后 `std::exit(0)` 快速返回;
  对照 direct 腿 4/4 `app_handler_alive`。⇒ 与 UF-004 同一机制, 见 0.3.4。
- **`SERVER SUMMARY` 缺失已坐实**: factory `rounds_with_server_summary 0/2` vs direct `2/2`
  ⇒ 库接管后应用自己的收尾打印**从未执行**, 与"栈不展开"一致。
- **残留 17 已坐实**: factory 每角色 17 段(构成见 0.3.4), direct 每角色 0 段。
- **产品码未改**: 本项自始至终**不授权改产品码**; 授权范围内可改的只有工具脚本与测试
  (`tools/sercli_live_probe/**`、`scripts/**`)以采集退出码与残留段计数 —— 本轮**也没有改它们**(只运行)。

**变异验证**: 与 0.3.4 同 —— 本项是**证据项**不是修复, 无产品码变动 ⇒ 无回滚对象, 0.3 第 5 条意义上的
变异验证**不适用**。**替代证据(⛔ 不等于变异验证)**: direct / factory 两条 via 的同批次对照 +
**预注册期望**(direct → `app_handler_alive`, factory → `library_exit0`)在跑之前就写进 `results.json`。

**限定(⛔ 不得当已全部闭环读)**

1. ⚠️ **与 0.3.4 完全同源**: 同一 artifact、同一批次、同样 2 轮/via ⇒ 0.3.4 的限定 1–4 **逐条适用**
   (轮次少、构建指纹不同、142130 那次不完整、`matrix.csv` 第 14/15 列标签对调)。本节**不重复**列,
   但**不得**把两节当作两次独立复现。
2. ⚠️ **残留 17 ≠ 泄漏 17 个段到进程死亡之后**: 见 0.3.4 的构成说明。
3. ⚠️ **0.5 第 9 条(脚本侧采集落点)仍未收口** —— 本节用的是 tester 的矩阵脚本,
   不是该条设想的独立采集落点。
4. ⚠️ **本项不因证据完成而升级授权** —— 要改产品码(让库不再接管退出), 仍需 leader 另行拍板;
   本节的 `⏸️待决策` **保持**。

### 0.3.6 UF-001 写回(2026-09-18; 判定 `✅已修` —— **选项 1 落码**)

> 本节是 0.4 协议第 2 步要求的"条目块"。详情已按第 3 步搬到 [shm_defect_fixes.md](shm_defect_fixes.md)
> **§ 修复记录 12**; 0.1 表 `UF-001` 行保留指针。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
**执行**: coder-claude(seq135); **静态复核**: 本节作者(`rg` + 读 `git diff`, 见末段)。

**改了什么(产品码, §1「修法选项 1」)**

`src/libipc/memory/allocator_wrapper.h` —— `allocate` 不再把"失败"翻译成"空指针":

- **去掉 `noexcept`**;
- `count > max_size()` ⇒ `throw std::length_error(...)`(原: `return nullptr`);
- `alloc_.alloc(count * sizeof(value_type))` 返回 `nullptr` ⇒ `throw std::bad_alloc()`(原: 把 `nullptr` 直接交给调用方);
- 补 `#include <new>`(取 `std::bad_alloc`)、`#include <stdexcept>`(取 `std::length_error`)。

⛔ **这不是"新加错误处理", 而是把"把一切内存失败压成写地址 0"的那个开关拆掉** —— 0.1 表把本条记作
**放大器**正是这个意思: 崩溃点原本在**很久之后、别处**, 且无诊断。

**执行的命令(可直接粘贴; 由 coder 执行, 非本节作者)**

```bash
# 构建
make -C build test_alloc_fault_inject                              # 期望 rc=0
# 判据本体(注入态)
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  ./build/bin/test_alloc_fault_inject
# 对照(无注入)
./build/bin/test_alloc_fault_inject
# 回归
./build/bin/test_ipc && ./build/bin/test_shm && ./build/bin/test_sync && ./build/bin/test_uf007
```

| 步 | 命令 | 结果 | 关键读数 |
|---|---|---|---|
| 1 | `make -C build test_alloc_fault_inject` | rc **0** | 构建通过 |
| 2 | 注入态(上) | **3/3 PASS** | `hits=1`、`band_calls=1`、`total_calls=23`; 断到 `std::bad_alloc` |
| 3 | 无注入(上) | **3 SKIP / 0 PASS** | 工装未加载 ⇒ 全部 SKIP, ⛔不得当绿读 |
| 4 | 回归四件 | **8 + 8 + 5 + 2 全 PASS** | `test_ipc` 8 / `test_shm` 8 / `test_sync` 5 / `test_uf007` 2 |

**结果**

- **PASS**。承重读数是 **`hits=1` 且 `band_calls=1`** ⇒ **注入真的命中了**(不是"没注入所以没崩"),
  这正是 0.3.1 第 3 条要求的"先拿命中计数, 再谈业务断言"; `total_calls=23` 是同批总分配次数。
- 无注入态 **3 SKIP / 0 PASS**: 与 0.3.1 第 2 条一致 —— 工装未加载时**绝不绿色通过**。
  SKIP 的含义是"本用例无判据力", **不是**"通过"。
- 回归四件全绿 ⇒ 本条改动**没有**把行为带到别的路径上。

**变异验证(0.3 第 5 条)**: **已做**。回滚本条改动(恢复 `noexcept` + `return nullptr`)后重跑,
**rc = 139**(SIGSEGV)。转红的断言是注入态里那条"分配失败必须抛 `std::bad_alloc`、不得以信号终止"
的判据 ⇒ 判据**承重**: 绿是改动挣来的, 不是"没注入"换来的。
> ⚠️ 与 **UF-000** 那条"回滚变异仍欠一次"不同 —— **本条有回滚读数**。

**限定(⛔ 不得当已全部闭环读)**

1. ⚠️ **只覆盖 `allocator_wrapper::allocate` 这一条入口**。`mem::alloc<T>`(`include/libipc/pool_alloc.h`,
   失败**返回 nullptr** 不抛)本轮**不改** —— 那一侧的收口在 **UF-002** 的判空里做(见 0.3.7)。
2. ⛔ **本条已于 2026-09-18 过期(订正; 勿再引用旧文)**: 原文写 `src/libipc/buffer.cpp:59`
   (`~buffer()` 的 `p_->clear()`)仍是**无条件解引用 `p_`**, 与 UF-002 同型; `buffer` **不在**
   0.3.7 的五类清单里 ⇒ 本轮**未改**, 不得当已修读。⚠️ **该行今日已被 coder 改动** ⇒ "未改"**已不成立**;
   ⛔ **但改动不构成修复(回滚变异阴性)**。准确现状与裁决口径见 **0.3.7 限定 1** 与 **0.3.8(7)**。
3. ⚠️ 上表读数**来自 coder seq135**; 本节作者只做静态复核(见下), **未重跑**该批命令。

**静态复核(可逐条复现)**

```bash
rg -n 'allocate|noexcept|length_error|bad_alloc' src/libipc/memory/allocator_wrapper.h
git diff src/libipc/memory/allocator_wrapper.h
```

### 0.3.7 UF-002 写回(2026-09-18; 判定 `✅已修` —— 五类 pimpl 失效态判空; ⚠️遗留 `buffer.cpp:59`)

> 本节是 0.4 协议第 2 步要求的"条目块"。详情已按第 3 步搬到 [shm_defect_fixes.md](shm_defect_fixes.md)
> **§ 修复记录 13**; 0.1 表 `UF-002` 行保留指针。

**日期**: 2026-09-18。**基线**: HEAD = `3818899687efefd35f8514acdff565226bec7cce`。
**执行**: coder-claude(seq135); **静态复核**: 本节作者(`rg` + 读 `git diff`)。

**改了什么(产品码, §2「修法选项 1」= 入口判空 + 失效态空转)**

`p_ == nullptr` 是**失效态, 不是不可能发生的状态**: `pimpl<T>` 走"不舒服"分支时 impl 在堆上,
`mem::alloc<T>` **失败返回 `nullptr` 而不抛**(`pool_alloc.h:87-97`), 构造函数不检查。
0.6 第 3 条已确证**光加判空不够** —— 必须改**解引用次序**(`handle::release()` 首句是
`impl(p_)->id_ == nullptr`, **先解引用 `p_` 再判 `id_`**)。本轮即按此收口, 五类逐处覆盖:

| 文件 | 类 | 失效态处置(节选) |
|---|---|---|
| `src/libipc/shm.cpp` | `shm::handle` 族 | `valid()` 假 / `size()` `0` / `name()` 空 / `ref()` `-1` / `detach()` 空转 / `release()` `-1` / `acquire()` 报错返回 |
| `src/libipc/socket/udp.cpp` | `UDPNode` | `create()` 报错返回 / `connect()`·`send()` `false` / `role()` 缺省 `SendRecv` |
| `src/libipc/sync/mutex.cpp` | `mutex` | `open()` 报错 `false` / `native()` `nullptr` / `valid()` `false` / `close()`·`clear()` 空转 |
| `src/libipc/sync/condition.cpp` | `condition` | 同上(同族同口径) |
| `src/libipc/sync/semaphore.cpp` | `semaphore` | 同上(同族同口径) |

- ⛔ **析构也是入口**: `~mutex()` 旧实现的第二句 `p_->clear()` 与首句 `close()` 一样是**崩点**;
  `~handle()` 同理(0.6 第 3 条: 其首句即 `release()`)。故五类的**析构一并判空**。
- ⛔ **不动全局错误模型**: 失效态靠"取原生句柄得 `nullptr` / 加解锁得 `false`"暴露; ⛔**刻意不新增枚举值**
  —— 加值会改枚举的取值范围(ABI/语义), 而失效态本身已由 `connect()`/`send()` 的 `false` 暴露,
  不需要靠 `role()` 说谎(理由原样写在 `udp.cpp` 内注释)。

**执行的命令 / 结果**

与 0.3.6 **同一次运行**(同一份注入态 gtest 批次里 `HandlePimplAllocFailureMustNotCrash` 就是本条的路径):

| 步 | 命令 | 结果 | 关键读数 |
|---|---|---|---|
| 1 | `make -C build test_alloc_fault_inject` | rc **0** | 构建通过 |
| 2 | 注入态(同 0.3.6 步 2) | **3/3 PASS** | `hits=1`、`band_calls=1`; `valid()` 假、`release()` 返回失败码而**不崩** |
| 3 | 无注入(同 0.3.6 步 3) | **3 SKIP / 0 PASS** | 工装未加载 ⇒ 全部 SKIP |
| 4 | 回归四件 | **8 + 8 + 5 + 2 全 PASS** | `test_ipc` 8 / `test_shm` 8 / `test_sync` 5 / `test_uf007` 2 |

**结果**: **PASS**。注入态在 `hits=1` 的前提下走通了"`handle` 的 pimpl 分配失败"这一路,
断言的是**失效态语义**(`valid()` 为假、`size()` 为 `0`、`detach()` 得 `nullptr`、`release()` 得 `-1`)
而**不是**"没崩" —— 后者单独不成判据(0.3.1 第 3 条)。

**变异验证(0.3 第 5 条)**: **已做**。回滚本条改动后重跑, **rc = 139**(SIGSEGV)。
转红的断言是"分配失败后 `valid()` 必须为 `false`"与"`release()` 应返回失败码**而非崩**"这两条 ——
即回到 0.6 第 3 条记载的崩溃形态(读 `0x8`)。⇒ 与 UF-001 各自独立回滚, **两项分别 rc139**。

**限定(⛔ 不得当已全部闭环读)**

1. ⛔ **本条表述已于 2026-09-18 过期(订正; 勿再引用旧文)**: 原文写 `src/libipc/buffer.cpp:59`
   (`~buffer()` 的 `p_->clear()`)仍是**无条件解引用 `p_`** —— 与本节修的五类**同型**, 但 `buffer`
   **不在**本节清单内 ⇒ **本轮未改**。⚠️ **该行今日已被 coder 改动**(仅析构那一行 + 注释, `+11/−1`)
   ⇒ "本轮未改"**已不成立**, 旧文作废。
   ⛔ **但改动不构成修复 —— 回滚变异为阴性**: 还原该行后重跑同一注入探针 **rc = 0, 不崩**;
   成因是 `pimpl<T>::clear()` 只做 `clear_impl(static_cast<T*>(this))`, 而"不舒服"分支即
   `mem::free(p)`, 传空指针**立即返回、从不读 `this` 的内存**。
   ⇒ ⛔ **不满足 0.3 第 5 条, 本节不得据此宣称 `buffer` 已加固**, 0.1 表状态列**不动**;
   **保留 / 回退待 leader 裁**, 详见 **0.3.8(7)**。
   (0.9 一览第 2 行原文"仅 `buffer` 已加固"今日**已反转**, 见 0.6 第 18 条)
2. ⚠️ **`utility/pimpl.h` 本身未改**: 本节只改**调用方**的判空, `pimpl<T>` 的"失败返回空"约定
   (`pool_alloc.h:87-97`)原样保留 ⇒ 语义仍是"失败可被观察到", 而不是"失败即抛"。
3. ⚠️ **语义裁决仍属 reviewer**: 各入口失效态的**返回值选择**(如 `role()` 取缺省值而非新增枚举值)
   是**实现侧的自洽选择**, 尚未经 reviewer-claude 的语义复核 ⇒ 若复核要求改动, 本条需重新
   走一次变异验证。**0.1 表 `负责人` 列本就写着"coder(实现)+ reviewer(语义)", 这一半未闭环**。
4. ⚠️ 上表读数**来自 coder seq135**; 本节作者只做静态复核, **未重跑**该批命令。

**静态复核(可逐条复现)**

```bash
rg -n 'invalid state' src/libipc/shm.cpp src/libipc/socket/udp.cpp \
  src/libipc/sync/mutex.cpp src/libipc/sync/condition.cpp src/libipc/sync/semaphore.cpp
rg -n 'p_->clear\(\)' src/libipc/buffer.cpp     # ⇒ :59 已改(2026-09-18); 但回滚变异阴性 ⇒ 非修复, 见 0.3.8(7)
```

### 0.3.8 剩余项裁定登记(2026-09-18; ⛔**设计态裁定, 无任何实测 PASS** —— 本节**不是** 0.4 写回)

> ⛔ **先读这一段的性质说明, 否则必然读歪**: 0.4 协议是**完成之后**的写回协议(第 2 步要
> "结果(PASS/FAIL 与关键读数)"、第 3 步要把详情搬进 [shm_defect_fixes.md](shm_defect_fixes.md)
> 新增「§ 修复记录 N」)。**本节一项都不满足** —— 下面全是**源码级设计判据 / 架构裁定**,
> 外加**一条阴性(FAIL 向)的变异结果**;**没有一条**交付了"修完之后哪条断言转绿"。
> ⇒ **本节不产生 0.4 写回, 不新增任何「§ 修复记录」, 0.1 表的状态列一格都不动**
> (`UF-003`/`UF-004`/`UF-005a`/`UF-005b`/`UF-006`/`UF-008` 与 `DF-*` 全部维持原值)。
> ⛔ **不得把本节任何一条读成 PASS**; 尤其 ⛔**不得**把"裁定已完成"读成"缺陷已修",
> 也 ⛔**不得**把"已具备落码条件"读成"已落码"。

**日期**: 2026-09-18。**来源(三条, 均为本轮实际收到的结果)**: ①reviewer-claude 只读架构裁定
(共享区 `review_remaining_uf_schedule_boundary_reviewer-claude.md` + seq10); ②coder-claude 的
`src/libipc/buffer.cpp` 落码与**阴性**回滚变异(seq8/seq9, 证据 `coder_claude_uf002_buffer_evidence.md`);
③tester-claude 对 **0.3.4** 的两处订正。
**本轮无产品码改动在文档侧、无新增测试**; 本节作者(writer) ⛔ **未运行**上述任何实验, 只做转述与限定。

#### (1) UF-003 —— ⛔ 不可先做; 且 0.1 表 `修改边界` **漏了一个文件**

- ⛔ **不可先做**: 改池 = 改**共享段布局** = 与旧版本不互通。且 ⛔ **只给段名加版本号/纪元也解决不了** ——
  chunk id **经环传递**(`src/libipc/ipc.cpp:66-67` 把 `storage_id_t` 写进环条目、`:646` 从 `msg->data_` 读回)
  ⇒ id 空间是**跨进程契约**; 段名改了而环共用 ⇒ 新旧进程**各用各池**, id 语义**静默分裂**(比残池更坏)。
  池段名今天是 `make_prefix(pref, {"CHUNK_INFO__", chunk_size})`(`ipc.cpp:332-333`), **无版本标记**。
- ⛔ **0.1 表 `修改边界` 漏项(2026-09-18 补)**: `chunk_info_t` 在仓里有**两份定义** ——
  `src/libipc/ipc.cpp:258` 与 **`src/libipc/sniffer.cpp:89-101`**; 原边界只写了前者
  ⇒ **不补则 sniffer 读旧布局**。已在 0.1 表该行补上, 落码前须按两处一起改。
- ✅ **可先做(⛔ 不是生产修复)**: 把 §6.3"确认无活进程映射后清池"做成**工具 + 手册**(= §3 修法选项 2),
  外加一个只读残池观测器。⚠️ 它是开发/测试环境兜底; ⛔ **不得据此降低"静默退化"的严重级**。
- **过期表述订正(本轮的指定动作)**: 0.1 表 UF-003 行 `可并行` 格原写"⛔与 UF-007 串行且在**其后**",
  与**同一行** `依赖` 格里的 `~~UF-007 必须先修~~ ✅ 已消解` **自相矛盾**(前者把已作废项当前置)。
  ⇒ 按 0.3.3 的作废判决, 该串行约束**已失效**, 该格已改写; ⛔ **UF-007 行 `可并行` 格的镜像表述
  ("⛔与 UF-003 串行")同步订正** —— 同一对关系必须两行一致(这是 0.6 第 11 条的同一类错, 别再犯)。
- ⛔ **判据侧 BLOCKED(2026-09-18 tester 实测, 本节新增)** —— 与上面"不可先做"是**两个独立的阻断**:
  "不可先做"是**实现**被互通性决策闸住; BLOCKED 是**即便实现了也判不了**, 两者不可互相顶替。
  - **残池污染客观存在且已量化**(清理前**只读**解码 4 个 `/dev/shm/__IPC_SHM__CHUNK_INFO__*`):
    `929792` 池 `next_` 含**自环**(`next_[31]=31`)、`cursor_=31`、空闲表仅 **1/32** ⇒ 其余 30 个 id
    **不可达 = 永久泄漏**; 且自环下 `acquire()` **永返 31** 且**永不 `empty()`**。`2048` 池丢 1 槽;
    `1024`/`132096` 完整。⇒ 自环成因与 `id_pool::release()` 一致(**重复归还**) = **UF-007 的机制指纹**
    ⇒ ⚠️ **该读数混入 UF-007 效应**, 引用前须分离(正是 §3 症状 2 那条冻结订正要求的事)。
  - ⛔ **现有两条判据不能复现它**: 残池态(A 臂)与清池态(B 臂)**逐用例完全相同** ——
    `test_chunk_hold` **3/3 PASSED**、`test_lap_safety` **3/3 PASSED**(含 `LappedDrainMustNotShrinkChunkPool`)
    ⇒ **A=B ⇒ 无判别力**。
  - ⛔ **原因由阳性对照坐实(⛔ 不是推测)**: 把 `929792` 池人为置为**完全耗尽**(`cursor_` 21→32 ⇒
    `empty()==true` ⇒ `acquire()` 恒 `-1`)后重跑, 两条判据**仍 3/3 与 1/1 全绿**
    ⇒ 它们**结构性不走该池的 `acquire` 路径**。
  - ⇒ **裁定**: 现有判据分不开(修好 / 未修 / 残池) ⇒ **"修完是否闭环"不可归因**。要让本项可验收,
    须**新补**一条能观测该尺寸档「借样成功率 / 可用槽数」的用例, 且**必须过同一阳性对照**
    (在**合成耗尽池**上能**转红**)才算合格 —— ⛔ 不过对照的用例不得计入验收。
  - **清池现状(已按 §6.3 执行)**: 删除前二次确认 `grep -l CHUNK_INFO /proc/*[0-9]/maps` = **0**;
    现 `/dev/shm` 中 `CHUNK_INFO` 段 = **0 个**; 清池后跑一次, 正常退出**完整归还**(残留段 `free=32/32`)。
    ⚠️ 这与 §6.3 末段"清完之后之前稳定失败的用例会突然变绿"**本次没发生** —— A/B 双臂**逐用例相同**
    ⇒ 恰好是"这两条判据不走该池"的**第二个佐证**, ⛔ 不得把"清池后没变化"读成"缺陷不存在"。
  - ⚠️ **构建指纹非冻结基线**: 工作区仍有未提交的 UF-001/UF-002 落码(源文件 mtime 14:56), 而测试二进制
    为 14:04、`libipc.so` 为 15:46, 运行期经 RUNPATH 实际加载后者 ⇒ A/B 结论**内部自洽**, 但
    ⛔ **不得对外当可复现锚点**(`test_chunk_hold` md5 `9c91b2db…`、`test_lap_safety` md5 `b8ec37aa…`)。
  - ⛔ **本轮未验证(须显式挂账)**: §3 症状 1(吞吐退化)与历史读数"20 条只有 10 条进环"**仍未**按 0.6
    第 6 条重测, 且重测**必须分离 UF-007 效应**; 另 [未核] 清池后 `929792` 段为何仍未被 unlink。

#### (2) UF-004 —— 修法**仍未拍板**(reviewer 唯一推荐 (b); ⛔否决 (c))

> ⚠️ **时效**: 本节写于 **2026-09-18 16:0x**, 当时**尚无一条落码**。当日 **17:1x 已拍 (b) 并落码 + 验收**
> ⇒ 本节里的"待拍 / 尚未执行 / 尚无一条落码"一律读作**当时状态**; **现行判定见 0.3.12**(`🔶部分修`)。
> ⛔ 本节对 **(c) 的四条否决**与对 **(b) 的推荐**继续有效 —— 实现与它逐条一致(见 0.3.12(d))。

- ⭐ **机制订正(0.3.4 的前提须补这一句)**: 库的处理器安装**只发生在四个工厂函数内** ——
  `ServerIPCPtrMake`/`ClientIPCPtrMake`/`PublisherIPCPtrMake`/`SubscriberIPCPtrMake`
  (`src/dzIPC/dzipc.cc:130/142/154/166` → `EnsureShutdownMonitorStarted` `:37` →
  `StartShutdownMonitor` `:96-97` 装信号 → 线程体 `:110` `std::exit(0)`)。
  **direct 路径(`autopath::auto_*`)根本不调用它**。⇒ 0.3.4 的假设 (C) **方向对**, 但现象前提是
  「应用**先**装自己的处理器(`sercli_live_driver.cc:406-407`, `on_signal` 仅置 `g_stop`)、**再**走工厂构造」。
  这才解释了 direct 4/4 `app_handler_alive` 与 factory 4/4 `library_exit0_fast` 的**非对称**。
- **修法比较(⛔ 尚无一条落码)**: **(b) opt-out**(`include/dzIPC/dzipc.h` **纯新增**开关)⇒ 默认路径
  **逐位不变**, 可用"删掉调用"的变异证明等价 ⇒ **唯一推荐**; **(c) 只在 `SIG_DFL` 时安装** ⇒ ⛔**否决**,
  四条理由: ①**隐式且顺序依赖**(同一应用因先/后装处理器而清理责任不同)、外部**不可观测**(无返回值无日志)
  ⇒ 判据不能承重; ②**不修 UF-009**(无处理器的工厂应用仍是 `SIG_DFL` ⇒ 照样拿到库处理器 ⇒ 照样
  `std::exit(0)` ⇒ 残留 17 原样); ③**反转 `SIG_IGN`**(从"干净 exit0"变成"什么都不发生", 属既有应用行为变更);
  ④查后装是 check-then-act 窗口。
- ⛔ **诚实上限** —— ⚠️ **本句"理由不全", 已按 reviewer seq155 §4 订正**:
  ~~(b)/(c) 都不归零默认路径的残留~~ ⇒ 残留根因确实是**分离线程里的 `std::exit(0)`(不展开栈)**,
  但"归零"有**两条**路: ①**改那条退出路径**(= UF-009 边界, 本轮**未授权**)或 ②**不让库接管**
  (应用自己的处理器生效 ⇒ 应用正常退出 ⇒ 展开栈 ⇒ unlink)。**opt-out 走的正是 ②** ⇒
  A1 的 `residue_own=0` **是可成立且已兑现的**(2026-09-18 实测, 见 0.3.12(b) 第 4 行)。
  ⇒ **UF-004 的上限仍是 `🔶部分修`, ⛔ 不得记 `✅已修`**, 而理由比旧句**更硬**(三条):
  ①**"默认路径逐位不变"是边界硬要求** ⇒ factory 臂"无 SUMMARY + `residue_own=17`"**按设计必然仍在**,
  而 0.1 表该行与判定实验的观测量**全部取自默认路径** ⇒ 同一格里同时写"逐位不变"与"已修"**自相矛盾**;
  **变异只能证明"开关承重", ⛔ 不能证明"默认观测量消失"**; ②修复对 **Python 面不可达**;
  ③**②类应用开 opt-out 更差**(无自有处理器 ⇒ `SIG_DFL` 硬杀) ⇒ ⛔ 不存在"全局已修"的措辞。
  ⚠️ 另: ⛔ **opt-out 不是对 `std::exit` 那条路径的修复** —— `StopDzipcLog()` / `CleanupIpcInstances()` /
  退出码恒 0 / detached 线程**全部逐字节未动**; 它只把"装不装处理器"交还应用。
  本裁定的作用是**把"库偷处理器"这一半显式化并可验证**, ⛔ **不是**关掉 UF-009。
- 验收骨架(✅ **已于 2026-09-18 17:0x 执行完毕**, 逐门读数见 0.3.12(b); ⛔ **本骨架保留为预注册原文, 事后不得改写**): **A0 默认不变门** = 不调 opt-out 时 `exit_semantics_matrix.sh`
  必须与今日指纹**逐位同**(direct 4/4 `app_handler_alive` + 残留 0; factory 4/4 `library_exit0_fast` +
  `residue_own=17`); **M2 等价门** = 删掉工具侧 `--no-shutdown-monitor` 调用 ⇒ 必须回到 A0;
  **M1** = 把 opt-out 判定改为恒 `false` ⇒ A1 的 ①② 必须转红; **A1** = ①`sigaction(SIGTERM,nullptr,&old)`
  查到 `old` = 应用处理器 ②"47~83 ms 的库快速 exit0"消失 ⇒ 出现 `SERVER SUMMARY` + 残留 0
  ③⛔**必须显式断言"变红方向"**: 应用未装处理器 ⇒ `by_signal ≠ 0` ——
  ✅ **执行结果(2026-09-18 17:0x)**: A0 门 4 轮全 factory `library_exit0_fast` + 残留 own=17
  (与 14:30 基线**逐字段一致**, `elapsed` **不作判据**); A1 ①② 兑现(`optout_ret=1` + `app_handler_alive` +
  `residue_own=0`); A1③ 的"变红方向"断言**显式通过**(无处理器臂 `by_signal=15`/rc=143); M1(LD_PRELOAD
  钉死判定)与第二变异(`12ae64ca` 恒 false)**均转红**, 且两个**默认路径用例在两种变异下都保持绿**。
  **缺这条会把 opt-out 误读成"残留已修"**。
- ⚠️ **顺序契约**: `EnsureShutdownMonitorStarted` 是**进程级一次性**(`shutdown_monitor_started.exchange`)
  ⇒ 已装的处理器**无法卸载** ⇒ opt-out 必须定义为"**使用任何 IPC 之前**调用"; 晚调行为**待拍**
  (no-op + 日志 vs 返回 `bool`)。
- **tester 对 0.3.4 的两处订正(已复核, 同意)**: ① `elapsed` 一栏**无路径判别力** ⇒ 由"佐证"**降级**为只登记;
  ② "残留 17" **≠** "17 个段泄漏到进程死亡之后", 措辞限"**承载 topic 映射的段未 unlink**"。

#### (3) UF-005a / UF-005b —— ⛔ 不可先做(同批决策前置)

- 可先做仅: **重算脚本 + 冲突清单 + 变异** = 只读/计算, 不碰 wire、**不动组分配**。
  ⛔ 实现与产品码在兼容性拍板前一律不动。
- **口径打架仍未裁**(0.5 第 4 条) ⇒ 排工对象本身尚未确定: 本文写"归入上一条的重设计",
  而 [shm_defect_fixes.md](shm_defect_fixes.md) `:181-186` 原文是**独立条目 + 独立实验判据**。

#### (4) UF-006 —— ✅ **在授权内**; 回收边界已裁; ⛔ (b) 仍待裁

- ✅ **授权认定(2026-09-18)**: 0.1 表本行 `修改边界` 列了文件且**无"不授权"字样**
  (对照 UF-008 / UF-009 明写"⛔不授权") ⇒ 与那两项**不同**, **在授权内**。
  但须拆两段: **(a) 表满可判别**(返回值/日志)= 授权内、**可先做**;
  **(b) 表满后顺带回收** = **产品行为变更**, 0.5 第 2 条待裁 ⇒ ⛔ **未授权**。
- ⛔ **措辞订正(本轮新发现)**: "**无周期性回收**"**不准确** —— `snapshot(bool gc_dead = true)`
  **默认就回收**, 仓里**已有回收点**: 回收 = `socket_pub_sub_ipc.cc:161/252`(**无参调用** ⇒ 吃默认 `true`)、
  `exec/dzipc_list/main.cc:166/175`、`exec/dzipc_pub/src/main.cc:165`、`exec/dzipc_topic_cat/src/main.cc:135`;
  不回收 = `src/dzIPC/auto_ser_cli_ipc.cc:47/92`(ser-cli **判定路径**, 即本行 `修改边界` 保护的
  `gc_dead=false` 语义)、`src/dzIPC/logger/dzipc_log.cc:848/1001`。⇒ 真问题不是"没有回收",
  而是**同一份池在路径间口径不一致**(同一时刻不同路径看到**不同的表**)。
- **回收候选的形状**(⛔ **尚未落码**): 在 `register_entry`(`src/dzIPC/ipc_info_pool.cc:381-407`)
  扫不到空槽**之后**回收一次 → 重试 → 仍失败才 `-1`; 谓词与 `alive` **同一**(`pid_alive`, `:105`)。
- ⛔ **"判定语义不变"须逐消费方举证, 不得笼统宣称**(✅ 2026-09-18 已**扫全仓 `snapshot(false)` 消费方、
  无遗漏项**): ✅ `auto_ser_cli_ipc.cc:47/92`(`find_peer`)与
  `exec/dzipc_pub/src/main.cc:382`(`find_subscribers`, `:43` 过滤 `!alive || !in_use`)都过滤 `!alive`
  ⇒ 死条目"以 `alive=false` 出现"与"被回收后不存在"**等价**; ⚠️ **唯一的反例在日志侧** ——
  `dzipc_log.cc:848-856/1001`(`RecordEndpointMeta`) **只过滤 `!e.in_use`、不看 `alive`** ⇒ 回收会让
  死条目**提前从 endpoint-meta 日志消失**(不改判定, 改**日志内容**) ⇒ ⛔ 必须随修法**显式登记**,
  否则今后有人拿"日志少了几条"当回归。
- ⛔ **契约须改写**: "死条目仍返回且 `alive=false` 不得静默剔除"**不能**作跨进程/跨调用的
  **存储级不变量** —— 回收由**他进程的写路径**触发, 该可见性只能 best-effort。可承重的只有
  **单次调用内**两条: ①调用自身不改段内字节 ②调用期内不剔除已存在条目。
  `test/test_ipc_info_pool.cpp:106-160`(`:139` `present_before`)只保证"**手工 `gc_dead()` 之前**可见",
  ⛔ **不得升格**为"永远可见"。
- **ABI/布局边界(可满足, 须钉死)**: `PoolEntry` 在 `ipc_info_pool.cc:46`, `kRegionSize` `:88`,
  段名 `dz_ipc_info_pool_v1`(`:34`, **自带版本后缀** —— 与 UF-003 的 `CHUNK_INFO__` 无版本标记**不同**)
  ⇒ 三条硬约束: ⛔不新增/不重排 `PoolEntry` 字段、⛔不改 `kRegionSize`/`kMaxEntries`(=512)、
  ⛔不换 `kShmName`。满足三条即"不改布局/ABI", **无需决策**。
- **并发**: 回收必须与 `register_entry` 的扫描**同一把 `ScopedShmLock` 内**(`:385`); 全仓池变更都在该锁内
  ⇒ 串行化 ⇒ 无撕裂读。
- ⭐ **现网基线(2026-09-18 tester 实测, 本节新增)**: `dz_ipc_info_pool_v1` 当前 **`in_use=16/512`**,
  且**这 16 条的 pid 全部已死**, 而产品侧**从不回收** —— 全仓 `gc_dead()` 的**唯一调用点**是
  `test/test_sercli_auto_path.cpp:257`(**仅测试用**) ⇒ ⛔ **该缺陷已在现网发生**, 不是理论风险。
  这也给了 (b) 一条现成的行为判据: 落码后同一基线应**由 16 归零**。
- ⛔ **回收节流限制(三条, 须随落码一并交付; ⛔ 缺任一条即不得宣称闭环)**:
  ① **只放写路径**: 仅在 `register_entry` 扫描**未找到空槽之后**做一次, 摊销只在满载付费;
  ② **回收数为 0 则短时不再重试**: 否则满载稳态(512 条全活 + 持续注册)会**每次注册 512 次 `kill()`
  且在锁内** ⇒ O(n²);
  ③ **节流阈值是设计参数**: ⛔ 不得由 coder 单方定值后即宣称闭环 —— 须 coder 定值、由 reviewer
  用例验证(满载稳态连续注册的吞吐与时延不得劣化), 阈值本身也须写进条目块。
- ⛔ **可诊断性另补**: `-1` 有**三义**(`:384` 池未就绪 / `:388` 取锁失败 / `:407` 表满) ⇒ 回收
  **不解决**可诊断性, 须另补 `ipc::error` 日志; ⛔**不改返回码**(保留调用方分支语义)。
- ⛔ **`pid_alive` 是唯一谓词 ⇒ 只能有界保守, ⛔不得称"根治"**: `gc_dead()`(`:551`)与 `alive`(`:484`)
  **完全不看 `heartbeat_ns`**(`heartbeat()` `:445` 采集了却**无人用于判定**) ⇒ ① pid 复用 ⇒ 死条目被判
  **活** ⇒ 永不回收(这正是表满的根因之一); ② ⛔**禁止引入超时回收**(会把长持有者**误回收**)。

#### (5) UF-008 —— ⛔ 只证据化(证据已完整; 待 reviewer 判 + 写回)

- analyst 结论经 reviewer 复核**成立**: A1 按字面(无发布端)在 `check_topic_state` 直接 `found=false`,
  **连 sniffer 都不建** ⇒ 恒绿、**无判据力**; 可达设置下活订阅端把段访问计数顶在 ≥2 ⇒ 发布端退出
  **不 unlink** ⇒ 段**合法仍在** ⇒ A1 **假红**。⇒ **A1/A2 今天不可作为收口依据**。
- ⛔ **产品码本轮不授权**(0.1 表原文); 若判成立, 限 `exec/dzipc_topic_cat/**` + `test/test_handshake_probe.cpp`。
- ⚠️ 其 §5 触发面属**推断**, ⛔ 不得当"已证"; 修后形态若需实测, **依赖 leader 授权执行面**(此前被门禁拦)。

#### (6) DF-001 – DF-005 —— 全部延期

- 0.5 第 7 条已裁**本轮不进产品修复**; 产品码一律 ⛔。仅 **DF-004** 的扇出可先做**只读证据化**。

#### (7) coder 侧实测结果 —— `buffer.cpp` 落码 + **回滚变异阴性**(⛔ **不是缺陷修复**)

- **事实**: coder 按 0.3.7 的遗留收口了 `src/libipc/buffer.cpp`(仅析构那一行 + 注释, `+11/−1`)。
  ⛔ **回滚变异为阴性** —— 还原 HEAD 后同一注入探针 **rc=0, 不崩**: `pimpl<T>::clear()` 只做
  `clear_impl(static_cast<T*>(this))`, "不舒服"分支即 `mem::free(p)`, 传 `nullptr` **立即返回, 从不读
  `this` 的内存**(反汇编双证: gcc 自出 `test rbp,rbp; je <ret>`)⇒ **该行是形式 UB 清理, 不是崩点**。
- ⇒ ⛔ **本项不满足 0.3 第 5 条, 不得记作"缺陷已修"**。**状态列不动**(`UF-002` 仍为 `✅已修`,
  那是**五类入口**的判决, 与本条无关); 本条**待 leader 裁**: **保留**(零风险、与另四处同口径)
  还是**回退**(免得台账记成"修掉一个缺陷")。
- ⚠️ **连带订正**: 上一轮"该行与 `shm`/`mutex`/`condition`/`semaphore` 四处**逐字同型**"的说法,
  就 `buffer` 而言**已被实测证伪**; 那四处的崩点是 `impl(p_)->成员` 类**读**(阳性对照测的正是那个),
  但**若其析构同样只是 `p_->clear()`, 则同样无变异效力** ⇒ 建议**各自补一次回滚变异**后再决定是否
  宣称修复。⛔ **不在本轮边界, 未动那 4 个文件**。
- ⚠️ **新发现(未修; 修前修后一致; 需另立条目, ⛔本轮未立)**: 分配失败时 `buffer(p,s,d,additional)` 的
  **所有权被静默丢弃、用户回调 `d` 永不触发**(资源泄漏), 且 `empty()` **区分不了**"构造失败"与
  "合法 `s == 0`"; 在"失效态禁抛出"的裁定下**本任务内不可修**。
- ⚠️ **F4 红线复跑仍欠一次**(coder 判无实质影响: `mode` 原样透传、默认参数在**未改动**的
  `include/libipc/shm.h`) ⇒ 请 leader 裁是否补跑。

### 0.3.9 UF-006 落码登记(2026-09-18; ⛔ **已落码, 但当时 ⛔ 未验收** —— 本节**不是** 0.4 写回; ⚠️ **其"未验收/无变异"结论已由 0.3.10 取代**)

> ⚠️ **时效提示(2026-09-18 16:1x 补)**: 本节写于验收门**尚未跑**之时, 其核心结论
> ("无实测 PASS / 无承重变异 / G9 BLOCKED")**此后已被实测推翻** ⇒ ⛔ 引用状态时**一律以 0.3.10 为准**,
> 本节保留为**当时缺口的记录**。⚠️ 且本节 (a) 的 md5 与"用例未改"事实**已过期**, 见 0.3.10(b)(d)。

> ⛔ **性质说明(与 0.3.8 同一口径)**: 本节登记的是**产品的实际改动**, 但**没有任何实测 PASS, 也没有
> 任何承重的变异结果**。0.4 第 1 步(状态列改 `✅已修`)与第 2 步("结果 PASS/FAIL 与关键读数")
> **一项都不满足** ⇒ **不产生 0.4 写回, 不新增任何「§ 修复记录」, 0.1 表状态列一格不动**
> (`UF-006` **维持 `⬜未修`**)。
> ⛔ **"已落码" ≠ "已验收" ≠ "已修" —— 三者不得混读**; 尤其 ⛔ **不得把本节读成"缺陷已修"**。

**来源**: 落码事实取自今日工作树 `git diff` 与文件 md5; 验收门判据取自 reviewer-claude 的共享区文件
`uf006_gate_baseline_reviewer-claude.md`(GATE ARMED, 改动前基线)与
`uf006_gate_pregate_addendum_reviewer-claude.md`(验收可跑性缺口), 均 2026-09-18 16:09。
⚠️ **本节作者(writer)⛔ 未运行任何实验、未跑任何用例**; 下文所有 ✅ 均限**源码级对照**, ⛔ 不是验收结论。

#### (a) 落码事实(可逐位复核)

- **改动面**: 仅 `src/dzIPC/ipc_info_pool.cc`, `+158 / −15`, md5 **`e21e92e386361fe225d0e1acdf9c527b`**
  (reviewer 基线 = HEAD 态 `989b16e2…`, **已变**)。
- ✅ **在 0.1 表 `修改边界` 内**: `include/dzIPC/ipc_info_pool.h` md5 **`e9763c22…`**(= reviewer 基线, **未改**);
  `test/test_ipc_info_pool.cpp` md5 **`d800e3b8…`**(= 基线, **未改**) ⇒ **本轮未新增也未修改任何用例**
  (这一点直接决定 (b) 的 G9 结论)。
- ✅ **未触判定路径**: `git diff -U0` 里出现的 `snapshot` / `gc_dead` **全部是注释行**(`+ *` 开头);
  `snapshot`(含 `snapshot(false)`)与 `gc_dead` 的**函数体不在 diff 内** ⇒ `gc_dead=false` 语义未动。
- ✅ **未触布局/ABI**: `PoolEntry` / `kRegionSize` / `kMaxEntries`(=512) / `kShmName`(`dz_ipc_info_pool_v1`)
  **均未出现在 diff 内**; 新增的节流与诊断状态是 **`.cc` 文件级 `std::atomic`**, ⛔ 不进 `PoolHeader`/`PoolEntry`。
- ✅ **未改返回码**: `register_entry` 仍返回槽号或 `-1`, 三义(`:384` 池未就绪 / `:388` 取锁失败 / 表满)的
  **取值语义不变**; 表满时改为**先回收、再重试一次**。
- **新增结构**: 4 个文件级函数 —— `clear_entry_raw`(复位一条 entry)、`claim_free_slot`(扫并认领空槽)、
  `reap_dead_locked`(**已持锁**前提下的死条目回收)、`diag_register_failure`(限流诊断);
  常量 `kFruitlessReapMinIntervalNs = 250 ms`、`kDiagMinIntervalNs = 1 s`;
  全局 `g_last_fruitless_reap_ns` 与 `g_diag_last_ns[5]` / `g_diag_suppressed[5]`。
- **表满新路径的顺序**(逐行对照): 取 `ScopedShmLock` → `claim_free_slot` → 失败即进表满分支 →
  未节流则 `reap_dead_locked` → **仅当回收数 > 0 时**再 `claim_free_slot` **一次** → 按原因写诊断 → `return slot`。

#### (b) 与 reviewer 验收门 G1–G10 的**源码级**对照 —— ⛔ 本节 ⛔ 不作验收判决

- ✅ **G1 谓词同一性**(源码级): `reap_dead_locked` 判据 = `in_use != 0 && !pid_alive(pid)`, 与 `gc_dead` 同源;
  ⛔ **未引入 `heartbeat` / 超时**(coder 注释亦自陈"只看 `pid_alive`, 不看 `heartbeat`")。
- ✅ **G2 锁范围**(源码级): 回收发生在 `register_entry` **同一** `ScopedShmLock` 内, 且**没有**从锁内调
  公共 `gc_dead()` —— 代码注释显式写明"它会再抢同一把 robust mutex(非递归) ⇒ 同线程自死锁", 并为此抽出
  **不取锁**的 `reap_dead_locked`。(reviewer 建议的名字是 `reclaim_locked`; **名字不同, 形状一致**。)
- ✅ **G3 重试有界**(源码级): 回收后**恰好重扫一次**(`if (reaped_any) slot = claim_free_slot(...)`),
  无 reclaim↔scan 交替。
- ✅ **G5 `snapshot` 零触碰**(源码级): 见 (a) 第三条。
- ✅ **G7 返回码未改**(源码级): 见 (a) 第五条。
- ✅ **G8 清零序列**(源码级): `clear_entry_raw` 清零 `pid` / `kind` / `register_ts_ns` / `heartbeat_ns` /
  `topic_name` / `type_name` / `domain_id` / `extra` **8 个字段** + `in_use.store(0, release)`, 与
  `unregister_entry` / `gc_dead` / `snapshot(gc=true)` 三处**同一序列** ⇒ 不会留下"只清 `in_use` 却留着 `pid`"
  的脏条目(那种条目在 pid 复用下会被误判活)。
- ⛔ **G9 变异判据 = BLOCKED(本轮最关键的结论)** —— ⚠️ **本结论已被 0.3.10 取代(2026-09-18 16:1x)**:
  用例**已新增**、变异**已实测转红**, 本节以下三句**只应读作"当时的缺口记录"**, ⛔ 不得再作为现状引用。
  reviewer 附录已证**全仓没有任何用例把池填满**;
  本节独立复核: `grep -rn kMaxEntries test/` = **0 命中**, 现网基线仅 `in_use=16/512` ⇒
  `register_entry` 的"扫不到空槽"分支**从未在任何测试里被执行过** ⇒ **把回收块改空的回滚变异不会有任何
  断言转红** ⇒ ⛔ **不满足 0.3 第 5 条**(「行为判据 + 变异验证, 缺一不算闭环」)。
  ⛔ **反方向同样成立**: 若回报"跑通了、没红", **同样不构成证据** —— 那是**没跑到**, 不是**没坏**。
  ⇒ 要闭环必须先**新增**一条"填满 → 造恰好 1 条死条目 → 断言 `register_entry` 返回 ≥ 0 → 变异后必须转 `-1`"
  的用例(构造法见 reviewer 附录 §2, 含三条自保约束: 逐个 `unregister` 清理、不假设段里只有自己、
  回收前先显式断言表确实满), 而**新增用例是否在授权内须 leader 拍板**。
- ⛔ **G4 / G6 / G10 本节不判**: ①G4 的节流性质见 (c) 第 2 条; ②G6 的日志差异见 0.3.8(4) 与 (c) 第 1 条;
  ③**G10 回归面一条都没跑**(本节作者未跑任何用例) ⇒ ⛔ 不得记 PASS。

#### (c) 落码带入的**新可观测行为**(⛔ 须随写回一并登记, 不得只说"无行为变更")

1. ⚠️ **库从"静默"变成"往 `stderr` 写字"**: 三条原本**无任何输出**的失败路径(池未就绪 / 抢锁失败 / 表满)
   现在各打一行黄色 `[dzIPC][info_pool] register_entry 失败: …`, **限流 1 条/秒**, 并在下一条里报告其间
   被抑制的条数。⇒ 这是**真实的行为变更** ⇒ ⛔ 任何"本次改动对消费者零可观测差异"的措辞都是错的。
   ⚠️ 该输出发生在**持有跨进程 `ScopedShmLock` 期间**; 函数标 `noexcept` 却使用 `std::cerr`(可能分配、
   管道满时可能阻塞、若流抛异常则 `std::terminate`) ⇒ **风险点, 待 reviewer 判**, 本节 ⛔ 不裁。
   coder 的取舍理由已写在代码注释里(不用 `dzIPC::logger`: 它是 bag 事件记录器、无 warn/error 级, 且其
   endpoint-meta 路径自身会调 `snapshot(false)` ⇒ 持锁回调会再抢同一把非递归锁)。
2. ⚠️ **节流状态是"进程内全局", 不是"段内"**: `g_last_fruitless_reap_ns` 是 `.cc` 文件级 `std::atomic`
   (⛔ 不在 `PoolHeader`/`PoolEntry` ⇒ **零布局/ABI 影响**, 这点满足 G4 的硬要求), 但**每个进程各记一份**
   ⇒ **N 个进程同时满载时, 整表扫描的聚合频率 ≈ N × 1/250 ms**。⇒ ①coder 注释自陈的单进程上界
   (≈ 0.2% CPU)**不能外推到多进程**; ②reviewer 的 G4 原文是"节流状态只能落在 `Impl`" ——
   落在 `.cc` 全局**同为零布局改动**, 但上述 N 倍性质**须由 reviewer 判是否可接受**。
3. ⚠️ **节流只对"空扫"计时 ⇒ 回收不是立即的**: 空扫后 `250 ms` 内再次表满 ⇒ **本轮跳过回收**, 且
   **需要"再来一次注册"才可能回收**(回收只在写路径发生) ⇒ 语义是"表满后**最多**推迟一个窗口",
   ⛔ 不得写成"表满即回收"。coder 已自陈这是**单调改善而非回归**(改动前该场景成功率为 0)。
   ⛔ 另: 阈值 `250 ms` 是 **coder 单方定值**, 尚未经 reviewer 用例验证(见 0.3.8(4) 节流限制第 ③ 条)。
4. ⚠️ 诊断文案本身是**用户可见字符串** ⇒ 一旦发布即成为**事实上的接口**, 改文案 = 破坏性变更;
   若日后要接入正式日志, 须连同本节一起订正。

#### (d) 本轮上限与状态

- ⇒ **UF-006 本轮上限 = 「已落码, ⛔ 未验收」**。⛔ 既**不得记 `✅已修`**(无 PASS、无变异), 也
  **不得记 `🔶部分修`** —— 后者要求"部分行为已变**且已被测到**", 而本轮**一条用例都没跑**。
  ⚠️ **本小节的"未验收"已被 0.3.10 取代(同日 16:1x)**: 实测 PASS 与变异转红**均已到手**;
  ⛔ 但 0.1 表状态列**仍维持 `⬜未修`** —— 理由已换为 **0.3.10(e) 的四条解禁条件**(其中第 ① 条:
  被验证的产物此刻**不在工作树**)。
- ⚠️ 本节 (a) 记的 `test/test_ipc_info_pool.cpp` md5 `d800e3b8…`(="未改用例")**已过期**: 该文件其后
  两度变更(`68d8b02b…` → `4fdcabb0…`, +用例), 见 **0.3.10(b)**。本节相应结论 ⛔ 不得再引用。
- **0.1 表 `UF-006` 状态列维持 `⬜未修`**(本轮**零处状态值改动**), 仅在状态列附注"已落码但未验收"。
  ⚠️ **本条自 2026-09-18 晚起失效**: 状态列其后经 0.3.10(证据)走到 **0.3.11(写回)**, 现为 **`✅已修`** ——
  "维持 `⬜未修`"只描述 **0.3.9 落笔时点**的记账。
- ⇒ **真正的下一步不是写回, 而是先补用例**(G9), 否则本项永远停在"改过但判不了"。

### 0.3.10 UF-006 验收证据登记(2026-09-18; ✅ **实测 PASS + ✅ 变异转红** —— ⚠️ 但其"状态列 ⛔ 不动"只对 **16:2x 落笔时点**成立: 当日已由 **0.3.11** 翻格为 `✅已修`)

> ⛔ **与 0.3.8 / 0.3.9 的关键区别**: 本节**第一次**载入 `UF-006` 的**实测**证据。0.3.9 是纯源码级对照
> (其自陈"未运行任何实验"), 而本节的三条承重读数**都是跑出来的** —— 转述 reviewer-claude 的
> 门结论(seq147)与 `build/uf006_verify/` 下的原始日志。
> ⚠️ **本节作者 writer ⛔ 未运行任何用例**: 下文所有读数均为**转述**, 但每条都附了日志路径与
> 本节作者**亲自复核过的文件 md5**。
> ⛔ **但本节仍不是 0.4 写回, 0.1 表状态列零改动** —— 因为"实测 PASS + 变异转红"只是 0.4 的
> **必要条件, 不是充分条件**: 本轮另有 (e) 的四条解禁条件**未全部满足**, 其中第 ① 条是硬事实
> (被验证的产物**此刻不在工作树里**, 见 (d))。
> ⇒ 正确读法: 本节 = 「**验收证据已到手, 但落笔条件未齐**」, ⛔ **不得读成"缺陷已修"**。

#### (a) 实测读数(三条承重 + 三条辅助)

- ✅ **修复态全绿**: `build/uf006_verify/armF2_20260918_161618/test_fix.log`(库 = 修复版留档 `ff125bb3…` 构建)
  ⇒ `test_ipc_info_pool` **10/10 PASSED**(该测试文件版本 = `4fdcabb0…`, 见 (b))。
- ✅ **变异转红(0.3 第 5 条的承重项)**: `build/uf006_verify/mutation_tree_build.log`(16:17:16, 库 = **改动前源码**构建)
  ⇒ **8 PASSED / 2 FAILED**, **三处断言转红**, 且**跑满有界重试窗口后仍红**:
  - `test/test_ipc_info_pool.cpp:422` `EXPECT_GE(slot, 0)` ⇒ 实测 **`-1 vs 0`**(表满 + 有死条目 ⇒ 未回收、注册失败);
  - `:434` `EXPECT_FALSE(still_there)` ⇒ 实测 **`true`**(死子进程的条目仍在表中 ⇒ 满载路径未回收它);
  - `:462` `EXPECT_NE(log.find("info_pool"), npos)` ⇒ 实测 **`stderr=[]`**(**满载失败静默** ⇒ 连"诊断可观测"这条判据也转红)。
  ⇒ ⛔ **0.3.9(b) 的「G9 = BLOCKED」结论已被本节取代**: 用例**已新增**(见 (b)), 变异**已有断言转红**。
- ✅ **既有面回归**: `build/uf006_verify/regress2_20260918_161631/` ⇒ 三个套件 **20/20 PASSED**
  (`test_dzipc_log` / `test_sercli_auto_path` / `test_dzipc_pub`), 即回收改动**未打破既有日志与 ser-cli 判定路径**。
- ✅ **臂夹具**: `build/uf006_verify/run_arms.sh`(tester-claude 出品)在修复库上 **A/B/C/D/E/F/G 全 `rc=0`**;
  其中 **armE = 「`snapshot(false)` 单次调用字节不变」⇒ `BYTE-INVARIANT: PASS`** —— 这正是
  0.3.8(4) 两条可承重契约里"**调用自身不改段内字节**"那一条的**实测**支撑(此前只有源码级推断)。
- ⚠️ **节流在实测中直接可见(非推理)**: 修复臂的 `stderr` 里同时出现两条**不同原因**的诊断 ——
  "表满且本轮整表回收**未找到死条目**"与"表满且本轮整表回收**被空扫节流跳过**"
  ⇒ 0.3.9(c) 第 3 条"**回收不是立即的**"有了实测凭据, 且**两条原因都可区分**(不是同一句话)。

#### (b) 用例**已新增**(订正 0.3.9(a)(b) 的"未改用例"事实)

- `test/test_ipc_info_pool.cpp` 指纹链: `d800e3b8…`(HEAD 态) → `68d8b02b…`(reviewer seq147 记的中间版)
  → **`4fdcabb041f4b3c5ca16565a0524904f`(18242 B, mtime 16:15:33)**(当前盘, 与 reviewer 留档
  `build/uf006_verify/rev1615_test_ipc_info_pool.cpp` **两份 md5 一致**)。
- 用例条数: HEAD **7 条** → 中间版 **9 条**(reviewer 的 "9/9 PASSED") → 当前 **10 条**。
- 新增的三条: `:359 FullTableRegisterReapsDeadChildEntry`、`:442 FullTableFailureStaysObservableAndLeavesSnapshotIntact`、
  `:473 SnapshotFalseKeepsDeadEntryWhileSnapshotTrueStillReaps`; 辅助函数 `fill_table`(`:54`)按
  "**第 kMaxEntries+1 次**注册返回 `-1`"判满, ⛔ **不硬编码 511** —— 满足 reviewer 附录 §2 的三条自保约束。
- ⚠️ **该文件仍在变动(时效限定)**: 本节落笔时(16:2x)它已再次变更 ——
  **`4df1b1fa0fcd372ea43a8d0616b1bc28`(19539 B, mtime 16:20:13, 10 条用例)**, 即
  `4fdcabb0` **不是冻结点**, ⛔ 引用时**必须重取 md5**; (a) 的 10/10 实测是**对 `4fdcabb0` 那一版**跑的。
- ⛔ **该文件不在 0.1 表 `UF-006` 行的 `修改边界` 列**(该列只写 `ipc_info_pool.cc` + 头文件)
  ⇒ **"把填满池的用例落进 `test/` 是否在本次授权内"仍须 leader 拍板**(reviewer seq147 风险 4 明列),
  即 (e) 第 ④ 条。支持性事实(⛔ 不构成授权): 0.9 分工表(`:939`)给 coder-claude 的职责里含 "**UF-006 用例侧**"。

#### (c) reviewer 门结论 + 三条**必须随写回一并登记**的限定

- **门结论 = `PASS(有条件)`**(reviewer seq147, 16:15:55): `G1`–`G8` **源码级全过**, 其中
  **⛔ 红线条 `G2` 已避开** —— `reap_dead_locked` 不自抢锁, 代码注释显式写明"不能调 `gc_dead()`:
  非递归 mutex 同线程自死锁"; 仅一次重扫、非循环。⚠️ 该结论锚定的指纹是 `e21e92e3…`, 其后 reviewer
  又留档一版 `ff125bb3…`(两者**不同**; 本节 (a) 的实测用的是 `ff125bb3…` 这一版)。
- ⚠️ **`G6` = 唯一真实可观测行为差异(⛔ 必须写明, 不得写"任何消费者都看不到差异")**:
  `src/dzIPC/logger/dzipc_log.cc:848-856` 与 `:1001`(`RecordEndpointMeta`)**只过滤 `!e.in_use`、不看 `alive`**
  ⇒ 回收会让**死条目提前从 endpoint-meta 日志消失**。已扫全仓 `snapshot(false)` 消费方(无遗漏项):
  `auto_ser_cli_ipc.cc:47/92`(判定路径, 过滤 `!alive` ⇒ **等价**)、`exec/dzipc_pub/src/main.cc:43/382`(**等价**)、
  `dzipc_log.cc:848/1001`(**不等价 —— 即本条**)。
- ⚠️ **诊断持锁写 = 遗留项, 且 ⛔ 该结论只对旧版成立(须精确区分)**: reviewer 风险 2 指出三条满载诊断以
  **无缓冲 `std::cerr` 在持跨进程锁期间**写出 ⇒ 终端/管道阻塞可拖住**全宿主池**。
  ⚠️ 本节作者复核**留档版** `ff125bb3…`: 诊断调用已移到 **解锁之后**(`:555-556`), 代码注释明写
  "限流只管频率, 不管单次时长" ⇒ **"诊断持锁写"在留档版上已不成立**。
  ⛔ 但因 (d) 的缘故, 该修正**尚未对任何在树版本成立** ⇒ 写回时须按**最终落码版**重核一次。
- ⚠️ **判据的环境前提(实测已假堵一次, ⛔ 必读否则误判)**: 池是**全机共享段**。若宿主池被更早的运行
  留在"满且 pid 复用后被判活"态, **修复臂也会红**, 但**签名不同**:
  `:389`(子进程未能注册)/`:394` 与 `:452`(`fill_table` 一槽未填)= **环境脏**;
  `:422` / `:434` / `:462` = **真变异信号**。⇒ 跑判据前**必须先 `uf006_probe reset` 并核对当前池占用**。
  干净池下修复臂与变异臂跑完 `total=0`, **不毒化宿主状态**(已确认)。

#### (d) ⛔ 硬事实: 被验证的产物**此刻不在工作树里**

- 时间线(本节作者逐次 `md5sum` 亲测, 非转述):
  - `16:14` 修复版在树: `989b16e2…`(HEAD 态) → `e21e92e3…` → **`ff125bb3…`**(24157 B)。
  - `16:16:05` reviewer 留档 `build/uf006_verify/rev1614_ipc_info_pool.cc` = **`ff125bb3…`**(24157 B)。
  - **`16:16:18`** 树内 `src/dzIPC/ipc_info_pool.cc` **回落到 `989b16e2…`(17129 B)**;
    此后连续 7 次采样(跨 ~40 s)**稳定于此**, 且 `git show HEAD:src/dzIPC/ipc_info_pool.cc | md5sum`
    **逐字节等于它** ⇒ **该文件 == HEAD 原样, 即 UF-006 的产品改动当前不在树上**。
  - 本节作者复核留档版的内容面: `snapshot` 与 `gc_dead` **函数体与 HEAD 逐行一致**;
    `PoolEntry` / `kRegionSize` / `kMaxEntries` / `kShmName` 的**定义**零变更(改动里出现的 `kMaxEntries`
    只是新增 helper 的循环上界)⇒ ✅ (a)(c) 引用的"未触判定路径/未改 ABI"两条对留档版成立。
- **已知原因(⛔ 不是"有人放弃修复")**: ①coder-claude **仍持有** `src/dzIPC/ipc_info_pool.cc` 的文件锁
  (至 `17:09:29`, 用途 = "UF-006 最小修复: register_entry 表满路径回收死条目 + 节流 + 最小诊断");
  ②`mutation_tree_build.log`(16:17:16)表明**变异臂构建会临时用改动前源码覆盖树文件**
  ⇒ 判定为**在途的构建中间态**, 而非最终回退。
- ⛔ **但在写回时点上它仍是事实**: `✅已修` 的含义是"**产品里**该缺陷已修"。若此刻记 `✅已修`,
  任何人去 `git show HEAD:` 或检视工作树都会发现**没有任何回收代码** ⇒ 该格即为**失真**。
  **故本轮 ⛔ 不翻状态列**(这正是"只有实测 PASS+变异才改状态"之外的另一半: 证据够 ≠ 落笔条件够)。
- ✅ **追踪: 已还原(2026-09-18 16:22 复核)** —— `src/dzIPC/ipc_info_pool.cc` 又变回 **`ff125bb3…`**
  ⇒ 上文"此刻不在工作树"**自 16:22 起失效**, **解禁条件 ① 已满足**; (d) 保留为**当时(16:16–16:22)的真实状态**,
  ⛔ 不得据它判"修复不存在"(留档 + 还原双证该修复是 coder 的在途产物, 而非被放弃)。
  ⚠️ 但 **⛔ 状态列仍不动**: 条件 ② 随之失效 —— (a) 的 10/10 是对**测试文件 `4fdcabb0`** 跑的, 而该文件
  此刻已是 **`4df1b1fa`** ⇒ 条件 ② 必须在**当前指纹对**上**重跑**; ③(coder 回报)、④(leader 拍用例授权)亦未齐。
- ✅ 修复版**未丢失**: 留档在 `build/uf006_verify/rev1614_ipc_info_pool.cc`(`ff125bb3…`, 24157 B)。
  ⚠️ 另: 0.3.9(a) 记的改动面 `+158 / −15` 是对**更早一版**(`e21e92e3…`)的读数;
  留档版(`ff125bb3…`)按 `diff` 计数为 **169 行新增 / 19 行删除**(单文件)。

#### (e) 解禁条件 —— ⛔ 四条**全满足**才可改状态(前三条可机械核对)

1. `md5sum src/dzIPC/ipc_info_pool.cc` **等于修复版指纹**(⛔ **不得**等于 `989b16e2…`, 即 HEAD 态);
2. 在**该指纹**上重跑 `./build/bin/test_ipc_info_pool` ⇒ **全绿且 `rc=0`**(今天应为 10/10);
3. coder-claude 的落码回报**已落 `results.jsonl`**, 且其改动面与 reviewer 审查范围一致
   (**未改** `snapshot` / 头文件 / `kRegionSize` / `kMaxEntries` / `kShmName` / `register_entry` 返回码)——
   reviewer seq147 明写"若其报告含范围外改动, 本结论须重确认";
4. **leader 拍板**"把填满池的用例落进 `test/` 在本次授权内"(见 (b))。
- ⇒ 四条满足后, 下一位落笔者按 **0.4 四步**执行: ①状态列改 `✅已修`; ②条目块写四要素
  (执行的命令 / 结果与关键读数 / 日期 / **变异验证 = (a) 的三条红**); ③详情搬进
  `shm_defect_fixes.md` **新增「§ 修复记录 14」**(现有末号为 §12 = UF-001、§13 = UF-002),
  ⛔ 并**必须把 (c) 的 `G6` 日志差异与节流/环境前提三条限定一并搬入**; ④0.1 表保留指针行。
- **四条现状(2026-09-18 16:22 复核)**: ①✅**已满足**(树内已还原为 `ff125bb3…`); ②⛔**须重跑**
  (上次 10/10 的测试文件是 `4fdcabb0`, 现已变更); ③⛔ coder 回报未落 `results.jsonl`; ④⛔ leader 未拍。
  ⇒ **四条未齐 ⇒ 状态列不动**(⛔ 尤其不得因为"①已满足"就跳过 ②③④)。
- ⛔ **在四条满足之前**, 本节**只**是「验收证据登记」, **不是** 0.4 写回,
  ⛔ 不得据本节把 0.1 表状态列读成已修, 也 ⛔ **不得据本节新增「§ 修复记录」**。
- ✅ **时效(2026-09-18 晚复核)**: 上述四条**已全满足**(逐条见 **0.3.11(a)**), 故本节末三行所述的
  "不翻状态列 / 不新增 § 修复记录"**均已失效** —— 0.1 表状态列已翻 **`✅已修`**,
  `shm_defect_fixes.md` **已新增「§ 修复记录 14」**。本节保留为**验收证据的原始登记**(不再代表终态)。

### 0.3.11 UF-006 写回(2026-09-18; 判定 `✅已修` —— **表满路径回收死条目**已落码并验收)

> ✅ **本节是 0.1 表 `UF-006` 行的 0.4 写回**: 第 1 步(状态列改 `✅已修`)与第 2 步(四要素)在本节,
> 第 3 步(详情搬移)= `shm_defect_fixes.md` **§ 修复记录 14**, 第 4 步 = 0.1 表保留**指针行**。
> ⚠️ **0.3.8 / 0.3.9 / 0.3.10 都不是写回**(它们各自显式声明过), 本节是。
> ⛔ **三者此前被刻意区隔, 现同时成立**: 落码 = 树内 `ff125bb3…`; 验收 = `10/10` + 变异**三臂**转红;
> **已修 = 本节**。⚠️ "已落码"与"已验收"仍**不等于**"已修" —— 后者要求前两者 + 本节(落笔)。

#### (a) 解禁条件 ①–④ 逐条兑现(0.3.10(e) 的四条 ⇒ 全齐)

| # | 条件 | 现状(2026-09-18 复核) |
|---|---|---|
| ① | 树内源 md5 == 修复版 | ✅ **已满足**(16:22 还原起稳定): `src/dzIPC/ipc_info_pool.cc` = `ff125bb35e69b631c0d250ea3837f962`(24157 B) ⛔ **≠ HEAD** `989b16e2…` |
| ② | 在该指纹对上重跑全绿 | ✅ **已满足**: 源 `ff125bb3…` + 测试 `4df1b1fa0fcd372ea43a8d0616b1bc28`(19539 B) ⇒ 测试二进制 `ee3ee95d…`; 库 `c54bf472…` ⇒ **`10/10 PASSED` ×3 连跑 `rc=0`**, 无 flake |
| ③ | coder 落码回报已落 `results.jsonl` | ✅ **已满足**: coder **seq150**(16:23:33)申报 `ff125bb3…` + `4df1b1fa…`(新增 3 条用例), 且明写**未改** `snapshot` / `gc_dead=false` 语义 / 返回码 / `PoolEntry` / `kRegionSize` / `kMaxEntries` / `kShmName` / 头文件 ⇒ **改动面与 reviewer 审查范围一致**, reviewer seq147 的"越界须重确认"前置**不触发** |
| ④ | leader 拍"用例落进 `test/` 是否在授权内" | ✅ **已满足** —— 2026-09-18 批复**授权落盘**, 并据此下发本次写回指令 |

- ⚠️ **0.3.10(d) 记的"产物不在工作树"是 16:16–16:22 的**在途构建中间态**(成因: coder 变异臂构建临时用
  改动前源码覆盖树文件), 已于 **16:22 起失效** —— 不是"修复被放弃"; 16:22 之后源文件**稳定**
  (验证窗口首尾两次取指纹一致, tester seq151)。
- ✅ **本节作者独立重取的指纹**(非转述): 树内两文件 == 留档 `build/uf006_verify/rev1621_ipc_info_pool.cc`
  (`ff125bb3…`)与 `rev1621_test.cpp`(`4df1b1fa…`)逐字节同一; 库 `armF3/libipc.so.3` = `c54bf472…`
  **含** `整表回收` 字符串, 而变异库 `armR` = `2188cb72…` **不含**; 测试二进制 `rev1621_test.bin` =
  `ee3ee95d…`, 与修复臂/变异臂日志中记录的 `testbin` **同值** ⇒ 两臂用的是**同一份**测试二进制。

#### (b) 四要素(0.4 第 2 步)

1. **执行的命令**:
   - 构建: `cmake --build build -j8 --target ipc --target test_ipc_info_pool` ⇒ `rc=0`(无警告);
   - 判据: `LD_LIBRARY_PATH=build/uf006_verify/armF3 ./build/bin/test_ipc_info_pool` ⇒ `rc=0`;
   - 一键最小复跑: `bash build/uf006_verify/reverify.sh build/uf006_verify/armF3 build/uf006_verify/rev1621_test.bin`
     (承重臂 B/C/D/E + 既有测试床)。⚠️ **必须显式指定臂目录**: 不设 `LD_LIBRARY_PATH` 会静默解析到
     旧装 `/usr/local/lib/libipc.so.3`(07-09, `6204ee77…`)⇒ 读数无效(tester seq149 环境陷阱)。
2. **结果与关键读数**(2026-09-18, 全部 `rc` 已核):
   - **`10/10 PASSED`, `rc=0`**(连跑 3 次恒定 ≈ 1.41 s, 无 flake); `FullTable*` 子集 `rc=0`;
   - 满载 512 全死 ⇒ **下一次 `register_entry`**: 修复前 `-1` → 修复后 **`slot=0`**(`in_use` 512→1)⭐承重;
   - 满载 511 死 + 1 活 ⇒ 修复前 **FAIL** → 修复后 **PASS**, 且活条目 `slot=511` **原样存活** ⭐承重;
   - 满载 512 全活 ⇒ **两臂均 `-1`**(返回码不变), 且修复臂**留下诊断行**(变异臂此处**无输出**);
   - `snapshot(false)` 单次调用 ⇒ 段字节 **`BYTE-INVARIANT: PASS`**(逐字节不变);
   - 既有回归: `test_sercli_auto_path` 15 / `test_dzipc_log` 20 / `test_dzipc_pub` 16 全 `rc=0`;
   - 8 进程并发撞满载池: 8/8 `rc=0`, slot 唯一 `0..7`; 收尾 `/dev/shm` info_pool 残留 **0**。
3. **日期**: 2026-09-18(基线 HEAD = `3818899687efefd35f8514acdff565226bec7cce`)。
4. **变异验证**: **三臂各自转红**(`build/uf006_verify/final_mutant_20260918_162452/mutation_testbed.log`):
   - **臂 A**(整文件回滚到 HEAD)⇒ `rc=1`, 红在 `test/test_ipc_info_pool.cpp:444`(`EXPECT_GE(slot,0)`
     实测 **`-1 vs 0`**)与 `:447`(死条目仍在表中), `:475`("不得静默")亦红;
   - **臂 B**(只把回收块改为空)⇒ 红点与 A **完全相同**(`:444`/`:447`), 而"不静默"用例**仍绿**;
   - **臂 C**(只删诊断调用)⇒ **只有 `:475` 红**(实测 `stderr=[]`), 回收用例**仍绿**;
   ⇒ 三条断言**各自**被**独立的**回滚方向咬住(不是"跑绿了就算"); 变异臂跑完已还原, 终树与留档版逐位一致。
   ⚠️ **行号必须连测试版指纹一起引**: 上述 `:444`/`:447`/`:475` 属**最终版** `4df1b1fa…`;
   0.3.10(a) 记的 `:422`/`:434`/`:462` 属更早的 `4fdcabb0…`, **两组不可混用**。

#### (c) 三条限定已随写回搬入「§ 修复记录 14」(0.4 第 3 步)

- ⛔ **措辞订正(tester seq149/151 两次点名, 必读)**: 回收**只在表满路径**; 池**未满**时死条目
  **不会**被"下一次 `register_entry`"回收(臂 A 逐字证明 100→100)⇒ 凡引用本修复必须写
  "**表已满时**的下一次 `register_entry`"。
- ⛔ **`G6` = 唯一真实可观测的行为差异**: `src/dzIPC/logger/dzipc_log.cc:848-856` 与 `:1001` 的
  `RecordEndpointMeta` **只过滤 `!e.in_use`、不看 `alive`** ⇒ 回收使死条目**提前从 endpoint-meta 日志
  消失**。全仓 `snapshot(false)` 消费方已扫尽, 另两处(`auto_ser_cli_ipc.cc:47/92` 判定路径、
  `exec/dzipc_pub/src/main.cc:43/382`)均过滤 `!alive` ⇒ **等价**。⛔ 不得写成"任何消费者都看不到差异"。
- ⛔ **节流 + 判据环境前提**: 空扫节流 **250 ms**(`:217`, 只对**空扫**计时, 扫到东西立即解除;
  代价 = 回收最多被推迟一个窗口且需**再来一次**注册, 相对修复前的"恒失败"是**单调改善**,
  但**窗口边界未独立量化**; 诊断限流 `1 s`/原因, `:221`); 池是**全机共享段** ⇒ 跑判据前必须
  `uf006_probe reset` 并核对池占用, **假红签名** = `fill_table` 未能填到表满(`:408` / `:465`),
  **真变异签名** = `:444` / `:447` / `:475`。
- ✅ **"诊断持锁写"这条遗留已在落码版上闭合(本节作者直接核过树内文件, 非转述)**: `:552` 作用域
  结束(解锁)后 `:555-556` 才输出诊断, `:553-554` 的注释原文写明理由("限流只管频率, 不管单次时长")
  ⇒ ⛔ 该风险**只对更早的 `e21e92e3…` 成立**。⚠️ 但**单次输出的时长仍无上界**。

#### (d) 本轮未闭项(⛔ 不得据本节读成已修)

- ⚠️ **pid 复用假活仍不可解**: 回收谓词与 `gc_dead()` 同(只看 `pid_alive`, 不看 `heartbeat_ns`)
  ⇒ 表满"根因之一"仍在; 已裁**只接受有界保守**, 改动需另行授权 + 阈值来源。
- ⚠️ **未测**: 真产品进程崩死 → 池满 → 再注册的**端到端**。
- ⚠️ **0.5 第 2 条未单独拍板**: 本轮以 leader 的**写回指令**为"表满时顺带扫死条目"这一形态的
  事实认可; ⛔ 若日后改口径(例如改成"未满也回收"), 本条的变异验证须**重做**。
- ⛔ **相邻项状态列一律未动**(本轮只翻 `UF-006` 一格): `UF-003` 仍 **`⬜未修`** 且**判据侧 BLOCKED**
  (见 0.3.8(1) 与 tester seq146: 残池态/清池态**逐用例完全相同** ⇒ 判据无判别力, 补判据须新写一条
  能在**合成耗尽池**上转红的用例 —— 残留池的水位/残段现状仍是未闭项); `UF-004` **当时**仍 **`⬜未修`**(⚠️ **该格已于同日 17:1x 由 0.3.12 翻为 `🔶部分修`** —— 本句只对 16:3x 时点成立)
  (写回上限 `🔶部分修`, 见 0.3.8(2)); `UF-008` / `DF-001–DF-005` 未动。

### 0.3.12 UF-004 写回(2026-09-18 16:59–17:14 落码验收; **22:44 指纹订正 + expected_red 补记**; 判定 `🔶部分修` —— opt-out 开关已落码并验收; ⛔ **不是** `✅已修`)

> 本节是 0.4 协议第 2 步要求的**条目块**, 也是本项的**唯一现行判定**。
> 详情已按第 3 步**搬入** [shm_defect_fixes.md](shm_defect_fixes.md) **§ 修复记录 15**, 0.1 表 `UF-004` 行保留**指针行**。
> ⛔ 状态列写 **`🔶部分修`**: 本次解除的是**强制性**(库不再强装处理器), ⛔ **不是**消除现象 ——
> 默认路径**按设计逐位不变**, 所以"默认观测量消失"这件事**没有发生**; 理由见 (c) 与 (d) 第 1 条。

#### (a) 0.4 四步对照

| 步骤 | 要求 | 本项落地 |
|---|---|---|
| ① | 0.1 表该行**状态列** | `⬜未修` → **`🔶部分修`**(五个口径写入该格; ⛔ 全表**无**"已修"字样) |
| ② | 条目块写**四要素** | 本节 (b); 限定与未闭项一并写入 (c) / (d) / (e) |
| ③ | 详情搬 `shm_defect_fixes.md` 新增「§ 修复记录 N」 | **§ 修复记录 15** |
| ④ | 0.1 表保留**指针行** | 该行 `详情指针` 列 = `§4 + 0.3.8(2)(裁定) + 0.3.12(🔶写回; 指针行)` |

⚠️ **一处显式偏离(须 leader 知悉)**: reviewer seq155 建议在 0.1 表"**新增两列**"区分"默认不变"与"opt-out 生效"。
该表是 **9 列 / 每行 10 竖线**的固定结构, 加两列须改**全部 16 行**。本项**未**动表结构, 改为
**把两组口径写进状态格 + 在 § 修复记录 15 里给"默认路径 vs opt-out 路径"对照表** ——
判据可读性不降, 且把改动面锁在本行(与 0.4 "写回是搬移"一致)。

#### (b) 四要素(0.4 第 2 步)

**日期**: 2026-09-18(落码 16:59 起; A3b 返工 17:10; 独立验收 17:0x–17:14)。

**指纹(写回时刻在**树内**重取, ⛔ 不是引用他人快照)**

| 对象 | 值 | 与 HEAD 的关系 |
|---|---|---|
| `src/dzIPC/dzipc.cc` | `ae2dd301209d5e3bc792da1b5d4f7a83`(208 行, +33/−0) | ≠ HEAD `df65c60dc5647437dd5dd7cd0552adf6` |
| `include/dzIPC/dzipc.h` | `743cf3bc0bf421f2db1e52db93991c03`(112 行, +13/−0) | ≠ HEAD `38767100df0d8bbb5f047b514c5b95e7` |
| `test/test_uf004_shutdown_monitor_optout.cpp` | `757b4e5f911a247adc32667b4ba3fafc`(486 行, 7 臂) | 新增文件 |
| `tools/sercli_live_probe/sercli_live_driver.cc` | `3ffe1307c876220dea01648cce3c1bc9`(+22/−1) | ≠ HEAD |
| `tools/sercli_live_probe/exit_semantics_matrix.sh` | `7ddaef96b9604186c9216bd05c7fab96` | 新增(未跟踪) |
| `tools/sercli_live_probe/uf004_optout_acceptance.sh` | `d278068f6db821afafb19d93da42e7e4`(22:44 订正; 写回时刻为 `997d9fd9420ef00f78a6ef803e9efbba`) | 新增(未跟踪); 订正原因见 (f) |
| 库 `build/lib/libipc.so.3` | `ed5a026508fea1e516f061795c373bf6`(22:44 重取; 写回时刻为 `b7e3c9caba65ba6ff6c37af995abf2f8`) | ⚠️ **树库已漂移** ⇒ 已不满足本表绑定条件, 见 (f) |
| `build/bin/test_uf004_shutdown_monitor_optout` | `7409f233b2c240ffcf6848c9a22116ef` | — |
| `build/bin/test_dzipc` | `ed808901015313bcc0f65a0a09f9f3be` | 既有回归 |
| 驱动二进制 `tools/sercli_live_probe/bin/sercli_live_driver` | `f295bbb0d5b7bb944c67326d8b9d70a0` | — |

⚠️ **"移动靶"纪律**: 写回前**重取一次**并与 coder / tester 报告值**逐一对齐**(承 UF-006 的教训 ——
变异臂构建会临时用改动前源码覆盖树文件, 且 `build/lib` 当日曾在 `b7e3c9ca` ↔ `12ae64ca` 之间来回)。
⇒ 本写回**绑定的是上面这张表**: 任一项再变, 即须重跑 (b) 四条命令后才能引用本节读数。

**执行的命令(可粘贴)**

```bash
# ① 新用例(7 臂)
./build/bin/test_uf004_shutdown_monitor_optout
# ② 验收矩阵(预注册断言表 + 符号门 + PASS/FAIL 汇总)
bash tools/sercli_live_probe/uf004_optout_acceptance.sh
# ②' 用冻结库副本复跑(可选)
UF004_LIB=<冻结副本路径> bash tools/sercli_live_probe/uf004_optout_acceptance.sh
# ③ 路径矩阵(默认臂; ⛔ 不传 --optout 时输出必须与 14:30 基线逐字段一致)
bash tools/sercli_live_probe/exit_semantics_matrix.sh --rounds 2 --count 3 --out <dir>
# ④ 既有回归
./build/bin/test_dzipc
```

**读数(PASS / FAIL 与关键值)**

| # | 门 | 读数 | 判定 |
|---|---|---|---|
| 1 | 新用例 | **7/7 PASSED**(408 ms), 连跑 5 次全绿 | ✅ |
| 2 | 验收汇总 | **PASS 28 / FAIL 0**(库 `b7e3c9ca`); `--reuse` 复核同结果 | ✅ |
| 3 | **A0 默认不变门**(承重) | direct 4/4 `app_handler_alive`(SUMMARY=1 / 残留 own=0); factory **4/4 `library_exit0_fast`**(SUMMARY=0 / **残留 own=17**); rc=0、`by_signal=0` —— 与 14:30 基线**及落码前库 `c54bf472`** 逐字段一致 | ✅ **默认路径逐位不变** |
| 4 | **A1 opt-out 生效**(承重) | factory 由 `library_exit0_fast` **翻成** `app_handler_alive`: rc=0 + 出现 SUMMARY + **`residue_own=0`** + **`optout_ret=1`**; 探针另证"库没偷处理器" | ✅ |
| 5 | A1③ 变红方向(⛔ 不可省) | opt-out + 应用**未装**处理器 ⇒ **rc=143 / `by_signal=15`**、`sigcgt=0`、无 SUMMARY、残留 23 | ✅ **硬杀**, ⛔ **不得读成"残留已修"** |
| 6 | A3 晚调用 | opt-out 在构造**之后** ⇒ **`ret=0`**、库已接管、rc=0 / 无 SUMMARY / 残留 17 ⇒ 与默认路径**逐位相同** | ✅ |
| 7 | A2 / A5 只关隐式路径 | opt-out 后**显式** `StartShutdownMonitor()` **仍照装** ⇒ 公开 API 无回退; `RequestShutdown()` 只剩置位语义(无监控线程消费) | ✅ |
| 8 | ABI | `nm -D` 980 → **981** 符号(恰 +1: `_ZN5dzIPC22DisableShutdownMonitorEv`)、删除 0; SONAME 仍 `libipc.so.3` | ✅ **纯加性** |
| 9 | 既有回归 | `test_dzipc` **8/8 PASSED**(含 `CtrlCSignalCapture` = "后装者胜"语义) | ✅ |
| 10 | 残留差集 | factory 默认 **17 段** → 开了 opt-out **0 段**(全消) | ✅ 与 A1 自洽 |
| 11 | 落码面机械核对 | G0②: `std::signal`(两行) / `100 ms` 轮询 / `StopDzipcLog` / `CleanupIpcInstances` / `std::exit(0)` / `detach` / `once_flag` 在 `git diff -U0` 中**±行数为 0** | ✅ **未动退出路径** |

⚠️ **`exit_elapsed_ms` 全程未作判据**: analyst 实测库路径 1–5 ms 与本记录 47–83 ms **同量级不可分**,
⛔ 任何"用它区分 direct / factory"的读法都是错的。本节出现的耗时只作**自洽性佐证**
(例如 101 ms ≈ 100 ms 监控轮询周期 ⇒ 确实以"库接管退出"结束)。

✅ **本节作者在写回时刻重跑了承重的那一条命令**(其余不重跑, 见下): `bash tools/sercli_live_probe/uf004_optout_acceptance.sh`
再次得到 **PASS = 28 / FAIL = 0**, 且四个探针臂**逐字段复现** —— `early`: rc=0 / `by_signal=0` / `optout_ret=1` /
`post_factory_term=app` / residue=0; `late`: `ret=0` / term=other / residue=17; `noapp`: **rc=143 / `by_signal=15` /**
`sigcgt=0` / residue=23; `M1noapp`: **rc=0 / `sigcgt=1`**(机械翻转); `M1early`: residue=17;
`explicit`: term=other / `sigcgt=1` ⇒ 上表**第 2、4、5、6、7 行**与**第 3 行的 A0 门**(该脚本自带, n=2+2)
由本节作者**独立复现**, ⛔ 不再是"转述 tester"。⚠️ **未重跑**的是: 第 1 行(新用例 7/7)、第 3 行的
`exit_semantics_matrix.sh --rounds 2 --count 3` 默认臂、第 9 行(`test_dzipc` 8/8) —— 这三条仍是 **tester seq159** 的读数。

**变异验证(0.3 第 5 条)**

| 变异 | 做法 | 结果 |
|---|---|---|
| **M1**(承重, ⛔ 不动产品码) | LD_PRELOAD 把 opt-out 判定**钉死为 `false`** | 矩阵 factory **4/4 回退**成 `library_exit0_fast` / 残留 17 / `ret=0` ⇒ **A1 必转红**; 无处理器臂由 rc=143 **机械翻转**为 rc=0 ⇒ **反向判据本身也承重**; 新用例 **7 臂中 4 条转红** |
| **M1a–M6**(用例侧) | 删置位行 / 恒返 `false` / 判定搬进 `call_once` / 恒返 `true` / 删两行 `std::signal` / 只删 `Ensure` 内判定 / 让 `StartShutdownMonitor` 也置位 | 每个变异**各自**打红对应断言; 其中 **M2(搬进 `call_once`)只打红 `Explicit` 臂** ⇒ "搬错会**永久静默**杀掉公开 API"这条**只有该臂能抓**; **M6 只打红臂 7** ⇒ 返回值语义被覆盖 |
| 第二变异 | 抓到树构建窗口里出现的库 `12ae64ca`(反汇编成功路径为 `xor eax,eax` = **恒 `false`**) | 验收侧 3 条红 + 用例侧 4 条红 |
| **阴性对照(方向正确性)** | 两条**默认路径**用例(`SigDflBaselineDiesBySignal` / `DefaultPathLibraryTakesOverExitZero`)在**两种变异下都保持绿** | ✅ 变异只打 opt-out 相关断言 |

⇒ **0.3 第 5 条满足**: "行为判据 + 变异验证"两条都在, 且**变异能把修复打回原形**。
⛔ 但**变异证明的是"开关承重"**, **不是**"默认路径的观测量消失" —— 这两句完全不同, 见 (c) 第 ③ 行。

#### (c) 五个口径必须分开读(⛔ 合并任何一个都会失真)

⚠️ 这五条**不是"补充说明", 是判定的组成部分** —— 0.1 表状态格里那五个短语就是它们的缩写。

| 口径 | 含义 | 证据 |
|---|---|---|
| ① **强制性已解除** | 应用**可以**在首个 IPC 构造前自行接管退出, 库**不再强装**处理器 | A1 `optout_ret=1` + 新用例臂 2 / 臂 7 |
| ② **opt-out 路径已验证(归零)** | 走 opt-out 时应用处理器生效, **段残留 0** | A1: `app_handler_alive` + `residue_own=0`; 差集 17 → 0 |
| ③ **默认路径保持(逐位不变)** | 不调 opt-out ⇒ 行为**逐位不变**; factory"无 SUMMARY + **残留 17**"**按设计仍在** | A0 与 14:30 基线 / 落码前库逐字段一致; A3 晚调用亦逐位相同 |
| ④ **残留 17 是预期不是回归** | 17 = **承载 topic 映射的段未 unlink**(根因 = 分离线程里 `std::exit(0)` 不展开栈), ⛔ **不是**"17 个段泄漏" | M1 之下 17 段**原样**出现 ⇒ 与开关无关; 逐段清单见 0.3.4 |
| ⑤ **无自有处理器 + opt-out = `SIG_DFL` 硬杀** | 对"**没有**自有处理器、今天靠库 `exit(0)` 收场"的应用(如 `exec/dzipc_list`), 开 opt-out **比现状更差** | A1③: rc=143 / `by_signal=15` / 无 SUMMARY ⇒ ⛔ 修复是**按应用分类有条件**的, **不存在"全局已修"的措辞** |

#### (d) 三条边界(⛔ 一条都不能被这次改动改写)

1. ⛔ **不改 UF-009, 默认残留不归零**: opt-out **只**交出"装不装处理器", **不动** `std::exit(0)` 那条退出路径
   —— `StopDzipcLog()` / `CleanupIpcInstances()` / 退出码恒 0 / detached 线程**全部逐字节未变**
   (见 (b) 第 11 行的机械核对) ⇒ **`UF-009` 的状态与证据一格未动**, 默认配置下的 17 段残留**照旧**。
2. ⛔ **Python 面不可达(API 未绑 Python)**: `python/src/interface.cc` **不在**本次允许改的文件内
   ⇒ (i) 该 API **没有** Python 绑定, `dzviz` / `dzplot` / python 示例**无法 opt-out** —— 它们仍走默认路径,
   仍被库 `std::exit(0)` 收场; (ii) 即便从 C++ 层 opt-out, `RequestShutdown()` / `IsShutdownRequested()`
   也**只剩置位语义**(没有监控线程去消费) ⇒ **opt-out 后应用自负退出**, 文档必须写明。
3. **晚调用 / 顺序语义已拍**: `bool DisableShutdownMonitor() noexcept` 返回的是"**是否来得及生效**" ——
   先构造再调 ⇒ **`false`**, **且行为不变**(不静默、不部分生效, 与默认路径逐位相同)。
   ⚠️ **A3b 顺序反例(已闭合)**: 先**显式** `StartShutdownMonitor()` 再调 opt-out ⇒ **返回 `true`**,
   而库**已接管**(显式 Start 直接走 `call_once`, 不置内部"已启动"标志) ⇒ 该返回值**只承诺"此后不再隐式安装"**,
   ⛔ **不承诺"库当前没在控制"**。正确判法是自查 `sigaction(SIGINT, nullptr, &old)`,
   ⛔ **不要读这个返回值**。该语义已写进头注释(`include/dzIPC/dzipc.h`)并由**臂 7** 覆盖(变红方向 = M6)。

#### (e) 本轮未闭项(⛔ 不得据本节读成"缺陷已修")

- ⚠️ **残留 17 段照旧**(默认路径, **按设计**) ⇒ 本项**没有**让任何产品在**默认配置**下变得更干净。
- ⚠️ **`OnShutdown(callback)` 未实现**: §4 修法选项 2 的后半句("`std::exit(0)` 改成调用回调")**未兑现**。
- ⚠️ **`exit_code = 0` / 非主线程 `std::exit` / `CleanupIpcInstances` 名不副实** 三条风险**一条都没消**。
- ⚠️ **`local/` 安装树滞后**: `local/include/dzIPC/dzipc.h` 与 **HEAD 版逐位一致** ⇒ 只走 `local/` 的外部
  消费者**看不到**该 API; 刷新 = `make -C build install`(**未执行**, 超出本轮授权)。
- ⚠️ **A3b 返工未再经 reviewer 二次复查**: reviewer seq157 给了 (a) / (b) 两条最小改法并写明"**二选一即可**",
  coder 取 **(a) 仅改注释、产品码零改动**(`dzipc.cc` 逐字节未变), 由 tester seq159 独立确认闭合
  ⇒ 判定按其**预置选项**成立, ⛔ **不得**写成"reviewer 二次复查 PASS"(**没有这一次**)。
- ⛔ **相邻项一律未动**: `UF-003` 仍 `⬜未修`(判据侧 BLOCKED)、`UF-006` 仍 `✅已修`、`UF-008` / `DF-*` 未动。

#### (f) 22:44 指纹订正 + expected_red 补记(2026-09-18)

**(f.1) 指纹订正(机械两格, ⛔ 不改任何读数/判定)**

| # | 对象 | 原值(写回时刻) | 订正值(22:44 树内重取) | 原因 |
|---|---|---|---|---|
| 1 | `tools/sercli_live_probe/uf004_optout_acceptance.sh` | `997d9fd9420ef00f78a6ef803e9efbba` | **`d278068f6db821afafb19d93da42e7e4`** | 文件已被改(新增 `--expect-red` 模式), 见 (f.2); 非本轮 writer 改动 |
| 2 | 库 `build/lib/libipc.so.3` | `b7e3c9caba65ba6ff6c37af995abf2f8` | **`ed5a026508fea1e516f061795c373bf6`** | 树库**已漂移**(原因归属见 (f.4)) |

⚠️ 其余 8 个对象 **22:44 重取与 (b) 表逐一对齐、无一变化**: `src/dzIPC/dzipc.cc` `ae2dd301`、
`include/dzIPC/dzipc.h` `743cf3bc`、`test/test_uf004_shutdown_monitor_optout.cpp` `757b4e5f`、
`tools/sercli_live_probe/sercli_live_driver.cc` `3ffe1307`、`exit_semantics_matrix.sh` `7ddaef96`(⛔ 此格**正确, 未订正**)、
`build/bin/test_uf004_shutdown_monitor_optout` `7409f233`、`build/bin/test_dzipc` `ed808901`、
`tools/sercli_live_probe/bin/sercli_live_driver` `f295bbb0`。

⛔ **订正后 (b) 表的绑定条件仍未满足**: 第 2 格已变 ⇒ 按 (b) 自订的"移动靶"纪律(任一项再变, 须重跑 (b)
四条命令后才能引用本节读数), **本节全部读数须先重跑 ①–④ 才可引用**; 在此之前引用即为违约。
⭐ 实务上把树库**冻结副本**经 `UF004_LIB=` 传入脚本(脚本第 ②' 行)可避开树库漂移, 但**不能**免掉 (f.2) 的红集前提。

**(f.2) expected_red 补记 —— 补的是"验收脚本新增了一种模式"这件事, ⛔ 不新增任何判定**

`uf004_optout_acceptance.sh` 现带 `--expect-red [--expect-red-set FILE]` 模式(`d278068f`, 未跟踪),
用途是把**变异臂**跑出的"预期红"变成**可机械核对**的断言。⛔ 本项判定与 (c) 五口径**一字未动**; 补记内容:

| # | 须记录的事 | 依据 |
|---|---|---|
| ① | 模式与退出码语义: `--expect-red` 下 **0 = 如期转红**(给了 set 时须与预注册集合一致); **1 = 意外全绿或集合不符** | 脚本 `:23-27` |
| ② | ⭐ 红集是**参数不是常量**: 同一缺陷两支变异红集不同 —— 库级变异 `12ae64ca` 为 **3 项**、LD_PRELOAD 行为层变异为 **9 项** ⇒ ⛔ **不得把红集硬编码进脚本** | `redset_mut_tree_lib.txt`(3 项) |
| ③ | **变异臂跑出全绿 ⇒ exit 1** —— 全绿意味着该臂**不承重**, 与"缺陷已修"无关 | 脚本 `:27` |
| ④ | **多出一条红 ⇒ 越界**: 说明变异打到了非预期断言(真缺陷或断言过宽), 须查而不是记成功 | 脚本 `:23-27` |
| ⑤ | ⛔ **`arms3/` 的裸 `FAIL 25/3` 是预期转红证据, 不得引作验收证据**; 该产物自标 `expected_red:true` / `not_acceptance_evidence:true`; **唯一有效验收 = `arms2/` 在 `lib_landed_b7e3c9ca` 下 PASS=28 FAIL=0** | `arms3/acceptance_result.json` |
| ⑥ | ⚠️ **行为层(LD_PRELOAD)变异无库级载体** —— 库级变异只红返回值列; "判定恒 false 且不置位 ⇒ 行为列一起翻"这件事**只能由 preload 臂观测**, 须在报告里写明 | `arms3_test_uf004_m1.log`(4 FAILED / 7) |
| ⑦ | 天花板仍 **`🔶部分修`**: `noapp` 臂 `by_signal=15` / rc=143 / 残留 23 ⇒ ⛔ 不得因本次补记改状态列 | (c) ⑤ + (d) |

⚠️ **两条机械缺口(须 leader 知悉, ⛔ 不得据现有证据抹平)**:

1. ⛔ **`redset_m1_preload.txt` 全仓不存在**: `build/uf004_verify/EXPECTED_RED.md` 的"预注册红集"行声称
   LD_PRELOAD 支有 9 项预注册集, 但盘上**只有** `redset_mut_tree_lib.txt`(3 项) ⇒ 该支的"9 项"
   **没有盘上载体**; 事后复核若传 `--expect-red-set` 会**无文件可传**, 只能退化为"仅校验方向"
   (脚本 `:255` 明写此退化路径)。⛔ **不得把 "9" 当作已存证据照抄** —— 要么补落盘, 要么在引用处降级措辞。
2. ⚠️ **落点说明**: `shm_defect_fixes.md` **§15 无任何 `--expect-red` / `EXPECTED_RED.md` 字样**
   (全 `docs/` grep 零命中) ⇒ 本补记**无既有归属**, 故落 0.3.12 而非 §15; §15 是否需要仅加一条
   **延期/证据指针**, 见 (f.3)。

**(f.3) `shm_defect_fixes.md` 是否需要改动 —— 裁定: ⛔ 本轮不加, 理由三条**

1. §15 是**详情搬运页**, 0.4 第 3 步只要求搬"四要素"; expected_red 是**验收脚本的模式说明 + 证据纪律**,
   属 0.3.12 的**判定侧限定**, 不是 UF-004 的修复详情 ⇒ 按 0.4"写回是搬移"的口径, 不该进 §15。
2. §15 的 **md5 已是移动靶**(`dec2e4f43a927d341d97c2b625d565ab`, `git diff --stat` 525 插入 / 0 删除, 且同日新增 §16)
   ⇒ 再加一格会把两处 md5 绑到一起, 扩大漂移面。
3. 指针**已存在**: 0.3.12 头部第 2 行已写明"详情已搬入 § 修复记录 15" ⇒ 反向可回溯, 不缺口。
⚠️ **若 leader 仍要求同步**, 最小形态 = §15 末尾加**一行**指针(⛔ 不复制 (f.2) 内容、不改任何读数),
且落笔前须重取 §15 所在文件的 md5 并重读 §15 尾部以免覆盖并发编辑。

**(f.4) 树库漂移的归属(⛔ 不是"UF-004 被动过")**

`b7e3c9ca` → `ed5a0265` 经逐指令 `objdump` 差分 + `nm` 符号地址 + 差异符号回查 `git status`,
**仅 16 行 / 3 个符号**, 全部由 `include/dzIPC/common/circularqueue.h` 的内联(§16 的最小队列=2 改动)所致;
**UF-004 相关反汇编逐字节相同** ⇒ 漂移**不构成** UF-004 的行为变化。
⛔ 但 `RUNPATH` 是**绝对**路径 ⇒ 旧证据**不逐位可复现**, 复核只能靠 `LD_LIBRARY_PATH`(可覆盖 RUNPATH)钉冻结库。

### 0.4 完成后的写回协议(冻结)

每完成一项, **立即**执行以下四步, **然后才能进入下一项**:

1. **保留指针行**: 本节 0.1 表中该 `UF-*` 行的状态列改 `✅已修`(⛔**不是**把正文 `⬜` 就地改掉);
2. 在该行所在条目块里写四样东西: **执行的命令 / 结果(PASS/FAIL 与关键读数) / 日期 / 变异验证**
   (回滚后哪条断言转红);
3. **详情搬到 [shm_defect_fixes.md](shm_defect_fixes.md)** 新增「§ 修复记录 N」一节, 沿用其
   「行为判据 + 变异验证」格式(该文档头部自陈的分工是"修完一条就从这里删掉、到那份加一节");
4. 本节 0.1 表保留**指针行**便于回溯 ⇒ 写回是**搬移**, 不是"打勾了事"。
   第 4 条是对该文档"删掉"字面的一处**显式偏离**, 已由 leader 在冻结指令中确认
   ("保留指针行标 ✅")。

### 0.5 待裁清单(冻结时仍未拍 + 评审 seq120 追加第 7–9 条; 第 8 条 2026-09-18 已兑现)

| # | 待裁事项 | 影响 | 建议 |
|---|---|---|---|
| 1 | UF-002 选"失效态语义"还是"抛 `bad_alloc`" | 后者与 **UF-001** 严格串行(与 UF-007 无关 —— 评审 B1 订正后 UF-002/UF-007 为**并行**) | 失效态语义(不动全局错误模型) |
| 2 | `register_entry` 表满时是否顺带扫死条目 | UF-006 的**产品行为变更** | 是, 但需 reviewer 先裁"证据只读"推理是否被破坏 |
| 3 | UF-003 / UF-005a 的互通性决策是否合并为一次 | 两条都要"与旧版本不互通" | 合并, 一次拍; **建议把 DF-001/DF-002/DF-003 一并拍** —— 三者都落在"未升级订阅方怎么办"这同一个问题上 |
| 4 | UF-005b 的口径: 并入 UF-005a 一次重设计 vs 保持独立条目先做实验 | 两文档现**互相打架** | 见"订正记录"第 7 条 |
| 5 | UF-003 / UF-005a / DF-001–DF-005 的**单一事实源**归属: 本文留正文 vs 只留指针 | 跨文档重复登记 | 只留指针 |
| 6 | `dzflat_known_issues.md` §4 自身矛盾(`:184`【已修, 见 §8】vs `:198`"现状: 未修, 既存") | **直接决定 UF-007 的定性** | 倾向"已修", 决定性验证 = 跑 `LapSafety.LappedDrainMustNotDeliverDuplicates` |
| 7 | **DF-001–DF-005(§5 第 3–7 行)本轮是否进产品修复** | 五条均为**设计待办/契约缺口而非缺陷**; 不进则长期停在 `⏸️待决策` | 本轮**不进**(它们不影响正确性); 要进的前置 = 与 **UF-005a/UF-005b 同一次兼容性拍板**。**这就是 DF-\* 的"后续入口"** |
| 8 | ~~UF-000 的**阳性对照选哪个已知用例**~~ ✅ **2026-09-18 已兑现** | 已定为 **`handle` 构造期的注入崩点**(= 0.3.1 第 2 条建议的那一个), 并由两条**独立**证据坐实 | 实测红已给: ① `fi_positive_control` **rc=139**; ② 判据用例 `test_alloc_fault_inject` **rc=139**。⚠️ **残余未闭**: UF-001/UF-002 尚未修 ⇒ 字面要求的"**回滚修复态**转红"本轮**不适用**, 修完须补一次真回滚(见 0.3.2 限定第 1 条) |
| 9 | UF-009 脚本侧采集的落点: 改现成 `run_live_auto_shm.sh` vs 新增独立采集脚本 | 决定"退出码 / 残留段计数"的可复现性(授权已给, 见 0.6 第 14 条) | 新增独立采集脚本较稳(不动现网驱动流程); 最终由 tester 定 |

### 0.6 订正记录(冻结时已确证的错锚与过期表述; 第 11–17 条为**评审后**订正)

> 第 1–10 条是**冻结当时**确证的; 第 11–16 条来自 reviewer 对冻结版的复核
> (共享区 `frozen_tasklist_UF000_UF009_review_架构评审.md`, seq120 的 B1/B2/B3a/B3b/B4),
> 第 17 条是我在落笔本轮订正时**自查**发现的同类漂移。两类都按"原文 → 订正"记账, 便于回溯。

| # | 位置 | 原文(错) | 订正(实测) |
|---|---|---|---|
| 1 | §2 成因代码块 | 锚 `src/libipc/utility/pimpl.h:52-56` 是 `make_impl` | `:52-57` 今天是 `struct pimpl` / `pimpl::make`; **"不舒服"分支的 `make_impl` 真身在 `pimpl.h:37-40`** (`clear_impl` `:42-45`)。⇒ **按旧行号去改会改错函数** |
| 2 | §2 表格 "`shm::handle`" 行 | "19(其中 **6 行**带 `nullptr` 的判的是 `id_`, 不是 `p_`)" | 实测只有 **3 行**(`shm.cpp:92/97/102`); `:126` 是**赋值**不是判空 |
| 3 | §2 修法选项 1 | "`p_ == nullptr` 时 `valid()` 返回 false, 其余方法安全空转" | **不够**: `handle::release()`(`shm.cpp:91-94`)首句就是 `impl(p_)->id_ == nullptr` —— **先解引用 `p_` 再判 `id_`**, 而 `~handle()`(`:37-40`)第一句即 `release()` ⇒ `p_ == nullptr` 时**必崩**; `valid()`(`:51-53`)同为崩点(读 **0x8**)。⇒ 必须改**解引用次序**, 不是"加几个判空" |
| 4 | §2 标题"同族六处, 只加固了一处" | — | **表述准确, 无需订正**; `buffer` 处数 **4 正确**(第 5 个 grep 命中是 `:76` 注释) |
| 5 | §4 "为什么登记它" 引语 | "以下都是**风险**, 不是已观测到的缺陷" + 文末"§4 **全部是推理**" | **已过期**: 已有**现成活体指纹** —— `tools/sercli_live_probe/sercli_live_driver.cc:406-407` **自己装了** SIGINT/SIGTERM 处理器, 而 `--via factory` 走公共工厂 ⇒ 库**覆盖它自己的处理器**; `CleanupIpcInstances()` 只 `clear()` weak_ptr ⇒ `std::exit(0)` 时用户 `shared_ptr` 仍持有实例、**析构不发生 ⇒ 段不 unlink**。与 `--via factory` "未打印 `SERVER SUMMARY` + 残留 17 段"逐条吻合, 而 direct 3/3 有 SUMMARY 且零残留。⚠️ **但机制链仍是假说**(证据吻合, 判定实验未做) |
| 6 | §3 症状 2 | "实测 **20 条只有 10 条**能进环" | **必须重测**: 该读数可能混入 **UF-007**(重复归还 → 空闲链自环)的效应 ⇒ 不分离则 UF-003 修完仍红会被误判成"没修好" |
| 7 | §5 第 2 行 | 位置写 "shm_defect_fixes.md **§3 末尾"未处理但已登记"**" | 读 `:270-308`(§3 尾)**未见该小节**; 实质内容在 §3「建议的修法 1」(`11451 + domain_id*hash` → `base + domain*STRIDE + hash%STRIDE`)。且 `shm_defect_fixes.md:181-186` 原文是**独立条目 + 独立实验判据**("未实测…先做实验再决定修不修") ⇒ 与本行"归入上一条的重设计"**口径打架**, 待裁(见 0.5 第 4 条) |
| 8 | §5 第 5 行 | "`local_shm_fanout.md` **首行**即'设计文档, 未实施'" | 实为**第 3 行**(`> **状态**: 设计文档, **未实施**。`) |
| 9 | §1 严重级理由 | "把一切内存问题都变成无诊断崩溃" | **触发面已缩小**: 修法选项 3「入口加闸」**已部分落码** —— `test/test_deser_alloc_guard.cpp`(5 用例)钉住"荒谬/负 count 不得分配""读失败不留栈垃圾""越界清零"。它治的是 dzflat/TLV 反序列化的**无界分配**(源自 [dzflat_known_issues.md](dzflat_known_issues.md) 第 3 条), **不覆盖 allocator 语义**。⇒ 剩余射程 = **非 wire 路径 + 真 OOM**。⚠️ **不得**据此判 §1 已修 |
| 10 | §7 处理顺序 | "§1 先做 → §2 → §4 → §3" | **已被本节 0.2 取代**(无 UF-000 时 §1/§2 的判据不可执行) |
| 11 | **0.1 表 UF-002 行的 `可并行` 格** | "✅ 与 UF-004/006 并行; ⛔与 UF-001/**007** 串行" | **同一对关系两处自相矛盾**(UF-007 行写"与 UF-001/**002** 并行"), 且两项 `修改边界` 文件不交、0.2 把它们画成**两条互不相交的分支** ⇒ 判该格是笔误, 订正为 "✅ 与 **UF-007**/UF-004/UF-006 并行; ⛔**与 UF-001 串行**"(仅保留 UF-001 串行)。**(评审 seq120 B1)** |
| 12 | **§5 第 3–7 行** | 无 ID, 0.1 表**零命中**(`本体章节` 列只映射了第 1、2 行) | **判为遗漏而非排除**: 0.1 自称"唯一权威台账"、0.9 一览却记这批领头项为"未修(见 §5)" ⇒ 两处矛盾。现补稳定 ID **`DF-001`–`DF-005`**(**不并入 `UF-*`**) 并**同时**进 0.1 表与 §5 表, 状态 `⏸️待决策`。**为何用 `DF-*` 而非 `UF-005c`–`UF-005g`**: 这五条是**设计待办/契约缺口, 不是缺陷**, 命名空间本身就要承载这个区别(这正是 B2 说的"`UD-*`/`DF-*` 分段被并入单一 `UF-*` 后消失了"的根因)。**(评审 seq120 B2)** |
| 13 | **0.9 一览第 1 行** | `⬜ 未修` | 与 0.1 表 UF-001 的 `🔶部分修` 矛盾(**冻结当天**即已漂移) ⇒ 统一为 `🔶 部分修`(理由见本节第 9 条)。**(评审 seq120 B3a)** |
| 14 | **0.1 表 UF-009 行的 `修改边界` 格** | "本轮**不授权**; 先补脚本采集退出码" | **同格自相矛盾**: 采集退出码**必然要改** `tools/sercli_live_probe/**` 与 `scripts/**` 的驱动脚本 ⇒ 按字面执行则采集做不了, UF-009 永远停在"崩溃/挂起无法区分"。订正为 "⛔不授权改**产品码**; ✅**授权**改工具脚本/测试以采集退出码与残留段计数"。UF-008 行的"不授权"按同一口径读(证据采集为只读)。**(评审 seq120 B3b; leader 本轮明确)** |
| 15 | **0.1 表 UF-000 行的 `本体章节` 格** | 仅 "本文 §6.1/§6.2" | §6.1/§6.2 是**配方文本** ⇒ UF-000 自身**无验收命令、无变异验证、无阳性对照**, 是 **0.3 第 5 条**的**唯一例外**。新增 **0.3.1** 补齐三条(工装自身构建/运行命令、阳性对照、注入尺寸档拦截计数), 并在该行 `本体章节` 与 `修改边界` 同步指向。**(评审 seq120 B4)** |
| 16 | **本节第 8 条的位置列** | "§5 **第 5 行**" | **我冻结时自己的错锚**: 该内容(`local_shm_fanout.md` "设计文档, 未实施")在 **§5 第 6 行 = `DF-004`**; §5 第 5 行是"订阅侧没有开关" = `DF-003`。⇒ 按 ID 订正, 避免与 `DF-003` 撞车 |
| 17 | **0.1 表 UF-005a 行的 `状态` 格** | `⬜未修` | **评审外自查**: §5 第 1 行写的是"**待决策**", 而 0.1 写 `⬜未修` —— 与第 13 条(B3a)**同一类**的 0.1↔本体漂移。UF-005a 的真状态是"修法已定、**等兼容性拍板**"(其 `依赖`/`负责人` 两栏本就写着"先拍兼容性") ⇒ 统一为 `⏸️待决策(先拍兼容性)`, §5 第 1 行同步加 `⏸️`。**这不是新增待裁项**, 只是状态栏对齐 |
| 18 | **0.9 一览第 1、2 行** | `🔶 部分修(… allocator 本体未修 …)` / `⬜ 未修(仅 buffer 已加固)` | **落笔本轮 UF-001/UF-002 写回时的同步**: 两项已于 2026-09-18 落码(`UF-001` 选项 1、`UF-002` 失效态判空), 0.1 表状态列改 `✅已修` ⇒ 0.9 一览按第 13 条的同一口径同步为 `✅ 已修`。⛔ 本节第 9 条 / 第 13 条是**冻结当时**的记账, **不随之改写**(本表不构成第二份台账, 口径见下)。另: 第 2 行原文的"仅 `buffer` 已加固"今日**已反转** —— `buffer` 恰是本轮**未改**的那一处(见 0.3.7 限定 2) |

| 19 | **§3「判据」两条** | "造一个占满 chunk 后 `kill -9` 的进程, 再起一个新进程发大消息: 必须走 storage 路径…" + "不引入误回收…必须有并发压力用例" | **判据侧失效(2026-09-18 tester 实测)**: 这两条**判不了本项** —— `test_chunk_hold` **3/3 PASSED**、`test_lap_safety` **3/3 PASSED**, 且在**残池态 / 清池态**双臂下**逐用例完全相同** ⇒ **A=B ⇒ 无判别力**; **阳性对照**(把 `929792` 池人为置为**完全耗尽** ⇒ `empty()==true` ⇒ `acquire()` 恒 `-1`)后两条**仍全绿** ⇒ 它们**结构性不走该池的 `acquire()` 路径**(⛔ 非推测)。⇒ 须**新补**一条能观测该尺寸档「借样成功率 / 可用槽数」的用例, 且**必须先在合成耗尽池上转红**才算合格。已在 §3「判据」处**就地加 ⛔ 标注**(不改判据原文), 详见 **0.3.8(1)** |

> **本表的读取口径**: 0.1 表是**唯一权威**; 本表只记"曾经写错什么、为什么错", 不构成第二份台账。
> 凡本表订正过的栏位, 一律以 0.1 表为准。

### 0.7 已修复 / 重复 / 过期 的识别结论

- **已修(复核一致)**: [shm_defect_fixes.md](shm_defect_fixes.md) §7 的四类加固均在 HEAD
  `3818899` 内(该文档自陈, 复核一致) ⇒ §2 的"六处只加固了一处"**表述准确**。
- **重复**: **无跨文档重复登记** §1–§4 —— 全 `docs/` 内 `allocator_wrapper|pimpl|StartShutdownMonitor`
  的**缺陷登记**命中只在本文(`docs/dzipc_log.md:100` 是把 `StartShutdownMonitor` 当**既定特性**
  描述, 不是缺陷登记; `docs/dzflat_shm.md:502` 的 pimpl 是 buffer 池化, 无关)。
  ⚠️ 但 §5 第 1 行与 [shm_defect_fixes.md](shm_defect_fixes.md) §0 第 5 行是**同一登记的跨文档重复**
  ⇒ 单一事实源待拍(0.5 第 5 条)。
- **UF-007 是"漏登记"不是重复**: chunk 池/环一族在 **4 个登记点**
  (a) 本文 §3 残池 **未修**; (b) `dzflat_shm.md §5.1`"覆写无条件归还" → **已落码 = 过期项**
  (`test_chunk_hold.cpp:1` 明写是 Step 0 回归、旧实现必失败); (c) `dzflat_shm.md §5.3 + §9.5`
  (池耗尽 → 64B 分片退化 → `force_push` 覆写 → 重组缓冲错乱 → SIGSEGV) **未修**;
  (d) `test_chunk_hold.cpp:18-25` 登记的第三处(套圈接收方重复读同槽) **未修** = **UF-007**。
  ⇒ 本文目前只登记了其中两处(UF-003 与 UF-007)。
- **过期项(在本文之外的文档, 本阶段不授权改, 登记待批)**:
  ① `docs/udp_shm_alignment_task_list.md:130` `rmem_max/wmem_max 实测 212992` —— **已被 F3 实测
  `536870912` 推翻**; ② 同文件 `:125` "T1/T3 共 16 文件未提交" —— **已落地**(`8c4ce71` / `3818899`);
  ③ `docs/dzflat_shm.md §5.1` 已落码却仍列在"硬阻塞"标题下, 会误导排期;
  ④ `docs/shm_defect_fixes.md:35-38` 那条"已撤销"note **结论对、理由错** —— 它称
  `dzflat_known_issues.md:198` 属"成因段的历史叙述", 但 §4 的结构是 症状→成因→**现状**→该怎么办,
  且 `:198` 引用的 `test_chunk_hold.cpp` 2026-09-17 才存在 ⇒ 那是一条**真实矛盾**被误判为不存在。
- ⚠️ **一条会传染的过期矛盾(须先裁, 见 0.5 第 6 条)**: `docs/dzflat_known_issues.md` §4
  **自身矛盾** —— `:184` 标题【已修, 见 §8】 vs `:198`"现状: 未修, 既存"。判据倾向"**已修**":
  §8.1/8.2 正是该机制修复(游标重同步 + 本格已消费检查), §8.6 实测
  `LappedDrainMustNotDeliverDuplicates` 288 条 283 重复 → **ok**, `test/test_lap_safety.cpp:1-12`
  把 ④ 列为已覆盖。**且已传染**: 2026-09-17 新增的 `test_chunk_hold.cpp:18-24` / `:145-146`
  把同一机制写成"已知的无关缺陷…刻意绕开" ⇒ **较新文件继承了被 §8 推翻的旧措辞**。
  **这直接决定 UF-007 是"修真缺陷"还是"修一个已被 §8 覆盖的重复项"** —— 必须先裁再动。

### 0.8 各角色职责与协作接口(冻结)

| 角色 | 职责 | 交付标准 | 接口 |
|---|---|---|---|
| **writer-claude** | 本文档与 [shm_defect_fixes.md](shm_defect_fixes.md) 的**唯一落笔人**(单写者); 去重、口径统一、字段回写 | 每栏须有 `file:line` 或 commit 锚, 无锚写 `UNVERIFIED` 而非留空; 不把"推理"写成"实测"; 行号标快照时点 | ← coder 给"修法选择 + 实际改动文件清单"; ← tester 给**可执行**验收命令与**实测**红/绿; ← analyst 给 §5 各外部项的实际归属小节; → 全员: 冻结后改动走"提交字段 → 我落笔" |
| **coder-claude** | UF-001(最小形态: 越界抛 + 失败记日志)、UF-002(照 `buffer` 样板收口 5 类, **含 `release()` 解引用次序**)、UF-006 用例侧、UF-007 | 硬边界: **不动共享段布局/ABI**(与 UF-003 天然互斥); libipc 改动后重跑 F4 红线 | → writer 改法选择与文件清单; → tester 提供注入工装需求; ← reviewer 裁 UF-002 失效态语义 |
| **tester-claude** | ✅ **UF-000 注入工装 已交付**(2026-09-18, 见 0.3.2; 含 0.3.1 的三条 —— 构建/运行命令、阳性对照的实测红 rc=139、尺寸档拦截计数)、UF-004 判定实验、UF-009 复现(脚本侧采集已授权)、UF-001/002 的注入态验收 | 判据必须能写成一条可粘贴命令; 不能跑的标"设计态"; 计数须给数法 | → coder 交付工装 ✅ 已完成(UF-001/002 判据与护栏已解锁); → writer 逐条证据 |
| **reviewer-claude** | UF-002"失效态"语义边界(`handle` 构造函数**没有失败通道**)、UF-006 若让判定路径顺带回收是否破坏"证据只读"推理、UF-007 的 `release()` 幂等语义影响面 | 复核锚点一致性与公共语义变更 | ← coder 提方案; → writer 结论 |
| **analyst-claude** | UF-008 证据、§5 外部项的实际归属小节、UF-007 判据分离设计 | 明确区分 `[码]` / `[推]` / `[未核]` | → writer 哪些行号/计数不可信 |

**`UF-000` 已于 2026-09-18 交付(工装可用; 0.3.1 三条验收字段已交付)**, 但有 0.3.2 的六条限定 ——
尤其字面意义的"回滚修复态变异验证"仍欠一次(UF-001/UF-002 修完后补)。其余 `UF-*` 条目均未执行
任何修复; 它们的验收命令一律为设计态, 尚无任何 PASS/FAIL 判定。

---

## 0.9 一览(下表编号即 §1–§5 本体的入口)

| # | 问题 | 严重度 | 触发条件 | 状态 |
|---|---|---|---|---|
| 1 | `allocator_wrapper::allocate` 是 `noexcept` 且失败**返回 nullptr** → 任何分配失败都表现为"往地址 0 写"的 SIGSEGV | **高(放大器: 把一切内存问题都变成无诊断崩溃)** | 真实 OOM, 或调用方传入被破坏的巨大长度 | ✅ 已修(**选项 1 落码**: 去 `noexcept` + 越界 `length_error` + 底层失败 `bad_alloc` —— 见 0.3.6) |
| 2 | `ipc::shm::handle` 及同族 pimpl 类: `make()` 返回空后**全线解引用空指针**(`handle` 19 处、`udp` 9 处、三个同步原语各 9 处均无判空) | **高(构造期崩, 无诊断)** | 该尺寸档分配失败 | ✅ 已修(五类 pimpl 入口 + 析构判空已落码; ⚠️ `buffer.cpp:59` 已于 2026-09-18 改动但**回滚变异阴性** ⇒ ⛔该行**不计入修复**, 保留/回退**待 leader 裁** —— 见 0.3.7 限定 1 与 0.3.8(7)) |
| 3 | 共享 chunk 池**没有崩溃回收**: 被杀的进程永久占走池槽, 残池污染后续所有进程 | **中高(时好时坏的性能悬崖 + 假回归)** | 任何未归还就死掉的持有者 | ⬜ 未修 |
| 4 | `dzIPC::StartShutdownMonitor` 让**库接管进程退出**: 覆盖应用信号处理器 + detached 线程里 `std::exit(0)` | **中(设计风险, 已实测坐实)** | 任何用到 IPC 的进程收到 SIGINT/SIGTERM | 🔶 部分修(**opt-out 开关已落码并验收 2026-09-18**: 默认路径逐位不变 + 残留 17 属预期; opt-out 路径实测归零; ⛔ 不改 UF-009、⛔ Python 面不可达 —— 见 0.3.12) |
| 5 | 已登记在别处的项: **缺陷** `UF-005a`(组地址碰撞) / `UF-005b`(端口乘性冲突) + **设计待办** `DF-001`–`DF-005`(DZFlat 默认 OFF 的灰度约束、per-topic/订阅侧开关缺口、同机多订阅者共享 SHM 入口、Python 发布端一次 memcpy) | 见各文档 | — | ⏸️ 待决策(见 §5; **与 0.1 表的 UF-005a/b + DF-001–DF-005 一一对应**, 两处必须同步) |

> ⛔ **本表只覆盖 §1–§5 本体**。**`UF-000`(注入工装)、`UF-006`(`ipc_info_pool` 静默退化)、
> `UF-007`(重复归还致 free-list 自环)** 是 0.1 表新增、**本体未登记**的条目, **不在本表** ——
> 查它们只看 0.1 表, 不要因为本表没有行就判"不在范围"。(评审 seq120 B2 的同型问题)

> ⚠️ **第 1、2 行的「问题」列写的是**登记时**的失效态, 不再代表今天**: `UF-001`/`UF-002` 已于
> **2026-09-18 落码修复**(写回见 **0.3.6** / **0.3.7**) ⇒ 本表状态列已同步为 `✅ 已修`。
> 第 1 行的"是 `noexcept` 且失败返回 `nullptr`"、第 2 行的"均无判空"读作**修复前**的形态。
> (同类同步的既有判例见 0.6 第 13 条 / 第 17 条)
> ⚠️ **第 4 行的状态列已于 2026-09-18 由 `⬜ 未修` 同步为 `🔶 部分修`**(见 0.3.12): 「事项」列仍描述**修复前**的形态 ——
> "覆盖应用信号处理器"**今天仍成立**(默认路径逐位不变), 但**已存在** opt-out 开关; 该行「风险」列同时由"未实测成缺陷"改为**已实测坐实**。

**严重度排序的理由**: 第 1 条是**放大器** —— 它把"分配失败"这个本来可以报错的状态统一翻译成
"只有一个 ip 的 SIGSEGV"。第 2 条是它在同一族里的第二个现成入口。两条都在**构造/析构路径**上,
调用方接不住。第 3、4 条不影响正确性, 影响的是"排障能不能做"与"行为能不能预期"。

---

## 1. `allocator_wrapper::allocate` 的 `noexcept`-nullptr 语义(放大器)

**症状**

任何一次"分配返回空"都不报错、不抛异常, 而是在**很久之后、别的地方**炸成
`SIGSEGV at 0`(写 NULL, `error 6`, in `libc.so.6`), 内核日志只留一个 ip, 现网完全看不出
"这是分配失败"。§7 那次崩溃里, 它就是把一个 use-after-free 放大成"写 NULL"的那一环。

**成因**

```cpp
// src/libipc/memory/allocator_wrapper.h:78-82
pointer allocate(size_type count) noexcept {
    if (count == 0) return nullptr;
    if (count > this->max_size()) return nullptr;   // ← 标准要求抛 std::length_error
    return static_cast<pointer>(alloc_.alloc(count * sizeof(value_type)));   // ← 标准要求抛 std::bad_alloc
}
```

底层 `pool_alloc::alloc` 是 `static_alloc::alloc` = `std::malloc`(`src/libipc/memory/alloc.h:20-27`),
失败返回空、永不抛。于是 libstdc++ 拿不到异常, 直接 `basic_string::_M_construct` →
`memmove(nullptr, src, huge_len)` → 写地址 0。`ipc::string` / `ipc::unordered_map` 等全部走这个
分配器(`src/libipc/memory/resource.h:30` 的 `allocator` 别名 → `:52` `unordered_map` / `:66` `string`),
所以**任何**用它们的代码路径都具备这个性质。

**触发条件**

- 真实 OOM(分配失败);
- 或者: 调用方传进来的长度已经是垃圾值(§7 的 UAF 就是这种: `_M_string_length` 被 tcache
  的 fd/key 覆盖, 实测 135193289566176)。**这一条尤其重要** —— 它意味着"上游内存被写坏"这个
  错误, 在下游会伪装成"分配失败 → 崩在地址 0", 现场只能看到一个 ip。

**为什么当时没修**

它改的是**全局错误模型**: 把"返回空"改成"抛异常", 影响的是几十处调用方(既有代码大量按
"空指针 = 失败"写)。而且 §7 之后, 唯一已知的真实触发(垃圾长度)已经随 UAF 一起消失了 ——
剩下的只有真 OOM。在"没有异常安全审计"的前提下改它, 风险大于收益。

**修法选项**

1. **按标准抛**(`count > max_size()` → `std::length_error`, 分配失败 → `std::bad_alloc`)。
   本仓**没有** `-fno-exceptions`(CMakeLists 只加 `-O2/-O3/-DNDEBUG`), 所以异常可用。
   代价: 需要逐个审计"构造期间抛出"的路径, 确认不会留下半初始化对象/泄漏。
2. **只在越界那一支抛**(`count > max_size()` → `std::length_error`), 分配失败仍返回空但**打一次
   日志**。这是"低成本的一半修复": 它能覆盖 §7 那种"垃圾长度"的形态(那条路径正是走
   `count > max_size()` 或巨大 size 分配), 而 OOM 仍然只能崩。
3. **不动分配器, 只在入口加闸**: 凡是有 `count` 来自 wire/外部的地方, 先按上限校验再
   `resize`(生成的 TLV 反序列化头里已经有这种闸, 见 shm_defect_fixes.md「一条误报」), 把
   分配器留给"内部可信输入"。

**判据**

- 传一个 `count > max_size()` 进去: 不得再出现"地址 0 上的 SIGSEGV"; 必须能观测到明确的
  错误(异常或日志);
- 正常路径行为不变(现有测试全绿);
- 加一条常驻用例把判据钉住(例如用 `LD_PRELOAD`/链接期 wrap 让 `malloc` 在指定尺寸档返回空,
  断言"抛出/记录"而不是"崩")。

---

## 2. pimpl 类的 `make()` 失败 → 全线空指针解引用(同族六处, 只加固了一处)

**症状**

定向让某个尺寸档的 `malloc` 失败后, 进程崩在 `ipc::shm::handle` 的**析构**里
(`handle::release` → `impl(p_)->id_`), 同样是"只有一个 ip 的 SIGSEGV", 同样没有诊断。

**成因**

`pimpl<T>` 的"不舒服"分支(`sizeof(T) > sizeof(void*)`)是**堆分配**的:

```cpp
// src/libipc/utility/pimpl.h:37-40   ← ⛔ 锚点已订正(原写 :52-56, 那是 struct pimpl / pimpl::make)
template <typename T, typename... P>
IPC_CONSTEXPR_ auto make_impl(P&&... params) -> IsImplUncomfortable<T> {
    return mem::alloc<T>(std::forward<P>(params)...);   // ← 已判空返回 nullptr(见 pool_alloc.h)
}
```

```cpp
// src/libipc/shm.cpp:23-25
handle::handle()
    : p_(p_->make()) {   // ← ① 通过未初始化的 p_ 调静态成员(按标准是 UB, 实际不解引用)
}                        //    ② 分配失败 ⇒ p_ == nullptr, 而这里不检查
```

`mem::alloc<T>` 在 §7 已加"就地构造前判空"的守卫(`include/libipc/pool_alloc.h:87-97`),
所以现在**不再崩在构造**了 —— 代价是 `p_ = nullptr`, 而后续 **19 处** `impl(p_)->...`
全无判空(`src/libipc/shm.cpp`, `impl(p_)` 出现 19 次, 判空 0 处):

```cpp
handle::~handle() {
    release();          // → impl(p_)->id_  ⇒ 读地址 0
    p_->clear();        // → mem::free(nullptr) ⇒ 已安全
}
```

**同族现状**(`p_->make()` + 无判空的 `impl(p_)`):

| 类 | 文件 | `impl(p_)` 代码处数 | 对 `p_` 判空 |
|---|---|---|---|
| `shm::handle` | `src/libipc/shm.cpp:23` | 19 | **0**(其中 3 行带 `nullptr` 的判的是 `id_`: `:92/:97/:102`; `:126` 是赋值。⛔ 且这三处判空**在解引用 `p_` 之后**, 拦不住 —— 见「修法选项 1」的订正) |
| `socket::UDPNode` | `src/libipc/socket/udp.cpp:24` | 9 | **0** |
| `sync::mutex` | `src/libipc/sync/mutex.cpp:27` | 9 | **0** |
| `sync::condition` | `src/libipc/sync/condition.cpp:27` | 9 | **0** |
| `sync::semaphore` | `src/libipc/sync/semaphore.cpp:28` | 9 | **0** |
| `buffer` | `src/libipc/buffer.cpp:37` | 4 | ✅ 全部(`:81-99`, 经局部量 `ip` 判空, §7 已加固) |

`buffer` 是已加固的样板 —— 它的访问器现在统一写成 `auto ip = impl(p_); return (ip == nullptr) ? … : …;`。
其余五个类的入口(尤其是**析构函数**和 `valid()`)应当照此收口。

**触发条件**

对应尺寸档的分配失败(真实 OOM, 或 §1 那种"长度已被写坏"的传导)。两者都需要 OOM 级事件才触发,
与 §7 那个"只需要析构顺序"的 UAF 不同 —— 这也是当时没有一起修的理由: **不能复现的东西很难验证
修好了**, 只能靠注入。

**为什么当时没修**

它是**真实 OOM 才触发**的路径, 而修它要动 5 个类的公共入口语义(尤其 `handle` 的构造函数没有
失败通道)。§7 那一轮的原则是"只修有确定复现路径的缺陷", 于是只就地加固了当天能复现的三处
(`mem::alloc<T>` / `make_cache` / `cache_t::append`)与 `buffer` 的访问器。

**修法选项**

1. **语义可兼容的收口**(推荐): 给 `handle` 这类类定义"失效态"语义 —— `p_ == nullptr` 时
   `valid()` 返回 false, 其余方法安全空转(析构不做事)。这与"`shm::acquire` 失败时 handle 本就
   是 invalid"的现状**一致**, 不新增 API。
   工作量 = 每个类加一个 `impl_or_null` 辅助 + 在公共入口(构造后相关入口 + 析构 + `valid()`)
   判空; 内部函数可以继续假定"已判过", 不必函数内四处铺开。
   ⛔ **冻结订正(2026-09-17): 只"加判空"不够。** `handle::release()`(`:91-94`)首句就是
   `impl(p_)->id_ == nullptr` —— **先解引用 `p_` 再判 `id_`**; 而 `~handle()`(`:37-40`)第一句
   即 `release()` ⇒ `p_ == nullptr` 时**必崩**。`valid()`(`:51-53`)同为崩点(`impl(p_)->m_`
   在 `p_ == nullptr` 时读 **0x8**)。⇒ 必须同时改**解引用次序**(先判 `p_`, 再碰 `impl(p_)->…`),
   不是"在几处补 `if (p_ == nullptr) return;`"。
2. **让 `pimpl::make` 失败时抛 `bad_alloc`**: 一行改完, 但等于把 §1 的错误模型改造提上来
   (构造期间抛出的异常安全要审计), 且会把"分配失败"从"静默降级"变成"terminate"。
3. **消除堆分配**: `pimpl` 的"不舒服"分支之所以存在, 是为了不在头文件里暴露实现大小。
   若某个类可以接受把实现内联进对象(或改成 `std::unique_ptr` + 明确的构造失败通道), 就直接
   没有这条路径了。对 `buffer` 这种被大量复制的小对象不划算, 对 `handle` 值得评估。

**判据**

- 定向注入(§6.1)让该尺寸档分配失败后: 进程**不崩**;
- `handle` 的 `valid()` 返回 false, 析构安全, `acquire/release/detach` 空转;
- 不注入时全部现有测试(`test_shm`、`test_ipc`、`test_dzipc*`、`test_sync`)行为不变;
- 把"注入 + 不崩"固化成一条用例(否则这条修完没法回归)。

---

## 3. 共享 chunk 池没有崩溃回收 → 残池污染后续**所有**进程

**症状**

两类, 都很难反推成因:

1. **性能悬崖**: 大消息本该走 storage 路径(直接借 chunk), 却退化成 64 字节分片 ——
   4MB 消息变成 65536 次 push 打进 256 槽的环。表现是"时好时坏"的吞吐, 不是错误。
2. **假回归**: `test_chunk_hold` 这种"按容量计数"的用例会稳定失败(实测 **20 条只有 10 条**能进环)。
   把§7 修复前的代码换回来在同一个残池上跑, **同样是 10/20** —— 也就是说, 一个**已经修好**的
   东西会看起来像"被新改动搞坏了"。
   ⚠️ **冻结订正(2026-09-17): 该读数必须先重测。** 它可能混入 **UF-007**(重复归还 → 空闲链表
   自环, 见本文「冻结任务清单」0.1)—— 不分离则本项修完仍红会被误判成"没修好"。重测前先按
   §6.3 清残池建立基线。
   ⚠️ **进度(2026-09-18)**: §6.3 的清池**已执行完毕**(删除前二次确认 `grep -l CHUNK_INFO /proc/*[0-9]/maps`
   = 0; 现 `/dev/shm` 中 `CHUNK_INFO` 段 = **0 个**)⇒ **"先清池"这一步已满足**;
   ⛔ 但**"重测该读数"仍未做**(tester 本轮明确挂账) ⇒ 本读数今天**仍不可引用**。

**成因**

chunk 池的可用位是**共享段里的 free-list**, 且没有任何 owner 信息:

```cpp
// src/libipc/ipc.cpp:258-262
struct chunk_info_t
{
    ipc::id_pool<> pool_;    // ← 位于共享段内, 跨进程共享
    ipc::spin_lock lock_;
};
```

```cpp
// src/libipc/utility/id_pool.h:76-86
storage_id_t acquire() {
    if (empty()) return -1;
    storage_id_t id = cursor_;
    cursor_ = next_[id];     // 从空闲链表里摘出
    return id;
}
bool release(storage_id_t id) {   // ← 唯一的归还路径
    if (id < 0) return false;
    next_[id] = cursor_;
    cursor_ = static_cast<uint_t<8>>(id);
    return true;
}
```

- 容量每尺寸档 **32** (`large_msg_cache = 32`, `include/libipc/def.h:44`);
- **只有"持有者自己归还"这一条路径**把 id 放回池子: `release_storage` / `recycle_storage`
  (`src/libipc/ipc.cpp:461/513`)。池里没有 pid、没有时间戳、没有心跳、没有进程存活检查;
- 所以进程被 `SIGKILL`/`SIGSEGV` 杀掉(或退出前没走完归还), 它占的 id **永久出列**。池段本身
  还活着(还有别的进程在映射), 于是**之后每个进程**看到的都是"少 N 个槽"的残池。

这一条与 §7 是同一场景的两面: §7 那次崩溃(段错误)之后的残池就是它造成的。

**触发条件**

任何"持有 chunk 后没有归还就死掉"的进程 —— 包括所有崩溃、所有被 `kill -9` 的进程。
一旦池被啃到 0, 之后**所有**大消息永久走分片路径(直到有人清掉段)。

**为什么当时没修**

修它要改**共享段的布局**(给 id 加 owner + 存活判定, 或换一种池结构) —— 属于"与旧版本进程
不互通"的改动, 需要和发布节奏一起决策, 与§7 那个"只改一个析构路径"的局部修复不同量级。

**修法选项**

1. **owner + 存活判定**(根治): 每次 `acquire` 记下 owner(pid + 进程启动时间, 或一个"心跳槽"),
   `acquire` 在空闲链为空时顺带扫一遍"owner 已死"的 id 并回收。要求: 判定必须廉价且不能误判
   (pid 复用 → 必须配启动时间或 `/proc/<pid>/stat` 的 starttime)。
2. **显式清理工具**(低成本兜底): 提供一个"确认无活进程映射后清池"的命令(见 §6.3 的手工版),
   并把它写进排障手册。它治不了"生产里静默退化", 但能让测试与开发环境不再被残池坑。
3. **把池做成可重建**: 段名带上"池版本/纪元", 崩过一次就换新段(旧段留给内核回收)。代价是
   内存短时翻倍, 且需要一个"何时判定该换"的信号。

**判据**

- 造一个"占满 chunk 后 `kill -9`"的进程, 再起一个新进程发大消息: 必须走 storage 路径
  (可用 `acquire_storage` 成功率或既有 chunk 计数观测), 不得永久退化;
- 不引入误回收: 正常持有者(长时间持有)的 chunk 不得被别的进程抢走 —— 这是本条的**主要风险**,
  必须有并发压力用例。

> ⛔ **判据侧 BLOCKED(2026-09-18 tester 实测; 详情见 0.3.8(1))** —— ⛔ **上面这两条判据今天
> 判不了本项**。实测: `test_chunk_hold` **3/3 PASSED**、`test_lap_safety` **3/3 PASSED**
> (含 `LappedDrainMustNotShrinkChunkPool`), 且在**残池态 / 清池态**双臂下**逐用例完全相同**
> ⇒ **A=B ⇒ 无判别力**。**阳性对照**坐实了原因(⛔ 非推测): 把 `929792` 池人为置为**完全耗尽**
> (`empty()==true` ⇒ `acquire()` 恒 `-1`)后重跑, 两条判据**仍全绿** ⇒ 它们**结构性不走该池的
> `acquire()` 路径**。⇒ **要验收本项, 必须新补一条能观测该尺寸档「借样成功率 / 可用槽数」的用例,
> 且必须先在合成耗尽池上转红(过阳性对照)才算合格**; ⛔ 不过对照的用例一律不计入验收。
> ⛔ 并须注意: 清理前解出的残池含**自环**(`next_[31]=31`), 成因与 `id_pool::release()` 一致
> (**重复归还 = UF-007 机制**) ⇒ 引用的残池读数**混入 UF-007 效应**, 须先分离。

---

## 4. `dzIPC::StartShutdownMonitor`: 库接管进程退出(设计风险, 未实测成缺陷)

> 🔶 **2026-09-18 写回(状态 [已实测坐实] + opt-out 已落码)**: 本节写于 2026-09-17, 当时机制链是**假说**、
> 且**无 opt-out API**。此后: ①analyst 用 LD_PRELOAD 把"库在**四个工厂入口**覆盖应用处理器"**仪器实测钉死**
> (`SignalHandler` 装上时 `replaced` = 应用自己的处理器、0.40 ms 之后; 退出线程 `std::exit(0)` 的
> `called-from` = `libipc.so.3`); ②tester 的 `exit_semantics_matrix.sh` 坐实 direct / factory **非对称**;
> ③**已新增 opt-out 开关** `bool dzIPC::DisableShutdownMonitor() noexcept` 并验收(**28 PASS / 0 FAIL**)。
> ⇒ 本节正文里"**全部属于推理**"、"**没有 opt-out API**"、"**机制链仍是假说**"三句**已过期**;
> 现行判定与全部限定见 **0.3.12**(`🔶部分修`)与 [shm_defect_fixes.md](shm_defect_fixes.md) **§ 修复记录 15**。
> ⛔ **本节三条风险(退出码恒 0 / 非主线程 `std::exit` / `CleanupIpcInstances` 名不副实)一条都没消** ——
> opt-out 只把"装不装处理器"交还应用, **不动** `std::exit(0)` 那条路径(= UF-009 的边界)。

**事实**

首个 `ServerIPCPtrMake` / `ClientIPCPtrMake` / `PublisherIPCPtrMake` / `SubscriberIPCPtrMake`
都会 `EnsureShutdownMonitorStarted()`, 于是:

```cpp
// src/dzIPC/dzipc.cc:91-113
void StartShutdownMonitor()
{
    std::call_once(shutdown_once, []() {
        std::signal(SIGINT,  SignalHandler);
        std::signal(SIGTERM, SignalHandler);
        shutdown_thread = std::thread([]() {
            while (!shutdown_requested.load(std::memory_order_relaxed)) sleep(100ms);
            dzIPC::logger::StopDzipcLog();
            CleanupIpcInstances();
            std::exit(0);                  // ← 从 detached 线程里结束整个进程
        });
        shutdown_thread.detach();
    });
}
```

**为什么登记它**(如实标注: 以下都是**风险**, 不是已观测到的缺陷)

> ⚠️ **冻结订正(2026-09-17)**: 上面这句与文末「§4 **全部是推理**」**已过期** —— 已有一份
> **现成活体指纹**: `tools/sercli_live_probe/sercli_live_driver.cc:406-407` **自己装了**
> SIGINT/SIGTERM 处理器, 而 `--via factory` 走公共工厂 ⇒ 库**覆盖它自己的处理器**;
> `CleanupIpcInstances()` 只 `clear()` weak_ptr ⇒ `std::exit(0)` 时用户 `shared_ptr` 仍持有实例、
> **析构不发生 ⇒ 段不 unlink**。与 `--via factory` "未打印 `SERVER SUMMARY` + 残留 17 段"逐条吻合,
> 而 direct 3/3 有 SUMMARY 且零残留。⚠️ **但机制链仍是假说**(证据吻合, 判定实验未做) ⇒
> 本项的第一项工作**仍然是做实验**。详见「冻结任务清单」0.6 第 5 条与 UF-009。

1. **覆盖率**: 库**覆盖应用自己的** `SIGINT`/`SIGTERM` 处理器, 且**没有 opt-out API**(只有
   `RequestShutdown()` / `IsShutdownRequested()`, 没有"别装处理器")。应用若自带优雅退出逻辑,
   会被这段代码抢先 `std::exit(0)` 掉。
   ⛔ **该句"且没有 opt-out API"已于 2026-09-18 失效** —— 见本节顶部横幅与 0.3.12: opt-out **已存在**,
   但它**只覆盖"工厂隐式安装"那一条路**(**显式** `StartShutdownMonitor()` 照装), 且**默认路径逐位不变**
   ⇒ "会被这段代码抢先 `std::exit(0)` 掉"对**未调用 opt-out** 的应用**依然成立**。
2. **退出码恒 0**: 被 `SIGTERM` 结束的进程也以 0 退出, 上层(systemd / 脚本 / CI)无法区分
   "正常结束"与"被终止", 也拿不到"信号导致"的信息。
3. **非主线程 `std::exit`**: 会在**其他线程仍在运行**时执行静态对象析构(以及 atexit 链)。
   这是数据竞争与析构顺序未定义的经典来源。§7 排查时它一度是首要嫌疑(现象"发生在所有断言
   通过之后的退出期"), 后来由 ASAN 排除了因果关系 —— 但**风险本身没有被证伪**。
4. **`CleanupIpcInstances()` 名不副实**: 它只是 `clear()` 掉四个 `weak_ptr` 容器
   (`src/dzIPC/dzipc.cc:71-89`), 既不 `stop()` 也不销毁 IPC 实例(实例由用户的 `shared_ptr`
   持有) —— 名为 cleanup, 不保证"干净停止"。
5. **处理器安装是懒触发的**: 应用在创建第一个 IPC 对象**之前**设的处理器会被覆盖, 之后设的
   会覆盖库的 —— 最终行为依赖调用顺序, 这是最难解释的一类现象。

**判据(先做实验, 再决定改不改)**

- 两线程程序(一线程建 IPC 后阻塞, 另一线程忙转 + 建/销对象), 发 `SIGTERM`:
  观察是否有退出期崩溃、日志截断、或析构期数据竞争(ASAN/TSAN 各跑一遍);
- 断言"`SIGTERM` 下退出码不是 0"(当前会失败 —— 那就把现状记下来再讨论要不要改);
- 断言"应用自己的 `SIGTERM` 处理器会被调用"(当前会失败)。
- ✅ **2026-09-18 实测结论(带限定)**: 第三条(应用自己的处理器会被调用)在**开了 opt-out 的臂上可通过**
  (`app_handler_alive`), ⛔ 但**默认臂仍失败**(逐位不变); 第二条"退出码不是 0"**仍未修** ——
  opt-out + 无自有处理器得到的是 `by_signal=15` / rc=143 的 **`SIG_DFL` 硬杀**, 不是"退出码被设计成非 0"。

**修法选项**

1. **默认不装处理器**: 只提供 `RequestShutdown()`, 由应用决定何时退出(把 `std::exit(0)`
   从库里拿掉)。最干净, 但**改变现有应用的退出行为**(dzviz / dzplot 是否依赖它需要先查)。
2. **保留默认 + 提供关闭开关**(`DisableShutdownMonitor()` / `OnShutdown(callback)`):
   向后兼容, 且给应用接管的机会。`std::exit(0)` 改成"调用回调, 没有再退"。
   ✅ **2026-09-18 已采用, 但只落了一半**: `DisableShutdownMonitor()` **已落码**(返回 `bool`, 晚调用返 `false`);
   ⛔ **`OnShutdown(callback)` 未实现**, `std::exit(0)` 那条路径**逐字节未动** ⇒ 本选项原文后半句
   ("`std::exit(0)` 改成调用回调, 没有再退")**未兑现**, ⛔ 不要按它读现状。
3. **只修细节**: 退出码用 `128+signo` 区分、`CleanupIpcInstances` 改名或真的停链路、
   统一"先 `RequestShutdown()` 再由主线程退出"。不改语义, 但至少让行为可预期。

---

## 5. 已登记在别处的未修项与设计待办(不在此重复内容)

| ID | 项 | 位置 | 状态 |
|---|---|---|---|
| **UF-005a** | 组播**组地址碰撞**: 8192 topic 下 60% 共组, 默认 `msg_id = 0` 时静默错收 | [shm_defect_fixes.md](shm_defect_fixes.md) §5 | ⏸️ **待决策** —— 已实测, 修法已定(改端口公式 ⇒ 与旧版本不互通) |
| **UF-005b** | 端口公式的乘性冲突(`domain*hash` 可撞同一端口) | [shm_defect_fixes.md](shm_defect_fixes.md) §3「建议的修法 1」⚠️(原写"§3 末尾'未处理但已登记'", 该小节**读不到**) | ⏸️ **口径打架, 待裁** —— 本文写"归入上一条的重设计", 而该文档 `:181-186` 原文是**独立条目 + 独立实验判据**("未实测…先做实验再决定修不修") |
| **DF-001** | DZFlat 库级默认 **OFF** 的灰度约束: 未升级订阅方会静默丢段, schema 演进从"向前兼容"变"硬同步" | [dzflat_shm.md](dzflat_shm.md) §9 | ⏸️ **设计待办, 待决策** —— 设计契约, 未改(现走"应用层选择性开启") |
| **DF-002** | 无 per-topic / per-publisher 的 DZFlat 开关(只有进程级 `EnableDzFlat`) | [dzflat_shm.md](dzflat_shm.md) §9 | ⏸️ **设计待办, 待决策** —— 未实现(全局开关 + 类型级 + 时序级是今天的替代) |
| **DF-003** | 订阅侧**没有**开关(恒双 wire): 无法表达"这个订阅者不要借样" | 同上 | ⏸️ **设计待办, 待决策** —— 设计如此, 未实现反向控制 |
| **DF-004** | 同机多订阅者共享一个 SHM 入口 | [local_shm_fanout.md](local_shm_fanout.md) | ⏸️ **设计待办, 待决策** —— **第 3 行**即"设计文档, **未实施**"(原写"首行") |
| **DF-005** | Python 发布端仍有**一次 memcpy**(段在 Python 地址空间生成) | [dzflat_shm.md](dzflat_shm.md) §9.7 | ⏸️ **设计待办, 待决策** —— 真零拷贝写 = 下一个里程碑 |

> **`DF-001`–`DF-005` 的后续入口**: 这五条是**设计待办而非缺陷**(与 `UF-*` 分开编号, 理由见
> 0.6 第 12 条), **本轮不进产品修复**, 立项须先由 leader 拍板 —— **入口 = 0.5 第 7 条**,
> 且 `DF-001`/`DF-002`/`DF-003` 建议与 **UF-005a/UF-005b 的兼容性决策同批**(三者都落在
> "未升级订阅方怎么办"这同一个问题上)。0.1 表与本节**一一对应**, 两处必须同步。

---

## 6. 复现手段(共用手册)

### 6.1 定向分配失败注入

目的是把"真实 OOM"变成可重复的实验。两个办法:

1. **`LD_PRELOAD` 拦截 `malloc`**: 按尺寸档(或调用计数)返回空。注意要**放过初始化期**的分配,
   否则进程根本起不来; 建议"从第 N 次、尺寸落在 [lo, hi) 的分配开始失败"。
   实测点: `handle_`(pimpl 的"不舒服"分支)与 `conn_info_t`(224 字节, 走 tcache 尺寸类) ——
   两个尺寸档各是一个崩点。
   ✅ **2026-09-18 已落地为工装 `tools/alloc_fault_inject/`**(UF-000; 见 0.3.1/0.3.2 与
   [shm_defect_fixes.md](shm_defect_fixes.md) § 修复记录 8): `tools/alloc_fault_inject/build.sh`
   构建, `LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so <被测进程>` 运行,
   退出时打 `[alloc_fault_inject] FINAL … band_calls=… hits=…` 汇总行。
   ⛔ **尺寸不要写死**: `handle_` 的实测尺寸随头文件变化(2026-09-18 实测 `sizeof(handle_)=64`;
   旧示例里写死的 `56` 是 `id_t(8)+void*(8)+ipc::string(32)+size_t(8)` 的**推算值**, 实测**未命中**)。
   工装默认**自标定**, 用它。
   ✅ **2026-09-18 已订正**: 工装 `README.md` §1/§2/§3 的过期尺寸示例已改为**自标定调用**, 并加了
   "`lo`/`hi` 不要写死"的**尺寸表**(旧值 `56` 标为"过期·推算", 新值 `64` 标为"2026-09-18 实测");
   文件 133 → **159** 行, md5 `0ae3ae57413e74efe9be0c3ec3d0ab30`。⚠️ 同一过期数字仍残留在
   `run_acceptance.sh:47-52` 与 `fi_positive_control.cc:22`, **尚未订正**(见 0.3.2 限定 6)。

2. **链接期 wrap**: `-Wl,--wrap=malloc` 或直接替换 `libipc` 里的 `pool_alloc::alloc` —— 更可控,
   但需要重建库(见 §6.2 的单建法)。

### 6.2 ASAN 单建 libipc + 探针(拿"因果", 不只是"现象")

不必重 configure 整仓 —— libipc 只有 12 个 TU(注意 `a0_*` 是 C, 要用 `gcc` 先编成 `.o`):

```bash
gcc -fsanitize=address -g -I src -I src/libipc/platform -I src/libipc/platform/linux -I include \
    -c src/libipc/platform/platform.c -o /tmp/platform.o
g++ -std=c++17 -fsanitize=address -g -DLIBIPC_LIBRARY_SHARED_USING__ \
    -I include -I src -I src/libipc/platform -I src/libipc/platform/linux -I . \
    probe.cpp src/libipc/{buffer,ipc,pool_alloc,shm,sniffer}.cpp src/libipc/socket/udp.cpp \
    src/libipc/sync/*.cpp src/libipc/platform/posix/shm_posix.cpp /tmp/platform.o \
    -o probe_asan -lpthread -lrt
```

### 6.3 残池清理(§3 的手工版)

**先确认没有任何活进程映射这些段**, 再删:

```bash
ls /dev/shm | grep CHUNK_INFO                     # 池段: __IPC_SHM__...CHUNK_INFO__<size>
grep -l CHUNK_INFO /proc/*[0-9]/maps 2>/dev/null   # 有输出 ⇒ 有活进程在用, 不要删
                                                   # (⛔ 此处刻意让通配符不紧跟斜杠: "星号+斜杠" 这个双字符
                                                   #  序列一旦被粘进 C++ 的块注释里会**提前闭合**注释并炸编译。
                                                   #  旧文用的是 "/proc/星号斜杠 maps", 本行已改写;
                                                   #  二者 bash 语义等价(pid 目录名都以数字结尾), 判据不变)
ls /dev/shm/__IPC_SHM__*CHUNK_INFO__* | xargs -r rm -f
```

清理前请先接受一件事: 清完之后, **之前那些"稳定失败"的用例会突然变绿**。那不是修好了,
是环境干净了 —— 反过来, 看到"某用例从绿变红"时也请先查一遍是不是刚崩过进程。

### 6.4 "只有一个 ip 的 SIGSEGV" 三步收敛法

1. **守门人**: 预加载一个 `SIGSEGV`/`SIGBUS` 处理器, 打印寄存器 + `backtrace_symbols_fd`,
   然后循环跑真实复现脚本(单次运行抓不到的是堆运气, 多跑)。
2. **符号化**: `addr2line -f -C -i -e build/lib/libipc.so <偏移>` 把栈落成函数名。
3. **坐实**: §6.2 的 ASAN 探针给出 alloc/free 双方栈 —— 这一步才叫"因果", 前两步只是"位置"。

---

## 7. 建议的处理顺序

> ⛔ **本节顺序已被「冻结任务清单与执行协议」0.2 取代(2026-09-17)** —— 无 **UF-000**(分配失败
> 注入工装)时, 第 1、2 步的判据**不可执行**(只能验"不注入时全绿", 那是回归不是判据)。
> 「UF-004 / UF-009 判定实验」不需要任何产品改动、成本最低, 故前置。本节保留作为**当时理由**的记录。

1. **§1(放大器)先做**, 因为它让后面所有"分配失败"从"无诊断的崩溃"变成"可观测的错误"。
   最小形态是选项 2(越界抛 + 失败记日志), 不必一次做到全标准语义。
2. **§2(同族空指针收口)** 紧随其后: `buffer` 已有样板, 照抄到 `handle` / `udp` / 三个同步原语;
   判据必须含"注入后不崩"的用例, 否则修完没法回归。
3. **§4(退出语义)** 排第三: 它影响**所有**应用的排障体验(退出码、信号、析构期), 而且是
   先"做实验记录现状"就能推进的一项, 不必等设计定稿。
4. **§3(池回收)** 最后: 它需要改共享段布局, 与"是否与旧版本进程互通"的发布决策绑定;
   在决策之前, 至少把 §6.3 的手工清理写进排障手册(已经写在本文件里)。
5. **§5 里的各项** 按各自文档的结论推进(组地址碰撞是其中最值得优先决策的: 规模相关 + 静默错收)。

---

## 附: 一条读法提醒

本文档每条都区分了**"已实测"**与**"推理"**:

- §1 的症状有实测(§7 的两次内核日志 + ASAN 因果链), 触发条件是推理;
- §2 的崩溃有实测(定向注入), 影响面是推理(只验了 `handle`);
- §3 有实测(10/20 残池), 修法风险的评估是推理;
- §4 原写 **全部是推理** —— ⚠️ **已过期**: 2026-09-17 已有现成活体指纹(见 0.6 第 5 条),
  **2026-09-18 更升为仪器实测**(LD_PRELOAD 实测库在**四个工厂入口**覆盖应用处理器, 见 0.3.12(a));
  ⇒ 它"第一项工作**仍然是做实验**"这句**已完成**(实验做了、开关也落了), ⛔ 但**风险只解除了一半**
  (默认路径逐位不变 / `std::exit(0)` 路径未动 / Python 面不可达) ⇒ 判定停在 **`🔶部分修`**。

不要把本文档里的"推理"当成"实测"。§7 那次排查最贵的教训就是: 症状相同的两个缺陷(§1 与 §2)
在**同一个内核签名**下长得一模一样, 而真正的修法在完全不同的地方 —— 只能靠实验区分。
