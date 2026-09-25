# 阶段 2 并发验收测试方案（只读报告，未改任何源码）

> 范围：审查现有 SHM pub/sub 测试组织，提出并行覆盖 **generation rebuild / 关闭唤醒 / 已弹出 buffer 不丢失** 的测试策略与可执行命令。
> 边界：**未修改 `RouteSession` 源码，也未修改任何产品代码**（本轮 `git status --porcelain` 为空）。
> 依据：`docs/消息接收架构改造/事件驱动线程池需求.md` §4/§8/§10/§11；`阶段2_RouteSession实现说明.md`；`阶段3_收包分流提取实现说明.md`；`阶段4_跨平台wait-set实现说明.md`。

---

## 0. 一句话结论

三条目标里，**generation rebuild 与关闭唤醒都能在"真实 SHM 但进程内"这一层做确定性验收**（现成模板充足）；**"已弹出 buffer 不丢失"是唯一必须跨真实 pub/sub 窗口的判据，也是最容易写成假绿的一条**——因为它在修复前后都可能"最终又收到消息"，必须按需求 §8-2 用**调用顺序计数**而不是功能结果来判。

同时必须先解决一个**可测性障碍**（§5）：`RouteSession::ReceiveLease` 把 `std::shared_ptr<ipc::route>` 写成了具体类型，测试替身注入不进去。

---

## 1. 现状审查

### 1.1 测试组织与约定（已核实）

| 项 | 事实 |
|---|---|
| 组织 | `test/` 平铺 `.cpp`，**一文件一 gtest 二进制**，target 名 = 源文件 basename |
| 接入 | `test/CMakeLists.txt` 用 `file(GLOB ...)` 扫 `test/*.cpp`，**无 `CONFIGURE_DEPENDS`** ⇒ 新增 `.cpp` 后**必须重跑 cmake** |
| CTest | 全仓只登记 2 项：`test_udp_port_boundary`（:126）、`test_shm_control_scheduler`（:134），均带 `TIMEOUT 120` |
| gmock | **无**（gtest 1.10.0 vendored，未带 gmock）⇒ 替身只能手写 |
| sanitizer | **全仓无 TSAN/ASAN 构建配置** |
| 链接 | 每个测试 target 链 `ipc gtest gtest_main Threads::Threads` |

### 1.2 现有 SHM pub/sub 测试盘点（与阶段 2 相关的部分）

| 文件 | TEST 数 | 与阶段 2 的关系 |
|---|---|---|
| `test_dzipc_shm.cpp` | 10 | `:242 SubscriberRecoversAfterPublisherRestart` = **顺序版** generation rebuild（publisher 先析构、再重建，无并发） |
| `test_shm_receiver_cap.cpp` | 2 | `:119` 直接用 `TopicControlPlane::open()` 读 `peer_count()` —— **控制面只读观测的现成范式** |
| `test_chunk_hold.cpp` | — | `:101 HeldChunkSurvivesOverwriteWithLaggingPeer` = **双接收方进度差**构造，是"buffer 被弹出后仍被持有"的最近邻模板 |
| `test_uf011_chunk_return.cpp` | 1 | 池级归还判据，含"量具不得改变被测对象"的采样纪律 |
| `test_sercli_auto_path.cpp` | — | `ctrl_head`/`read_ctrl_head()` + `:1337` **`generation` 不推进**判据（ser 侧），可移植到 pub/sub 侧 |
| `test_uf004_shutdown_monitor_optout.cpp` | 7 | `:89 run_in_child()` = **fork+pipe+poll+waitpid 硬超时**模板 |
| `test_uf009_graceful_exit.cpp` | 3 | 进程级关闭（信号路径），非对象级析构唤醒 |
| `test_shm_control_scheduler.cpp` | 19 | 阶段 1 产物：文件组织 + ctest 注册 + 并发用例写法的**直接同构模板** |

### 1.3 盲区（三条目标各自的现有覆盖）

| 目标 | 现有覆盖 | 缺口 |
|---|---|---|
| generation rebuild | `test_dzipc_shm.cpp:242`（**顺序**）；`test_sercli_auto_path.cpp:1337`（ser 侧不推进） | **无**「recv 正阻塞时触发 rebuild」的并发用例；无「rebuild 与 recv 重叠」的调用顺序计数 |
| 关闭唤醒 | `test_uf009_graceful_exit`（信号）；`test_uf004_timeout_unlink`（超时 unlink） | **无**「析构时 recv 正阻塞 ⇒ `stop_and_wake` 唤醒 ⇒ join 不依赖 50ms 超时」 |
| 已弹出 buffer 不丢失 | `test_dzipc_shm.cpp` 有端到端完整性，但**无 rebuild 窗口** | **无**「rebuild 期间 recv 弹出的那条必须进队列」的判据 |

