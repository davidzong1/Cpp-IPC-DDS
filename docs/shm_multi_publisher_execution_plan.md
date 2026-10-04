# SHM 话题多发布者支持执行方案

编制日期：2026-10-04  
适用基线：当前分支 `e887d5e` 及其工作区源码  
状态：P0-P4 完成；P5 专项完成，完整回归保留已知基线失败

## 1. 结论与目标

当前 SHM 话题通道是 **单发布者、多订阅者**：`shm_pub_ipc` 固定创建
`ipc::route`，其类型为 `chan<relat::single, relat::multi, trans::broadcast>`。
底层 `sender_checker` 用共享 `atomic_flag` 只允许一个 sender 占用同一段。

本方案的目标是让同一 `(topic_name, domain_id)` 支持多个独立发布进程或发布句柄，
并保持现有订阅语义、DZFlat 借样、TLV 回退、订阅者心跳回收和跨进程生命周期安全。

目标交付后应满足：

- 多个发布者加入同一话题时复用同一代 SHM 通道，不互相执行清段或重建。
- 一个发布者退出时，其他发布者和订阅者继续工作。
- 最后一个发布者退出时，通道按租约和在途样本规则收尾。
- 发布者崩溃后可被发现、回收并允许存活发布者接管协调职责。
- 单发布者、多订阅者现有路径继续可用，旧版本不会静默接入新布局。
- 明确跨发布者顺序语义：只保证每个发布者自身的提交顺序，不保证多个发布者之间的全局顺序。

非目标：本方案不引入 DDS ownership、持久化历史、跨主机 SHM，也不改变 `IPC_SOCKET_ONLY`
的 UDP 多发布源语义。

## 2. 现状证据与约束

### 2.1 传输层

`ipc::route` 的公共定义明确写明“一生产者到多消费者”，而 `ipc::channel` 才是多生产者
版本：【[include/libipc/ipc.h:408](/home/zwc/cpp_ipc_dds/include/libipc/ipc.h:408)】。

`shm_pub_ipc::InitChannel()` 当前无条件执行：

1. `control_plane_.begin_rebuild()`；
2. `ipc::route::clear_storage(topic_name_)`；
3. 新建 `ipc::route(..., ipc::sender, ...)`；
4. `control_plane_.set_ready()`。

见【[src/dzIPC/shm_pub_sub_ipc.cc:870](/home/zwc/cpp_ipc_dds/src/dzIPC/shm_pub_sub_ipc.cc:870)】。
因此第二个发布者不是加入已有通道，而是可能触发清理、generation 递增和订阅者重连。

单发布者策略的占用检查在【[src/libipc/circ/elem_array.h:61](/home/zwc/cpp_ipc_dds/src/libipc/circ/elem_array.h:61)】；
发送前 `ready_sending()` 失败会直接返回 false，见
【[src/libipc/ipc.cpp:1245](/home/zwc/cpp_ipc_dds/src/libipc/ipc.cpp:1245)】。

### 2.2 控制面

当前 `TopicControl` 有一个 `owner_pid`、一个 `peer_count` 和订阅者 `PeerSlot` 表，
没有发布者 slot、发布者计数或发布者身份表：【[include/dzIPC/common/control_plane.h:62](/home/zwc/cpp_ipc_dds/include/dzIPC/common/control_plane.h:62)】。

`begin_rebuild()` 会将控制面置为 Clearing，清零 peer 记账、清除 peer slots 并递增
generation：【[src/dzIPC/common/control_plane.cc:98](/home/zwc/cpp_ipc_dds/src/dzIPC/common/control_plane.cc:98)】。
这套动作适合故障重建，不适合普通发布者加入。

### 2.3 本地快速路径与 payload 池

`LocalPubSubRegistry` 只注册订阅队列，发布端根据订阅快照做进程内扇出，不能作为多发布者
协调机制：【[include/dzIPC/common/local_pub_sub_registry.h:57](/home/zwc/cpp_ipc_dds/include/dzIPC/common/local_pub_sub_registry.h:57)】。

同话题多个发布句柄共享 DzFlat payload pool，不等于它们可以同时写入同一个 SHM route。
payload 池租约必须与通道租约分开管理。

### 2.4 必须保留的旧语义

