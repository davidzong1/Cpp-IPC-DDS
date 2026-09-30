# W07 socket 容量与高 fd 修复 —— 交付

| 项 | 值 |
|---|---|
| 工作包 | **W07**（P0，socket 容量与高 fd） |
| 负责人 | socket与数据面负责人 |
| attempt | `18582ebb-ed70-4232-8661-6ee2d272c2c1`（task `t8`） |
| 基线 | `e800ccc496ac710b711c9346709e86a148c41241`（2026-09-28） |
| 改动面 | 3 文件、+170/−20：`src/libipc/platform/posix/udp.h`、`include/dzIPC/ipc_info_pool.h`、`src/dzIPC/ipc_info_pool.cc` |
| 独立补丁 | `artifacts/perf/20260928-r01-W07/w07.patch` |
| 证据等级 | **S3 单项验证通过**（可复现命令 + 原始日志 + 反事实）。⛔ 未宣称 S4/S5 —— 独立验收属 W10/REVIEW。 |

改动面指纹（sha256，before → after）：

| 文件 | HEAD（e800ccc） | 现在 |
|---|---|---|
| `src/libipc/platform/posix/udp.h` | `c66aae49b238581b54218e204a41a468cc219b3665ec326ce46d536a2825aedf` | `226950d437e71e7857cf4c7e938565e3c60960bf8b1ed70b79b4394254f27130` |
| `include/dzIPC/ipc_info_pool.h` | `f7461f8a698219f66cec192e1e49b577f3b0eb62d2d2cdca7d3038c0a65d4822` | `3e2825645bf3a32d63caf99d17d96dca8a9ebcb0929a23dd2bf2544579e12da3` |
| `src/dzIPC/ipc_info_pool.cc` | `b366901e1ac106ec3db941acad89b4653e69913c6b4b04474756f6d0a38da2ab` | `7bbb15e86010f1820b400a9d14f9b6cd017aa5f9cc39d192685fa49a3bf1ab4e` |

---

## 0. 一分钟结论

**这是两个彼此独立的缺陷**（要求 1 要求分开定位，本节即分开）：

| # | 缺陷 | 根因（定位） | 修复 | 判决性证据 |
|---|---|---|---|---|
| ① | **注册表容量**：1000 路里约 488 路注册失败 | `kMaxEntries = 512`（`include/dzIPC/ipc_info_pool.h`），而池是**全机共享**的、每个端点占 1 条 ⇒ 1000 路订阅需要 ≥1000 条 | 4096 条 + 段名/布局版本 + attach 拒绝性校验 | `regression/test_ipc_info_pool_layout.log`（1000/1000 注册可见）、`test_socket_high_fd.log`（`registered=1000`） |
| ② | **fd 越界**：fd ≥ 1024 时 SIGABRT（`*** buffer overflow detected ***`） | `src/libipc/platform/posix/udp.h` 的 `UDPNode::receive(tm)` 用 `select`+`FD_SET` 等**一个** fd；`FD_SET(fd≥FD_SETSIZE)` 走 glibc `__fdelt_chk` ⇒ `__chk_fail()` | 同函数改用 `poll(fd, POLLIN, timeout)` | `test_socket_high_fd.log`（`maxfd=1125/1061/2070` 全部 >1024 且收发成功）+ 反事实见 §4 |

**验收**（t8 合同）：

| 验收项 | 结果 | 读数 |
|---|---|---|
| 1000 路**有效注册** | ✅ | `scale=1000 registered=1000`（registered = 本进程在册 SocketSub 条目数；不靠线程数） |
| 1000 路**实际收发** | ⚠️ **只到首条 route** | `sent=20 received=20`，而 `received` 只从 **`subs[0]` 一条 route** 累计（`test/test_socket_high_fd.cpp` 的 `while (subs[0]->try_get(sample)) ++received;`）⇒ 只能支撑"该 topic 的组播链路可通、首条 route 能收到"，⛔**不能**支撑"1000 路每条都实际收发"。按方案 §13.2 第 2 条，**逐 route 收发（valid_rx_count == expected_count 且每 route 序号 + 载荷校验）由 W10 补台账**（已写入 t11 合同）。 |
| 高 fd 不崩溃 | ✅ | 预占 fd 后 `maxfd=1125`（worker）/`1061`（compat），往返 10/10；反事实（还原 select）3/3 FAIL `signal=6` |
| 原 socket 模块回归 | ✅ | `test_socket_wait_set` 9 / `test_socket_recv_worker` 7 / `test_socket_readable` 5 / `test_socket_ser_concurrency` 3，全 OK |

