# 阶段 2 · shm_sub_ipc 接入 RouteSession —— 改造点与验证

任务：`cpp_ipc_team-1790158528986172073-4`
范围：在 `RouteSession` 模块与测试完成后接入 `shm_sub_ipc`（阶段 2 说明 §1 表格第 3/4 行、§4、§5）。
**未扩展阶段范围**：不提取 `process_received_buffer`（阶段 3）、不引入线程池/`recv_wait_set`（阶段 4/5）。

---

## 0 改动文件（3 个，全部落在阶段 2 说明 §1 表格内）

| 文件 | 动作 | 规模 |
|---|---|---|
| `include/dzIPC/shm_pub_sub_ipc.h` | `+ #include "dzIPC/shm_route_session.h"`；成员 `std::shared_ptr<ipc::route> subscriber_` + `std::mutex channel_mtx_` → `RouteSession route_session_` | +9 / −2 |
| `src/dzIPC/shm_pub_sub_ipc.cc` | 析构 §5 顺序；`sub_handshake()` 五处 `channel_mtx_`；订阅循环 lease + 锁外 `recv` | +76 / −34 |
| `test/CMakeLists.txt` | `add_test(test_shm_route_session)` + `TIMEOUT 120` | 队友并发写入，非本任务改动 |

`git diff --stat`（本任务两文件）：

```
 include/dzIPC/shm_pub_sub_ipc.h |   9 +++-
 src/dzIPC/shm_pub_sub_ipc.cc    | 101 +++++++++++++++++++++++++++-------------
```

---

## 1 改造点（文件:行）

### 1.1 头文件

- `include/dzIPC/shm_pub_sub_ipc.h:182` —— `RouteSession route_session_;`
  取代 `subscriber_` + `channel_mtx_`。`channel_mtx_` 已**整体删除**（全仓 `grep channel_mtx_` 现仅剩 `shm_route_session.h` 的历史注释）。收包路径不再持有任何互斥量。

### 1.2 析构（`shm_sub_ipc::~shm_sub_ipc`，阶段 2 说明 §5）

| 说明 §5 步骤 | 落点 |
|---|---|
| 1 注销 `LocalPubSubRegistry`（停收包之前） | `.cc:546-554`（原样保留） |
| 3 注销控制面（= 让 `sub_handshake` 看到 `running == false`） | `.cc:559` `running.store(false)` |
| 4 `RouteSession::stop_and_wake()` | `.cc:560` |
| 5 join `subscribe_thread_` → join `sub_handshake_thread_` | `.cc:565-582` |
| 6 `RouteSession::wait_quiescent()` | `.cc:587` |
| 7 `release()` route | `.cc:588-593` |

要点：
- `running = false` 只能让循环在 `recv` 返回后退出；卡在 `recv(50)` 里靠 `stop_and_wake()` 的 `disconnect`/`quit_waiting` 叫醒，**不依赖 50ms 超时**（说明 §5 第 4 步）。
- 析构线程**不持** `RouteSession` 锁时 join（否则收包线程的 `release_receive` 会与 join 死锁）。
- 第 7 步用 `current_route()` 的**拷贝**调 `release()`：两个线程都已 join ⇒ 无在途 `recv`，`release()` 与 `recv()` 不并发（说明 §4 第 5 步 / I2）。
- 顺带把两个 `std::thread*` 在 `delete` 后置 `nullptr`（防御性收紧，防未来改成分段析构时 double-free；行为不变）。

### 1.3 `sub_handshake()` 五处 `channel_mtx_`

| 现状（HEAD） | 改为 | 落点 |
|---|---|---|
| ① Ready 且 generation 变化：锁内 `release` + 新建 `route` | 一次 `begin_rebuild(generation, create)`，`create` = `make_shared<ipc::route>(topic_name_, ipc::receiver, verbose_)` | `.cc:666-673` |
| ② `add_peer` 失败：锁内 `disconnect` + `reset` | `stop_and_wake()` + `wait_quiescent()` + 拷贝 `current_route()` 后 `release()`，**不建新对象** | `.cc:681-689` |
| ③ 读 `connected_id()` | `begin_rebuild` 返回后 `current_route()` 拷贝上读 `connected_id()`（此刻无并发 release） | `.cc:699-700` |
| ④ 控制面离开 Ready：锁内 `disconnect` + `reset` | 与 ② 同处置 | `.cc:764-771` |
| ⑤ 订阅循环持锁 `recv` | 见 §1.4 | `.cc:826-836` |

