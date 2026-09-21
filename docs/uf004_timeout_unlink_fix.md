# UF-004 保留面收口: 超时收尾栈不展开 ⇒ 段名扫除(unlink_created_segments)

> 状态: ✅已修(保留面收口 + UF-004 子项第二批收口, 见 §6)。指纹(证据规范, RELEASE_NOTES.md §4):
> tree_head=`90510a5`+工作树(本文档随其一), shm_posix.cpp=`e5269e11`, shm.h=`d9394973`,
> dzipc.cc=`a286b81c`, dzipc.h=`4f0f7d7e`, libipc_md5=`aaf28f18`,
> 判据驱动 md5=`76c8c717`(test_uf004_timeout_unlink)/`7d2a0b13`(test_uf009_graceful_exit)。

## 1. 缺陷(UF-009 有条件修复的保留面)

dzipc.cc 的两条 std::exit 收尾路径栈不展开:

- 超时路径: 信号回放/复权后 500ms 宽限耗尽(ShutdownMonitorThreadBody 尾部);
- RequestShutdown 内部退出路径。

main 里的 IPC 实例 shared_ptr 不析构 ⇒ 段不经析构 unlink ⇒ factory 路径的"残留 17 段"在超时路径上依然发生。UF-009 落码时如实登记为保留面(审计文档 §3.1)。

## 2. 修法: 创建者名单 + 名字级扫除

不触碰任何 C++ 实例(不强拆、与优雅路径兼容), 在 shm 层补"谁建的段谁认领":

| 组件 | 落点 |
|---|---|
| 创建者判定 | `shm_posix.cpp acquire()`: O_CREAT\|O_EXCL 试探, 成功即本进程创建(原子判定); EEXIST 回落 attach。default(create\|open) 语义逐位保留——纯 create 对已存在段仍失败、open 永不登记; 探测-attach 之间被 unlink 的竞态以一次 O_CREAT 重试补齐 |
| 名单 | 进程级注册表 `vector<(pid, name)>`(mutex 保护); release/remove unlink 段名时摘除条目, 防止段易主后同名误扫 |
| 扫除 | `ipc::shm::unlink_created_segments()`: 对 pid==getpid() 的条目逐个 shm_unlink, 只删名字、不碰映射/实例; fork 继承条目(pid 不符)退回名单不误扫 |
| 接线 | dzipc.cc 两条 exit 路径在 StopDzipcLog 前调用扫除; 原 ⛔ 保留面注释改写为收口态契约 |
| win | 空架子(命名内核对象随最后句柄消失, 无可扫) |

与既有机制的相容性:

- 优雅路径(UF-009): 实例析构 unlink 段名并摘除条目 ⇒ 扫除时该条目已不在, 双路径无冲突;
- std::exit 后静态析构的 release(): 引用计数单次扣减语义不变(扫除只删名字);
- 名字已被 release 删过: 扫除 shm_unlink 返回 ENOENT, 无害。

## 3. 判据(test_uf004_timeout_unlink, 2 用例)

- 臂 1(进程内直测): default 建段登记; 扫除后创建者段名消失、attach-only(原生 shm_open+attach)豁免; 二次扫除幂等; 纯 create 对已存在段仍 EEXIST(原语义保留回归)。
- 臂 2(fork, 承重): 子进程 factory 建 pub(实测 12 段映射)+SIG_DFL+raise ⇒ 库走超时路径(扫除+exit(0)); 父进程断言: 子 exit 0、子 tag 段**零残留**(保留面收口)、父进程 fork 前建段**存活**(pid 守卫不被子进程扫除误删)。
- ⚠️ 防复发: 子进程内不得复用 run_tag() 类 static tag(父进程 fork 前已初始化, 继承后与父 tag 同串, 父段名会落进子的零残留检查)——臂 2 曾因此假红一轮, 已在测试内注明。

## 4. 验证(实跑)

- 专项 2/2 × 3 轮绿; uf009 3/3 × 2 轮(优雅/SIG_DFL/RequestShutdown 三臂, 后者现走扫除)、uf004_optout 7/7、loan 10/10、adopt_loan_quota 5/5、dzflat_transport 9/9、uf007 2、uf010 5、uf011 1、chunk_hold 3、lap_safety 3、pool_exhaust 2 —— 全绿。
- 残段协议后 /dev/shm `__IPC_SHM__*` 归零。

## 5. 已知边界

- 扫除只回收"本进程以 create 模式建出的段名"; attach-only 段归其创建者/引用计数管辖——语义边界, 非遗漏。
- ~~CleanupIpcInstances 仍不析构实例~~ → §6 裁决: 实例析构为刻意边界(强拆=退出竞态崩溃), 安全子集由扫除+回调覆盖。
- 名单为进程生命周期内存结构, 高频建段线性增长(每条约几十字节, 量级无关紧要)。

## 6. UF-004 子项第二批收口(2026-09-20 同日)

台账 UF-004 🔶 剩余三个子项的处置:

| 子项 | 处置 |
|---|---|
| 退出码恒 0 | **已修**: 超时路径 `std::exit(128+signo)`(SIGINT→130/SIGTERM→143)——超时即"应用未在宽限内自行退出", 对监督者等价于死于信号; RequestShutdown 内部路径保持 `exit(0)`(非信号死亡)。契约更新: uf009 臂 2(SigDfl)、optout 四臂、本套件臂 2 由 code==0 改判 130 |
| OnShutdown 回调钩子 | **已补**: 新增 `dzIPC::RegisterShutdownCallBack(ShutdownCallBackFun)` —— `void(int signo)`, 超时路径 signo=实际信号、RequestShutdown 路径 signo=0; 两条 exit 路径在扫除/StopDzipcLog **之前**按注册顺序快照执行, 异常吞掉, swap 语义(回调内再注册不保证本轮被调); 优雅路径不触发(应用收尾在 main 栈展开里)。边界: 监控线程未启动时永不触发; Python 绑定未暴露(后续按需) |
| CleanupIpcInstances 实例析构 | **裁决为刻意边界**: 实例由 main 栈 shared_ptr 持有, 监控线程无法安全强拆(异线程析构=使用中对象竞态崩溃); 其安全子集(段名回收/应用侧收尾)已由 ① 扫除 + ② 回调覆盖。台账注明, 不再列为"未修" |

判据增量: test_uf004_timeout_unlink 增至 4 用例(臂 3=超时路径回调 signo=2+code 130; 臂 4=RequestShutdown 回调 signo=0+code 0), 4/4 × 2 轮绿; uf009 3/3 × 2 轮、optout 7/7(退出码契约更新后)、adopt_loan_quota 5/5、loan 10/10 全绿。

另: UF-002 尾巴(`buffer.cpp:59` 变异阴性待裁)同日裁决为**保留**——该改动是形式 UB 消除, 变异阴性=行为等价恰为预期; 见台账 UF-002 行与详情。