> ⚠️ **本表第 2 行是队长 2026-09-28 现场复核后的降级表述**（原文把两行合并成"1000 路有效注册并实际收发 ✅"）。
> 降级原因与口径见上一段与最后一段；⛔ 本交付**不**声称"1000 路逐 route 收发已验证"。


> **对既有基线读数的一处订正（不影响结论，但影响归因）**：`compat_1000.log` 里**没有** `sent=` 行（该跑在 512 路处就崩了）；带 `sent=0 / received_sub0=0` 的是 `worker_1000.log` 与 `compat_477.log`，而 run_scale_sweep.sh 对这两档**没有传 `--send-msgs`** ⇒ 那两个 0 是"根本没发"的平凡 0，⛔不能当作"注册失败导致收发为零"的证据。真正坐实注册失败的是 `w1000_pub.log` / `worker_1000.log` 的
> `[dzIPC][info_pool] register_entry 失败: 表满且本轮整表回收未找到死条目`（512 槽全活 ⇒ UF-006 的死条目回收救不了）。本交付的 `registered==1000` 断言直接钉在"有效注册数"上，不靠线程数、也不靠那两个平凡的 0。

---

## 1. 分开定位（要求 1）

### 1.1 注册表容量

- 池是**全机一个段**（`dz_ipc_info_pool_*`），每个端点（pub/sub/ser/cli **实例**）注册时占且只占 1 条：`socket_pub_sub_ipc.cc:1040` 的 `pool_reg_.rebind({SocketSub,...})` 在 `InitChannel` 里**每个实例**调用一次 ⇒ 1000 路订阅 = 1000 条。
- 基线 `kMaxEntries = 512`（`include/dzIPC/ipc_info_pool.h`，旧值）⇒ 第 513 路起 `register_entry` 返回 `-1`。UF-006 已有的"同锁内回收死条目 + 一次重试"只对**死**条目有效；1000 路全活时回收扫不到东西，注册**永久失败**。
- 基线实测：`test/perf/out/20260927_t6_fd/w1000_pub.log`、`test/perf/out/20260927_t6_scale/worker_1000.log` 均有上述失败行；后者同时给出 `create_total_ms=123.230 / fds_open=2068 / max_socket_fd=2067`。
- ⛔ 本缺陷**与接收线程数无关**：所以判据取"本进程在册 SocketSub 条目数"，⛔不用"线程数下降"替代。

### 1.2 fd 越界

- `src/libipc/platform/posix/udp.h:345-353`（旧）：`fd_set read_fds; FD_ZERO; FD_SET(server_fd,...); ::select(server_fd+1,...)`。
- `fd_set` 是定长位图；`FD_SET(fd)` 展开为 `__fdelt_chk(fd)`，`fd ≥ FD_SETSIZE(1024)` 时 `__chk_fail()` → `*** buffer overflow detected ***` + `SIGABRT`。
- socket 侧每个订阅者约 2 个 fd（实测 `fds_open / scale ≈ 2.07`）⇒ 512 路上下就越过 1024。
- **既有 gdb 证据**（无需重推）：`test/perf/out/20260927_t6_fd/head_511_gdb.log`
  ```
  #7 __GI___chk_fail ()
  #8 __GI___fdelt_chk (d=<optimized out>)
  #9 ipc::socket::UDPNode::receive(unsigned long)
  #10 dzIPC::socket::(anonymous namespace)::recv_chunk_common_impl(...)
  ```
  同目录 `w511_pub.log`（`maxfd=1089`）、`w1000_pub.log`（`maxfd=2067`）、`compat_1000.log`（第 512 个订阅者后 abort）都停在同一签名上。
- 关键点：该函数**无论后面有没有数据都会先填位图**，所以 worker 路径与 compat 路径都躲不掉（不是"兼容线程才有"）。

