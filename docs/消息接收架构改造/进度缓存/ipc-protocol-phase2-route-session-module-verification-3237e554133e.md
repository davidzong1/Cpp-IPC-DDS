# 阶段 2 · RouteSession 独立模块 + 单测 —— 交付与验证报告

任务：`cpp_ipc_team-1790157343181417249-1`（分工表 A 项）
角色：ipc-protocol（IPC协议与数据编解码工程师）
文档依据：`docs/消息接收架构改造/阶段2_RouteSession实现说明.md` §2-§6、§7-A、§8

---

## 0. 结论摘要

| 项 | 结果 |
|---|---|
| §2 接口签名 | 7/7 与说明**逐字符一致**（含 `noexcept`、`const`、参数类型） |
| 单测 | **13/13 PASSED** |
| 稳定性 | 全套 **10 连跑 0 失败** |
| 目标构建 | rc=0，**0 error 0 warning** |
| 头文件自包含 | rc=0 |
| `.cc` 严格告警（`-Wall -Wextra -Wpedantic -Wshadow`） | 0 warning |
| `ctest` | **3/3 passed**（含本用例） |
| ASan（`-fsanitize=address -DNDEBUG`） | 13/13 PASSED，**0 报告** |
| 变异矩阵 | 11 个变异体：**10 个被红灯抓住**，1 个为**等价变体**（已证明，见 §5） |
| 邻接回归 | 7 个 SHM 相关测试全 rc=0，共 47 条 OK |

---

## 1. 交付物

| 文件 | 行数 | md5 | 状态 |
|---|---|---|---|
| `include/dzIPC/shm_route_session.h` | 138 | `31a09c203935f2d6ae41ad1cac496ccf` | 新增（未跟踪） |
| `src/dzIPC/shm_route_session.cc` | 168 | `5706eec2398d09f1827f21b05d20b731` | 新增（未跟踪） |
| `test/test_shm_route_session.cpp` | 588 | `47254650132e82587285ae65a87df9e5` | 新增（未跟踪） |
| `test/CMakeLists.txt` | — | — | `+13` 行（CTest 注册） |

`.cc` 落在 `src/dzIPC/` 下，被既有 `aux_source_directory(${LIBIPC_PROJECT_DIR}/src/dzIPC SRC_FILES)`
（`src/CMakeLists.txt:14`）收编 ⇒ **无需改 `src/CMakeLists.txt`**，与说明 §1 一致。
已确认 `build/src/CMakeFiles/ipc.dir/build.make:324-328` 有该 `.cc` 的编译规则，
`nm -DC build/lib/libipc.so | grep -c RouteSession` = **9**。

---

## 2. §2 接口逐条比对（说明 → 头文件）

| 说明 §2 签名 | 说明 | 头文件 |
|---|---|---|
| `std::optional<ReceiveLease> acquire_receive();` | 1 | 1 |
| `void release_receive() noexcept;` | 1 | 1 |
| `void begin_rebuild(uint32_t new_generation,` | 1 | 1 |
| `void stop_and_wake() noexcept;` | 1 | 1 |
| `void wait_quiescent();` | 1 | 1 |
| `std::shared_ptr<ipc::route> current_route() const;` | 1 | 1 |
| `uint32_t generation() const;` | 1 | 1 |

`struct ReceiveLease { std::shared_ptr<ipc::route> route; uint32_t generation{0}; }`
与说明 §2 一字不差。内部状态成员（`mtx_`/`cv_`/`route_`/`generation_`/
`receive_inflight_`/`stopping_`/`rebuilding_`）与说明 §2 完全对应，**无增删**。

---

## 3. 13 个用例 → 任务点名的覆盖

任务点名 4 类场景，逐条落点：

| 任务要求 | 用例 |
|---|---|
| 空状态 | `EmptyStateIsWellDefined`(:128) |
| 重建中拒绝 acquire | `RebuildRejectsNewAcquire`(:224) |
| inflight 未归零不 release | `RebuildWaitsForInflightBeforeReleasingOldRoute`(:271) |
| stop/wake 可解除阻塞 recv | `StopAndWakeUnblocksBlockedRecv`(:409) |

