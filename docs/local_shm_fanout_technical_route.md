# local_shm_fanout 技术路线闭环

> 状态：方案审计与分期路线已完成；产品实现暂不启动，等待 T0/T1 闸门和产品语义签字。
> 依据：`docs/local_shm_fanout.md`，以及团队对源码锚点、量测计划和验收矩阵的联合审阅。

## 1. 目标与范围

目标是解决跨进程同机同话题的重复 UDP 接收：由一个按 `(topic, domain)` 归属的共享入口接收网络数据，再通过 DZFlat SHM 扇出给本机订阅者。跨机 UDP 路径、组播寻址修复本身和 ser/cli 专用路径不在首批实现范围内。

本轮没有修改产品代码，也没有声称 bridge 已实现。已完成源码审计、风险识别、量测设计和分期验收定义。

## 2. 已确认事实与原文订正

1. socket/UDP wire 当前是 TLV，DZFlat 是 SHM 专用布局（见 `docs/dzflat_shm.md:400`）。bridge 若不改变 wire，必须承担 TLV 到 DZFlat 的转换；原文 §3.1 的“约 2 次拷贝”和“N=1 打平”不能直接成立。
2. socket 订阅者是 2 个 socket、1 个订阅接收线程；原文“每订阅者 2 个线程”引用错位。
3. `IP_MULTICAST_LOOP` 是主机级开关，必须保持为 1（`platform/posix/udp.h:225-231`）；删除原文把它描述为 per-socket 的备选方案。
4. 数据帧 tail 没有 `sender_id/sequence`，`local_node_id` 只在控制面出现。因此不能在 SHM 消费侧补做去重。
5. `SO_REUSEADDR/SO_REUSEPORT` 不能保证唯一入口；双 bridge 可能同时 bind 并静默双投递，认领必须有独立互斥。
6. `IpcInfoPool`/`discovery_loop` 可复用作存活订阅计数，但存在 `kMaxEntries=512` 上限和 native UDP 监听者不可见边界，必须定义 fallback。

## 3. 分期与依赖

### T0：前置可行性核查（最高优先级，零产品代码）

- 核实 wire 形态、TLV→DZFlat 转换成本和可用的 DZFlat 开关。
- 核实同一 netns 的本机接口判定、`recvmsg + IP_PKTINFO` 所需接收侧改动。
- 固化原文勘误及组碰撞（`shm_defect_fixes.md` §5）处理决策。

验收：每个结论都有 `file:line` 证据；任何“需改 wire”或无法定义本机边界的结论都阻断 S2/S3。

### T1：量测（可与 T0 并行）

- Q1：1 发布端 + N 个真实 SocketSub，N=1/2/4/8；记录每进程 CPU、`/proc/net/udp` drops、`/proc/net/snmp` 组播计数和收包数。用 raw-socket 与完整重组两层切片，分离内核复制和用户态重组。
- Q2：使用 `test/dzipc_perf_benchmark.cpp` 对同机 IPC_SHM/IPC_SOCKET 做同 wire 对照，记录吞吐、p50/p99 和丢包率。已有单订阅者参考数据：64B 时 SHM/socket p50 约 9.1/54.3us，1MB 时约 243.8/9074us；这些数据只能作为基线，不能替代同 wire 复测。
- Q3：bridge 尚不存在时不伪造点估计；原型阶段用 `/proc/<tid>/schedstat` 估算 SHM 唤醒等待，并与直接 UDP 对照。
- Q4：测单话题单线程重组上限，覆盖“小载荷×高频”场景；这是否决集中化的关键量测。

验收：原始 CSV、环境参数、复现命令和 md5 可复算；预注册负载、CPU 统计窗口和延迟预算后再定 N*，不以“线性增长”单独裁定。

### S2：发布侧 SHM + 订阅侧切换 + 去重（不可拆）

发布者向本机 SHM 写入不能与订阅者继续只读 UDP 并行作为“完成”；S2 必须同批交付订阅侧切换和去重，否则只会增加一次拷贝。

- 去重位置：bridge/接收侧 UDP，在 `recvmsg + IP_PKTINFO` 得到源 IP/ifindex 后按“同一 netns 本机接口”过滤；不改 wire。
- 保持组播 loopback=1。
- 订阅计数触发必须与 `IpcInfoPool` 存活判定一致；计数池满或发现盲区时明确回退到原 UDP 路径。
- 预注册启用门：若仍需 TLV→DZFlat，默认先按 N≥3 设计；若 T0 证明可直写 DZFlat，可评估 N≥2。最终以固定负载量测和单核余量门为准，不做运行时带宽自适应。