- `publish()` / `publish_best_effort()` 的 best-effort 返回语义。
- `publish_blocking()` 的单次发送等待语义。
- DZFlat loan 失败后的 TLV/分片回退。
- 订阅者最多 32 个连接位的底层限制，以及控制面 64 个 peer slot 的现有边界。
- route 重建时先摘除接收 worker、再释放旧 route 的生命周期顺序。
- 旧单发布者部署可通过回滚开关继续运行。

## 3. 目标架构

### 3.1 新增版本化 MPMC 通道

不直接改变全局 `ipc::route` 的别名。新增明确的多生产者广播通道，例如：

```cpp
using mpmc_channel = chan<relat::multi, relat::multi, trans::broadcast>;
```

SHM pub/sub 的发布端和订阅端统一使用该类型。服务、sniffer、现有 route 测试继续使用
`ipc::route`，降低 ABI 和行为回归范围。

由于 `relat::multi` 的共享内存布局与 `relat::single` 不同，MPMC 通道必须使用新的段名
版本（例如 `...__SHM_MPMC_V2`），不能与旧 route 段混挂。控制面 magic/layout/version
必须同时升级，旧端点发现到不兼容布局时要显式失败或回退，不得静默读错。

### 3.2 发布者注册与协调者

在 `TopicControl` 中增加发布者注册表，至少包含：

| 字段 | 作用 |
|---|---|
| `publisher_id` | 发布者实例身份，进程重启后必须变化 |
| `pid`、`start_token` | 判定进程是否仍是原实例，防 PID 复用 |
| `generation` | 发布者加入时看到的通道代次 |
| `heartbeat_ns` | 发布者存活心跳 |
| `in_use` | slot CAS 占用标志 |
| `coordinator` | 当前负责初始化、回收和接管的发布者标记 |

控制面保留一个协调者 lease，但协调者不是唯一发布者。普通发布者加入时：

1. 打开既有控制面并校验 magic、布局版本和消息类型。
2. 获取 publisher slot。
3. 若已有 Ready generation，直接加入，不清理数据段、不递增 generation。
4. 若话题尚未建立，竞争协调者 lease，由胜者创建通道并发布 Ready。
5. 周期刷新自己的 publisher slot heartbeat。

只有以下情况允许 generation 重建：首次创建、最后一个发布者离开后的重新创建、协调者确认
死亡后的接管、显式版本迁移或不可恢复的通道损坏。普通第二发布者加入不得触发重建。

### 3.3 通道租约与清理规则

`clear_storage()` 不再位于普通 `InitChannel()` 路径。清理需要同时满足：

- publisher registry 无活跃发布者；
- subscriber peer slots 无活跃订阅者；
- 共享 payload pool 无活跃 lease、未发布 loan 或延迟释放 Sample；
- 没有正在执行的 route receive lease；
- 清理者持有跨进程 catalog/协调锁，并确认 generation 未发生变化。

清理失败时保留段，记录原因，等待下一次协调者扫描；不能为了“重新建链”直接删除活跃段。

### 3.4 发布和接收数据语义

MPMC 环只负责安全地承载多个写入者；产品层定义如下：

- 同一发布者的成功提交顺序保持不变。
- 不同发布者之间没有全局顺序保证，订阅者可以按实际提交顺序观察交错消息。
- 每条消息最多交付一次；慢订阅者继续按现有 best-effort/覆盖策略处理。
- `publish_blocking()` 只对当前调用提供等待，不承诺等待其他发布者的消息。
- 如需诊断排序，在控制面或消息 envelope 中加入 `publisher_id + publisher_sequence`，
  不能复用现有 `msg_id` 类型标识。

### 3.5 发布端对象和 loan API

`shm_pub_ipc` 当前成员是 `std::shared_ptr<ipc::route>`，而
`LoanedMessage` 也持有 route 类型。实施 MPMC 时应提供通道抽象或模板化 loan 持有者，
避免把 `ipc::route` 强转为 MPMC 类型。建议：

- 新增 `ipc::mpmc_channel` 的同构 API；
- 将 SHM 专用 `LoanedMessage` 改为对通道类型参数化；
- 保留旧 route 的 loan API，服务和其他调用方不变；
- `shm_pub_ipc` 的公开类名和方法签名保持不变，内部按 feature flag 选择通道后端。

## 4. 分阶段执行

### P0：冻结契约和基线

交付：

- 固化当前单发布者行为测试：一个发布者、多个订阅者、断开重连、generation 重建、
  DZFlat A/B、TLV 回退、sniffer。
