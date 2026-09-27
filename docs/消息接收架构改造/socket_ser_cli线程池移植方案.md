# Socket ser-cli 接收线程池移植方案

> 状态：执行方案，尚未实现。
> 参照：shm_pub_sub_ipc 已落地的 shared state、处理函数抽取、RouteSession 与安全注销；固定归属 worker 以 `shm_sub_thread_consolidation_plan.md` 阶段 5 为设计依据，socket 另建 UDP 可等待 backend。
> 范围：socket_ser_ipc 服务端请求接收；socket_cli_ipc 同步响应接收另行评审。

## 1. 当前结构与目标

socket_ser_ipc 在数据面启动时创建 response_thread_，循环调用 chunk_rev_server 收取和组装请求，随后执行 callback 与响应发送。server_handshake 独立管理握手和路径信号。stop_data_plane/restart_data_plane 会关闭并重建数据通道。

socket_cli_ipc 没有独立响应接收线程：send_request 在调用线程发送请求并同步等待响应。请求与响应由调用栈配对，不能简单把 chunk_rev_server 放到共享 worker。

首期将 socket_ser_ipc 的服务端请求接收迁入固定 socket worker，保持每 service 单消费者和 callback 串行。client 同步路径维持现状。

## 2. 从 shm_pub_sub_ipc 复用的机制

当前 `shm_pub_sub_ipc` 已落地状态组织、收包处理抽取、RouteSession 和 SHM wait-set，但固定 route ReceiveWorker 尚未在该分支落地。本方案复用前述已实现模式，固定 worker 和 SocketWaitSet 需要按阶段 5 及本方案新增实现。

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

### C0：基线与 UDP wait 能力
记录多 server 空闲线程数、CPU、请求/响应延迟、callback 时长、停止/重启时间和分片吞吐。实现 UDPNode 可等待句柄及取消等待，并验证 close/wait 与句柄复用安全。

### C1：共享 SocketWaitSet
复用 socket pub-sub 的平台 backend；测试多 socket 同时就绪、remove/stop、超时、关闭唤醒、fd 复用和错误降级。

### C2：服务端处理路径抽取
从 response_thread_func 抽取完整 request 处理函数，建立 service shared state 和固定 route registration；保留兼容线程后端。

### C3：stop/restart 与协议回归
覆盖握手持续活跃、stop 同时有数据、callback 执行中 stop、重启新 generation、大 payload、ACK 端点分离、多 client 和 shm/socket 路径切换。

### C4：客户端集中响应接收评审
集中接收前需新增 request ID/pending map、并发约束、超时取消、重启失败通知和 API 兼容层。尚未证明响应可靠关联时，继续由 send_request 调用线程接收。

## 7. 验收标准

- 多 socket server 的数据接收线程受固定 worker 数限制；握手线程暂时维持原样，直至控制面定时调度单独迁移。
- 每 server callback 串行，request/response 顺序不变，分片组包不跨 worker。
- stop/restart 和析构无永久等待、悬挂 wait 项、旧 fd 误关联或悬空访问。
- 大消息、超时、ACK/NACK 和路径切换回归通过。
- backend 不可用时明确使用兼容 response thread，不忙轮询。
- client 继续同步接收，除非 C4 协议另行通过验收。

## 8. 风险

chunk_rev_server 单次调用可能持续到完整请求组装，callback 也可能耗时较长；预算只能在完整请求边界生效，热服务会增加同 worker 的其他服务延迟。必须记录组包和 callback 时长，并保留独占线程逃逸选项。shared_ptr<UDPNode> 不能替代 close/rebuild 的 in-flight 同步屏障。
