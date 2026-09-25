# 阶段 2 RouteSession 改动 · 独立只读验收（v4）

- 任务：`cpp_ipc_team-1790173044725027950-1`
- 角色：IPC 测试与性能工程师
- 性质：**只读验收**。本轮未修改仓库任何文件；`git status --porcelain` 前后逐字一致，6 个产物 sha256 与开工时相同（§7）。
- 对照基准：`docs/消息接收架构改造/阶段2_RouteSession实现说明.md` §2–§8。
- 与 v3 `a6af71011e60` 的关系：v3 已给"可合入"结论。本轮是**新一轮独立重跑**，并在任务点名的「disconnect 零填充伪影」上取得**新证据：该伪影在运行期真实可达，且是本次改动引入的行为变化**（§4）。v3 只把它记为"既有缺陷、本阶段未修"，未证明可达性。

---

## 0. 结论摘要

| 检查项（任务点名） | 判定 |
|---|---|
| 并发重建（§4 顺序 / I2 / I3） | ✅ 实现正确；单元层承重；**集成层第 3 步 `disconnect` 零守门**（X1） |
| `stop_and_wake` | ✅ 实现正确、幂等、非终态；单元层承重；**集成层零守门**（X3） |
| 析构顺序（§5 八步） | ✅ **逐条对齐**，含第 8 步"再释放队列"（成员声明逆序） |
| **`disconnect` 零填充伪影** | ❌ **P0：运行期可达，且为本次改动引入**（§4） |

**验收判定：功能实现正确、可合入；但 §4 的伪影回归必须在阶段 3 动收包路径之前处置** —— 它是"静默数据污染"型缺陷，13 套件 / 79 条既有回归**全绿**，任何现有自动化判据都抓不到。

---

## 1. 只读边界与工作树核实

| 文件 | git | sha256（前 12） | 本轮 |
|---|---|---|---|
| `include/dzIPC/shm_route_session.h` | `??` | `b90664af8bb6` | 未改 |
| `src/dzIPC/shm_route_session.cc` | `??` | `a602ec459ecf` | 未改（`grep -c MUTANT` = 0） |
| `test/test_shm_route_session.cpp` | `??` | `e3091b3f08bc` | 未改 |
| `test/CMakeLists.txt` | `M` | `87b082768956` | 未改 |
| `src/dzIPC/shm_pub_sub_ipc.cc` | `M` | `b6eeb7687d3d` | 未改（+69/−32） |
| `include/dzIPC/shm_pub_sub_ipc.h` | `M` | `88cf3fbf2f85` | 未改 |

```
$ git status --porcelain          # 与开工时逐字一致
 M include/dzIPC/shm_pub_sub_ipc.h
 M src/dzIPC/shm_pub_sub_ipc.cc
 M test/CMakeLists.txt
?? include/dzIPC/shm_route_session.h
?? src/dzIPC/shm_route_session.cc
?? test/test_shm_route_session.cpp
```

**本轮全部变异体、探针、对照库均在 `/tmp/rs_acc/`**，仓库内零 scratch 残留（`git status --ignored` 只余开工前既有的 `build/`、`.vscode/` 等）。
**会话环境干净**：`LD_LIBRARY_PATH=[/opt/openrobots/lib:/opt/openrobots/lib:]`，`ldd build/bin/test_shm_route_session` 确认加载 `build/lib/libipc.so.3`（v3 §1 的污染陷阱本轮不复现）。

---

## 2. 按现有构建配置重跑聚焦构建 / CTest

```
$ cmake -S . -B build            # file(GLOB) 无 CONFIGURE_DEPENDS ⇒ 必须重跑
$ cmake --build build --target test_shm_route_session -j4
[100%] Built target test_shm_route_session          # 无重编 ⇒ 二进制由当前源码产出
$ touch src/dzIPC/shm_route_session.cc && cmake --build build --target ipc -j4
告警行数: 0
```

| 项 | 结果 |
|---|---|
| 聚焦用例 | **14/14 PASSED**，连跑 5 次 rc=0（各 ≈1315 ms） |
| shuffle 5 种子（1/42/1234/99999/7） | **5/5** rc=0 / 14 PASSED |
| 逐条隔离跑（`--gtest_filter` 单跑 14 条） | **失败数 0** |
| CTest 全套 | **3/3 PASSED**（udp 4.05s / control_scheduler 3.56s / route_session 1.32s） |
| CTest 注册 | `add_test(test_shm_route_session)` + `TIMEOUT 120` 已在 `test/CMakeLists.txt:147-150` |
| 既有 SHM 回归 | **13 套件 / 79 条全绿**（见 §6） |
| SUT 编译告警 | **0** |

