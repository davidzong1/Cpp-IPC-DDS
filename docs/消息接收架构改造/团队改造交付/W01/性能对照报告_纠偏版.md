# 事件驱动接收线程池 vs Eclipse iceoryx —— 性能对比分析（W01 纠偏版）

> **文档状态**：本文是《事件驱动线程池_vs_iceoryx_性能对比.md》的 **W01 纠偏修订版**。
> **初版**：2026-09-28，sha256 `a8e3d8b30ac6d7e4c88a222a2951f9e17cbc6c317552c44dd194f5bbd711cb95`。
> **修订前快照（原文保留，不覆盖）**：`团队改造交付/W01/历史版本/事件驱动线程池_vs_iceoryx_性能对比_20260928_W01修订前快照.md`（同 sha256）。
> **修订依据**：`团队改造方案_性能证据闭环与SHM规模化.md` §4 W01；队长派单 t2。
> **逐条纠正对照**：`团队改造交付/W01/纠正说明_初版对照.md`；**证据映射**：`团队改造交付/W01/证据索引.md`；**未决事实**：`团队改造交付/W01/未决事实清单.md`。
>
> **性质**：只读分析 + 本机复测 + 文档纠偏。⛔ 本轮未修改任何产品代码、测试与既有报告，未覆盖任何原始样本。
>
> **阅读约定 —— 每条结论都带证据等级标签：**
>
> | 标签 | 含义 |
> |---|---|
> | 【观测-本机】 | 本机原始结果文件存在，路径可回溯（见证据索引） |
> | 【代码】 | 本仓源码调用点/常量，附文件:行 |
> | 【来源】 | 外部结论，已固定版本/提交并有本地存证（见 `外部来源存证/`） |
> | 【历史-不可回溯】 | 来自初版正文，**未找到原始输出文件**，只能作历史观测，不得作为通过判据 |
> | 【推测】 | 未直接测量或未核验成因的推断 |
> | 【待验证】 | 明确未测/未闭环项，见未决事实清单 |
>
> **架构图**：`docs/消息接收架构改造/事件驱动线程池架构图.svg`（本文引用其分层编号 ①–⑥）。
> **复测环境**：Intel i9-14900KF / 32 逻辑核 / 24 物理核 / 内核 6.8.0-138 / Ubuntu 22.04.5 / GCC 11.4 / `-O2`；`cpu_governor=powersave`、无 CPU 隔离、测试窗口内 load average ≈ 4–10。
> **关键前提**：**本机 iceoryx 是"CycloneDDS + iceoryx"的组合**（见 §1.2），不是裸 iceoryx API。全文区分「官方裸 iceoryx 数据」与「本机 CycloneDDS+iceoryx 组合数据」。

---

## 0. 结论摘要（先读这段）

> **本节的每一条都相对初版做了收敛。改动原因逐条列在 `纠正说明_初版对照.md`。**

1. **两者优化的是不同的轴，不能用一个数字判高下。**【推测】
   事件驱动线程池解决的是**规模轴**（线程数/空闲唤醒/fd 不随话题数线性增长），iceoryx 的强项是**单条延迟轴**（零拷贝、恒定延迟与载荷无关）。
   ⚠️ **初版在此处写"规模轴已达到工程可用（1000 订阅 33 线程恒定）"，已删除。** 正确表述见第 5 条：本波只观测到"**socket 模块、同一 topic** 下线程数不随对象数增长"，**没有**证明 1000 个有效订阅可用。

2. **本机同机对照（64 B 与 1 MiB）—— 数字保留，但必须连口径一起读：**【观测-本机】

   | payload | 速率（实际） | dzIPC SHM pub/sub p50（跨进程） | CycloneDDS+iceoryx p50（跨进程） |
   |---|---|---|---|
   | 64 B | 1000.5 Hz | **9.31 µs**（`tmp/perf_match_64/results.csv`） | 12.0 µs（`tmp/iox_probe/sub_l64.log`） |
   | 1 MiB | 500.6 / 500 Hz | 298.47 µs（`tmp/perf_match_1m/results.csv`） | **157.7 µs**（`tmp/iox_probe/sub_l1m.log`） |

   - 两侧**都是跨进程**、发布端打时间戳、订阅端作差、单向 —— 这一点本次复核后**成立**。
   - ⚠️ **但两侧的"等待方式、计时终点、消费者工作量"不对称**（1 MiB 档尤其严重：dzIPC 侧计时终点在 `try_get_clone` 完整物化之后，iceoryx 侧计时终点在 DDS 回调入口、不遍历载荷）。**因此 298.47 vs 157.7 只能称"两条不同工作负载的端到端读数"，不能称"wire 快 1.9 倍"**。详见 §3.2。
   - ⚠️ **初版把 1 MiB 的 298.5 µs 当作"本仓 SHM 大消息成本"、把差距归因于 wire 拷贝，属过度归因。** 消费侧工作量差异足以解释其中相当一部分。