---

## 2. select/FD_SET 梳理与迁移（要求 2）

**梳理结论（与合同描述的出入，按实报告）**：合同点名的三个文件**已经不含 select/FD_SET**：

| 文件 | 现状 | 说明 |
|---|---|---|
| `src/dzIPC/threepools/socket_wait_set.cc` | `epoll_create1` / `epoll_wait` | 阶段 5 已迁 epoll |
| `src/dzIPC/socket_ser_cli_ipc.cc` | `udp_node_readable()` = `poll(fd, POLLIN, 0)` | 只做非阻塞可读探测 |
| `src/dzIPC/socket_pub_sub_ipc.cc` | 同上（worker 准入判据走 poll） | — |

全仓剩余的 Linux `select/FD_SET` **只有一处**，在 libipc 平台层：

| 位置 | 现状 | 处置 |
|---|---|---|
| `src/libipc/platform/posix/udp.h` `UDPNode::receive(uint64_t tm)` 的定时等待分支 | 旧：`select`+`FD_SET`（单 fd） | **已迁 `poll`**（`udp.h:360-368`） |
| `src/libipc/platform/win/udp.h`（4 处 `select`+`FD_SET`） | 仍是 `select` | ⛔ 本轮**未动**（见 §7 已知限制） |

迁移写法与**语义逐条对齐**（`poll` 的 timeout 是 `int` 毫秒，原 `timeval` 能装更大的 `tm`，故超 `INT_MAX` 截断、由既有的 `refresh_time` 剩余时间循环续等，**总等待时长不变**）：

| 原 select 分支 | 新 poll 分支 | 语义 |
|---|---|---|
| `ret > 0`（可读） | `ret > 0`（有事件） | 照旧 `recvfrom(..., MSG_DONTWAIT)`；`EINTR/EAGAIN/EWOULDBLOCK` 且 `<10` 次 ⇒ `goto refresh_time`；否则返回空 |
| `ret == 0` | `ret == 0` | 真超时 ⇒ 返回空 |
| `ret < 0 && EINTR && <5` | 同 | 刷新剩余时间重试 |
| `ret < 0` 其他 | 同 | 返回空 |

为什么 `poll` 而不是"先 `recvfrom` 试一次"：`readable()`（`udp.h:521`）已经是 `poll(fd, POLLIN, 0)`，两者同一原语、同一个 `<poll.h>`；单 fd 等待不存在 `poll` 的 O(n) 扫描面。**未新增任何依赖**（`<poll.h>` 原有，新增 `<climits>` 供 `INT_MAX`）。

---

## 3. 容量调整 + 共享布局/版本兼容（要求 3，含队长 D-4 四项硬约束）

### 3.1 约束 1｜容量是启动配置 + 容量表

| 项 | 值 | 依据 |
|---|---|---|
| 默认容量 `kMaxEntries` | **4096**（编译期常量，⛔不随 worker/route 数在运行中变化） | 目标规模 1000 路独立话题 × 两端 = 2000 条，4096 = 2000×2 + 96 余量 |
| 池归属 | **全机共享一个段**；4096 条 = **全机总预算**，不是"每进程 4096" | `kShmName` 单例，`IpcInfoPool::instance()` |
| 每进程预算 | **每端点 1 条**（pub/sub/ser/cli 各实例）；1000 订阅 + 1 发布 = 1001 条 | `socket_pub_sub_ipc.cc:461/1040` 的 `rebind` |
| `sizeof(PoolEntry)` | **296 B**（`static_assert(sizeof(PoolEntry)==kPoolEntryBytes)` 钉住） | `ipc_info_pool.cc:107` |
| 段头 | 56 B（Linux，`offsetof` 断言钉住 0/4/8/12 四个字段偏移） | `ipc_info_pool.cc:103-106` |
| 段总字节 `segment_bytes()` | **1212472 B = 1184.05 KiB** = 56 + 296×4096 | 实测 `probe_capacity.txt` |
| `/dev/shm` 文件实际大小 | 1212476 B（= `calc_size(region)` = region + 4 B 引用计数） | 实测 |
| 1000 路余量 | **3096 条**（旧容量 512 时为 −488） | 实测 |
| **超限时首个失败资源** | **池槽位**：第 4097 次 `register_entry` 返回 `-1` 并打限流诊断（`表满…`）；⛔不是 fd、不是内存 | `register_entry` 语义未动 |

