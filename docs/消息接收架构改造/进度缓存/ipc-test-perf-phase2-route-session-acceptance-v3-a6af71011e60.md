# 阶段 2 RouteSession 集成只读验收（v3 · 独立复核 + 验收环境校正）

- 任务：`cpp_ipc_team-1790166119985865896-8`
- 角色：IPC 测试与性能工程师
- 性质：**只读验收**。本轮未修改任何仓库文件；`git status --porcelain` 前后逐字一致（§7）。
- 对照基准：`docs/消息接收架构改造/阶段2_RouteSession实现说明.md` §2–§8。
- 与 v1/v2 的关系：v2 `7761923398aa` 已给出同一结论。本轮是**独立重跑**，并纠正了一个会
  使**所有同会话读数失效**的环境陷阱（§1）—— 这不是重复劳动，v2 的读数可信度依赖它。

## 0. 结论摘要

| 项 | 判定 | 证据 |
|---|---|---|
| 聚焦用例 `test_shm_route_session` | **14/14 PASSED**，干净环境 5 连跑全绿 | §5.1 |
| CTest 全套 | **3/3 PASSED** | §5.2 |
| 既有 SHM 回归 | **8 套件 / 69 条全绿** | §5.3 |
| 单元层不变式 I1–I4 | **承重**（sha 未变 ⇒ 既有 13 变异体证据仍有效） | §4.0 |
| §8.2 `begin_rebuild` 第 3 步 `disconnect` | **零守门**（X1） | §4.1 |
| §8.4 已弹出 buffer 不丢失 | **零守门 + 端到端不可自然触发**（X2） | §4.2 |
| §8.3 析构 stop-wake | **实现正确**；集成层**零守门**（X3）；`recv(50)` 下判据**不可用** | §4.3 |
| §8.1 / §8.5 / §8.6 | 覆盖（§8.1 两条分支为间接覆盖） | §3 |

**验收判定：可以合入。** 三条零守门项全部是「**静默失效**」型 —— 破坏了不崩、不报错、现有
14 单测 + 8 套件 SHM 回归**全绿**。建议在阶段 3 动收包路径之前补齐，并按 §4.3 的方法论结论
把 X3 判据改成**长超时探针**而非析构耗时。

## 1. ⚠️ 验收环境陷阱（本轮最有操作价值的发现）

**现象**：本会话进程环境里残留 `LD_LIBRARY_PATH=/tmp/x2v/libX1`（前一轮变异实验的遗留；
不在 `~/.bashrc` / `~/.profile` / `/etc/environment`，仅本会话）。

**后果**：测试二进制的 `RUNPATH` 是 `build/lib`，但 `LD_LIBRARY_PATH` **优先级更高**：

```
$ ldd build/bin/test_shm_route_session | grep ipc          # 带污染的会话环境
    libipc.so.3 => /tmp/x2v/libX1/libipc.so.3              # ← X1 变异库，不是基线
$ env -u LD_LIBRARY_PATH ldd build/bin/test_shm_route_session | grep ipc
    libipc.so.3 => /home/zwc/cpp_ipc_dds/build/lib/libipc.so.3   # ← 基线
```

`/tmp/x2v/libX1` 是 **X1 变异库**（`begin_rebuild` 删掉第 3 步 `disconnect`，见 §4.1）。

⇒ **任何在本会话里直接 `./build/bin/...` 或 `ctest` 的读数都可能是变异库的结果**，
与源码无关。本轮所有结论均已在**干净环境**下重取（`env -u LD_LIBRARY_PATH`）。

**附带收获**：X1 变异库下聚焦用例 **14/14 依然全绿** —— 这本身就是 §4.1「X1 零守门」的
直接证据（一个破坏 §8.2 的变异，聚焦套件毫无反应）。

**纪律建议**：并发/变异实验后必须清理会话级 `LD_LIBRARY_PATH`；报告里引用任何"全绿"
读数时，先 `ldd` 确认实际加载的库。

## 2. 落点与产物核实

