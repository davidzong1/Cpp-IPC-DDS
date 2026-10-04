# 共享网络端点与按话题分发：详细执行方案

编制日期：2026-10-04  
复核日期：2026-10-05（补齐来源目录、去重保留边界与容量核算）\
源码观察基线：e887d5e 及当时工作区；SHM 多发布者改造仍在另一个任务中进行。  
文档状态：**待执行的设计与任务说明；不是已经实现或验证的能力。**  
建议文件名：docs/shared_network_endpoint_execution_plan.md  
前置方案：[SHM 多发布者执行方案](shm_multi_publisher_execution_plan.md)

> 执行者首先阅读第 1～5 节，再按第 15 节的任务卡顺序实施。不要只读任务标题。
> 所有“新增”类、函数、配置、工具、测试名均是本方案要求创建的内容，不表示仓库已经存在。
> 当前其他 Agent 的工作区改动不属于本任务。不得清理、覆盖、回滚或提交这些改动。
> 本方案允许先交付中间阶段，但不得把“阶段通过”报告成“完整方案完成”。

### 给执行 Agent 的阅读方式

不要一次实现全部模块。先保存第 3 节决策、第 5 节不变量和当前任务卡的摘要，
再按下表只读取本卡需要的详细契约。每次接续工作先读第 20 节记录，不能从 T00 重新开始。

| 当前任务 | 需要配合阅读的章节 |
|---|---|
| T00～T01 | 2、3、5、12、13、16 |
| T02～T03 | 6、7、8.1～8.2、8.5、9.2、9.4、11 |
| T04～T05 | 4.5、5.1～5.3、9、12.2～12.3 |
| T06～T07 | 5、6、7、10、12 |
| T08～T10 | 7.5～7.7、8、9.4、11、12.4、17.2～17.3 |
| T11～T13 | 3.5、5、9、10、12、14、18 |
| T14～T15 | 16～20 |

本文件中所有端口、协议和返回语义都是**新模式的设计契约**，默认旧模式不随文档改变。

## 1. 要解决的问题与最终交付

### 1.1 问题

当前每个 socket 发布对象和订阅对象各自创建网络 socket。即使多个话题共用一个端口号，
或者同机数据改走 SHM，这些对象仍可能各自持有网络 socket、缓冲和接收状态。
大量设备发布不同话题、监控机全部订阅时，单机资源会随本机话题端点数量增长。

目标不是只把端口公式改成常数，而是：

1. 应用进程不再为新模式的每个话题创建 UDP socket。
2. 本机网关用少量 UDP socket 承载所有已注册域和话题。
3. 收到远端数据后，按完整通道身份和发布者身份重组。
4. 完整消息只向对应本机 SHM 话题提交一次，由现有 SHM 路径分发。
5. 可靠确认与重传不争抢数据接收 socket。
6. 增加话题、发布者、订阅者时，UDP socket 数量保持受配置上限约束。
7. 老模式仍可构建和运行，显式配置新模式后不在运行中偷偷切回老模式。

### 1.2 最终产品

- 新程序 dzipc_gateway：前台运行的本机网关，可由现有进程管理器托管。
- 新后端 SharedPublisher / SharedSubscriber：通过既有 pub/sub 抽象接入。
- 新网络协议 DZMX v1：带完整消息身份，支持分片、重组、ACK/NACK 和配额。
- 进程到网关的本机控制会话与 SHM 出站通道。
- 网关到业务 SHM 话题的原始消息注入接口。
- 状态命令、指标、网络抓包说明和模式诊断。
- 单元、跨进程、故障、网络仿真、真实跨机和资源规模验收。
- 可操作的启用、停止和回滚说明。

### 1.3 完成判据

必须同时满足第 17 节的验收项和第 19 节的最终清单。
至少实测：

- 1 / 100 / 1000 个话题；
- 同话题 1 / 2 / 8 个发布者；
- 同机 1 / 2 / 8 / 32 个订阅者，受前置 SHM 实际上限约束；
- 至少两台主机，或两个隔离网络与 IPC 环境；最终发布仍须真实跨机数据；
- 乱序、重复、丢片、ACK 丢失、端点析构、网关退出和重启。

资源目标是减少网络端点与本机重复收包。**不预先承诺吞吐或延迟一定优于旧模式。**

## 2. 已确认的源码事实

这些事实供定位使用。实施时重新按符号查找，不依赖会漂移的行号。

