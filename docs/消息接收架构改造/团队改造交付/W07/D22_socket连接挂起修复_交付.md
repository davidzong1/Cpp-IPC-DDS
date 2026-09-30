# D-22：socket 千路端口碰撞挂起修复（有界重试 + 可识别诊断）—— t29 交付

> 任务：`t29`（队长裁决 **D-22**，回应 t11/W10 未满足条件 1，**阻塞级**）；attempt_id `3c13e252-40a9-4387-b83d-393bd09ab573`；
> 负责人：socket与数据面负责人。
> 证据目录：`artifacts/perf/20260929-r32-W10-portfix/`（**新 run_id**，⛔ 未覆盖任何既有 run；含 `.log` / `src/` / `counterfactual/`）。
> 上游证据：`artifacts/perf/20260929-r25-W10/socket-ind-1000.hang.log`、`.../port-conflict/`、`docs/.../W10/W10_交付.md` §5.1。

---

## 0. 一分钟结论

| 项 | 结果 |
|---|---|
| **挂起是否消除** | ✅ 不再挂起。同一场景（1000 独立话题、含真实端口碰撞）由 **rc=124（无限期挂住）** 变为**有界时间内结束**：要么逐 route 全通，要么在 ~29 s（默认 30 次 × 1 s）后**显式失败并给出可识别原因** |
| **实际有效收发** | ✅ **端口段避让后 1000/1000 路逐 route 合法收包**（`routes_ok=1000 routes_lost=0`，逐 route `rx=1/1`） |
| **碰撞时的行为** | ✅ 逐 route 显式失败 + 逐条结构化终报；**失败是局部的**（同进程其它话题照常收发，实测 `good_route_rx=1`） |
| 诊断可识别性 | ✅ 五类 errno 分类（`port_in_use` / `permission_denied` / `address_not_available` / `no_route` / `fd_exhausted` / `other_errno`）+ 端口/组地址/次数/耗时/first_errno/建议 |
| 上限可配置 | ✅ `DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS`（默认 30；`=0` ⇒ **显式**保留旧的无界语义并首报一行说明） |
| ⛔ 端口公式 | **未改**（遵 D-22 第 2 条）。`udp.h` 本次**零改动**（指纹与开工前一致） |
| `ctest -j4` | ✅ **27/27 Passed**（55.74 s，`40_ctest_j4_after_fix.log`） |
| **我发现的两处需上报项** | ① W10 工装 `w10_pick_socket_topics()` 的端口排除**因解析取错列而恒不生效**（§6.1）；② 端口窗口与 `ip_local_port_range` 52.2% 重叠是**设计层冲突**，需产品级决策（§7） |

---

## 1. 现象与根因（复核队长结论，并补两条自有实测）

现象（`socket-ind-1000.hang.log`，404 行）：全部为 `SocketRecvWorker[N]: idle exit` +
`Failed to connect subscriber,reconnect affter 1 second` ⇒ 卡在**连接建立**、route 从未归属。100 路通过、1000 路挂起。

根因三条，逐条复核：

| # | 根因 | 我的复核 |
|---|---|---|
| 1 | `InitChannel` 内 `while (!subscriber_->connect()) { sleep(1s); }` 无上界、无诊断、不可退出 | ✅ 定位到 `socket_pub_sub_ipc.cc:1013-1018`（订阅端）与 `:438-443`（发布端）；⛔ 且**析构/切换路径的 join 同样会被永久阻塞**（同类问题在 `socket_ser_cli_ipc.cc:409-431` 已有先例：那里为可中断加了 `running` 检查） |
| 2 | `UDPNode::connect()` 用 `SO_REUSEADDR\|SO_REUSEPORT` 绑**组地址:PORT**；占用者未开 `SO_REUSEADDR` ⇒ 必 `EADDRINUSE` | ✅ 读源码确认（`udp.h:170-198`：仅 `joins_group()` 时 bind 组地址）；✅ A/B 反事实复跑（占用者不开 `SO_REUSEADDR` 时 `connect()` 必 false） |
| 3 | 碰撞面 50.9–52.0% | ✅ 我独立复算：**52.20%**（`50_port_surface.txt`） |

