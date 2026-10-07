# 本机延迟根因针对性修复执行方案

## 1. 文档定位

本方案依据 `results.md`、`execution.md`、D02/D03 原始汇总和当前 `shared_v1` 源码制定，目标是把三个历史未通过项分别定位、修复并用可复核证据关闭。它是执行顺序和准入规则，不是对尚未验证的根因作结论。

本批版本为：

- A：`f066a82c1b4b10ea28322f86042bf6da5e122318`；
- B：`94856f7bcef69e054ce679420dd8b7ee8e6b0c1e`；
- C：尚不存在，不能开始正式 D12 验收。

D02-006 已完成 24/24 个定位窗口，D03-001 已完成 16/16 个应用缝窗口，完整性均为零错误；但 `/home/zwc` 被声明为环境活动根，包含用户要求保留的 JAX 活动，因此它们是同期定位对照，不是绝对静置的正式成绩。D04 因 `perf_event_paranoid=4`、无非交互 `sudo` 和不可读的 tracefs/debugfs 暂停。没有调度链证据时，不得用旧候选的栈数据替代当前 A/B。

## 2. 证据和问题边界

### 2.1 三项历史失败和当前状态

历史正式候选 `9f536dc1` 的失败为：

| 场景 | 历史 A | 历史候选 | 超出 |
|---|---:|---:|---:|
| 1 SUB / 4KiB p50 | 26.862 µs | 63.986 µs | +32.124 µs |
| 1 SUB / 1MiB p50 | 108.485 µs | 120.911 µs | +1.578 µs |
| 32 SUB / 64B p99 | 195.178 µs | 222.719 µs | +8.023 µs |

当前 D02-006 的 B−A 为：

| 场景 | A | B | B−A |
|---|---:|---:|---:|
| 1 SUB / 4KiB p50 | 29.603 µs | 16.205 µs | −13.398 µs |
| 1 SUB / 1MiB p50 | 94.610 µs | 68.760 µs | −25.850 µs |
| 32 SUB / 64B p99 | 209.785 µs | 167.999 µs | −41.786 µs |

因此三项历史失败在当前短窗中都未复现，不能宣布已经修复，也不能把当前 B 的改善归因于某一项代码改动。

### 2.2 当前可用的分段事实

B 的 D03 L1 汇总显示：

- 1 SUB / 4KiB：发布起点到 `recv_return` 的 p99 为 `154.912 µs`；`recv_return→入队`、`入队→出队`、`出队→API 返回` 的 p99 分别为 `4.880`、`7.844`、`2.157 µs`。
- 32 SUB / 64B：发布起点到 `recv_return` 的 p99 为 `180.272 µs`；后三段 p99 分别为 `6.890`、`25.489`、`3.006 µs`。
- 1MiB 的 D02 发布 p50 为 B `62.277 µs`、A `60.495 µs`，但 B 的端到端 p50 反而低于 A `25.850 µs`；这只说明需要把发布复制段单独测量，不能把整段差异直接写成 `memcpy` 根因。

历史独立实验可用于形成假设，但不能作为当前 B 的因果证据：旧数据曾观察到特定核型上的 1MiB 复制 CPU 时间增大，AVX2 预取和 16KiB 分块没有稳定收益；旧候选的 4KiB 唤醒等待差异、32 SUB 广播唤醒累计 `31–32 µs` 以及 `waking→wakeup` `126/142 µs` 长尾仍需在当前 A/B 重建。

### 2.3 代码边界

修复前必须以当前源码为准，重点位置如下：

- `src/dzIPC/shm_pub_sub_ipc.cc`：`process_received_buffer`、`sub_recv_attempt`、`assisted_pop`、`SubRecvRoute::recv_once`、`publish_best_effort`、`publish_blocking`、`try_publish_dzflat`、`start_recv_path`、`fallback_to_compat`、`compat_recv_loop`、`teardown_recv_path`；
- `src/dzIPC/threepools/recv_worker.cc`：`run_budget`、`wait_once`、`assist_route` 和池的固定 route 归属；
- `include/dzIPC/threepools/recv_worker.h`：单消费者、预算、协作接收和禁止全量忙轮询的契约；
- `src/libipc/recv_wait_set.cpp`：`recv_local_signal::notify/wait`、`recv_wait_set::wait`、Linux `futex_waitv` 双槽/多槽等待；
- `include/dzIPC/shm_route_session.h`：lease、代次、重建、停止、`wait_quiescent` 的 I1–I5 生命周期协议。

