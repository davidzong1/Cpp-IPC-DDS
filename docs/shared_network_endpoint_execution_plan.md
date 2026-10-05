# 共享网络端点与按话题分发：详细执行方案

编制日期：2026-10-04  
复核日期：2026-10-05（本机直达与共享网络低延迟架构修订）\
源码基线：f066a82，已包含 MPMC 交付与上一版共享网络方案。\
文档状态：**完整方案修订；共享网络代码尚未实施，不代表性能已经验证。**\
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
| T04～T05 | 4.5、5、9（含两类信用）、12 |
| T06～T07 | 5、6、7、10、12 |
| T08～T10 | 7.5～7.7、8、9.4、11、12.4、17.2～17.3 |
| T11～T13 | 3.5、5、9、10、12、14、18 |
| T14～T15 | 16～20 |

本文件中所有端口、协议和返回语义都是**新模式的设计契约**，默认旧模式不随文档改变。
本次修订替代 f066a82 中“所有本机消息先经网关”的方案，不与旧草案混合实施。
网络 DZMX/DZGD/DZGC 仍为 v1；本机 DZLC/DZTX 升为 v2，DZTX 前缀为 112B。
用户已批准按完整方案开始执行；T01～T03 已完成，T00 基线补证部分完成，当前状态见第 20 节。

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
7. 本机交付直接使用 MPMC，不依赖网关逐消息转发；可靠发送热路径不强制进行本机 BEGIN 往返。
8. 老模式仍可构建和运行，显式配置新模式后不在运行中偷偷切回老模式。

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

资源目标是减少网络端点与本机重复收包；延迟目标是移除本机网关转发、逐消息 BEGIN 往返
和全局统一 SHM 提交队列。**优化目标须由第 17.4 节的同机对照证明，不预先承诺倍数提升。**

MPMC 已交付可作为前置基线。本轮重新构建成功、MPMC/info 专项 8/8 通过；交付记录中的
完整回归 8 项旧基线失败、尚缺的压力/性能证据仍单列，不将专项通过扩大成全部验收通过。

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
| include/dzIPC/common/shm_channel.h | f066a82 已包含 route / mpmc_channel 类型封装 | 本机发布者和网关注入端复用已交付 MPMC 生命周期 |
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

### 3.4 本机 MPMC 直达，网关只处理跨机数据

SharedPublisher 在应用内持有自己的 MPMC 发布端，本机消息由它直接提交。
需要网络输出时，再经每进程出站 SHM 交给网关；网关对这些记录只向远端发送，
绝不再次写入源主机业务 SHM。远端入站才由目标网关注入其业务 MPMC。

- SharedSubscriber 仍通过 shm_sub_ipc 消费业务 SHM，没有第二条网络消费腿。
- 显式 IPC_SHM 发布者保持仅本机语义；网关不订阅业务 SHM 并无差别向外转发。
- 同一话题可有多个应用发布者和一个网关注入端，复用 MPMC 普通 join/leave。
- 本机路径与网络路径的所有权固定，不根据发现状态把本机投递在应用和网关之间来回切换。
- 一条消息可能本机成功、网络失败，或反过来；不实现跨两条队列的原子广播。
  第 5 节定义部分提交、公开 bool 与预构造段回退边界。
- 已初始化的本机 MPMC 路径不因网关运行中退出而关闭；网络腿明确失效。
  新建 shared_v1 对象仍要求完成网关握手，避免把初次网络配置失败隐去。
- 无远端需求的 best-effort 可使用原有 DZFlat 直接写 loan 的本机路径；
  有网络输出时允许先生成一个不可变 WireBlob，向两腿提交，不能重复序列化业务对象。
  首版不跨进程共享该 WireBlob 的裸指针；共享载荷加描述符另列后续优化。

本机直达消除了网关调度依赖，不等于消除了编码、SHM 复制或订阅者调度。
必须分别测仅本机路径和本机/网络同时活跃路径的端到端延迟。

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
  P["应用 SharedPublisher"] --> LS["本机业务 MPMC SHM"]
  LS --> S["本机订阅者"]
  P --> TX["每进程网络出站 SHM"]
  TX --> G["本机网关：仅发送远端副本"]
  G --> N["少量 UDP 单播端点"]
  N --> RG["远端网关：重组与注入"]
  RG --> RS["远端业务 MPMC SHM"]
  LP["显式 IPC_SHM 发布者"] --> LS
  IN["其他主机的网络入站"] --> GI["本机网关入站分片线程"]
  GI --> LS
~~~

每应用进程一个 Unix SOCK_SEQPACKET 控制连接负责注册、额度补充、路由提示、状态和
可靠结果。业务载荷不走控制连接；已有额度时，单条 reliable 无须先发控制请求再等许可。

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
| 应用控制连接 / 网络出站通道 | 每进程 1 个 / 1 条，不是每个话题一个 |
| 应用本机 MPMC 发布端 | 每 SharedPublisher 一个发布者登记；段与池按业务话题共享 |
| 业务 SHM 通道 | O(T)，仍按话题隔离 |
| 发布者登记和订阅登记 | O(P + S)，无法因端口复用消失 |
| 重组状态 | 与并发在途消息相关，必须有硬配额 |
| 网络载荷带宽 | 单播约为消息大小 × R，重传另计 |

验收必须同时报告 UDP socket、全部 FD、内核 socket 缓冲、RSS、SHM、线程和流量。
仅展示“固定端口减少”不算完成资源目标。

### 4.3 本机发布

1. 初始化应用 MPMC 发布端与网关 PUB 登记；本机发布对象的生命周期由应用拥有。
2. 公开调用入口校验消息、句柄和 tm；分配 publisher_id + sequence，冻结本机接收面。
3. runtime 读取已同步的网络需求提示；reliable 总是请求网关给出本次远端结果，
   best-effort 仅在已知无远端需求时可以省略网络腿，未知状态仍尝试提交网络。
4. 可靠等待者在任何可见提交前登记。共用 WireBlob 时先完成编码；仅本机时可直接写 loan。
5. 在调用线程向本机业务 MPMC 做一次有界提交，不等待网络额度、网关调度或远端 ACK。
6. 网络腿在已授予信用内提交一条 DZTX v2 记录。best-effort 信用不足即记该腿未提交；
   reliable 可在本机提交之后等待异步补充信用，但不超过原 deadline。
7. 网关读取网络记录，冻结远端目标并发送；从不为该记录重新提交源主机业务 SHM。
8. best-effort 返回本次接管汇总；reliable 在应用内合并本机提交结果与远端 SEND_RESULT。

冻结本机接收面和冻结远端目标发生在两个明确时点，不声称它们构成跨主机原子快照。
本机提交明确失败时可继续尝试网络腿；本机结果不确定时也不得自动重投本机。
所有分支都必须服从第 5 节的一次调用所有权规则。

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

同一进程、同一控制路径复用一个 SharedClientRuntime，管理网络会话、出站通道、
信用账本与可靠等待表。各 SharedPublisher 的本机 MPMC 发布端不由 runtime 统一转发。

- 一个控制读循环负责接收应答、ROUTE_STATE、CREDIT_GRANT、TX_PROGRESS 和 SEND_RESULT。
- 多个发布线程禁止竞争 recv 同一控制连接；不持注册表锁等待额度或 ACK。
- PID 改变在获取继承 mutex 前判定；旧句柄拒绝使用，支持 fork 后 exec。
- 网关断连使网络等待者返回 GatewayLost，并立即使缓存网络需求/信用失效。
  已存活本机发布端和订阅端继续按 MPMC 运行；新一次 best-effort 可仅本机接管并报告网络离线。
- publish_blocking 要求网络查询完成；网络离线时即使本机已投递仍返回失败并标明可能部分交付。
- 旧网络会话不自动迁移到新网关；新建对象可使用新 runtime，旧网络句柄保持失效。
  本机端仍存活不代表旧 publisher_id、旧出站段或信用可在新会话重放。
- 对象整体析构、reset_message 或 InitChannel 才按第 12 节停止自己的本机端。
  网络会话关闭与应用对象关闭须是两种独立状态。

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
| I09 | publish_prebuilt_segment 返回 false 前，本机与网络两腿都明确没有可见提交 |
| I10 | 旧模式的入口、协议和默认配置保持原行为 |
| I11 | 注销先撤销路由、等待在途工作，再释放 FD / route / pool |
| I12 | 所有重组表、发送表、去重表和队列都有字节数与条目数上限 |
| I13 | 网络入站不写出站队列，避免转发环路 |
| I14 | domain 使用 64 位，无窄化成 int / uint32_t |
| I15 | 普通话题加入/离开不创建/关闭网关数据 socket |
| I16 | 不通过清空共享 socket 接收队列清理某个话题或消息 |
| I17 | 来源 ID 不使用 IP:port、PID、msg_id 或单独 sequence 代替 |
| I18 | 达到资源上限显式失败/丢弃并计数，不无界分配或反复创建线程 |
| I19 | 应用本机直达与网关网络出站各司其职；出站记录不再次注入源主机 SHM |
| I20 | 已有发送信用时，单条消息无前置 BEGIN 控制往返 |
| I21 | 每个入站 RouteKey 由一个 shard 负责重组和提交，不经过全局统一提交线程 |

### 5.1 返回语义

新模式将“本机接管”“网络出站接管”和“远端网关确认”分开报告：