**我新增的两条实测（原报告没有，但对判据必要）**：

- **(a) `errno` 在 `connect()` 返回 false 之后**仍可读为 `EADDRINUSE`** —— 这是"可识别诊断"成立的前提。
  实测（`errno_preservation.txt`）：`port=47777 connect_ok=0 errno_after=98(Address already in use)`。
  libipc 的 pimpl + `IPC_EXCEPTION_` 包装（`udp.h:65-69`）与内部的 `::close(fd)` **没有**吃掉 errno。
  ⛔ 即便如此，代码仍**紧贴调用**取值（并在调用前置 `errno=0`），不依赖这一"恰好成立"。
- **(b) `zero-length` 组地址的 bind 冲突面**：`UDPNode::connect()` 失败也可能来自**非 errno 原因**
  （pimpl 失效态、`inet_pton` 解析失败路径 —— 这些路径 `return false` 但不设 errno）。
  故分类器把 `errno==0` 归为 `unknown` 而**不是**猜成 `port_in_use`：错分类比不分类更坏。

---

## 2. 修复（最小、不改语义、可回滚）

**改动面：仅 `src/dzIPC/socket_pub_sub_ipc.cc`（+201/−8，最终 sha256 `b1867d0f…`）。`src/libipc/platform/posix/udp.h` 未改动**
（sha256 `226950d4…`，与开工前 `fingerprints_before.txt` 逐字一致 ⇒ 队长给的"若需要"授权**未动用**）。

新增（文件内 `namespace {}` 匿名命名空间，无新增公共 API / 无 ABI 变化）：

| 名字 | 作用 |
|---|---|
| `socket_connect_max_attempts()` | 进程内只读一次的上限（`DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS`，默认 `kSocketConnectAttemptsDefault = 30`） |
| `connect_errno_class(int)` | errno → 六类可识别原因 |
| `connect_remediation(int)` | 按 errno 给**方向正确**的处置建议（⛔ 不给错方向的建议） |
| `connect_with_bounded_retry(...)` | 有界重试 + 日志节流 + 终止结构化报告；**返回 false ⇒ 调用方必须提前返回** |

两处调用点替换（`socket_sub_ipc::InitChannel` 与 `socket_pub_ipc::InitChannel`）：

```cpp
// 之前（无上界、无诊断）：
while (!subscriber_->connect()) { std::cerr << "...reconnect affter 1 second..."; sleep(1s); }
// 之后（有界、可诊断、显式失败）：
if (!connect_with_bounded_retry(subscriber_.get(), running, topic_name_, "SubInfo", "subscriber",
                                ipaddr_, port_hash_))
{
    return;   // ⛔ 不注册 info_pool 条目、不建收包路径 —— "没建起来"必须可见
}
```

**保留原语义的三条**：
1. **重试间隔逐位相同**：仍是 1 s（拆成 10×100 ms 只为让"停止"可打断 —— 同 `socket_ser_cli` 先例）；
2. **首次立即尝试**（不是"先睡 1 s"）；
3. **`running` 为假即中止**：与旧行为只在"能不能退出"上不同，重试时机与次数上限内的行为完全一致。

**兼容路径（显式、不静默）**：`DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=0` ⇒ 无界重试（历史语义），
并在进程内**首报**一行"已显式启用无界重试"（实测见 §4.3）。

**析构安全**：新增的提前 `return` 落在 `catch` 之外、`pool_reg_`/`subscriber_` 尚未建成之前；
`socket_sub_ipc` 析构里 `if (subscriber_)` / `if (ack_tx_)` 已是空指针安全（`teardown_receive_path()` 亦容忍无 state）。
⇒ "显式失败"的对象**可以正常析构**（实测 §4.4 的 39 路 A/B 与 `ctest` 全绿）。