### 3.2 约束 2｜跨进程版本与布局协商（选 (i)，并把 (ii) 的拒绝性校验一并补上）

**为什么"只把 512 改大"不够**（这是必须换段名的硬理由，不是偏好）：本仓 libipc 的
`shm::handle::acquire(name,size)` → `get_mem()` 对**既有**段也会按**调用方请求的 size** 做 `ftruncate`
（`src/libipc/platform/posix/shm_posix.cpp:198-204`），而引用计数 `acc_` 落在**各自 mapped 区间的末尾**
（`shm_posix.cpp:41-43`）。于是同一段名上混跑新旧二进制时：
- 新 size < 旧 size ⇒ 新进程把旧段的映射**截断** ⇒ 旧进程访问尾部 SIGBUS；
- 引用计数落在两个不同地址 ⇒ 一方 `release()` 就可能把另一方还在用的段 **unlink** 掉；
- 新进程按 `kMaxEntries` 遍历旧段的 `entries` 数组 ⇒ **读越界**（2026-09-18 的 UF-006 结论也正是靠
  "⛔不改 `kRegionSize`/`kMaxEntries`/`kShmName`" 才免于决策的）。

**采取的做法**：
1. **(i) 段名带版本后缀**：`kShmName` `dz_ipc_info_pool_v1` → **`dz_ipc_info_pool_v2`**（Windows 具名互斥量同步为 `..._v2_mtx`）；`kLayoutVersion` 1 → **2**；magic 保持 `'DZIP'`。旧二进制只认 v1、新二进制只认 v2 ⇒ **物理隔离，不可能互相 resize/错解**。
2. **(ii) 的 attach 校验也补上**（防御纵深，针对**同名**但布局/容量不符的残留段）：新增公开纯函数
   `layout_mismatch(mapped, mapped_bytes)`（`ipc_info_pool.cc:390`，声明 `ipc_info_pool.h:81`），按
   `段长 → magic → layout_ver → max_entries` 顺序判定，⛔**在首次访问 `entries` 之前**做门禁；不过就整池拒绝
   （`ready=false`、`header/entries=nullptr`）并打**明确**错误：
   ```
   [dzIPC][info_pool] 拒绝使用既有段 dz_ipc_info_pool_v2: 段头 layout_ver 与本版本不符
     (本版本 layout_ver=2 max_entries=4096; 段头 layout_ver=1 max_entries=512)
   ```
   随后 `register_entry` 返回 `-1`，诊断原因新增 `kDiagLayoutMismatch`（表格第 6 项），⛔不再笼统报"池未就绪"。

**混合版本证据**（`test/test_ipc_info_pool_version.cpp`，全部在**全新子进程**里跑，父进程从不构造池，
因而乱序/过滤都安全）：

| 用例 | 造的场景 | 断言 | 结果 |
|---|---|---|---|
| `StaleOldLayoutOnCurrentNameIsRejectedNotMisread` | 在**生产段名** `v2` 上预置旧布局段（旧 size 151608、`layout_ver=1`、`max_entries=512`、`init_state=ready`） | `register_entry < 0`；stderr 含"拒绝使用既有段"且**同时**含 `layout_ver` 与 `max_entries`（可识别） | ✅ |
| `OldGenerationSegmentIsNotTouchedByNewLayout` | 旧代段 `v1` + 新代码跑一次注册 | v1 的**大小与前缀逐字节未变**；新代 `v2` 段被创建；本代注册成功 | ✅ |

**旧段处置 / 是否需重启 / 回滚行为**：
- 旧 `v1` 段：新二进制不再打开它；最后一个使用它的旧进程退出时按既有引用计数 `unlink`。确认无进程映射后可
  `rm -f /dev/shm/dz_ipc_info_pool_v1`；`IpcInfoPool::reset_storage()` 清的是**本代**（v2）段名。