| 接口 | shared_v1 中 true 的含义 | false / 失败边界 |
|---|---|---|
| publish / publish_best_effort | 至少一腿确定接管，或存在不可安全重试的不确定提交 | 两腿均明确未提交；可能是无目标、资源不足或编码失败 |
| publish_prebuilt_segment | 至少一腿已经或可能接管该合法完整段，调用者不得回退重发 | 仅两腿都明确未提交时返回 false |
| publish_blocking(msg, tm) | 本机冻结目标若存在则提交成功；网络查询完成且冻结远端全部确认；至少有一个实际目标 | 任一必需腿失败、未知或超时；可能已有部分目标收到 |
| has_subscribed | 本机有效 SHM 接收者存在，或健康网络会话有已同步的远端匹配需求 | 两者均无或网络状态未知；不作为后续投递保证 |

best-effort 的 true 表示调用者不得把相同调用当作未发生而自动重试，不表示两腿都成功，
也不表示已发 UDP 或所有应用已消费。保守的 true/Indeterminate 必须有具体诊断，不能计入
成功吞吐。预构造段在本机成功、网络失败时返回 true；其网络失败通过结果记录和指标暴露。

应用内结果记录按 publisher_id + sequence 保存：local_required、local_result、
network_result、remote_target_count、remote_acked_count、possible_partial_delivery。
不新增公共虚函数；采用有界诊断接口/记录，不把所有历史结果永久保留。

可靠网络 SEND_RESULT 只描述远端。远端返回 NoSubscribers 但本机冻结目标已成功时，
整体可以成功；本机无目标且远端也无目标则整体 NoSubscribers。GatewayLost 不等同于
“远端无目标”，即使本机成功，整体仍失败。源应用被杀后不持久化或重放该调用。

可靠 ACK 仅确认目标网关将完整消息提交业务 SHM，不保证每个应用回调完成，
不保证慢订阅者免于既有 SHM 覆盖策略，不承诺跨网关崩溃的 exactly-once。

### 5.2 顺序语义

首版按同一发布者生成序号，用于身份和去重；**网络交付不承诺严格 FIFO**。
不同消息可因网络乱序、不同目标调度、重传而先后颠倒。
跨发布者也没有全局顺序。

本机应用若逐次调用 publish_blocking 并等待成功，再提交下一条，
在目标持续存活、路由不变的条件下可获得该调用链的先后关系。
不要把此特例写成所有 publish 都保证顺序。

需要严格网络 FIFO 的部署应继续使用已满足其要求的模式；
增加重排窗口、缺失序号跳过协议属于后续独立任务。

### 5.3 两腿提交与回退边界

每腿结果至少区分：NotRequired、NotSubmitted、Accepted/Committed、Indeterminate。
本机接收面不存在时 local=NotRequired；已同步无远端需求的 best-effort 可使 network=NotRequired。
NotRequired 不是某个已选目标提交失败，不允许用它掩盖断连或配额失败。

| 本机结果 | 网络出站结果 | best-effort / prebuilt | reliable 的处理 |
|---|---|---|---|
| 两腿均明确未提交或不需要 | 同左 | false，可由调用者决定回退 | 失败或无目标 |
| Committed | NotRequired（best-effort 已知无远端） | true，仅本机接管 | 不适用；可靠仍查询远端结果 |
| Committed | NotSubmitted / 离线 | true，记录本机成功、网络失败 | false，可能部分交付 |
| NotSubmitted（本机目标已选） | Accepted | true，记录本机失败 | false，网络可能仍交付 |
| NotRequired | Accepted | true，记录仅网络接管 | 等待远端结果 |
| Committed | Accepted | true | 等远端结果后合并 |
| 任一腿 Indeterminate | 任意 | true，暴露未知结果，不重投 | false，可能部分交付 |

两腿的预留只减少部分失败概率，不提供原子提交。顺序固定先本机后网络；本机提交后
网络失败不能调用普通 publish 重做本机，也不能由网关补注入同一源消息。
本机一次 try_commit 返回 NotSubmitted 后，首版不在本次调用中自动重试本机；远端入站
NotSubmitted 的有限重试仍由接收 shard 管理。业务重试须使用自己的业务 ID 处理可能重复。

两腿资源分别用 RAII 归还。网络出站的异常覆盖或 Indeterminate 使该网络会话失效，
不能猜测信用恢复值；本机是否已投递仍保留在调用结果中。T05/T06/T11 必须用真实故障
证明这些边界，不能仅凭底层 bool 推断“没有发生任何可见提交”。

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
- 首次缺片等待和请求间隔使用第 11.4 节参数，默认 2 ms；首次计时从首片建立 assembly 起。
  只有新片增加时才更新进展，不因重复片无限推迟；有进展可延后一次请求但不延长绝对期限。
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
网关 bridge 只为远端入站服务，保活条件是待完成 SUB、Ready SUB 或在途 lease。
本机仅有 SharedPublisher 时，网关只登记 PUB 来源，不创建供本机出站回注的 bridge。
应用本机发布端独立持有 MPMC 租约；最后一个网络订阅者离开，不会关闭应用发布端。

### 8.4 发送目标与需求提示

- 本机目标由应用在调用内冻结为有效 MPMC 接收面；提交失败不能事后改为“本机无目标”。
- 远端目标由网关接纳网络出站记录时冻结，只选已安装目录的 SUB 位，不包含自己。
- reliable 不根据应用缓存提前跳过网络查询；即使没有远端，网关也返回明确 NoSubscribers。
- 已有目标在发送中途退出或换 epoch，原事务失败；不能删掉失败目标后声称全部成功。
- best-effort 可根据 ROUTE_STATE 中健康、版本匹配的 remote_ready_count=0 跳过网络腿。
  缓存未知则尝试网络；缓存滞后造成的新订阅启动丢包按既定 best-effort 边界计数。
- ROUTE_STATE 在 PUB_REGISTERED 后及对应路由需求变化时推送，携带 session/epoch、
  publisher_id 和单调 state_version。只接受本会话更新；网络失联后所有缓存都变 Unknown。
- has_subscribed 合并应用自己的 SHM 接收面和网络需求提示，不能同步调用 QUERY_STATE
  作为每次 publish 的前置步骤。跨机零启动丢包测试仍使用第 16.3 节双向发现屏障。

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

### 9.2 DZLC v2 控制消息

本机控制头显式编码，固定 40B，整数为网络字节序：

~~~text
magic[4] = DZLC
u16 version = 2
u16 kind
u32 total_size
u32 flags = 0
u64 request_id
u64 session_id
u64 gateway_epoch
~~~

版本 1 的旧草案不兼容；收到旧版本明确失败，不能按新 body 长度猜测解析。
HELLO 的 session_id/gateway_epoch 为 0，之后所有包必须匹配会话。
普通请求使用非零且递增的 request_id；主动通知为 0，SEND_RESULT 复用出站记录的请求 ID。

| 编号 | 消息 | body 字段顺序 / 效果 |
|---:|---|---|
| 1 | HELLO | locality[16]、u64 process_start_token、u64 clock_domain_id、u32 capabilities |
| 2 | WELCOME | locality[16]、u32 max_message_bytes、u32 outbox_limit_bytes、u32 outbox_record_limit、u64 granted_bytes、u64 granted_records、u16 tx_name_len、tx_name |
| 3 | ATTACH_TX | 空 body，SCM_RIGHTS 恰好一个非阻塞 eventfd |
| 4 | TX_READY | 空 body，网关出站 receiver 已连接 |
| 5 | REGISTER_PUB | publisher_id[16]、RouteDescriptor；登记来源，应用拥有本机发布端 |
| 6 | PUB_REGISTERED | publisher_id[16]；随后推送初始 ROUTE_STATE |
| 7 | REGISTER_SUB | handle_id[16]、RouteDescriptor |
| 8 | SUB_REGISTERED | handle_id[16]、u32 shm_generation、u64 receiver_route_epoch（未 Ready 可为 0） |
| 9 | SUB_READY | handle_id[16]、u32 shm_generation |
| 10 | SUB_READY_ACK | handle_id[16]、u64 receiver_route_epoch（非零） |
| 11 | UNREGISTER | handle_id[16]、u8 role（1=PUB，2=SUB） |
| 12 | UNREGISTERED | handle_id[16] |
| 13 | QUERY_STATE | u8 kind（0=汇总，1=单话题，2=话题与 peer）；kind=1/2 后跟 scope[32]、u32 msg_id；kind=2 再跟 peer_id[16] |
| 14 | STATE | u32 json_bytes、UTF-8 JSON；含头总长不超过 8192B |
| 15、16 | 保留 | 旧草案 SEND_BEGIN/BEGIN_READY，不允许发送或接纳 |
| 17 | SEND_RESULT | publisher_id[16]、u64 sequence、u32 result_code、u32 flags、u32 target_count、u32 acked_count |
| 18、19 | PING / PONG | u64 nonce；PONG 原样返回 |
| 20 | ERROR | u32 error_code、u16 text_bytes、UTF-8 有界说明 |
| 21 | TX_PROGRESS | u64 released_records、u64 released_capacity_bytes；累计归还出站 SHM 信用 |
| 22 | ROUTE_STATE | publisher_id[16]、u64 state_version、u32 remote_ready_count、u32 flags（bit0=状态已同步） |
| 23 | CREDIT_REQUEST | u32 min_bytes、u32 min_records；请求补充发送信用，不绑定某条业务消息 |
| 24 | CREDIT_GRANT | u64 granted_bytes、u64 granted_records；累计授予量，回应请求时回填其 request_id，主动补充时为 0 |