---

## 3. 开关与回滚

| 级别 | 动作 | 效果 |
|---|---|---|
| **L1 调上限**（无需重编） | `DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=<n>` | 默认 30；`=3` 实测 ~2.0 s 失败（§4.3）；`=0` ⇒ 旧的无界语义 |
| **L2 代码回滚** | `git checkout <本提交> -- src/dzIPC/socket_pub_sub_ipc.cc` + 重编 | 回到 `while(!connect()) sleep(1s)`；已留档 `counterfactual/socket_pub_sub_ipc.cc.HEAD`（sha `169dd872…`，= 开工前源码）与修复版 `.D22fix` |
| **L3 接口冻结** | 本次**未**新增/修改任何公共头 | 无 ABI 影响；`pub_sub_base.h` 的 `InitChannel` 签名不变 |

⚠️ **回滚的代价必须写清**：L2 会让千路场景**重新变成 rc=124 挂起**（实测：修复前库 + 同场景 = 无限期挂住，
日志无限增长）。⇒ 只应在"该版本必须与旧版二进制行为逐位一致"时使用。

---

## 4. 验证（修复前后对照 + 最小复现件）

### 4.1 最小复现件（比 1000 路更小，稳定触发）

**`n = 39`（domain=3103，默认命名）**：第 39 条即 topic `w10_socket_independent_3103_38`，
端口 `udp_discovery_port_calculate(...) = 48650` —— 本机被**另一个进程**（uid 65534，非 dzIPC）长期占用。

| n | 修复前 | 修复后 |
|---|---|---|
| 38 | rc=0（不碰撞） | rc=0 |
| **39** | **rc=124 挂起**（25 s 内 25 行重连，永不返回） | **rc=1，~29 s 显式失败**，给出 `port=48650 errno=98(port_in_use)` |

复现命令（`command.txt` 有全量）：
```bash
cmake -S . -B build && make -C build -j16
export LD_LIBRARY_PATH=$PWD/build/lib:$PWD/build/bin
# 修复前 rc=124 / 修复后 rc=1（进程内置探针；两版源码都在 counterfactual/）
timeout 200 ./socket_scale_probe 39 3103 1 naive
```

### 4.2 逐 route 台账（⛔ 不是"只看不挂起"）

自建只读探针 `socket_scale_probe`（`src/socket_scale_probe.cpp`，可独立重编复核）：
建 N 个真实 `socket_sub_ipc` + N 个 `socket_pub_ipc`，**逐 route 发 1 条、逐 route 收**，台账落在每路自己的计数上。

| 场景 | 结果 | 证据 |
|---|---|---|
| **N=1000，端口段避让（scanned）** | ✅ `tx_ok=1000/1000`、**`routes_ok=1000 routes_lost=0`**、端口段零重叠（`slot_clashes=0`） | `22_fixed_n1000_scanned.log` |
| N=500 scanned | ✅ 500/500 | `21_fixed_n500_scanned.log` |
| N=200 scanned | ✅ 200/200 | （同上目录） |
| N=39 **naive（含真实碰撞）** | rc=1：`routes_ok=38 routes_lost=1`，唯一丢失的正是被诊断出的 #38 | `11_fixed_n39_naive.log` |
| **N=1000 naive（原失败场景）** | 有界结束：`routes_ok=994 routes_lost=6`，**6 条丢失 = 6 条被诊断的端口碰撞**（见下） | `30_fixed_n1000_naive.log` |

**逐条吻合（这是"可识别"的关键证据）**：把探针实测丢失的 route 与"本机已占端口"独立复算比对：