| 当前文件 / 符号 | 已确认行为 | 对本方案的影响 |
|---|---|---|
| src/dzIPC/topic_ipc.cc：publisher_ipc_impl、subscriber_ipc_impl | IPCType::Socket 选择 hybrid 后端，SocketOnly 选择纯 socket | 在工厂里按新配置选择后端，保持枚举值不变 |
| src/dzIPC/hybrid_pub_sub_ipc.cc：Publisher、Subscriber | 每个对象仍创建 socket 腿；订阅对象同时从 SHM 和 socket 取消息 | 不能把现有 hybrid 当作主机共享网关 |
| src/dzIPC/socket_pub_sub_ipc.cc：InitChannel | 发布者创建数据发送 / ACK 接收；订阅者创建数据接收 / ACK 发送 | 只共享接收线程不能消除每对象 socket |
| include/dzIPC/common/channel_scope.h | 通道身份包含 kind、64 位 domain、话题；端口分配为 4263 个五端口槽 | 端口不是唯一身份；新协议继续使用完整 scope |
| include/dzIPC/topic_ipc.h、common/name_operator.h | 公共域参数及 SHM 命名入口仍使用 size_t | 首版共享后端要求 64 位进程，不能仅验证网络头便声称端到端域不截断 |
| src/libipc/platform/posix/udp.h | 接收端绑定组播地址，发送端隐式使用临时源端口 | 合并固定端口还必须消除大量独立发送 socket |
| src/libipc/socket/udp.cpp：UDPNode_::accept | 一个 UDPNode 只接受一个 scope，并剥掉外层来源头 | 不能让多个 topic 共用现有对象后轮流 set_scope |
| src/dzIPC/common/data_rev.cc：chunk_rev_topic / chunk_send_ex | 以单次完整消息收发为主要处理单元，有阻塞等待和旧页尾协议 | 不得直接拿来实现共享 socket 的多消息并发重组 |
| src/dzIPC/common/hybrid_discovery.cc：Discovery | 每进程共享旧发现通道 239.255.250.250:11245 | 新模式需要按网关汇总订阅；旧发现协议不直接改版 |
| include/dzIPC/common/shm_channel.h | 在进行中的工作区有 route / mpmc_channel 类型封装 | 前置任务完成后复核实际接口 |
| src/dzIPC/shm_pub_sub_ipc.cc | SHM 已有 DZFlat、TLV、生命周期、借样和接收 worker | 网关注入必须复用这些生命周期，不私自清段 |
| include/dzIPC/threepools/socket_recv_worker.h | 旧 recv_once 以完整消息边界让出 | 新收包引擎使用独立状态机，不能改变旧 worker 契约 |
| include/dzIPC/pub_sub_base.h：publish_prebuilt_segment | false 允许调用者回退并再次 publish | 必须保证 false 之前没有发生可见提交 |
| src/libipc/ipc.cpp：publish_loan_impl | 队列记录携带 loan 容量，真实载荷长度由上层信封说明；失败路径可尝试 force_push | DZTX 必须从 payload_size 截取载荷；出站接管不等于持久保存，需验证覆盖与背压 |
| include/dzIPC/ipc_info_pool.h：kMaxTopicName、kMaxEntries | 当前诊断池话题名数组为 128B、登记表为 4096 项 | 不能拿截断的展示名称恢复路由；1000 话题验收需核算逻辑与内部登记项 |
| include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp | TLV 序列化结果包含历史页尾结构 | 新网络协议把其视为不透明字节，不剥除旧 TLV 页尾 |
| src/CMakeLists.txt | aux_source_directory 在配置期收集源码 | 新目录必须接入构建，并重新运行 CMake |
| test/CMakeLists.txt | test/*.cpp 的目标创建与 CTest 注册分开 | 新测试必须既可执行，又被 CTest/驱动脚本实际执行 |

旧文档 local_shm_fanout.md 提到的设计理由仅作背景。它标注“未实施”，其中旧端口、
线程数量、拓扑和约束不能覆盖本方案或当前源码事实。

## 3. 冻结的设计决策

本节给出执行默认值，避免执行者在中途自行选择相互冲突的架构。

### 3.1 一个主机网关，服务多个域

每个本机运行实例由一个 dzipc_gateway 服务多个 domain 和 topic。
“本机运行实例”要求参与进程处于可互通的 IPC / 网络环境，使用相同 Unix 控制地址、
UID 和 IpcInfoPool locality identity。

- 同一运行实例只有一个网关。
- 网关用整个进程生命周期持有的文件锁排除重复启动。
- 所有域共享该网关的网络端点，域通过协议中的完整 64 位 domain 隔离。
- 同主机不同 UID / 容器可启动不同实例，但必须使用不同监听地址或端口配置。
- 锁文件唯一不等于网络端口唯一；网络 bind 仍是最终冲突判据。
- 网关不由第一个订阅者“临时担任”，不实现基于心跳抢占活进程的选主。
- 不自动启动后台守护进程。先提供前台命令，便于测试与部署。
- 首版运行平台为 Linux IPv4、64 位进程（sizeof(size_t)=8）。其他平台或 32 位进程
  保留旧后端，新模式显式报 UnsupportedPlatform；不为此修改旧公共 ABI。

### 3.2 首版采用按订阅网关单播

数据通过单播发送给确有订阅的远端网关，每个目标网关每条消息仅接收一份。
发现使用一个固定组播通道，数据不按话题加入组播组。

这样同时收敛 socket 数量和本机组播成员关系，并避免每台设备收到所有话题数据。
其明确代价是：

- 一个话题被 R 个远端网关订阅时，源网关发送约 R 份载荷；
- 旧组播理想情况下只需要源端发送一份；
- 高扇出图像 / 点云必须计算 R × 带宽，不能只看节省了多少端口；
- 如果目标部署因此超过网卡或 CPU 预算，首版不得宣称满足该部署，应继续使用旧模式。

按话题选择组播、可靠组播和动态组播组管理列为后续扩展，不混入首版验收。

### 3.3 数据、可靠控制、发现分别持有少量 socket

默认 K = 4 条数据 socket、1 条控制 socket、1 条发现 socket，共 K + 2 个 UDP socket。
每条数据 socket 同时 sendto / recvmsg，不为每个发布者、话题或目标建立发送 socket。

- 数据默认端口：24000～24003。
- 控制默认端口：24004。
- 发现默认端口：24005，组播地址 239.255.250.251。
- K 可配置为 1～16；数据端口连续，另外两类端口显式配置。
- 所有端口必须互不重复，且在 1～65535 内。
- 这些端口是工程默认值，**没有被操作系统或 IANA 为本项目保留**。
- 可能与旧模式散列端口或其他进程冲突。冲突即失败，输出具体地址、端口、errno。
- 数据和控制 socket 不使用 SO_REUSEPORT 实现多个网关“共享监听”。
- 发现 socket 可按组播要求使用 SO_REUSEADDR；不依赖其提供应用级去重。
- 源端按目标公布的 K 计算目标分片，不假设两端配置相同。

### 3.4 应用出站经过网关，本机入站继续使用业务 SHM

首版 SharedPublisher 不再同时直写业务 SHM 和发网络。它先提交网关出站队列，
由网关决定向本机业务 SHM 和哪些远端发送。

这么做是为了使一次提交只有一个接管点，明确预构造段回退和故障边界。
**这比旧 hybrid 本机直达路径多一跳 SHM 与网关调度，必须实测本机延迟。**
旧 hybrid 保留，首版不自动替换所有纯本机业务。

SharedSubscriber 只通过现有 shm_sub_ipc 消费业务 SHM；不持有网络数据 socket。
显式 IPC_SHM 的发布者仍仅向本机发布，网关不监听业务 SHM 再把所有消息转发出去。

因此：

- 网关是每个接入话题的一个 SHM 发布者；
- 本机显式 IPC_SHM 发布者可与它并存，依赖前置 MPMC；
- 远端入站消息永远不会再次进入网关出站队列；
- 不需要修改业务 SHM 载荷格式来添加来源标记；
- 首版不实现发布者直接 SHM 快路与网关路径之间的动态切换。

### 3.5 模式选择与兼容范围

新增 DZIPC_NET_BACKEND：

| 配置 | IPC_SOCKET | IPC_SOCKET_ONLY | IPC_SHM / 服务请求响应 |
|---|---|---|---|
| 未设置或 legacy | 当前 hybrid | 当前纯 socket | 当前行为 |
| shared_v1 | 新共享网关后端 | 当前纯 socket | 当前行为 |
| 其他值 | 构造时报告配置错误 | 同样报告全局配置拼写错误 | 不影响未读取此配置的纯 SHM 路径 |

- shared_v1 要求前置 MPMC 已启用且通过验收。
- 如果当前开关仍为 DZIPC_SHM_MPMC，要求其值为 1。
- 不偷偷把进程全局 SHM 开关改为 1；缺前置条件就失败。
- shared_v1 与 legacy 的网络协议不互通。不得自动双发到两套协议。
- 同话题需要新旧互通时，使用明确规划的独立桥接任务；首版不提供隐式桥接。
- 同机只有前置版本兼容的 IPC_SHM 端点才允许共享业务 SHM。
- 保持已有公共类虚表、IPCType 枚举数值和旧 UDPNode 行为。
- 新 API / 配置行为必须在工具和用户文档中显示为“共享网络模式”。

## 4. 架构、资源和数据流

### 4.1 数据流图

~~~mermaid
flowchart LR
  P["应用发布者"] --> TX["每应用进程一条出站 SHM"]
  TX --> G["本机网关"]
  G --> LS["本机各话题业务 SHM"]
  LS --> S["本机订阅者"]
  G --> N["少量 UDP 单播端点"]
  N --> RG["远端网关"]
  RG --> RS["远端各话题业务 SHM"]
  LP["显式 IPC_SHM 发布者"] --> LS
~~~

控制流单独使用每应用进程一个 Unix SOCK_SEQPACKET 连接：
注册、注销、状态、可靠发送完成事件和一次性 eventfd 传递。**业务载荷不通过此连接。**

### 4.2 资源计算口径

令：

- T = 网关当前活跃业务话题数；
- A = 与网关连接的应用进程数；
- P / S = 发布 / 订阅对象总数；
- K = 数据分片数，默认 4；
- R = 某条消息的远端接收网关数。

目标：

| 资源 | 增长方式 |
|---|---|
| 网关 UDP socket | K + 2，与 T / P / S 无关 |
| shared_v1 应用 UDP socket | 0，不含应用自身其他业务 |
| 网关本机控制连接与出站唤醒 FD | O(A) |
| 应用控制连接 / 出站通道 | 每进程 1 个 / 1 条，不是每个话题一个 |
| 业务 SHM 通道 | O(T)，仍按话题隔离 |
| 发布者登记和订阅登记 | O(P + S)，无法因端口复用消失 |
| 重组状态 | 与并发在途消息相关，必须有硬配额 |
| 网络载荷带宽 | 单播约为消息大小 × R，重传另计 |

验收必须同时报告 UDP socket、全部 FD、内核 socket 缓冲、RSS、SHM、线程和流量。
仅展示“固定端口减少”不算完成资源目标。

### 4.3 本机发布

1. 应用构造后端，连接网关，协商版本和容量。
2. 发布者注册，得到逻辑发布身份；创建/复用每进程出站通道。
3. 应用将消息编码成完整 DZFlat 段或完整旧 TLV blob。
4. 给该消息分配唯一发布者序号，构造本机出站信封。
5. 原子提交一条 SHM 记录，敲响该会话的 eventfd。
6. 网关读取并验证记录，冻结本次目标订阅快照。
7. 有本机接收者时，向业务 SHM 提交；有远端接收网关时，投递网络发送队列。
8. reliable 请求等待本机提交结果及冻结目标的 ACK；best-effort 不等待远端确认。

### 4.4 远端入站

1. 数据分片进入某条数据 socket。
2. 先检查长度、版本、校验、目标网关代次、完整话题身份和订阅代次。
3. 按第 7 节的消息键查找重组状态。
4. 完成重组与整包校验。
5. 在单一状态机中将消息置为 CommitPending。
6. 使用业务 SHM 原始注入接口提交一次。
7. 确定成功后记录去重状态，才可以发送 ACK。
8. 相同消息重传只重发 ACK，不再次写业务 SHM。
9. 本机订阅者通过既有 SHM worker / Sample / TLV 物化路径取得数据。

### 4.5 本地控制会话

应用进程第一次使用 shared_v1 时创建 SharedClientRuntime，之后所有对象复用它。

- 每进程至多一个该运行实例的会话；可使用按控制路径索引的运行时表。
- 库初始化与 fork 不能共享继承来的锁和 FD。检测到 PID 变化后，在取旧锁前拒绝旧句柄；
  支持 fork 后 exec，首版不承诺多线程进程 fork 后直接继续使用继承对象。
- 一个控制读循环负责接收应答并按 request_id 唤醒等待者。
- 多个调用线程禁止竞争 recv 同一控制连接。
- 发送大载荷、等待 ACK 时不持有运行时表锁或注册表锁。
- 连接断开后所有可靠等待立即结束为 GatewayLost，不永久等待。
- 已失效 runtime 不恢复旧句柄。相同进程后来新建后端对象时，工厂可为该控制路径创建新会话；
  旧对象继续明确失效，不把它们的 publisher_id、等待者或旧出站记录移到新会话。

## 5. 不变量与协议语义

以下编号同时用于代码评审、测试和交付记录。

| 编号 | 必须保持的不变量 |
|---|---|
| I01 | 任意应用话题对象不创建自己的 shared_v1 UDP socket |
| I02 | 每个 UDP 接收 FD 同时只有一个读取者 |
| I03 | 分片重组前已确定 scope、源网关代次、发布者 ID、序号 |
| I04 | 不同 topic / domain / kind 永远不会因端口相同串话 |
| I05 | 同一消息在目标网关代次和订阅代次内最多向业务 SHM 提交一次 |
| I06 | 只有确定业务 SHM 接纳后才发 ACK |
| I07 | ACK/NACK 通过控制 socket，不能让发送线程读取共享接收 FD |
| I08 | 接收回调不执行用户业务回调，不等待缺片或本机消费者 |
| I09 | publish_prebuilt_segment 返回 false 前没有可见出站提交 |
| I10 | 旧模式的入口、协议和默认配置保持原行为 |
| I11 | 注销先撤销路由、等待在途工作，再释放 FD / route / pool |
| I12 | 所有重组表、发送表、去重表和队列都有字节数与条目数上限 |
| I13 | 网络入站不写出站队列，避免转发环路 |
| I14 | domain 使用 64 位，无窄化成 int / uint32_t |
| I15 | 普通话题加入/离开不创建/关闭网关数据 socket |
| I16 | 不通过清空共享 socket 接收队列清理某个话题或消息 |
| I17 | 来源 ID 不使用 IP:port、PID、msg_id 或单独 sequence 代替 |
| I18 | 达到资源上限显式失败/丢弃并计数，不无界分配或反复创建线程 |

### 5.1 返回语义

新模式是显式选择的排队式传输，必须把以下契约写进用户说明。

| 接口 | shared_v1 中 true 的含义 | false / 失败边界 |
|---|---|---|
| publish / publish_best_effort | 完整消息已交给本机网关出站通道接管 | 提交前失败；后续 best-effort 丢弃通过指标可见 |
| publish_blocking(msg, tm) | 本机目标提交成功，且本次冻结的全部远端网关均确认；至少有一个目标 | 无目标、超时、配额、网关退出等；可能已有部分目标收到 |
| publish_prebuilt_segment | 合法完整 DZFlat 段已被出站通道接管，不允许再回退重发 | 仅在尚未提交时返回 false |
| has_subscribed | 网关已确认本机业务 SHM 可达订阅者，或未过期远端匹配订阅者 | 发现尚未完成、会话断开、没有匹配订阅 |

best-effort 的 true 不表示网关已发出 UDP，也不表示任何应用已经消费。
这是新模式的显式语义，不得把该返回边界反向修改到 legacy。
出站接管不是持久化。T05 须证明正常信用窗口内不会覆盖未读记录；若底层异常导致覆盖，
按第 9.5 节计数并使会话失效，可靠调用失败，不能补发为新 sequence 或伪造成功。

可靠发送的 true 仅确认目标网关已将消息提交到其业务 SHM。
它不保证各个应用回调完成，不保证慢订阅者不被 SHM 的既有策略丢弃。
不承诺跨网关崩溃的 exactly-once；不持久化历史；不会自动重放失败调用。

### 5.2 顺序语义

首版按同一发布者生成序号，用于身份和去重；**网络交付不承诺严格 FIFO**。
不同消息可因网络乱序、不同目标调度、重传而先后颠倒。
跨发布者也没有全局顺序。

本机应用若逐次调用 publish_blocking 并等待成功，再提交下一条，
在目标持续存活、路由不变的条件下可获得该调用链的先后关系。
不要把此特例写成所有 publish 都保证顺序。

需要严格网络 FIFO 的部署应继续使用已满足其要求的模式；
增加重排窗口、缺失序号跳过协议属于后续独立任务。

### 5.3 不能把部分提交伪装成可安全重试

内部结果至少分为：

- NotSubmitted：明确没有任何可见提交，可安全由调用者选择回退。
- Accepted：所有权已转交，不再由调用者重投。
- Indeterminate：底层可能提交，但当前无法确认。
- Completed / Failed / TimedOut：可靠事务完成结果，不表示历史投递可以撤销。

Indeterminate 必须中止该事务并计数。预构造段接口在所有权可能转交后不得返回 false；
返回已接管并暴露异步错误，或在进入公开路径前证明底层从不出现该状态。
第 T05 任务必须以真实跨进程故障用例证明该边界，不能靠布尔变量命名假设原子性。

## 6. 网络与本机身份

### 6.1 RouteKey

逻辑 RouteKey 由完整 channel_scope_token 和 msg_id 组成。
scope 内包含通道种类、64 位 domain 和双 64 位话题指纹。

- 首版只支持 ScopeKind::PubSub，收到 Service 必须拒绝。
- 网络固定头携带 kind、domain 和 16 字节话题指纹，能无损还原其逻辑 scope。
- 发现/注册同时交换完整 topic 字节串，用来检查指纹冲突。
- 同样指纹但不同完整 topic 必须报 TopicIdentityCollision，不允许选择其中一个。
- 完整名称验证同时覆盖发布来源和订阅目标；只交换订阅目录不足以证明远端 DATA 的来源名称。
  第 8.2 节目录因此也登记仅有发布者的路由，第 8.5 节规定入站核对顺序。
- 指纹不能作为安全认证；首版假设受信任局域网，数据仍必须做结构校验和配额检查。
- 一条实际 SHM 话题首版只允许一个非冲突 msg_id 和类型契约。
- 具体类型的非零 schema_hash 不一致则注册失败；GenericMessage 的零 schema 表示待校验，
  不能覆盖已确定的非零 schema。TLV 的类型名称只作诊断，不可伪称能证明类型相同。

RouteKey 的规范字节编码固定为 36B：`channel_scope_token[32] + u32_be(msg_id)`。
scope 的 32B 为 `DZS2[4] + u32_be(kind) + u64_be(domain) + fingerprint[16]`；
直接复用 channel_scope_token 的产出，不序列化 C++ 对象、不用十六进制文本参与 shard 哈希。
网络头省略的 DZS2 在解码后补回；scope[32] 出现于目录和 DZTX 时必须验证该 magic。
完整名称以原始字节重新计算两段指纹，与声明值逐字节比较，再检查冲突。
msg_id=0 是具体的消息 ID，不是通配符；schema_hash=0 才有前述“待校验”的含义。

协议允许的 1024B 名称上限与诊断池的 128B 数组是两个限制。内部路由始终保留完整名称；
诊断若只能展示截断名称，须带完整指纹并标明截断，查询使用原名或规范 RouteKey。
不能将 IpcInfoPool 中的截断名称回填到发现目录，也不能让同前缀名称合并为一个话题。

### 6.2 其他身份

| 名称 | 长度 | 分配规则 |
|---|---|---|
| gateway_id | 16 字节 | 本机非零 locality identity；控制握手验证应用与网关一致 |
| gateway_epoch | 64 位 | 每次网关启动生成新的非零随机值 |
| publisher_id | 16 字节 | 每个发布对象生成随机值；同会话重新注册幂等，换会话分配新值 |
| sequence | 64 位 | 从 1 开始递增；同发布对象发送入口串行分配，溢出换身份并重新注册 |
| session_id | 64 位 | 网关给控制会话分配；仅在本 gateway_epoch 内有效 |
| receiver_route_epoch | 64 位 | 目标网关 RouteKey 从无订阅到有订阅时分配；完全注销后不复用 |
| request_id | 64 位 | 本地控制会话中递增；用于匹配注册应答和可靠完成事件 |

相同主机 IP 不等于相同 SHM 可达性；容器、IPC namespace 或 UID 不一致不能合并 locality。
目标网关重启，旧 epoch 下的在途消息失败并返回 PeerRestarted；不搬到新 epoch 重发。

## 7. DZMX v1 数据与确认协议

### 7.1 通用编码规则

- 固定头 160 字节；所有整数均为网络字节序。
- 不发送 C++ struct 的内存，不依赖 packed、编译器布局或机器端序。
- 手工 put_u16 / put_u32 / put_u64 与 get 函数必须先检查范围。
- DATA 的单片载荷上限固定 1024 字节；最大 UDP 载荷 1184 字节。
- IPv4 常见 MTU 1500 可容纳；更小 MTU 路径不在首版自适应范围。
- recvmsg 检测 MSG_TRUNC，截断报文不能继续解析。
- 零长度业务 blob 不发送；最大业务 blob 默认 16 MiB，受第 12 节配额限制。
- HeaderCRC 不是认证；首版采用 CRC32C 检测报文与整包损坏。

CRC32C 固定采用 Castagnoli 反射多项式 0x82F63B78，初值与最终异或均为 0xffffffff；
`123456789` 的结果须为 0xe3069283。所有 CRC 字段仍用网络字节序写入。
packet_crc32c 覆盖实际头和 payload，将本字段四字节置零；message/body CRC 仅覆盖
约定的实际 blob/body，不包括 SHM 档位填充、UDP/IP 头或外层传输头。

### 7.2 固定头精确布局

| 偏移 | 长度 | 字段 | 规则 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII DZMX |
| 4 | 1 | version | 1 |
| 5 | 1 | packet_kind | 1=DATA，2=ACK，3=NACK，4=REJECT |
| 6 | 2 | flags | 首版为 0，未知位拒绝 |
| 8 | 2 | header_size | 160 |
| 10 | 2 | payload_size | 必须等于实际报文长 - 160，且 <=1024 |
| 12 | 4 | channel_kind | PubSub=1 |
| 16 | 8 | domain | 完整 uint64 |
| 24 | 16 | topic_fingerprint | scope 的两个话题指纹 |
| 40 | 16 | source_gateway_id | 当前发此报文的网关 |
| 56 | 8 | source_gateway_epoch | 当前发此报文的网关代次 |
| 64 | 16 | publisher_id | 原始数据发布者，ACK/NACK 同样保留 |
| 80 | 8 | sequence | 原始消息序号 |
| 88 | 16 | target_gateway_id | 此报文目标网关 |
| 104 | 8 | target_gateway_epoch | 此报文目标网关代次 |
| 112 | 4 | message_size | 完整业务 blob 的字节数 |
| 116 | 4 | fragment_index | DATA 从 0 开始；控制报文为 0 |
| 120 | 4 | fragment_count | 原始消息总分片数 |
| 124 | 4 | message_crc32c | 完整业务 blob 校验 |
| 128 | 4 | msg_id | 业务消息 ID，可为 0 |
| 132 | 4 | schema_hash | DZFlat 对应段头；TLV 为 0 |
| 136 | 8 | receiver_route_epoch | 原始数据接收侧的订阅代次；确认方向不交换它 |
| 144 | 4 | packet_crc32c | 将本字段置零，对完整头及本包 payload 计算 |
| 148 | 1 | encoding | 1=原始 TLV blob，2=完整 DZFlat 段 |
| 149 | 1 | delivery | 0=best-effort，1=reliable |
| 150 | 2 | reserved16 | 0 |
| 152 | 8 | reserved64 | 0 |

ACK/NACK/REJECT 反向交换 source / target gateway 字段，publisher / sequence / RouteKey、
message_size / message_crc / fragment_count / receiver_route_epoch 仍描述原始数据。
因此匹配确认时，原始发送网关等于确认头的 target，确认发送者等于原始接收网关。

### 7.3 DATA 分片算法

~~~text
fragment_count = ceil(message_size / 1024)
offset         = fragment_index * 1024
expected_size  = min(1024, message_size - offset)
~~~

要求：

- 用至少 64 位整数先检查乘法与边界，再转 size_t。
- fragment_count 必须等于由 message_size 算出的值。
- 每个分片都带全套身份和元数据，包括第一片之外的分片。
- 任意片可先到；第一次到达就能建立重组状态。
- 同消息出现相互矛盾的大小、编码、CRC、schema、片数，整条消息标记坏包并丢弃。
- 重复同一片且字节相同：只计数，不重复增加 received_bytes。
- 重复同一片但字节不同：丢弃该消息，不选择“最后一片为准”。
- 不把旧 TLV 页尾的 page_cnt / now_page 当作新网络分片身份。

### 7.4 重组键和状态

AssemblyKey：

~~~text
RouteKey
+ source_gateway_id
+ source_gateway_epoch
+ publisher_id
+ sequence
+ local gateway_epoch
+ receiver_route_epoch
~~~

每个 AssemblyState 保存：

- 固定元数据副本；
- 已接收位图；
- 有限的载荷存储或 SHM staging loan；
- 收到的片数与字节数；
- first_seen / last_progress / next_nack / deadline；
- 当前阶段：Collecting / CommitPending / Committed / Rejected。

重组默认绝对存活上限为 first_seen + 5000 ms；last_progress 不得无限延长绝对期限。
best-effort 连续 500 ms 无新片可提前丢弃；reliable 使用 NACK / 最终绝对期限决定结束。
这些时间不等于发送者的剩余 deadline，发送者仍按自己的更早 deadline 终结。

可复用现有 CRC32C 实现，但不复用旧 chunk_rev_topic 的阻塞收包循环。
首版允许先在网关私有内存重组，再复制一次进业务 SHM。
“直接重组到借用 SHM chunk”只作为有独立基准证据的后续优化。

### 7.5 ACK / NACK / REJECT

ACK：

- payload_size=0。
- 仅在确定 SHM 提交成功后发送。
- 重复完整消息或重复分片命中 Committed 时可以重发 ACK，但有速率限制。

NACK：

- payload 是递增且不重复的 uint32 缺失片号列表，最多 256 个。
- payload_size 必须是 4 的倍数且大于 0。
- 缺片更多时逐次分组请求，不分配超大控制包。
- 首次缺片等待 20 ms 后请求；有进展可继续等待，重发间隔至少 20 ms。
- 丢了所有分片时接收侧无法 NACK，发送侧必须有整体探测重发定时器。

REJECT：

- payload 是 4 字节 reason_code。
- 1=UnknownRoute；2=RouteEpochMismatch；3=QuotaExceeded；4=BadMetadata；
  5=ShmUnavailable；6=ShmCommitIndeterminate；7=UnsupportedEncoding。
- 只向已确认的 peer 控制地址发送，不使用包内随意指定的第三方回包地址。
- best-effort 可只记丢弃指标，避免对大量坏包回报形成控制风暴。

### 7.6 去重窗口的正确性

不能简单“TTL 到期删除消息 ID，后面又当新消息”。

首版每个接收 stream（RouteKey + source_gateway_id + source_gateway_epoch + publisher_id
+ receiver_route_epoch）维护 4096 个序号的状态窗口和最高序号。
状态用两个位图或等价有界结构表示，不能用一个 bool 同时表示成功和拒绝。

- 窗口内：准确记录 Committed / Rejected / InProgress。
- 低于窗口下界且没有有效的可靠回执/在途记录：一律视为 stale，不重新提交；
  必要时返回失败，不能伪造成功 ACK。
- 序号上移时，不能挤掉尚在有效 reliable 期限内的状态而允许其重投。
- 状态容量不足时拒绝接纳新事务，不通过静默删去重项腾空间。
- peer epoch 或本机订阅代次改变后旧包在入口即拒绝。
- stream 注销后保留拒绝旧 epoch 的路由墓碑，或保证旧路由已经不可匹配。
- 首版每发布者可靠在途上限 64，另维护有配额的可靠回执表，回执至少保留到
  first_seen + 5 秒。窗口滑动不能清除该期限内的 Committed 记录。
- 收包依次查：可靠回执表、在途表、滑动窗口。旧 reliable 的 ACK 重试可命中回执表，
  即使大量 best-effort 消息已经推高序号，也不能因此重复提交。
- 新订阅者可能在发布者已经发出数百万条后才加入；首次有效非零 sequence 可以是任意值，
  以它建立窗口。不能要求首次 sequence=1 或小于 4096。
- 后续大跳跃只移动固定大小的窗口，不按序号差申请数组；在途与回执继续独立保活。
- sequence=0 拒绝。完成回执配额不足时拒绝新 reliable，不能先提交再发现无处记录去重。

stream 的最高序号与窗口不能因“空闲 TTL 到期”“发布者从目录消失”或 peer 租约过期就删除。
同一源 gateway_epoch 和 receiver_route_epoch 以后仍可能出现相同消息；只释放大 buffer
不等于可以忘记已提交事实。回执到期后仅释放回执，窗口中的成功/拒绝标记继续有效。
仅在本机 route epoch 已永久失效，或源 epoch 已按第 8.5 节永久拒绝后，才可回收相应 stream。
容量满时拒绝新 stream，不采用 LRU 淘汰仍可匹配的旧 stream；长期发布对象反复重建会
消耗 stream 配额，这是首版需要暴露和容量规划的限制。

### 7.7 入站检查顺序与状态不可逆边界

所有数据 shard 使用相同校验顺序；在前置检查通过前不分配重组 buffer：

1. 检查报文实际长度、MSG_TRUNC、magic、版本、固定头长、kind、保留位和 packet CRC。
2. 验证 target_id / target_epoch 等于本实例，并核对已接纳 peer 的源 IP 和实际 UDP 源端口。
3. DATA 源端口必须等于按 RouteKey 和源 peer 的 K 算出的源数据端口；目标接收 FD
   必须等于按本机 K 算出的目标 shard。发到错误 shard 的报文丢弃并计 wrong_shard，
   不在另一 shard 建第二份 assembly。两端 K 不同仍按各自 K 计算。
4. 按第 8.5 节核对源发布目录、完整名称、目标 Ready route epoch 与类型契约。
5. 验证 encoding / delivery、消息大小、片数及片偏移，再查询回执、在途表和窗口。
6. 仅为首次接纳的合法消息预留全部必需配额；任一预留失败则全部回退。

ACK/NACK/REJECT 仅从控制 FD 接收，源 IP / port 必须匹配该 peer 的控制地址。
确认必须匹配原事务的全部固定元数据（含 encoding、delivery、schema 和 CRC），
不能只凭 publisher_id + sequence 终结；未知事务、错误方向和晚到确认仅计数。
只有 reliable 使用这些确认；不为 best-effort 创建可靠等待或回执。

Collecting 中元数据或同片内容冲突，转 Rejected 并保留去重标记；CommitPending 已交接后，
冲突包只能被拒绝，不能清掉 ticket 并重建 assembly。Committed 后元数据不一致不回成功 ACK。
这样坏包、timeout 和重复 DATA 都不能绕过第 10.4 节的一次提交边界。

## 8. 发现、发布来源与订阅路由

### 8.1 发现只介绍网关

保留旧发现通道原样。新发现使用 239.255.250.251:24005，TTL=1。

HELLO 固定 64 字节，使用独立 magic DZGD：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 4 | DZGD |
| 4 | 1 | 版本 1 |
| 5 | 1 | 类型 1=HELLO |
| 6 | 2 | 长度 64 |
| 8 | 16 | gateway_id |
| 24 | 8 | gateway_epoch |
| 32 | 2 | data_base_port |
| 34 | 2 | data_shards K |
| 36 | 2 | control_port |
| 38 | 2 | capability_flags，首版 bit0=shared_v1 |
| 40 | 8 | 当前 subscription_snapshot_version |
| 48 | 4 | max_message_bytes |
| 52 | 4 | CRC32C，计算时该字段置零 |
| 56 | 8 | 保留 0 |

远端 IPv4 从 recvmsg 源地址获得，不信任报文声明的另一 IP。
每 1 秒广播 HELLO，网关启动立即广播；3 秒未见则 peer 过期。
自有 gateway_id + epoch 的 HELLO 忽略。
接口由配置显式指定，不把 0.0.0.0 当作远端可达地址发布。

### 8.2 路由目录快照

收到新 HELLO、版本变动或主动刷新时，通过远端控制端口请求完整路由目录快照。
首版按完整快照替换，不做难以恢复的增量链。
目录同时描述发布来源和 Ready 订阅需求；HELLO 中沿用字段名 subscription_snapshot_version，
但发布角色的增删也必须推动它递增。仅有 PUB 角色的条目绝不能成为数据发送目标。

快照协议使用独立 magic DZGC，不能冒充第 7 节的 ACK。
公共头：magic 4B、version 1B、kind 1B、header_size 2B、payload_size 2B、
reserved 2B、source_id 16B、source_epoch 8B、target_id 16B、target_epoch 8B、
snapshot_version 8B、page_index 4B、page_count 4B、body_crc32c 4B、
packet_crc32c 4B，共 84B，网络序。

kind：1=SNAPSHOT_REQUEST，2=SNAPSHOT_PAGE。
完整快照 body 按 RouteKey 排序编码：

~~~text
u32 route_count
重复 route_count 次：
  scope[32]
  u32 msg_id
  u32 schema_hash
  u64 receiver_route_epoch
  u16 topic_name_bytes
  u16 role_flags（bit0=存在已注册 PUB，bit1=存在 Ready SUB）
  topic_name 原始字节（不含结尾零）
~~~

- role_flags 只允许 1、2、3；同一 RouteKey 合并成一条。带 SUB 位时 route epoch 必须非零，
  仅 PUB 时必须为 0；不公开每个本机句柄。schema 契约仍按第 6.1 节合并。
- topic_name 长度上限 1024 字节，拒绝嵌入 NUL；不做斜杠替换或大小写归一。
- body 按最多 1024B 分页；条目允许跨页，先完整收齐再解析。
- body_crc32c 为完整 body 的校验，每页相同。
- REQUEST 的 payload 为空，page_index / page_count / body_crc 为 0。
- 版本不同的页面不能拼在一起；相同 page_index 重复内容冲突则丢弃候选快照。
- 收齐所有页面并校验后，原子替换该 peer 的路由表。
- 未收齐不发布半张路由表；旧快照在 peer 租约内可保留。
- 候选快照默认 2 秒过期；请求重试间隔从 100 ms 退避到 1 秒。
- 发送端按控制预算分批发页，不一次淹没控制队列。
- 每 peer 最多一份候选快照，body 上限 8 MiB；总候选内存上限 64 MiB。
- 注册前计算新快照大小；超过公告能力上限则拒绝该注册，不能创建无法公告的 Ready 话题。
- snapshot_version 单调递增，溢出通过重启 epoch 处理，不回绕比较。
- 收到新 gateway_epoch 先清旧 peer 路由，再请求新快照。
- peer 3 秒过期后从新的目标快照中剔除；在途 reliable 返回 PeerGone。
- PAGE 的 page_count 必须为 1～8192，page_index 小于 page_count；
  除最后一页外 payload 必须正好 1024B，最后一页为 1～1024B。
  接收前验证 page_count 与最大 body 配额，防止先按攻击性页数分配内存。
- page_count * 1024 用 64 位计算；最终 body 长度由最后一页推导，再检查 <=8 MiB。
  route_count、每个字符串长度、结尾是否恰好用完全部 body 也必须验证。
- REQUEST 的 snapshot_version 表示请求者最近看到的版本；应答发送时抓取一份不可变完整快照，
  所有页都使用它的版本，即使发送过程中本机订阅又发生变化。
- 旧版本页面不能覆盖已经安装的新版本；同 epoch 中按 uint64 普通大小比较，禁止回绕。
- body 必须严格按 36B RouteKey 字节序递增、无重复；空目录仍编码 u32(0)，占一页 4B。
  新候选版本不得低于当前候选版本，旧 PAGE 不能反复替换正在收取的新候选。
  已安装目录、候选目录和正被发送者持有的不可变旧目录分别计入第 12.4 节内存配额。

### 8.3 本机订阅的 Ready 时机

注册必须是两阶段：

1. 应用连接控制会话并发 REGISTER_SUB，网关校验 RouteDescriptor。
2. 网关创建/加入该话题的内部 MPMC SHM 发布端，回 REGISTERED。
3. 应用初始化并确认自己的 shm_sub_ipc 已连接正确 generation。
4. 应用发送 SUB_READY。
5. 网关只有收到 SUB_READY 后才把它计入订阅数并更新快照版本。

不能把“控制连接建立”或“构造订阅对象”当作网络可投递。
最后一个 Ready 订阅者离开后清除目录 SUB 位、撤销订阅需求并使该 receiver_route_epoch 失效；
若仍有发布者则保留 PUB 条目，不能把发布来源一同撤销。
重新出现订阅者分配新的 route epoch。

receiver_route_epoch 属于整个 RouteKey，不属于单个订阅对象。只要仍有 Ready 订阅者，
另一个订阅者加入/退出不会换 epoch；新加入者可能读到入站已在途、随后才 SHM commit 的消息。
SUB_READY 只控制网络需求公告，不提供“只收构造时刻之后发布的数据”或独立历史隔离。
全部 Ready 订阅退出再出现才有旧 epoch 隔离；仍在 Committing 的工作需按第 10.4 节
先收敛，再确认新订阅 generation 可用，不能把旧提交写入已重建的业务通道。

同机额外的 IPC_SHM 订阅者可被业务 SHM 投递覆盖，但它们单独出现时不自动成为远端
发现订阅者。远端公告要求至少一个 SharedSubscriber 或网关显式订阅登记。
本机由 SharedPublisher 提交的消息，则还应检查业务 SHM 的有效 receiver 状态，
允许已经接入相同 MPMC 话题的显式 IPC_SHM 订阅者接收；这些接收者不被自动公告为远端需求。
工具必须通过 REGISTER_SUB + SUB_READY 表达需求，不能只偷偷打开 SHM 段。
网络订阅代次与 bridge 的生命周期分别计数：只有本机 PUB、待完成的 SUB 登记、Ready SUB
和在途 lease 都已归零，才释放内部 SHM 发布端。不能在最后一个 SharedSubscriber 离开时
关闭仍供 SharedPublisher 向显式 IPC_SHM 订阅者投递的 bridge。

### 8.4 发送目标选择

- 每条出站记录被网关接纳处理时冻结目标：本机有效业务 SHM 接收面 + 匹配的远端快照。
- 不包括自己，不按远端应用订阅者数重复发。
- reliable 不在发送中途增加新目标，不因某目标退出就把“全部成功”改为剩余目标成功。
- 远端路由 epoch 变化使旧事务明确失败，新 publish 才使用新 epoch。
- 首版无订阅时 best-effort 可接管后丢弃，计 no_route_drop；blocking 返回 NoSubscribers。
- has_subscribed 反映已同步控制状态，是瞬时快照，不能充当后续投递保证。

### 8.5 发布来源核验与 peer 代次保留

接收方只有在源 peer 的已安装目录中找到带 PUB 位的相同 RouteKey，并确认其完整 topic
与本机订阅名称一致时，才接纳 DATA。来源无需在本机有同名订阅，因此不能靠订阅公告
间接推断发布来源。源目录内无 PUB、源目录尚未同步或名称/schema 冲突时均不建立 assembly。

- 未同步来源目录时丢弃 DATA，并对已知 peer 合并触发一次快照请求；请求有重试退避和配额，
  不能为每个分片发一次请求或缓存无界“待确认”数据。reliable 在原期限内重试，best-effort
  可以丢失此阶段消息；PUB_REGISTERED 不承诺所有远端已完成发现。
- 已知源条目与本机完整名称相冲突时记录 TopicIdentityCollision，保持两条登记不互通；
  已 Ready 的本机订阅不会被远端坏目录改名。V05 必须包含“远端只有发布者”的情况。
- 接收到自己的 gateway_id、不同 epoch 的 HELLO，或相同 id/epoch 来自不同 IP，报告
  GatewayIdentityConflict，不当作新远端接纳。首版一个实例只公布一个 IPv4；不自动迁移地址。
- 对已知远端，接纳新 epoch 前先将旧 epoch 永久标为 retired，撤销旧路由并终结旧事务。
  延迟的旧 HELLO/PAGE/DATA 不能让 peer 回退到旧 epoch；随机 epoch 不能用数值大小判断新旧。
- peer 的 3 秒租约过期只撤销可达性和新发送目标，不删除去重历史。相同 epoch 恢复时，
  重新取得完整目录，沿用去重状态；不同 epoch 恢复时执行上述 retired 流程。
- peer 身份历史包括当前、租约已过期及 retired 代次，在本机网关存活期内不因 TTL/LRU
  被淘汰；总量与每 peer 均有限额。
  无空间保存必要记录时拒绝接纳新 peer/epoch 并报告 PeerHistoryFull，不能先删除旧记录。
  本机重启产生新 target_epoch 后可清除旧历史，旧报文仍会在目标 epoch 检查处被拒绝。

这些检查防止意外串话、旧包回流和配置冲突；不构成对恶意局域网参与者的密码学认证。

## 9. 本机控制与出站 SHM

### 9.1 Unix 会话与权限

- 默认控制路径为显式 DZIPC_GATEWAY_CONTROL；部署示例使用 /tmp/dzipc-gateway-UID/control.sock。
- 网关创建实例目录时设置 0700，socket 设置 0600，校验目录拥有者与类型。
- 锁路径位于同一目录，使用 flock(LOCK_EX | LOCK_NB)，锁 FD 始终保持打开。
- 进程活着但 SIGSTOP 时不会被另一个网关接管；锁竞争者立即退出。
- 锁释放后才允许下一实例清理已证明失效的 control.sock；不能 unlink 活网关 socket。
- 使用 SOCK_SEQPACKET 保存消息边界、SO_PEERCRED 验证同 UID。
- SCM_RIGHTS 仅用于握手传一个出站 eventfd；不为每条消息传 FD。
- 每个控制包上限 8192B；结构化解码，禁止传递原生指针或 STL 对象。
- 运行时路径、SHM 名称和 session_id 由握手确认，应用不接受外部任意段名。

### 9.2 控制消息

本机控制头也显式编码，固定 40B：

~~~text
magic[4] = DZLC
u16 version = 1
u16 kind
u32 total_size
u32 flags = 0
u64 request_id
u64 session_id
u64 gateway_epoch
~~~

必需消息：

| 消息 | 请求内容 | 应答 / 效果 |
|---|---|---|
| HELLO / WELCOME | locality_id、支持版本、进程 start token、能力 | session_id、epoch、容量、出站段名 |
| ATTACH_TX / TX_READY | 出站 SHM 已打开；通过 SCM_RIGHTS 传 eventfd | 网关 receiver 已连接，允许发布 |
| REGISTER_PUB | 完整 RouteDescriptor、publisher_id | 成功或具体冲突原因 |
| REGISTER_SUB | 完整 RouteDescriptor、订阅句柄 ID | 注册成功及当前 SHM generation |
| SUB_READY | 订阅句柄 ID、确认 generation | 开始网络公告 |
| UNREGISTER | 句柄 ID、角色 | 撤销并等待相应在途使用结束 |
| QUERY_STATE | 可选 RouteKey | Ready 数、peer 数、配额和健康状态 |
| SEND_BEGIN / BEGIN_READY | publisher_id、sequence、payload_size、encoding、schema、绝对单调时钟 deadline | 为可靠记录登记等待与预留配额 |
| SEND_RESULT | publisher_id、sequence、request_id、结果码 | 完成可靠等待 |
| TX_PROGRESS | 累计已释放记录数与 loan 容量字节 | 向应用归还出站信用，不表示业务投递成功 |
| PING / PONG | 时间戳、会话状态 | 控制连接健康检查 |
| ERROR | 明确错误码与有界诊断字符串 | 不能只回 false 或空报文 |

RouteDescriptor 编码重用第 8.2 节条目，注册时 receiver_route_epoch 和 role_flags 均填 0；
角色由 REGISTER_PUB / REGISTER_SUB 的消息编号决定，目录角色由网关汇总生成。
所有变长字符串前置 u16/u32 长度；实施 T02 时为每个消息固定 golden bytes。
消息缺字段、尾部多余字节、无效角色、重复 request_id 必须有确定处理规则。

固定消息编号：HELLO=1、WELCOME=2、ATTACH_TX=3、TX_READY=4、REGISTER_PUB=5、
PUB_REGISTERED=6、REGISTER_SUB=7、SUB_REGISTERED=8、SUB_READY=9、SUB_READY_ACK=10、
UNREGISTER=11、UNREGISTERED=12、QUERY_STATE=13、STATE=14、SEND_BEGIN=15、BEGIN_READY=16、
SEND_RESULT=17、PING=18、PONG=19、ERROR=20、TX_PROGRESS=21。
编号不可在实现中按 enum 声明顺序隐式变化。

成功应答使用对应编号；失败统一 ERROR，并携带原 request_id。
SUB_READY、UNREGISTER 等请求的重复必须按句柄和代次幂等处理；重复不会增加引用计数。
已有对象 InitChannel 重复调用应先结束旧注册再建立新注册，不能留下两份登记。

最低 body 契约：

| 消息 | body 字段顺序 |
|---|---|
| HELLO | locality[16]、u64 process_start_token、u64 clock_domain_id、u32 capabilities |
| WELCOME | locality[16]、u32 max_message_bytes、u32 outbox_limit_bytes、u32 outbox_record_limit、u16 tx_name_len、tx_name |
| ATTACH_TX | 空 body，附带恰好一个 eventfd；重复 ATTACH 必须关闭多收到的 FD |
| REGISTER_PUB / REGISTER_SUB | handle_id[16]、RouteDescriptor；PUB 的 handle_id 等于 publisher_id |
| PUB_REGISTERED / SUB_REGISTERED | handle_id[16]、u32 shm_generation、u64 receiver_route_epoch |
| SUB_READY | handle_id[16]、u32 shm_generation |
| SUB_READY_ACK / UNREGISTERED | handle_id[16] |
| UNREGISTER | handle_id[16]、u8 role（1=PUB，2=SUB） |
| SEND_BEGIN | publisher_id[16]、u64 sequence、u32 payload_size、u32 schema_hash、u8 encoding、u8 reserved=0、u16 reserved=0、u64 deadline_monotonic_ns |
| BEGIN_READY | publisher_id[16]、u64 sequence |
| SEND_RESULT | publisher_id[16]、u64 sequence、u32 result_code、u32 flags、u32 target_count、u32 acked_count |
| TX_PROGRESS | u64 released_records、u64 released_capacity_bytes；会话内累计值，request_id=0 |
| QUERY_STATE | u8 kind（0=汇总，1=单话题，2=话题与 peer）；kind=1/2 后跟 scope[32]、u32 msg_id；kind=2 再跟 peer_id[16] |
| STATE | u32 json_bytes、UTF-8 JSON；含头总长度不得超过 8192B |
| PING / PONG | u64 nonce，PONG 原样返回 |
| ERROR | u32 error_code、u16 text_bytes、UTF-8 有界诊断字符串 |

WELCOME / TX_READY 中实际 session_id 和 gateway_epoch 使用控制公共头。
TX_READY body 为空；HELLO 发出时头中的 session_id / gateway_epoch 为 0，之后必须匹配会话。
clock_domain_id 来源于可验证的本机时钟 namespace 身份；没有 time namespace 功能的内核使用
明确的共同标记。网关结合 SO_PEERCRED 验证，不能只信任客户端声明。

SEND_RESULT.flags 的 bit0 表示 local_commit_success，bit1 表示 possible_partial_delivery；
其余位为 0。target_count / acked_count 仅统计远端网关，本机结果单列。
result_code：0=Completed、1=NoSubscribers、2=Busy、3=TimedOut、4=PeerGone、5=PeerRestarted、
6=Rejected、7=GatewayLost、8=Cancelled、9=Indeterminate、10=UnsupportedTimeout。
ERROR 的错误码由 T02 编码头定义并与第 14 节错误字典同步，不混用 errno 数值。

possible_partial_delivery 必须保守计算：已发送任何 DATA、已有本机提交成功，或本机 ticket
已进入 Committing/Indeterminate 后失败，即使 acked_count=0，也不能向调用者宣称没有投递。
共享 pub/sub 公共 bool 接口仍返回 false；具体结果通过本机会话诊断保留 publisher/sequence
和 request_id，不能因缺少扩展返回类型便把不确定性隐藏掉。失败不触发库自动重发。

Unix recvmsg 同时检查 MSG_TRUNC 和 MSG_CTRUNC；异常消息附带的 FD 必须全部关闭。
仅 ATTACH_TX 接受恰好一个 SCM_RIGHTS FD，其余消息携带 FD 一律拒绝。
控制 FD、传入 eventfd 和锁 FD 均设置 CLOEXEC；eventfd 为非阻塞，不能阻塞发布线程。

### 9.3 每进程一条出站 SHM

首版使用独立 ipc::route：应用是唯一 sender，网关是唯一 receiver。
同进程多个发布线程只在提交环节用短互斥串行化；序列化和等待可靠完成在锁外。

使用单 sender route 的原因是每进程只需一个提交者实例。
不要因“多发布者”三个字就让每个发布对象各建一个底层 sender。
业务 SHM 仍使用前置任务的 MPMC 通道，二者不共用段名或控制面。

段名：

~~~text
dzgw_tx_v1_<locality_hex>_<gateway_epoch_hex>_<session_id_hex>
~~~

段名包括所有权代次，不复用旧 session 的段。
网关退出后新实例使用新 epoch，不能重连旧出站段并重放残留数据。

创建顺序：

1. WELCOME 给出唯一名称。
2. 应用创建一个出站 sender，并创建 eventfd。
3. 应用发送 ATTACH_TX；网关打开 receiver 并登记唤醒 FD。
4. 网关回 TX_READY。
5. 应用开始发布。TX_READY 前禁止写记录。

退出顺序：

1. 停止接收新 publish；
2. 完成或取消本进程可靠等待；
3. 注销逻辑句柄；
4. 网关摘除 eventfd 并等待相应处理结束；
5. 双方释放出站通道和 eventfd；
6. 仅按本会话所有权回收独占段，不清理业务 SHM。

### 9.4 出站记录

固定前缀 96B，显式使用网络字节序；不发送原生 C++ 结构体。

~~~text
0   magic[4] = DZTX
4   u16 version = 1
6   u16 header_size = 96
8   u64 gateway_epoch
16  u64 session_id
24  publisher_id[16]
40  u64 sequence
48  scope[32]
80  u32 msg_id
84  u32 payload_size
88  u32 schema_hash
92  u8 encoding
93  u8 delivery
94  u16 reserved = 0
96  完整业务 blob
~~~

可靠 timeout 和 request_id 不放在固定前缀里：增加本机 SEND_BEGIN 控制请求，
按 publisher_id + sequence 登记 request_id、目标超时时刻，然后回 BEGIN_READY。
仅 BEGIN_READY 后才提交 reliable 出站记录。超时从公开调用开始计算，包含编码和排队。
deadline 使用 Linux CLOCK_MONOTONIC 的 uint64 纳秒时间，同机跨进程可比较；
不发送 std::chrono::time_point 的内存表示，不把其传给远端。网关重新检查 deadline 未过期。
若平台启用了不同 time namespace，应用与网关必须验证时钟域一致；不能一致时拒绝
shared_v1 会话并报告 UnsupportedClockDomain，不直接比较两个时钟域的绝对时间。

- best-effort 不需要 SEND_BEGIN。
- reliable 先登记等待再发布，避免极快 SEND_RESULT 早于等待者注册。
- BEGIN_READY 后应用未提交而退出/超时，网关回收事务占位。
- 每 publisher/session 同时进行的 BEGIN 数受 reliable in-flight 上限约束。
- SEND_BEGIN 只传元数据，不传业务载荷。
- payload_size 等字段在 BEGIN_READY 前就必须明确，网关据此预留发送缓存；
  后来的出站记录必须与已登记值逐字段一致。
- 相同 request_id 和完整相同内容的 SEND_BEGIN 重试是幂等的，不重复扣配额；
  同 request_id 内容不同，或 publisher_id + sequence 被另一个请求占用，返回 RequestConflict。
- 明确编码失败可以在 SEND_BEGIN 前结束；不能占着 BEGIN 配额无限等待编码完成。

网关收到 ipc::buff_t 时，其 size 可能是尺寸档位容量，不能要求它恰好等于 96+payload_size。
先用 64 位运算验证 `96 + payload_size <= buffer_capacity`，再只取这个范围；容量尾部不是
第二条记录，也不能进入网络 blob 或 CRC。若 buffer 小于信封声明长度则拒绝，不能补零。

### 9.5 提交与唤醒

- loan 一块可容纳 96B 头和 blob 的出站 chunk，填完后 publish_loan。
- 网关接管后拥有 buffer 的生命周期；应用不得修改或立即回收已发布 chunk。
- 底层 loan / publish_loan 的真实失败可见性必须先在 T05 验证。
- 不能用可部分发送多段的大记录 try_send 直接假装是一次原子提交。
- 如果现有 loan 无法满足记录大小和失败边界，T05 停在该阻塞点，补原子描述符提交适配；
  不用“暂时经 Unix socket 传载荷”冒充实现完成。
- 成功提交后 eventfd_write(1)，失败不产生业务唤醒。
- 网关读 eventfd 后 drain SHM 到预算；预算未耗尽且队列空才等待。
- 预算耗尽但还有记录时加入本地 deferred 队列，不能依赖将来的下一次 eventfd 通知。
- eventfd 计数不是消息数；合并通知、重复通知和 EAGAIN 都不改变队列事实。
- 网关还要验证记录中的 session、epoch、publisher_id、scope、msg_id 与控制登记一致。
  一个会话不能通过伪造 DZTX 头向未注册的话题发送。
- 出站按 loan 实际容量和记录数同时预留信用，提交前失败归还；不能在 publish 返回时就
  当作队列已被消费。WELCOME 的 outbox_record_limit 不得大于 T05 验证的底层可用槽位。
  所有本会话发布线程共用账本，计入尚未完成提交的预留，避免并发穿透额度。
- 网关复制或丢弃记录并释放 loan 后累计 TX_PROGRESS，可合并到每轮 drain 结束发送，
  最迟 20 ms 发出且最后一批也必须发送。应用只应用累计值的增量，重复/旧通知不重复归还。
  值不能超过本会话已预留提交的总量；计数溢出前关闭会话，不回绕。
- T05 必须证明信用窗口内不会因 force_push 覆盖未读记录；若仍检测到覆盖/进度缺口，
  会话明确失效并结束可靠等待，不能猜测被覆盖记录的容量来恢复信用。进程或网关退出后
  通过独占会话清理结算剩余 loan，旧信用不迁移到新会话。
- 首版网关将出站记录复制到受配额管理的 WireBlob 后立即释放出站 loan，避免重传长时间
  阻塞应用出站池。复制期间两份内存都是真实占用，必须纳入相应配额。
  将来若改成直接持有出站 loan，需要另测背压与崩溃生命周期，不能直接删除这次复制。

## 10. 原始载荷与业务 SHM 注入

### 10.1 只保留两种业务 blob

DZFlat：

- 完整 SegHeader + 段数据；
- 验证 magic、layout、总长度、root 范围、msg_id、schema_hash；
- 不将 DZFlat 转成 TLV 再转回；
- GenericMessage 持有平坦段时直接保留段，不调用其 serialize() 假装能转 TLV。

TLV：

- 保留现有 IpcMsgBase::serialize() 输出的全部字节，包括历史页尾；
- 新 UDP 按 1024B 切片仅作外层传输；
- 网络重组后得到与编码时完全相同的 blob；
- 不调用旧 chunk_send_ex 去“修正页号”，不在网关重复反序列化/序列化。
- 在本机订阅者使用现有 AcceptWire / deserialize_ok 做最终类型与结构校验。

编码函数建议统一成 WireEncoder::encode / encode_prebuilt，返回拥有生命周期的 WireBlob。
最大尺寸、空指针、无效 schema 的失败发生在出站提交前。

### 10.2 新增内部注入接口

建议新建 ShmWireBridge，并通过明确的内部访问接口复用 shm_pub_ipc：

~~~text
open(RouteDescriptor) -> Ready / MpmcRequired / TypeConflict / Failed
try_commit(WireBlob)  -> NotSubmitted / Committed / Indeterminate
close_after_quiescent()
~~~

落实要求：

1. 通过 GenericMessage 模板和注册的 msg_id 创建内部 shm_pub_ipc，标记 internal。
2. 生命周期、MPMC 发布者注册、心跳、借样池、generation 都使用现有实现。
3. DZFlat 原样复制到业务 SHM loan，提交后由订阅者借样。
4. TLV 原样写入同一业务 SHM 通道，不经过 GenericMessage 解析后再序列化。
5. 注入访问使用新增非虚内部方法或 friend 适配器，不修改公共虚表。
6. 不将业务 payload 包在 DZMX / DZTX 头里写入原业务通道。
7. 不走 nodelet 本地注册表扇出；网关必须让其他进程可见。
8. 不在失败时自动切换编码或换另一条投递路径。
9. 严格确认借样提交失败是否仍需 discard，复用前置已验证的所有权契约。
10. 网关对同一 RouteKey 只维护一个内部 SHM 发布者，不按远端来源创建发布者。

### 10.3 ACK 与注入结果

- Committed：记录去重成功，发送 ACK。
- NotSubmitted：可在保留的有界 CommitPending 队列中重试，或明确 REJECT。
- Indeterminate：标记本消息拒绝/未知，发送 REJECT(ShmCommitIndeterminate)，禁止再次提交。
- 源网关的本机 reliable 注入受公开调用 deadline 限制；远端接收网关仅使用自己的
  first_seen + 5000 ms 重组/注入期限，不能读取或推算发送端绝对 deadline。
- best-effort 注入失败直接计数并丢弃。
- 在可靠等待期间，不长期占用业务 SHM 的读端 loan；发送重传缓存使用独立 WireBlob。
- 应用持有 Sample 时网关退出，Sample 仍须有效到其既有租约结束。

DZMX v1 不携带远端取消或调用剩余时间。发送者超时会停止后续发送并返回失败，
已经发出的报文仍可能随后重组并提交；超时返回不是撤销投递，也不是“此后不会再收到”。
两端时钟无需同步；不能把本机 SEND_BEGIN 的单调时钟纳秒值拿到另一主机比较。

### 10.4 提交线程与超时的交接协议

每次完整消息投递创建一个有所有权的 DeliveryTicket，包含 AssemblyKey、WireBlob、
route lease、配额令牌和原子提交状态。禁止仅把 payload 指针压入队列。

状态只允许：

~~~text
Ready -> Committing -> Committed
                    -> NotSubmitted
                    -> Indeterminate
Ready -> Cancelled
~~~

1. 接收 shard 创建 Ready 并将同一个 ticket 交给提交队列。
2. 提交循环执行 CAS(Ready, Committing)，失败则不访问业务 SHM。
3. 该 CAS 成功后只调用一次非阻塞 try_commit，并产生唯一 CommitResult。
4. shard 超时/注销时可以 CAS(Ready, Cancelled)，成功才保证此次没有发生提交。
5. 若已经 Committing，不能宣称“尚未提交”，也不能删除原状态再接受相同消息。
   保留 ticket / 去重位置，待提交结果回来；上游发送者仍可按自己的 deadline 返回超时。
6. Committed 先保留成功回执，再通知网络控制循环发 ACK；发送者已超时可忽略此晚 ACK。
7. NotSubmitted 的重试需要由所属 shard 将原 ticket 纳入新的明确尝试，
   同一时间最多一个 Committing，不能由提交循环自行重新排队并发重试。
8. Indeterminate 保留拒绝/不确定墓碑，永不自动再次提交相同消息。
9. 所有终结路径最后释放配额和 blob；还有在途 ticket 时 route / buffer 必须存活。

必须新增确定性竞态测试：让提交线程停在 CAS 前、CAS 后、SHM 提交后、结果回传前，
分别触发 timeout / unregister / 重复 DATA，检查实际业务 SHM 投递次数不超过一次。
测试暂停点只存在于测试适配层，生产路径不能睡眠等待测试信号。

## 11. 共享 IO、调度与可靠发送

### 11.1 固定有界线程模型

首版固定采用：

- 1 个控制循环：Unix 会话、发现、网络控制、定时器和快照；
- K 个数据 shard 循环：每循环独占一条数据 UDP socket，发送、收包和重组；
- 1 个本机提交循环：出站 SHM drain 与业务 SHM CommitPending；
- 1 个有界初始化工作线程：执行可能等待的 SHM open / InitChannel，完成后移交 Ready 对象；
- 线程间使用有界队列 + eventfd 唤醒；
- 不执行用户回调，不创建每 topic / publisher / message 线程。

现有 SHM 发布者生命周期可能使用共享调度器，应报告其线程，
不能把这些额外线程漏掉来声称只有 K+3 条线程。K+2 仅是 UDP socket 数。
初始化任务队列默认 64 项，单次注册等待默认 2 秒；满时返回 Busy，超时取消该注册请求。
初始化工作线程只能使用已经有界的前置 SHM 初始化 API；无法取消的无限等待是 T00/T06 阻塞项。
初始化超时/注销后，迟到的成功结果必须撤销内部发布者并释放资源，不能重新发布 Ready 或恢复公告。

所有业务 SHM 注入只在本机提交循环执行，避免某话题多次提交竞争。
网络 shard 只向提交循环投递完整消息和稳定身份。

### 11.1.1 状态归属表

| 状态 | 唯一写入者 | 其他线程如何访问 |
|---|---|---|
| session、peer、已公告路由、快照版本 | 控制循环 | 不可变快照或控制命令 |
| 某数据 FD 的收包与 AssemblyState | 对应数据 shard | 单包输入及完成事件队列 |
| 某出站消息的 reliable TxState | 按本机 RouteKey 选出的源 shard | 控制循环转发 ACK/NACK 和 BEGIN 请求 |
| 业务 SHM bridge 的提交操作 | 本机提交循环 | 其他线程提交 CommitRequest |
| bridge 初始化 | 初始化工作线程，尚未 Ready | 完成后一次性移交，不与提交并发 |
| 整体状态计数与配额 | RAII 配额令牌及原子计数 | 查询快照，不持全局锁执行 IO |
| 应用 reliable 等待表 | 应用 runtime，内部短锁 | 唯一控制读循环终结并唤醒 |

- 使用 shared_ptr<RouteState> 或等价 lease 保活路由；队列中禁止只存可能已释放的裸指针。
- 每条跨线程命令包含 gateway_epoch、route_epoch 和 command_id，晚到旧命令必须失效。
- 路由注销先标记 Closing 并从公告移除，再排 barrier，等待对应 shard / 提交队列的引用退出。
- 回写成功的 CommitResult 必须携带原始 AssemblyKey，不能“回给当前同名 topic”。
- C++17 发布不可变 shared_ptr 快照可用 std::atomic_load / std::atomic_store 的 shared_ptr 重载；
  不能直接采用仅较新标准支持的 std::atomic<shared_ptr<T>>。
- 不同时持 route 表锁与等待 SHM / ACK 的锁，不在控制循环里同步等待数据 shard 回答。
- 先注册事务，再通过队列投递工作；结果到达早于调用者 wait 时也必须保留并可读取。

### 11.2 数据分片选择

对目标端：

~~~text
target_shard = fnv1a64(RouteKey 的规范字节编码) % target.data_shards
target_port  = target.data_base_port + target_shard
~~~

同一 RouteKey 固定进入同一目标 shard，有助于局部状态管理，但不代表严格消息 FIFO。
源端固定用相同规范键按自己的 K 选发送 shard，以满足第 7.7 节源端口验证；
不为不同目标另开 socket。

改变 K 要重启网关产生新 epoch，首版不做运行中在线重分片。

### 11.3 每轮处理预算

默认单次就绪处理满足任一条件就让出：

- 64 个数据报；
- 64 KiB 数据；
- 200 微秒处理时间。

这是初始调参值，必须记录并允许配置，不作为跨机器性能保证。
控制与可靠定时器不等整条大消息收完才执行。

错误实现示例：

~~~text
共享收包线程：
  选 topic A
  调 chunk_rev_topic(A, timeout=50ms)
  等 A 所有分片或超时
  再处理 topic B
~~~

正确结构：

~~~text
循环：
  等待 IO 或最近定时器
  在预算内接收单个报文
  验证并推进对应 AssemblyState
  完整消息交给提交队列
  执行到期重传 / NACK / 回收
  处理本地 deferred 工作
~~~

### 11.4 可靠发送状态机

~~~text
SEND_BEGIN -> AwaitingRecord -> Queued -> Sending -> WaitingAcks
                                     -> Failed
WaitingAcks -> Completed / TimedOut / PeerGone / PeerRestarted / Rejected
~~~

规则：

- 目标在网关从出站 SHM 接纳记录时冻结。
- 本机目标只提交一次；本机成功不能代替远端 ACK。
- 每个远端目标分别记录确认、缺片和重试时刻。
- 对每个目标，首轮所有分片已交给 sendto 后才启动 20 ms 无确认探测；
  指数退避到最多 200 ms。不能从大消息入队时就开始重发仍未首发完的整包。
- NACK 可触发缺片定向重发，同一轮合并请求。
- 每目标维护“已首发片”位图；NACK 请求尚未首发的片时不另发副本，保留正常首发任务。
  重传中的同一片也只保留一个待发任务，避免重复 NACK 使发送队列无界增长。
- 全丢片时整体重发；ACK 丢失时相同消息身份重发，接收端去重后再 ACK。
- reliable 公开等待最长 5000 ms；tm 的单位为毫秒，tm=0 在编码、SEND_BEGIN 和出站提交前
  直接返回 TimedOut，不启动后台可靠事务；
  超过 5000 ms 或无限等待值按 UnsupportedTimeout 明确失败，不能悄悄缩短调用者的超时。
  新模式文档必须告知此上限；不改变 legacy 的等待规则。
- 重传在 deadline 到达后停止，释放缓存并只发一次 SEND_RESULT。
- 超时与 ACK 同时发生，以事务状态机的单次终结操作决定结果。
- 可靠失败不自动重新生成 sequence 再发。
- 不向控制通道发送完整业务载荷。
- 发送端口已显式 bind，网络发送不分配每话题临时源端口。
- socket 发送 EAGAIN 使用可写事件或有界重试队列，不 busy loop。
- 实际 sendto 前再次检查事务 deadline / 目标 epoch，不能因队列排队在超时后继续补发。
- best-effort 等待首次发送的排队期限为 1000 ms；开始发送后整条发送最长 5000 ms。
  超期停止发送剩余片并计数，不无界等待可写，也不因普通大消息首发超过 20 ms 就复制整包。
- 发送缓存总配额覆盖 reliable 与 best-effort 尚未发送完的 WireBlob；
  不能只限制 reliable，留下无限增长的 best-effort 发送队列。
- deadline 从公开调用入口采样，计算毫秒转纳秒及相加时检查溢出；编码结束、BEGIN_READY
  到达、出站提交前均重新检查。不能在慢编码之后重新给一次完整的 tm。
  本机时钟可比较不代表任意用户自定义 serialize 可被抢占；若编码本身阻塞，返回时限只能
  在编码返回后检查，必须单独报告编码耗时，不能把网络状态机的界限写成任意用户代码的硬实时保证。

### 11.5 发送公平与控制优先级

- 每 topic 设置有界发送队列，调度使用按字节计费的轮转。
- 一个大消息不能一次连续发送全部分片后才轮到小消息。
- 本机提交循环按 RouteKey 轮转，失败的热门话题不能阻塞其他通道。
- ACK/NACK 优先于订阅快照分页；二者均有总速率限制。
- 控制线程必须能在大流量下及时处理 stop / unregister / deadline。
- QoS / CPU 参数首版只影响明确的网关配置，不能把每个应用对象的绑核参数直接修改共享线程。
- 若公共构造传入与网关冲突的专用绑核要求，报告该要求不适用于共享后端；
  不静默承诺它已经生效。

## 12. 配额、故障与生命周期

### 12.1 默认上限

| 配置项 | 初始默认值 | 达限行为 |
|---|---:|---|
| 活跃业务话题 | 4096 | 注册拒绝 TopicLimit |
| 应用会话 | 128 | 连接拒绝 SessionLimit |
| 远端 peer | 128 | 忽略新 HELLO 并计数 |
| 逻辑发布/订阅句柄 | 总 16384 / 每会话 8192 | 注册拒绝 HandleLimit |
| 单消息 | 16 MiB | 提交前拒绝 MessageTooLarge |
| 接收重组总字节 | 256 MiB | 拒绝新 assembly |
| 每 peer 重组字节 | 64 MiB | 该 peer 新 assembly 拒绝 |
| 每 RouteKey 重组字节 | 32 MiB | 该 topic 新 assembly 拒绝 |
| 重组条目数 | 4096 | 拒绝新 assembly |
| 全部发送缓存总字节 | 256 MiB | SEND_BEGIN / best-effort 接纳失败 |
| 每 publisher reliable 在途 | 64 | Busy |
| reliable 事务（含 AwaitingRecord） | 总 4096 / 每会话 512 | BEGIN_READY 前 Busy |
| 发送目标状态（含 best-effort） | 总 32768 | 冻结目标前 Busy / 丢弃并计数 |
| 本机 CommitPending 总字节 | 64 MiB | 拒绝或留在原重组配额内，禁止重复不记账 |
| 去重 stream 数 | 16384 | 新 stream 拒绝 |
| 每 stream 去重窗口 | 4096 个序号 | 依第 7.6 节处理 |
| 可靠去重回执数 | 总 65536 / 每 peer 8192 | 接纳前拒绝新 reliable |
| 单进程出站占用字节 | 32 MiB | 提交前 Busy / allocation failure |
| SHM 初始化等待任务 | 64 | 注册返回 Busy |
| 候选目录 body | 每 peer 8 MiB，总 64 MiB | 拒绝候选快照 |
| 已安装远端目录（含解析索引） | 总 64 MiB | 拒绝替换并计数，旧目录只保留到租约结束 |
| 仍被引用的旧目录版本 | 总 64 MiB | 限制新事务/替换，不能越限保留旧版本 |
| peer 身份历史记录 | 总 16384 / 每 gateway_id 256 | PeerHistoryFull，不驱逐必要拒收记录 |
| 控制输出队列 | 每会话 256 KiB、总 8 MiB；网络控制总 8 MiB | 关闭故障会话或丢弃可重试网络控制并计数 |

所有项都验证数值范围、乘加溢出和相互约束。默认值是软件上限，不表示启动即全部预分配。
出站和业务 SHM 实际最大 chunk 必须在启动能力握手中校验；
若不能容纳配置的最大消息，显式拒绝启动或要求用户降低配置，不能静默截断。

配额按持有的真实内存/loan 记账；同一 buffer 转移队列只转移归属，不重复扣费或重复释放。
系统 socket SO_RCVBUF/SO_SNDBUF 是上限配置，报告 getsockopt 实际值，不能把请求值当实占内存。

表中句柄上限不等于 IpcInfoPool 能承载同样多的展示项。注册须同时预留实际诊断池槽位、
MPMC 发布者槽位、目录空间和路由对象；任何一项不足都在 Ready 前失败并撤销其余预留。
状态工具必须区分 TopicLimit、HandleLimit、RegistryFull 与底层 SHM 槽位耗尽。

### 12.2 主要故障处置

| 故障 | 必须行为 |
|---|---|
| 网关 bind 失败 | 初始化失败，无半工作状态；释放已创建端点 |
| 应用在 REGISTER 后、SUB_READY 前退出 | 回收登记，不对外公告 |
| 应用持有出站 loan 后退出 | 由会话所有权清理独占段；不清业务 SHM |
| 网关 SIGKILL | 应用会话失效；在途可靠返回 GatewayLost；不自动切 legacy |
| 网关 SIGSTOP | 不发生锁接管；健康超时后应用停止新提交，明确不可用 |
| 网关重启 | 新 epoch，新会话、新出站段；重新注册；不重放旧出站记录 |
| 远端重启 | 旧目标事务 PeerRestarted；新调用等待新快照 |
| 最后 Ready 订阅者注销 | 先撤销网络 route epoch，再处理在途状态；bridge 按 8.3 的独立引用归零后释放 |
| 注入端退出但还有其他 SHM 发布者 | 普通 MPMC leave，不能 clear_storage |
| 有应用仍借用 Sample | 不提前 unlink 其仍在使用的段/池；遵守前置租约 |
| 错误 topic/schema | 注册拒绝或入站丢弃，不送到用户反序列化回调 |
| 缺片超时 | 回收 assembly，可靠事务最终失败，不交付半包 |
| 无订阅 / 慢订阅 | 显式计数，可靠边界按第 5 节，不承诺应用消费 |
| 控制连接写满 | 有界发送队列，超限关闭故障会话并唤醒等待者 |
| FD 被操作系统复用 | 注销令牌携带 owner + generation，不把旧事件分配给新对象 |

### 12.3 正常关闭顺序

1. 设置 Stopping，拒绝新注册和新发送。
2. 对外停止公告新订阅，通知本机会话。
3. 取消可靠事务，发出可发送的终结结果，唤醒所有等待。
4. 停止 IO 等待，摘除 FD，join 所有网关循环。
5. 清理 assembly / retry / commit 队列，使 buffer 和 loan 按 RAII 归还。
6. 注销内部业务 SHM 发布端，等待已有在途引用归零。
7. 关闭出站会话与独占段，关闭 Unix socket。
8. 清理本实例控制路径，最后释放锁 FD。
9. 重复 stop 必须幂等。

不使用全局 rm /dev/shm，不使用 killall，不清空整个 UDP 接收缓冲作为退出步骤。

### 12.4 配额账本与容量计算

条目上限和 payload 字节上限要同时满足。跨线程队列节点、位图、每目标状态、定时器、
解析索引和历史记录都要有可核验的上界；“有 256 MiB blob 上限”不能代替这些元数据限额。
T01 固定所有有界队列的条目/字节配置，T07/T10 验证预留失败时不会留下半个事务。

| 阶段 | 必须预留 | 归还/转移时机 |
|---|---|---|
| REGISTER | 句柄、路由/SHM 槽位、登记槽位、目录增量 | 注册失败全部撤销；注销等待引用收敛 |
| SEND_BEGIN | 事务项、发送 blob 字节、等待结果项 | 未提交过期也归还；不重复预留同一 request |
| 冻结发送目标 | 本次目标状态及首发/重传位图 | 完成/失败一次归还；目标退出不悄悄缩小集合 |
| 首片接纳 | assembly 条目、完整消息容量、位图、stream；reliable 还需回执位置 | 提交后转去重/回执；超时释放 blob，按第 7.6 节保留必要历史 |
| CommitPending | 队列项和 route lease | 同一 buffer 仅转移归属；复制才增加字节占用 |
| 目录替换 | 候选、解析后的新目录及尚被引用的旧目录 | 原子替换后旧引用归零才归还 |

多目标可共享一个不可变 WireBlob，payload 不必复制 R 份，但每目标的片位图、确认状态和
重试任务仍为 O(R)。目标冻结前预留这些状态；不能先给部分目标发送，再发现装不下其余目标。
重传任务合并到原事务，不按 NACK 次数另建无界任务。best-effort 同样计算目标状态。

令 M 为最大业务 blob、C(n) 为实际 loan 档位容量，出站能力至少满足：

~~~text
outbox_record_bytes = 96 + M
session_outbox_limit >= C(outbox_record_bytes)
business_loan_capacity >= M
fragment_count = (M + 1023) / 1024        # 使用足够宽的整数
~~~

当前大 loan 按 2 的幂分档，16 MiB blob 加 96B 信封可能进入 32 MiB 档位。
因此不能用“底层支持 16 MiB 消息”推断出站已可支持 16 MiB blob，也不能按 M 少记一半 loan。
每尺寸档位底层还可能映射多个 chunk：在途 loan 配额、池映射容量、tmpfs 实占与 RSS 是
不同口径。必须分别报告，不能把 32 MiB 出站占用上限当作整个会话 SHM 映射上限。
纯字节转移的 blob 账本也不能代替业务 SHM 池自身的槽位与租约约束。

目录极端容量必须分别测“已安装且活跃”“更新中候选”“旧版本仍被事务引用”。
仅验证 128×8 MiB 候选分配被限制，不能证明 128 份已安装目录和旧版本也有界。
同理，4096 话题是配置最大数，不表示所有上限的笛卡尔积可以同时接纳；触顶原因必须可查询。

## 13. 文件与接口落点

### 13.1 新增模块

以下文件均为建议的精确落点。需要改名时先更新本表和测试引用。

| 文件 | 责任 |
|---|---|
| include/dzIPC/net/shared_config.h | 模式、限额、端点配置与纯解析接口 |
| src/dzIPC/net/shared_config.cc | 参数校验、进程级配置读取 |
| include/dzIPC/net/wire_protocol.h | WireHeader、编解码结果、错误枚举；不含 socket 平台头 |
| src/dzIPC/net/wire_protocol.cc | 160B 网络头编解码、CRC、分片校验 |
| include/dzIPC/net/wire_blob.h | 有所有权的业务 blob 与 RouteDescriptor |
| src/dzIPC/net/wire_blob.cc | TLV / DZFlat 编码与结构校验 |
| include/dzIPC/net/datagram_endpoint.h | 裸数据报接口、源地址、截断、错误状态 |
| src/dzIPC/net/datagram_endpoint_linux.cc | Linux 非阻塞 UDP，bind、sendto、recvmsg |
| include/dzIPC/net/reassembly.h | 单包推进重组和去重状态机 |
| src/dzIPC/net/reassembly.cc | 配额、片表、完成/拒绝、超时 |
| include/dzIPC/net/reliable_session.h | 目标快照、ACK/NACK 路由、可靠事务 |
| src/dzIPC/net/reliable_session.cc | 定时重发与单次完成 |
| include/dzIPC/net/peer_directory.h | 网关发现、发布/订阅目录、代次历史和租约 |
| src/dzIPC/net/peer_directory.cc | HELLO / DZGC 编解码与快照事务 |
| include/dzIPC/net/local_protocol.h | Unix 控制消息、DZTX 编码与错误码 |
| src/dzIPC/net/local_protocol.cc | 控制消息验证、golden bytes 支撑 |
| include/dzIPC/net/client_runtime.h | 进程级会话、出站通道、可靠等待 |
| src/dzIPC/net/client_runtime.cc | 注册/注销、fork 闸、连接关闭处理 |
| include/dzIPC/net/shm_wire_bridge.h | 原始 blob 到业务 SHM 的内部适配 |
| src/dzIPC/net/shm_wire_bridge.cc | MPMC 生命周期与原始注入 |
| include/dzIPC/net/gateway_runtime.h | 网关公开启动/停止/状态接口 |
| src/dzIPC/net/gateway_runtime.cc | 路由所有权、线程调度、配额汇总 |
| include/dzIPC/shared_pub_sub_ipc.h | 新 SharedPublisher / SharedSubscriber |
| src/dzIPC/shared_pub_sub_ipc.cc | pub/sub 抽象适配与返回契约 |
| exec/dzipc_gateway/src/main.cc | 命令行：serve / status / check-config |
| exec/dzipc_gateway/CMakeLists.txt | 新程序构建、安装 |
| test/shared_net/ | 多进程/跨机驱动、资源采样、故障脚本和样例配置 |

平台系统调用可再拆 local_control_linux.cc，但必须是明确的 Linux 编译单元。
不要把平台宏和原生 FD 操作散落到 pub/sub 业务层。

### 13.2 修改已有文件

| 文件 | 允许改动 | 不允许顺手改动 |
|---|---|---|
| src/dzIPC/topic_ipc.cc | IPC_SOCKET 按配置选择新后端 | 改 IPCType 枚举语义、改服务路径 |
| include/dzIPC/shm_pub_sub_ipc.h / src/dzIPC/shm_pub_sub_ipc.cc | 新增内部原始注入适配，复用 MPMC | 重写前置注册表、绕过租约、普通 join 清段 |
| src/CMakeLists.txt | 接入新 net 目录；Linux 实现条件编译 | 把所有平台私有实现无条件编译 |
| CMakeLists.txt | 新增网关子目录与构建选项 | 改默认旧传输模式 |
| test/CMakeLists.txt | 新增测试目标 / CTest / 标签 | 删除失败的旧测试以凑通过率 |
| exec/dzipc_topic_cat/ | 识别共享网络模式，通过本机网关订阅 | 按旧 topic 哈希端口直接监听新模式 |
| exec/dzipc_list/ 与 info pool 展示 | 展示逻辑端点、网关、mode、epoch | 把每个内部注入端伪装成业务发布者 |
| README.md / exec/README.md | 启用、语义、限制、回滚入口 | 宣称未测平台或性能通过 |

### 13.3 构建隔离

新增 DZIPC_BUILD_SHARED_NET，默认 64 位 Linux ON、其他平台 OFF。
其他平台即便 shared_v1 配置存在，也应报 UnsupportedPlatform，不隐式落到 legacy。
支持平台显式关闭构建选项时，新模式报 BackendNotBuilt；模式解析与失败分支仍可链接。

src/CMakeLists.txt 现有 aux_source_directory 不递归。
推荐显式列出 net 的通用源，并只在 Linux 添加 *_linux.cc。
新增文件后重新运行 CMake；不要只运行 make。
顶层 aux_source_directory 也会拾取 shared_pub_sub_ipc.cc：关闭构建选项时须将其排除，
或保留不依赖未构建模块的明确失败实现。只给 net 子目录加 if 而漏掉该入口会产生链接错误。

新 C++17 代码与现有项目标准一致。避免为了此任务引入新的网络框架或第三方 RPC 系统。

## 14. 可观测性、诊断和工具

### 14.1 状态输出

新增：

~~~bash
dzipc_gateway check-config --listen-ip 192.168.10.10 --interface eth0
dzipc_gateway status --control /tmp/dzipc-gateway-1000/control.sock --json
~~~

以上命令是待实现的接口示例，不代表当前已经可执行。
check-config 只校验配置；端口可用性由 serve 的实际 bind 决定，不能用先探测再关闭保证之后无冲突。

status JSON 至少包含：

- protocol_version、gateway_id、gateway_epoch、state；
- listen_ip、interface、data_ports、control_port、discovery_group / port；
- 实际 UDP socket 数和 SO_RCVBUF / SO_SNDBUF；
- client_sessions、logical_publishers、ready_subscribers、active_routes、peers；
- 当前/峰值重组字节、发送缓存字节、出站 SHM 借样、CommitPending 字节；
- 各配额及触顶计数；
- 逻辑线程数与实际线程采样口径；
- 最后一次错误码与有界说明；
- 单话题查询时返回其 domain、原始 topic、msg_id、schema、epoch 和流量摘要。
- 话题与 peer 查询返回已安装目录版本、完整来源是否已核验、PUB/SUB 角色及目标 route epoch，
  供部署与测试确认双向发现就绪；不存在时明确返回未就绪，不虚构远端状态。

默认 status 返回汇总，单话题明细使用 --topic / --domain / --msg-id 查询；
加 --peer-id 时映射到 QUERY_STATE kind=2，单次只查询一个 peer；
首版不在一个 8192B 控制包里塞入 4096 个话题的全部详情。
话题总览使用本机登记列表逐条查询或工具侧流式展示，不构造无界控制报文。

JSON 使用字符串表示 64 位 ID、epoch、domain、sequence，避免 JavaScript 数值精度损失。
指标计数可输出十进制字符串或明确的 uint64 编码规则，不能随机混用。

### 14.2 必需指标

| 类别 | 最少指标 |
|---|---|
| 提交 | tx_accepted、tx_not_submitted、tx_indeterminate、tx_no_route |
| 网络 | datagrams_rx / tx、bytes_rx / tx、send_eagain、send_error、truncated |
| 验证 | bad_magic、bad_version、bad_header、packet_crc_fail、message_crc_fail、foreign_route、wrong_shard、source_route_unverified |
| 分片 | assemblies_active、assembly_bytes、assembly_timeout、duplicate_fragment、conflicting_fragment |
| 投递 | shm_commit_ok、shm_commit_not_submitted、shm_commit_indeterminate、duplicate_message_suppressed |
| 可靠 | reliable_started / completed / timed_out、acks_rx / tx、nacks_rx / tx、retransmitted_bytes |
| 公平 | 每 topic 排队时长分位数、每 shard 处理字节与 backlog |
| 发现 | peers_active、peer_expired、snapshot_complete / rejected / timeout、peer_epoch_retired、peer_history_full、identity_conflict |
| 生命周期 | sessions_active、registration_rejected、gateway_lost、route_recreated |
| 配额 | 每类 quota_rejected 和峰值 |
| 业务差异 | 本机提交次数、远端目标份数、零目标丢弃数 |
| 出站 | outbox_reserved_records / capacity、outbox_released_records / capacity、outbox_progress_lag、outbox_overwrite |

热路径只更新有界计数，不对每个分片同步写文本日志。
日志节流键不能无界地按任意外来 publisher_id 建表。

### 14.3 topic_cat / dzipc_list

- 新模式的逻辑端点应显示 shared_v1、网关实例和域。
- 网关内部 SHM 发布者不能让用户误认为多了一个业务发布者。
- topic_cat 通过同一 SharedSubscriber 接入，自动注册订阅并触发远端发现。
- GenericMessage / DZFlat 的 schema 仍在工具或 Python 侧解析，网关不编译用户类型。
- 仅想观察状态时不注册业务订阅；真正读取消息时必须声明订阅。
- 未启动网关应显示 GatewayUnavailable，不循环尝试旧哈希端口。
- 显式 socket-only 抓包选项继续指向旧协议，界面说明其不能读取 DZMX。
- Python 通过既有 pimpl 后端选择得到新模式；另外测试预构造段 false 回退不重复。

## 15. 串行执行任务卡

### 15.1 统一执行规则

每张卡开始前：

1. 阅读该卡列出的文件和依赖章节。
2. 确认上一张卡退出条件有实际日志。
3. git status 记录当前变更，确认不覆盖其他任务。
4. 每次只实施一个可验证目标。
5. 新源文件或测试加入后重新配置构建。
6. 失败时先记录具体失败与源码指纹，不能删断言或增大 sleep 掩盖。
7. 完成卡后更新第 20 节记录，附命令、退出码、测试数、skip 数和制品路径。

任何阶段都不能以“编译通过”代替其要求的行为测试。
不需要为了文档列出路径而提前创建空实现；只在对应任务开始时创建文件。

### T00：前置 SHM 和现状基线

**依赖：** SHM 多发布者任务完成，负责人提供提交号与验收结果。

**必须读取：**

- docs/shm_multi_publisher_execution_plan.md；
- include/dzIPC/common/shm_channel.h；
- include/dzIPC/common/publisher_registry.h；
- shm_pub_sub_ipc 中 InitChannel、MPMC join/leave、loan 和析构；
- test_shm_multi_publisher、test_shm_mpmc_*；
- 本文第 2～5 节。

**步骤：**

1. 记录最终基线 commit 和工作区 diff，不能仅照抄 e887d5e。
2. 确认 MPMC 段名、控制面版本、发布者 slot 数、订阅者上限。
3. 确认同话题两个普通发布者同时存活，加入/退出不清旧段。
4. 确认 gateway 额外占一个 SHM publisher slot；配置容量需给它留位置。
5. 验证借样提交失败、持有 loan 时退出、持有 Sample 时发布端退出。
6. 以旧模式采集 1 / 100 / 1000 话题的 socket / FD / 线程 / SHM / RSS。
7. 记录旧模式纯本机延迟、跨机单收端和多收端吞吐作为比较基线。
8. 若关键前置失败，停止接入公开后端；可以继续独立协议纯函数工作，
   但不能把整体前置标为通过。
9. 核对 size_t 位宽、完整话题名与诊断池槽位；按第 12.4 节测最大 blob 加 96B 信封后的
   实际 loan 档位和池映射，不能只测试恰好 16 MiB 的裸载荷。

**产物：** baseline.md、命令日志、资源 CSV、MPMC 能力表。  
**退出条件：** 前置生命周期和多发布者关键测试通过；历史失败明确列出且与本次无关。

### T01：配置、模式和构建骨架

**改动：** shared_config、CMake、新目录、模式解析测试。  
**步骤：**

1. 实现 legacy / shared_v1 精确字符串解析。
2. 配置只在确定的进程入口读取一次；测试调用纯解析函数，不污染全局缓存。
3. 校验 K、端口、地址、限额、interface 和 Unix 路径长度。
4. 校验 discovery / data / control 端口不重叠。
5. Linux 构建通用库模块和网关空壳；非 Linux 编译 legacy 与明确的不支持分支。
6. 此时不切换 topic_ipc 的实际后端；选择 shared_v1 可显式报 NotImplemented。
7. 新增 test_shared_net_config 并登记 CTest 标签 shared_net。
8. 验证 64 位 Linux 的 ON/OFF 构建与不支持平台的失败分支；固定跨线程队列、
   已安装/旧目录、peer 历史和目标状态限额，不只限制 payload。

**退出条件：** 默认旧测试可运行，坏配置有确定错误码，不能因拼写错误静默启用其他模式。

### T02：身份、字节协议和编码器

**依赖：** T01。  
**改动：** wire_protocol、wire_blob、local_protocol 的纯编解码。  
**步骤：**

1. 先写第 7 节 160B 头每个偏移的字节向量测试。
2. 固定 RouteKey 编码、64 位 domain、publisher_id 和 sequence。
3. 实现 DATA / ACK / NACK / REJECT 编解码与 CRC。
4. 实现 DZGD 64B、DZGC 84B、DZLC 40B、DZTX 96B 头。
5. 为本机每种消息增加显式枚举值；把 SEND_BEGIN / BEGIN_READY 纳入消息表。
6. 为每种变长 body 固定编码顺序、长度检查和未知字段策略。
7. WireEncoder 保留完整 TLV blob；DZFlat 使用完整段。
8. 生成 C++/Python 都能读取的 golden_vectors.json，包含字段与十六进制结果。
9. 使用纯内存测例，不需要网络或守护进程。
10. 向量包含 36B RouteKey、目录角色位、空目录、CRC32C 标准样例和 TX_PROGRESS；
    WELCOME 容量字段及 PUB-only 目录不能由实现者另定布局。

**测试：** test_shared_net_wire、test_shared_net_blob、test_shared_net_local_protocol。  
**退出条件：** 各字段偏移、端序、截断、CRC、溢出、编码往返均有断言；旧 TLV 字节逐字节保留。

### T03：裸数据报端点与有界 IO

**依赖：** T02。  
**改动：** datagram_endpoint_linux、网关底层等待循环。  
**步骤：**

1. 新增裸数据报 API，不修改 UDPNode::set_scope 的单话题契约。
2. 支持显式 bind、非阻塞 sendto、recvmsg 源地址与 MSG_TRUNC。
3. recv 返回 Data / WouldBlock / Truncated / Fatal，不能用“空 buffer”混淆所有状态。
4. 实现 stop 唤醒、先摘除后 close、FD generation 校验。
5. 为数据与控制 socket 禁用共享 bind；测试第二次同端点启动失败。
6. 创建 K+2 个 UDP socket，增加 1000 个逻辑 RouteKey 后再次计数。
7. 确保往 100 个不同远端地址发送不会建立 100 个发送 socket。
8. 实现按报文/字节/时间预算让出和 deferred 重调度。
9. 检查实际源地址/端口及目标 shard；K=1/4/16 两端交叉配置，错误 shard 的副本不能
   建第二份 assembly。分片、源/目标 shard 都使用同一 RouteKey 编码向量。

**测试：** test_shared_net_endpoint、test_shared_net_io_lifecycle。  
**退出条件：** 固定端点数、非阻塞、取消、FD 复用和 bind 冲突测试通过。

### T04：网关独占启动与本机会话

**依赖：** T03。  
**改动：** gateway_runtime 启停、client_runtime 基础、Unix 平台层。  
**步骤：**

1. 实现 check-config / serve / status。
2. 验证目录、UID、文件锁、socket 路径。
3. HELLO 校验 locality 和版本，WELCOME 分配 session_id / epoch。
4. 每进程控制连接复用；一个读循环按 request_id 分发。
5. 模拟 1000 个逻辑句柄，控制连接数仍按进程计。
6. 错误请求、重复 request_id、断连、慢读者和写满都必须有界。
7. PID 变化在获取旧运行时 mutex 前判定并失败。
8. 可靠等待表先使用假完成事件验证唤醒逻辑。

**测试：** test_shared_net_gateway_lock、test_shared_net_client_lifecycle。  
**退出条件：** 两个网关不能同时接管；SIGSTOP 不触发接管；断连唤醒所有等待；遗留路径可安全重启。

### T05：出站 SHM 的原子接管

**依赖：** T04 和 T00 借样能力。  
**改动：** client_runtime 出站通道、DZTX 记录处理。  
**步骤：**

1. 每会话建立一条独占命名的 ipc::route，应用仅一个 sender 实例。
2. 通过 ATTACH_TX 传 eventfd，TX_READY 后才开放发布。
3. 为出站记录实现 loan / 填写 / publish_loan / 所有权释放。
4. 真实跨进程验证一个提交只出现一个完整记录。
5. 注入 loan 失败、提交失败、应用在填充中退出、网关在提交前后退出。
6. 证明 NotSubmitted 不可见；若底层有不确定结果，落实第 5.3 节处理。
7. 实现进程多发布线程并发，短锁只包提交，可靠等待在锁外。
8. 证明 eventfd 合并通知、队列预算耗尽、最后一条记录均不会丢唤醒。
9. 限制在途占用，超限时不另建通道或无界申请内存。
10. 信用同时限制记录数和实际 loan 容量；测 TX_PROGRESS 合并/重复/最后一批、
    消费早于 publish 返回、控制连接写满及会话关闭，不能漏记或超额归还。
11. 在每个档位边界验证 DZTX 长度裁剪，填充尾部不得上网；信用窗口内不得覆盖未读记录。

**测试：** test_shared_net_outbox、test_shared_net_outbox_failure。  
**退出条件：** 真实跨进程原子接管证据齐全；可安全说明预构造段 false 的边界。
**禁止：** 以“通常不会失败”跳过原子性；以 Unix 载荷传输临时代替并宣布完成。

### T06：业务 SHM 原始注入

**依赖：** T00、T02；可使用测试 blob，不依赖网络。  
**改动：** shm_wire_bridge、shm_pub_ipc 的内部访问适配。  
**步骤：**

1. 一个 RouteKey 对应一个内部 MPMC 发布对象。
2. 利用注册的 msg_id 和 GenericMessage 模板建立无需用户头文件的注入端。
3. DZFlat / TLV 两种 blob 都直接注入，不在网关反序列化业务对象。
4. 用一个普通 IPC_SHM 发布者 + 网关注入端同时向两个进程的订阅者发送。
5. 注入端退出，普通发布者继续发送；反向退出也要测试。
6. 验证有借样 Sample 时网关关闭不提前回收。
7. 实现 NotSubmitted / Committed / Indeterminate，不以单一 bool 掩盖不确定结果。
8. 确认一个话题多个远端来源仍只占一个网关 publisher slot。
9. 检查内部端点不会重复登记为逻辑业务端点。

**测试：** test_shared_net_shm_bridge。  
**退出条件：** 多进程接收、两种编码、双发布者并存、析构与 lease 均通过。

### T07：分片、重组与去重

**依赖：** T02、T03、T06。  
**改动：** reassembly、quota、CommitPending 和去重窗口。  
**步骤：**

1. 先实现可喂入单个报文的纯状态机。
2. 一个报文只推进对应 AssemblyKey，不读取下一包、不等待缺片。
3. 乱序和重复分片仍得到同一完整 blob。
4. interleave 两个发布者、相同 msg_id / sequence / 大小但不同 payload，验证不会混包。
5. 再 interleave 两个话题，刻意使用相同数据 socket。
6. 完整 CRC 后进入 CommitPending，且该状态只能生成一次本机提交请求。
7. 提交完成事件回到所属状态机后再写 Committed。
8. 去重状态与路由 epoch、peer epoch 一起测试，包括窗口滑动和晚到旧包。
9. 实现绝对超时、内存配额和坏包回收。
10. 测试 1023 / 1024 / 1025B、1MiB、配置最大值、最大值+1。
11. 超过空闲/回执期限后重放同一 best-effort/reliable；stream 配额满时旧窗口不被 LRU
    淘汰。向错误 shard 和 CommitPending 状态注入冲突分片，业务提交仍不超过一次。

**测试：** test_shared_net_reassembly、test_shared_net_dedup、test_shared_net_quota。  
**退出条件：** 测试 payload 全量校验，不能仅断言“收到消息数”；没有半包交付或重复注入。

### T08：网关发现与路由目录快照

**依赖：** T04、T06；只用控制报文即可测试。  
**改动：** peer_directory、RouteDescriptor 注册和 SUB_READY。  
**步骤：**

1. 实现 HELLO 定时、租约、自己过滤和显式 interface。
2. 实现 RouteDescriptor 类型冲突和完整 topic 指纹冲突检查。
3. 建立 REGISTER_SUB / REGISTERED / SUB_READY 两阶段。
4. 目录包含已注册 PUB 和 Ready SUB，目标选择只使用 SUB 位；版本每次有效变更递增。
5. 快照分页、乱序、重复、缺页、CRC、版本切换均独立测试。
6. 半张快照不得替换现有路由。
7. 最后订阅者离开立即使 route epoch 失效；晚包不能进入新订阅者。
8. 128 peer / 4096 topic 配置容量内，状态和候选快照内存有界。
9. 若发现组播不可用，允许显式静态 peer 配置复用同样快照协议；
   不改成全网广播数据。静态 peer 仍需 HELLO 握手和过期机制。
10. 测远端仅发布、不订阅时的全名与指纹冲突；来源目录未同步时不提前接纳 DATA。
11. peer 过期后以相同 epoch 返回仍保留去重；新 epoch 被接纳后延迟的旧 HELLO/PAGE
    不得切回旧实例。已安装/候选/旧目录和 retired 历史逐项触顶验证。

**测试：** test_shared_net_discovery、test_shared_net_subscription_snapshot。  
**退出条件：** 迟到订阅可收新消息，退出订阅被撤销，gateway epoch / route epoch 不串代。

### T09：best-effort 端到端

**依赖：** T05～T08。  
**改动：** 网关完整数据路径，测试用发布/订阅程序。  
**步骤：**

1. 一个应用发布者 -> 本机网关 -> 本机业务 SHM -> 两个订阅进程。
2. 加入远端网关和两个远端订阅进程。
3. 一个远端主机多个订阅者，发送侧仅产生一份目标载荷。
4. 两边都有同话题发布者，相互收取；本机与远端副本各一次。
5. 构造 A->B 入站，检查 B 不将它再次出站发回 A。
6. 测无订阅、晚订阅、普通 IPC_SHM 本机发布者、不同 domain。
7. 同一 UDP socket 同时传多个 topic / msg_id=0。
8. gateway status 证明 application_udp_sockets=0，网关=K+2。
9. 不接入默认公共工厂，避免可靠功能未完成时暴露半成品。

**测试：** test_shared_net_end_to_end，跨进程驱动。  
**退出条件：** 按 publisher + sequence + 内容检查每个接收者，不重复、不串话。

### T10：可靠发送与公平性

**依赖：** T09。  
**改动：** reliable_session、SEND_BEGIN / SEND_RESULT、定时器、ACK/NACK。  
**步骤：**

1. SEND_BEGIN 记录调用开始时间与 deadline，校验 5 秒上限。
2. 数据被接纳时冻结目标集合。
3. 收 ACK 通过统一控制路由匹配事务；发送调用不得 recv UDP。
4. 接收端只在 SHM Committed 后发 ACK。
5. 全部数据丢失时发送端超时探测重发。
6. 缺部分片时 NACK 只补缺片；ACK 丢失时去重后重发 ACK。
7. 测一个目标成功、另一个超时，整个调用返回失败且报告部分交付。
8. 测 ACK 与 deadline / unregister / gateway stop 并发，终结结果恰好一次。
9. 测失败提交重试不重复，Indeterminate 不自动再提交。
10. 热话题 1MiB 连发同时冷话题短消息，冷话题和控制报文能持续推进。
11. 释放所有发送缓存、等待者和计时器。
12. tm=0 无提交；慢编码不重置 deadline。发送端超时后再放行已发 DATA，允许远端晚交付
    但不得重复；零 ACK 不等于零投递，失败结果须正确标记 possible_partial_delivery。

**测试：** test_shared_net_reliable、test_shared_net_fairness、test_shared_net_control_pressure。  
**退出条件：** 所有目标策略、重传、去重、失败边界与配额通过；无永久等待。

### T11：公共 API 接入与兼容

**依赖：** T10。  
**改动：** shared_pub_sub_ipc、topic_ipc 工厂、模式说明。  
**步骤：**

1. SharedPublisher 实现全部 pub_ipc_base 接口。
2. SharedSubscriber 包装 shm_sub_ipc 并执行两阶段注册。
3. 只在 DZIPC_NET_BACKEND=shared_v1 且前置满足时选择新后端。
4. 缺网关、错误 locality、无 MPMC、协议不匹配都应明确失败。
5. 测 publish_prebuilt_segment 编码失败返回 false，回退 publish 后接收一次。
6. 测出站已接管后不会再返回可回退的 false。
7. reset_message 必须等待该句柄在途事务终结，注销旧 descriptor，重新注册新 descriptor；
   不能原地修改共享路由键。失败后句柄保持明确不可用或旧状态，二选一并固定。
8. 覆盖 GenericMessage、生成 C++ 消息和 Python 预构造段。
9. 验证 IPC_SOCKET_ONLY、IPC_SHM、服务请求响应仍选择旧路径。
10. 未完成的新配置不能被默认开启。

**测试：** test_shared_net_public_api、test_shared_net_prebuilt、test_shared_net_compatibility。  
**退出条件：** API 行为矩阵完整，默认旧回归通过，二进制兼容约束检查完成。

### T12：故障、关闭与重启

**依赖：** T11。  
**步骤：**

1. 每个有向生命周期边界放测试注入点：收到最后一片前、SHM commit 前后、ACK 前后。
2. SIGKILL 网关，验证应用返回 GatewayLost 和旧 Sample 生命周期。
3. SIGSTOP 网关，验证不出现第二网关接管。
4. 重启网关，新 epoch 使旧记录/旧网络包无效。
5. 应用重新创建后端并重新注册；不假装旧句柄自动可用。
6. 最后订阅者注销后立即重建，旧数据不能进入新 route epoch。
7. 循环创建/销毁 100 次，检查 FD、线程、SHM 和配额回到基线。
8. 并发 publish / reset / unregister / stop，以竞态检测或可重复故障测试验收。
9. 加载接近限额的在途数据后关闭，确认所有 loan 和 buffer 有所有权归还路径。
10. 任何清理只针对本次测试创建的路径和进程。

其中活跃配额应回到基线；第 7.6、8.5 节要求保留的去重/身份历史单独列账，验证有界。
不能为了让总内存曲线立即归零而删除仍承担拒收旧包职责的记录。

**测试：** test_shared_net_failure、test_shared_net_restart、test_shared_net_teardown。  
**退出条件：** 无悬挂、无 use-after-free、无旧消息重放、无增长性资源泄漏。

### T13：诊断工具与安装

**依赖：** T11、T12。  
**步骤：**

1. 完成 status JSON、第 14 节指标和错误码字典。
2. dzipc_list 展示逻辑端点与共享模式；内部端点不重复计业务实例。
3. topic_cat 通过网关注册并读一次，本机/远端消息各验证。
4. 安装网关二进制和必要库文件，提供前台部署示例。
5. 文档显式写出单播扇出、网关额外一跳、可靠 ACK 边界和跨模式不互通。
6. 编写回滚步骤，确保不清理其他业务 SHM。
7. 测命令参数错误、未知命令、端口冲突、不可写目录、网关离线的退出码。

**测试：** test_shared_net_cli、test_shared_net_topic_cat。  
**退出条件：** 从安装目录启动并完成跨进程 pub/sub，诊断结果与系统采样一致。

### T14：规模、网络故障与性能验收

**依赖：** T13。  
**步骤：**

1. 跑第 17 节全部矩阵，记录实际测试数量和跳过原因。
2. 正式比较 baseline / shared_v1，不能比较不同编译优化级别。
3. 1 / 100 / 1000 topic 固定 K，核验 UDP socket 不变。
4. 1 / 8 / 32 本机订阅者，验证远端入站重组与 SHM commit 每消息仍各一次。
5. 1 / 2 / 8 远端主机订阅同一大流量话题，验证并报告单播放大。
6. 注入数据丢失、控制丢失、重复、乱序和 burst；测试后移除仅测试网卡上的规则。
7. 纯本机与跨机分别报告吞吐、p50/p95/p99、CPU、RSS、FD、SHM、网卡字节。
8. 记录空闲与关闭后的资源回落。
9. 大话题与冷话题混合，验证第 17.5 节公平性。
10. 不满足目标部署带宽/延迟预算时保持默认 legacy，交付报告写“不满足”，不能调整图表掩盖。

**产物：** results.md、cases.json、CSV、状态快照、命令、源码指纹。  
**退出条件：** 正确性和资源硬门槛通过，性能取舍可量化，真实跨机证据齐全。

### T15：最终审查与交付

**依赖：** T14。  
**步骤：**

1. 对照第 5 节 I01～I18 和第 19 节逐项给证据。
2. 检查最终 diff，只包含本任务范围。
3. 检查默认配置未变、实验模式未提前升级为默认。
4. 检查文档示例能在安装目录执行，所有路径、目标、选项真实存在。
5. 审查还有没有每话题 UDPNode / send socket / recv thread 泄漏到 shared_v1。
6. 验证旧模式下不意外连接网关或初始化新网络线程。
7. 提交交付摘要：改动、验证、性能、明确限制、回滚。
8. 按当前 AGENTS.md 使用团队 MCP 回报；若工具不可用，明确记录无法执行，不伪造回报。
9. 按团队约定处理上下文压缩；没有可调用能力时不宣称已经执行 /compact。

**退出条件：** 所有必要项有结果，未通过项不被写成完成。

## 16. 可复制的构建、运行与取证流程

本节命令分为“现在已有的构建命令”和“本任务必须实现后才可运行的命令”。
执行者每阶段仅运行已创建的目标；不能把 command not found 当作测试跳过。

### 16.1 基线记录

~~~bash
cd /home/zwc/cpp_ipc_dds
git rev-parse HEAD
git status --short
git diff --stat
uname -a
cmake --version
c++ --version
~~~

另记录 CPU、内存、网卡速率、MTU、编译类型和关键环境变量的允许列表。
不要导出全部 env，避免把无关凭据写进日志。

构建使用新的独立目录，不能复用正在运行其他任务的 build 缓存：

~~~bash
cmake -S /home/zwc/cpp_ipc_dds -B /home/zwc/cpp_ipc_dds/build-shared-net \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_DEBUG_INFO=OFF \
  -DLIBIPC_BUILD_TESTS=ON \
  -DLIBIPC_BUILD_PYTHON=OFF \
  -DUPDATA_MSG_SRV_GENERATOR=OFF
cmake --build /home/zwc/cpp_ipc_dds/build-shared-net --parallel 4
ctest --test-dir /home/zwc/cpp_ipc_dds/build-shared-net -N
~~~

DZIPC_BUILD_SHARED_NET 只有 T01 增加后才可以在配置里显式指定。
Python 测试另建启用 Python 的构建，不用 OFF 的构建假称验证 Python。
性能基线和新模式都检查真实编译命令；当前 ENABLE_DEBUG_INFO=ON 可追加 -O0，
不能只凭目录名或 CMAKE_BUILD_TYPE 判断优化级别相同。

### 16.2 新测试的登记与执行

每个 test_shared_net_*.cpp：

1. 确认是否已被 test/CMakeLists.txt 的 GLOB 创建目标。
2. 若已自动创建，不重复 add_executable。
3. 显式 add_test。
4. 设置标签 shared_net 和合理 TIMEOUT。
5. Linux 专用用例以 CMake 条件注册，不把未实现平台判为运行成功。
6. 集成测试的端口和目录按测试实例分配，或设置 RESOURCE_LOCK 串行执行。
7. 先 ctest -N 检查预期数量，再执行；0 tests 不是通过。

~~~bash
ctest --test-dir /home/zwc/cpp_ipc_dds/build-shared-net \
  -L shared_net -N
ctest --test-dir /home/zwc/cpp_ipc_dds/build-shared-net \
  -L shared_net --output-on-failure --timeout 90
~~~

新模式测试需要 MPMC=1；旧回归需要其原本配置。
不要给整个 CTest 一律设置 DZIPC_SHM_MPMC=1，否则旧单发布者语义用例可能被错误改变。
由各测试的 ENVIRONMENT 属性或驱动脚本为单个子进程设置环境。

### 16.3 网关启动示例（T13 后）

以下以 UID=1000、网卡 eth0、主机 A 地址 192.168.10.10 为例。
根据实际环境替换参数，不能在不存在的地址上测试失败后改为任意地址继续。

~~~bash
DZIPC_SHM_MPMC=1 build-shared-net/bin/dzipc_gateway serve \
  --listen-ip 192.168.10.10 \
  --interface eth0 \
  --control /tmp/dzipc-gateway-1000/control.sock \
  --data-base-port 24000 \
  --data-shards 4 \
  --control-port 24004 \
  --discovery-group 239.255.250.251 \
  --discovery-port 24005
~~~

应用进程的配置：

~~~bash
DZIPC_NET_BACKEND=shared_v1 \
DZIPC_SHM_MPMC=1 \
DZIPC_GATEWAY_CONTROL=/tmp/dzipc-gateway-1000/control.sock \
build-shared-net/bin/shared_net_probe --role sub --topic /example/image --domain 7
~~~

shared_net_probe 是 T09 必须新增的集成程序，不是现有命令。
至少实现：

- --role pub|sub；
- --topic、--domain、--msg-id；
- --messages、--bytes、--publisher-count；
- --encoding tlv|dzflat、--delivery best-effort|reliable；
- --seed、--output、--ready-file；
- 收到重复、内容错误、数量不符时非零退出；
- 输出 publisher_id / sequence / payload_hash，可据此重算丢失与重复。

测试驱动等待 ready-file 或结构化 Ready 应答，不能以固定 sleep 猜接收端已就绪。
本机 SUB_READY 与跨机可发送是两个检查点。要求零启动丢包的正确性测试须先创建两端对象，
由驱动查询源端已安装目标 SUB/route epoch、目标端已安装源 PUB/完整名称，再放行发送屏障。
单个应用 ready-file 不能代替这两个方向的目录检查；另设启动竞态用例验证未同步时的丢弃/重试。

### 16.4 单机模拟两台设备

优先使用专用测试环境中的两个 network namespace + 独立 IPC 环境，或两个隔离容器。
两者需各有独立 locality identity、Unix 目录和 SHM 挂载/命名环境。
**只改变 IP、只换端口或只创建两个 network namespace，不一定隔离本机 SHM 身份。**

网络故障工具优先支持：

- 用户态报文注入/转发适配，适用于无需特权的可重复单元和集成测试；
- 独立测试 veth 上的 tc netem，适用于实际网络栈验证；
- 真实两台设备，适用于最终性能与跨机验收。

不得在用户日常使用的 eth0 上直接增加全局丢包规则。
测试脚本记录自己创建的 namespace、接口和 PID，并只清理这些对象。

### 16.5 资源采样

~~~bash
ss -uapn
ls /proc/<gateway_pid>/fd
cat /proc/<gateway_pid>/status
cat /proc/<gateway_pid>/smaps_rollup
build-shared-net/bin/dzipc_gateway status \
  --control /tmp/dzipc-gateway-1000/control.sock --json
~~~

尖括号是占位符，执行时替换为测试记录的 PID。
正式脚本用 Python /proc 解析，不依赖 ls 文本作为唯一计数证据。

每一轮分别保存：

- 启动前；
- 网关 Ready、无话题；
- 1 / 100 / 1000 topic 稳定后；
- 流量峰值期间；
- 注销全部 topic 后；
- 网关退出后。

同时记录应用进程和网关。不能只展示应用 RSS 下降而隐藏网关增长。

## 17. 验收矩阵与硬门槛

### 17.1 协议与隔离

| 编号 | 用例 | 必须断言 |
|---|---|---|
| V01 | 160B 网络头 golden bytes | 偏移、字节序、CRC 全部一致 |
| V02 | 0 / 2^32 / 2^40+3 / UINT64_MAX 域 | 域不截断，不串话 |
| V03 | 同 msg_id=0、同 socket 的两个话题 | 只投到各自话题 |
| V04 | 相同 publisher sequence、不同 publisher_id | 重组不混合 |
| V05 | 强制话题指纹碰撞，含远端仅 PUB | 本机注册拒绝或跨端目录匹配拒绝，不按错误 topic 交付 |
| V06 | DATA 缺头、截断、超长、未知版本 | 拒绝且无异常退出 |
| V07 | 整数乘加溢出、超大 message_size | 分配前拒绝 |
| V08 | 片 CRC 正确但整包 CRC 错 | 整条不交付 |
| V09 | 乱序、重复、同片内容冲突 | 内容正确或整条明确丢弃 |
| V10 | TLV 跨旧页边界字段 | 原始 blob 保持，订阅者正确解析 |
| V11 | DZFlat / GenericMessage / Python | 平坦段生命周期正确，不被转 TLV |
| V12 | 旧协议发给新端点、新协议发给旧端点 | 明确不接受，不误反序列化 |
| V13 | RouteKey / scope / CRC 标准向量 | DZS2 恢复、36B 编码、跨语言哈希与 CRC 一致 |
| V14 | 同包发给两个数据 shard，双方 K 不同 | 只允许规范 shard 接纳，业务 SHM 只提交一次 |
| V15 | 错误源 IP/端口、错误确认元数据 | 不建重组、不终结真实事务、不向第三方回包 |
| V16 | 仅 PUB、仅 SUB、PUB+SUB、空目录 | 来源能核验，只有 SUB 可作为发送目标，空目录原子撤销 |
| V17 | 同前 127B、不同后缀的话题名 | 内部路由和目录不截断、不合并；工具标明展示截断 |

### 17.2 资源与功能

| 编号 | 用例 | 必须断言 |
|---|---|---|
| R01 | K=4，T=1/100/1000 | 网关 UDP socket 恒为 6 |
| R02 | 单应用 1000 话题 | 应用新增 shared_v1 UDP socket 为 0 |
| R03 | 同话题 32 个本机订阅者 | 每远端消息网关重组/注入各一次 |
| R04 | 2 / 8 个本机发布者 | 相互不清段，退出一个不影响其他 |
| R05 | 两台主机互相发布 | 每个接收者恰好收到其应收消息，无回流环 |
| R06 | 100 次增删话题 | FD / 线程 / 活跃配额无单调增长 |
| R07 | 加入大量远端 peer | socket 仍固定，内存受 peer 和快照上限约束 |
| R08 | 某话题触顶 | 该话题失败，其他话题仍推进 |
| R09 | 新订阅者迟到、退订、重订 | 全部 Ready 归零后重建拒收旧 epoch；非最后订阅者变动遵守 8.3 的在途边界 |
| R10 | topic_cat 订阅远端消息 | 正常注册，读一次，无额外 UDP socket |
| R11 | 已安装目录、候选与旧版本并存 | 分别受总内存配额限制，替换失败不发布半张表 |
| R12 | 最大 blob 与各 loan 档位边界 | 按容量记账；DZTX 填充不上网，出站信用未超额 |
| R13 | 反复重建发布者直至 stream/历史触顶 | 拒绝新身份，旧消息重放不再次提交，诊断指出容量原因 |

“恰好一次”只在本次测试的网关/路由代次存活条件下断言，不扩展成跨崩溃 exactly-once 保证。

### 17.3 可靠性与故障

| 编号 | 注入条件 | 必须断言 |
|---|---|---|
| F01 | 首轮所有 DATA 丢失 | 发送探测重试后可恢复或按 deadline 失败 |
| F02 | 1% / 5% 丢片 | 缺片重传；只交付完整正确消息 |
| F03 | 完成 ACK 丢失 | 重传不再次 SHM commit，补 ACK 后成功 |
| F04 | A 成功、B 不可达 | blocking 失败并表明可能部分交付 |
| F05 | SHM 提交明确失败 | 不发送成功 ACK |
| F06 | SHM 提交不确定 | 不自动重复提交 |
| F07 | ACK 与 timeout 并发 | SEND_RESULT 只终结一次 |
| F08 | 网关 SIGKILL | 等待者退出，旧记录不被新网关重放 |
| F09 | 网关 SIGSTOP，第二实例启动 | 第二实例拿不到锁 |
| F10 | 目标网关重启 | 旧事务 PeerRestarted，新事务重新发现 |
| F11 | publish_prebuilt 提交前失败 | 返回 false 后回退只交付一次 |
| F12 | 提交成功后控制连接断开 | 不返回“可以安全回退”的结果 |
| F13 | 最后订阅者退出时仍有分片 | 旧 route epoch 的包被拒绝 |
| F14 | 旧 Sample 持有期间网关退出 | Sample 内容和释放过程仍合法 |
| F15 | 应用 fork 后使用旧句柄 | 在获取继承锁前失败，无死锁 |
| F16 | 发送/接收/快照配额逐个触顶 | 精确错误和回收，无无界增长 |
| F17 | peer 租约过期、同 epoch 恢复 | 重新同步目录且保留去重，旧 DATA 不再提交 |
| F18 | 新 epoch 接纳后重放旧 HELLO/PAGE | retired epoch 永不重新安装，不反复切代 |
| F19 | 空闲与回执 TTL 后重放相同消息 | 窗口仍拒绝重复；失去回执也不伪造 ACK |
| F20 | tm=0、慢编码、发送超时后网络延迟包 | 零超时无提交；期限不重置；晚交付符合文档而非撤销保证 |
| F21 | DATA 已发送但 ACK 全丢 | 即使 acked_count=0，失败仍标记可能部分交付 |
| F22 | 出站通知合并/重复、控制写满、记录覆盖 | 信用不提前/重复归还；异常会话明确失效，可靠调用不假成功 |

故障测试随机种子必须固定并保留。
正确性用例在合理负载下以“零串话、零错误载荷、零不期望重复”为硬门槛。
best-effort 的故障丢包可以发生，但必须与计数和序号缺口一致。

### 17.4 性能报告

统一在相同 CPU、网卡、MTU、编译优化、payload 和订阅拓扑下比较：

| 场景 | 主要看什么 |
|---|---|
| 纯本机 1 pub -> 1 sub | 网关额外一跳的 p50/p99 与 CPU 成本 |
| 纯本机 1 pub -> 8/32 sub | 分发成本、SHM 消费能力 |
| 跨机 1 pub -> 1 host / 多进程 sub | 避免重复网络重组的收益 |
| 跨机 1 pub -> 2/8 host | 单播发送带宽放大 |
| 1000 个低频话题 | socket、FD、RSS、发现快照、空闲 CPU |
| 少量高频大话题 | shard 饱和、发送公平、可靠重传 |
| 同话题 8 个发布者 | 重组并发、锁竞争、配额分布 |

每个性能场景至少 3 次独立运行，包含预热和明确的测量窗口，报告中位数与范围。
不能删去不利轮次，只能标明机器异常并完整保留原始数据。

若目标部署尚未提供性能预算：

- 功能和固定 socket 门槛仍必须通过；
- 报告完整数据；
- 模式维持显式启用；
- 不宣称“性能验收已满足所有部署”。

### 17.5 公平性基准

建立可重复的初始门槛：

- 热话题：1 MiB，发送速率限制在本机/网络测得可持续吞吐的 70%；
- 冷话题：64B，100 Hz；
- 同时进行 HELLO、订阅变更和可靠 ACK；
- 冷话题 30 秒窗口内无超过 1 秒的停滞；
- 所有 reliable 调用在配置 deadline 加 250 ms 测试调度余量内返回；
- 无队列或重组内存持续增长。

这验证不会被错误的完整消息阻塞循环饿死，不代表最终实时性 SLA。
本基准使用已知有界的编码器，并从公开调用开始计时；另外用慢编码用例验证编码返回后
不再超期提交，不把不可抢占的用户编码时间隐去。
若平台调度使 250 ms 余量不适用，记录时间线解释，不直接删除超时断言。

## 18. 启用、迁移与回滚

### 18.1 启用步骤

1. 完成前置 MPMC 和本方案全部必要验收。
2. 选择一组独立测试 topic/domain；确认应用/网关处于同一可达本机运行实例。
3. 选择实际空闲的端口组，前台启动网关并查看 Ready。
4. 两端确认 shared_v1 协议和限额兼容。
5. 先启动订阅端并等待 SUB_READY；创建发布对象后先不发送，按第 16.3 节确认两端
   话题与 peer 状态均已同步，再放行需要避免启动丢包的业务流量。
6. 检查 gateway status、UDP socket 数和错误计数。
7. 低流量运行，确认数据正确后再逐步提高流量。
8. 为目标部署验证延迟与单播带宽预算。
9. 只为选定应用进程设置新环境变量，不全局覆盖所有用户进程。

### 18.2 跨版本与混合部署

- legacy 与 shared_v1 同时存在时，它们各自使用对应协议和端点。
- 不承诺同名 topic 会自动跨协议互通。
- 同机业务 SHM 只有在 MPMC 段名/布局/类型一致时才可互通。
- 如必须桥接旧网络，另建显式桥接进程、独立命名与去重规则，并单独验收；
  不把桥接放入本方案的隐藏兼容分支。
- 旧端口可能与新默认端口重叠，部署者必须按真实 bind 结果配置。
- 更新 gateway_epoch 或协议版本后，旧在途事务不迁移到新实例。

### 18.3 回滚步骤

1. 停止新模式应用继续提交。
2. 让可靠调用在原 deadline 内结束，记录未完成/部分投递。
3. 停止新模式应用对象和网关。
4. 对要回滚的应用设置 DZIPC_NET_BACKEND=legacy，或取消该变量。
5. 根据前置 SHM 版本决定是否保留 DZIPC_SHM_MPMC=1；
   本任务回滚不应擅自回滚另一个任务的 SHM 布局。
6. 重启对应应用并验证旧网络协议。
7. 仅在确认无活跃使用者和 loan 后清理本任务独占的出站段。
8. 不对整个 /dev/shm 执行清理，不清业务 topic 的在用段。

回滚会中断在途通信；没有持久化重放。业务若自行重试，需用业务 ID 处理可能的重复。

## 19. 最终交付检查表

- [ ] T00 前置 SHM 实际完成且有提交/测试证据。
- [ ] T01～T15 均有记录；未通过项明确列出。
- [ ] 默认后端仍为 legacy。
- [ ] Linux 以外旧构建未因平台头或新增源失败。
- [ ] 共享模式应用没有每话题 UDP socket。
- [ ] 网关 UDP socket 数为 K+2，1000 topic 下有系统级采样证据。
- [ ] 分片有完整消息身份，交错多发布者测试通过。
- [ ] 完整域、话题、msg_id、schema、epoch 校验有效。
- [ ] PUB-only 来源全名可核验；错误 shard、错误确认元数据与旧 HELLO 均被拒绝。
- [ ] ACK 只在确定 SHM commit 后发送。
- [ ] 部分交付、超时和不确定结果文档与实现一致。
- [ ] 预构造段 false 不产生可见提交。
- [ ] 重组、重传、快照、去重和本机队列有硬配额。
- [ ] 活跃/候选/旧目录、发送目标状态和 peer 历史均有配额，去重不因 TTL/LRU 丢失。
- [ ] DZTX 按实际 loan 容量计费，信用与记录长度边界验证通过。
- [ ] tm=0、晚交付、零 ACK 的不确定结果与订阅加入边界已写入用户说明并验证。
- [ ] 重启、析构、最后订阅者退出和旧 Sample 均验证。
- [ ] 远端入站不会再次出站。
- [ ] TLV 页尾完整保留，DZFlat 不经 TLV 中转。
- [ ] 发现迟到、丢页、撤销、版本更新有效。
- [ ] 诊断显示真实状态，不把内部端点计成业务发布者。
- [ ] topic_cat 和 Python 相关路径有验证。
- [ ] 所有新测试实际执行，测试数非零，skip 明确。
- [ ] 至少两台真实主机的正确性和性能证据齐全。
- [ ] 本机额外延迟与单播带宽放大已量化。
- [ ] 回滚演练完成，没有误清其他 Agent / 业务资源。
- [ ] 最终 diff 仅包含本任务改动。
- [ ] 团队回报已完成，或工具不可用事实已说明。

## 20. 执行记录模板

每卡使用独立记录，表格只放结论与证据索引。开始时全部保持“未开始”。

| 任务 | 状态 | 提交/源码指纹 | 构建目录 | 测试数/失败/跳过 | 证据 | 未解决事项 |
|---|---|---|---|---|---|---|
| T00 | 未开始 | | | | | |
| T01 | 未开始 | | | | | |
| T02 | 未开始 | | | | | |
| T03 | 未开始 | | | | | |
| T04 | 未开始 | | | | | |
| T05 | 未开始 | | | | | |
| T06 | 未开始 | | | | | |
| T07 | 未开始 | | | | | |
| T08 | 未开始 | | | | | |
| T09 | 未开始 | | | | | |
| T10 | 未开始 | | | | | |
| T11 | 未开始 | | | | | |
| T12 | 未开始 | | | | | |
| T13 | 未开始 | | | | | |
| T14 | 未开始 | | | | | |
| T15 | 未开始 | | | | | |

单卡详细记录：

~~~text
任务：
开始/结束时间：
依赖是否已满足：
基线提交与工作区指纹：
本卡修改文件：
关键实现决策：
运行命令（完整）：
退出码：
预期测试数 / 实际测试数：
通过 / 失败 / 跳过：
资源采样：
故障种子和注入点：
未解决事项：
下一卡可开始的证据：
~~~

### 20.1 本次文档复核记录（不是实施完成记录）

2026-10-05 对照 e887d5e 与共享工作区复核，补充来源目录角色、36B RouteKey、收包校验顺序、
peer 代次历史、去重保留条件、订阅加入边界、远端晚交付、出站信用和 loan 档位容量。
同步任务卡、构建开关约束及 V13～V17、R11～R13、F17～F22 的验收要求。
前置 MPMC 文档仍有后续压力/故障/兼容验收未完成项；本次未执行 T00～T15，任务状态不变。
新增协议字段是待实现的 DZMX v1 设计修订，不能声称与尚未冻结的旧草案编码互通。

## 21. 执行者遇到问题时的处理规则

1. 找不到文中符号：先用 rg 按类名/函数名定位，再更新文档锚点；不创建同名重复实现。
2. 前置 SHM 接口变化：保留本方案行为契约，写小适配；不强转 route 与 MPMC 类型。
3. 文档与当前实现冲突：当前实现决定基线事实，本方案决定新模式目标；记录实际差异。
4. 底层原子提交不能满足：停在 T05 的边界做专项验证；不通过重试隐藏重复风险。
5. 旧测试失败：在基线上复跑一次分类，已有问题记录；本次引入的问题修复后再推进。
6. 性能没有提升：先核对是否消除了重复工作、是否遇到单播放大或网关一跳；报告数据。
7. 没有跨机环境：完成本机和模拟环境测试，并明确真实跨机验收仍未完成。
8. 缺用户部署参数：采用本文默认值制作可评审实现，不改用户实际网卡或全局系统配置。
9. 不确定网络/SHM 可靠语义：按第 5 节做失败处理，不声称端到端应用消费成功。
10. 不把 SO_REUSEPORT、多个 epoll waiter、共享线程池当作主机消息分发协议。
11. 不把端口减少、编译通过或单消息成功当作完整交付。
12. 只有第 19 节必要项通过，才报告本方案完成。

## 22. 明确延期的优化

以下不属于首版执行范围，禁止为了追求功能齐全在主线中顺手加入：

- 本机发布直接 SHM 与网关之间的动态快路切换；
- 数据组播、自适应单播/组播和跨网关路由转发；
- 网络严格 FIFO、持久化历史和跨崩溃 exactly-once；
- 零拷贝网卡收包、直接重组到业务 SHM loan；
- 运行时 K 调整与动态 CPU 迁移；
- 同话题多 schema 并存；
- Windows / QNX / IPv6 / 32 位进程新后端；
- 旧网络协议透明桥接；
- RPC / 服务请求响应端点共享；
- 自动安装 systemd 服务和开机启动。

这些能力可以在首版证据基础上另立任务。它们的延期不降低本文明确要求的正确性和资源门槛。