| 文件 | git | sha256（前 12） | 核实 |
|---|---|---|---|
| `include/dzIPC/shm_route_session.h` | `??` | `b90664af8bb6` | 138 行；与说明 §2 接口逐字一致；额外显式裁定「`stop_and_wake` 幂等且非终态」 |
| `src/dzIPC/shm_route_session.cc` | `??` | `a602ec459ecf` | 168 行；`aux_source_directory` 自动收编（`build/src/CMakeFiles/ipc.dir/dzIPC/shm_route_session.cc.o` 在列） |
| `include/dzIPC/shm_pub_sub_ipc.h` | `M` | — | `subscriber_` + `channel_mtx_` → `RouteSession route_session_`；`channel_mtx_` 已彻底删除 |
| `src/dzIPC/shm_pub_sub_ipc.cc` | `M` | `b6eeb7687d3d` | +69/−32；收包循环改 lease；`sub_handshake()` 五处持锁点全部改写 |
| `test/test_shm_route_session.cpp` | `??` | `e3091b3f08bc` | 673 行 / **14 TEST** |
| `test/CMakeLists.txt` | `M` | — | +13 行：`add_test(test_shm_route_session)` + `TIMEOUT 120` |

**构建一致性**：源码 20:59:45 → `libipc.so.1.3.0` 20:59:47 → 测试二进制 20:59:50；
`cmake --build build --target test_shm_route_session` 无重编 ⇒ 二进制由当前源码产出。

## 3. §8 验收条目逐条映射

| § | 条目 | 覆盖 | 说明 |
|---|---|---|---|
| 8.1 | 发布/订阅行为一致 | ✅ | `test_dzipc_shm` 等全绿 |
| 8.1 | generation 重建 | ⚠️ 部分 | 单测 `RebuildSwapsRouteAndPublishesNewGeneration`；集成 `test_dzipc_shm.cpp:242 SubscriberRecoversAfterPublisherRestart` 只验「重启后又能收到」，**不验旧 route 是否被正确 release** |
| 8.1 | `add_peer` 失败重试 | ⚠️ 无直接用例 | 实现 `shm_pub_sub_ipc.cc:677-688`；单测 `StopThenSuccessfulRebuildReopensLeases` 只覆盖其**语义前提** |
| 8.1 | 连接位耗尽 `cc_id == 0` | ⚠️ 间接 | `test_shm_receiver_cap` 验控制面 `peer_count ≤ kCap`；**未进 ctest** |
| 8.1 | 控制面离开 Ready | ⚠️ 无直接用例 | 实现 `:759-772` |
| 8.2 | 重建与 recv 重叠 | ⚠️ **有实质缺口** | 见 §4.1 X1 |
| 8.3 | 析构时 recv 正阻塞 | ⚠️ **实现正确 / 零守门** | 见 §4.3 X3 |
| 8.4 | 已弹出 buffer 不丢失 | ❌ **零守门** | 见 §4.2 X2 |
| 8.5 | `LocalPubSubRegistry` 先注销 | ✅ | `:551 unregister_subscriber` 在 `:560 stop_and_wake()` **之前**；`test_shm_nodelet.cpp:528 PublishAfterSubDestroyNoCrash` 守门 |
| 8.6 | 既有 SHM 收包回归 | ✅ | 8 套件全绿（§5.3） |

### 单元层不变式（I1–I5）

| 不变式 | 守门用例 | 判定 |
|---|---|---|
| I1 recv 期间对象存活 | `DtorReleasesOnlyItsOwnShare` | ✅ 承重 |
| I2 不并发 release | `RebuildWaitsForInflightBeforeReleasingOldRoute` | ✅ 承重 |
| I3 拒绝新 lease 在先 | `RebuildRejectsNewAcquire`、`ConcurrentAcquireReleaseWithRebuildIsRaceFree` | ✅ 承重 |
| I4 计数配对（双向） | `WaitQuiescentBlocksWhileInflightNonZero`、`ExtraReleaseDoesNotUnderflowQuiescence` | ✅ 承重 |
| **I5 不丢已弹出字节** | **无** | ❌ **X2** |

头文件 `:19` 自述「每一条都有单测守门」—— 对 **I5 该声明不成立**。

## 4. 缺口（全部变异 + 探针独立实证，均在 `/tmp` 隔离，`LD_LIBRARY_PATH` 覆盖 `RUNPATH`）

### 4.0 已有变异证据的有效性

