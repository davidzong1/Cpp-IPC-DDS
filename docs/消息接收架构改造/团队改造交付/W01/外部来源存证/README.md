# 外部来源存证（W01）

> **目的**：满足 W01 验收"**外部结论固定版本或提交**"与"每个机制判断有代码或版本化来源"。
> **规则**：主报告中任何引用外部结论的句子，都必须能指向本目录中的**某个文件**（或其中的行号）。
> **禁止**：指向 `main`/`master` 分支的链接一律视为不可引用（会上游漂移）。本目录内的文件即"当时的原文"。

## 0. 固定点（tag → commit）

| 仓库 | Tag | Tag→commit | 解析方式 | 抓取日期 |
|---|---|---|---|---|
| `eclipse-iceoryx/iceoryx2` | `v0.10.0` | `135d09dd8b29f321f1725920d434864c4e512378` | `GET /repos/eclipse-iceoryx/iceoryx2/git/ref/tags/v0.10.0` → `object.sha` | 2026-09-28 |
| `eclipse-iceoryx/iceoryx` | `v2.95.8` | `ffd361023196d1422f1ff96723d1c83d9d4838ae` | `GET /repos/eclipse-iceoryx/iceoryx/git/ref/tags/v2.95.8` → `object.sha` | 2026-09-28 |
| 本机 man-pages | `5.10-1ubuntu1` | — | `dpkg -s manpages` → `Version: 5.10-1ubuntu1` | 2026-09-28 |

> ⚠️ 注意区分：本机安装的 **`RouDi version 2.0.5`** 属于 iceoryx **1.x** 产品线，**不是** `iceoryx2 v0.10.0`。二者不得混引。

## 1. 文件清单（含 sha256）

### 1.1 `iceoryx2_v0.10.0/`（8 个）

| 存证文件 | 上游路径（tag `v0.10.0`） | 字节 | sha256 | 用途（对应证据索引 ID） |
|---|---|---|---|---|
| `epoll.rs` | `iceoryx2-bb/linux/src/epoll.rs` | 28870 | `73fef37d53f128d50fc704cdcfa9aea1428d7096a304740dcb431553f528e699` | E-30/E-31：`max_wait_events()==512` 用作 `epoll_wait` 事件数组长度；注册总量上限来自 `max_user_watches` |
| `listener.rs` | `iceoryx2/src/port/listener.rs` | 14120 | `fd32bcc873a353a14964c0b6f85a96cdcc972aefb221db750be95ae7112dcf63` | E-32：`try_wait()`/`blocking_wait()` 由**用户线程**调用，无后台线程 |
| `notifier.rs` | `iceoryx2/src/port/notifier.rs` | 29098 | `125aae1ed7142da1069583a196cb833ceae0e61977538e00598a7bae2debac58` | E-32 对照：Notifier 同为端口，无自身线程 |
| `event_recommended.rs` | `iceoryx2-cal/src/event/recommended.rs` | 854 | `ded7ace49a566edbd4040731cedb07c2a2153d7e4ab0c7dce3b1b3c79e30a5e2` | E-33：`Ipc = UnixDatagramShmCountingBitSet`、`Local = SocketPairCountingBitSet` |
| `implementations.rs` | `iceoryx2-cal/src/event/implementations.rs` | 1779 | `934956f1527db994da3d802fd49ae071c1a3bb00bbd8801ca049d4e908e39e7f` | E-32：事件实现层**无线程创建** |
| `common.rs` | `iceoryx2-cal/src/event/common.rs` | 23245 | `10b6410acc20b02ea8e6f4d13575acf351b4f93a9044eb1bcc1107d4a2dfa1fd` | E-32：同上（全目录无线程） |
| `event_based_communication_README.md` | `examples/cxx/event_based_communication/README.md` | 2492 | `6e65ac4e780ecf32f9038fe0963a17df798e94cde563a5b549c73b70dc1765b2` | E-34：官方用法是"挂 `WaitSet`、用户取 `EventId`" |
| `benchmark_mechanism_comparison_i7_13700h.dat` | `internal/plots/benchmark_mechanism_comparison_i7_13700h.dat` | 1303 | `1f34237c20a3a188d5bbd3ade583ebeab979d2f76e222149128d13e19a4df799` | E-38：iceoryx2 index 前 18 点均为 0.092–0.099 µs（即 92–99 ns 平坦） |

