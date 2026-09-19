# 未修缺陷登记(台账 · 精简版)

> 2026-09-19 压缩(原 1853 行): 只留状态/结论/限定。0.3.x 写回/待裁/订正/角色接口/命令证据见存档
> **unfixed_defects_full_v1.md**(同目录),锚点到存档解析;施工详情见 shm_defect_fixes.md §8/12/13/14/15/16/17。基线 3818899。
> **证据规范(RELEASE_NOTES.md, 0918 起)**: 验收/判定记录须绑定三方指纹 **driver_md5 / libipc_md5 / tree_head**,
> 缺一即无效;无指纹旧记录仅作线索;⛔禁事后补写指纹键;漂移后引旧读数须带指纹复跑对账(233904_fp 已佐证读数可迁移)。
> 状态: ⬜未修/🔶部分修/⏸️待决策/✅已修/❌作废。⛔单写者协议: 不得直接编辑,变更经 writer-claude。

## 1. 任务总表(优先级/负责人见存档 0.1)

| ID | 标题 | 状态 | 详情 |
|---|---|---|---|
| UF-000 | 分配失败注入工装 | ✅阳性 rc=139;注入自报计数 | 8 |
| UF-001 | allocator noexcept 放大器 | ✅去 noexcept+bad_alloc | 12 |
| UF-002 | pimpl 失败→全线空解引用 | ✅五类入口+析构判空;⚠️buffer.cpp:59 变异阴性待裁 | 13 |
| UF-003 | chunk 池无崩溃回收 | ⬜⛔等互通性决策;判据 BLOCKED | §3.2 |
| UF-004 | 库接管进程退出 | 🔶opt-out 验收 28/0;OnShutdown/退出码未修 | 15 |
| UF-005a/b | 组播组碰撞+端口冲突 | ⏸️兼容性+口径打架,同批 | fixes §5/§3 |
| UF-006 | ipc_info_pool 表满静默 -1 | ✅表满路径回收+变异三臂 | 14 |
| UF-007 | id_pool 自环 | ❌已作废(路径不可达),非已修 | 存档 0.3.3 |
| UF-008 | sniffer 判据缺陷 | ⏸️仅证据,不授权改产品 | analyst 报告 |
| UF-009 | factory 残留 17 段+缺 SUMMARY | ✅链式接管+500ms 宽限;17→0;变异转红;UF-004 7/7 | §17 |
| DF-001~005 | DZFlat 设计待办 | ⏸️非缺陷,不进本轮修复 | dzflat_shm §9 |

## 2. 顺序与横切(0.2–0.4)

- 剩余: UF-003(等决策);UF-005a/b+DF-*(等拍板;DF 入口=存档 0.5 第 7 条)。
- 横切: ① 验收 `cmake -S . -B build && cmake --build build -j8 --target <t>`;新 test_*.cpp 须重跑 cmake。
  ② test_mem.cpp 空二进制非验收物。③ libipc 改动重跑 F4 红线。④ 池类判据先清残池(原 0.3 第 4 条)。⑤ 必附变异验证。

## 3. 本体速览(论证见存档 §1–§5)

- UF-001: allocate 失败返 nullptr 且 noexcept ⇒ 分配失败伪装成空指针崩溃。
- UF-002(按修法选项 1): pimpl 失败→p_=nullptr,handle(19)/udp(9)/mutex/cond/sem(各 9)无判空,样板 buffer.cpp:81-99。
- UF-006: 修后仅表满路径回收死条目。UF-007: 触发路径不可达故作废。
- UF-003(⬜): pool_ 在共享段,`id_pool::release` 唯一归还路径且无 owner/存活检查 ⇒ 持有者被杀槽永久丢失并污染后续进程。修法: owner+存活(pid+starttime)/清理工具/纪元化。判据: kill -9 占满后大消息仍走 storage。⛔chunk_hold/lap_safety 无判别力,先补借样成功率用例过阳性对照先转红。
- UF-004(🔶): 工厂入口覆盖应用信号处理器,`std::exit(0)` 栈不展开、段不 unlink、退出码恒 0。已落码 `DisableShutdownMonitor`。未修: OnShutdown、退出码 128+sig、CleanupIpcInstances;opt-out+无处理器=SIG_DFL 硬杀 ⇒ 分类处理。
- DF-*: DZFlat 默认 OFF 致未升级订阅方静默丢段;无 per-topic/订阅侧反向开关;SHM 扇出未实施;Python 一次 memcpy。

## 4. 复现手册要点(命令见存档 §6.2)

- 注入工装: LD_PRELOAD `liballoc_fault_inject`;无注入判据用例必须 SKIP(假绿已堵)。
- 残池清理(原 §6.3): 先 `grep -l CHUNK_INFO /proc/*[0-9]/maps` 确认无活进程再删段;清池后"稳定失败变绿"非修好。
- 单 ip SIGSEGV: 预载处理器拿 backtrace → addr2line → ASAN 双栈坐实因果。
