# 未修缺陷登记(台账 · 精简版)

> 2026-09-19 压缩(原 1853 行): 只留状态/结论/限定。0.3.x 写回/待裁/订正/角色接口/命令证据见存档
> **unfixed_defects_full_v1.md**(同目录),锚点到存档解析;施工详情见 shm_defect_fixes.md §8/12/13/14/15/16/17。基线 3818899。
> **证据规范(RELEASE_NOTES.md, 0918 起)**: 验收/判定记录须绑定三方指纹 **driver_md5 / libipc_md5 / tree_head**,
> 缺一即无效;无指纹旧记录仅作线索;⛔禁事后补写指纹键;漂移后引旧读数须带指纹复跑对账(233904_fp 已佐证读数可迁移)。
> 状态: ⬜未修/🔶部分修/⏸️待决策/✅已修/❌作废。⛔单写者协议: 不得直接编辑,变更经 writer-claude。
> (2026-09-19 收尾闭环写回: UF-010 判 ✅已修——修法/判据/复跑见 docs/uf010_hash_consistency_fix.md;
> 新登记 UF-011(池空闲链二次入池, ⏸️仅登记);UF-007 行补 UF-011 交叉注记。)
> (2026-09-20 写回: 新登记 UF-012(adopt 借样无界钉池)→ 同轮判 ✅已修——借样配额+池扩容 40/钉上限 10,
> 修法/判据/复跑见 docs/adopt_loan_quota_fix.md;专项 5/5 绿, 回归面 12 驱动全绿。)

## 1. 任务总表(优先级/负责人见存档 0.1)

| ID | 标题 | 状态 | 详情 |
|---|---|---|---|
| UF-000 | 分配失败注入工装 | ✅阳性 rc=139;注入自报计数 | 8 |
| UF-001 | allocator noexcept 放大器 | ✅去 noexcept+bad_alloc | 12 |
| UF-002 | pimpl 失败→全线空解引用 | ✅五类入口+析构判空;buffer.cpp:59 已裁(2026-09-20): 保留——形式 UB 消除, 变异阴性=行为等价恰为预期 | 13 |
| UF-003 | chunk 池无崩溃回收 | ⬜⛔等互通性决策;判据 BLOCKED | §3.2 |
| UF-004 | 库接管进程退出 | 🔶opt-out 验收 28/0;段不 unlink+退出码 128+sig+ShutdownCallBack 钩子均已收口(2026-09-20);CleanupIpcInstances 实例析构裁决为刻意边界 | 15 + uf004_timeout_unlink_fix.md |
| UF-005a/b | 组播组碰撞+端口冲突 | ⏸️兼容性+口径打架,同批 | fixes §5/§3 |
| UF-006 | ipc_info_pool 表满静默 -1 | ✅表满路径回收+变异三臂 | 14 |
| UF-007 | id_pool 自环 | ❌已作废(路径不可达),非已修;同型后果在 UF-011 登记的路径上实发(2026-09-19 直接指纹) | 存档 0.3.3 + UF-011 |
| UF-008 | sniffer 判据缺陷 | ⏸️仅证据,不授权改产品 | analyst 报告 |
| UF-009 | factory 残留 17 段+缺 SUMMARY | ✅链式接管+500ms 宽限;17→0;变异转红;UF-004 7/7 | §17 |
| UF-010 | ipc::hash<string> 按指针哈希 ⇒ 句柄缓存永不命中/重复 mmap | ✅已修(按内容哈希 90510a5;广播用例缺陷态红20/20→修复态绿10/10;复跑 18 驱动全绿) | uf010_hash_consistency_fix.md |
| UF-011 | 池空闲链二次入池(回收竞态, 非 UF-007 的不可达路径) | ⏸️仅登记, 判据构建待排 | shm_chunk_pool_occupancy_plan.md §3步骤③/§5(a) |
| UF-012 | adopt 借样无界钉池: GenericMessage 收 schema-less DZFlat 段借进 msg_queue_, 深度=用户 queue_size 可钉穿池 | ✅已修(借样配额=ViewQueueCap()=10;满即物化+spilled 计数;池扩容 32→40 对齐 ROS depth;驱逐/pop/入队三路记账;fast-path 借样物化) | adopt_loan_quota_fix.md |
| DF-001~005 | DZFlat 设计待办 | ⏸️非缺陷,不进本轮修复 | dzflat_shm §9 |

## 2. 顺序与横切(0.2–0.4)

- 剩余: UF-011(⏸️新登记, 判据构建待排);UF-003(等决策);UF-005a/b+DF-*(等拍板;DF 入口=存档 0.5 第 7 条)。
- UF-012 已修(✅): 剩余钉面=用户手持期(与 view 路径同一契约, 不设限, 靠 spilled/池观测兜底感知)。
- 横切: ① 验收 `cmake -S . -B build && cmake --build build -j8 --target <t>`;新 test_*.cpp 须重跑 cmake。
  ② test_mem.cpp 空二进制非验收物。③ libipc 改动重跑 F4 红线。④ 池类判据先清残池(原 0.3 第 4 条)。⑤ 必附变异验证。

## 3. 本体速览(论证见存档 §1–§5)