---

## 2. 前置依赖（必须先裁定，否则 A 臂写不出来）

### 2.1 RouteSession 尚未落码

```
include/dzIPC/shm_route_session.h        → 不存在
src/dzIPC/shm_route_session.cc           → 不存在
grep -rn "RouteSession|acquire_receive|wait_quiescent|stop_and_wake" include/ src/ test/
                                          → 只命中 docs/ 下的 4 个 .md，无任何 .h/.cc/.cpp
```

阶段 2 说明自述「状态：实现说明，代码未改」。⇒ 本方案是**实现后即可执行**的设计稿。

### 2.2 接口在两份文档间不一致（需 leader 裁定，否则测试无法定稿）

| 项 | 需求 §4 | 阶段2 文档 §2 |
|---|---|---|
| `begin_rebuild` | `begin_rebuild(uint32_t new_generation)` | `begin_rebuild(uint32_t, const std::function<std::shared_ptr<ipc::route>()>& create)` |
| 只读访问器 | 无 | 有 `current_route()` / `generation() const` |
| `release_receive` | 未单列 | `void release_receive() noexcept` |

**测试影响**：`create` 回调是否入参，决定 B 臂能不能用「计数版 create」观测「第 5 步之前禁止 `release()`」——这是需求 §8-2 的核心判据。建议采纳阶段2 文档版本（可注入），并把差异登记为待确认项。

---

## 3. 三条并行覆盖臂

> 三条臂**互不依赖，可并行开发与并行执行**（§6 给出 `-j` 命令）。命名沿用现有约定：`test_<unit>.cpp` ⇒ target 同名。

### A 臂 · 纯逻辑状态机（`test/test_shm_route_session.cpp`）

**目标**：把 `RouteSession` 的状态协议钉死，不依赖真实握手、不依赖挂钟计时。这是三条臂里唯一能做到**完全确定性**的一条。

| 用例 | 构造 | 判据 | 反向判据（防假绿） |
|---|---|---|---|
| `AcquireReturnsEmptyBeforeFirstRebuild` | 新建 `RouteSession`，立即 `acquire_receive()` | 返回 `nullopt` | 若返回 lease ⇒ 空 route 被当成可用 |
| `AcquireIncrementsThenReleaseNotifies` | `begin_rebuild(1, create)` → `acquire_receive()` | lease 非空、`generation()==1` | 不 release 时 `wait_quiescent()` 必须**阻塞**（用第二线程 + 短超时断言未返回） |
| `AcquireRejectedWhileRebuilding` | 线程 T 卡在 `begin_rebuild` 的「等 inflight 归零」；主线程 `acquire_receive()` | 主线程拿到 `nullopt` | 若拿到 lease ⇒ 重建期放进了新 recv |
| `ReleaseOnlyAfterInflightZero` | `create` 回调内**断言** `inflight==0`（自注入计数） | `create` 被调用时计数为 0 | 需求 §8-2 的**决定性判据**：`release()` 必须发生在 recv 返回且 `release_receive()` 之后 |
| `StopAndWakeUnblocksWaiter` | T 卡在 `wait_quiescent()`；主线程 `stop_and_wake()` | T 在 **≪ 挂钟超时**内返回 | 若 T 只是被超时放过 ⇒ `stop_and_wake` 是空转 |
| `StopAndWakeMakesAcquireEmpty` | `stop_and_wake()` 后 `acquire_receive()` | `nullopt` | — |
| `CreateFailureLeavesCleanState` | `create` 返回 `nullptr` | `rebuilding_==false`、route 为空、后续 `acquire_receive()` 仍 `nullopt` | 阶段2 文档 §4 明确要求「不留 `rebuilding_==true`」 |

⛔ **不要**在这一臂里用「sleep 100ms 后看状态」这类挂钟判据——A 臂的全部价值就在于无计时。等待一律用「第二线程 + `condition_variable` + 长超时兜底（如 5s，仅用于卡死时给出可读失败，不作为性能判据）」。

### B 臂 · generation rebuild 与 recv 重叠（`test/test_shm_sub_rebuild_concurrency.cpp`）

