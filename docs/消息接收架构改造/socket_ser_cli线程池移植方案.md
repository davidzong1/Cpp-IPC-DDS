# Socket ser-cli 接收线程池移植方案

> 状态：**C1–C3 已落地并验证**（socket_ser_ipc 服务端请求接收接入共享层固定 socket worker）；**C0 的 UDPNode 可等待句柄由 ipc-transport（t2）交付**，C4 本波不做。
> 参照：shm_pub_sub_ipc 已落地的 shared state、处理函数抽取、RouteSession 与安全注销；**共享层追加了 socket 侧固定收包 worker**（`include/dzIPC/threepools/socket_recv_worker.h`，契约 §5，owner = ipc-transport）。
> 依赖的冻结契约：`ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md`（含 §5 socket 侧 + §6 t12 追加的可读判据/字节出口）＋**必读**勘误 `ipc-transport-phase5-shared-wait-layer-contract-errata-d8f45cfa45bb.md`（E1/E2/E3）。
> 实现落点（该模块唯一作者）：`include/dzIPC/socket_ser_cli_ipc.h`、`src/dzIPC/socket_ser_cli_ipc.cc`。
> 范围：socket_ser_ipc 服务端请求接收；socket_cli_ipc 同步响应接收另行评审（C4）。
> ⚠️ **本波未达成项（t11 汇总时不得计入已交付）**：① C4 客户端集中接收**不做**；② 控制面 `server_handshake` 仍 per-service 独立线程（未迁移，同 D7 口径）。详见 §9。

## 1. 当前结构与目标

socket_ser_ipc 在数据面启动时创建 response_thread_，循环调用 chunk_rev_server 收取和组装请求，随后执行 callback 与响应发送。server_handshake 独立管理握手和路径信号。stop_data_plane/restart_data_plane 会关闭并重建数据通道。

socket_cli_ipc 没有独立响应接收线程：send_request 在调用线程发送请求并同步等待响应。请求与响应由调用栈配对，不能简单把 chunk_rev_server 放到共享 worker。

首期将 socket_ser_ipc 的服务端请求接收迁入固定 socket worker，保持每 service 单消费者和 callback 串行。client 同步路径维持现状。

## 2. 从 shm_pub_sub_ipc 复用的机制

**回写更正（t10）**：本段起草期的两处表述已作废，以实现/契约为准：

1. ~~「固定 route ReceiveWorker 尚未在该分支落地」~~ ⇒ 共享层已交付并冻结：SHM 侧 `include/dzIPC/threepools/recv_worker.h`、**socket 侧 `include/dzIPC/threepools/socket_recv_worker.h` + `.cc`**（契约 §5，与 SHM 侧逐条同形，仅等待身份改用 `SocketWaitToken`）。本模块**只消费、不复制**。
2. ~~「固定 worker 和 SocketWaitSet 需要按阶段 5 及本方案新增实现」~~ ⇒ `SocketWaitSet`（`include/dzIPC/threepools/socket_wait_set.h`）与 `UDPNode` 可等待句柄（`waitable()/wait_handle()/readable()/cancel_wait()/clear_wait()` + `data_rev.h` 的 `udp_node_*` 转发，含 t12 追加的 `udp_node_readable`）**均已由 ipc-transport 交付**；本模块 C1 只做**消费**与回归对照。

本模块实际新增的**只有** route 适配器与其注册/回退/停机协议：`socket_ser_receive_state`（`.cc:63-110`）与 `socket_ser_request_route`（实现 `SocketRecvRouteSource` 的 10 个虚函数）。

（以下为起草期原文，仅作设计依据保留）

当前 `shm_pub_sub_ipc` 已落地状态组织、收包处理抽取、RouteSession 和 SHM wait-set；**固定 route worker 已由共享层交付**（见上方更正），本方案复用前述已实现模式。

- 参照 `SubState` 建立 shared receive state：稳定 service key、UDPNode、模板/回调、generation、in-flight 和统计；worker 不持有裸 `socket_ser_ipc` 指针。
- 把 request 收包后的 wire 校验、callback 和 response 处理抽成独立函数，类似 process_received_buffer，保持原调用顺序。
- 按阶段 5 的固定 worker route 映射设计实现：同一数据 socket 固定归属一个 worker，不 work-stealing；在完整请求边界执行消息/字节/时间预算。
- 参照 RouteSession：先停止新 lease、注销 wait 项，再唤醒接收并等待 in-flight 归零，最后关闭通道。
- backend 不可用时明确回退原 response_thread_，不对 socket 做忙轮询。

## 3. Socket 专属等待层