```
route=  38 base=48650 hit_offsets=[0]      route= 326 base=33539 hit_offsets=[0]
route= 342 base=36255 hit_offsets=[0]      route= 418 base=48628 hit_offsets=[0]
route= 442 base=53484 hit_offsets=[0]      route= 697 base=53484 hit_offsets=[0]
预期丢失 = [38, 326, 342, 418, 442, 697]   实测丢失 = [38, 326, 342, 418, 442, 697]   一致: True
```
且这 6 条与 W10 当时给出的 `topic_port_clashes.txt`（`38/326/342/418/442/697`）**逐条相同**。

### 4.3 上限可配置 + 兼容路径（A/B）

| 配置 | 实测 |
|---|---|
| 默认（未设） | `attempts=30 elapsed_ms=29018`，日志为 4 行（attempt 1/10/20/30）+ 1 行结构化终报 |
| `=3` | `attempts=3 elapsed_ms=2001`（≈2.0 s 失败，日志 2 行 + 终报） |
| `=0`（显式无界） | 20 s 超时仍挂（**rc=124**）⇒ 旧语义确实被保留，首报"已**显式**启用无界重试" |

日志形态（节流前 3 行 + 终报 1 行，实测 30 次尝试只打 5 行，对比修复前 500+ 行洪水）：

```
[<topic>SubInfo] Failed to connect subscriber (attempt 1/30), reconnect after 1 second:
    port=48650 ip=239.255.213.238 errno=98(port_in_use:Address already in use)
[dzIPC][socket_connect_failed] topic=... role=SubInfo noun=subscriber port=48650 ip=239.255.213.238
    attempts=30 elapsed_ms=29018 first_errno=98(port_in_use:Address already in use) last_errno=98(port_in_use)
    ⇒ 本通道**不可用**, 已显式失败(不再重试); 端口已被占用；建议: ss -ulnp | grep :<port> ...
    如需旧的无界重试: DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=0
```

### 4.4 「显式失败」的外部可判定面（要求 1 的"返回可判定错误"）

`socket_sub_ipc::InitChannel` 的签名是 `void`（`pub_sub_base.h:58` 冻结），⛔ 本轮**未改**接口。
因此"可判定"落在**可观测状态**上，实测四判据（`33_error_surface.log`，`d22_error_surface` 探针）：

| 判据 | 实测 | 含义 |
|---|---|---|
| 失败话题在 `IpcInfoPool` 里**无** `SocketSub` 条目 | `pool_socketsub_bad=0`（对照 `good=1`） | 外部可查台账；"注册成功"不再可能被误报 |
| 失败话题订阅端 `try_get_clone()` 恒 false | `bad_route_rx=0` | 不假装收到 |
| **同进程其它话题照常收发** | `good_route_rx=1` | **失败是局部的，不是整片挂起** |
| 返回耗时落在声明区间 | `bad_init_ms=29016`（29~40 s 判据内） | 有界性可被外部计时验证 |

> ⛔ 我**没有**为"显式失败"新增返回值/异常（会改冻结接口）。若队长认为需要 `InitChannel` 返回
> 状态码，那是一次**接口变更**，须单独评审 —— 见 §7 选项 (D3)。

### 4.5 回归

| 命令 | 结果 |
|---|---|
| `ctest --test-dir build -j4`（27 项） | ✅ **27/27 Passed**（55.69 s，`40_ctest_j4_after_fix.log`） |
| `test_dzipc_socket` / `test_socket_*` 全族 | ✅（含在 27 项内） |

---

## 5. 容量与 fd 边界（要求 4）

本机（32 vCPU，`ulimit -n = 1048576`，`ip_local_port_range = [32768,60999]`）实测：

| N | create_ms（订阅侧 / 发布侧） | fds_open | max_fd | threads | 有效收发 |
|---|---|---|---|---|---|
| 200 | 129（sub 极少；pub 为主） | 868 | 867 | 233 | 200/200 |
| 500 | 6 727（sub 81 / pub 6 646） | 2 068 | 2 067 | 533 | 500/500 |
| 1000 | 132 483（sub **164** / pub **132 319**） | 4 068 | 4 067 | 1 033 | **1000/1000** |

