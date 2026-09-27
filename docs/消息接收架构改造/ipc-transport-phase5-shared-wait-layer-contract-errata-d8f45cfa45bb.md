# 阶段 5 共享等待层与固定收包 worker —— 接口契约勘误（消费方必读）

> 状态：**勘误，优先于契约正文**。owner：ipc-transport（共享层唯一 owner）。
> 主体契约：`ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md`
> —— 公开签名、线程模型、route↔worker 归属规则、预算、注销协议、能力探测与
> 回退策略**仍以它为准**；下面 E1/E2/E3 三条按本勘误读。
> **状态更新（t12 交付后）**：正文新增 **§6**（`readable()` / `udp_node_readable` 的
> 非阻塞可读判据，以及 `chunk_rev_*` 的 `out_bytes` 字节出口 —— 都属于"新增一律追加"，
> 原符号全部保留），并在 §5 表格钉死两条消费面语义：「`recv_once()` 正返回值 = 本次完整
> 消息/请求的字节数」与「必须先 `udp_node_readable` 再调 `chunk_rev_*`；无数据立即返回 0，
> ⛔ 不得在共享 worker 线程上空读阻塞到 tm」。本勘误 E1/E2/E3 **继续有效**。
>
> **状态更新（t2 交付后）**：正文已按本勘误**就地修正**，冲突表述不复存在 ——
> §1 的 `wait_handle()` 描述已改为"Linux 返回阻塞接收 fd、无 O_NONBLOCK/FIONBIO"
> （E1）；§4.4 的 `add_route()` 判定表已改为"先 try_claim_recv 后查表"（E2）；
> §0 交付面表已补列 `src/libipc/socket/udp.cpp`（E3）。本勘误**保留**作为权威记录
> 与理由说明（含实测依据），不删除。
> 起因：契约正文 §1 对 `wait_handle()` 阻塞模式的描述与实现**相反**，消费方若
> 照正文实现，会出现"裸 `recvfrom` 阻塞 worker 线程"的静默故障（不是崩溃，是
> 该 worker 此后永久停收，且没有任何报错）。

---

## E1（硬错误，必须按本条）`wait_handle()` **不改变** fd 的阻塞模式

契约正文 §1 原写：

```text
Linux = 非阻塞接收 fd；Windows = 惰性创建的 WSAEVENT
        （首次调用内部做 WSAEventSelect + FIONBIO）。0 表示不可等待。
```

**该描述作废。** 实现与冻结语义为：

| 平台 | 实际行为 |
|---|---|
| Linux | 返回**接收 fd 本身，阻塞模式保持原样（阻塞）**。`wait_handle()` 不做 `fcntl(O_NONBLOCK)`，也不做 `FIONBIO`。 |
| Windows | 惰性创建并 `WSAEventSelect(FD_READ\|FD_CLOSE)`。WinSock 的固有副作用是 socket 被置为**非阻塞**，**无**显式 `FIONBIO`；`receive(invalid_value)` 的无限等待分支已针对这一点补了 `WSAWaitForMultipleEvents` 阻塞等待。 |

**消费方必须遵守**：

- 从 wait-set 拿到就绪后，**一律**用 `receive_nowait()` / `chunk_rev_*` 读取，
  不得对该 fd 用裸 `recvfrom()` —— 它是阻塞的，会让 worker 线程永久挂住。
- 不得自己给这个 fd 置非阻塞：`receive(invalid_value)` 的无限等待分支依赖阻塞
  语义，置了会把它退化成紧循环忙轮询（阶段 5 的第一红线）。
- 不变量未变：`wait_handle() != 0` ⇒ `waitable() == true`；`cancel_wait()` 之后
  `waitable() == false` 且 `wait_handle() == 0`。

理由（实现注释原文要点，`src/libipc/platform/posix/udp.h`）：epoll 是
level-triggered，单消费者下 `epoll_wait` 报可读时数据一定还在，`recvfrom` 直接读
即可，不需要非阻塞；而置非阻塞会**破坏既有语义** —— `receive(invalid_value)` 的
无限等待分支用的是不带 `MSG_DONTWAIT` 的 `recvfrom`，非阻塞 fd 上它会立刻返回
`EAGAIN`，调用方（如 `data_rev.cc` 的 ACK 等待）就从"阻塞等到数据"退化成紧循环。

---

## E2（精确化）`add_route()` 的 duplicate / busy 判定顺序

契约正文 §4.4 判定表把 `try_claim_recv(worker) 失败` 与"已注册同一 route"写成
两条并列项。实际实现是**一次 CAS + 一次查表**：

```text
try_claim_recv(worker) 成功                        -> 继续注册
try_claim_recv(worker) 失败:
    在 worker 的 route 表内找到同一条 route          -> duplicate
    否则（别的 owner，如 compat_thread 正在 recv）   -> busy
```

对外语义与正文**一致**（消费方据此区分"自己重复注册"与"兼容线程在收"，两者处置
相反），只是判定顺序实现为"先查表"。`RecvWorkerPool::add_route()` 同。

---

## E3（交付面补列）`src/libipc/socket/udp.cpp`

契约 §0 的交付面表漏列了 `src/libipc/socket/udp.cpp`。它是 `UDPNode` 的 pimpl
转发层，也是 `include/libipc/udp.h` 新增 4 个声明的**唯一落点**（不实现即未定义
符号、链接失败），因此属于本任务的必要改动。改动内容是 4 个纯转发 + 失效态中性值
（`waitable→false` / `wait_handle→0` / `cancel,clear→no-op`），不触碰任何既有方法。

---

## 针对"三份方案文档漏接口"的确认

三份模块移植方案文档漏列 `RecvRouteSource` 的 9 个纯虚接口与注销第 6 步
`release_recv()`。**主体契约已完整覆盖**：

- §4.1 列出 9 个纯虚接口 —— `route_name` / `domain_id` / `read_wait_token` /
  `recv_once` / `recv_owner` / `try_claim_recv` / `release_recv` /
  `stop_and_wake` / `wait_quiescent`（另有带默认实现的 `has_pending`）；
- §4.4 列出注销 6 步，第 6 步即 `route->release_recv()`（归还 owner 到 `none`，
  之后兼容线程才允许重新接管）。

**模块作者请以主体契约 §4.1/§4.4 与 `include/dzIPC/threepools/recv_worker.h`
为准**，不要照方案文档的接口清单实现 —— 漏 `release_recv()` 会让 `add_route`
永久返回 `busy`、静默丢包。

---

## 未在 Windows 验证（重申）

本机为 Linux 6.8。`src/libipc/platform/win/udp.h` 的 WSAEvent/WSAEventSelect 路径、
`src/dzIPC/threepools/socket_wait_set.cc` 的 `WaitForMultipleObjects` 分支、
`recv_wait_set.cpp` 的既有 Windows 分支，本次**均未在 Windows 验证**，只做编译期与
接口一致性保证。