`/tmp/rs_acc/mut.log`（13 变异体 / 13 KILLED）**仍有效**：变异时源码 sha256 `a602ec45…`
与当前 `.cc` 逐字节一致（本轮复核）。但该 13 个变异体**只覆盖单元层不变式**，不含 X1/X2/X3。

### 4.1 X1 —— `begin_rebuild` 第 3 步 `disconnect` 零守门【最有分量】

**变异**（`shm_route_session.cc:59-62`）：
```diff
-    if (old) { old->disconnect(); }
+    (void)old;  /* MUTANT X1: begin_rebuild step-3 disconnect removed */
```

**判别力探针**（同一二进制 `probe_base`，只换库；真 `recv(2000)` 卡住 → 调 `begin_rebuild`）：

| 加载的 lib | `recv` 返回 | `begin_rebuild` 阻塞 |
|---|---|---|
| `build/lib`（基线） | `size=64 empty=0`（叫醒伪影） | **0 ms** |
| `/tmp/x2v/libX1`（X1 变异） | `size=0 empty=1`（等满超时） | **1800 ms** |

⇒ **因果干净**：删掉第 3 步 ⇒ 卡住的 recv 只能等满超时，`begin_rebuild` 白等 1800ms。

**守门强度**：X1 变异库下 **9 个目标全绿**（`test_shm_route_session` 14/0、
`test_shm_nodelet` 13/0、`test_shm_control_scheduler` 20/0、`test_dzipc_shm` 10/0、
`test_wire_accept` 9/0、`test_shm_ser_cli_nodelet` 5/0、`test_lap_safety` 3/0、
`test_chunk_hold` 3/0、`test_shm_receiver_cap` 2/0）⇒ **零守门**。

**根因**：聚焦用例里的「在途 recv」是**模拟**的（主线程持 lease / `sleep`，`sleep` 自己会
结束，不依赖叫醒）。全仓唯一用真 recv 的 `StopAndWakeUnblocksBlockedRecv` 驱动的是
`stop_and_wake()` 里的 disconnect（`shm_route_session.cc:139`），**不是** `begin_rebuild` 里的这条。

**影响**：§4 第 3 步（重建时叫醒卡住的 recv）实际由**无守门代码**承担。误删后症状是
`begin_rebuild` 白等一次 recv 超时 —— 集成层表现为握手线程被 `recv(50)` 拖住 50ms
（正是本阶段要消除的问题），而全部测试保持绿。

### 4.2 X2 —— §8.4 已弹出 buffer 不丢失：零守门 + 端到端不可自然触发

**静态事实**：`grep -n 'generation' src/dzIPC/shm_pub_sub_ipc.cc` 在收包循环体内**零命中**
（`:824` 只有注释）。实现当前成立 —— 但靠「无人写该判断」成立，不是测试守住。

**变异**（`shm_pub_sub_ipc.cc:835` 后插入）：
```cpp
if (lease->generation != route_session_.generation()) { continue; }  /* MUTANT X2 */
```

**守门强度**：X2 变异库下 **9 个目标全部 rc=0 / FAILED=0** ⇒ **零守门**。

**窗口非空（机制探针 `/tmp/x2r/x2_reach`）**：
```
[x2r] recv_nonempty=1 size=17 lease_gen=1 now_gen=2
[x2r] X2 判断(lease_gen != now_gen) = 1  ⇒ 命中：这块已弹出的非空 buffer 会被 continue 丢弃
```
`release_receive()` 内的 `notify_all` 正是唤醒 `begin_rebuild` 第 4 步 `cv_.wait` 的信号；
rebuild 线程醒来后只需拿锁推进 `generation_`，而收包线程还要从 `release_receive()` 返回再
读 `generation()`。两者竞速，rebuild 可胜出。

**端到端可达性（重要负结果，`/tmp/x2v/loss_probe`）**：

| lib | 6 轮 publisher 重启 / 360 条 | 结果 |
|---|---|---|
| `build/lib`（基线） | 跑 2 次 | received=360, last_seq=359 |
| `/tmp/x2v`（X2 变异） | 跑 2 次 | received=360, last_seq=359 |

**无丢包差异** ⇒ X2 是「窗口存在但极窄」的**静默风险**：一旦有人加这个判断，端到端功能测
（含 `SubscriberRecoversAfterPublisherRestart`）**很难自然暴露**，比 X1 更隐蔽。
这正是需求 §8.2 特别点名「不能只看功能测」的那一条。