`cc_id == 0`（连接位耗尽）路径**未动**：仍不置 `handshake_completed`、退掉已登记 peer、重试（`shm_defect_fixes.md` 第 2 条的黑洞修复保持不变）。

### 1.4 订阅循环（阶段 2 说明 §3）

```text
lease = route_session_.acquire_receive()      // .cc:826
若无 lease：sleep 50ms（与未握手时同口径），continue   // .cc:827-834
buff_t raw_data = lease->route->recv(50);     // .cc:835 —— 不持有 RouteSession 锁
route_session_.release_receive();             // .cc:836 —— 无论 buffer 是否为空
if (raw_data.empty()) continue;
<分流代码一行未动>
```

- `lease` 的 `shared_ptr` 在 `recv` 全程保活 route（I1）。
- `release_receive()` 在 `recv` 返回后**立刻**调用，先于任何分流/`continue` 出口 ⇒ 每次成功 `acquire` 恰好一次 `release`（I4）。
- **未**因 `lease.generation` 落后于当前 generation 丢 buffer（I5）—— 字节已从旧 route 弹出，丢掉即丢消息。
- 原代码 `if (!subscriber_) continue;`（忙等自旋）改为无 lease 时 `sleep 50ms`，与说明 §3 字面要求一致。

---

## 2 语义不变性核对（任务要求「分流、队列、nodelet 语义不变」）

| 面 | 结论 |
|---|---|
| 分流 | `raw_data` 非空之后的**每一行未动**（`looks_like_dzflat`/`has_dzflat_magic`/adopt 配额/`AcceptWire`/`NoteDzFlatRx` 计数）。符合阶段 3 说明「两边都改订阅循环时，阶段 2 只拥有 recv 前后的 lease」。 |
| 队列 | `msg_queue_` / `view_queue_` / `adopt_cap_` / `adopt_borrowed_` / evict 回调注册点全部未动。 |
| nodelet | `LocalPubSubRegistry` 的 `register_subscriber`/`unregister_subscriber` 与 `msg_id_` 重登记逻辑未动；注销仍在停收包之前。 |
| 控制面 | `peer_slot_` 登记/心跳/回收逻辑未动；`add_peer` 失败与离开 Ready 的「清空 route 且不建新对象」与说明 §4 表格一致。 |
| 重建成功复位 `stopping_` | `begin_rebuild` 成功时复位 `stopping_ = false`（`shm_route_session.cc:114`），否则 `add_peer` 失败重试路径会让该话题收包**静默停摆**（头文件「接口语义裁定」）。 |

---

## 3 验证证据

环境：`build/`（Release，`LIBIPC_BUILD_TESTS=ON`，`LIBIPC_BUILD_PYTHON=OFF`），GCC 11.4 `gnu++17`。

### 3.1 构建

```
cmake -S . -B build            → rc=0
cmake --build build -j$(nproc) → rc=0（全量 100%）
cmake --build build --target ipc -j4 → 无 warning / 无 error
```

`src/dzIPC/shm_route_session.cc` 已被既有 `aux_source_directory(${...}/src/dzIPC)` 收编（`build/src/CMakeFiles/ipc.dir/dzIPC/shm_route_session.cc.o` 在位，`build.make` 命中 13 处）—— 无需改 `src/CMakeLists.txt`。

### 3.2 CTest（`ctest --output-on-failure`）

```
1/3 test_udp_port_boundary      Passed  4.05s
2/3 test_shm_control_scheduler  Passed  3.56s
3/3 test_shm_route_session      Passed  1.32s
100% tests passed, 0 failed out of 3
```