- 添加显式配置：`DZIPC_SHM_MPMC=0|1`，默认 `0`。
- 增加通道版本和控制面版本常量，记录到诊断输出与 info pool。
- 明确旧端点遇到 MPMC 段时返回“不兼容”，不得继续打开。

退出条件：现有 SHM 回归全绿；关闭 MPMC 开关时二进制行为无变化。

### P1：实现并验证 MPMC 基础通道

交付：

- 实现 `mpmc_channel`，包括多生产者安全 push、广播接收、loan、publish、discard、
  wait token 和 receiver connection 位图。
- 为 MPMC 队列使用新的共享段布局和段名版本。
- 明确 `force_push` 覆盖慢订阅者时的 chunk 归还规则，沿用现有 published/route_tag
  保护，禁止重复入池。
- 添加纯 libipc 单元测试，不经过控制面。

退出条件：2/8/32 个 writer 与 1/2/32 个 reader 的小消息、分片消息、DZFlat loan、
慢 reader 和 writer 并发测试无数据竞争、无重复归还、无 payload 损坏。

### P2：发布者控制面与租约

交付：

- 在 `TopicControl` 增加 publisher registry、publisher heartbeat、slot 回收和 PID
  start token 校验。
- 实现首次创建协调、普通加入、协调者死亡接管、最后发布者退出和清理判定。
- 让第二发布者加入保持 generation 不变。
- 将控制面动作从 `shm_pub_ipc::InitChannel()` 中拆成 `join_topic()` 和
  `rebuild_topic()` 两条明确路径。

退出条件：

- 并发首次创建只有一个协调者，所有成功加入者看到同一 generation；
- 第二发布者加入不调用 `clear_storage()`；
- 非最后一个发布者退出不改变 Ready 状态；
- 最后一个发布者退出后，订阅者收到停止/重建通知且段不会过早 unlink；
- 协调者 kill 后，存活发布者可在规定超时内接管。

### P3：接入 `shm_pub_ipc` / `shm_sub_ipc`

交付：

- 发布端切换为 MPMC 通道；所有发布者共享同一 route generation。
- 订阅端创建对应 MPMC receiver，保持 worker pool、RouteSession、lease 和析构顺序。
- 删除普通 join 路径的 `clear_storage()`；清理只由控制面协调者执行。
- 发布者 heartbeat 与 subscriber heartbeat 分开记账，不能用 `peer_count` 代替 publisher count。
- 失败时返回明确状态或记录明确诊断：版本不兼容、publisher slot 满、协调锁超时、
  sender/receiver slot 满、payload pool exhausted。

退出条件：同一 topic/domain 下两个独立进程可以同时发布，双方订阅者都能收到两方消息；
其中一方退出不影响另一方继续发布。

### P4：兼容、迁移和观测

交付：

- `IpcInfoPool` 增加 transport mode、layout version、publisher count、coordinator
  slot 和 generation 展示字段。由于 `extra` 物理字段只有 64 字节（含结尾 NUL），
  采用 slot 而不是可能达到 64 位最大值的 publisher id，保证核心字段不会被截断。
  展示格式中的 `coord` 值就是 coordinator slot。
- `dzlist` / `topic_cat` / sniffer 识别 MPMC 版本，禁止把旧 route 当成 MPMC route。
- 为旧单发布者部署提供两种策略：
  - 继续使用 `DZIPC_SHM_MPMC=0`；
  - 新旧端点完全隔离的版本化 topic 段。
- 需要混合部署时，使用 UDP/Hybrid bridge 做过渡，不让旧 route 与新 MPMC 段共享名字。

退出条件：版本不匹配可诊断、可回退；升级和降级不会删除仍被旧端点持有的段。

### P5：压力、故障和完整回归

交付：

- 执行单发布者回归、多发布者低负载、饱和负载、慢订阅者、重建、崩溃、PID 复用、
  chunk 池耗尽、跨进程并发初始化矩阵。
- 运行 sanitizer/TSAN 能覆盖的进程内测试；跨进程场景使用 fork/独立进程 harness。
- 输出每个 publisher 的发送成功/失败、publisher slot 状态、generation、接收唯一序号、
  伪影/拒绝/淘汰/应用出队和 chunk 回收计数。

## 5. 测试与验收矩阵