**目标**：需求 §8-2 —— 测试线程卡在 `recv` 时触发 `begin_rebuild`，`release()` 必须发生在 recv 返回且 `release_receive()` 之后。

**构造**（不链接完整握手，用真实 `ipc::route`）：

```text
ipc::route::clear_storage(name)
ipc::route tx{name, ipc::sender}
// 1. 先建立一条可用 route，灌入若干消息，让收包线程确实卡在 recv(50) 上
// 2. 收包线程：lease = acquire_receive(); lease.route->recv(50); release_receive();
// 3. 主线程：观测到「已进入 recv」的 atomic 标志后，调用 begin_rebuild(gen+1, create)
// 4. create 回调内记录 (t_create, inflight_at_create)；disconnect 旧 route 的钩子里记录 t_disconnect
```

**判据**（每条都要有反向判据）：

| # | 判据 | 反向判据 |
|---|---|---|
| 1 | `create` 被调用时 `receive_inflight_ == 0` | 若 >0 ⇒ §4 第 5 步顺序被破坏 |
| 2 | `t_create > t_recv_returned` 且 `t_create > t_release_receive`（用单调计数或 `steady_clock` 时间戳对） | 若顺序颠倒 ⇒ 旧 route 在 recv 期间被 release |
| 3 | 旧 route 的 `disconnect()` 发生在 `begin_rebuild` 内部、且**在等 inflight 归零之前**（§4 第 3→4 步） | 若 disconnect 在归零之后 ⇒ 卡住的 recv 只能等 50ms 超时 |
| 4 | 重建完成后新 lease 的 `generation == new_generation` | — |
| 5 | 全程无 SIGSEGV/挂死（用 B 臂自己的硬超时） | — |

⚠️ `ReceiveLease` 硬绑具体类型 `std::shared_ptr<ipc::route>` ⇒ **替身注入不进去**（见 §5）。B 臂的现实替代方案：真实 `ipc::route` + 在 `begin_rebuild` 的 `create` 回调里注入计数（**这正好要求采纳 §2.2 的 `create` 版本**）。

### C 臂 · 关闭唤醒（`test/test_shm_sub_rebuild_concurrency.cpp` 同文件，或独立 `test_shm_sub_shutdown_wake.cpp`）

**目标**：需求 §8-3 / §11-3 —— 析构时 recv 正阻塞，`stop_and_wake` 之后 join 能返回，**不依赖 50ms 超时**，不访问已析构的 `shm_sub_ipc`。

**这是本轮审查发现的最高价值用例**，因为当前实现与目标**相反**：

```text
现状 src/dzIPC/shm_pub_sub_ipc.cc:542-578（shm_sub_ipc::~shm_sub_ipc）
  1. 注销 LocalPubSubRegistry                      ← 与 §5 第 1 步一致 ✓
  2. running = false
  3. join subscribe_thread_                        ← ⛔ 此时**没有** stop_and_wake，
  4. join sub_handshake_thread_                       只能靠 recv(50) 超时
  5. disconnect() + subscriber_.reset()            ← ⛔ disconnect 在 join **之后**
  6. exit_flag = true

需求 §4.2/§5 要求：stop_and_wake → join → wait_quiescent → release/reset
```

即：**「先 join 再 disconnect」vs 需求「先 stop_and_wake 再 join」**。这条用例就是该差异的硬门。

**构造与判据**：

| 用例 | 构造 | 判据 | 反向判据 |
|---|---|---|---|
| `DestructWithRecvBlockedJoinsFast` | 让收包线程稳定卡在 `recv(50)`（已握手、队列空）；主线程析构 `shm_sub_ipc`，量测析构耗时 | 析构耗时 **< 50ms**（说明是被 `disconnect` 叫醒，不是超时放过） | 若 ≈50ms 或更大 ⇒ 唤醒路径没生效。**必须在 loop 里多测几次取最小**，单次读数会被调度抖动污染 |
| `StopAndWakeDoesNotDependOnTimeout` | 把 recv 超时临时拉长（若可配）或改用「无超时阻塞」变体 | 唤醒延迟与超时参数**无关** | 需求 §9 明确：不把 `recv(50)` 改无限等待（未测过叫醒前）——所以本用例**先测 50ms 版**，无限等待版作为阶段 4 之后的追加 |
| `NoCallbackAfterDeregistration` | fork 子进程（`run_in_child` 模板），子进程内析构订阅者 | 子进程 `_exit(0)`，父进程断言退出码为 0 且**非信号致死** | 若 SIGSEGV ⇒ 回调打到已析构对象（需求 §4.2 末句） |
| `DeregisterBeforeStopOrder` | 用 `ipc_info_pool` / `LocalPubSubRegistry` 观测注册表在收包停止前已注销 | 注册表条目在 join 返回前消失 | 需求 §8-5 |