### 1.2 `iceoryx_v2.95.8/`（7 个）

| 存证文件 | 上游路径（tag `v2.95.8`） | 字节 | sha256 | 用途 |
|---|---|---|---|---|
| `iceperf_README.md` | `iceoryx_examples/iceperf/README.md` | 20680 | `bfb500303a1deaafe4f1f80633bf99aca7ad4d97ea8ae5a87d593f634c0689dd` | 官方 0.58–0.73 µs、1 kB–4 MB 平坦 |
| `base.cpp` | `iceoryx_examples/iceperf/base.cpp` | 2184 | `9749ef17ace112bc3512759054872256dc95330391e17ff09fdaa6c22fd71413` | **E-37（关键）**：`:49-52` `TRANSMISSIONS_PER_ROUNDTRIP{2U}` ⇒ iceperf 表值是**单向**（已折半） |
| `base.hpp` | `iceoryx_examples/iceperf/base.hpp` | 1690 | `cbb953c023763c1bd57f5b4f4e510078b986ca4a5453f49a2634064b52169fe8` | 上条的接口声明 |
| `iceperf_leader.cpp` | `iceoryx_examples/iceperf/iceperf_leader.cpp` | 8610 | `6721b347692df7daca04bf37cf715faa4f4ee76c9f0ce2d165b017d17a9a9d92` | 表格打印路径（`Average Latency [µs]`） |
| `iceoryx.cpp` | `iceoryx_examples/iceperf/iceoryx.cpp` | 3177 | `41802caebebe3d81a363f4570d1a524cbffc20bacb2a07e78aafe150eabdddea` | iceperf 的 iceoryx 实现（loan/publish 收发） |
| `listener_design.md` | `doc/design/listener.md` | 16876 | `46f6912d4ec4790b2e455eb6921b95d96eaf62acf4e54963c28b227d08d55cb5` | E-35：`:9` "callback is called in the Listener background thread"、`:13` "a separate background thread"、`:102` `m_thread : std::thread` |
| `condition_listener.cpp` | `iceoryx_posh/source/popo/building_blocks/condition_listener.cpp` | 4084 | `380852fe7ab39f84cd2de7a75e5e654076d4c16efb788e3b6d189ebe4b9cb1e0` | E-36：`:58-68` `wait()` 内部 `sem_wait`；`:81-107` `waitImpl` —— 是**调用者线程的阻塞调用**，不自行起线程 |

### 1.3 `本机man/`（1 个）

| 存证文件 | 来源 | 字节 | sha256 | 用途 |
|---|---|---|---|---|
| `proc.5.txt` | 本机 `man 5 proc`（man-pages `5.10-1ubuntu1`，`MANWIDTH=120`） | 206041 | `005bec2109acb1f8a0d9700ffcbde13672156cf1e6437af7afe8705df21d89e2` | E-41：`voluntary_ctxt_switches` / `nonvoluntary_ctxt_switches` / `utime` 的**字段定义**（该文档未给聚合语义，故 E-42 仍为未决） |

## 2. 关键行的直接引用（便于评审就地核对）