S2 硬门：恰好一份投递、N=1 不增加 bridge 流量、ABAB/高频回归无重复、收齐率不劣于基线、`dzflat_shm.md:592` 慢消费者崩溃路径已复现并修复或明确排除、现有 41/41 回归通过。

### S3：共享入口（最高风险）

仅在 T0/T1 与 S2 全绿后启动。

- `(topic, domain)` 单 owner；复用 control-plane generation/slot 互斥，不能依赖端口 bind 成功。
- owner 崩溃后可重认领；预先定义接管窗口和窗口内允许丢失上界，使用带 generation 的存活判定，不能只依赖 `kill(pid,0)`。
- ACK 语义必须产品签字。推荐默认“出口级”：SHM 写入成功即 ACK，并暴露本机溢出/队列耗尽计数；若产品要求每订阅者可靠，需另行设计水位和序号机制，当前 SHM 通道不具备该能力。
- bridge 单核重组必须低于基线总 CPU，并在额定负载 60s + 2 倍突发 10s 下无队列/溢出计数增长；70% 仅作诊断参考，不单独作为通过门。
- 延迟门按话题预算定义；无预算的话题只能记录暂定值，不能直接写入最终验收条款。

## 4. 关键阻断与未决事项

以下任一项未书面裁决，S2/S3 不得合入：

- ACK 采用出口级还是其他应用语义；
- 组碰撞先修复，还是 bridge 启动时做共组自检并书面接受放大后果；
- DZFlat 默认关闭时的 fallback 与可观测指标；
- owner 接管窗口、丢失上界和 PID 复用防护；
- 多发布者、容器/netns 下“本机”的精确定义；
- ser/cli 是否永不启用 bridge（首批建议 N≡1 回退原路径）。

> **2026-09-14 补注（文档分析-claude）**：上述最后一条仍是**本方案（桥共享入口）**的未决项；
> 但需注意它已不再是“ser/cli 会不会有同机 SHM 路径”的问题——**ser/cli 已独立实现自动同机切
> SHM**（T2 设计 + T3 落码，见 `udp_shm_alignment_task_list.md`，「三项拍板结果」）。两者是
> **两条互不相同的路径**：本方案是“桥收网 + 本机 SHM 扇出”解决**同话题多订阅者各自收一份 UDP**；
> ser/cli 是 **1:1 配对**连接在同机时把承载换成 SHM（不需要双写、选路互斥）。因此本条的准确
> 表述是：**ser/cli 是否禁用 bridge**，而不是“ser/cli 是否走 SHM”。

## 5. 闭环结论

“共享入口 + 本机 SHM 扇出”方向合理，但当前文档不能直接作为实现规格。团队一致结论是：先完成 T0/T1，修正文档中的 wire、拷贝账、线程数和 LOOP 事实；再以 S2 的“发布侧、订阅侧、去重同批交付”为最小可交付；最后在唯一 owner、崩溃接管、ACK 语义和慢消费者崩溃门全部明确后推进 S3。

在这些闸门通过前，任何直接实现 bridge 的工作都可能产生静默双投递、可靠性语义退化或性能反向劣化，因此本轮闭环结论为“路线已拆解、证据与验收已就绪、实现暂缓”。

## 6. 拷贝账订正：UDP 借样恒有一次去帧拷贝（2026-09-14 拍板）

T1 落地后，§2 的事实清单需要补一条**与收益论证直接相关**的结论：

- **UDP 上的 `try_get` 借样不是零拷贝**，恒有**一次整段去帧拷贝**：接收缓冲是 `UDPNode`
  复用的临时内存（`src/libipc/platform/posix/udp.h`，非拥有视图、析构器 `nullptr`），
  且分帧把 12B 页尾插进段中间，跨页段在 wire 上不连续（`src/dzIPC/common/data_rev.cc:1585-1615`）。
  消除它只有“重构 libipc 共享接收路径”或“改 wire”两条路，**已拍板为长期契约、不投入消除**
  （依据与锚点见 `udp_shm_alignment_task_list.md`「三项拍板结果」）。
- **对本文的射程**：UDP 借样省掉的是“把 DZFlat 段当 TLV 反序列化 + 字段读”，**不是拷贝**
  ⇒ §3.1/§4 里任何“零拷贝”收益论证都**不得**引用它；本方案的可借样前提仍只能来自
  **DZFlat over SHM**（§4.1 “本机分发必须复用 DZFlat 借样”不变）。