RouteDescriptor 重用第 8.2 节条目，登记时 receiver_route_epoch/role_flags 均为 0；
HELLO.capabilities 首版固定 bit0=支持 MPMC V2，必须为 1，未知位拒绝。
角色由消息编号决定。类型不匹配、长度不足、多余尾部、未知标志均拒绝。
重复 SUB_READY/UNREGISTER 按句柄与代次幂等；重复 request_id 内容冲突返回 RequestConflict。
SEND_RESULT.request_id 必须匹配 DZTX 的 reliable 请求；它不是控制连接上曾发出的 BEGIN ID。

SEND_RESULT 只描述远端投递：flags.bit0 保留为 0，bit1 表示 possible_remote_delivery；
任意 DATA 已交给 sendto 后失败都要保守置 bit1，零 ACK 不证明零交付。
result_code：0=Completed、1=NoSubscribers、2=Busy、3=TimedOut、4=PeerGone、5=PeerRestarted、
6=Rejected、7=GatewayLost、8=Cancelled、9=Indeterminate、10=UnsupportedTimeout。
网关不伪造本机提交结果；应用按第 5.1 节合并自己的 local_result。
连接断开时 GatewayLost 由应用 runtime 本地生成，不要求失联网关发回结果。

clock_domain_id 使用可验证的本机时钟 namespace 身份，并结合 SO_PEERCRED 验证；
不一致则拒绝会话。Unix recvmsg 检查 MSG_TRUNC/MSG_CTRUNC，异常附带 FD 全部关闭。
只有 ATTACH_TX 接受 SCM_RIGHTS；重复 ATTACH 关闭额外 FD。控制、eventfd 和锁 FD 均设 CLOEXEC。

### 9.2.1 发送信用：预授予、异步补充、分别结算

需要两个独立账本，不能把“出站 loan 已消费”当成“网络发送缓存已释放”：

| 账本 | 计费 | 可再次使用的时机 |
|---|---|---|
| 出站 SHM 信用 | 实际 loan 容量 C(112+payload_size) 与 1 个记录槽 | 网关释放该 loan 后的 TX_PROGRESS |
| 网络发送信用 | W(payload_size) 字节与 1 个发送状态槽，best-effort/reliable 都计费 | 网关结束发送/丢弃并释放缓存后重新 CREDIT_GRANT |

WELCOME 包含初始发送信用；默认请求初始 1 MiB/16 条，实际授予受全局剩余配额限制，可为 0。
每会话发送窗口最大 32 MiB/512 条；大于初始信用的消息可由后台 CREDIT_REQUEST 补充。
每会话至多一个未完成补充请求，控制循环按会话轮转授予，拒绝不能满足的请求并报告 Busy。
初始信用和补充信用都是真实预留，不允许向 128 个客户端各承诺全部 256 MiB 总预算。
网络计费函数 W(n)=align_up(n,64)，先验证溢出；网关 WireBlob 的可用容量固定为 W(n)，
分配器/控制块额外开销计入有界元数据预算。不能复用更大容量的 buffer 却只按小消息扣费。
后台在信用不足以容纳待发消息或低于本会话窗口的 1/4 时合并补充；无需求时不反复申请。
Busy 后至少退避 1 ms 再请求，且不越过调用 deadline；这不是每条消息固定执行的握手。

对每个会话，granted 是累计单调值；客户端保存 used 与本地 reserved，
可用量等于 granted-used-reserved。重复/旧 GRANT 只按增加量应用，不能重复获得信用。
任一计数即将溢出时结束网络会话；所有计数仅在当前 session/epoch 内有效。

1. 编码前可先预估大小；准确大小确定后同时预留两类信用和等待表项。
   额度不足不得影响已具备条件的本机提交；reliable 的后续信用等待受原 deadline 限制。
2. 出站提交前明确失败，撤销本地预留。所有权转交或不确定时计入 used，不能自行退信用。
3. 网关验证记录费用与会话余额，再把预授予预算转为该发送状态的实际占用；不重复扣费。
4. TX_PROGRESS 只回收出站容量/槽，发送状态继续占用网络信用直到其终结。
5. 终结后的信用可按公平策略再授予原会话或其他等待会话；客户端未用的授予仍占预算。
   首版不强制撤销客户端手中信用；每会话上限限制长期占用，关闭会话后再安全收回。
6. 网关断连先封住网络提交、使信用失效，待出站读者和发送状态收敛后释放该会话预算。

已有足够信用时，publish 的网络腿没有前置控制往返。信用不足、冷启动和首次大消息
仍可能等待补充或失败，应单独测量，不能把这类停顿从 p99 报告中删除。
目标列表/位图在网关冻结目标时另受配额限制；信用不是“全部目标必能接纳”的承诺。

### 9.3 每进程一条出站 SHM

首版使用独立 ipc::route：应用是唯一 sender，网关是唯一 receiver。
同进程多个发布线程只在提交环节用短互斥串行化；序列化和等待可靠完成在锁外。

使用单 sender route 的原因是每进程只需一个提交者实例。
不要因“多发布者”三个字就让每个发布对象各建一个底层 sender。
业务 SHM 仍使用前置任务的 MPMC 通道，二者不共用段名或控制面。

段名：

~~~text
dzgw_tx_v2_<locality_hex>_<gateway_epoch_hex>_<session_id_hex>
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

1. 停止接收本会话的新网络提交；整体对象析构另行封住其本机入口；
2. 完成或取消本进程可靠等待；
3. 注销逻辑句柄；
4. 网关摘除 eventfd 并等待相应处理结束；
5. 双方释放出站通道和 eventfd；
6. 仅按本会话所有权回收独占段，不清理业务 SHM。

### 9.4 DZTX v2 出站记录

固定前缀 112B，网络字节序；仅描述网络发送，本机提交结果不写入此头。

| 偏移 | 长度 | 字段 | 规则 |
|---:|---:|---|---|
| 0 | 4 | magic | DZTX |
| 4 | 2 | version | 2 |
| 6 | 2 | header_size | 112 |
| 8 | 8 | gateway_epoch | 当前网络会话代次 |
| 16 | 8 | session_id | 当前会话 |
| 24 | 16 | publisher_id | 已登记发布者 |
| 40 | 8 | sequence | 非零消息序号 |
| 48 | 32 | scope | DZS2 规范编码 |
| 80 | 4 | msg_id | 具体业务 ID |
| 84 | 4 | payload_size | 不含信封与档位填充 |
| 88 | 4 | schema_hash | 与业务段匹配 |
| 92 | 1 | encoding | 1=TLV，2=DZFlat |
| 93 | 1 | delivery | 0=best-effort，1=reliable |
| 94 | 2 | reserved | 0 |
| 96 | 8 | request_id | reliable 非零；best-effort 为 0 |
| 104 | 8 | deadline_monotonic_ns | reliable 原始调用 deadline；best-effort 为 0 |
| 112 | payload_size | blob | 完整业务载荷 |

应用在出站提交前登记等待者，再发布记录；极快的 SEND_RESULT 也能匹配，不需要 BEGIN。
网络等待表与控制请求表共享非零 request_id 分配器，不能发生两种请求的 ID 冲突。
同一个 publisher_id/sequence 只允许提交一次；重复记录是协议错误，不重复扣费或重新投递。

deadline 使用 Linux CLOCK_MONOTONIC 的 uint64 纳秒时间；只在已验证相同时钟域的本机
进程间比较，不传给远端。网关接纳记录时重新检查期限；到期返回 TimedOut 并结算信用。
tm 从公开调用入口算起，包含编码、本机提交、信用等待、排队及远端确认。
reliable 的 request_id/deadline 都必须非零；剩余期限大于 5000 ms 的记录拒绝。
best-effort 两字段必须为零；不能借它们绕过排队超时规则。

ipc::buff_t.size 可能是尺寸档位容量。先以 64 位运算检查
112+payload_size <= buffer_capacity，再只提取实际载荷；尾部填充不能进入 UDP 或 CRC。
记录的 session、epoch、PUB 身份、scope、msg_id、schema、编码和信用必须逐项验证。
不接受旧版 96B 头，也不尝试推测新旧混合布局。

### 9.5 提交、唤醒与信用回收

- loan 容纳 112B 信封和 blob，填完后 publish_loan；真实失败可见性先由 T05 验证。
- 应用所有发布线程共用本会话的短提交锁与账本；编码、信用等待、ACK 等待在锁外。
- 不使用可能部分提交多段的大记录 try_send 伪装原子接管；原子性不满足时停在 T05 修适配。
- 成功提交后 eventfd_write；通知是提示，不是消息计数。合并、重复和 EAGAIN 不改变队列事实。
- 一个出站 drain 循环读取各会话并按预算轮转，转发给 RouteKey 所属源 shard；
  它不提交业务 SHM，不在一个应用或大话题上等完整网络发送。
- 预算耗尽且仍有记录时加入 deferred 队列，不依赖下一次应用通知。
- WireBlob 首版仍复制到网关私有缓存后释放出站 loan；复制期间两份内存分别计费。
  这保留明确的跨进程所有权，避免重传长期钉住应用出站池。