## 3. 不可改变的约束

以下项目在所有诊断和候选中保持不变：

1. 不以可读探测、短睡眠自旋或全量忙轮询换取延迟；兼容路径的阻塞语义和 worker 的 wait-set 语义都必须保留。
2. 不修改电源策略、内核 sysctl、正式门槛、消息频率、负载大小或统计口径来制造收益；绑核只用于诊断，不是正式成绩。
3. 不减少复制、校验、完整性检查、DZFlat schema/id 检查、队列入队或广播接收次数；不得用丢通知解决尾延迟。
4. 不改变 publisher identity、发布序号、route key、generation、lease 配对、I5 已弹出字节不丢失、取消、停止、析构、重建和回退语义。
5. `recv_once()` 仍由固定 owner worker 或兼容线程中的单一消费者调用；getter 协作仍须经过消费锁，不能产生第二个并发消费者。
6. 强制兼容开关 `DZIPC_SHM_RECV_COMPAT`、池容量和预算只作为对照或能力验证，不得把回退线程静默当作 worker 成绩。
7. 不读取、停止、重排或修改 `/home/zwc/MPC_GPU` 的 JAX 任务。环境活动必须记录；受其影响的窗口只能标记为定位对照。

## 4. 统一诊断缝和 D04 前置能力

### 4.1 事件契约

在 A/B 的诊断构建中使用同一 `CLOCK_MONOTONIC` 时间域，所有事件至少记录：`timestamp_ns`、进程/TID、CPU、模式（A/B）、场景、发布 sequence、订阅者 id、route key、generation、事件名称和载荷字节数。每个发布 sequence 的事件必须能和发布 CSV、每个订阅者 CSV 关联。

接收侧保留已有缝，并补齐无法对齐的边界：

1. 发布起点；
2. `recv_wait` 进入、序号快照、实际阻塞开始、阻塞返回及返回原因；
3. `recv` 返回；
4. lease release；
5. `process_received_buffer` 前后；
6. 入队前后；
7. getter 出队前后和 API 返回；
8. `assist_route` 获取/释放、worker wait-set enable/disable 和 route recheck。

通知缝记录 `recv_local_signal::notify` 的 sequence 变化、等待者计数和是否发出 futex wake；wait-set 记录全扫、登记、唤醒返回、ready 数和中断原因。缝本身不能持有 lease 阻塞，也不能改变生产控制流。

### 4.2 调度事件能力

进入 D05/D06/D08 前，必须由提权采集器或等价的原始事件源提供当前 A/B 的：

- `sched_waking`、`sched_wakeup`、`sched_switch`；
- `sched_migrate_task`；
- `power/cpu_idle` 或等价 idle/IPI 证据；
- 与上述事件同域的用户缝时间和 TID/CPU 映射。

采集器必须保存命令、权限、内核版本、事件格式、丢事件计数和原始文件哈希。普通用户 `perf` 被拒绝时，流程停在 D04；不得伪造空事件、从旧批次复制调用栈，或把 `perf_event_paranoid` 临时改动当作产品修复。

## 5. 三条独立修复工作流

### W1：1 SUB / 4KiB p50——等待、唤醒和提交后交接

#### 目标与假设

目标是解释端到端 p50 的主要变化为何落在发布起点到 `recv_return`，并区分以下三个假设：

1. 共享 worker 的 wait-set/本地 signal 登记、复查或 futex 返回存在重复交接；
2. 发布提交后 route sequence、通知和 worker requeue 的状态传播放大了等待；
3. 应用端 `recv_return` 之后的队列和 getter 并非主因，修改 getter 不能带来稳定收益。

#### 单因素执行