**补法**：按 §8.2「可以用测试替身计数」—— 在收包循环注入可观测投递计数，构造
「recv 已返回 → 触发 `begin_rebuild` → 断言该 buffer 仍进 `msg_queue_`/`view_queue_`」。
当前 `ReceiveLease` 硬绑 `std::shared_ptr<ipc::route>`（无虚函数、不可替身），需借
`begin_rebuild` 的 `create` 回调或新增只读访问器。

### 4.3 X3 —— §8.3 集成层零守门；且 `recv(50)` 下该判据**不可用**

**静态事实**：`grep -ln 'stop_and_wake' test/*.cpp` 只命中 `test_shm_route_session.cpp`
（单元层）—— 没有任何用例构造 `shm_sub_ipc` 卡在 recv 再析构。

**变异**（`shm_pub_sub_ipc.cc:560` 删 `route_session_.stop_and_wake();`）：
X3 变异库（`recv(50)`）下 **9 个目标全部 rc=0 / FAILED=0** ⇒ 零守门。

**判别力对照实验**（`/tmp/rs_acc/integ_probe`，n=12，测 `shm_sub_ipc` 析构耗时）：

| lib | 收包 recv 超时 | 析构耗时 |
|---|---|---|
| `build/lib`（基线） | `recv(50)` | min 2 / p50 3 / max 6 / **avg 3.4 ms** |
| `/tmp/x2v/libB`（删 stop） | `recv(50)` | min 2 / p50 4 / max 6 / **avg 3.7 ms** |
| `/tmp/x2v/libA`（**留** stop） | `recv(2000)` | **avg 4.3 ms** |
| `/tmp/x2v/libB`（**删** stop） | `recv(2000)` | min=p50=max=**1550 ms** |

**两个结论**：

1. **叫醒确实生效**：长超时下 4.3ms vs 1550ms，相差 **380×**。1550ms ≈ 2000ms − 探针的
   500ms 等待（收包线程在 t≈0 进入 `recv(2000)`，此后无新数据，等满超时）。
2. **`recv(50)` 下用析构耗时做判据是不可行的**：3.4 vs 3.7 ms，噪声量级内。原因是
   `recv(50)` + 周期性心跳/数据场景下 recv 的兜底等待本来就短（实测 ≈4ms 量级，不是均匀
   相位的 25ms）⇒ 「叫醒」与「超时兜底」的差异被压缩到不可判别。

**补法（必须按此写法，否则会写成 flaky）**：复用 `test_uf004_shutdown_monitor_optout.cpp:89`
的 `run_in_child()`（fork+pipe+poll，8s 硬超时 SIGKILL）构造 `shm_sub_ipc` 卡在 recv 再析构，
**且判据不能挂在 `recv(50)` 上** —— 需借助长超时（4.3ms vs 1550ms 级差异）或改用
「析构完成后 `stop_and_wake` 调用计数 / inflight 归零序」这类因果判据。

## 5. 回归与稳定性（全部实测，**干净环境** `env -u LD_LIBRARY_PATH`）

### 5.1 聚焦用例
```
14 tests from 1 test suite ran. (1313 ms) [  PASSED  ] 14 tests.
连跑 5 次：rc=0 ×5，各 14 OK / 0 FAILED
```

### 5.2 CTest
```
1/3 test_udp_port_boundary ....... Passed  4.05 sec
2/3 test_shm_control_scheduler ... Passed  3.56 sec
3/3 test_shm_route_session ....... Passed  1.32 sec
100% tests passed, 0 tests failed out of 3
```

### 5.3 既有 SHM 回归（未注册进 ctest，需手工跑）
```
test_dzipc_shm rc=0 OK=10 FAILED=0    test_shm_nodelet rc=0 OK=13 FAILED=0
test_shm_receiver_cap rc=0 OK=2       test_wire_accept rc=0 OK=9
test_lap_safety rc=0 OK=3             test_chunk_hold rc=0 OK=3
test_shm_ser_cli_nodelet rc=0 OK=5    test_shm_control_scheduler rc=0 OK=20
```