- **滚动升级期间观测面短暂分成两代**（旧二进制 `dzipc info` 只看得到 v1 条目、新的只看得到 v2 条目）——
  这是本方案的已知代价，写在头文件注释里；⛔不存在"同一段上静默错解"。
- 无需重启即可让新二进制生效（它自己建/用 v2）；但**要让一次升级内所有进程口径一致**，应在升级窗口内重启旧进程。
- **回滚**：还原本文件与头文件 ⇒ 重新使用 v1；v2 段在无映射后由 `reset_storage()`/`rm` 清理。旧 v1 段在回滚后可被立即复用（布局未变）。

### 3.3 约束 3｜内存与 fd 预算对账

| 容量 | entries 字节 | 段总字节 | 对 /dev/shm(≤2 GiB) | 对 RSS 增量(≤1 GiB) |
|---|---|---|---|---|
| 512（旧） | 151552 | 151608 B（148.1 KiB） | 0.007% | 0.014% |
| **4096（新）** | 1212416 | **1212472 B（1184.05 KiB）** | **0.056%** | **0.113%** |

实测来源：`artifacts/perf/20260928-r01-W07/probe_capacity.txt`（`segment_bytes()=1212472`、`/dev/shm/dz_ipc_info_pool_v2 size=1212476`、`mapped_bytes_minus_segment=4`）。
布局由编译期断言钉住（`ipc_info_pool.cc:103-107`）：段头四字段偏移 0/4/8/12、`sizeof(PoolEntry)==296`、
`kMaxEntries>=1000`、`kRegionSize < 4 MiB`。
fd 预算：池**不占 fd**（每进程 1 个映射）；增容量不改变 fd 占用（高 fd 场景的 fd 由 socket 侧产生）。

### 3.4 约束 4｜回退与语义保持

- **未改注册/回收语义**：表满 → 同锁内回收死条目 + 一次重试 → 仍失败返回 `-1` 并限流诊断（区分
  池未就绪 / 锁失败 / 表满）——`ipc_info_pool.cc` 的这段逻辑一行未动；`test_ipc_info_pool` 10/10 通过，
  含 `FullTableRegisterReapsDeadChildEntry`、`FullTableFailureStaysObservableAndLeavesSnapshotIntact`。
- 新用例只**追加**公开物：`kLayoutMagic` / `kLayoutVersion` / `kPoolEntryBytes` / `segment_bytes()` / `layout_mismatch()`；
  ⛔未改 `register_entry/unregister_entry/heartbeat/snapshot/gc_dead` 的签名与返回码契约。
- 扩容**不掩盖**注册失败：容量满时仍然 `-1` + 限流诊断，且诊断现在能区分"布局不符"与"表满"。

---

## 4. 边界回归（要求 4）

新增三个用例文件（target 由 `test/` 的 `file(GLOB)` 自动生成；**CTest 登记待架构负责人落地**，见 §7）：

| 文件 | 用例 | 覆盖 |
|---|---|---|
| `test/test_socket_high_fd.cpp` | `RoundTripWithFdAboveFdSetSizeWorkerPath` / `...CompatPath` | **主动预占 fd**（`/dev/null` 顶到 `FD_SETSIZE+32`）⇒ 真实 socket fd > 1024 ⇒ 真实**收发**往返 |
| 同上 | `ThousandLanesRegisterAndRoundTrip` | **1000 路有效注册 + 实际收发**（同进程千订阅 + 1 发布） |
| `test/test_ipc_info_pool_layout.cpp` | `LayoutMismatchRejectsOldVersionOldCapacityAndShortSegment` | 段长/ magic / `layout_ver` / `max_entries` 四类拒绝（纯函数） |
| 同上 | `SegmentBytesMatchPinnedLayoutAndLeaveHeadroom` | 容量表与编译期布局一致 + ≥1000 + 余量 |
| 同上 | `CapacityCoversThousandLanesAndRecovers` | 1000 路注册可见 + **释放后可恢复** |
| `test/test_ipc_info_pool_version.cpp` | 2 用例 | 混合版本拒绝 + 新旧代隔离（§3.2） |

**实测读数**（`artifacts/perf/20260928-r01-W07/test_socket_high_fd.log`）：