| 编号 | 场景 | 必须验证 |
|---|---|---|
| A1 | 单发布者 + 多订阅者 | 现有行为、顺序、DZFlat/TLV 全部不回归 |
| A2 | 同进程两个发布者 | 不清段；两个发布者均可发送；订阅者收到两方 |
| A3 | 跨进程两个发布者 | 同一 generation；无 sender flag 失败；退出一方后另一方继续 |
| A4 | 8/32 个发布者 | publisher slot 上限明确；满时显式拒绝；释放后可恢复 |
| A5 | 并发首次创建 | 只有一个协调者；不会出现双重清理或双 generation |
| A6 | 协调者崩溃 | 存活发布者接管；订阅者不永久停收 |
| A7 | 非协调者崩溃 | slot 被回收；其他发布者无须重建即可继续 |
| A8 | 最后发布者退出 | Ready/Stopping 迁移正确；池和段按租约延迟清理 |
| A9 | 慢订阅者和覆盖 | 不误踢活订阅者；chunk 只归还一次；不破坏邻路 |
| A10 | DZFlat pool 耗尽 | loan 显式失败；普通发送按既有规则回退；计数分类正确 |
| A11 | 旧端点与新端点混用 | 版本不兼容显式失败或走 UDP 过渡，不静默读写 |
| A12 | 发布者退出后持有 Sample/loan | route 可释放但 payload 延迟归还，最后引用释放后池恢复 |

建议新增或恢复以下测试目标：

- `test_shm_mpmc_channel`
- `test_shm_multi_publisher`
- `test_shm_mpmc_control_plane`
- `test_shm_mpmc_recovery`
- `test_shm_mpmc_compatibility`

其中控制面测试必须进入 CTest；压力工装可只作为构建目标，但每次发布候选版本必须保存
构建指纹、IPC 段清理结果和完整日志。

## 6. 验收门槛

功能门槛：

- A1–A12 全部通过；失败不能用超时放宽、补发唤醒消息或过滤异常样本掩盖。
- 低负载下计划发送数、接收唯一消息数和应用有效交付数一致。
- 多发布者之间允许交错，但每个发布者自己的序列不能倒退、重复或缺失。
- publisher registry、subscriber registry 和连接位图的计数最终归零或保留明确活跃租约。

安全门槛：

- TSAN/ASAN 目标测试无新的数据竞争、UAF、double release 或共享段越界。
- 任何 `clear_storage()` 都有明确的租约检查和跨进程协调锁。
- 协调者、发布者和订阅者析构顺序不能让 worker 或控制回调触碰已析构对象。

性能门槛：

- 单发布者模式相对当前基线吞吐和 p99 退化不超过 5%，否则保持旧模式为默认。
- 两个及以上发布者在低负载下无明显额外重建；publisher heartbeat 不引入随发布者数
  无界增长的控制线程。
- 压力下出现 pool exhaustion 时必须可观察，不能静默改变交付语义。

## 7. 回滚与发布策略

1. 第一阶段默认 `DZIPC_SHM_MPMC=0`，只在专用测试 topic 启用 MPMC。
2. 发现数据损坏、段清理错误、协调者接管失败或单发布者退化超过门槛时，关闭开关即可
   回到旧 `ipc::route`；旧段名和新 MPMC 段名不同，不需要在线改段。
3. 任何版本切换前先停止同一 topic 的旧发布者和订阅者，或把新旧版本放到不同 topic/domain；
   不允许两种布局共用一套 SHM 段名。
4. 回滚后保留 MPMC 控制段和诊断日志，待确认无活跃 lease 后再清理；不能直接 `rm /dev/shm`。
5. 发布候选版本必须附带：源码 commit、库 SHA-256、测试二进制 SHA-256、配置快照、
   CTest 结果和残留段检查结果。

## 8. 风险与待决事项

| 风险 | 影响 | 处理 |
|---|---|---|
| 直接把 `ipc::route` 改成 MPMC | 全局 ABI/layout 与旧服务回归 | 新增版本化 MPMC 类型，不改旧别名 |
| 多发布者全局顺序未定义 | 上层可能错误假设单调序列 | 文档和测试只承诺 per-publisher order |
| 协调者清理过早 | UAF、丢消息、旧段被 unlink | publisher/subscriber/pool/route 四类租约共同判定 |
| publisher slot 泄漏 | 新发布者无法加入 | 心跳、PID start token、显式 reaping 和恢复测试 |
| MPMC force_push 与 chunk 回收竞态 | 重复入池或池永久耗尽 | 复用 published、route_tag 和一次性归还判据 |
| 旧端点混挂新段 | 静默丢包或崩溃 | 段名、magic、layout version 三重隔离 |
| 每发布者增加控制线程 | 规模下降 | 使用现有进程级 `ShmControlScheduler`，slot 是数据而非线程 |
| 多发布者带来池容量竞争 | loan 更频繁回退 | publisher 数、在途 chunk、慢消费者纳入容量压测 |