3. **与 iceoryx 官方极限的差距是量级参照，且必须带来源与口径。**【来源】
   - iceoryx 1.x 官方 iceperf：**0.58–0.73 µs，1 kB–4 MB 平坦**（[iceperf README @ v2.95.8](https://github.com/eclipse-iceoryx/iceoryx/blob/v2.95.8/iceoryx_examples/iceperf/README.md)）。
     ✅ **本次核验解决了初版的疑问**：其"Average Latency"**是单向**——`iceoryx_examples/iceperf/base.cpp:49-52` 明确 `TRANSMISSIONS_PER_ROUNDTRIP{2U}`，即 `duration / (往返次数 × 2)`。
   - iceoryx2 官方原始数据：单向 **92–99 ns**，64 B–4 MB 平坦（[原始数据 @ v0.10.0](https://github.com/eclipse-iceoryx/iceoryx2/blob/v0.10.0/internal/plots/benchmark_mechanism_comparison_i7_13700h.dat)）。
   - 本仓 64 B 单向 8–12 µs（视轮次，见 §3.3），相差 **约 12–100×** —— 该量级结论保留。
   - ⚠️ **但"线程/唤醒层不是主因、每消息 ≥1 次搬运才是主因"这一因果判断，现有结果不支持。** 现有实验没有隔离编解码、分配、等待、调度、排队各自成本（§6）。初版把它写成已归因，已收敛为【推测】。

4. **本仓自己的"延迟杠杆"已经存在，且与线程模型无关 —— 但初版对它的标注与结论都必须改。**【代码】【历史-不可回溯】
   - **A/B 标注纠正**：`build/bin/ipc_benchmark --compare` 的 DZFlat 档**不是** B 级 loan-and-build，**是 A 路径**。
     `test/ipc_benchmark.cpp:322` 传的是**已构造好的 `TestMsg`** 去 `publish_blocking()`；`src/dzIPC/shm_pub_sub_ipc.cc:460-486` 的 `try_publish_dzflat()` 内部 `loan(need)` → `msg->dzflat_write(lo.data, …)` → `publish_loan(lo, tm)`。
     即：**容器 → chunk 仍有 1 次拷贝**（`dzflat_write` 逐字段写进借来的 chunk）。B 级要求**应用在借用内存中就地把构造**（`include/dzIPC/common/loaned_message.h` 头部契约），`--compare` 不提供该证据。
   - **不可直接比较**：该档是**同进程**（`bench_shm` 在本进程内起订阅线程）、订阅端**忙等**、且计时终点在**视图读取**之后；对比对象 157.7 µs 是**跨进程** + DDS 回调。必须先统一进程模型/等待方式/计时范围/消费工作，才可对照（§7 P1）。
   - **可回溯性**：初版 §5.1 的 TLV 481.5 µs / DZFlat 33.6 µs（14.32×）**在本仓未找到原始输出文件**（已检索 `tmp/`、`perf_results/`、`test/perf/out/`），标为【历史-不可回溯】，**不得**再进入胜负表，须由 W02/W11 重测后才能引用。见未决事实 UF-01。
   - **删除的结论**：初版"33.6 µs 已优于本机 CycloneDDS+iceoryx 的 157.7 µs，把 DZFlat 变成大消息默认路径就能反超本机 iceoryx"——**删除**。理由：口径不可比 + 数字不可回溯。

5. **线程池买到的是规模与空闲成本 —— 一条站得住、一条必须重测。**【观测-本机】【代码】
   - **规模（socket 模块，同 topic）**：worker 路径 `threads_max ≡ 33`（scale 1→1000 不增长），静默后回落到 2（`test/perf/out/20260927_t6_scale/worker_{1,100,477,1000}.log`）。
     ⚠️ **但这不等于"1000 订阅可用"**：探针（`test/perf/out/20260927_t6_probes/t6scale.cpp:91`）给**所有对象用同一个 topic `t6scale`**；scale=1000 时日志出现 **2 次 `register_entry 失败: 表满`**，且 **`sent=0`、`received_sub0=0`**（`worker_1000.log`）。即：**"33 线程"是在一片注册失败、零实际收发的对象集合上测到的**。见 §4.1、UF-02。
   - **空闲 CPU**：477 路静默 2 s，基线 **1.124 核 → worker 0.0050 核**（≈225×）。两侧同用 `/proc/self/stat` 的 utime+stime，本轮实测**该字段是线程组累计值**（§4.2），故**这条比较口径一致，结论保留**。
   - **空闲上下文切换**：初版"19325 → 10（225×）"**不可用**。原因不是"都是错的"，而是**两侧口径不同**：基线探针自报 `ctx_scope=sum(/proc/self/task/<tid>/status) 全线程聚合`（`test/perf/out/20260927_t6_boundary/base_477.log:3`），而 worker 探针读 `/proc/self/status`（**主线程自身**，实测见 §4.2）。见 UF-03。

6. **可比性缺口必须承认（完整清单见 §6 + 未决事实清单）。** 摘要级缺口：iceoryx 侧未测满速吞吐与 1000 订阅规模；1000 路数据来自 **socket** 模块（SHM 订阅侧尚未接入接收池，§5.5）；两套 Harness 不可直接相减；本机 iceoryx 是否走零拷贝路径**无计数口可验证**；1 MiB 档两侧消费者工作量不对称（§3.2）。

> **一句话（修订后）**：这张架构图里的线程池，是"把 O(话题数) 的开销压成 O(1)"的**方向正确**的工程，它对本机可证的收益是**空闲 CPU（同口径，≈225×）**；但"1000 订阅可用"与"上下文切换 225×"两项**证据不足，不能引用**。它对标 iceoryx 的是**可扩展性与空闲开销**，而不是单条消息延迟。要在延迟上追平 iceoryx，要动的是**数据面拷贝次数与 wire**（本仓已有 B 能力，但**尚未有 B 路径的性能证据**），不是 worker 数量。

---

## 1. 对比对象与前提

### 1.1 两个系统各是什么

| | 本仓（dzIPC + 事件驱动接收池） | iceoryx |
|---|---|---|
| 定位 | 机器人中间件传输层：pub/sub + ser/cli，SHM 与 socket 双传输，DZFlat/TLV 双 wire | 同机零拷贝 IPC 中间件（ROS 2 的 SHM 后端之一） |
| 发现/控制面 | 共享段控制面（`control_plane`）+ 阶段 1 的进程级 `ShmControlScheduler`（10 ms/50 ms/2 s）。⚠️ 该调度器**全仓零接入点**【代码】，见 §5.5 | iceoryx 1.x：RouDi 守护进程；iceoryx2：无守护进程，服务发现靠文件系统 + 文件锁监控【来源见存证】 |
| 等待原语 | 共享内存 sequence 字 + `futex_waitv`（Linux，最多 127 路/worker，`src/libipc/recv_wait_set.cpp:23`）【代码】 | 1.x：共享内存里的 POSIX `sem_t`（每 Listener/WaitSet 一个）；2.x：**epoll** + 进程间 unix domain datagram socket 门铃【来源】 |
| 收包线程 | 进程级固定 N 个 worker（N=CPU，上限 128），route→worker 固定归属，空闲退出归还 OS | 1.x：订阅者 **0 线程**（等待由用户线程/WaitSet 承担）；**Listener 另起 1 条后台线程**（见 §2）；2.x：`Subscriber` 无事件接口，事件须由**用户线程**调 `Listener::try_wait()/blocking_wait()`，**Listener 自身不起线程** |
| 载荷约束 | 任意可序列化对象；DZFlat 平坦布局可选；TLV 兼容 | 1.x/2.x 均要求共享内存可放置（无堆/指针/虚函数；2.x 为 `ZeroCopySend`） |

> **修订说明（机制描述）**：初版写"iceoryx2 事件驱动需另配 Notifier/Listener（**Listener 也是独立线程**）"。**该句错误，已改**：iceoryx 1.x 的 `Listener` 确有后台线程（`doc/design/listener.md:9,13,102`），但 **iceoryx2 的 `Listener` 由用户线程轮询/阻塞等待**（`iceoryx2/src/port/listener.rs:275-283,304-310`，`iceoryx2-cal/src/event/` 全目录无线程创建）。见 §2 与 `证据索引.md` E-07。

### 1.2 本机 iceoryx 部署证据（为什么说本机对照是"CycloneDDS + iceoryx"）

1. `/etc/iceoryx/roudi_config.toml`：mempool `128B×10000, 1KB×5000, 16KB×1000, 128KB×200, 1MB×50`；段 `zwc` 实际预留 149,264,720 B，管理段 66,761,736 B（`tmp/iox_probe/roudi.log`）。【观测-本机】
2. `/opt/lejurobot/iceoryx/bin/iox-roudi`，`--version` = **RouDi version 2.0.5**（Build date 2026-06-16）。⚠️ 是 1.x 产品线版本号，**不是 iceoryx2**。【观测-本机】
3. `lejulab_platform` 内 CycloneDDS 0.10.2 的 `libddsc.so.0.10.2` 导出了 iceoryx 集成符号：`iceoryx_header_from_chunk`、`ddsi_serdata_iox_size`、`free_iox_chunk`、`iox_cfg_max_chunks_held_per_subscriber_simultaneously`。【观测-本机】
4. 配置 `src/leju_launch/config/cyclonedds_shm.xml`：`<SharedMemory><Enable>true</Enable>`，接口 `lo`。【观测-本机】
5. 实测：`iox-roudi` 常驻后 `latency_publisher/latency_subscriber` 正常跑通（RouDi 未运行时报 `IPC_INTERFACE__REG_ROUDI_NOT_AVAILABLE` 并 coredump）。【观测-本机】
6. 本机 iceoryx 侧订阅端源码**可读**：`/home/zwc/branch/lejulab_platform/src/lejusdk/perf_tests/src/latency_subscriber.cpp`（DDS 回调式消费），发布端 `latency_publisher.cpp`。【代码】

⚠️ **未验证项**：CycloneDDS 是否**真的**进入 iceoryx 零拷贝路径**没有计数口**。`Enable=true` + RouDi 运行 + shm 段被创建只证明依赖存在，不能等价于"每条消息都零拷贝"。

### 1.3 本机对照的运行条件（可复现）

```bash
# RouDi（本机 2.0.5）
setsid nohup /opt/lejurobot/iceoryx/bin/iox-roudi > tmp/iox_probe/roudi.log 2>&1 < /dev/null &

# iceoryx 侧：跨进程、单向时延（发布端把 ns 时间戳打进消息头）
cd /home/zwc/branch/lejulab_platform
export CYCLONEDDS_URI=file://$PWD/src/leju_launch/config/cyclonedds_shm.xml
export LD_LIBRARY_PATH=$PWD/devel/lib:$PWD/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2/lib:$PWD/src/lejusdk/3rd_party/x86_64/cyclonedds-cxx-0.10.2/lib
setsid nohup devel/lib/lejusdk-perf-tests/latency_subscriber --topic /perf/l64 --count 2000 > tmp/iox_probe/sub_l64.log 2>&1 &
devel/lib/lejusdk-perf-tests/latency_publisher --topic /perf/l64 --rate 1000 --size 64 --count 2000 --warmup 100
# 1 MiB 档：--topic /perf/l1m --rate 500 --size 1048576 --count 800

# 本仓侧：同负载、同速率（跨进程，fork+exec 独立收发进程）
LD_LIBRARY_PATH=build/lib ./build/bin/dzipc_perf_benchmark \
  --cases=pubsub_shm --payloads=64 --duration=2 --warmup=0.3 --lat-rate=1000 --skip-tput --out=tmp/perf_match_64
LD_LIBRARY_PATH=build/lib ./build/bin/dzipc_perf_benchmark \
  --cases=pubsub_shm --payloads=1048576 --duration=1.6 --warmup=0.3 --lat-rate=500 --skip-tput --out=tmp/perf_match_1m
```

⚠️ **日志字段陷阱（本轮新发现，已在证据索引登记）**：`latency_subscriber` 打印的 `Publish Rate:` / `Size:` 是**它自己的命令行默认值**，不是实测值——`sub_l1m.log` 头部写 `Rate: 1000 Hz`、`Size: 64 bytes`，而该轮发布端实际是 `--rate 500 --size 1048576`（`pub_l1m.log`），结果块里打印的 `Message Size: 1048576 bytes` 才是真值。引用时**必须以发布端日志或结果块为准**。

---

## 2. 架构分层对照

对照本仓架构图 ①–⑥ 与 iceoryx 的对应构件。

| 维度 | 本仓（架构图编号） | iceoryx 1.x | iceoryx 2.x |
|---|---|---|---|
| **等待机制**（④） | 共享段 sequence 原子字 + `futex_waitv`；`kMaxRoutes=127`/worker（`src/libipc/recv_wait_set.cpp:23`）【代码】 | 共享内存 `ConditionVariableData`，底层 POSIX `sem_t`；**每 Listener/WaitSet 一个信号量**。⚠️ `ConditionListener::wait()` 本身是**调用者线程的阻塞调用**（`condition_listener.cpp:58-68,81-107`），不自己起线程【来源】 | Linux：**epoll**；`Epoll::max_wait_events() = 512`，用作**单次 `epoll_wait` 返回事件数组的长度**（`iceoryx2-bb/linux/src/epoll.rs:449-450,584-591`）【来源】 |
| **唤醒粒度** | **按 route 定位**：`consume_ready()` 返回就绪 token，worker 精确 `recv_once()` 该 route | 条件变量"有事件"，订阅者自行 `take()` 查所有端口 | reactor 返回就绪 fd；`Subscriber` 本身无事件接口 |
| **线程模型**（③） | 进程级池：N=CPU（上限 128）固定 worker，route→worker 固定归属，**空闲退出归还 OS** | 订阅者 **0 线程**；**WaitSet 在用户线程**；**`Listener` 自带 1 条后台线程**执行事件回调（`doc/design/listener.md:9,13,102`：`m_thread : std::thread`）【来源】 | **无 per-subscriber 线程**；`Listener`/`Notifier` 是**端口**，由**用户线程**调 `try_wait()`/`blocking_wait()`（`listener.rs:275-310`），**不自带线程**【来源】 |
| **fd / 句柄成本** | SHM 路径 **0 fd/route**（等的是共享段字，不是 fd）；socket 路径 1 fd/route | 订阅者 0 fd、0 线程；`sem_t` 不占 fd【来源-部分】 | **每 Listener 1 个接收 fd + 每 Notifier 1 个发送 fd + 每 shm 对象 1 句柄**【来源】；官方 FAQ 有 `Running Out of File Descriptors` 章节 |
| **队列/背压** | `view_queue_`/`msg_queue_` 有界队列 + evict 回调；chunk 池**每尺寸档 40 块**（`include/libipc/def.h:49`，全进程共享），环 256 槽【代码】 | 订阅者队列=可持有 chunk 上限（默认 **256**），溢出"安全丢最旧" | buffer/history 可配（默认 buffer 2、history 0） |
| **内存配置** | chunk 池按 1 KiB 台阶分档、容量**编译期固定**（40/档），段名编码容量 | **发行时必须预配置** mempool（本机 `1MB×50` 等）；池耗尽即丢包 | publisher 预分配 data segment |
| **零拷贝** | A 级 1 拷贝（用户容器→chunk）；**B 级 `loan`+就地构造 = 0 拷贝**（`include/dzIPC/common/loaned_message.h`）【代码】。⚠️ **本仓尚无 B 路径的性能证据**（见 UF-04） | loan/publish 全程 0 拷贝 | `loan`/`loan_slice` 0 拷贝 |
| **类型约束** | 无（任意可序列化对象；DZFlat 为可选优化，TLV 兜底） | 须共享内存可放置：无堆/指针/虚函数、可重定位；不要求 IDL | 须 `ZeroCopySend`（`repr(C)`、无堆/指针、`'static`） |
| **单条上限** | 无硬上限；>chunk 档位或池耗尽时退化 64 B 分片（`src/libipc/ipc.cpp:770-798` 区域）——**是悬崖不是优雅降级** | 单条=所配 chunk；本机最大 1 MiB×50，官方示例到 4 MiB×10 | publisher 预分配段 |
| **编译期规模上限** | `kMaxRoutes=127`/worker → 超限返回 `wait_set_full`，模块**显式回退兼容线程**（`src/dzIPC/threepools/recv_worker.cc:611,620,624`）【代码】 | `IOX_MAX_SUBSCRIBERS=1024`、`MAX_NUMBER_OF_NOTIFIERS=256`、`MAX_NUMBER_OF_CONDITION_VARIABLES=1024` | 事件端口默认 16 listener/16 notifier，可配 |

**读法**：架构图 ③（固定分片收包池）+ ④（共享等待层）对标的是 iceoryx 的"等待与线程归属"这一层；两者在"唤醒精确到 route"这个能力上**同级**。真正的分水岭在**「一条消息要搬几次字节」**——但**本轮没有把这个分水岭量化出来**（§6）。

> ⚠️ **epoll 512 的正确读法（纠偏要点 6）**：512 是 `Epoll::max_wait_events()` 的返回值，被用作**一次 `epoll_wait` 调用可返回的最大事件数**（即那趟 `events[512]` 数组的长度，`epoll.rs:584-591`）；**不是**"最多只能注册 512 个对象"。注册对象总量的上限来自 **`/proc/sys/fs/epoll/max_user_watches`**（`epoll.rs:77`；另见 `epoll_ctl(2)`/`epoll(7)`）【来源】。
> 初版正文该句虽已写"单次 `epoll_wait` 上限 512 事件"（方向正确），但仍把 512 与"规模上限"并置在同一格里，且链接指向 `main` 分支（会漂移）。**本版：语序改为"单次返回事件数"，并把链接固定到 `v0.10.0`（commit `135d09dd8b29f321f1725920d434864c4e512378`）。**

---

## 3. 本机实测对照（同机、同负载）

### 3.1 读数表

| payload | 速率 | 系统 | n | p50 | p99 | mean | min | max | 原始文件 |
|---|---|---|---|---|---|---|---|---|---|
| 64 B | 1000 Hz | **dzIPC SHM**（跨进程） | 2001 | **9.31 µs** | 92.47 µs | 12.83 µs | 3.90 µs | 1127.27 µs | `tmp/perf_match_64/results.csv` |
| 64 B | 1000 Hz | **CycloneDDS+iceoryx**（跨进程） | 2000 | 12.0 µs | 105.0 µs | 17.0 µs | 2.9 µs | 240.3 µs | `tmp/iox_probe/sub_l64.log` |
| 1 MiB | 500 Hz | **dzIPC SHM**（跨进程） | 801 | 298.47 µs | 477.16 µs | 299.05 µs | 213.74 µs | 635.00 µs | `tmp/perf_match_1m/results.csv` |
| 1 MiB | 500 Hz | **CycloneDDS+iceoryx**（跨进程） | 800 | **157.7 µs** | 313.5 µs | 165.4 µs | 101.2 µs | 411.9 µs | `tmp/iox_probe/sub_l1m.log` |

> 初版把 n 写作 2000 / 800；**实际原始 CSV 的 `sent`/`recv`/`sample_count` 为 2001 / 801**（速率×时长 + 1）。此处按原始文件改正。

### 3.2 三条读数解读（修订：补上"消费者工作量不对称"）

1. **64 B：本仓略优（p50 1.3×、p99 1.1×）。** 小消息下两边都被"唤醒 + 调度"支配。此档位**不能用来说明架构优劣**（差值在噪声与 governor 抖动带内）。
2. **1 MiB：iceoryx 读数更小（1.9×），但这条比较的"消费者工作量"不对等，不能归因于 wire。**【代码】
   - **dzIPC 侧**：`test/dzipc_perf_benchmark.cpp:898` 走 `try_get_clone(rcv)` —— 对 `TestMsg` 做**整包物化/克隆**；1 MiB 档的 `TestMsg` 由 `make_payload()`（`test/ipc_benchmark.cpp:175-186` 同一构造）组成 ≈ **0.6 MiB `float64[]` + 0.2 MiB `int32[]` + 约 1638 条 64 字节 `std::string` + bool**，克隆意味着 **1 MiB 级 memcpy + 约 1600 次堆分配**。计时终点是 `test/dzipc_perf_benchmark.cpp:917` 的 `rx = now_ns()`，**在克隆完成之后**。
   - **iceoryx 侧**：`latency_subscriber.cpp:29-31` 的 `on_data_received()` 第一句就取 `recv_time`，**计时终点在 DDS 回调入口**；回调体只读 `header_sec/nanosec` 并压一个样本进统计向量，**完全不遍历载荷**。
   - ⇒ **298.47 vs 157.7 的差里，混着"整包克隆 + 1600 次分配"vs"仅读头部"的差异。** 把它称作"拷贝次数导致的 wire 差距"或"1.9× 的传输优势"都不成立。
   - 同理，**也不能反向宣称"DDS 零拷贝所以快"**：iceoryx 侧无法从外部区分"DDS 已零拷贝交付"与"DDS 在回调前已完整反序列化"（§1.2 未验证项）。
3. **p99 都不好，且本波 p99 明显受环境支配。** `powersave` 调频、未绑核、无 CPU 隔离、测试窗口 load average ≈ 4–10。**不要把本表的 p99 当作任何一方的架构能力。**
   - 同一份 dzIPC 64 B 用例在两轮之间 p50 稳定（9.31 / 11.71 µs）但 **p99 由 92.47 µs 跳到 446.90 µs**（`tmp/perf_match_64/` vs `tmp/perf_current_shm/`），足以说明 p99 目前是环境读数。

### 3.3 与官方极限数据的对照（不同 Harness，只作量级参照）

| 系统/口径 | 延迟 | 条件 | 来源（已固定版本） |
|---|---|---|---|
| iceoryx 1.x，官方 iceperf | **0.58–0.73 µs**，1 kB–4 MB 平坦 | 10 万次往返，Ubuntu 18.04 + Xeon E3-1505M v5；**已折半为单向**（`base.cpp:49-52`） | [iceperf README @ v2.95.8](https://github.com/eclipse-iceoryx/iceoryx/blob/v2.95.8/iceoryx_examples/iceperf/README.md) |
| iceoryx2，官方 benchmark | **92–99 ns** 单向，64 B–4 MB 平坦 | i7-13700H + Arch Linux 6.10，订阅端忙等 | [原始数据 @ v0.10.0](https://github.com/eclipse-iceoryx/iceoryx2/blob/v0.10.0/internal/plots/benchmark_mechanism_comparison_i7_13700h.dat) |
| iceoryx 1.x，同一份 iceoryx2 图 | 1.1 µs 单向 | 同上 | 同上 |
| **本仓 dzIPC SHM**（官方口径，3 s/档） | p50 **8.23 µs**（64 B）/ **8.21 µs**（1 KiB）；256 B 为 7.66 µs | 跨进程、1000 Hz、nodelet 关 | `perf_results/20260916_185113/report.md` §5.1 |
| **本仓 dzIPC SHM**（本波复测，2 s/档） | p50 **11.71 µs**（64 B）/ **12.63 µs**（1 KiB） | 跨进程、1000 Hz；p99 分别为 446.9 / 2596.5 µs | `tmp/perf_current_shm/results.csv` |
| **本波"配速"复测（与 iceoryx 同速率同窗口）** | p50 **9.31 µs**（64 B）/ **298.47 µs**（1 MiB） | 跨进程、1000 / 500 Hz | `tmp/perf_match_64/`、`tmp/perf_match_1m/` |
| **本机 CycloneDDS+iceoryx** | 12.0 µs（64 B）/ 157.7 µs（1 MiB） | 跨进程、1000 / 500 Hz | `tmp/iox_probe/` |

- 与**官方裸 iceoryx** 比：本仓小消息差 **约 12–100×**（8.23 µs vs 0.58 µs 量级；11.7 µs vs 0.092 µs 量级），大消息（1 MiB）差约 250×（253–298 µs vs 0.6 µs 量级）。**该量级结论保留**，因为官方数字是"与载荷无关"的零拷贝，而本仓随载荷线性增长。
  ⚠️ 初版把本仓小消息写成一个区间"7.7–12.6 µs"并同时引用**两个不同日期、不同口径**的数据集（`20260916` 与 `tmp/perf_current_shm`）。本版**拆成两行**分行列出，不再拼成区间。
- 与**本机 CycloneDDS+iceoryx 组合**比：差距收敛到 1–2×。这正是 §0 的关键判断：本仓对标的现实基线是"本机实际部署的 DDS+iceoryx 组合"，而非 iceoryx 论文数字。
- ✅ **初版遗留的疑问已解决**：iceperf README 虽未在正文写明方向，但其实现 `base.cpp:49-52` 明确按 `TRANSMISSIONS_PER_ROUNDTRIP{2U}` 折半 ⇒ **表格值是单向**。故初版那句"⚠️ 未明示，只作量级参照"改为"**已由源码确认为单向**"。
  ⚠️ 仍需保留的限制：该 ping-pong 口径把**对端(echo)的处理时间**也算进了往返，再除以 2 ⇒ 对"纯单向交付延迟"是**近似**，不是等价的单向测量。

---

## 4. 线程池买到的东西：规模与空闲成本

### 4.1 线程数与规模（本仓实测）

口径：`test/perf/out/20260927_t6_probes/t6scale.cpp`（按当前头文件重编），`socket_pub_sub` 路径，静默窗口 2 s，每 200 ms 采样 `/proc/self/task`（`t6scale.cpp:65`）。**该探针只覆盖 socket 模块**（日志首行 `mode=socket_pub_sub`）。

| scale | worker 路径 threads_max | worker threads_min | compat threads_max | 改造前基线 threads_max |
|---|---|---|---|---|
| 1 | **33** | 2 | 2 | 5 |
| 100 | **33** | 2 | 101 | 104 |
| 477 | **33** | 2 | 478 | 481 |
| 1000 | **33** | 2 | abort | abort（基线 1000 实测 abort） |

- worker 路径线程数**恒定 33**（32 worker + 主线程），规模 1→1000 完全不增长；静默后回落到 **2**（`threads_series=33,33,33,2,2,2,2,2,2,2`，空闲退出真实发生）。
- **⚠️ 该表的成立范围（纠偏要点 3）：**
  1. **所有对象共用同一个 topic `t6scale`**（`t6scale.cpp:91`）——**不是 1000 个独立话题**。方案 §6.1/§13.1 明确要求"1/100/1000 个**独立**话题"，本探针不满足。
  2. **scale=1000 时注册失败**：`worker_1000.log` 出现 2 次 `[info_pool] register_entry 失败: 表满…`（scale=100/477 时为 0 次）。
  3. **零实际收发**：`worker_1000.log` 中 **`sent=0`、`received_sub0=0`**（该轮未启用 `--send-msgs`；带发送的 `worker_100_pub.log` 也只有 `sent=20 / received_sub0=0`）。
  ⇒ **正确结论只能写到"线程数不随对象数增长（socket 同 topic）"**；"1000 订阅可用"**删除**。见 UF-02。
- 对照口径（iceoryx）：1.x 订阅者 **0 线程**；iceoryx2 `Subscriber` **0 线程**、事件端口由用户线程等。**在"线程数"这一项上 iceoryx 的模型本身更省**——它把等待交给用户线程，本仓则要保证"用户不写等待循环也能收包"。这是设计取舍，不是缺陷。

### 4.2 空闲成本（本仓实测）

静默 2 s 窗口：

| scale | 路径 | 探针/原始文件 | `ctx_vol` | 探针声明的 ctx 口径 | 空闲 CPU（核） |
|---|---|---|---|---|---|
| 1 | 基线库 | `test/perf/out/20260927_t6_boundary/base_1.log` | 4080 | **全线程聚合** | 0.025 |
| 100 | 基线库 | `.../base_100.log` | 7077 | **全线程聚合** | 0.173 |
| 477 | 基线库 | `.../base_477.log` | 19325 | **全线程聚合** | **1.124** |
| 477 | **worker** | `test/perf/out/20260927_t6_scale/worker_477.log` | **10** | **主线程自身**（`/proc/self/status`） | **0.0050** |
| 477 | compat | `.../compat_477.log` | 10 | **主线程自身** | 0.0749 |

> 基线行的 `ctx_scope` 由探针自己在日志第 3 行声明：`ctx_scope=sum(/proc/self/task/<tid>/status) 全线程聚合；窗口首尾各读一次`（`base_477.log:3`）。

- **空闲 CPU：1.124 核 → 0.0050 核（≈225×）—— 结论保留。**
  两侧都取 `/proc/self/stat` 的 `utime+stime`。本轮自建探针实测该字段**是线程组累计值**（C=205 ticks vs 全线程组 D=206 ticks，比值 0.995），故两侧**同口径**，可以相减。原始：`团队改造交付/W01/本机复现/csw_cpu_scope_probe.output.txt`。
- **空闲上下文切换：19325 → 10 —— 结论删除，改为"口径不一致，必须重测"。**（纠偏要点 4）
  - 基线侧 19325 是**全线程聚合**（探针自报；`build_baseline/bin/baseline_probe` 内含字符串 `sum(/proc/self/task/<tid>/status)` 与 `ctx_scope`，sha256 `ac098bd6059e0238c9771cfed187bad35079fb9937903e9dc91947c0656c2830`）。
  - worker 侧 10 是**主线程自身**（`t6scale.cpp:41` 读 `/proc/self/status`；二进制内**没有** `ctx_scope` 字符串，sha256 `2b3fafda224430c7213d250836ffe9e9a20b899b000cc091bfcdee5cbfd2d394`）。
  - 实证：8 条 `usleep(1 ms)` 保活线程 + 1 条忙转线程、2 s 窗口，`/proc/self/status` 自愿切换增量 **1**，而全线程组累计 **15171**（比值 15000+）。即主线程自身值**不能**代表"进程空闲上下文切换"。
  - ⇒ **"225×"这个倍数只对 CPU 成立；对 ctx switch 目前没有可用数字。** 重测方法见 §7 P0 与 UF-03。
- **归因**（保留但降级为【推测】）：改造前每个订阅者一条线程、每 50 ms 一次 `select()` 空轮询；改造后 worker 阻塞在等待层，超时不是错误、也不轮询。基线 ctx_vol 随规模从 4080 → 19325 增长，与"每路轮询"一致；但**本轮没有把"轮询间隔 × 路数"与实测值做定量对账**，故归因标记为【推测】。

### 4.3 fd 成本与规模上限（两者都有硬边界，位置不同）

| | 本仓 | iceoryx 1.x | iceoryx 2.x |
|---|---|---|---|
| SHM 路径 fd/route | **0**（等共享段字） | 0（`sem_t` 不占 fd） | 每 shm 对象 1 句柄 + event 端口 fd |
| socket 路径 fd/route | 1 | — | — |
| 单进程规模硬边界 | SHM：`kMaxRoutes=127`/worker → `wait_set_full` → **回退兼容线程**；socket：**fd 号 ≥ FD_SETSIZE(1024) 时 `select()` 触发 `__fdelt_chk` abort**（既有缺陷；实现位于 ~~`src/libipc/platform/posix/udp.h:349-353`~~ → **`src/libipc/platform/posix/udp.h:345-353`** 的 `FD_SET(server_fd)`）【代码】 | `IOX_MAX_SUBSCRIBERS=1024`（编译期） | epoll **单次 wait 最多返回 512 事件**；注册总量受 `max_user_watches` 限；官方 FAQ 明列 fd 耗尽章节 |

> **⛔ 行号更正（UF-17 / t54 收口，2026-09-28）**：本表原写「实现位于 `udp.h:349-353` 的 `FD_SET(server_fd)`」，该范围**装不下**其声称的 `FD_ZERO`/`FD_SET`。锚定提交 **HEAD `e800ccc`**（该文件 blob `19b6132687874c23a742fa7d0b216a1aaa58cef2`）逐行为：
> `:345 fd_set read_fds;` ｜ `:346 FD_ZERO(&read_fds);` ｜ `:347 FD_SET(server_fd, &read_fds);` ｜ `:349-351 timeval timeout; tv_sec / tv_usec` ｜ `:353 int ret = ::select(server_fd + 1, …);`
> ⇒ **覆盖三者应为 `:345-353`**（原 `:349-353` 只覆盖 `timeval` + `::select`）。
> **依据链**：架构负责人补出的 **W01-F3**（t25 自查漏检后补正）+ 文档负责人独立复核（`git show e800ccc:src/libipc/platform/posix/udp.h | sed -n '344,354p' | nl -ba -v344`）⇒ **两人结论一致**。
> ⛔ **不得**按「另一套行号口径」理解：两提交该文件为**同一 blob**，「`udp.h` 多套计数口径并存」的推论已由**全历史扫描 10 个提交**证伪（`:346=FD_SET` 提交数 **0**、`:347=FD_SET` 提交数 **1**）⇒ 裁定为 **off-by-one 计数错误**。
> ⚠️ **锚定 HEAD `e800ccc`**：W07 已把该路径改为 `poll()` ⇒ 工作区行号属**预期漂移**，复核本行请在 `e800ccc` 树上查看。
> **原句（已纠正，保留原文）**：~~实现位于 `src/libipc/platform/posix/udp.h:349-353` 的 `FD_SET(server_fd)`~~

- 两条边界的**性质不同**：本仓 SHM 侧超限是**显式回退**（`RecvRegisterStatus::wait_set_full`/`backend_unavailable`，`src/dzIPC/threepools/recv_worker.cc:611-624`）；socket 侧 fd 越界是**进度崩溃**（既有缺陷）。
- 实测佐证：socket 探针 scale=1000 时 `fds_open=2068`、`max_socket_fd=2067`；worker 模式存活，compat/基线模式 abort（`test/perf/out/20260927_t6_scale/worker_1000.log`、`compat_1000.log`）。
- iceoryx 侧超限是**预配置/编译期**上限，撞上就是注册失败或丢包——**没有"退化为每路一条线程"的路径**。在"超限时的行为可推理"这一点上，本仓的显式回退设计是**优于** iceoryx 的工程属性。

---

## 5. 差距归因：延迟差在哪一层

> **本节整体从"已归因"降级为"待分层测量"。** 初版把主因判给"每消息字节流搬运次数"，但现有实验**没有隔离**编解码、分配、等待、调度、排队各自成本（方案 §10.2/§10.7 要求的正是这种分层）。以下保留可证的观察，删去不可证的因果。

### 5.1 数据面拷贝：A 路径已实现，B 路径**尚无性能证据**

- **A 路径（本仓现网默认的大消息优化）**：`try_publish_dzflat()` = `loan` → `dzflat_write` → `publish_loan`，**跳过** `serialize()` 的整包 `new` 与 TLV 页尾分段拷贝，但**仍有容器→chunk 的 1 次字段级拷贝**。【代码】`src/dzIPC/shm_pub_sub_ipc.cc:460-486`
- **B 路径（0 拷贝）**：契约在 `include/dzIPC/common/loaned_message.h`（应用在借用 chunk 中就地把构造，`loan(varlen_budget)` → 写 → `publish_loaned`）。【代码】
- ⚠️ **`ipc_benchmark --compare` 走的是 A 路径，不是 B**（纠偏要点 1）。判据三重：
  1. `test/ipc_benchmark.cpp:322` 调用 `pub->publish_blocking(pmsg->topic(), 100)`，`pmsg` 是**已经 `make_payload()` 构造好的 `TestMsg`**；
  2. 路径内部只到 `loan → dzflat_write → publish_loan`（`shm_pub_sub_ipc.cc:475-485`）；
  3. B 路径要求的是**应用侧**直接对借出内存构造，`--compare` 从不调用 `loan<T>()`/`publish_loaned()`。
- **初版 §5.1 的 TLV 481.5 / DZFlat 33.6 µs（14.32×）读数**：本仓**未找到原始输出文件**，标【历史-不可回溯】，**已从胜负表中移除**，须由 W02/W11 用统一跨进程基准重测（UF-01）。删除的结论包括"33.6 µs 已优于本机 iceoryx 157.7 µs"与"把 DZFlat 变成默认路径就能反超本机 iceoryx"。
- 嵌套数组场景的 40×/75× 等数字来自 `docs/dzflat_shm.md` §9.2 的**离线/专项基准**，与本节口径不同，**不得合并引用**。

### 5.2 chunk 池天花板：40/档 vs 可配置 mempool

- 本仓每尺寸档 **40 块**全进程共享（`include/libipc/def.h:49`）。⚠️ **注释与代码不一致**：`src/libipc/ipc.cpp` 的注释仍写 `ipc::large_msg_cache(=32)`，`include/dzIPC/common/loaned_message.h` 头注也写"每个尺寸档位只有 32 块"，**以代码 40 为准**（该不一致已登记；见 UF-05）。
- 后果：池满时大消息退化为 **64 B 分片**打进 256 槽环。文档明确定性为"**性能悬崖，不是优雅降级**"（`docs/dzflat_shm.md` §5.3）。
- iceoryx：mempool **发行前按业务配置**（本机 1 MB×50、128 B×10000…；官方示例到 4 MiB×10）。这是"配置换确定性"。
- **对本仓的直接含义**：A/B 的收益**只有在池容量与消费速率匹配时才拿得到**；容量与背压属于 W09 的范围，本文不做结论。

### 5.3 控制面与 DDS 层（保留）

本机 iceoryx 数据经 CycloneDDS。这解释了为什么本机组合是 12 µs / 158 µs，而官方裸 iceoryx 是 0.6 µs / 0.1 µs：**DDS 实体与串行化栈的固定成本被计入了**。因此 §3.1 的 1–2× 是本仓与"实际部署形态"的差距，§3.3 的 12–250× 是本仓与"理论极限"的差距。**混用这两个数字会得出错误结论。**

### 5.4 p99 抖动：环境而非架构（保留，但不再当作已排除）

两侧 p99 都劣化 2–10×，共同环境：`powersave`、未绑核、`load ≈ 4–10`。同一份 dzIPC 用例两轮 p99 从 92.47 µs 跳到 446.90 µs，证明 p99 目前主要由环境支配。本仓提供了显式逃逸通道（`--pin=A,B`、`ThreadDispatch` 的 `SCHED_FIFO` opt-in），但**默认不启用**。要做尾延迟对比，必须两侧都绑核并固定 governor。
⚠️ **初版把"p99 都是环境噪声"用来排除线程模型影响；该推论不成立**——环境噪声大不等于架构差异为零，只说明当前实验**分辨不出**。尾延迟结论**待验证**。

### 5.5 覆盖缺口：SHM 订阅侧尚未接入线程池（保留，证据复核通过）

| 模块 | 是否接入池 | 证据 |
|---|---|---|
| `socket_pub_sub_ipc` | ✅ `SocketRecvWorkerPool` | 5 处引用；`worker_mode_` |
| `socket_ser_cli_ipc` | ✅ `SocketRecvWorkerPool` | 6 处引用 |
| `shm_ser_cli_ipc` | ✅ `RecvWorkerPool` | 7 处引用；`SerRequestRoute` |
| **`shm_pub_sub_ipc`（SHM pub/sub 订阅侧）** | ❌ **0 处** | 仍为 `sub_handshake_thread_` + `subscribe_thread_`（`src/dzIPC/shm_pub_sub_ipc.cc:940-941`），`recv(50)` 在 lease 内执行 |

- **⚠️ 纠偏要点 5 复核通过**：SHM 订阅侧**同时**创建**握手线程**与**订阅(收包)线程**，固定 worker 接收**未实现**。`InitChannel()` 里两行 `new std::thread(...)` 即在 `shm_pub_sub_ipc.cc:940-941`。
- 影响面：**§4.1 的"33 线程"只覆盖 socket 两模块**；`ShmControlScheduler` **全仓零接入点**（只被自己的 `.cc` 与单测引用）。这是需求 §11.1/§11.2 尚未闭环的部分，也是与 iceoryx 比"规模轴"时**必须承认的未完成项**。

---

## 6. 可比性边界与未测项（诚实清单）

**未测：**
1. iceoryx 侧**满速吞吐**（本轮只跑定速时延；`latency_publisher` 是限速发送模型）。【待验证】
2. iceoryx 侧 **1000 订阅/多话题**规模对照（本机 perf_tests 只提供单/双 topic 场景）。【待验证】
3. iceoryx2（0.10.0）**本机实测**——本机只装了 iceoryx **2.0.5**（1.x 产品线版本号，≠ iceoryx2）。【待验证】
4. p99.9/max 尾延迟的**受控**对比（需绑核 + governor 固定）。【待验证】
5. 跨机路径（两者都只测了本机）。【待验证】
6. CycloneDDS 是否走零拷贝路径的**直接证据**（无计数器）。【待验证】
7. **B 路径（应用就地构造）的跨进程性能**——本轮完全未测（§5.1）。【待验证】
8. **A 路径的 `--compare` 读数无原始文件**——不可回溯（UF-01）。【待验证】

**不可比：**
9. Harness 不同：本仓基准是专用多进程基准（写 CSV/JSON/样本），iceoryx 侧是示例程序、只输出统计摘要；**不能把两侧的 mean/σ 直接相减**。
10. **消费者工作量不对称（本轮新增，1 MiB 档的关键）**：dzIPC 侧 `try_get_clone` 整包克隆（≈1 MiB memcpy + ~1600 次堆分配），iceoryx 侧回调只读头部（§3.2）。**这不是同一工作负载。**
11. **等待方式不同**：dzIPC 订阅端忙等自旋（`test/dzipc_perf_benchmark.cpp:898-901` 的 `continue` 轮询）；iceoryx 侧由 DDS 回调驱动。忙等会改变唤醒延迟与 CPU 占用。
12. 订阅语义不同：本仓默认进入有界队列、由用户 `try_get*` 消费；iceoryx 侧是回调/`take` 语义。队列深度与消费时机不同会让"延迟"含义发生偏移。
13. 载荷类型不同：本仓是 `TestMsg`（`float64[] + int32[] + string[] + bool`）的 TLV/DZFlat；iceoryx 档是 `Float64Array` 经 CDR 编码。字节数相同（1 MiB），**编码与分配成本不同**。
14. ✅ 初版第 10 条（iceperf 方向未明示）**已解决**：源码确认为单向（§3.3）。

**已知不对称（有利于本仓的）：**
15. 本机 iceoryx 组合多一层 CycloneDDS；纯 iceoryx API 会更快。
16. 本机 RouDi mempool 最大档 1 MiB×50，而本仓 chunk 池"1 KiB 台阶 × 40/档"没有大档位预配置压力 —— 但反过来说本仓的池更容易在大消息上退化为分片（§5.2），**这一点不利于本仓**，故不构成"本仓占便宜"。

---

## 7. 优化建议（按收益排序，按证据强弱排序）

> 判断依据：**先补证据与口径（P0-0），再动数据面拷贝次数（A 已实现、B 待测），再动规模覆盖与 SHM 接入（§5.5），最后才动调度细节。** 线程池已处于"没有明显可再压的空闲成本"的状态（CPU 0.0050 核，同口径）。

| 优先级 | 动作 | 预期收益 | 验证方法 | 归属 |
|---|---|---|---|---|
| **P0-0（本轮新增）** | 先按方案 §6.2/§12 建**统一跨进程基准**与 run_id 目录，把本文所有读数重采一遍；特别是把 A/B/TLV 做成**同进程模型、同等待方式、同计时边界、同消费工作**的三组实验 | 让后续所有数字可比、可回溯 | W02/W03 交付 | 基准/测量 |
| **P0** | **B 路径（应用就地构造 → `publish_loaned`）跨进程专项基准**，与 A 路径分列；**不得**用内部 `loan()` 代替 B 证据 | 量化 B 的真实收益（本轮无任何 B 性能证据） | 新增跨进程 B 基准 + `dzflat`/`fallback` 计数 | ipc-transport / DZFlat（W08） |
| **P0** | **SHM 订阅侧接入 `RecvWorkerPool`**，消除 per-route `recv(50)` 线程 | 1000 路 SHM 路径 threads 从 O(N) → 固定；消除 50 ms 空轮询 | 复用 `t6scale.cpp` 的 **SHM 版 + 每路独立 topic**（当前只有 socket 版且同 topic） | ipc-transport（W06） |
| **P0** | **空闲观测口径统一**：两侧（基线/worker/compat）都用全线程聚合 + ≥60 s 窗口，并分别统计启动峰值、稳定活动、空闲回落 | 让"19325 → 10"这类比较成立；当前无可用 ctx 数字 | `sum(/proc/self/task/*/status)` 或 `perf stat`；60 s 窗口 | 测量（W03） |
| **P1** | 让 `ShmControlScheduler` 真正被 `shm_pub_sub_ipc`/`shm_ser_cli_ipc` 接入（当前**全仓零接入点**） | 握手线程 O(N) → 1；与架构图 ⑤ 一致 | `test/test_shm_control_scheduler.cpp` + 模块级线程计数 | ipc-transport（W05） |
| **P1** | 固定 `governor=performance` + `--pin` 绑核，重跑双方 p99/p99.9 | 把 p99 从"环境噪声"变成"架构读数" | 两侧同条件重跑；本仓 `--pin=A,B` | 测试（W11） |
| **P2** | socket 数据面 `select()` → `poll()`（既有缺陷，~~`src/libipc/platform/posix/udp.h:349-353`~~ → **`src/libipc/platform/posix/udp.h:345-353`**） | 解除 `fd ≥ 1024` abort；1000 路 socket 数据面可达 | `test/perf/out/20260927_t6_probes/fdprobe --scale=1000` | ipc-transport（W07） |
| **P2** | chunk 池容量与 `ViewQueueCap()` 联动配置化（当前 40 硬编码），并收敛 **40 vs 注释 32** 的口径不一致 | 让 A/B 收益在压测/满负载下不退化为分片 | `docs/dzflat_shm.md` §5.3 预算推算 + 池耗尽计数 | ipc-transport（W09） |

> **⛔ 行号更正（UF-17 / t54 收口，2026-09-28）**：本表 P2 行原写 `udp.h:349-353`，与 §4.3 同源、同一处 off-by-one ⇒ 已改为 **`udp.h:345-353`**（原文以删除线保留于本行内）。统一口径逐行：`:345 fd_set` / `:346 FD_ZERO` / `:347 FD_SET` / `:349-351 timeval` / `:353 ::select`。锚定 HEAD **`e800ccc`**（blob `19b61326…`）；W07 改 `poll` 后为**预期漂移**。依据：**W01-F3** + 全历史扫描（`:346=FD_SET` **0** 次、`:347=FD_SET` **1** 次）。详见 §4.3 表下的「行号更正」块。

**明确不建议做：**
- **不要把"iceoryx 式定容 POD"当默认载荷模型。** `docs/dzflat_shm.md` §6 已给代价：IDL 须声明上界、4 MB × 32 槽 = 128 MB/topic、Python 侧要另写。本仓的价值之一恰是"任意可序列化对象都能走"，TLV 兜底不能拆。
- **不要用"减少 worker 数/统一单线程收包"去追 iceoryx 的延迟。** §4.2 已证明空闲 CPU 成本已经接近零（同口径）；再收敛线程数只会损失并行度。
- **不要拿注册规模换 fd**：iceoryx2 的 epoll 路线把成本转成了 fd（每 Listener 1 fd + 每 shm 对象 1 句柄），本仓 SHM 路径 0 fd 反而是优势。
- **不要在口径统一前引用任何跨方案倍数**（14.3× / 225× ctx / 1.9× 等）。

---

## 8. 复现与原始数据索引

> **完整映射（数值 → 原始文件/行、机制判断 → 代码/版本化来源）见 `团队改造交付/W01/证据索引.md`。**

| 内容 | 路径 |
|---|---|
| 本仓 SHM 基准（官方口径，3 s/档） | `perf_results/20260916_185113/report.md` §5.1、`results.csv` |
| 本仓 SHM 基准（本波复测，2 s） | `tmp/perf_current_shm/results.csv` |
| 本仓「配速」复测（与 iceoryx 同速率） | `tmp/perf_match_64/`、`tmp/perf_match_1m/` |
| iceoryx 本机实测原始日志 | `tmp/iox_probe/{sub_l64,sub_l1m,pub_l64,pub_l1m,roudi}.log` |
| iceoryx 本机对照**源码**（消费/发布与计时点） | `/home/zwc/branch/lejulab_platform/src/lejusdk/perf_tests/src/{latency_subscriber,latency_publisher}.cpp` |
| 线程/上下文切换/空闲 CPU（**socket 路径**，同 topic） | `test/perf/out/20260927_t6_scale/{worker_1,worker_100,worker_477,worker_1000,compat_477}.log` |
| 基线库 ctx/CPU（**全线程聚合**口径，自报 `ctx_scope`） | `test/perf/out/20260927_t6_boundary/{base_1,base_100,base_477,base_511}.log` |
| 探针源码 | `test/perf/out/20260927_t6_probes/t6scale.cpp`（socket；`/proc/self/status` 口径） |
| 空闲 CPU/切换口径实证（本轮新增） | `团队改造交付/W01/本机复现/csw_cpu_scope_probe.{c,output.txt}` |
| 集成测试与性能基准报告（历史） | `docs/消息接收架构改造/集成测试与性能基准报告.md` §3 |
| DZFlat 离线与传输基准 | `docs/dzflat_shm.md` §9.2/§9.5；`build/bin/dzflat_benchmark`、`dzflat_tx_benchmark` |
| 外部来源存证（固定 tag/commit + sha256） | `团队改造交付/W01/外部来源存证/` |
| 架构图 | `docs/消息接收架构改造/事件驱动线程池架构图.svg` |

**外部来源（已固定版本/提交）：**
- iceoryx **v2.95.8**（commit `ffd361023196d1422f1ff96723d1c83d9d4838ae`）：iceperf README、`base.cpp`（单向折半依据）、`doc/design/listener.md`、`condition_listener.cpp`。
- iceoryx2 **v0.10.0**（commit `135d09dd8b29f321f1725920d434864c4e512378`）：`iceoryx2-bb/linux/src/epoll.rs`（512 语义）、`iceoryx2/src/port/{listener,notifier}.rs`（无后台线程）、`iceoryx2-cal/src/event/*`、官方 benchmark 原始数据 `.dat`、event 示例 README。
- 本机 `proc(5)` man page（man-pages **5.10-1ubuntu1**）：`voluntary_ctxt_switches`/`utime` 字段定义。

**修订记录**：初版 2026-09-28（快照见 §头部 sha256）；W01 纠偏版 2026-09-28。任何进一步修订必须新增快照，不得覆盖本版。