```
[W07 worker-path] multicast=1 hogged=1 maxfd=1125 sent=10 received=10
[W07 compat-path] multicast=1 hogged=1 maxfd=1061 sent=10 received=10
[W07 1000-lane]   multicast=1 scale=1000 registered=1000 maxfd=2070 openfds=2071 sent=20 received=20
[  PASSED  ] 3 tests.
```

**反事实（证明用例承重，不是"跑绿即过"）**：把 `udp.h` 还原成 HEAD 的 `select` 版、其余判定不动，
`test_socket_high_fd` **3/3 FAIL**，两个预占 fd 用例报 `signal=6`（SIGABRT，子进程 stderr 为
`*** buffer overflow detected ***: terminated`）——见 `test_socket_high_fd_baseline_mutation.log`；
随后已复原为 `poll` 版并复跑通过。容量侧：基线 512 槽下 `registered` 只能到 512，用例断言
`registered==1000` 即红（基线失败行见 §1.1 的 `w1000_pub.log`）。

**回归全套**（`artifacts/perf/20260928-r01-W07/regression/`）：

| 测试 | rc | OK | FAILED |
|---|---|---|---|
| `test_socket_wait_set` | 0 | 9 | 0 |
| `test_socket_recv_worker` | 0 | 7 | 0 |
| `test_socket_readable` | 0 | 5 | 0 |
| `test_socket_ser_concurrency` | 0 | 3 | 0 |
| `test_ipc_info_pool` | 0 | 10 | 0 |
| `test_ipc_info_pool_layout` | 0 | 3 | 0 |
| `test_ipc_info_pool_version` | 0 | 2 | 0 |
| `test_socket_high_fd` | 0 | 3 | 0 |

**热路径硬闸**（`include/libipc/**` 改动的强制项，改前/改后各一次，读数在
`hotpath_gate_before.txt` / `hotpath_gate_after.txt`）：

| 门 | 改前（19:2x） | 改后（19:4x） | 判据 |
|---|---|---|---|
| 门 1：SHM pub/sub 1 MiB 吞吐（下限 700 msg/s） | 1079 msg/s ✅ | 705 msg/s ✅ | 两次都过；差值归因见下 |
| 门 1：SHM pub/sub 64 B 吞吐（下限 80000 msg/s） | 86051 msg/s ✅ | 170401 msg/s ✅ | 两次都过 |
| 门 2：8 个邻接回归 | 全 OK，过闸 | 全 OK，过闸 | ✅ |

⚠️ **对 1 MiB 那一档 1079→705 的归因（⛔不当作本改动的回归）**：
① 同一轮 64 B 档**翻了近一倍**（86051→170401）——真回归不会出现这种反向变化，这是**同机并发负载**
（本机 load average ≈ 3.0，另有 7 名成员并行跑构建/基准）；
② 本改动的 `udp.h` **不在** `pubsub_shm` 路径上（SHM 路由不经过 `UDPNode::receive`）；
③ 事后用同一命令补采三次 1 MiB：**1034 / 1065 / 1193 msg/s**（`hotpath_gate_after_resample.txt`），
与改前的 1079 同档 ⇒ 705 是负载抖动。⛔ 后续 W10/W11 正式采数应在无并发负载时重采。

---

## 5. 独立工作线声明（要求 5）

**W07 是独立工作线**，只负责 socket 容量与 fd 边界；⛔不扩展《事件驱动线程池需求》首批范围
（不新增接收后端、不改 `RecvWorkerPool`/`SocketWaitSet` 语义、不动 `kMaxWorkerCount`/`RecvBudget`）。
本改动与 W05/W06 的接收池接入正交：`socket_pub_sub_ipc.cc` **一行未动**（高 fd 崩溃点在它下面的
libipc 原语上修，正是为了不与其交叉）。

---

## 6. 证据索引

全部在 `artifacts/perf/20260928-r01-W07/`：