recv_wait_set 只等待 SHM 共享 sequence，不能用于 UDP。与 socket pub-sub 共用 UDPNode 可等待句柄及 SocketWaitSet。只注册 ipc_r_ptr_ 请求数据通道；ack_r_tx_ 是发送端点，握手 socket 与 data plane 分离，均不加入本 ReceiveWorker。

## 4. 必须保持的行为

- 同一 ipc_r_ptr_ 只由固定 worker 调用 chunk_rev_server，分片重组不跨线程。
- wire/schema 验证、每 server callback 串行、response 序列化及发送顺序不变。
- stop 时注销 route、打断 wait/recv、等待 in-flight 退出，再关闭数据与 ACK 通道；握手通道继续常驻。
- restart 必须使用新 generation：旧 wait 项和处理完成后关闭旧 socket，再注册新数据 socket，避免 fd 复用事件误关联。
- callback 执行中的停止默认等待 callback 完成，禁止对象销毁后继续发送响应。
- client send_request 的同步完成、rev_tm 和响应配对不变，直到独立关联协议通过验收。

## 5. 生命周期协议

stop/析构：标记数据面 stopping，拒绝新处理；同步注销 worker route 并取消 OS wait；shutdown/close 以唤醒当前组包接收；等待 chunk_rev_server、callback 和 response send in-flight 归零；关闭数据/ACK 节点；按既有规则管理握手线程。

restart：完全注销旧 generation，关闭旧节点，建立并连接新节点，注册新 generation 到固定 worker，最后置 data_plane_running_。不能在旧 worker 仍可能访问时复用 UDPNode 或底层 fd。

## 6. 执行阶段

### C0：基线与 UDP wait 能力 —— 已完成（能力由 ipc-transport 交付）
记录多 server 空闲线程数、CPU、请求/响应延迟、callback 时长、停止/重启时间和分片吞吐。**UDPNode 可等待句柄及取消等待由 ipc-transport（t2 + t12）交付并冻结**（`waitable()/wait_handle()/readable()/cancel_wait()/clear_wait()` + `udp_node_*`），并已验证 close/wait 与句柄复用安全（`test_socket_wait_set` 9 条 + `test_socket_readable` 5 条）。⛔ 本模块**未**重复实现该能力。

### C1：共享 SocketWaitSet —— 已完成（共享层由 ipc-transport 交付）
`SocketWaitSet` 由 ipc-transport 交付（每 worker 一实例；Linux epoll / Windows WFMO 上限 63）；本模块**只消费**。多 socket 同时就绪、remove/stop、超时、关闭唤醒、fd 复用与错误降级的自检见 `test_socket_wait_set`（9 条）。

### C2：服务端处理路径抽取 —— 已完成
从 `response_thread_func` 抽出完整请求处理（wire 校验 → **用户 callback** → 响应序列化 → 发送），建立 service shared state（`socket_ser_receive_state`）与固定 route 适配器（`socket_ser_request_route`）；保留兼容 `response_thread_func` 后端。
⛔ **裁决 D1 落实**：用户 callback 与响应发送**不在** `recv_once()` 内，而在本 service 的**处理路径** `process_thread_func()`（`.cc:962`）里；`recv_once()` 只做「先 `udp_node_readable()` 判可读 → 一次 `chunk_rev_server(ServerRevTime=200ms)` 到完整请求边界 → 入有界队列 → 返回本次字节数」。

### C3：stop/restart 与协议回归 —— 已完成
覆盖握手持续活跃、stop 同时有数据、callback 执行中 stop、重启新 generation、大 payload、ACK 端点分离、多 client 和 shm/socket 路径切换。
实测（t9 复审，两臂各 3 轮）：worker 臂 14/14 Passed rc=0、compat 臂 14/14 Passed rc=0；`test_dzipc_socket` worker 证据行 `request receive on shared socket worker 12 (generation 1, workers 32)`；compat 臂打印 `socket wait-set unusable (DZIPC_SOCKET_COMPAT_THREAD=1); keeping per-service receive thread` 且 `receive state missing` = 0。

### C4：客户端集中响应接收评审 —— **本波不做**（未达成项 U-1）
集中接收前需新增 request ID/pending map、并发约束、超时取消、重启失败通知和 API 兼容层。尚未证明响应可靠关联时，继续由 `send_request` 调用线程接收 —— 本波**保持原样**，`socket_cli_ipc::send_request` 一行未改。

## 7. 验收标准

- 多 socket server 的数据接收线程受固定 worker 数限制；握手线程暂时维持原样，直至控制面定时调度单独迁移。
- 每 server callback 串行，request/response 顺序不变，分片组包不跨 worker。
- stop/restart 和析构无永久等待、悬挂 wait 项、旧 fd 误关联或悬空访问。
- 大消息、超时、ACK/NACK 和路径切换回归通过。
- backend 不可用时明确使用兼容 response thread，不忙轮询。
- client 继续同步接收，除非 C4 协议另行通过验收。