- **fd**：`2.03 fd/route`（订阅+发布合计 4.07/route）—— 与 W07 实测的 `2.07 fd/route` 同档；
  fd 远未触及 `ulimit`，**不是本机瓶颈**。
- **端口**：1000 路占 5000 个端口槽（每条 topic 用 `[base, base+4]`），窗口 54 081 槽 ⇒ **9.2% 占用**，
  本机空闲时**可稳定建立**（实测 1000/1000）。
- ⚠️ **不可消除的环境依赖**：本机有 ~150–180 个 UDP 端口被**其它进程**长期占用（含 uid 65534 的 5 个），
  domain=3103 下与这 1000 路相交 14–15 条 ⇒ **会导致这 14–15 条显式失败**。这是**环境事实**而非本模块缺陷：
  占用者（如内核/NAT/其它服务）是否可让路不由 dzIPC 决定 ⇒ 判据只能是"**可识别失败** + 建议动作"（§4.3 的终报已给）。
- ⚠️ **创建耗时随 N 超线性**（500→6.7 s，1000→132 s）：**反事实证明与本修复无关** ——
  修复前库同场景 `pubs_ms=138 339`、修复后 `pubs_ms=132 319`（**同档**）。
  成本落在**发布端** `discovery_loop()`（每路一条线程，每 50 ms 做一次 O(池规模) 的
  `IpcInfoPool::snapshot()` ⇒ N 条线程 × O(N) 快照 = O(N²)）。这属**既有规模成本**，
  我**未修**（不在 D-22 授权面内），登记为 R-2。

---

## 6. 我发现的两处需上报项

### 6.1 W10 工装 `w10_pick_socket_topics()` 的端口排除**恒不生效**（解析取错列）

`test/perf/w10/w10_matrix.cpp:1176-1190` 逐字复刻并计数（`03_picker_parse_check.txt`）：

```
rows=173 parsed_nonzero=0 used_set_size=1
first_token_sample="145:"          ← 取到的是行首 **slot 号**，不是 local_address
real_occupied=160  real_has_48650=1
topic38_base=48650  picker_would_exclude_it=0
```

⇒ 该函数本意是"避开本机已占端口"，实际只排除了端口 `{0}` ⇒ **冲突话题照旧进入名单**。
这也解释了为什么 W10 的 socket 千路在你我复核时都是"必挂不误"。

⛔ **不在我的授权面内**（`test/perf/**` 属 W10），**我未改**，仅上报。修法一行：把 `is >> addr` 改成
`is >> slot >> addr`（跳过第一列 slot 号）。我的自建探针用的是正确解析，故能把"修复后的有界失败"与
"端口不碰撞时千路能否真的收发"**分开**验证。

### 6.2 队长给的库指纹与现状不符（已查明，非本次改动）

队长给的 `build/lib/libipc.so.1.3.0` = `acde21e6…`（t6 报告）→ 我开工时实测 `dc2d5c5a…`
（期间 t22/W07/W06 等改动入库并重编）。**已在开工前留档**（`fingerprints_before.txt`），非未声明改动。

---

## 7. 设计层冲突与选项（要求 6，⛔ 我不自行扩大改动面）

**冲突事实**（`50_port_surface.txt`）：

```
dzipc_window=[11451,65531] slots=54081
ip_local_port_range=[32768,60999]
overlap=[32768,60999] slots=28232  overlap_ratio=0.5220    ← 52.2%
```

即 dzIPC 的端口窗口有 **52.2%** 落在内核**临时端口池**里。临时端口是**内核动态分配**的：
本工程自己的 `SendOnly` ACK 套接字（`udp.h:169` 明确"不 bind"）就在用它们 —— 于是**本工程能自己撞自己**。
这是**设计层**问题（端口分配策略），D-22 明确不要求本轮解决。可选项（**请你裁决，我不动**）：