- loan 释放后的 TX_PROGRESS 每轮 drain 结束合并发送，不人为积攒 20 ms；最后一批也须发送。
  网络缓存释放后的 CREDIT_GRANT 同样及时排入控制队列，两者不能互相代替。
- 信用窗口不得超过底层实际可用槽位。T05 验证正常窗口内无 force_push 覆盖未读记录；
  检测到覆盖/不确定进度则关闭故障网络会话，不能猜测容量或重放记录。
- best-effort 因无远端目标被丢弃，也要完成两类信用结算；控制连接堵塞受有界队列限制，
  不能为等待信用的消息新增通道或线程。

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
- 现有 serialize 跨页时先递增 now_page；该历史字段按原字节保留，不验证为新外层分片下标。
- 在本机订阅者使用现有 AcceptWire / deserialize_ok 做最终类型与结构校验。

编码函数建议统一成 WireEncoder::encode / encode_prebuilt，返回拥有生命周期的 WireBlob。
最大尺寸、空指针、无效 schema 的失败发生在出站提交前。

### 10.2 应用本机写入与网关原始注入接口

新增非虚内部适配 ShmWireWriter/ShmWireBridge，复用 shm_pub_ipc 的 MPMC 生命周期：

~~~text
open(RouteDescriptor) -> Ready / MpmcRequired / TypeConflict / Failed
try_commit(WireBlob)  -> NotSubmitted / Committed / Indeterminate
try_commit_local(message_or_prebuilt) -> 同样三态，仅本机时可直接编码到 loan
close_after_quiescent()
~~~

应用 writer 按发布对象持有；网关 bridge 按入站 RouteKey 持有一个，不能共用跨进程 C++ 对象。
应用与网关都是普通 MPMC 发布者，均复用注册、心跳、generation、借样池和租约。

1. 网关注入使用 GenericMessage 模板及登记的 msg_id，不要求编译用户消息头。
2. 两种原始 blob 直接提交，不把 DZMX/DZTX 头写入业务 SHM，不重复反序列化/序列化。
3. 有网络腿时先编码一次不可变 WireBlob；应用先本机提交，再向出站 loan 复制网络副本。
4. best-effort 已知只有本机需求时允许 DZFlat 直接写业务 loan，省掉中间 WireBlob。
   不改变本机提交者，不因此引入网关回注或双路切换。TLV 仍保持完整历史页尾。
5. 原始注入不走进程内对象注册表扇出；本机路径复用 nodelet 时须另证无重复和借样配额，
   首版可固定走 MPMC，并记录相对已有 nodelet 的性能差异。
6. 不修改公共虚表；借样失败、discard 和已发布所有权复用前置已验证契约。
7. 本机与网络的部分提交由上层合并；适配器不在可能提交后回退另一种编码再发。
8. 网关中一个话题多个远端来源只占一个 publisher slot；应用本机发布者各自占一个 slot。
9. 本机慢消费者的覆盖策略不因网络 reliable 而变成逐应用确认。

初次 open 可使用有界初始化流程；消息热路径不调用等待 Ready 的 InitChannel。
单次非阻塞提交仍可能消耗编码/复制时间，必须计入第 17.4 节延迟测量。

### 10.3 ACK 与注入结果

- Committed：记录去重成功，发送 ACK。
- NotSubmitted：可在保留的有界 CommitPending 队列中重试，或明确 REJECT。
- Indeterminate：标记本消息拒绝/未知，发送 REJECT(ShmCommitIndeterminate)，禁止再次提交。
- 应用本机提交前检查公开调用 deadline；目标网关的远端入站仅使用自己的
  first_seen + 5000 ms 重组/注入期限，不能读取或推算发送端绝对 deadline。
- best-effort 注入失败直接计数并丢弃。
- 在可靠等待期间，不长期占用业务 SHM 的读端 loan；发送重传缓存使用独立 WireBlob。
- 应用持有 Sample 时网关退出，Sample 仍须有效到其既有租约结束。

DZMX v1 不携带远端取消或调用剩余时间。发送者超时会停止后续发送并返回失败，
已经发出的报文仍可能随后重组并提交；超时返回不是撤销投递，也不是“此后不会再收到”。
两端时钟无需同步；不能把本机 DZTX 的单调时钟纳秒值拿到另一主机比较。

### 10.4 shard 内提交状态与关闭边界

每条完整远端消息保留 DeliveryTicket：AssemblyKey、WireBlob、route lease、配额令牌
和提交状态。收包、去重、try_commit 与结果处理均由同一 RouteKey 所属 shard 负责。
没有“每条消息先交给全局提交线程，再等待回传”的必经步骤。

~~~text
Ready -> Committing -> Committed
                    -> NotSubmitted
                    -> Indeterminate
Ready -> Cancelled
~~~

1. 完整校验后进入 Ready；所属 shard 转 Committing，再执行一次有界 try_commit。
2. NotSubmitted 可进入该 shard 的有界延期重试队列；同一消息同时只有一次尝试。
3. Committed 先记录回执/去重，再向控制线程排 ACK；Indeterminate 保留墓碑，不再提交。
4. 控制线程只发送 Closing/Cancel 命令并使路由不可再公告，不跨线程释放 ticket 或修改去重。
   已进入 Committing 的消息不能被声明“未提交”；必须等所属 shard 返回明确结果。
5. 路由注销 barrier 等待已有提交结果与 lease 收敛，随后才能释放或重建 bridge。
   新订阅代次不能接收旧代次尚在执行的提交。
6. 正常 stop 先停止接纳并排关闭命令；各 shard 处理取消、清理和确认后才 join，
   控制线程不能持路由锁同步等待它们。

测试在进入提交前、进入后、SHM 可见后、记录回执前暂停，注入超时、注销和重复 DATA，
检查最多一次业务提交。生产路径不插入测试等待；应用直达提交另测发布与析构/reset 并发。
如果将来增加异步复制线程，必须重新提供所有权交接协议，不能沿用单线程假设。

## 11. 共享 IO、调度与可靠发送

### 11.1 固定有界线程模型

- 1 个控制循环：Unix 会话、发现、可靠控制报文、信用和路由管理。
- K 个数据 shard：独占各自 UDP FD，发送、收包、重组、去重及本 shard 的业务 SHM 注入。
- 1 个出站 drain 循环：从每应用一条网络出站 SHM 读取并向源 shard 分发。
- 1 个有界初始化工作线程：执行可能等待的 SHM open，Ready 后移交给指定 shard。
- 应用本机 MPMC 提交在调用线程执行，不经上述网关线程。

网关主体为 K+3 个线程，UDP 为 K+2 个 socket；MPMC 控制调度器等额外线程仍要如实计数。
初始化队列默认 64 项，注册默认等待 2 秒；超时后迟到的初始化结果必须撤销。
跨线程使用有界队列及 eventfd，按报文/字节/时间预算让出，不建立每话题线程。

### 11.1.1 状态归属表

| 状态 | 唯一写入者 | 其他线程如何访问 |
|---|---|---|
| session、peer、已公告路由、版本 | 控制循环 | 不可变快照或有界命令 |
| 发送信用授予、会话总预算 | 控制循环 | drain/shard 返回结算事件，不自行重复授予 |
| 出站 SHM receiver 与消费进度 | 出站 drain 循环 | 控制线程处理累计 TX_PROGRESS |
| AssemblyState、去重、入站 bridge 提交 | 对应数据 shard | 控制线程发送关闭/代次命令 |
| reliable TxState、目标位图、重传 | 按本机 RouteKey 选出的源 shard | 控制循环转发 ACK/NACK |
| bridge 初始化 | 初始化工作线程，尚未 Ready | 一次性移交目标 shard |
| 应用本机提交状态 | 各发布对象的串行提交入口 | 析构/reset 等待在途调用收敛 |
| 应用等待表、信用、网络提示 | client runtime 的短锁保护 | 唯一控制读循环更新并唤醒 |

队列消息携带 session/epoch、RouteKey/route epoch 和稳定 lease，不传可能悬空的裸指针。
初始化结果只能移交一次；同一 bridge 不同时由两个 shard 提交。改变 K 必须重启网关。
控制循环发布 Closing 后通过 barrier 收敛，不能跨线程直接销毁仍在提交的 bridge。
C++17 不可变 shared_ptr 快照使用 atomic_load/atomic_store 重载，不要求 C++20。
网关和 runtime 的全局注册表锁不能覆盖编码、网络 IO、SHM 等待或可靠等待。

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

### 11.4 可靠发送状态机与重传

应用状态与网关状态分别管理：

~~~text
应用：Validate -> WaiterRegistered -> LocalAttempt -> CreditReady/WaitingCredit
     -> NetworkEnqueued -> MergeRemoteResult -> Completed / Failed / TimedOut
网关：RecordAccepted -> FreezeRemoteTargets -> Sending -> WaitingAcks
     -> Completed / NoSubscribers / TimedOut / PeerGone / PeerRestarted / Rejected
~~~

- 应用在任何提交前登记等待者，先本机尝试，再进入可能需要等待的网络步骤。
- 网关状态从实际出站记录开始；没有 SEND_BEGIN，也没有逐消息 AwaitingRecord 占位。
- 网络目标只在网关接纳记录时冻结；本机结果由应用合并，网关不代替本机提交。
- 每远端目标分别记录首发片、缺片、确认和重试时刻；NACK 与待发片任务合并，
  尚未首发的片保留正常任务，不额外复制。一个消息/目标/片只允许一个待发任务。