**C 臂的进程隔离必要性**：`NoCallbackAfterDeregistration` 一旦真红就是 SIGSEGV，会把整个测试二进制带走、其余用例全丢。必须放进子进程（`test_uf004_shutdown_monitor_optout.cpp:89 run_in_child` 正是为此写的：`fork` + `pipe` + `poll` + `waitpid`，**硬超时后 SIGKILL**，并把子进程日志回传父进程）。

### D 臂（跨臂）· 已弹出 buffer 不丢失（`test/test_shm_sub_rebuild_concurrency.cpp`）

**目标**：需求 §8-4 / 阶段2 文档 §3 —— `recv` 返回的非空 `buff_t` 必须送进现有分流，**不得因为 `lease.generation` 已不是当前 generation 就丢掉**。

**为什么这条最容易假绿**：修复前后「最终又能收到消息」都可能为真。所以判据必须落在**投递计数**上。

| 用例 | 构造 | 判据 | 反向判据 |
|---|---|---|---|
| `PoppedBufferSurvivesGenerationChange` | 真实 pub/sub。收包线程 `acquire_receive` → 发布端发 1 条 → 让 recv **正好返回**这条的瞬间，主线程 `begin_rebuild` 推进 generation | 该条消息**必须**出现在 `view_queue_` 或 `msg_queue_`（用 `try_get`/`try_get_clone` 取到，或直接数队列长度） | 需求 §3 原文：字节已从旧 route 弹出，丢掉就是丢消息 |
| `NoLossAcrossRebuildLoop` | 循环 N 轮：每轮发 1 条 + 触发一次 rebuild | 送达计数 **== N**（不丢、不重） | ⚠️ 这条是**统计判据**，N 轮里偶发丢 1 条会被淹没 ⇒ 必须配合上面那条逐条判据，不能只靠它 |
| `RebuildDoesNotDuplicate` | 同上 | 计数 **== N**（不是 >N） | — |

⛔ **采样纪律**（承自 `test_uf011_chunk_return.cpp:26-28`）：量具（读队列/读段）**不得插在发布循环里**，否则会改变竞态窗口。只在开跑前与收尾后各量一次。

---

## 4. 可复用测试入口（已确认，精确到符号）

| 复用项 | 位置 | 用途 |
|---|---|---|
| **fork 子进程 + 硬超时** | `test/test_uf004_shutdown_monitor_optout.cpp:89 run_in_child()` | C 臂 `NoCallbackAfterDeregistration` 的进程隔离壳。`fork`+`pipe`+`poll`+`waitpid`，8s 超时，超时后 SIGKILL 并把子进程日志回传 |
| **控制面段头只读采样** | `test/test_sercli_auto_path.cpp` 的 `struct ctrl_head` + `read_ctrl_head(name)` | 读 `magic/generation/state/owner_pid/peer_count/shm_ref`。**generation rebuild 的直接签名**（`begin_rebuild()` 是唯一推进 generation 的地方） |
| **控制面直接 open + peer_count** | `test/test_shm_receiver_cap.cpp:119-125` | 不建完整握手就观测 `peer_count`。可用来断言「重建期对端计数归零」 |
| **双接收方进度差构造** | `test/test_chunk_hold.cpp:101-129` | D 臂「buffer 已弹出但仍被持有」的最近邻模板（rx_fast 持有 / rx_slow 落后） |
| **轮询等待 + 超时** | `test/test_dzipc_shm.cpp:264 wait_for_marker()`；`test/test_sercli_auto_path.cpp wait_for()` | 通用「等到条件成立或超时」 |
| **同构文件组织 + ctest 注册** | `test/test_shm_control_scheduler.cpp` + `test/CMakeLists.txt:134` | 阶段 1 产物：直接照抄文件头注释风格、`TEST(Suite, Case)` 命名、`add_test` + `TIMEOUT` |
| **`ipc::route` 独立构造** | `test/test_chunk_hold.cpp:108-110`、`test/test_lap_safety.cpp:118-136` | B 臂不需要完整 `shm_sub_ipc` 握手，直接 `ipc::route rx{name, ipc::receiver}` |
| **路由别名** | `include/libipc/ipc.h:352` `using route = chan<relat::single, relat::multi, trans::broadcast>` | B/D 臂的 SUT 基元 |