| 选项 | 内容 | 代价 |
|---|---|---|
| **D1（推荐，本轮已具备）** | 保持端口公式不变，只保证"碰撞 → 有界 + 可识别"；把避让交给运维（`ip_local_reserved_ports` 为窗口让路 / 换 domain / 换话题名） | 需要 root 或改名；碰撞时仍会有话题不可用（**但可见、可归因**） |
| D2 | 缩窄 `ip_local_port_range`（如 `49152-60999`）或把 dzIPC 窗口移到其外 | 需 root、影响全机其它服务；⛔ 本轮不做 |
| D3 | 让 `InitChannel` 返回状态码 / 抛特定异常，使"显式失败"可被调用方**程序化**判定 | **接口变更**（`pub_sub_base.h` 冻结）⇒ 须单独评审 |
| D4 | 给 `SO_REUSEADDR` 冲突加"端口漂移/回退"（改用另一个端口） | 会**破坏互操作**（对端按公式算端口）⇒ 我不建议 |
| D5 | 让 `SendOnly` 套接字也 bind 到公式端口（消除"自己撞自己"） | 会回到"同机多进程抢固定端口"的老问题（`udp.h:169` 注释已说明为何不 bind） |

---

## 8. 残余

| # | 残余 | 处置 |
|---|---|---|
| R-1 | 端口窗口与 `ip_local_port_range` 52.2% 重叠 | ⚠️ **未修**（D-22 第 2 条明确不要求）；选项见 §7，需产品级决策 |
| R-2 | 发布端创建成本 O(N²)（`discovery_loop` 每 50 ms 全池快照 × N 路）⇒ 千路 build 需 ~130 s | ⚠️ **未修**（不在 D-22 面内）；反事实证明与本次改动无关（修复前 138.3 s vs 修复后 132.3 s） |
| R-3 | W10 工装 `w10_pick_socket_topics()` 解析错列 ⇒ 端口排除不生效 | ⚠️ **未改**（`test/perf/**` 属 W10）；修法一行，见 §6.1 |
| R-4 | `InitChannel` 仍无返回状态（"显式失败"只能靠可观测状态判定） | 见 §7 选项 D3 |
| R-5 | 失败话题不注册 `IpcInfoPool` ⇒ 依赖该池做"是否存在订阅者"判断的调用方会看到 false | ✅ **正确行为**（不假装成功），但调用方需知情 —— 已写入 §4.4 |

---

## 9. 证据索引（`artifacts/perf/20260929-r32-W10-portfix/`）

| 文件 | 内容 |
|---|---|
| `src/socket_scale_probe.cpp` / `src/d22_error_surface.cpp` / `src/port_surface.cpp` / `src/picker_parse_check.cpp` / `src/errno_survive.cpp` | 四个**只读**探针源码（可独立重编复核） |
| `01_baseline_n1000.log` | **修复前**：w10_matrix 千路 rc=124 挂起（复现队长证据） |
| `02_baseline_n38.log` / `02_baseline_n39.log` | **最小复现**：n=38 通过 / **n=39 挂起** |
| `10/11/12_fixed_n39_*.log` | **修复后** n=39：默认上限显式失败 / naive / scanned 全绿 |
| `20/21/22_fixed_n*_scanned.log` | 修复后 1000 / 500 / 200 路**逐 route 台账** |
| `30_fixed_n1000_naive.log` | 修复后**原失败场景**：有界结束，6 条丢失与 6 条诊断逐条吻合 |
| `31_legacy_unbounded_n39.log` / `32_maxattempts3_n39.log` | 兼容路径（`=0` 仍悬挂）/ 上限可配置（`=3` ⇒ 2.0 s） |
| `33_error_surface.log` | 显式失败的四判据（池台账 / 不假装收到 / 局部性 / 有界耗时） |
| `34_domain_*_n39_naive.log` | 运维动作可行性：**换 domain 即绕开**（domain=7 ⇒ 39/39 全绿） |
| `40_ctest_j4_after_fix.log` | 27/27 Passed |
| `41_w10matrix_n1000_after_fix.log` | **原报告场景**跑修复库（rc=124 消失；见 §9.1 说明） |
| `50_port_surface.txt` | 碰撞面复算（52.20% 重叠、本机占用、逐 route 碰撞清单） |
| `03_picker_parse_check.txt` | 工装 picker 解析错列的实测证明 |
| `errno_preservation.txt` | errno 跨 `connect()` 可读性的前提实测 |
| `counterfactual/` | 修复前/后源码留档（`…cc.HEAD` = `169dd872…`、`…cc.D22fix`）+ 两版构建日志 + 修复前库的对照运行 |
| `manifest.json` / `command.txt` / `source-status.txt` / `build-config.txt` / `binary-fingerprint.txt` / `environment.txt` / `verdict.md` | 方案 §12 要求的元数据 |