时间链一致：`shm_route_session.cc` → `libipc.so.1.3.0` → `test_shm_route_session` 依次更新，`--target test_shm_route_session` 无重编。

---

## 3. 三条检查项逐条判定

### 3.1 并发重建（§4 七步顺序 / I2 / I3）

`src/dzIPC/shm_route_session.cc:45-120` 与说明 §4 逐步对齐：

| 步 | 说明要求 | 实现 | 判定 |
|---|---|---|---|
| 1 | 锁内 `rebuilding_ = true` | `:52-53` | ✅ |
| 2 | 拷旧 shared_ptr，放锁 | `:54-55` | ✅ |
| 3 | 锁外 `disconnect()`（叫醒卡住的 recv） | `:59-62` | ✅ 正确 / ⚠️ 零守门（X1） |
| 4 | 锁内等 `inflight == 0` | `:65-69` | ✅ |
| 5 | 仍在锁内 `release()` + `reset()` | `:74-78` | ✅ |
| 6 | **锁外** `create()`（可阻塞） | `:86-101` | ✅ 含 catch-all，不向外抛 |
| 7 | 锁内发布新 route/generation + 复位 `rebuilding_` | `:103-119` | ✅ |

- **I2（不并发 release）**：`release()` 只出现在第 5 步（`:76`），此前已在锁内确认 `inflight == 0` 且 `rebuilding_` 已拒绝新 lease ⇒ 无 `release`/`recv` 并发。**单元层承重**（`RebuildWaitsForInflightBeforeReleasingOldRoute` 用 `create` 回调的因果序探针判定，与机器快慢无关）。
- **I3（拒绝新 lease 在先）**：`acquire_receive` 在**同一把锁内**检查 `stopping_ || rebuilding_ || !route_`（`:20`），置位方也在同一把锁内 ⇒ 无窗口。**承重**（`RebuildRejectsNewAcquire` + churn 用例 `acquired≈11860 / rejected≈10682` 双非零，非空跑伪绿）。
- **X1 复核（独立重做）**：`/tmp/rs_acc/cur_repo` 副本删第 3 步 `disconnect`，重编库后用 `LD_LIBRARY_PATH` 覆盖加载 ⇒ **聚焦 14/14 依然全绿**。与 v3 §4.1 一致：**零守门**。根因同 v3：聚焦用例的"在途 recv"是模拟的（`sleep` 自己会结束，不依赖叫醒）。

### 3.2 `stop_and_wake`

- `:122-141`：锁内 `stopping_ = true` + `notify_all`，**锁外** `disconnect()`。与说明 §6"disconnect 放锁外"一致。
- **幂等 + 非终态**：`stop_and_wake` 不清 `route_`（`current_route()` 仍返回旧对象），产品代码三处依赖它（析构 `:560`、`add_peer` 失败 `:681`、离开 Ready `:764`）—— 其中 `add_peer` 失败路径是 `stop_and_wake + wait_quiescent + release(current_route())` 三步收尾，**依赖 stop 之后 `current_route()` 非空**。头文件 `:49-56` 显式裁定，`StopIsIdempotentAndNonTerminal` 守门（`use_count ≥ 1`、重复调用 `current_route().get()` 不变、成功重建后重新放行 lease）。
- **`wait_quiescent` 谓词只等计数归零**（`:152`），不掺 `!rebuilding_` —— 与 §2 字面语义一致，且避免与重建方互等。
- **M-SW 变异（本轮新做）**：`/tmp` 副本删掉 `stop_and_wake` 里的 `disconnect()` 调用 ⇒ 聚焦 `StopAndWakeUnblocksBlockedRecv` **FAILED (2001 ms)**（等满超时）⇒ **单元层有守门**。
- **X3 复核（独立重做）**：删 `shm_pub_sub_ipc.cc:560` 的 `stop_and_wake()` ⇒ 既有 4 套件（`test_dzipc_shm` 10 / `test_shm_nodelet` 13 / `test_wire_accept` 9 / `test_shm_control_scheduler` 20）**全绿** ⇒ **集成层零守门**。
- **判别力对照（复核 v3 §4.3 结论）**：`/tmp/rs_acc/artifact_dtor` 测 `shm_sub_ipc` 析构耗时（n=12，收包线程在 `recv(50)`）：
  - base（会 disconnect）：`min 27 / p50 28 / max 29 / avg 28.2 ms`
  - M-SW（不 disconnect）：`min 27 / p50 28 / max 29 / avg 28.2 ms` —— **逐样本相同**
  ⇒ **在 `recv(50)` 下用析构耗时做判据不可行**，与 v3 独立结论一致。补 X3 守门必须改用**长超时探针**或**因果判据**（如 `stop_and_wake` 调用序 / inflight 归零序），否则必然写成 flaky。