| 结论 | 文件:行 | 原文要点 |
|---|---|---|
| epoll 512 语义 | `iceoryx2_v0.10.0/epoll.rs:449-450` | `pub const fn max_wait_events() -> usize { 512 }` |
| 512 的实际用途 | `iceoryx2_v0.10.0/epoll.rs:584-591` | `let mut events: [MaybeUninit<epoll_event>; Self::max_wait_events()] = …; …epoll_wait(…, Self::max_wait_events() as _, …)` |
| 注册总量上限 | `iceoryx2_v0.10.0/epoll.rs:77` | `const MAX_USER_WATCHES_FILE: &str = "/proc/sys/fs/epoll/max_user_watches";` |
| iceoryx2 Listener 无自身线程 | `iceoryx2_v0.10.0/listener.rs:275-283` / `:304-310` | `pub fn try_wait(…)` / `pub fn blocking_wait(…)` —— 用户调用 |
| iceoryx2 门铃实现 | `iceoryx2_v0.10.0/event_recommended.rs:15,19` | `pub type Ipc = crate::event::UnixDatagramShmCountingBitSet;` / `pub type Local = crate::event::SocketPairCountingBitSet;` |
| iceoryx 1.x Listener 有后台线程 | `iceoryx_v2.95.8/listener_design.md:9,13,102` | "callback is called in the Listener background thread as a reaction."；"a separate background thread…"；`m_thread : std::thread` |
| ConditionListener 不自己起线程 | `iceoryx_v2.95.8/condition_listener.cpp:58-68,81-107` | `wait()` → `getMembers()->m_semaphore->wait()`（调用者线程阻塞） |
| **iceperf 值已折半为单向** | `iceoryx_v2.95.8/base.cpp:49-52` | `constexpr uint64_t TRANSMISSIONS_PER_ROUNDTRIP{2U};` → `duration / (numRoundTrips * TRANSMISSIONS_PER_ROUNDTRIP)` |
| iceoryx2 92–99 ns 平坦 | `iceoryx2_v0.10.0/benchmark_mechanism_comparison_i7_13700h.dat` | 索引 0 列：0.064→0.098、0.128→0.094、…、4096→0.094（单位 µs） |

## 3. 未取到的存证（诚实边界）—— 其中 1 项已由 W03/t4 补齐（2026-09-28）

| 目标 | 结果 | 影响 |
|---|---|---|
| `torvalds/linux` `fs/proc/array.c`（v6.8） | ~~两次抓取均超时（各 55 s，0 字节）~~ → **✅ 已由 W03/t4 补齐（2026-09-28）**：改用 `git.kernel.org` 的 plain 端点取回（HTTP 200）。存证在 **`团队改造交付/W03/外部来源存证/proc_array_v6.8_do_task_stat.md`**（本目录**不重复存放**，避免双份漂移）；本文档负责人已就同一 URL 独立复取并逐段核实 | `/proc` 两字段的聚合语义现为"**版本化源码 + 三份实测**"（W01 探针 / W01 归档输出 / W03 采集器 `artifacts/perf/20260928-r03-W03-ctxscope/`）⇒ `../未决事实清单.md` **UF-06 已解除**；残余：上游 v6.8 ≠ 本机补丁树（未逐字节核对） |
| iceoryx 1.x 的 `sem_t`/mempool/订阅者队列默认值等 | 本轮**未抓取**（沿用旧 `.research/iceoryx_perf_facts.md` 的链接，仍指向 `main`） | 主报告 §2 表格中标【来源-部分】的格子需在 W02/W03 补存证后升级 |

## 4. 复现抓取（供复核者重跑）

```bash
B2=https://raw.githubusercontent.com/eclipse-iceoryx/iceoryx2/v0.10.0
B1=https://raw.githubusercontent.com/eclipse-iceoryx/iceoryx/v2.95.8
curl -sS -o epoll.rs   "$B2/iceoryx2-bb/linux/src/epoll.rs"
curl -sS -o listener.rs "$B2/iceoryx2/src/port/listener.rs"
curl -sS -o base.cpp   "$B1/iceoryx_examples/iceperf/base.cpp"
curl -sS -o listener_design.md "$B1/doc/design/listener.md"
# 校验：sha256sum -c 对比第 1 节表格
```
