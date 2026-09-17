# UDP 接收路径与 ser-cli 连接策略改造任务列表

状态：任务拆解完成，待分配执行。创建时间：2026-09-14。
**同步更新 2026-09-14 23:25（文档分析-claude）**：三项决策已拍板并落入本文「三项拍板结果」一节；
T1/T3 已落码，原「T3 必须等待 T2」的排期已过期，依赖栏已修正。本文只记状态与决策，
证据、复现命令与未覆盖项见共享上下文区报告（`t1_udp_borrow_架构评审.md`、
`t3_impl_report_性能测量.md`、`t5_integration_review_架构评审.md`、
`tasklist_closure_recommendations_文档分析.md`）。

## 任务总览

| 任务项 | 负责人 | 依赖关系 | 状态 | 验收标准 | 验证方式 | 交付物 |
|---|---|---|---|---|---|---|
| T0 接口与现状对齐 | 文档分析-claude | 无；阻塞 T1/T2 | ✅ 完成 | 明确 SHM `try_get`/`try_get_clone` 语义、UDP 订阅接收端入口、ser-cli 状态机和可复用接口；列出 file:line 锚点 | 源码审计、现有测试/文档核对 | 共享上下文审计报告 |
| T1 UDP 话题订阅接收端借样 | 架构评审-claude | T0 | ✅ 已落码 | 仅修改话题发布/订阅的 UDP 订阅接收端；`try_get` 走内部借样，`try_get_clone` 保持拷贝；接口/生命周期与 SHM 一致 | 定向单测、编译、现有 socket/SHM 回归；检查拷贝计数或对象身份 | 代码改动、测试结果、变更说明 |
| T2 ser-cli 同主机路径判定设计 | 架构评审-claude + 文档分析-claude | T0；与 T1 可并行 | ✅ 完成 | 定义 UDP 握手判定、同主机→SHM、跨主机→socket、切换时机、是否重连、状态字段与失败回退 | 状态机审查、双端拓扑用例设计 | 设计决策记录与状态转移表 |
| T3 ser-cli 路径切换实现 | 性能测量-claude | T2 **+ R4 ✅**（R4 已修，见下） | ✅ 已落码 | 握手先走 UDP；同主机自动切 SHM，跨主机保持 socket；连接中断清理全部相关状态；下次连接重新判定；切换无重复连接/资源泄漏 | 同主机、跨主机、握手中断、已建立连接断开、重连循环测试 | 代码改动、测试日志 |
| T4 测试矩阵与集成验证 | 测试验证-claude | T1、T3 | 🚧 进行中 | 覆盖零拷贝/拷贝语义、同/跨主机选择、中断清理、重连重判、兼容性和回归；所有验收项可复现 | CTest/专用测试、必要时故障注入；记录命令与结果 | 测试矩阵、验证报告 |
| T5 Leader 集成审查 | codex-leader（审查执行：架构评审-claude） | T1、T3、T4 | 🟡 审查已完成，遗留 3 条待办 | 审查 diff、接口一致性、线程安全、状态清理和文档同步；确认无超范围改动 | 构建、测试、静态检查、人工 diff 审查 | 集成结论与后续建议 |

## 三项拍板结果（2026-09-14）

### 拍板 1 · UDP `try_get` 仍有一次去帧拷贝 —— 确认并收口，不消除

**决策**：UDP 借样路径上那**一次整段去帧拷贝是长期契约**，不投入消除；文档与收益论证必须按此写。

- **借样在 UDP 上省掉的不是拷贝**，而是"把 DZFlat 段当 TLV 反序列化 + 逐字段读"
  （借样分支不调用 `msg->deserialize`，`src/dzIPC/common/data_rev.cc:1672-1691`、`:1917-1931`）。
- **为什么消不掉（两条独立原因，缺一都不足以定论）**：
  1. **所有权**：`src/libipc/platform/posix/udp.h` 的 `receive_nowait()`（`:268-278`）与
     `receive()`（`:336-340`）返回
     `ipc::buffer(temp_buffer.data(), received, nullptr)` —— **非拥有视图 + 析构器 `nullptr` +
     指向复用的成员缓冲**（`temp_buffer` 是 `:25` 声明的成员）。`Sample` 引用它，下一次
     `recvfrom` 即改写 ⇒ 悬空。要让视图安全，
     必须把 libipc 接收路径改成拥有/引用计数缓冲，**影响全部 UDP 消费者**（pub/sub 数据、
     ACK 通道、ser-cli 请求/响应/ACK/握手）。
  2. **布局**：`de_frame_dzflat()`（`data_rev.cc:1585-1615`）页数公式 `seg_len / (1460−12) + 1`
     （`:1597`，与发送侧 `correct_total_size` 同式），去帧循环 `read += n + TAIL_SIZE`（`:1611`）。
     **`seg_len > 1460` 时 12B 页尾插在段中间** ⇒ 载荷在 wire 上不连续，引用也拼不出一个段；
     只有 `seg_len ≤ 1460` 才连续。现实中的 DZFlat 段常是多页（T1 用例即 40 页 / 57.6KB）。