### 5.4 编译告警
`shm_route_session.cc`：`-Wall -Wextra` **零告警**。
`shm_pub_sub_ipc.cc`：告警全部来自**既有**头文件（`ipc_msg_base.hpp` / `pub_sub_base.h` 的
unused-parameter、`shm_pub_sub_ipc.h` 的 `-Wreorder`），**非本次接入引入**，未扩大基线。

## 6. 风险登记

| # | 风险 | 等级 | 说明 |
|---|---|---|---|
| R1 | X1 `begin_rebuild` 第 3 步无守门 | **高** | 静默；误删后 `begin_rebuild` 白等一次 recv 超时（探针 0ms→1800ms），握手线程被拖 50ms，测试全绿 |
| R2 | X2 §8.4 无守门且端到端不可自然触发 | **高** | 窗口非空（机制探针），但 6 轮重启实测无差异 ⇒ 比 X1 更隐蔽 |
| R3 | X3 §8.3 集成层无回归 | 中 | 实现正确；`recv(50)` 下析构耗时判据不可用（§4.3 已给可用写法） |
| R4 | §8.1 `add_peer` 失败 / 离开 Ready 无直接用例 | 中 | `:677-688`、`:759-772` 是 §4 表格第 2/4 行 |
| R5 | 全仓无 sanitizer | 中 | 「无数据竞争」是「计数归零 + 无挂死 + 5 连跑稳定」级证据，**非 TSAN 证明** |
| R6 | 6 套件未进 ctest | 低 | 只注册 3 个，其余需手工跑（§5.3） |
| R7 | 会话级 `LD_LIBRARY_PATH` 污染 | **中（流程）** | 见 §1；会让同会话的所有测试读数指向变异库 |
| R8 | 全部产物未提交 | — | 3 个 `??` + 3 个 `M`（§2） |

## 7. 边界声明

- 本轮**只读**：未修改任何仓库文件。`git status --porcelain` 前后逐字一致；4 个产物 sha256
  与开工时相同（§2）。
- 所有变异体、探针、变异 lib 均在 `/tmp`（`/tmp/x1v`、`/tmp/x2v`、`/tmp/x2r`、`/tmp/x3v`、
  `/tmp/rs_acc`），通过 `LD_LIBRARY_PATH` 覆盖 `RUNPATH` 加载，**未触碰 `build/` 与仓库源码**。
  仓库无 scratch 残留（`git status` 无 `.log/.out/probe/mut`）。
- `shm_pub_sub_ipc.{h,cc}` 的 101 行接入属 B/C 分工，本轮只做编译自洽、行为一致性核实与
  守门强度验证，**未评审其设计**。
- X2 机制探针用同步 `begin_rebuild` 复刻「rebuild 抢在收包线程读 generation 之前胜出」，
  证明**窗口非空**；它**不证明**自然竞速下必现（与端到端负结果一致）。
- 「无数据竞争」结论不含 TSAN/ASAN 证据（全仓无 sanitizer 构建）。

## 8. 可执行命令

```bash
# ⚠️ 先清掉会话级 LD_LIBRARY_PATH，否则读到的是变异库（见 §1）
env -u LD_LIBRARY_PATH bash

# 0. 新增 .cpp 后必须重跑 cmake（file(GLOB) 无 CONFIGURE_DEPENDS）
cmake -S . -B build

# 1. 聚焦用例
cmake --build build --target test_shm_route_session
env -u LD_LIBRARY_PATH ./build/bin/test_shm_route_session

# 2. 经 ctest（已注册，TIMEOUT 120）
env -u LD_LIBRARY_PATH ctest --test-dir build -R test_shm_route_session --output-on-failure

# 3. 全套已注册 ctest
env -u LD_LIBRARY_PATH ctest --test-dir build --output-on-failure

# 4. 既有 SHM 回归（未注册，需手工跑）
for t in test_dzipc_shm test_shm_nodelet test_shm_receiver_cap test_wire_accept \
         test_lap_safety test_chunk_hold test_shm_ser_cli_nodelet; do
  env -u LD_LIBRARY_PATH ./build/bin/$t || echo "FAILED: $t"
done

# 5. 稳定性（并发用例必做）
for i in $(seq 5); do env -u LD_LIBRARY_PATH ./build/bin/test_shm_route_session || break; done

# 6. 读数前先确认实际加载的库（§1 的教训）
env -u LD_LIBRARY_PATH ldd build/bin/test_shm_route_session | grep ipc
```