## 8. 风险

chunk_rev_server 单次调用可能持续到完整请求组装，callback 也可能耗时较长；预算只能在完整请求边界生效，热服务会增加同 worker 的其他服务延迟。必须记录组包和 callback 时长，并保留独占线程逃逸选项。shared_ptr<UDPNode> 不能替代 close/rebuild 的 in-flight 同步屏障。

---

## 9. 未达成项、遗留项与同步边界表（t10 回写）

### 9.1 未达成项（t11 汇总口径）

| ID | 未达成项 | 依据（文档位置 → 实现/实测） | 归属 |
| --- | --- | --- | --- |
| **U-1** | **C4 客户端集中响应接收不做**：`socket_cli_ipc::send_request` 仍在调用线程同步收发，无 request ID / pending map / 超时取消协议 | §6·C4；`src/dzIPC/socket_ser_cli_ipc.cc:1468`（`socket_cli_ipc::send_request`，本波未改） | 独立阶段（需先过 C4 协议验收） |
| **U-2** | **控制面未迁移**：`server_handshake` 仍为 per-service 独立线程（与 shm_ser_cli 的 D7 同口径） | §7 第 1 条；`src/dzIPC/socket_ser_cli_ipc.cc:741` | 独立阶段（需控制面 owner 评审） |
| **U-3** | Windows 分支（`WaitForMultipleObjects` / WSAEvent）**未在 Windows 验证** | 契约 §7；本方案 §8 末段 | 声明性限制（非缺陷） |

### 9.2 与基准实现的差异点（防误判，逐条给"为什么可以不同"）

| 差异点 | 本模块做法 | 与 shm_pub_sub 基准的关系 |
| --- | --- | --- |
| 等待身份 | `SocketWaitToken{owner=request_node.get(), handle=udp_node_wait_handle(request_node)}` | SHM 用 `ipc::recv_wait_token`（共享内存 seq）；socket 无 seq 字，只能以"内核说可读"为提示，**这是契约 §5 明文规定的唯一机制差异** |
| 假就绪兜底 | 连续 4 轮无果的 token 会被 `remove+add` 重挂（共享层，`SocketWaitSet`） | SHM 侧以 seq 为准、不需兜底；socket 侧不兜底会把 worker 变成忙轮询（契约 §5 表） |
| 一次收包边界 | 一次 `chunk_rev_server(ServerRevTime=200ms, ser=true, ack_r_tx_)` 可以跨多个分片，但**必须在完整请求边界返回** | 同基准语义（SHM 的 `recv` 也在 libipc 内完成多片重组后才返回） |
| 读路径 | 只用 `chunk_rev_server`（内部 `MSG_DONTWAIT` 语义）；先 `udp_node_readable()` 判可读 | **勘误 E1 硬约束**：`wait_handle()` 返回的是**阻塞** fd，⛔ 不得裸 `recvfrom`、⛔ 不得自置 `O_NONBLOCK`（会把既有 `receive(invalid_value)` 无限等待退化成紧循环忙轮询） |
| 停机唤醒 | worker 路径靠 `remove_route` → `stop_and_wake()` → `udp_node_cancel_wait()` 打断在途组包 | 同基准（SHM 侧是 `confirm/disconnect` 叫醒 `recv`） |
| restart | 完全注销旧 generation → 关旧节点 → 建并连接**新** `UDPNode`（不复用旧 fd）→ 注册新 generation → **最后**置 `data_plane_running_`（裁定 ②/⑥） | 基准是 generation 单调递增 + `begin_rebuild`；socket 侧无 route 重建 API，只能换节点 |

### 9.3 同步边界表（P4 / 防复发 · 必填）

> **由来**：t7 finding **C-01（high）** 的根因就是 `receive_state_` **未列入任何收尾清单**，
> 于是被 `restart_data_plane()` 写、被 `reset_message()/reset_callback()/process_thread_func()` 并发读而**无人发现**。
> 本节把新引入的同步面逐条登记；新增成员若未进本表，视为同类风险。