1. 先完成同缝 A/B、D04 调度链采集，按 sequence 对齐 `publish→notify→wakeup→scheduled→recv_return`。
2. 在同一二进制、同一负载下分别固定 `assist=0/1`、worker 路径/`DZIPC_SHM_RECV_COMPAT=1`、相同 worker 数；每次只改变一个因素。
3. 对 `recv_local_signal` 做通知前后的序号和 waiter 计数审计；对 `recv_wait_set` 做登记到返回的一次性生命周期审计，检查是否出现无效 token、重复 enable、无 ready 的异常唤醒或多余 interrupt。
4. 用短诊断窗口验证每条假设，再以两个完整平衡块复测；L1 绝对值仅作定位，L0 才用于候选的端到端判断。

#### 候选改动边界

只有在事件证明重复通知、重复复查或无效交接后，才允许设计合并通知、减少一次状态复查或调整交接位置的候选。任何改动必须保留“先观察序号、再登记/等待、返回后重新扫描”的丢通知保护，并保持 `sub_recv_attempt` 中异常释放、空读保活 lease、非空读在分流前释放的顺序。若后续段仍只有微秒级 p99，暂不修改 `process_received_buffer`、队列或 getter。

#### 成功和撤回

成功条件是：`publish→recv_return` 的 p50 按假设方向下降，调度链能解释下降位置，L0 端到端 p50 同向，三场景没有一致退化；所有消息完整、无重复/损坏/丢失，I1–I5 和重建/析构测试通过。出现任何丢唤醒、重复消费、lease 不归零、等待永久阻塞、回退增加或 32 SUB p99 同向恶化，立即撤回候选。

### W2：1 SUB / 1MiB p50——复制、页触达和缓冲条件

#### 目标与假设

目标是把发布侧端到端成本拆为序列化、loan/缓冲获取、DZFlat 写入或 TLV `memcpy`、共享段提交和接收返回，验证核型、缓存、页触达和缓冲复用是否造成尾部或中位数变化。当前 B 的整体改善意味着复制假设尚未得到当前批次的支持。

#### 单因素执行

1. 先固定发布者、订阅者、网关 CPU 集合和核型，固定消息内容、复制次数、缓冲大小、预热和发送频率；记录 page fault、CPU 时间、墙钟和每个发布 sequence。
2. 在同一生产库中分别测量标准 `memcpy`、现有 DZFlat `loan→dzflat_write→publish_loan`、预构造段路径和 TLV 回退路径；只改变一个路径开关。
3. 再做发布 CPU 0/16 等核型对照，保持其他进程和缓冲条件相同；每一条件至少两个完整平衡块，ABBA/BAAB 交错执行。
4. 只有获得 PMU/页触达能力后，才把 cache miss、内存带宽、fault 或迁移与复制时段关联；无该能力时只报告可见的墙钟/线程 CPU 事实。

#### 候选改动边界

优先级为缓冲复用和页预触达的证据验证，其次才考虑改变复制实现。历史 AVX2 预取和 16KiB 分块没有稳定收益，暂不重新接入生产；标准 `memcpy` 保持为基线。不得把减少一次必要复制、绕过校验或改变借样配额作为优化。

#### 成功和撤回

候选必须同时降低发布复制相关分段和端到端 1MiB p50，且 4KiB p50、32 SUB p99 不出现一致退化；大载荷内容、sequence 和接收完整性不变。若发布段改善而端到端不改善、收益只在某一核型、或出现借样泄漏/池耗尽/回退增加，保留实验负面证据并撤回。

### W3：32 SUB / 64B p99——广播顺序和目标线程激活长尾

#### 目标与假设

当前 B 的 L1 后续段中 `入队→出队` p99 为 `25.489 µs`，但尚未能拆出内核等待。待验证假设为：

1. 单个发布对 32 个订阅者的通知遍历形成可测量的顺序累计成本；
2. 目标线程被唤醒到真正运行之间存在少数长尾，且它与广播顺序或目标 CPU idle/IPI 状态相关；
3. 固定 worker route 归属、预算轮和协作 getter 的交接使长尾被放大。

#### 单因素执行