| 文件 | 内容 |
|---|---|
| `command.txt` | 复现命令（构建/单测/反事实/热闸/gdb） |
| `w07.patch` | 独立补丁（3 文件） |
| `fingerprints.txt` | before/after sha256 + diffstat |
| `probe_capacity.cpp` / `probe_capacity.txt` | 容量实测（kMaxEntries/PoolEntry/segment_bytes/shm 文件大小） |
| `test_socket_high_fd.log` | 高 fd + 千路读数组 |
| `test_socket_high_fd_baseline_mutation.log` | **反事实**：还原 select ⇒ 3/3 FAIL signal=6 |
| `udp.h.head_select` / `udp.h.w07_poll_patched` | 反事实用到的两个版本（可复核） |
| `regression/*.log` | 8 个用例文件的逐项 gtest 日志 |
| `hotpath_gate_before.txt` / `hotpath_gate_after.txt` | 热路径硬闸改前/改后读数 |
| `hotpath_gate_after_resample.txt` | 1 MiB 档补采三次（判定 705 msg/s 为负载抖动） |
| `mutation_build.log` / `rebuild_after_restore.log` | 反事实与复原的构建日志 |
| `test_ipc_info_pool_layout.log` / `test_ipc_info_pool_version.log` | 池容量/布局/版本用例日志 |

外部既有证据（不在本目录，只引用不复制）：`test/perf/out/20260927_t6_fd/{head_511_gdb.log,w511_pub.log,w1000_pub.log}`、
`test/perf/out/20260927_t6_scale/{worker_1000.log,compat_1000.log,compat_477.log}`、
`test/perf/out/20260927_t6_probes/{t6scale.cpp,fdprobe.cpp,run_scale_sweep.sh}`。

---

## 7. 已知限制与后续（⛔ 不假装已闭环）

1. **CTest 未登记**：三个新用例 target 由 GLOB 生成但**尚未进 CTest**（`test/CMakeLists.txt` 的写入责任人是架构负责人）。已把精确 `add_test` 行提交给架构负责人；落地后才算"回归常驻"。本交付的读数是**直接跑 `build/bin/<target>`**得到的。
2. **Windows 未动**：`src/libipc/platform/win/udp.h` 仍有 4 处 `select`+`FD_SET`（单 fd，Winsock 默认 `FD_SETSIZE=64`），同样会在高 fd 下失败。⛔本轮**未改也未验证**（Linux 工具链无法编译/运行 Windows 路径；盲改风险大于收益）。该文件应另立条目按同一模式迁 `WSAPoll`。
3. **`test_ipc_info_pool_version` 会临时在生产段名上装陈旧段**（必须如此才测得到），因此⛔不应与其它池用例并行跑；用例结束前已 `unlink` 干净。
4. **socket_ser_cli 未单独加高 fd 用例**：其收包走同一个 `UDPNode::receive`，已被同一修复覆盖、`test_socket_ser_concurrency` 亦通过；但"sercli 在 fd>1024 下的端到端收发"没有专属用例，属可补的缺口。
5. **容量再变更的约束**：若 W09 因背压/容量模型需要再次调整 `kMaxEntries`，必须**沿用同一段名/版本机制**（升 `kLayoutVersion` + 换段名后缀 + 保留 `layout_mismatch` 门禁与混合版本用例），⛔不得另立平行容量常量或段名（已由队长写入 t8/t10 合同）。
6. **升级语义**：滚动升级窗口内新旧代观测面分叉（§3.2），需要运维知晓；本包未提供"自动清理旧段"的工具，只给出 `rm` / `reset_storage()` 两种处置。

---

## 8. 回滚方法

```bash
# 代码回滚（三项一起还原才能回到旧布局/旧段名）
git checkout e800ccc496ac710b711c9346709e86a148c41241 -- \
    src/libipc/platform/posix/udp.h include/dzIPC/ipc_info_pool.h src/dzIPC/ipc_info_pool.cc
cmake --build build --target ipc -j"$(nproc)"
# 段清理：回滚后重新使用 v1；确认无进程映射后清掉本代遗留段
rm -f /dev/shm/dz_ipc_info_pool_v2
```

⚠️ 回滚后**不要**与未回滚的进程混跑：v2 段的引用计数语义仍受 §3.2 的同名 resize 约束，处置方式同"旧段"。
回滚本身可单独定位：`udp.h`（fd 修复）与 `ipc_info_pool.{h,cc}`（容量/版本）可以**分开**还原，二者无耦合。