- **消除它只有两条路**：重构 libipc 共享接收路径，或改 wire —— 后者与 `docs/dzflat_shm.md:438`
  「不动 socket/UDP wire」的既有裁决正面相撞。
- **契约落地位置**：`include/dzIPC/pub_sub_base.h:60-68`（"SHM 上是真零拷贝；UDP 上恒有一次
  整段拷贝"）、`include/dzIPC/common/data_rev.h`（`chunk_rev_topic(..., ipc::buffer* out_payload)`
  重载与 out_payload 契约）。
- **使用约束**：⛔ **不得**用 socket 借样论证 S2/S3 的零拷贝收益 —— 本机那一跳的真零拷贝
  仍只能来自 DZFlat over SHM。

> **2026-09-17 兑现细分**："UDP 恒有一次整段拷贝" 指的是 `de_frame_dzflat` 把去帧后的段落成
> 独立连续块那一次（分帧格式的固有成本）。在此之上**还曾多拷一次** —— 接收循环对 schema-less
> 话题走 `AcceptWire` → `GenericMessage::dzflat_read()` 又把段整段拷进 `dzflat_seg_`。现两者
> 都已改用 `dzflat_adopt`（与 SHM 腿同构），UDP 上只剩前者；`test_socket_borrow.cpp` 的
> `GenericMessageTopicBorrowsTheSegmentOverUdp` 钉住 `dzflat_is_borrowed()`。
- **附加事实**（2026-09-17 更新）：常规 UDP 发送仍恒 `msg->serialize()` = TLV（`publish` /
  `publish_best_effort` / `publish_for_sniffer`），但已新增**显式入口**
  `socket_pub_ipc::publish_prebuilt_segment()`（`src/dzIPC/socket_pub_sub_ipc.cc`）与它在
  SHM 上的同构实现，只接受调用方按 schema 写好的平坦段；Python 的 `dzipc.publish_dzflat()`
  就是它的第一个生产者（见 `docs/dzflat_shm.md` §9.7）。
  ⇒ T1 的准确表述是**能力就绪、且已有选择性的生产者**，但**默认路径仍是 TLV**（无 schema 的
  类型、开关未开、无接收方、nodelet 拓扑、池耗尽都会回退）；视图队列在现网仍近乎为空。

### 拍板 2 · `IPC_AUTO` 传给 pub/sub：显式拒绝，禁止静默降级

**决策**：`IPC_AUTO` 只对 ser/cli（`ServerIPCPtrMake`/`ClientIPCPtrMake`）有实现；传给
**pub/sub**（`PublisherIPCPtrMake`/`SubscriberIPCPtrMake`）必须**显式失败**，不得静默退化成 socket。

- **现状（2026-09-14 复核）**：pub/sub 的构造分支只有 `== Shm` / `== Socket` 两条，
  `Auto` 落入 `throw std::invalid_argument("Unsupported IPC type")`
  （`src/dzIPC/topic_ipc.cc:91` 发布者、`:177` 订阅者）⇒ **拒绝已经成立**，且在
  `RegisterIpcInstance` 之前抛出，不产生半成品实例。
- **方向正确，待办是把它从"恰好抛异常"变成"有测试钉住的契约"**：补一条用例断言
  `IPC_AUTO` 传给两个 pub/sub 工厂**抛出**；同时断言 ser-cli 的 `IPC_AUTO` 行为不变。
- ⚠️ **一处订正（本文档上一轮报告的结论有误）**：`IPC_AUTO` 映射的是 `IPCType::Auto`
  （`include/dzIPC/dzipc.h:19`）而非 `IPCType::Socket`，因此走的是 `else throw` 分支，
  **不是**"静默退化成 socket"。请不要按旧结论去改类型映射。

### 拍板 3 · Auto 模式的日志必须记"当下实际传输"

**决策**：日志的 `TransportKind` 读**当下实际传输**（Auto→Shm / Auto→Socket），
**不得**读构造期 `IPCType`。

- **实现位置**：`src/dzIPC/server_ipc.cc` 新增 `live_transport_kind(leg, ipc_type)`
  （`:13-33`，函数体自 `:21` 起）与取值器 `TransportGetter`（`:38`）—— 回调在**建腿之前**包好，那一刻只拿得到
  构造期类型，所以传"怎么取值"而不是"值"。四处记录点全部改读活值：服务端回调 `:120`、
  `reset_callback` `:163`、客户端 `send_request` 的请求/响应 `:244`/`:260`。
  取不到活值（腿未建/正在拆）时回落构造期映射，与改动前逐字节一致。