- 所有首轮分片交给 sendto 后才启动无确认探测，不能在大消息尚未首发完时复制整包。
- 首次缺片等待与发送探测初始值均默认 2 ms，允许配置 1～20 ms；探测指数退避到
  默认 100 ms 上限。实际参数必须写入性能报告，1 ms 配置不代表能保证 1 ms 恢复。
- NACK 请求默认至少间隔 2 ms；ACK/NACK 与重传有每 peer 和全局速率预算。
  跟踪误重传与带宽，不能仅靠缩短计时器提高“看上去”的恢复速度。
- 全部 DATA 丢失依靠发送探测；ACK 丢失重传同一身份，目标去重后补 ACK。
- tm 单位为毫秒，公开上限 5000 ms；tm=0 在编码、本机提交和网络提交前直接 TimedOut。
  超上限或无限值返回 UnsupportedTimeout，不悄悄缩短。deadline 运算检查溢出。
- 编码结束、本机提交前、信用获得后、出站提交前及每次 sendto 前检查同一 deadline。
  不可抢占的用户编码耗时单列，不能在慢编码之后重新计算完整的 tm。
- ACK 与超时由所属状态机单次终结；调用超时使应用 waiter 终结，但已入网关的缓存仍
  由网关按原 deadline 安全回收，应用不能提前退发送信用。
- socket EAGAIN 使用可写事件或有界延期队列，不 busy loop；超时后不再补发。
- best-effort 首发排队上限 1000 ms，开始发送后最长 5000 ms，超期丢弃并结算信用。
- 使用 sendmmsg/recvmmsg 批量处理已经就绪的报文，每批最多 32 包且受本轮预算限制；
  不为了凑批睡眠。部分发送只推进已成功的前缀，剩余报文保留原身份和 deadline。
- CRC 和包头可复用已验证的不可变计算结果，但不得跳过校验、改变分片语义或提前 ACK。

首版不实现 RTT 自适应或忙轮询；先测可配置短定时器与批量 IO，后续优化另立证据。
跨主机可靠完成还包括网络往返和远端 SHM 提交，不能与 best-effort 的本机接管时间混比。

### 11.5 发送公平与控制优先级

- 每 topic 设置有界发送队列，调度使用按字节计费的轮转。
- 一个大消息不能一次连续发送全部分片后才轮到小消息。
- 同一 peer 存在首发积压时，重传最多占本轮数据发送预算的 50%；没有首发时可借用余额。
- 各 shard 内的发送与注入重试按 RouteKey 轮转，失败热话题不得阻塞其他通道。
  非阻塞不等于零 CPU 时间；大 blob 的一次复制若超出时间预算，需在报告中显示。
  单话题固定一个 shard，因此增加 K 不会自动提高一个热话题的吞吐。
- ACK/NACK 优先于订阅快照分页；二者均有总速率限制。
- 控制线程必须能在大流量下及时处理 stop / unregister / deadline。
- QoS / CPU 参数首版只影响明确的网关配置，不能把每个应用对象的绑核参数直接修改共享线程。
- 若公共构造传入与网关冲突的专用绑核要求，报告该要求不适用于共享后端；
  不静默承诺它已经生效。

### 11.6 调度与重传参数

以下参数由 T01 暴露为网关命令行选项，check-config 验证边界，status 输出实际值。

| 参数 | 默认值 | 有效范围 / 约束 |
|---|---:|---|
| --io-batch-max | 32 包 | 1～64，仍受单轮字节/时间预算限制 |
| --io-round-packets / --io-round-bytes / --io-round-us | 64 / 65536 / 200 | 分别为 1～4096 / 1～16777216 / 1～1000000；单包处理不可抢占 |
| --nack-delay-ms | 2 ms | 1～20 ms |
| --nack-interval-ms | 2 ms | 1～20 ms |
| --retry-initial-ms | 2 ms | 1～20 ms |
| --retry-max-ms | 100 ms | 不小于 initial，不大于 5000 ms |
| --control-rate | 10000 包/秒 | 正数，受总预算上限校验 |
| --peer-control-rate | 1000 包/秒 | 正数，不大于 control-rate |
| --control-burst | 64 包 | 正数；每 peer 突发最多 16 包且不超过全局突发 |

控制速率限制计入 ACK、NACK、REJECT 与目录分页，发送队列仍有独立内存上限。
ACK/NACK 高优先级，但为等待中的目录分页保留至少 10% 的发送机会；没有分页时可借用。
限速只延后或丢弃可重试报文，不伪造 ACK；重复缺片请求合并，不建立逐次重传任务。
本机 Unix 信用通知不占 UDP 包速率，但受控制循环预算及其有界输出队列限制。

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
| 全部发送缓存及未使用发送授予 | 合计 256 MiB | CREDIT_REQUEST 无额度；记录不能超额接纳 |
| 每 publisher reliable 在途 | 64 | Busy |
| 发送状态及未使用记录授予 | 合计 4096 / 每会话 512 | 无信用则等待原 deadline 或 Busy |
| 发送目标状态（含 best-effort） | 总 32768 | 冻结目标前 Busy / 丢弃并计数 |
| 本机 CommitPending 总字节 | 64 MiB | 拒绝或留在原重组配额内，禁止重复不记账 |
| 去重 stream 数 | 16384 | 新 stream 拒绝 |
| 每 stream 去重窗口 | 4096 个序号 | 依第 7.6 节处理 |
| 可靠去重回执数 | 总 65536 / 每 peer 8192 | 接纳前拒绝新 reliable |
| 单进程出站占用字节 | 32 MiB | 仅网络腿 Busy，不撤销已发生本机提交 |
| 每会话发送信用窗口 | 32 MiB / 512 条 | 异步补充，受全局总预算限制 |
| 初始发送授予请求 | 1 MiB / 16 条 | 按剩余预算实际授予，允许为 0 |
| 应用结果诊断记录 | 每 runtime 256 条 | 覆盖最旧诊断；活动 waiter 不得随之删除 |
| 未完成信用补充请求 | 每会话 1 项 | 合并请求，不按每条消息建请求 |
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
| 网关 SIGKILL | 网络会话失效；可靠返回 GatewayLost/可能部分投递；已初始化本机 MPMC 继续运行 |
| 网关 SIGSTOP | 不发生锁接管；健康超时停止网络提交，本机直达仍可运行 |
| 网关重启 | 新网络会话与出站段；旧网络句柄失效但本机端仍存活；新建对象重新注册，不重放 |
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

1. 网关设置 Stopping，拒绝新网络注册/记录；不擅自销毁应用拥有的本机 MPMC 发布端。
2. 对外停止公告新订阅，通知本机会话。
3. 取消可靠事务，发出可发送的终结结果，唤醒所有等待。
4. 各 shard 处理停止命令，取消尚未提交的 ticket，收敛已开始的 SHM 提交并返回关闭确认。
   drain 停止读新记录；信用与 buffer 按会话清算。然后摘除 FD、退出等待并 join 网关循环。
5. 清理 assembly / retry / commit 队列，使 buffer 和 loan 按 RAII 归还。
6. 注销内部业务 SHM 发布端，等待已有在途引用归零。
7. 关闭出站会话与独占段，关闭 Unix socket。
8. 清理本实例控制路径，最后释放锁 FD。
9. 重复 stop 必须幂等。

不使用全局 rm /dev/shm，不使用 killall，不清空整个 UDP 接收缓冲作为退出步骤。

### 12.4 配额账本与容量计算

条目与字节同时设限。跨线程节点、片位图、每目标状态、定时器、解析索引和历史记录均有上界。
T01 固定有界队列配置，T05/T07/T10 验证预留失败会全部归还，不能留下半个事务。

| 阶段 | 必须预留 | 归还/转移时机 |
|---|---|---|
| REGISTER | 句柄、登记、目录及实际需要的 MPMC 槽位 | PUB 应用端与 SUB 网关 bridge 分别持有，失败按所有权撤销 |
| CREDIT_GRANT | 全局发送缓存预算及发送状态槽 | 转为实际记录占用或保持未使用授予；会话关闭才可撤销未消费授予 |
| 应用出站准备 | 出站实际容量/槽、发送信用、可靠 waiter | 明确未提交撤预留；可能提交不自行退信用 |
| 冻结远端目标 | 每目标位图和状态 | 失败一次归还，不因目标退出悄悄缩小成功集合 |
| 远端首片接纳 | assembly、完整容量、位图、stream、可靠回执位置 | blob 按状态释放，去重历史按 7.6 保留 |
| shard 内提交等待 | 队列项、route lease | 同一 buffer 转移不重复计费，复制才增加占用 |
| 目录替换 | 候选、新解析目录及仍被引用的旧目录 | 旧引用归零才释放旧版本 |

发送预算必须满足：所有会话未使用授予 + 已提交未终结记录 <= 全局上限。
出站 loan 消费完但 WireBlob 仍在发送时，只返 TX_PROGRESS，不把发送预算也释放。
多个目标可共享一个不可变 blob，但目标状态/重传位图仍为 O(R)，在首发前预留。

令 M 为最大 blob，C(n) 为底层真实 loan 档位：