补充覆盖（说明 §8 其余验收点）：`AcquireLeaseCarriesRouteAndGeneration`(:150)、
`WaitQuiescentBlocksWhileInflightNonZero`(:172)、`ExtraReleaseDoesNotUnderflowQuiescence`(:198)、
`RebuildSwapsRouteAndPublishesNewGeneration`(:332)、`RebuildFailureLeavesEmptyAndRetryable`(:357)、
`StopThenSuccessfulRebuildReopensLeases`(:387)、`StopThenQuiesceReturns`(:487)、
`DtorReleasesOnlyItsOwnShare`(:515)、`ConcurrentAcquireReleaseWithRebuildIsRaceFree`(:540)。

### 判据为何不依赖时序余量

- 「release 必须先于 create」用**因果序探针**：`create` 回调是 §4 第 6 步的唯一入口，
  它内读到的 `released` 标志与旧 route `valid()` 构成因果断言（:301-304）。
- 「叫醒而非超时兜底」用**数值边界**：`wake_ms < 1000` 且 `recv_elapsed < 1500`
  对 `recv(2000)` 超时（:461-465）。
- 所有等待都经有界 `wait_for`（:61），无 `sleep` 式断言。

---

## 4. 验证命令与结果

```
cmake -S . -B build                                   # rc=0
cmake --build build --target ipc -j$(nproc)           # rc=0
cmake --build build --target test_shm_route_session   # rc=0，0 warning
LD_LIBRARY_PATH=build/lib:build/bin ./build/bin/test_shm_route_session
    → 13/13 PASSED（1314 ms）
ctest --test-dir build --output-on-failure
    → 3/3 passed（test_udp_port_boundary 4.05s / test_shm_control_scheduler 3.56s / test_shm_route_session 1.32s）
```

严格告警与自包含：

```
g++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -I include -I src \
    -fsyntax-only src/dzIPC/shm_route_session.cc          # rc=0，0 warning
printf '#include "dzIPC/shm_route_session.h"\n' | g++ -std=c++17 -Wall -Wextra -I include -fsyntax-only -x c++ -
                                                          # rc=0（头文件自包含）
```

稳定性：`SameRound*` 同族与全套各 **10 连跑 0 失败**（`for i in $(seq 1 10)`）。

ASan：`-O1 -g -fsanitize=address -DNDEBUG` 链接产品 `.cc` ⇒ 13/13 PASSED，
`ERROR: AddressSanitizer` 计数 **0**；`acquired=11864 rejected=13063`（churn 用例真跑到）。

邻接回归（`LD_LIBRARY_PATH=build/lib:build/bin`）：

| 测试 | rc | OK |
|---|---|---|
| `test_shm_receiver_cap` | 0 | 2 |
| `test_dzipc_shm` | 0 | 10 |
| `test_dzipc` | 0 | 8 |
| `test_dzipc_pub` | 0 | 16 |
| `test_uf003_crash_reclaim` | 0 | 2 |
| `test_handshake_probe` | 0 | 6 |
| `test_shm_domain_isolation` | 0 | 3 |

---

## 5. 变异矩阵（判据承重性）

在 `/tmp/rs_mut/` 的**产品 `.cc` 副本**上注入变异，链接**未改动的**测试文件重编运行。
仓库文件零改动。11 个变异体：

| # | 变异（破坏的不变式） | 结果 | 抓它的用例 |
|---|---|---|---|
| M1 | 第 4 步不等待 `receive_inflight_==0` | **红灯** rc=134 | `RebuildRejectsNewAcquire` :233「rebuilder 未等待在途 recv」 |
| M2 | `acquire_receive` 忽略 `rebuilding_` | **红灯** rc=124 挂死 | `RebuildRejectsNewAcquire`（inflight 被多占，`join` 永不返回） |
| M3 | 去掉下溢保护 | **红灯** rc=124 挂死 | `ExtraReleaseDoesNotUnderflowQuiescence` :194 |
| M4 | 失败时不清空 `route_` | **红灯** rc=124 | `RebuildFailureLeavesEmptyAndRetryable` :348/:350 |
| M5 | 第 5 步不 `release` 旧 route | **红灯** rc=1，9 处 Failure | `RebuildWaitsForInflight…` :303/:309、`RebuildSwapsRoute…` :327 |
| M6 | 成功重建不复位 `stopping_` | **红灯** rc=1 | `StopThenSuccessfulRebuildReopensLeases` :383 |
| M7 | `stop_and_wake` 不 `disconnect` | **红灯** rc=1 | `StopAndWakeUnblocksBlockedRecv`（2001 ms ≈ 超时兜底） |
| M8 | `acquire_receive` 不递增计数 | **红灯** rc=134 | `WaitQuiescentBlocksWhileInflightNonZero` |
| M9 | `release_receive` 不 `notify_all` | **红灯** rc=124 挂死 | 多个等待用例 |
| M10 | 整句删掉 `rebuilding_` 复位 | **红灯** rc=1，37 处 Failure | 8 个用例 |
| M11 | 失败时**保留** `rebuilding_==true` | **全绿** | —— 见下「等价变体」 |

