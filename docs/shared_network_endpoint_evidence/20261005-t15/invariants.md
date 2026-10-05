# I01～I21 最终审计

源码基线为 T14 `46a0be8`；本节点不修改通信实现。测试入口和实际数量见 [结果](results.md) 与 [T14 矩阵](../20261005-t14/matrix.md)。通过仅指下表的证据范围。

| 不变量 | 结论 | 代码与证据 |
|---|---|---|
| I01 应用无每话题共享 UDP | 通过 | shared_pub_sub_ipc/PublisherEndpoint 只持本机腿及 ClientRuntime；scale 1000 话题进程 UDP=0 |
| I02 每 UDP FD 单读取者 | 通过 | GatewayData 每 shard 独占 endpoint；控制/发现由控制循环读取；endpoint/io_lifecycle 测试 |
| I03 分片完整身份 | 通过 | wire_protocol 160B 头、reassembly Admission 检查在分配前；wire golden 与交错发布者 |
| I04 topic/domain/kind 不串话 | 通过（协议/重组层） | ScopeToken/RouteKey 包含完整域与 kind，T14 八路高域、msg_id=0、同前 127B 名称共 shard 测试 |
| I05 代次内最多一次 SHM | 通过 | reassembly 提交 ticket 与 dedup 不因 TTL/LRU 遗忘；ACK 丢失、冲突、双 shard 重复输入及真实 SHM |
| I06 确定提交后 ACK | 通过 | ReassemblyShard::tick 按 SubmitState 终结，真实 bridge ACK 测试；未知提交不重试 |
| I07 ACK/NACK 控制 socket | 通过 | GatewayData 反馈进入有界事件队列，GatewayRuntime 控制 socket 发出；发送状态机不读接收 FD |
| I08 接收不等待业务/缺片 | 通过 | ingest 单片推进，tick 非阻塞 bridge.try_commit；不同 shard 冷热窗口通过。单次大块复制仍可能超过时间预算，未声称硬实时 |
| I09 prebuilt false 无可见提交 | 通过 | local_direct/partial_submit/prebuilt 三态归并及失败后单次回退；Python 坏段回退 |
| I10 旧模式入口和默认 | 通过（本机 Linux） | shared_config 缺变量映射 Legacy；topic_ipc 仅显式 SharedV1 分支；旧回归8项、OFF2项、安装回滚哨兵0连接 |
| I11 注销先撤销再释放 | 通过 | LocalRegistration active + shard fence 取消在途；subscriber retire/close 等待 getter；credit/teardown/restart |
| I12 全部表和队列字节/条目上限 | 部分 | 主要重组/目录/信用/命令队列有硬上限；target/stream 元数据部分依赖条目数间接界定；每类精确字节账本及逐项触顶审计未全覆盖 |
| I13 入站不回流 | 通过 | 入站仅 bridge；bridge 不写 Outbox；双向1/8/32订阅及多目标无回流 |
| I14 完整64位域 | 通过（协议/重组层） | RouteKey scope 及目录编码保留 uint64；0/2^32/2^40+3/MAX 专项 |
| I15 话题变更不改 socket | 通过 | GatewayRuntime 构造时固定 K+2；scale 1/100/1000 和关闭时 UDP 均为6 |
| I16 不清共享 socket 队列退订 | 通过 | 退订撤销 route epoch/fence，不调用按话题 drain UDP；reassembly 旧代次拒绝 |
| I17 身份不混用 | 通过 | random gateway/publisher/session 身份、epoch、sequence 独立；重放和交错发布者测试 |
| I18 触顶显式失败并计数 | 部分 | 主要分配拒绝和汇总计数存在，全部第14.2节细分触顶/错误计数未齐；保持缺项 |
| I19 本机直达/源不回注 | 通过 | PublisherEndpoint 本机先尝试，GatewayData 发送路径不调用 bridge；source_injections=0 且各本机消费者只收一次；SIGSTOP/SIGKILL 后本机继续 |
| I20 无逐消息 BEGIN | 通过 | 旧DZLC类型15/16拒绝，等待者先登记；已有信用直接Outbox，credit_requests=0；两类信用独立守恒 |
| I21 RouteKey 独占 shard 提交 | 通过 | 同一个 Shard::run 内 ingest/tick/bridge.try_commit，无全局统一业务提交线程；不同K双shard只提交一次、同/不同shard冷热实测 |

未声称证明任意机器调度、网络故障或所有输入下的性能。源发布取消发生于注销屏障前，已经进入网络的报文不具备撤回保证。所有“最多一次”均限于明确存活的网关/路由代次。