~~~text
outbox_record_bytes = 112 + M
session_outbox_limit >= C(outbox_record_bytes)
session_tx_window_bytes >= W(M)          # 要允许本会话发送配置的最大消息
business_loan_capacity >= M
fragment_count = (M + 1023) / 1024        # 宽整数计算
~~~

16 MiB blob 加 112B 信封可能进入 32 MiB loan 档位。出站按实际 C(n) 计费，
发送缓存按第 9.2.1 节 W(M) 容量计费，分配器额外开销计入元数据上限。
池映射通常包含同档多个 chunk，映射容量、tmpfs 实占、RSS 与在途 loan 是不同口径，分别报告。
不能把 32 MiB 出站占用上限称为整个应用或网关的 SHM 内存上限。

目录分别测已安装、候选和旧版本被事务引用三种占用。4096 话题、128 peer 等是各自上限，
不表示其所有组合都能同时接纳。发布者 churn 留下的必要去重/身份历史也必须单独计数。

应用直达仍受每话题 MPMC 发布者、接收者和 chunk 槽位限制；网关只为有入站需求的话题
占一个发布者槽。两个腿的真实复制内存不得以“同一业务消息”为由只算一份。

## 13. 文件与接口落点

### 13.1 新增模块

以下文件均为建议的精确落点。需要改名时先更新本表和测试引用。

| 文件 | 责任 |
|---|---|
| include/dzIPC/net/shared_config.h | 模式、限额、端点配置与纯解析接口 |
| src/dzIPC/net/shared_config.cc | 参数校验、进程级配置读取 |
| include/dzIPC/net/wire_protocol.h | WireHeader、编解码结果、错误枚举；不含 socket 平台头 |
| src/dzIPC/net/wire_protocol.cc | 160B 网络头编解码、CRC、分片校验 |
| src/dzIPC/net/byte_codec.h | 已检查长度后的内部端序与 UTF-8 辅助，不作为公共 API |
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
| include/dzIPC/net/local_protocol.h | DZLC v2 控制消息、112B DZTX v2 与错误码 |
| src/dzIPC/net/local_protocol.cc | 控制消息验证、golden bytes 支撑 |
| include/dzIPC/net/client_runtime.h | 网络会话、两类信用、路由提示、可靠等待与本机结果合并 |
| src/dzIPC/net/client_runtime.cc | 注册/注销、fork 闸、连接关闭处理 |
| include/dzIPC/net/shm_wire_bridge.h | 应用 ShmWireWriter 与网关 ShmWireBridge 的内部适配 |
| src/dzIPC/net/shm_wire_bridge.cc | MPMC 生命周期与原始注入 |
| include/dzIPC/net/gateway_runtime.h | 网关公开启动/停止/状态接口 |
| src/dzIPC/net/gateway_runtime.cc | 路由归属、出站 drain、分 shard 注入与信用汇总 |
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
- local_path / network_path 独立健康状态；客户端本机结果由客户端诊断提供，不由网关推断；
- 出站可用容量/槽、未用发送授予、实际在途发送预算、信用等待次数/时长；
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
| 提交 | local_committed、local_not_submitted、local_indeterminate、network_accepted、network_not_submitted、partial_submit |
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
| 发送信用 | tx_credit_unused / inflight、credit_wait_count / time、credit_grant_rejected |
| 延迟分段 | encode、local_commit、credit_wait、outbox_wait、network_first_send、remote_commit、ack_wait、api_return |

热路径只更新有界计数，不对每个分片同步写文本日志。
网关指标只证明远端入站注入，应用本机投递次数须由应用计数；不能把两者重复汇总为同一消息。
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

### T00：确认已交付 MPMC 与性能基线

**前置事实：** MPMC 已随 f066a82 保存；本轮重编成功、专项 8/8 通过，完整回归历史问题见前置文档。
这些事实不替代本卡尚需补充的规模、原子性和性能证据。

**必须读取：** MPMC 交付记录、ShmChannel/PublisherRegistry、SHM loan/提交/析构与相关专项测试。

1. 记录实际源码提交、构建指纹、64 位平台和 MPMC layout V2；不把历史阶段的“未接入”当成现状。
2. 核对应用发布者及网关注入端的 slot 数、接收者上限、诊断池容量和完整话题名。
3. 核对 loan 失败、持有 loan/Sample 时退出、同话题发布者加入/退出不清段的证据。
4. 测 112B 信封加最大 blob 的真实 loan 档位与映射；准备 T05 两类信用边界数据。
5. 采集已有 legacy hybrid 和直接 MPMC 的 1/100/1000 话题资源，以及本机小包/大包延迟。
6. 记录跨机单目标/多目标基线；无真实跨机环境时标明未完成，不用模拟结果替代最终发布验收。
7. 旧基线已知失败单列，关键 MPMC 生命周期若出现新失败则先定位；独立协议纯函数工作可继续。

**产物：** baseline.md、MPMC 能力表、原始性能/资源数据与已知问题列表。
**退出条件：** 行为依赖明确、源码可复现；本机延迟对照可用于第 17.4 节，不把专项交付写成性能通过。

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
4. 实现 DZGD v1 64B、DZGC v1 84B、DZLC v2 40B、DZTX v2 112B 头。
5. 固定 DZLC v2 编号及两类信用、ROUTE_STATE 编码；15/16 保留且拒绝旧 BEGIN。
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
8. 可靠等待表先使用假完成事件验证先登记、早到结果和本机/网络结果合并。
9. 用假发送状态验证 WELCOME 初始信用、CREDIT_REQUEST/GRANT、ROUTE_STATE 和会话断连；
   控制循环只管理网络状态，不能关闭应用本机端。

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
10. 分别验证出站容量信用与发送缓存信用；TX_PROGRESS 不能提前释放网络在途预算。
11. 测初始零信用、大消息补充、重复 GRANT、消费早于 publish 返回、最后一批进度与关闭结算。
12. 信用充足的单条 reliable 直接提交 DZTX，不发送逐消息 BEGIN；信用不足等待有原 deadline。
13. 在每个档位边界验证 DZTX 长度裁剪，填充尾部不得上网；信用窗口内不得覆盖未读记录。

**测试：** test_shared_net_outbox、test_shared_net_outbox_failure。  
**退出条件：** 真实跨进程原子接管证据齐全；可安全说明预构造段 false 的边界。
**禁止：** 以“通常不会失败”跳过原子性；以 Unix 载荷传输临时代替并宣布完成。

本卡仅证明网络腿的边界，整个 prebuilt 的 false 还要在 T06/T11 合并本机提交结果后验收。

### T06：应用本机直达与远端原始注入

**依赖：** T00、T02；使用测试 blob 即可验证，不依赖真实网络。
**改动：** ShmWireWriter、ShmWireBridge 与 shm_pub_ipc 非虚内部适配。

1. 应用每发布对象持有自己的 MPMC 端；网关仅为入站话题持有一个 bridge。
2. DZFlat/TLV 原始注入不重复解析；仅本机 DZFlat 路径可直接编码到 loan。
3. 普通 IPC_SHM 发布者、SharedPublisher 本机适配、网关注入端共同向多个进程发布，验证无重复和清段。
4. 验证 NotSubmitted/Committed/Indeterminate，特别是本机成功后网络失败不能再次本机提交。
5. 持有 Sample 时退出任意发布者，其他发布者继续工作，lease 释放合法。
6. shard 独占网关 bridge 的提交；应用 writer 不被网关停机或 drain 操作销毁。
7. 核对逻辑与内部登记，不把一个网关 bridge 计成额外业务发布者。

**测试：** test_shared_net_shm_bridge、test_shared_net_local_direct。
**退出条件：** 两类编码、多发布者并存、本机三态提交及析构租约通过。

### T07：分片、重组与去重

**依赖：** T02、T03、T06。  
**改动：** reassembly、quota、CommitPending 和去重窗口。  
**步骤：**

1. 先实现可喂入单个报文的纯状态机。
2. 一个报文只推进对应 AssemblyKey，不读取下一包、不等待缺片。
3. 乱序和重复分片仍得到同一完整 blob。
4. interleave 两个发布者、相同 msg_id / sequence / 大小但不同 payload，验证不会混包。
5. 再 interleave 两个话题，刻意使用相同数据 socket。
6. 完整 CRC 后在所属 shard 进入 CommitPending，一次只允许一个提交尝试。
7. 同 shard 完成提交后记录 Committed/回执，不经全局提交线程；NotSubmitted 进入有界重试。
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

### T09：本机直达与 best-effort 网络端到端

**依赖：** T05～T08。
**改动：** 双路径编排、完整网关数据路径、shared_net_probe 与测试驱动。

1. 应用直接 MPMC 提交给两个本机订阅进程；暂停网关后，既有本机端仍能收取新消息。
2. 加入远端网关/订阅者；源网关处理出站记录时本机 SHM 提交计数必须为 0。
3. 两台主机同时发布同一话题，本机直达与远端入站各按原始来源只交付一次，无回流。
4. 单远端主机多个订阅者，源端仍只发一份目标载荷，目标网关仅重组/注入一次。
5. 验证网络提示为零、未知、滞后和失效时的行为；无远端 best-effort 可省去出站。
6. 穷举本机成功/失败/未知与网络接管/失败组合，公开返回与计数服从第 5 节。
7. shared_net_probe 输出两腿结果、publisher/sequence/内容校验和，驱动使用双向发现屏障。
8. 核验应用 shared_v1 UDP=0、网关=K+2；尚不切换默认公共工厂。