- **附带事实**：该路径今天**没有生产者**——全仓 UDP 发送恒 `serialize()` = TLV
  （`src/dzIPC/socket_pub_sub_ipc.cc:337/385/422`）⇒ T1 是“能力就绪，非生效”，
  §1 的“桥若不改 wire 必须承担 TLV→DZFlat 转换”这条成本**不变**。

## 7. ser/cli 活体闭环证据（2026-09-15）

为避免把隔离命名空间中的假阴当成产品结论，已在宿主网络与共享内存命名空间完成一次跨进程活体运行：

- 运行目录：`build/live_runs/20260915_230701_2524992`；topic=`live_auto_1789484821_2524992`，domain=3。
- 客户端：`SUMMARY sent=8 ok=8 failed=0`，`FINAL kind=Shm`，`switch_attempts=1 switch_successes=1 switch_fallbacks=0`。
- 服务端：`SERVER SUMMARY cb=8`，并在回调序列中由 `Socket/Establish` 转为 `Shm/Active`。
- base+2 只读探针：观测到 `Unknown(0) → ProposeShm(1) → ConfirmShm(2)`，`undecodable=0`；同时捕获控制面及读写连接段创建/回收。
- 内核提示：`net.core.rmem_max/wmem_max=212992`，大于约 208KB 的多分片 UDP 仅可作为环境风险记录，不影响本次 8 次小消息判据。

因此，ser/cli 同主机自动切换 SHM、业务无丢失及断言握手状态机在该宿主环境下通过。此前 `build/live_runs/20260915_223134_3`、`223920_3` 的失败发生在隔离 namespace（`/dev/shm` 为空、UDP 无法建链），不作为产品失败证据。

同一活体中 `topic_cat_auto.log` 已打印 `Transport changed to SHM (slot 3). Reconnecting...`，随后持续显示 `Service(SHM)`；`topic_cat --watch_handshake`（socket 腿）也正确观察到双方 `ConfirmShm(2)`。因此 ser/cli 数据面切换与工具侧选路重建均有直接证据。

另有命名兼容性缺陷保持单独立项：ser/cli topic 带前导 `/` 时，`shm_service_prefix()` 生成含第二个斜杠的 POSIX SHM 名，触发 `shm_open(EINVAL)` 并静默回退 socket；本轮不改变既有段名契约，需后续设计旧/新名迁移与跨版本兼容。

> ⚠️ **2026-09-16 订正（writer-claude）**：上句"需后续设计旧/新名迁移"的前提**已被推翻** —— 含 `/` 的 topic **从来没有段**（`shm_ser_cli_ipc.cc:275-278` 控制面 open 失败即 `throw`，先于 `:280-281` 的 `clear_storage`），**无旧段名可发现、无迁移可做、无残留可清**；真正要做的是"改名 + 补可诊断日志"。验收边界见本文 §8，契约与用例见 `udp_shm_followup_tasks.md` §A/§B。

该缺陷已用同一宿主环境完成承重对照：`build/live_runs/20260915_232219_2580718`（topic 带前导 `/`）出现两行 `shm_open[22]`、`FINAL kind=Socket`、`shm_events=0` 且业务仍为 8/8；与无斜杠基线相比，前导 `/` 至少构成充分阻断因素（必要性及其他非法字符组合仍需另测）。同时确认失败分支把原因误标为 `ChannelOccupied/ShmChannelOccupied` 且未递增 `switch_attempts/switch_fallbacks`，建议作为后续可观测性修复项。

## 8. F1/F2/F3 验收边界与 F4 契约同步（2026-09-16）

本节把 §7 两条缺陷的**验收边界**收口，并同步 §2 的 sniffer 相关约束。设计细节、完整代码锚点与用例定义见 `udp_shm_followup_tasks.md` §A/§B，此处只记与本文路线相关的结论与订正。

### 8.1 订正 §7 的"迁移"前提（F1）

§7 末句"需后续设计旧/新名迁移与跨版本兼容"的**前提不成立**：含 `/` 的 topic **从来没有段** —— POSIX `shm_open` 必 `EINVAL`，且 `src/dzIPC/shm_ser_cli_ipc.cc:275-278` 的控制面 open 失败即 `throw`，位置**先于** `:280-281` 的两次 `clear_storage`（连破坏性路径都到不了）⇒ **无旧段名可发现、无迁移可做、无残留可清**。要做的是「改名（POSIX 逐字符 `/`→`_`，Windows 原样；不截断、不哈希）+ 补可诊断日志」，而别名守卫**已存在**（`auto_ser_cli_ipc.cc:303` → `:121-128` 以**派生段名**探测）。合法 topic 的段名**逐字节不变**，故 §7 已记录的活体证据与 `_topic` 相关结论**不受影响**。