### 等价变体证明（M11）

M11 把第 7 步改成 `rebuilding_ = (rebuilding_ && !created)`，于是**失败的**重建
之后 `rebuilding_` 保持 `true`。这是**语义等价**，不是判据盲点：

`rebuilding_` 只有一处被读（`acquire_receive` 的拒绝条件 `stopping_ || rebuilding_ || !route_`）。
失败路径上 `route_` 已被第 5 步 `reset()` 成空 ⇒ `!route_` 已为真 ⇒ 返回值恒为
`nullopt`，与 `rebuilding_` 取何值**无可观测差异**。其余接口（`begin_rebuild` 第 1 步
重新置位、`stop_and_wake`、`wait_quiescent`）均不读 `rebuilding_`。

因此"失败后 `rebuilding_` 必须复位"这条**在接口层面不可判定**，说明 §4 该要求的
真实作用是"不留 `rebuilding_==true` 且 `route_` 非空的卡死态"——而那个状态需要
`route_` 非空，即 M4 覆盖的情形（M4 红灯）。

> 结论：11 个变异体中 10 个被抓住，第 11 个已证明等价。判据无未覆盖的
> **可观测**行为。

---

## 6. 边界与未做

1. **本任务（A 项）只交付 RouteSession 本体与单测**。工作区里
   `include/dzIPC/shm_pub_sub_ipc.h` / `src/dzIPC/shm_pub_sub_ipc.cc` 已被改动
   （14 处 `route_session_` 引用、`channel_mtx_` 已移除）——那是 **B/C/D 分工**，
   由队友 `ipc-transport` 于 18:28/18:43 完成（其 deliverable
   `ipc-transport-phase2-shm-sub-ipc-route-session-integration-e48a4c1f331e.md`）。
   本任务文件时间戳为 18:17，**未触碰** `shm_pub_sub_ipc.*`。
2. `RouteSession` 本体**不调用** `recv`/`send`（`grep -cE '\->(recv|send)\('` = 0），
   只依赖 `shm_route_session.h` + `<cassert>` + `<utility>` ⇒ 数据面零耦合。
3. **已上报的既有缺陷（本阶段未修）**：`disconnect()` 叫醒的 `recv` 返回的是
   **零填充**而非空 buffer（size=64=`ipc::data_length`、全零、`empty()==false`；
   机理 `src/libipc/ipc.cpp:1044` 的 `wait_for` 在 `quit_waiting()` 后直接 `break`
   而未填充 `msg`）。收包循环只判 `raw_data.empty()`（`shm_pub_sub_ipc.cc:836`），
   零填充缓冲过不了这一关；当话题 `msg_id == 0` 时 `AcceptWire` 的 `check_id`
   （比对缓冲尾部 4 字节，全零 ⇒ 0）恰好通过 ⇒ 可能投递一条全零假消息。
   本测试只守住更弱的界「返回值不含非零负载」，**不断言**该缺陷行为。
4. `test/CMakeLists.txt:128` 的注释仍写「阶段 1 控制面调度器聚焦用例 (16 条)」，
   实际为 **20 条**（阶段 1 的既有笔误，非本任务范围，未改）。
5. 首次 `cmake --build build`（全量）曾因外部生成器正在重写
   `include/ipc_msg/std_msgs/*` 的瞬时窗口失败于 `test_generated_headers.cpp`
   （fatal error: `ipc_msg/std_msgs/std_point_cloud.hpp: 没有那个文件或目录`，
   该头文件 mtime 在失败前后各变一次）。**与本改动无关**，重跑即 rc=0。
6. 工作区存在**并发写者**：`test/test_shm_route_session.cpp` 在本轮被外部进程
   追加过注释块（19:18，关于零填充叫醒伪影），本 agent 的 `#include <functional>`
   /`<stdexcept>` 补正（19:16）仍在。最终 md5 `47254650132e82587285ae65a87df9e5`
   已连续采样不变。