### 3.3 析构顺序（§5 八步）

`shm_sub_ipc::~shm_sub_ipc()`（`src/dzIPC/shm_pub_sub_ipc.cc:542-596`）：

| §5 步 | 实现 | 行 |
|---|---|---|
| 1 注销 `LocalPubSubRegistry` | `unregister_subscriber` | `:548-553` |
| 2 注销收包调度器（阶段 5 前无操作） | — | — |
| 3 注销控制面（`running = false`） | `running.store(false)` | `:559` |
| 4 `RouteSession::stop_and_wake()` | ✅ | `:560` |
| 5 等 `subscribe_thread_` 退出并 join | ✅（不持锁） | `:569` |
| 5' 握手线程 join | ✅ | `:578` |
| 6 `wait_quiescent()` | ✅ | `:588` |
| 7 `release` route | `cur->release()`（先 `valid()` 判） | `:590-595` |
| 8 再释放队列 | **成员声明逆序**：`route_session_`(`:182`) 早于 `msg_queue_`(`:185`)/`view_queue_`(`:186`) ⇒ 队列先析构 | ✅ |

- **§5 第 1 步仍在 stop 之前**：`:548` 的 `unregister_subscriber` 早于 `:560` 的 `stop_and_wake()` ⇒ 快路径发布端在收包停止前已摘除队列，`test_shm_nodelet.cpp:528 PublishAfterSubDestroyNoCrash` 守门。**判定 ✅**。
- **析构线程未持 RouteSession 锁时 join**：`stop_and_wake` 与 `wait_quiescent` 各自在调用内取放锁，`join` 处无锁 ⇒ 与说明 §5 末段一致。
- **`release()` 顺序正确**：`begin_rebuild` 第 3 步 `disconnect()` 时 `valid()` 仍为 true（`release()` 在第 5 步），所以 disconnect 有效；析构路径 `stop_and_wake` 的 disconnect 同理。`route::disconnect()` 内部有 `if (!valid()) return;`（`include/libipc/ipc.h:231-237`），`release()` 之后调用是 no-op —— 当前顺序避免了这一点。

---

## 4. ❌ P0 新发现：`disconnect` 零填充伪影在运行期可达（本次改动引入）

> **缺陷标记**：`DEFECT-ZEROFILL-ARTIFACT` · 影响任务 `cpp_ipc_team-1790173044725027950-1` / `cpp_ipc_team-1790166119985865896-8`
> 任务点名的「disconnect 零填充伪影」检查项，结论为 **运行期真实可达 + 本次改动引入的回归**。

### 4.1 现象（改前 / 改后对照，全程**不发送任何真实消息**）

判据设计：探针全程不发真实消息 ⇒ 队列里出现**任何**消息都只能是叫醒伪影。先用一条真实消息（`width=42`）证明链路确实连通，再统计伪影。

| 加载的库 | 真实消息 | 零填充伪影 | 重复性 |
|---|---|---|---|
| `build/lib`（当前工作树） | 1 | **1** | 5/5 轮均为 `伪影=1` |
| `/tmp/rs_acc/head_build/lib`（`git archive HEAD` 重编） | 1 | **0** | 5/5 轮均为 `伪影=0` |

- 多轮版（6 轮 publisher 析构 / 60 条真实消息）：改后 **6 条伪影（1 条/轮）**，改前 **0 条**；各重复 3 次结果一致。
- 伪影形态：`msg_id=0 width=0 height=0 data=0`，即**全零 `StdImage`**，被 `try_get_clone()` 正常取出。
- **落点**：`clone` 队列（`get_clone`/`try_get_clone`）；`view` 队列 = 0（伪影不是 DZFlat 段）。

### 4.2 因果归属（窗口隔离 + 变异钉死）

`/tmp/rs_acc/artifact_paths` 把两个 disconnect 触发点分到互不重叠的窗口：

| 窗口 | 对应代码路径 | 伪影数 |
|---|---|---|
| `A_leave_ready`（pub 析构后空窗） | `shm_pub_sub_ipc.cc:764 stop_and_wake()` | **7** |
| `B_rebuild`（pub 重建后） | `shm_route_session.cc:59-62 begin_rebuild` 第 3 步 | 0 |