**测试：** test_shared_net_end_to_end、test_shared_net_partial_submit。
**退出条件：** 多进程、本机独立运行、跨机单份传输及部分提交语义均验证。

### T10：无逐消息 BEGIN 的可靠发送与公平性

**依赖：** T09。
**改动：** reliable_session、信用结算、SEND_RESULT、短定时器与批量 IO。

1. 应用先登记等待者，直接 DZTX 提交请求 ID/deadline；信用充足时抓控制流证明不存在 BEGIN。
2. 本机提交由应用记录，远端目标由网关冻结；NoSubscribers/GatewayLost 的合并含义不同。
3. ACK 通过统一控制循环转所属 shard，发送调用不 recv UDP；ACK 只在目标 SHM 提交成功后发。
4. 全丢、缺片、ACK 丢失、重复 NACK、超时与 ACK 并发均只终结一次，不重复提交。
5. 验证默认 2 ms 定时、配置边界、退避和速率预算；比较恢复延迟与误重传成本。
6. sendmmsg 部分成功/EAGAIN 保留未发送后缀；recvmmsg 逐包检查截断，不等待凑批。
7. 本机成功/远端失败、无本机/有远端、两边无目标、网络离线均按第 5 节聚合结果。
8. 发送缓存未终结时不能因 TX_PROGRESS 重新授予相同预算；所有失败路径最终结算。
9. 热话题与冷话题、同 shard 与不同 shard 混合，信用等待和控制处理不被饥饿。
10. tm=0 无任何提交，慢编码不重置期限，发送超时后的晚交付与部分交付标记正确。

**测试：** test_shared_net_reliable、test_shared_net_credit、test_shared_net_fairness、test_shared_net_control_pressure。
**退出条件：** 成功/失败语义、配额和生命周期通过，无永久等待，有低延迟路径的结构证据。

### T11：公共 API 接入与兼容

**依赖：** T10。  
**改动：** shared_pub_sub_ipc、topic_ipc 工厂、模式说明。  
**步骤：**

1. SharedPublisher 实现全部 pub_ipc_base 接口。
2. SharedSubscriber 包装 shm_sub_ipc 并执行两阶段注册。
3. 只在 DZIPC_NET_BACKEND=shared_v1 且前置满足时选择新后端。
4. 缺网关、错误 locality、无 MPMC、协议不匹配都应明确失败。
5. 测 publish_prebuilt_segment 编码失败返回 false，回退 publish 后接收一次。
6. 分别测本机已提交、仅出站已接管、任一腿未知时，prebuilt 不返回可回退的 false。
7. reset_message/InitChannel 先封住两腿新调用，等待/取消旧事务并注销旧登记，再建立新描述；
   不原地修改共享路由键。失败后整个句柄保持不可用，不能留下本机新类型、网络旧类型的混合状态。
8. 覆盖 GenericMessage、生成 C++ 消息和 Python 预构造段。
9. 验证 IPC_SOCKET_ONLY、IPC_SHM、服务请求响应仍选择旧路径。
10. 未完成的新配置不能被默认开启。

**测试：** test_shared_net_public_api、test_shared_net_prebuilt、test_shared_net_compatibility。  
**退出条件：** API 行为矩阵完整，默认旧回归通过，二进制兼容约束检查完成。

### T12：故障、关闭与重启

**依赖：** T11。  
**步骤：**

1. 每个有向生命周期边界放测试注入点：收到最后一片前、SHM commit 前后、ACK 前后。
2. SIGKILL 网关，验证可靠返回 GatewayLost/部分交付，已初始化本机 MPMC 继续收发、旧 Sample 仍合法。
3. SIGSTOP 网关，验证不出现第二网关接管。
4. 重启网关，新 epoch 使旧记录/旧网络包无效。
5. 新建后端恢复网络；旧对象仅本机端可用，旧网络身份/信用不能迁移或重放。
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
5. 用户说明覆盖本机直达、网络源端额外一跳、部分提交、信用等待、ACK 边界和跨模式不互通。
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

1. 对照第 5 节 I01～I21 和第 19 节逐项给证据。
2. 检查最终 diff，只包含本任务范围。
3. 检查默认配置未变、实验模式未提前升级为默认。
4. 检查文档示例能在安装目录执行，所有路径、目标、选项真实存在。
5. 审查还有没有每话题 UDPNode / send socket / recv thread 泄漏到 shared_v1。
6. 验证旧模式下不意外连接网关或初始化新网络线程。
7. 先准备好交付摘要：改动、验证、性能、明确限制、回滚，待测试与审查完成后回报。
8. 全部工作完成后的第一个动作按 AGENTS.md 调用 member_report_result；不可用则说明事实，不伪造回报。
9. 回报后按团队约定处理上下文压缩；没有可调用能力时不宣称已执行 /compact。

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
| V18 | DZLC v2 / DZTX v2 golden bytes | 40B/112B 布局、请求 ID/deadline、信用字段端序正确 |
| V19 | 旧 DZLC/DZTX、保留的 15/16 消息 | 明确版本/类型错误，不误接纳旧 BEGIN 或 96B 记录 |

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
| R14 | 本机与网络同时发布 | 应用本机提交一次；源网关对出站记录的本机注入次数为 0 |
| R15 | 已初始化后 SIGSTOP/SIGKILL 网关 | 本机 best-effort 仍可收发；网络失败可见，不回退 legacy |
| R16 | 信用充足时单条 reliable | 无 SEND_BEGIN/BEGIN_READY 控制往返，等待者先于记录可见 |
| R17 | 多 shard 并发入站 | 各 RouteKey 在其 shard 提交，无全局串行提交队列 |
| R18 | 多会话未用授予加网络在途缓存 | 总预算不超限；不能重复借用已授予而未消费的信用 |

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
| F23 | 本机成功而网络 loan/信用/会话失败 | prebuilt 返回 true，不回退重发；blocking 失败并标明部分交付 |
| F24 | 本机目标提交失败、网络提交成功 | 本机不重试；网络可交付；blocking 不伪装全部成功 |
| F25 | 任一腿 Indeterminate | prebuilt 不允许回退，诊断未知；同一腿不再次提交 |
| F26 | 重复 GRANT、TX_PROGRESS 早于网络完成 | 两类信用分别守恒，不能超额授予或提前复用缓存预算 |
| F27 | 零初始信用、首次大消息、等待中断连 | 有界等待/失败；本机提交不被撤销；旧信用不用于新会话 |
| F28 | sendmmsg 部分发送、批次 EAGAIN | 只推进成功前缀，未发后缀身份不变，不等凑批、不忙转 |
| F29 | 重传/NACK 风暴、目录同时更新 | 控制限速有界，目录仍推进；恢复延迟与误重传均记录 |
| F30 | 网络离线后 reset/重新 InitChannel 失败 | 整个重置句柄不可用，不留下本机新类型/网络旧类型混合状态 |

故障测试随机种子必须固定并保留。
正确性用例在合理负载下以“零串话、零错误载荷、零不期望重复”为硬门槛。
best-effort 的故障丢包可以发生，但必须与计数和序号缺口一致。

### 17.4 低延迟验收与性能报告

比较对象必须实际存在：f066a82 的直接 MPMC/legacy hybrid 与本次 shared_v1 实现。
旧“全部经网关”只有文档，不要求为了制造对照而先实现一整套旧网关，也不能给它编造数字。
相同 CPU、网卡、MTU、编译优化、编码、订阅数量和 offered load 下比较；饱和吞吐另列。

| 场景 | 主要判断 |
|---|---|
| 仅本机，1 pub -> 1/8/32 sub | 是否保留直接 MPMC 的短路径，网络提示为零时是否没有出站载荷 |
| 本机+跨机同时活跃 | 本机可读时间是否受网络额度、网关暂停或远端 ACK 阻塞 |
| 跨机 1 pub -> 1 host / 多订阅进程 | 单份网络重组/注入，批量 IO 的系统调用成本 |
| 跨机 1 pub -> 2/8 host | 单播放大、发送预算和尾延迟 |
| 1000 个低频话题 | socket、FD、RSS、SHM、线程和空闲 CPU |
| 大热话题与小冷话题，同 shard / 不同 shard | 注入/重传是否形成新的排队瓶颈 |
| 同话题 8 发布者 | MPMC 竞争、本机提交和网络排队分别计时 |
| 冷启动、零信用、首次大消息 | 信用补充和发现等待的成本，不能只报预热后的最优路径 |
| 丢片与 ACK 丢失 | 默认短定时器的恢复 p99、误重传次数及额外带宽 |

每组至少覆盖 64B、4KiB、1MiB 的目标消息档位，报告实际 wire blob 字节数；
至少 3 次独立运行，预热后测量窗口不短于 30 秒，冷启动单独采样。
有界编码器下同时记录以下时间，不能只测 publish 返回：

1. 调用开始到本机订阅者可读取完整消息。
2. 调用开始到 best-effort 返回，以及到 reliable 返回。
3. 编码、本机提交、信用等待、出站排队、首发、远端重组/提交及 ACK 等待。
4. 实际交付数、丢失/重复、超时与晚交付、CPU、上下文切换、复制字节、FD/RSS/SHM 和网络流量。