### 8.2 F1 的验收边界：**不阻塞 F2**

F1 的承重断言只取**服务端**指纹（`kind=Shm`、`switch_successes`、段是否存在），**不使用**客户端 `FINAL kind` —— 后者在 F2 修好前不可信（见 8.3）。另：`/topic` 用例必须在**改名后**跑；别名用例（先起 `_foo` 再起 `/foo`）**必须带变异条款**（关掉 `:121-128` 的派生命探测后须转红），否则它过的是"新人惰性"而非"守卫生效" —— 改名前的派生名对 topic 串是**单射**，别名对根本不存在。

### 8.3 F2 的范围 = §7 第二条的对外可观测性

§7 记录的"原因误标为 `ChannelOccupied/ShmChannelOccupied` 且计数器不递增"根因在 `src/dzIPC/auto_ser_cli_ipc.cc:244-265`：`withdraw_to_socket()` 对**任何**原因都发 sig 4，而客户端把**任何** 4 都读成"对端拒绝/占用"（`:660-683`）⇒ "对端建腿失败"被记成"通道占用"。⇒ **F2 负责客户端侧标签与计数，F1 不替它背断言**；§7 的活体对照（`..._2580718`：两行 `shm_open[22]`、`FINAL kind=Socket`、`shm_events=0`、业务 8/8）即 F2 的现网证据。

### 8.4 F3 与本文的关系

F3（UDP `rmem`/`wmem`）与 F1/F2 **互不阻塞**，需运维变更窗口；§7 记录的内核实测值（`rmem_max`/`wmem_max` = `212992`）即 F3 的起点基线。

### 8.5 F4：公共 libipc 默认 `open_or_create` **不得变更**（本节对 §2/§3 是约束）

- **冻结理由**：sniffer 的默认模式是**文档化公共契约**（`include/libipc/sniffer.h:64-67`："opens-or-creates … so calling open() before the publisher is fine"；`include/libipc/shm.h:16` 默认参数即 `create | open`），且"先于发布端 open"是调用侧**正依赖**的能力（`exec/dzipc_topic_cat/src/shm_sniffer.cc:76-79` 的 `open_channels()` 仍**无条件**调用、失败即 `std::exit(1)`）；`ipc::sniffer` 已导出到 Python（`python/src/interface.cc:476-490`、`tools/dzplot/main.py:955`/`:1027`）⇒ 加模式参数必须同时决定 pybind 口径。
- ⇒ 本方案任何"工具侧不建段"的改动**只能走调用侧 open-only 探测**：`ipc::shm::acquire(name, 0, open)` → `get_mem` → **`release_no_unlink`**（⛔ **不可**用 `release()`，它会在引用计数归零时 `shm_unlink`）。既有同构先例：`src/dzIPC/common/control_plane.cc` 的 `occupied_by_other`、`exec/dzipc_topic_cat/include/control_plane_naming.h` 的 `control_plane_segment_exists`。**不得**改库默认、不得加公共 API。
- ⛔ **残留射程订正**：真正机制是 **sniffer 永不 unlink**（`src/libipc/sniffer.cpp` 析构走 `release_no_unlink()`）⇒ **任何"工具比发布端活得久"的运行都会永久留下该段**（dzplot / `topic_cat` 的常见用法），"无发布端"只是最显眼的子集 ⇒ §4 里"没有发布端就别 open"这类调用侧门控**不足以收口**。
- ⚠️ **HEAD 现状（2026-09-16 复核）**：工具侧控制面探测**已落地**（`shm_sniffer.cc:57-75` 用 `control_plane_segment_exists()` 门控，段不存在只提示、**不建**控制面段），但**数据通道侧仍未收口** —— `open_channels()`（`:76-79`）依旧无条件调用 ⇒ 无发布端时仍会**建出** `QU_CONN` 段，**对应验收用例 A1 在 HEAD 尚未通过**。

⚠️ **锚点基准与漂移**：本节 `file:line` 的复核基准是 HEAD `9d91212`（复核时工作区对该文件无改动）。截至本次同步，**F1/F2 正在 `src/dzIPC/auto_ser_cli_ipc.cc` 与 `src/dzIPC/common/name_operator.{h,cc}` 上落码** ⇒ 落地后 ser/cli 侧行号**需重核**；libipc 侧锚点（`shm.h`、`sniffer.h`、`sniffer.cpp`、`shm.cpp`）不受影响。