**结论**：可复用**模式**齐全，但除 `test.h`/`thread_pool.h` 外，脚手架**都在各文件匿名 namespace 里重复**（`wait_until` ×4、`run_in_child` ×3）⇒ 复用的是**写法**，不是可 `#include` 的符号。新文件按同样方式自带一份。

---

## 5. ⛔ 可测性障碍（建议在实现前解决）

**问题**：`RouteSession::ReceiveLease` 的成员是 `std::shared_ptr<ipc::route>`——**具体类型**，不是接口。

**后果**：
- 需求 §8-2 明说「**可以用测试替身计数**，不能只看功能测『最终又能收到消息』」；
- 但当前签名下，**替身注入不进去** ⇒ A 臂的 `ReleaseOnlyAfterInflightZero`、B 臂的判据 1/2 只能退化成「真实 route + 时间戳/回调计数」的间接观测；
- 这会把两条本该确定性的判据变成**依赖真实 recv 时序**的准 flaky 用例。

**三个可选缓解（按推荐度排序）**：

1. **`begin_rebuild` 保留 `create` 回调**（采纳阶段2 文档 §2 签名）——让测试能注入计数版 `create`，在回调内断言 `inflight==0`。**成本最低，不改产品语义**。
2. **暴露只读观测**：`inflight()` / `rebuilding()` / `stopping()` 三个 const 访问器（阶段 1 的 `ShmControlScheduler::Stats` 已有同类先例）。让测试无需替身就能观测状态机。
3. 把 route 抽象成接口 + 生产 `ipc::route` 适配器。**成本最高，不建议为测试改产品类型**。

**建议**：至少采纳 1 + 2。否则 A/B 两臂会退化成挂钟判据，与阶段 1 那条「scheduler 若不暴露 tick 观测，T1/T3 只能退化成挂钟计时判据，必然 flaky」是同一类问题。

---

## 6. 可执行命令

### 6.1 一次性准备

```bash
cd /home/zwc/cpp_ipc_dds

# ⛔ test/CMakeLists.txt 用 file(GLOB) 且无 CONFIGURE_DEPENDS ⇒ 新增 .cpp 后必须重跑 cmake
cmake -S . -B build
```

### 6.2 A 臂（纯逻辑，可完全确定性）

```bash
cmake --build build --target test_shm_route_session -j"$(nproc)"
./build/bin/test_shm_route_session --gtest_filter='RouteSession*' --gtest_brief=1

# 并发/状态机用例的必修课：稳定性复跑（单次绿不算数）
for i in $(seq 1 50); do
  ./build/bin/test_shm_route_session --gtest_brief=1 >/dev/null || { echo "FAILED at run $i"; break; }
done; echo "A-arm 50/50 done"
```

### 6.3 B/C/D 臂（集成，真实 SHM）

```bash
cmake --build build --target test_shm_sub_rebuild_concurrency -j"$(nproc)"
./build/bin/test_shm_sub_rebuild_concurrency --gtest_brief=1

# 逐条定位（B 臂）——注意 gtest 过滤器用 '.' 分隔 Suite.Case，不是 '::'
./build/bin/test_shm_sub_rebuild_concurrency --gtest_filter='RouteRebuild.*'

# 逐条定位（C 臂，进程隔离）
./build/bin/test_shm_sub_rebuild_concurrency --gtest_filter='RouteShutdown.*'
```

### 6.4 ctest 接入（照 `test/CMakeLists.txt:134` 的先例）

在 `test/CMakeLists.txt` 的 CTest 区追加（**当前只有 2 项登记，新臂不登记就跑不到**）：

```cmake
add_test(NAME test_shm_route_session COMMAND test_shm_route_session)
set_tests_properties(test_shm_route_session PROPERTIES TIMEOUT 120)

add_test(NAME test_shm_sub_rebuild_concurrency COMMAND test_shm_sub_rebuild_concurrency)
set_tests_properties(test_shm_sub_rebuild_concurrency PROPERTIES TIMEOUT 120)
```

```bash
ctest --test-dir build -R 'test_shm_(route_session|sub_rebuild_concurrency)' --output-on-failure

# 三条臂并行执行（ctest -j 会并行跑已登记的测试）
ctest --test-dir build -R 'test_shm_(route_session|sub_rebuild_concurrency|control_scheduler)' \
      -j3 --output-on-failure
```