实施前需要产品明确两项选择：

- 是否接受“跨发布者无全局顺序”，还是需要额外的全局序列分配器；
- MPMC 模式是否最终成为 `IPC_SHM` 默认，还是长期保留显式开关。

## 9. 执行记录模板

每个阶段完成后追加一行，不以“代码已合并”替代验收：

| 阶段 | commit | 构建指纹 | 测试/压力结果 | 未解决问题 | 状态 |
|---|---|---|---|---|---|
| P0 | 工作区变更（未提交） | 保留旧 route 默认路径；新增纯解析的 `DZIPC_SHM_MPMC` 约定（只有精确值 `1` 才表示启用），并加入 MPMC 数据段/控制面命名函数 | `test_shm_mpmc_config`、`test_channel_scope` 的命名隔离断言通过；默认运行时仍未启用开关，避免暴露未完成的数据面 | 尚未把配置接入 `shm_pub_ipc`；info pool/工具尚未展示 transport/layout 版本 | 基线与隔离基础完成 |
| P1 | 工作区变更（未提交） | 确认现有 `wr<multi,multi,broadcast>` 已提供 MPMC 实现；新增明确的 `ipc::mpmc_channel` 名称、真实共享段测试、类型化 RouteSession/LoanedMessage | 两个 writer + 一个 reader、两个 writer + 两个 broadcast reader、8/32 writer 保持各自序列、256 槽满时明确失败且释放后复用、延迟 reader、真实 SHM loan/publish/discard、MPMC lease 和 DZFlat 视图均通过；`test_shm_mpmc_channel`、`test_shm_mpmc_types` 通过 | 未覆盖慢 reader 在持续覆盖下的长时压力和完整跨进程故障矩阵；这些属于 P5 扩展项 | 基础通道与类型适配完成 |
| P2 | 工作区变更（未提交） | 新增版本化 `PublisherRegistry`（magic/layout、publisher slot、PID/start token、heartbeat、coordinator lease、stale reap），并注册 `test_shm_mpmc_control_plane` | 共享 generation、同身份幂等加入、身份冲突拒绝、并发首次加入、coordinator lease 到期接管、stale slot 回收与复用均通过；`ctest -R 'test_shm_mpmc_(channel|control_plane)'` 通过 | 尚未接入 `shm_pub_ipc`；普通 join 仍会走旧 `begin_rebuild/clear_storage`；尚未实现跨进程 start token 读取和最后发布者清理策略 | 控制面基础完成 |
| P3 | 工作区变更（未提交） | `shm_pub_ipc`/`shm_sub_ipc` 按 `DZIPC_SHM_MPMC=1` 选择版本化 MPMC 数据段；发布者注册表接入首次创建、普通加入、心跳、stale 回收、协调者租约和最后发布者退出；保留旧 route 默认路径 | `test_shm_multi_publisher` 4/4 通过：同进程双发布者、跨进程加入与 survivor 继续发布、协调者 `SIGKILL` 后 survivor 接管、高层 DZFlat 段往返；`test_shm_mpmc_channel`、`test_shm_mpmc_types`、`test_shm_mpmc_control_plane`、`test_dzflat_transport`（默认旧路径）通过；完整重编后旧 `RouteSession` 编译回归通过 | 旧 worker/I5、ready-transition 部分时序用例仍有历史不稳定失败；未执行 8/32 个独立高层发布进程的长时压力 | 已完成，进入 P4 |
| P4 | 工作区变更（未提交） | `IpcInfoPool::ScopedRegistration::update_extra()` 原位更新 MPMC transport/layout/generation/publisher count/coordinator slot；`dzipc_list`、`dzipc_topic_cat` 和 sniffer 使用版本化段名与 `channel` 拓扑；旧 route 与 `__MPMC_V2` 物理隔离 | `test_ipc_info_pool`、`test_ipc_info_pool_layout`、`test_ipc_info_pool_version`、`test_channel_scope` 全部通过；高层测试断言每个 MPMC 发布者都能在 info pool 看到 `transport=shm_mpmc;layout=V2;gen=...;pub=...;coord=...`，且长度不超过 63 字节；工具和 sniffer 目标已重新构建 | `extra` 只能展示 coordinator slot，不能在定长字段中稳定展示完整 64 位 publisher id；混合部署仍要求显式启用 UDP/Hybrid 过渡 | 已完成 |
| P5 | 工作区变更（未提交） | 重新构建 `/tmp/cpp_ipc_dds_mpmc_build`，执行 MPMC 专项、默认 legacy 聚焦回归、完整 CTest、段残留检查和构建指纹采集 | MPMC 专项 `ctest -R 'test_(shm_mpmc|shm_multi_publisher|ipc_info_pool|channel_scope)'`：8/8 通过；默认 `DZIPC_SHM_MPMC=0` 聚焦回归：8/8 通过，另行运行 `test_shm`、`test_channel_scope`、`test_shm_sniffer_control_name` 全部通过；完整 CTest：28/36 通过；最终复跑后仍无 `__MPMC`/`MPMC` 共享内存残留，`git diff --check` 通过，并已采集库、专项测试及工具 SHA-256 | 完整 CTest 的 8 个失败测试目标均可在旧基线复现：`test_w05_stale_slot_gate`、`test_w05_stale_slot_gate_arm`、`test_wakeup_artifact`、`test_shm_i5_pop_buffer`、`test_shm_sub_dtor_gate`、`test_shm_ready_transition`、`test_w03_measurement`、`test_dzflat_fallback_semantics`；失败集中在旧 worker/时序、弹出缓冲与计数基线，不是 MPMC 专项失败。`test_dzipc_shm` 因共享环境中已有同名长时进程占用资源，未完成本次独立运行，不能记为通过 | 专项验收完成，完整回归待基线问题处理 |