### 9.1 ⚠️ `41_*.log` 的读法（⛔ 不夸大）

该文件是**原报告场景**（w10_matrix `--n 1000`，命名未做端口避让）在修复库上的运行：
- **rc=124 的"无限期挂住"消失**：`registered_count=1000/1000`、`create_ms≈390 s` 后进入收发阶段并输出
  `valid_rx_count=997/1000`、`socket_pool_routes_alive=997/1000`；
- 丢的 3 路**正是**被诊断出的 3 条端口碰撞（`38 / 326 / 418`）—— 逐条有结构化终报，⛔ 不是静默丢失；
- **⛔ 但它不是"千路有效收发"的判据**：① 该工装的 socket 分支 `wait_handshake()` **无条件返回 true**
  （`w10_matrix.cpp:416-424`），故 `registered_count` 对 socket 不是有效台账；② 工装的端口避让因 §6.1 失效。
- ⇒ **"1000 路有效收发"这一条我只用自建探针主张**（`22_fixed_n1000_scanned.log`：1000/1000 逐 route OK）。

---

## 10. 自评与未自证项

- 本包主张：**D-22 的"无界挂起 + 不可诊断"已修复**，且在端口不碰撞时**千路逐 route 有效收发**成立。
- ⛔ **未自证**：独立验收（示例由评估方/队长指派）；§7 的设计层决策；R-1/R-2/R-3/R-4 均**未修**。
- ⛔ 本包**不**声称：端口分配策略已改善、千路创建耗时已优化、工装缺陷已修、`InitChannel` 已能返回状态。

---

## 11. 评审要点（给非实现者）

1. **"有界"是否真的成立**：请核 `for (int attempt = 1;; ++attempt)` 的每条出口
   （`last` 分支 / `running` 假 / 成功）是否都能到达，且 `last` 一定在 `max_attempts` 次时命中；
   并核 `=0` 分支为何是"显式无界"而不是"不重试"。
2. **"可识别"是否被夸大**：五类 errno 分类 + `unknown` 兜底是否覆盖了真实的失败面？
   ⛔ 特别核 `errno==0` 的归类（我在调用前置 0，故 `unknown` 不会被误报为 `port_in_use`）。
3. **"不改语义"是否属实**：重试间隔 1 s、首次立即、上限内重试行为是否与旧实现逐位一致？
   ⛔ 唯一差异应是"耗尽后返回 false 且调用方提前返回"。
4. **提前返回是否安全**：`InitChannel` 在 `return` 时
   （`subscriber_` 已建、`pool_reg_` 未建、无收包路径）对象能否正常析构与重复 `InitChannel`？
   请核 §2 末段的三条依据，并可用 `d22_error_surface` 做端到端复现。
5. **是否越界**：`git diff --stat` 应只含 `src/dzIPC/socket_pub_sub_ipc.cc`；
   `udp.h` 与 `test/perf/**` 应**零改动**（后者缺陷见 §6.1，仅上报）。