本机跨进程时间戳需处于相同时钟域。跨机单向时延只有在 PTP/等价同步及误差界有证据时
才报告；否则报告同一源时钟测得的往返/可靠完成时间，以及各主机内部阶段耗时。
不同主机 CLOCK_MONOTONIC 不能相减。丢失和失败调用必须进入统计，避免只统计幸存快消息。

工程初始门槛：

- I19～I21 和 R14～R18 必须通过，证明短路径真实存在。
- 仅本机同等配置下，对 p50、p99 分别要求：新值-直接 MPMC 基线 <= max(基线×10%, 5 微秒)。
  这是初始回归门槛，5 微秒用于容纳很短路径的绝对测量波动，不是用户实时性 SLA。
- 若没有达到本机回归门槛，保留全部数据并定位编码/复制/线程成本，不能声称低延迟优化完成。
- 本机+网络同时活跃时，网关暂停或网络信用耗尽不得让本机交付等待网络恢复；
  public reliable 返回仍可能等待 deadline，两种时间必须分别呈现。
- 跨机以实测报告收益和退化；无用户部署预算时维持显式启用，不宣称所有部署性能已通过。

nodelet 配置必须一致；已存在的纯进程内 nodelet 路径另作参考，不能关闭它后声称全面超越。
不删除不利轮次；机器异常注明并保留原始结果。若后续目标预算更严格，新增门槛而非改图表掩盖。

### 17.5 公平性基准

建立可重复的初始门槛：

- 热话题：1 MiB，发送速率限制在本机/网络测得可持续吞吐的 70%；
- 冷话题：64B，100 Hz；
- 同时进行 HELLO、订阅变更、信用补充和可靠 ACK；分别把冷热话题放在同 shard 与不同 shard；
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
4. 确认两端网络 v1、本机控制/出站 v2 兼容；应用与网关 MPMC layout/时钟域一致。
5. 先启动订阅端并等待 SUB_READY；创建发布对象后先不发送，按第 16.3 节确认两端
   话题与 peer 状态均已同步；为首个大消息准备足够发送信用，再放行需要避免启动丢包的流量。
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
- DZLC/DZTX v1 仅为旧草案格式，新实现拒绝它；不能混用旧 96B 与新 112B 出站头。
- 网关运行中故障后，本机 MPMC 继续工作是明确的新模式语义，不代表网络已自动恢复。
  需要恢复网络时新建后端会话，并避免业务对不确定旧调用无条件重试。

### 18.3 回滚步骤

1. 停止新模式应用的本机和网络两腿继续提交；仅停网关不足以让本机发布停止。
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
- [ ] DZLC v2 / DZTX v2 的版本、112B 布局、实际 loan 计费与信用边界验证通过。
- [ ] tm=0、晚交付、零 ACK 的不确定结果与订阅加入边界已写入用户说明并验证。
- [ ] 重启、析构、最后订阅者退出和旧 Sample 均验证。
- [ ] 远端入站不会再次出站。
- [ ] TLV 页尾完整保留，DZFlat 不经 TLV 中转。
- [ ] 发现迟到、丢页、撤销、版本更新有效。
- [ ] 诊断显示真实状态，不把内部端点计成业务发布者。
- [ ] topic_cat 和 Python 相关路径有验证。
- [ ] 所有新测试实际执行，测试数非零，skip 明确。
- [ ] 至少两台真实主机的正确性和性能证据齐全。
- [ ] 本机直达与直接 MPMC 的 p50/p99 对照达到约定门槛，单播放大已量化。
- [ ] 源网关不回注应用出站消息，网关故障不关闭既有本机端。
- [ ] 两类信用独立守恒，可靠热路径没有逐消息 BEGIN 往返。
- [ ] 分 shard 注入、批量 IO 部分成功、短定时器与控制预算均有证据。
- [ ] 冷启动/信用不足、两腿部分提交与错误回退都已验证。
- [ ] 回滚演练完成，没有误清其他 Agent / 业务资源。
- [ ] 最终 diff 仅包含本任务改动。
- [ ] 团队回报已完成，或工具不可用事实已说明。

## 20. 执行记录模板

每卡使用独立记录，表格只放结论与证据索引。开始时全部保持“未开始”。

| 任务 | 状态 | 提交/源码指纹 | 构建目录 | 测试数/失败/跳过 | 证据 | 未解决事项 |
|---|---|---|---|---|---|---|
| T00 | 部分完成 | f066a82 + 工作区 | build-shared-net | 新增 3/0/0 | [基线与能力表](shared_network_endpoint_evidence/20261005-foundation/baseline.md) | 规模、延迟、长期压力及真实跨机数据待采集 |
| T01 | 已完成 | f066a82 + 工作区 | build-shared-net / build-shared-net-off | 配置各 10/0/0；旧路径 22/0/0 | [构建与配置记录](shared_network_endpoint_evidence/20261005-foundation/baseline.md) | 非 Linux 仅纯函数分支验证；实际平台构建待验收 |
| T02 | 已完成 | f066a82 + 工作区 | build-shared-net | GTest 16/0/0；Python 向量 1/0/0 | [协议记录](shared_network_endpoint_evidence/20261005-foundation/implementation.md) | 具体类型解码仍由订阅者承担；未验证异端序 DZFlat 平台 |
| T03 | 已完成 | f066a82 + 工作区 | build-shared-net | GTest 12/0/0 | [端点与 IO 记录](shared_network_endpoint_evidence/20261005-foundation/implementation.md) | 路由生命周期与重组接入仍属后续卡；尚无跨机性能结论 |
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

### 20.1 本次完整方案修订记录（不是实现完成记录）

用户要求先提交现状，再完整修订方案，本轮暂不实现共享网关代码。

| 项目 | 本次事实 |
|---|---|
| 优化前提交 | f066a82：MPMC 代码、配套工具/测试、前置交付记录及上一版网络方案，共 31 个文件 |
| 提交前构建 | cmake --build /tmp/cpp_ipc_dds_mpmc_build --parallel 2，退出码 0 |
| 提交前专项 | ctest --test-dir /tmp/cpp_ipc_dds_mpmc_build --output-on-failure --tests-regex 'test_(shm_mpmc\|shm_multi_publisher\|ipc_info_pool)'，8/8，退出码 0 |
| 本轮修改范围 | 仅本文件；未创建或修改共享网关实现 |
| 文档检查 | 22 主章节、16 任务卡、21 不变量、67 验收项的编号、表格、链接与章节引用检查通过；160B/84B/40B/112B 协议布局检查通过 |
| 前置遗留 | MPMC 专项已交付；原完整回归的 8 个旧基线失败及未完成的压力/性能证据保留，不重分类为通过 |
| 团队工具 | 当前会话没有 member_report_result 或 /compact 可调用能力，不能宣称已回报/压缩 |

本次固定的替代决策：

| 旧草案 | 本次方案 |
|---|---|
| 应用消息先到网关，再注入本机 | 应用本机直达；网关对出站记录只发远端 |
| 单接管点 | 两腿独立接管，公开 bool 与 prebuilt 按第 5 节合并 |
| 每条 reliable 先 BEGIN 往返 | 预授予信用，DZTX v2 携带 request/deadline |
| DZLC/DZTX v1、96B 出站头 | DZLC/DZTX v2、112B 出站头；网络协议仍为 v1 |
| 全局本机提交循环 | 每 RouteKey 在所属 shard 完成入站提交 |
| 固定 20 ms 缺片/探测初值 | 默认 2 ms、可配置的短定时器与控制速率预算 |

上述修订结束时 T00～T15 均未开始。后续执行从本修订的契约和任务卡开始，不执行旧草案的网关回注、
逐消息 BEGIN 或全局串行提交方案。本机直达收益、跨机延迟和新故障用例尚无运行结果。

## 21. 执行者遇到问题时的处理规则

1. 找不到文中符号：先用 rg 按类名/函数名定位，再更新文档锚点；不创建同名重复实现。
2. 前置 SHM 接口变化：保留本方案行为契约，写小适配；不强转 route 与 MPMC 类型。
3. 文档与当前实现冲突：当前实现决定基线事实，本方案决定新模式目标；记录实际差异。
4. 底层原子提交不能满足：停在 T05 的边界做专项验证；不通过重试隐藏重复风险。
5. 旧测试失败：在基线上复跑一次分类，已有问题记录；本次引入的问题修复后再推进。
6. 性能没有提升：按阶段定位编码/复制、信用等待、网络排队和单播放大，保留完整对照数据。
7. 没有跨机环境：完成本机和模拟环境测试，并明确真实跨机验收仍未完成。
8. 缺用户部署参数：采用本文默认值制作可评审实现，不改用户实际网卡或全局系统配置。
9. 不确定网络/SHM 可靠语义：按第 5 节做失败处理，不声称端到端应用消费成功。
10. 不把 SO_REUSEPORT、多个 epoll waiter、共享线程池当作主机消息分发协议。
11. 不把端口减少、编译通过或单消息成功当作完整交付。
12. 只有第 19 节必要项通过，才报告本方案完成。

## 22. 明确延期的优化

以下不属于首版执行范围，禁止为了追求功能齐全在主线中顺手加入：

- 本机交付者在应用与网关之间动态切换（首版固定应用直达）；
- 共享业务 payload 的跨进程描述符/额外租约、一次提交同时供本机与网关读取；
- 每进程直接持有网络发送端点、DPDK/AF_XDP、忙轮询与 RTT 自适应；
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