- UF-001: allocate 失败返 nullptr 且 noexcept ⇒ 分配失败伪装成空指针崩溃。
- UF-002(按修法选项 1): pimpl 失败→p_=nullptr,handle(19)/udp(9)/mutex/cond/sem(各 9)无判空,样板 buffer.cpp:81-99。
- UF-006: 修后仅表满路径回收死条目。UF-007: 触发路径不可达故作废;同型后果链(重复入池 ⇒ 自环)在 UF-011 登记的路径上实发, 两者不矛盾。
- UF-010: `ipc::hash<string/wstring>` 特化按 `c_str()` 指针哈希, 与容器 `std::equal_to`(按内容)语义不一致 ⇒ 内容相同的 key 永不命中 ⇒ chunk 池句柄缓存反复 emplace、同一 shm 段反复 mmap 且不 munmap、多接收者零拷贝广播地址分裂。已改按内容哈希(`src/libipc/memory/resource.h`, 90510a5);端到端判据 `Loan.BroadcastToMultipleReceivers` 缺陷态稳定红 → 修复态稳定绿。证据链原始记录: uf010_evidence_registration.md。
- UF-011: 池空闲链**二次入池**——`pop()` 先拷出槽位、后才清 rc 位的残余窗口被 DZFlat Sample 长期持有拉成秒级 ⇒ `release(id)` 头插时 `cursor_` 已是 id ⇒ `next_[id]==id` 自环 ⇒ `acquire()` 永返同块、多生产者共块互踩。与已作废 UF-007 的区别: 该路径**实发**(2026-09-19 直接指纹 `next_[23]==23`)。机制登记/指纹/判据思路见 shm_chunk_pool_occupancy_plan.md §3 步骤③ 副产品与 §5 发现(a)。
- UF-012: schema-less 话题(GenericMessage)收大段 DZFlat 时 `dzflat_adopt` 把活 chunk 借进 `msg_queue_`(深度=用户 queue_size, 不受步骤③的 view 队列钉制约束)⇒ 一个慢消费者即可钉穿 32 块/尺寸档的池, 全档位 loan 退化 TLV。修法(池侧→借样侧): adopt 借样配额=ViewQueueCap(), 配额满即 dzflat_read 物化(每消息一次拷贝, 非池饿死);计数三路(入队+1/pop−1/满队驱逐−1, 驱逐经 CircularQueue 新增 evict 回调);发布 fast-path 借样 clone 物化防绕过;池扩容 32→40(对齐 ROS 2 默认 QoS depth=10, 钉上限=40/4=10, 4 满钉订阅者余量不变);段名编码容量 __C<cap> 防跨版本混挂(兼收 UF-003 档案登记的"无版本标记"缺口)。判据: test_adopt_loan_quota 5 用例(配额分割/排空恢复/驱逐不漂移/常态零拷贝/超限警告)。
- UF-002 尾巴裁决(2026-09-20): `buffer.cpp:59` 析构判空(2026-09-18 落码)变异阴性 —— 裁定为**保留**: 该改动消除的是形式 UB(默认构造 buffer 上 `p_->clear()` 为无诊断 UB), 行为等价恰是变异阴性的预期含义; 真实崩点在访问器 `impl(p_)->p_` 且已修。原"该文件不在 UF-002 修改边界列"的流程顾虑以本裁决收口, 不再回退。
- UF-003(⬜): pool_ 在共享段,`id_pool::release` 唯一归还路径且无 owner/存活检查 ⇒ 持有者被杀槽永久丢失并污染后续进程。修法: owner+存活(pid+starttime)/清理工具/纪元化。判据: kill -9 占满后大消息仍走 storage。⛔chunk_hold/lap_safety 无判别力,先补借样成功率用例过阳性对照先转红。⚠️排期时与 UF-011(二次入池竞态)一并评估, 同在池归还路径。
- UF-004(🔶): 工厂入口覆盖应用信号处理器,`std::exit(0)` 栈不展开、段不 unlink、退出码恒 0。已落码 `DisableShutdownMonitor`。**三个子项已收口(2026-09-20)**: ① 段不 unlink —— `shm::unlink_created_segments()` 名字级扫除接入超时/RequestShutdown 两条 exit 路径; ② 退出码 —— 超时路径 `std::exit(128+signo)` 如实上报信号(此前恒 0 掩盖), RequestShutdown 保持 0; ③ OnShutdown 钩子 —— 新增 `RegisterShutdownCallBack(ShutdownCallBackFun)`(signo=信号/0), 收尾最早时机快照执行、异常吞掉。修法/判据/指纹见 uf004_timeout_unlink_fix.md(判据 test_uf004_timeout_unlink 4 用例含回调两臂, uf009 三臂+optout 7 臂契约更新后全绿)。**CleanupIpcInstances 实例析构裁决为刻意边界**: 监控线程强拆 main 栈持有的实例=退出竞态崩溃, 段名级扫除+回调钩子已覆盖其安全子集;opt-out+无处理器=SIG_DFL 硬杀 ⇒ 分类处理。
- DF-*: DZFlat 默认 OFF 致未升级订阅方静默丢段;无 per-topic/订阅侧反向开关;SHM 扇出未实施;Python 一次 memcpy。

## 4. 复现手册要点(命令见存档 §6.2)

- 注入工装: LD_PRELOAD `liballoc_fault_inject`;无注入判据用例必须 SKIP(假绿已堵)。
- 残池清理(原 §6.3): 先 `grep -l CHUNK_INFO /proc/*[0-9]/maps` 确认无活进程再删段;清池后"稳定失败变绿"非修好。
- 单 ip SIGSEGV: 预载处理器拿 backtrace → addr2line → ASAN 双栈坐实因果。