- **剩余射程**：`src/dzIPC/topic_ipc.cc:19-20`/`:46-47` 的 `log_publish_event` /
  `log_subscribe_event` 仍是 `ipc_type == IPCType::Shm ? kShm : kSocket`。因拍板 2 已使
  `Auto` 到不了 pub/sub，**当前不构成缺陷**；但若将来 pub/sub 接受 `Auto`，这两处必须同批改
  （登记为约束，勿遗忘）。
- ⛔ **本条尚未收敛为可验收事实**（截至 23:25 仍在途，见下）：
  - 判据用例 `DzipcLog.AutoPathLogsTheLiveTransport`（`test/test_dzipc_log.cpp`）读 bag 里
    `TransportPacket` 的 **transport 字节**（0=kShm/1=kSocket）而不是日志文本，方向正确；
  - 但**实测不稳定**：同一二进制 12 轮 **2 次失败**、15 轮复测**第 2 轮失败**，出现两种形态
    ①"解析出 0 条 TransportPacket"、②"解析出的记录 `transport=1`(socket) 而断言期望 0(shm)"。
    形态②更值得查：`switched` 是"两侧都到位"的**快照**，而 RPC 与日志记录发生在**更晚**时刻，
    若其间发生一次路径回退或腿处于 Auto 过渡态，日志如实记 socket 而用例判它失败 ⇒
    很可能是**判据口径**问题而非记录缺陷；但也不排除是真实回退，需实现者定性。
  - 该用例与 `server_ipc.cc` 在本次文档同步期间由 **架构评审-claude 并发编辑**
    （文件 mtime 23:19:48 / 23:22:24），故上述比例只代表**在途状态**，不可作为验收依据。
  - 建议：先断言 `fallback` 计数为 0 并钉住"记录发生时刻"的传输，再做一次**回滚式变异验证**
    （把 `server_ipc.cc` 回滚后重跑，证明改前确实记 socket）——T5 报告 §1 提出的是同一件事。

## 依赖与执行顺序

1. 先完成 T0，冻结 `try_get` 语义、UDP 接收端范围和 ser-cli 状态机边界。✅ 已完成。
2. T1 与 T2 可在 T0 后并行。✅ 均已完成。
3. ~~T3 必须等待 T2 的状态转移设计~~ → **已过期**：T3 已落码（`include/dzIPC/auto_ser_cli_ipc.h`、
   `src/dzIPC/auto_ser_cli_ipc.cc`、`test/test_sercli_auto_path.cpp`）。T3 的实际前置是
   **T2 + R4**，R4 已修：`connect_with_retry(node, running, …)`（`src/dzIPC/socket_ser_cli_ipc.cc:35-53`，
   检查点 `:44` 与 `:51`），四处连接重试全部替换（`:214/:218/:639/:643`）+ 握手两处（`:322/:712`）。
4. T4 等待 T1/T3 的实现（现已具备）；T5 在全部测试结果齐备后执行。

## 统一约束

- T1 不修改 ser-cli；T3 不改变话题订阅端的借样语义。
- 复用已有 SHM 接口和状态清理模式，不新造平行语义；发现不一致先记录并回到 T0 对齐。
- 路径切换必须可观测：记录当前 transport、判定结果、回退原因和清理完成状态。
- 不以日志"看起来切换成功"作为唯一证据，必须有消息收发、资源和状态断言。
- **UDP 视图路径恒有一次去帧拷贝**（拍板 1）：任何"零拷贝收益"的写法都必须写明它省的是
  反序列化而不是拷贝。
- **`IPC_AUTO` 的适用面**（拍板 2）：只对 ser/cli 有效；pub/sub 必须显式失败。
- **Auto 日志口径**（拍板 3）：记当下实际传输；`topic_ipc.cc` 两处三元式在 pub/sub 接受
  `Auto` 之前无需改，但需在上述前提变化时同批修改。
- **未提交边界**：T1/T3 共 16 文件（+1014/−173）同处未提交态，git 无法切分归属
  ⇒ T5 落结论前应先落提交边界。

## 环境提示（影响复现，非本任务缺陷）

- 内核 `net.core.rmem_max` / `wmem_max` 未放开（实测 212992），用例启动时会打印
  `UDP socket buffer truncated by the kernel` 警告；大消息多分片场景会丢片。
  复现长跑用例前按该警告给出的 sysctl 调整。
- 多成员共用 `build/` 目录：并发构建期间可能出现瞬时编译错误、以及**二进制与 `libipc.so`
  短暂不一致**（实测：一条用例在旧库上失败、库重建后连续 5 轮通过）。判断失败前先确认
  `find src include -newer build/lib/libipc.so` 为空。