**变异钉死**：`/tmp` 副本把 `stop_and_wake()` 内的 `disconnect()` 删掉（M-SW）⇒ 同一探针 **伪影 = 0**（2/2 轮）⇒ 因果链确认由 `stop_and_wake` 的 disconnect 产生。

### 4.3 机理链（逐行）

1. `stop_and_wake()` → `route::disconnect()`（`shm_route_session.cc:139`）
2. `chan_impl::disconnect()` → `info_of(h)->disconnect_receiver()`（`ipc.cpp:1196-1206`）
3. `disconnect_receiver()` = `que_.disconnect()` + `this->quit_waiting()`（`ipc.cpp:1112-1117`）⇒ `rd_waiter_.quit_waiting()` ⇒ `quit_ = true`（**sticky，不会自动复位**，`waiter.h:27/66`）
4. `wait_for` → `waiter.wait_if(pred, tm)`：谓词是 `!quit_ && pred()`（`waiter.h:107-110`）⇒ `quit_` 为真时**短路**，`que->pop(msg, ...)` **从未被调用**
5. 但 `wait_if` 返回 **true**（跳出 while 后 `return true`，`waiter.h:120`）⇒ `wait_for` 认为"等到消息了"
6. `msg` 仍是第 1661 行的**值初始化** `queue_t::value_t msg{}` ⇒ `remain_ == 0`
7. `r_size = ipc::data_length(64) + 0 = 64 > 0`（`ipc.cpp:1689-1697`）⇒ **返回 64 字节全零 buff**
8. `buffer::empty()` = `(ip==nullptr)||(ip->p_==nullptr)||(ip->s_==0)`（`buffer.cpp:91`）⇒ `size=64` ⇒ **false**
9. 收包循环 `if (raw_data.empty()) continue;`（`shm_pub_sub_ipc.cc:836`）**不生效**
10. `looks_like_dzflat`（全零，magic 不符）与 `has_dzflat_magic` 均为 false ⇒ 落入 TLV 分支
11. `AcceptWire` → `sink.check_id(raw, expected_msg_id)`：读**尾部 4 字节**（`ipc_msg_base.hpp:35-46`）⇒ 全零 = `0`
12. 当 `expected_msg_id == 0` 时 **`check_id` 恰好通过**、`deserialize_ok()` 为 true ⇒ **全零假消息 `push` 进 `msg_queue_`**（`shm_pub_sub_ipc.cc:978-983`）

**单元层复现**（`/tmp/rs_acc/artifact_route`，只驱动 RouteSession，无 `shm_sub_ipc`）：
```
[RouteSession.stop_and_wake] recv_ms=200 wake_ms=0 size=64 empty=0 all_zero=1 data_length=64
[verdict] 叫醒返回 非空零填充（收包循环会继续分流）
```
**准入复现**（`/tmp/rs_acc/artifact_probe`）：
```
[rs_acc_wake]    disconnect=1 recv_ms=200 wake_ms=0  size=64 empty=0 all_zero=1
[rs_acc_timeout] disconnect=0 recv_ms=500 wake_ms=-1 size=0  empty=1          ← 只有超时返回空
[accept:zero_id0]  size=64 fill=0x00 expected_msg_id=0  check_id=1 AcceptWire=1 deserialize_ok=1
[accept:zero_id77] size=64 fill=0x00 expected_msg_id=77 check_id=0 AcceptWire=0
```

### 4.4 改前为何不出现（说明这是本阶段**故意引入**的新场景）

- 改前收包循环在 `channel_mtx_` 内调 `subscriber_->recv(50)`（`HEAD:792-797`），而握手线程的 `release()`/`disconnect()` 也要拿这把锁（`HEAD:642/657/671/735`）⇒ **recv 与 disconnect 互斥**，"disconnect 叫醒在途 recv"这一场景被锁挡掉了。
- 改前析构路径的 `disconnect()` 在**两个 join 之后**（`HEAD:575`，join 在 `:559`/`:569`）⇒ 那时收包线程已退出，永远没有在途 recv。
- 本阶段为消除"锁被 `recv(50)` 占住"（说明 §0 后果②）**故意**把 disconnect 移到锁外、移到 join 之前（§4 第 3 步、§5 第 4 步）⇒ 叫醒在途 recv 成为常规路径，伪影由此进入运行期。