| # | 对象 / 成员 | 谁写 | 谁读 | 同步原语 / 约束（实测锚点） |
| --- | --- | --- | --- | --- |
| B-1 | `socket_ser_ipc::receive_state_` | `start_receive_path`（注册成功时）/ `teardown_receive_path`（重置） | `reset_message`、`reset_callback`、`process_thread_func` | **`receive_state_mtx_` + 三个访问口**（`current_/set_/clear_receive_state()`，`:460/466/472` 定义；成员访问仅 `:463/469/475` 三处且在锁内）；锁序 = **成员锁取完快照即释放 → `state->mtx`**，两组绝不交叉持有 |
| B-2 | `socket_ser_receive_state::owner` | 共享 worker `add_route` / 兼容线程 claim / `remove_route` 第 6 步 | 三方 | `std::atomic<RecvOwner>` CAS（`.cc:84`，契约 §4.6 单消费者互斥） |
| B-3 | `receive_route_` / `data_plane_running_` / `response_thread_` / `handshake_thread_` | `InitChannel` / `open_data_plane` / `stop_data_plane` / `restart_data_plane` | 同上四个入口 + 处理线程 | **不加锁**，依赖 API 约束：数据面生命周期入口 `open/stop/restart_data_plane` 由调用方串行化（auto 层 `leg_mtx_`）；`data_plane_running_` 为 `std::atomic<bool>`，处理线程用 `running && data_plane_running_` 双条件退出（`:913`） |
| B-4 | `state->request_node` / `request_ack_tx` / `response_node`（强引用） | 建 state 时（`open_data_plane`） | worker `recv_once`（只碰 request_node） | worker 侧 entry 持 `shared_ptr` ⇒ 节点活到 route 注销之后；**节点只在 `remove_route` 返回后才 `close`**（防句柄复用误关联），实测顺序 `stop_data_plane():696-707` |
| B-5 | `state->mtx` + `msg_template` / `callback` 快照 | `reset_message` / `reset_callback` | worker（取模板）与处理线程 | `mutable std::mutex`；**取快照后必须释放**再碰其他锁（t8 §3 锁序核对） |
| B-6 | `state->queue`（有界请求队列，`kQueueCap`） | worker `recv_once` 入队（`enqueue_request`） | 处理线程 `process_thread_func` 出队；停机路径作废 | `queue_mtx` + `queue_cv`；t9 已逐点核对 **8/8 容器访问全在临界区内、3 处加锁点全 RAII** |
| B-7 | `state->stopping` / `recv_in_flight` / `quiesce_cv` | `stop_and_wake()` / 注销方 | worker、注销方 | `std::atomic` + `quiesce_mtx`/`quiesce_cv`；`wait_quiescent()` 有界 **2000 ms**（`kSerQuiesceTimeoutMs`，`:56`），超时只打诊断（C-07 修复） |

> ⛔ 本表是**收尾清单的一部分**：新增成员若未进本表，视为 C-01 同类风险。

### 9.4 注释与调用点机械核对（P5 / 防复发）

> **根因**：C-03/C-04 的本质是「**注释抄了语义、函数体继承了旧实现**」（注释说兼容线程会 claim，函数体里却没有）。

| 注释/契约声明 | 机械判据（命令） | 期望（t9 实测） |
| --- | --- | --- |
| 兼容线程会 claim `compat_thread` 并在退出时归还 | `grep -n "compat_thread" src/dzIPC/socket_ser_cli_ipc.cc` | **≥ 2**（`:901` claim + 名字表/诊断）；修前 **0** |
| RAII 释放（正常/异常出口都归还） | `grep -n "SerCompatClaimGuard" src/dzIPC/socket_ser_cli_ipc.cc` | 定义 + 使用（`:908`）各命中；析构调 `:392` |
| `recv_once` 内**不含**用户 callback | `grep -c "std::function<void(std::shared_ptr<ServiceData>&)>" <recv_once 体>` | **0**（callback 只在 `state->callback` 快照与处理线程里） |
| 读路径不碰 E1 红线 | `grep -n "recvfrom\|O_NONBLOCK\|FIONBIO" src/dzIPC/socket_ser_cli_ipc.cc` | **0 调用**（`<fcntl.h>` 是既有 include，`O_NONBLOCK` 仅出现在注释里） |
| 不调用池 `stop` | `grep -rn "Pool::instance().stop" src/dzIPC/*.cc` | **0** |

### 9.5 验收纪律（A8，强制）

```bash
cd /home/zwc/cpp_ipc_dds && cmake -S . -B build && make -C build -j$(nproc)
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS ctest --output-on-failure
env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS ./bin/test_dzipc_socket
DZIPC_SOCKET_COMPAT_THREAD=1 ./bin/test_dzipc_socket        # 兼容臂对照
```

判据（三条同时成立才算 worker 路径生效）：① 无 `socket wait-set unusable (...)` 回退行；
② `test_dzipc_socket` rc=0 且 `[ FAILED ]` = 0；③ 出现 `request receive on shared socket worker <N> (generation 1, workers <M>)` 证据行。
⛔ 两个环境变量都必须显式清空（残留会静默反转读数）；⛔ 任何验收前必须重跑 CMake 配置再重编（配置期展开 + build/ 曾残留已消失实现的符号 ⇒ 旧产物假绿）。