1. 每个发布 sequence 记录 32 个订阅者的通知发起、通知返回、waking、wakeup、scheduled、`recv_return`，计算首位/末位偏差和每个订阅者的最大唤醒间隔。
2. 用调度事件把 `notify→waking`、`waking→wakeup`、`wakeup→scheduled`、`scheduled→recv_return` 分开；将 idle/IPI、迁移、worker 预算轮和 route worker id 一并记录。
3. 依次对照 assist 开关、兼容线程、worker 数/route 分布和通知顺序；一次只改变一项，不改变订阅数量、发送频率和消息大小。
4. 32 个接收条目按发布 sequence 聚类统计，不能把同一发布的 32 条样本当作 32 个独立发布样本。

#### 候选改动边界

只有在证据确认重复通知或可安全合并同一 sequence 的通知时，才设计通知批处理/去重候选；去重必须证明每个活跃订阅者仍至少收到一次可观察的 sequence 变化，并覆盖停止、重建和迟到订阅者。若证据显示主要是目标 CPU 激活长尾，先评估 route 分布或调度交接，不能用忙等、提升优先级或丢弃慢订阅者解决。不得破坏发布顺序、publisher identity 或单消费者契约。

#### 成功和撤回

成功条件是广播累计时间和 `waking→scheduled` 长尾均按假设方向改善，32 SUB 的总体 p99 同向下降，最慢订阅者没有新的系统性偏置，且 1 SUB 两种载荷不退化。发现任何订阅者缺 sequence、通知去重无法覆盖重建边界、route owner 冲突、worker 饥饿或尾部转移，立即撤回。

## 6. 分阶段执行顺序

### P0：冻结版本、产物和环境

- 保存 A/B/C（当前 C 为空）的完整 SHA、编译命令、编译器、实际加载库、头文件和消息生成器哈希。
- 冻结 D02/D03 场景、频率、预热、正式时长、窗口顺序和随机种子；继续记录环境快照及 `/home/zwc` 活动，不操作 JAX。
- 建立每个候选的 manifest：源码 SHA、二进制 SHA、运行器 SHA、环境 JSON、参数、原始 CSV/trace 和统计脚本哈希。
- P0 退出条件：可重复构建、六个冒烟场景完整、解析器和完整性检查通过。

### P1：恢复 D04 调度链能力

- 由提权采集器或等价事件源完成能力预检；先以无业务短测验证事件格式、时间域、TID/CPU 映射和无丢事件。
- 在 A/B 各执行 1 SUB/4KiB、32 SUB/64B 的最小调度链窗口，确认 trace 能覆盖通知到 `recv_return`。
- P1 未通过时只做源码和应用缝准备，不进入生产修复候选。

### P2：关闭 W1 的定位环

- 先跑 A/B 同缝，再执行 assist、兼容线程和 worker 配置的单因素对照。
- 根据调度链选择至多一个生产因素形成候选 C1；完成两个完整平衡块和功能/lifecycle smoke。

### P3：关闭 W2 的复制环

- 在 C1 或基线中进行同缓冲、同核型、同复制次数的 D07；标准 `memcpy` 保留对照。
- 只有复制段和端到端指标同时改善，才形成 C2；若 W1 候选存在，C2 必须以 C1 为父版本并独立记录差异。

### P4：关闭 W3 的广播环

- 完成每 sequence 的 32 路通知和调度链配对，先判断累计通知还是线程激活长尾。
- 只在有明确机制证据时形成 C3；不能用短窗 p99 改善直接关闭该问题。

### P5：候选逐一对照

- 每个候选只包含一个生产因素；保持上一候选已验证的改动不变并记录父 SHA。
- 每次按相同窗口顺序执行至少两个完整平衡块；对目标段和端到端 p50/p95/p99 同时检查。
- 任何候选未达门槛都保留负面结果和回滚 SHA，不通过反复挑选窗口来“修正”统计。

### P6：回归和关闭前检查

- 功能：发布/接收 sequence、顺序、重复、损坏、丢失、DZFlat id/schema、TLV 回退、无订阅者和混合 peer。
- 生命周期：RouteSession I1–I5、add/remove/rebuild、stop/wake、取消、析构、fork 子进程、池容量、invalid token、fallback reason 和 assist=0。
- 工具：OFF 构建和运行、ASan/UBSan（含泄漏检查）、已有 SHM/worker/兼容线程测试。
- 诊断开销：关闭 seam 后重新编译，确认候选收益不是观测缝引入或消除的结果。