**RouteSession 本身无过错**：它的契约（头文件 I1–I5）不涉及 recv 返回值的形态；`release_receive()` 也在 `recv` 返回后立即调用（`shm_pub_sub_ipc.cc:835`），顺序正确。缺陷落在**"叫醒后 recv 返回值形态"与"收包循环准入判据"的接口假设不匹配**上。

### 4.5 影响面

| 维度 | 结论 |
|---|---|
| 触发条件 | `stop_and_wake()` 时有**在途 recv** ⇒ 每次 stop 恰好 1 条（stop 后 `acquire_receive` 被拒，后续循环走 `sleep(50)` 分支，不再 recv） |
| 受影响话题 | **`msg_id == 0`**：`TopicData(topic)` / `TopicDataPtrMake<T>()` 的默认值 ⇒ 假消息进 `msg_queue_` 被用户 `get_clone`/`try_get_clone` 消费 |
| `msg_id != 0` 话题 | `check_id` 失配 ⇒ 静默丢弃（不投递，但白走一次分流） |
| `view` 队列 | 不受影响 |
| 现有自动化判据 | **全部抓不到**：13 套件 / 79 条全绿。既有用例只断言"能收到带 marker 的那条"（`test_dzipc_shm.cpp:265-275 wait_for_marker`），不断言消息总数；`PubSub` 只断言 `subscriber_count > 0`。`test_dzipc_shm.cpp:242 SubscriberRecoversAfterPublisherRestart` 用 `msg_id=0` + 重启，**恰好走在风险面上却依然绿** |
| 运行期触发路径 | publisher 退出/崩溃/重启 ⇒ 控制面离开 Ready ⇒ `shm_pub_sub_ipc.cc:764 stop_and_wake()`（常态运维事件，非边缘 case） |

### 4.6 修复线索（本轮**未**实施，属产品侧决策）

在 `/tmp/rs_acc/artifact_fix` 上探测了可用的公开判据：
```
[初始]          valid=1 connected_id=1 recv_count=1
[disconnect 后] valid=1 connected_id=0 recv_count=0     ← 可用于区分
[叫醒 recv]     size=0 empty=1                          ← 注意：本探针此次走的是空返回
```
- **首选**：修 `libipc` 叫醒路径 —— `wait_if` 短路时不应让 `pop` 缺席却返回 `r_size = data_length`；叫醒应返回空 `buff_t`（与超时路径一致）。这是**最小且最彻底**的改法，一次性消除所有上层受害点。
- **兜底**（不改 libipc）：收包循环在 `raw_data.empty()` 之外，增加"叫醒伪影"判据，例如 `lease->route->valid()` 或 `connected_id() == 0` ⇒ `continue`。但注意 **`route::release()` 之后 `disconnect()` 是 no-op**（`ipc.h:231`），判据必须放在 release 之前。
- **不建议**：把判据写成"`size == ipc::data_length && 全零`" —— 它会把合法的全零真消息（`msg_id=0` 的默认构造消息）一并丢掉，且依赖 libipc 内部实现细节。

---

## 5. X1 / X2 / X3 复核（独立重做）

| 编号 | 变异 | 单元层 | 集成层 | 判定 |
|---|---|---|---|---|
| X1 | `shm_route_session.cc:59-62` 删 `disconnect` | 14/14 全绿 | — | **零守门**（同 v3） |
| X2 | 收包循环加 `generation` 判断 | — | — | **当前实现正确**：静态确认收包循环体内无 `generation` 判断（唯一命中是 `:824` 的注释），符合 §3/I5。属"靠无人写该判断成立"，非测试守住 |
| X3 | `shm_pub_sub_ipc.cc:560` 删 `stop_and_wake()` | 聚焦全绿 | 4 套件全绿 | **零守门**（同 v3） |
| M-SW | `RouteSession::stop_and_wake` 内删 `disconnect` | **FAILED `StopAndWakeUnblocksBlockedRecv` (2001 ms)** | 4 套件全绿 | 单元层有守门 / 集成层零守门 |

X2 的窗口机制（引用 v3 §4.2，本轮静态复核一致）：`release_receive()` 的 `notify_all` 正是唤醒 `begin_rebuild` 第 4 步的信号，rebuild 线程可在收包线程读 `generation()` 之前推进 `generation_` ⇒ 窗口非空但极窄，端到端 6 轮 / 360 条无丢包差异。

---

## 6. 回归与稳定性（全部实测，干净环境 `env -u LD_LIBRARY_PATH`）