### 3.3 聚焦回归（SHM 收包 / 握手 / 重建 / 生命周期）

| 用例 | 结果 |
|---|---|
| `test_shm_route_session`（13 条，lease 配对 / 重建顺序 / stop-wake） | PASSED 13 |
| `test_dzipc_shm`（含 `SubscriberRecoversAfterPublisherRestart` = 真重建路径） | PASSED 10 |
| `test_shm_nodelet` | PASSED 13 |
| `test_shm_receiver_cap`（`cc_id == 0` 连接位耗尽） | PASSED 2 |
| `test_dzflat_rx` | PASSED 8 |
| `test_shm_domain_isolation` | PASSED 3 |
| `test_wire_accept`（含真 SHM pub/sub 喂计数器） | PASSED 9 |
| `test_uf003_crash_reclaim` | PASSED 2 |
| `test_uf009_graceful_exit` | PASSED 3 |
| `test_uf004_shutdown_monitor_optout` | PASSED 7 |
| `test_shm_sniffer_control_name` | PASSED 3 |
| `test_handshake_probe` | PASSED 6 |

合计 79 条断言全绿，无 SKIP。

---

## 4 风险与遗留

1. **`add_peer` 失败 / 离开 Ready 路径会在握手线程上同步阻塞**：`stop_and_wake()` + `wait_quiescent()` 等的是「当前 `recv(50)` 被 `disconnect` 叫醒」的时间。原实现是抢 `channel_mtx_`（可能等满 50ms 超时）；现在上限变成「一次被叫醒的时间」，**不差于原实现**，但仍是阻塞点。阶段 1 调度器若把 `sub_handshake` 迁到 tick 回调上，须注意这条（阶段 1 说明 §8 R3 已记录同类代价）。
2. **析构期间可能多创建一次 route**：若 `sub_handshake` 恰在 `begin_rebuild` 第 4 步等待时 `running` 变 false，它会完成 `create` 并复位 `stopping_`，随后 while 退出；析构在 join 之后 `wait_quiescent()` + `release()` 收尾。无 UAF、无泄漏，仅多一次 `shm_open`。不属阶段 2 射程。
3. **`recv` 换线程仍未做**：`recv_cache()` 是 `thread_local`，换线程属阶段 5，且必须整条 route 固定同一 worker（说明 §6）。本阶段未动。
4. **`recv(50)` 仍是有限超时**：说明 §9 明确不在本阶段改成无限等待。

---

## 5 一处既有测试缺陷（已闭环，非本改动引入）

`RouteSession.StopAndWakeUnblocksBlockedRecv` 原先断言「被 `disconnect` 叫醒的 `recv` 必须返回空缓冲」。该前提被 libipc 实测**证伪**：

- 探针（`disconnect()` 叫醒一次阻塞中的 `recv(2000)`）：返回 `empty=0 size=64 data!=nullptr`，前 16 字节全 `00` —— 即 `ipc::data_length`(64) 字节的**零填充** buffer；
- 只有**超时**路径返回空 buffer（`wait_for` 返回 false ⇒ `return {}`）。

**归属证据（决定性）**：把 HEAD 版 `shm_pub_sub_ipc.{h,cc}`（未接入 RouteSession）+ 当前 `shm_route_session.*` + 该测试源码放进 `/tmp` 独立构建树（`/tmp/rsbaseline2`），同一编译器下该用例**同样失败**（`recv_empty.load() == 0`）⇒ 与本次接入改动无关，是测试对 libipc 行为的错误断言。

该断言已被队友并发修正（`test/test_shm_route_session.cpp` 18:41 改为记录 `recv_size`/`recv_all_zero` 而非断言空缓冲），修正后 13/13 通过。本任务**未触碰**该测试文件。

（同批 `/tmp` 基线还用于排除 `test_wire_accept` 一次核心转储：那是旧二进制与新 `libipc.so` 混用；重编后 PASSED 9。基线构建树亦复现了 `test_generated_headers` / `ipc_benchmark` 的编译错误，同样与本次改动无关。）