### 9.1 最终核验记录（2026-10-05）

- 构建目录：`/tmp/cpp_ipc_dds_mpmc_build`；`cmake --build ... -j2` 退出码为 0。
- MPMC/info 专项：`ctest --test-dir /tmp/cpp_ipc_dds_mpmc_build --output-on-failure --tests-regex 'test_(shm_mpmc|shm_multi_publisher|ipc_info_pool)'`，8/8 通过，退出码 0。
- legacy 聚焦：`DZIPC_SHM_MPMC=0` 下 `test_shm`、`test_channel_scope`、`test_shm_sniffer_control_name` 均通过；此前记录的聚焦集合为 8/8。
- 完整 CTest：36 个目标中 28 个通过、8 个失败，退出码 8；失败目标见 P5 行，均为旧基线已知问题。环境中已有长期运行的 `test_dzipc_shm`（PID 672331），未终止或复用其资源。
- 质量检查：`git diff --check` 通过；`/dev/shm` 未发现名称包含 `__MPMC` 或 `MPMC` 的残留文件。
- 关键产物 SHA-256：
  - `lib/libipc.so.1.6.0`：`6b002d93cca6ece1d64b1b4f902e6c34e62fb238e2c85c0b484bf2d7e0c1fbc6`
  - `bin/test_shm_mpmc_channel`：`bd01401c9683dc89505b70181f228b710cd331b13e71cb4f6f381c829813d2de`
  - `bin/test_shm_mpmc_types`：`3b085c69cb985bf357ee811047ae113e3acd7cd3b358b7273731660c4cb49844`
  - `bin/test_shm_mpmc_config`：`bee93a728da1913a3758e9ea638b109cc0d857797e711016c8ef29aac7b2d4cc`
  - `bin/test_shm_mpmc_control_plane`：`6a646b87559be1c36ff848d2b45fe1846a546ccd3d1b44244bbb028054a6628e`
  - `bin/test_shm_multi_publisher`：`1bb16d5daf7282474584b914cc419fadcd5882c213286388bd61fcef40c4cd92`
  - `bin/test_ipc_info_pool`：`71bc802d359fecc8c548628a3feb7a592c071221be7b6915c45297bd351b40d9`
  - `app/dzipc_list`：`7a818c37360c56734ed1e6b91f281f27307cb6033457c806e4a3d85106981e55`
  - `app/dzipc_topic_cat`：`916740b8d503d9e3fe9279c57bf477eb9264c108e4335ad4226f5524535b93d7`