### 6.5 建议先跑基线（确认没把现有 SHM 回归弄红）

```bash
cmake --build build --target test_dzipc_shm test_shm_receiver_cap test_chunk_hold \
                        test_shm_control_scheduler -j"$(nproc)"
ctest --test-dir build -R 'test_shm_control_scheduler|test_udp_port_boundary' --output-on-failure
./build/bin/test_dzipc_shm --gtest_filter='DzIpcShm.*' --gtest_brief=1
```

### 6.6 TSAN（可选，**有重大限制**）

仓库**无** sanitizer 构建配置。若新增：

```bash
cmake -S . -B build_tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build_tsan --target test_shm_sub_rebuild_concurrency -j"$(nproc)"
TSAN_OPTIONS='halt_on_error=0 second_deadlock_stack=1' \
  ./build_tsan/bin/test_shm_sub_rebuild_concurrency
```

⚠️ **限制**：`libipc` 大量使用**无锁共享内存 + 跨进程原子**（`rd_waiter_`、`circ` 环、`id_pool`），TSAN 对这类代码会产生**大量误报**（它看不到跨进程的 happens-before）。⇒ **TSAN 结果只能当线索，不能当门**；且必须给 `libipc` 加抑制文件。不建议把 TSAN 纳入阶段 2 的验收门。

---

## 7. 风险与残余

| # | 风险 | 影响 | 处置 |
|---|---|---|---|
| R1 | **当前析构顺序与需求相反**（`:542-578` 先 join 再 disconnect） | C 臂 `DestructWithRecvBlockedJoinsFast` 现在**必然红**（耗时 ≈50ms） | 这正是阶段 2 要改的点；实现前先把该用例写成红，作为改动的前置证明 |
| R2 | `ReceiveLease` 硬绑具体类型 ⇒ 替身注入不进去（§5） | A/B 两臂退化成挂钟判据，必然 flaky | 实现前采纳 §5 的缓解 1+2 |
| R3 | D 臂「最终收到消息」在修复前后都为真 | 假绿 | 判据落在**投递计数**与**调用顺序**上，不落在功能结果上 |
| R4 | `file(GLOB)` 无 `CONFIGURE_DEPENDS` | 新增 `.cpp` 不重跑 cmake ⇒ **旧二进制假绿** | 每次改测试源码后强制 `cmake -S . -B build`；读结果前比对二进制 mtime 与源码 mtime |
| R5 | 挂钟判据（50ms 唤醒）受调度抖动污染 | 偶发红/假绿 | 多次取**最小**值；配合「与超时参数无关」的第二判据 |
| R6 | `recv(50)` 的唤醒依赖 `disconnect`/`quit_waiting` | 若平台唤醒不可靠 ⇒ 用例挂死 | 所有集成用例外层套硬超时（ctest `TIMEOUT` + 子进程 `run_in_child` 兜底） |
| R7 | 阶段 3 的 `process_received_buffer` 与本阶段都改订阅循环 | 两边同时改会冲突 | 阶段2 文档 §3 已划界：阶段 2 只拥有 `recv` 前后的 lease，阶段 3 只拥有 `raw_data` 非空之后的函数体。D 臂的判据落在**入队结果**上，对两者都不敏感 |

### 明确不做

- 不改 `RouteSession` 源码（本任务边界）。
- 不改 `libipc` 的 `recv` 实现、不改队列、不改 nodelet 注册键。
- 不把 `recv(50)` 改无限等待（需求 §9 明确排除，须等阶段 4 的叫醒测过）。
- 不用「最终又能收到消息」作为 D 臂判据（需求 §8-2 明确禁止）。

---

## 8. 交付清单（实现后应产出）

| 文件 | 动作 | 归属臂 |
|---|---|---|
| `test/test_shm_route_session.cpp` | 新增（A 臂，7 条用例） | A |
| `test/test_shm_sub_rebuild_concurrency.cpp` | 新增（B/C/D 臂，约 9 条用例） | B/C/D |
| `test/CMakeLists.txt` | 追加 2 条 `add_test` + `TIMEOUT 120` | — |

**验证顺序**：先跑 §6.5 基线确认没弄红现有 SHM 用例 → 再跑 §6.2 A 臂 50 次 → 再跑 §6.3 集成臂 → 最后 §6.4 ctest。