### P7：正式 D12

- 先生成 C 的完整 SHA 和实际加载库审计；没有 C 不得开始。
- A/C 各执行 3 轮 × 9 场景，合计 54 个窗口；每轮 9 个 A/C 配对均按逐轮门槛检查，共 27 个配对门槛。
- 正式运行关闭所有诊断缝和实验开关，使用规定的 CPU/环境门控；任何 JAX 或未声明环境活动使窗口按既定规则标记，不私自纳入。

## 7. 数据、统计和候选准入契约

1. 每个窗口保留发布原始 CSV、所有 `sub*.csv`、环境快照、日志、trace、完整性报告和哈希；缺失、重复、损坏、时间回退、sequence 不连续和事件溢出分别分类。
2. 总体 p50/p95/p99 从窗口内全部订阅者原始样本合并后计算；不平均各订阅者分位数。32 SUB 还要按发布 sequence 聚类，报告最慢订阅者和首末唤醒偏差。
3. L1 分段分位数只描述各段分布，不能相加成端到端分位数；A 缺失的 seam 保持“未采样”，不能填零。
4. 每个候选至少两个完整平衡块方向一致，目标分段、端到端指标和其他两个失败场景都必须按预测方向检查。短窗、三轮中位数、绑核诊断和旧批次栈数据都不能替代正式门槛。
5. 正式允许差值采用既定规则：候选相对基线的 p50/p99 差值必须不超过 `max(基线 × 10%, 5 µs)`；任何完整性或生命周期失败直接不通过。

## 8. 生命周期和代码审查清单

每个候选合入前逐项审查：

- `sub_recv_attempt` 的每个成功 `acquire_receive` 都有且仅有一次 `release_receive`，包括异常出口；非空数据在分流前释放，空读协作等待保留 token lease。
- `RouteSession` 的停止/重建先拒绝新 lease，再唤醒，在途 recv 归零后才 release 旧 route；generation 不用于丢弃已弹出的 buffer。
- `RecvWorker::run_budget` 只在一次完整 `recv_once` 返回后检查预算；序号变化、空读、requeue、assist 和 remove_route 的边界不被改变。
- wait-set 的 token 代次、interrupt、`futex_waitv` 返回错误和 unavailable 路径均有可机读证据；能力不足时显式回退并保留原因计数。
- `process_received_buffer` 的 DZFlat/TLV 校验、adopt 配额、队列入队和 wakeup artifact 处理不被绕过。
- 发布侧的 `publish_best_effort`、`publish_blocking`、loan/discard/publish_loan、序列和 publisher registry 生命周期保持原语义。

## 9. 回滚、停止和交付物

### 9.1 立即停止条件

出现消息丢失、重复、损坏、sequence 缺口、跨代丢包、死锁/永久等待、lease 泄漏、双消费者、worker 静默退出、错误回退、ASan/UBSan 报告或正式数据无法追溯时，停止该候选，不继续扩大窗口。

没有可用 D04 原始调度事件、没有完整 C 产物、环境活动未记录、统计脚本无法复算或窗口完整性不满足时，停止进入下一阶段。

### 9.2 回滚方式

每个候选只允许通过父 SHA、构建产物哈希和运行时开关回滚；回滚后重新跑最小功能、生命周期和三个目标场景冒烟。不得通过改变阈值、删除失败窗口或保留半程接收线程来回滚。

### 9.3 交付物

- 本方案的执行日志和阶段状态；
- D04 能力报告及 A/B 原始调度链；
- 每个 W1/W2/W3 候选的补丁、设计记录、manifest、负面证据和回滚 SHA；
- 统一 seam 事件 schema、采集器、统计脚本和可复算的窗口汇总；
- 功能、生命周期、OFF、ASan/UBSan 回归报告；
- C 的二进制/库/头文件/消息生成器哈希和正式 D12 54 窗审计。

在 P7 的 54 窗全部满足逐轮门槛、完整性和可追溯性之前，三个历史未通过项的状态仍应标记为“待验证”，不能以当前 D02-006 的短窗改善作为修复结论。