```
聚焦:   14/14 PASSED ×5 连跑；shuffle 5 种子 5/5；逐条隔离 14/14
CTest:  3/3 PASSED
既有:   13 套件 / 79 条全绿
```

| 套件 | 通过 | 套件 | 通过 |
|---|---|---|---|
| `test_dzipc_shm` | 10 | `test_dzflat_rx` | 8 |
| `test_wire_accept` | 9 | `test_shm_domain_isolation` | 3 |
| `test_nodelet_switch` | 8 | `test_dzipc_larger_data_shm` | 1 |
| `test_shm_nodelet` | 13 | `test_chunk_hold` | 3 |
| `test_adopt_loan_quota` | 5 | `test_shm_ser_cli_nodelet` | 5 |
| `test_dzflat_transport` | 9 | `test_shm_receiver_cap` | 2 |
| `test_lap_safety` | 3 | **合计** | **79 / 0 fail** |

⚠️ 注意：这 79 条**全绿**正是 §4 缺陷的隐蔽性证据 —— 伪影不崩、不报错、不改变"能收到消息"这一断言。

---

## 7. 边界声明

- **未修改仓库任何文件**：`git status --porcelain` 前后逐字一致；6 个产物 sha256 与开工时相同；`grep -c MUTANT` 在仓库内为 0。
- 所有变异体、探针、HEAD 对照库均在 `/tmp/rs_acc/`，通过 `LD_LIBRARY_PATH` 覆盖 `RUNPATH` 加载；`/dev/shm` 无 `rs_` 前缀残留（探针自带 `clear_storage`）。
- `/tmp/rs_acc/head_repo` + `head_build` 由 `git archive HEAD` 导出并重编（`LIBIPC_BUILD_TESTS=OFF`），作为"改前"对照；已用 HEAD 头文件与当前头文件分别编译探针，排除类布局差异（`sizeof(shm_sub_ipc)` 336 → 408）带来的干扰，两组结论一致。
- **全仓无 sanitizer 配置** ⇒ 本报告的"无数据竞争"级结论是"计数精确归零 + 无挂死 + shuffle 稳定"，**不是 TSAN 证明**。
- 本轮**未**验证 §8.1 的 `add_peer` 失败重试 / `cc_id == 0` / 控制面离开 Ready 三条分支的行为一致性（无直接用例，同 v3 §3）；也未评估 §4 伪影对**下游用户代码**（如 `data3` 遍历、schema 校验）的二次影响 —— 只证明它作为一条全零 `StdImage` 进入了 `msg_queue_`。

---

## 8. 复现命令

```bash
# 1) 聚焦构建 + 用例（清掉会话级 LD_LIBRARY_PATH，v3 §1 的陷阱）
cmake -S . -B build && cmake --build build --target test_shm_route_session -j4
env -u LD_LIBRARY_PATH ./build/bin/test_shm_route_session
env -u LD_LIBRARY_PATH ctest --test-dir build -R test_shm_route_session --output-on-failure

# 2) §4 伪影：改前 / 改后对照（探针源码 /tmp/rs_acc/artifact_e2e3.cpp）
cd /tmp/rs_acc
env -u LD_LIBRARY_PATH ./artifact_e2e3 rs_demo_after     # ⇒ 真实消息=1 零填充伪影=1
env -u LD_LIBRARY_PATH ./artifact_e2e3_head rs_demo_before # ⇒ 真实消息=1 零填充伪影=0

# 3) §4.2 窗口归属（A_leave_ready vs B_rebuild）
env -u LD_LIBRARY_PATH ./artifact_paths rs_demo_paths 4    # ⇒ 离开Ready窗口=7 重建窗口=0

# 4) 机理链单点复现（无 shm_sub_ipc）
env -u LD_LIBRARY_PATH ./artifact_probe                     # 叫醒=64B全零 / 超时=空
env -u LD_LIBRARY_PATH ./artifact_route                     # RouteSession 层同样复现
```

探针清单（全部在 `/tmp/rs_acc/`）：
`artifact_probe.cpp` 叫醒 vs 超时 + 准入 · `artifact_route.cpp` RouteSession 层复现 · `artifact_sticky.cpp` quit_ sticky · `artifact_e2e3.cpp` 改前/改后伪影计数 · `artifact_e2e5.cpp` 量化（1 条/轮） · `artifact_paths.cpp` 窗口归属 · `artifact_dtor.cpp` §8.3 判别力 · `artifact_fix.cpp` 修复线索 · `artifact_e2e6.cpp` `msg_id=77` 对照 · `artifact_get.cpp` view/clone 队列归属。
