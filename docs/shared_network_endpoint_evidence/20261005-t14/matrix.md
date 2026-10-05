# 第 17 节验收覆盖审计

“通过”限于列明的测试层次；“部分”表示只覆盖部分断言/部署条件；不把单元测试等同物理跨机验证。执行日志为本目录 ctest.log、sanitizer.log，以及对应 JSON；T04～T13 证据沿用各节点目录。本次没有测试级 skip，未执行的部署/性能场景逐项列在表内。

| 编号 | 结果 | 证据与边界 |
|---|---|---|
| V01 | 通过 | wire：独立 golden 的 DATA/ACK/NACK/REJECT 逐字节对拍 |
| V02 | 通过（重组层） | reassembly 0/2^32/2^40+3/MAX 与独立目录编码逐项对拍、同 shard 八路不串话 |
| V03 | 通过（重组层） | 同源端口、同接收 shard、msg_id=0 的两话题逐字节交错重组 |
| V04 | 通过 | reassembly：InterleavedPublishersAndTopicsNeverMix |
| V05 | 部分 | wire 的全名与指纹一致性、snapshot 类型冲突/PUB-only 校验；未注入真实双哈希碰撞 |
| V06 | 通过 | wire 所有截断长度、逐字节损坏及未知字段拒绝 |
| V07 | 通过 | wire/config 长度、片号、计数和溢出边界，分配前拒绝 |
| V08 | 通过 | reassembly 整包 CRC 与片冲突拒绝 |
| V09 | 通过 | reassembly + final-fault-mixed 的真实 UDP 重复/乱序 |
| V10 | 通过 | blob/local_direct/bridge 多页 TLV 字节保持与普通订阅者 |
| V11 | 通过 | blob/prebuilt/public_api；T11 Python 两项，T13 工具段读取 |
| V12 | 部分 | 网络 codec 拒绝错误 magic/version；旧 UDP 端点双向异协议进程专项未执行 |
| V13 | 通过 | wire + generate_vectors.py 独立 Python/C++ RouteKey/CRC 对拍 |
| V14 | 通过（重组/SHM 层） | K=3 来源，K=2 接收双 shard 都收到相同数据，仅规范 shard 向真实 SHM 提交一次 |
| V15 | 通过 | reassembly 来源/端口零分配，reliable 错误 ACK 元数据不能终结 |
| V16 | 通过 | snapshot/发现，PUB-only、SUB、混合角色与空目录原子撤销 |
| V17 | 通过（目录/重组层） | 同前 127B、不同后缀名称在编码和单 shard 重组保持独立；T13 工具显示截断提示 |
| V18 | 通过 | local_protocol 所有 v2 类型及 112B 出站独立 golden |
| V19 | 通过 | local_protocol 拒绝旧版本与保留 BEGIN 类型 |
| R01 | 通过 | scale-1/100/1000.json，K=4、UDP=6 |
| R02 | 通过 | scale-1000.json，应用 UDP=0（业务 SHM FD 仍随话题增长） |
| R03 | 通过 | subscribers-32.json，驱动断言每消息目标 commit 一次，无重复/回流 |
| R04 | 通过 | multipublisher-2/8.json，退出一个后剩余发布进程继续逐字节交付 |
| R05 | 部分 | 两套隔离 IPC/mount 的真实网关互发通过；没有两台物理主机 |
| R06 | 通过 | teardown：同会话 100 次创建/关闭，FD/线程/SHM 集合回基线 |
| R07 | 部分 | endpoint 100 目标共 socket，discovery peer 配额；8 个真实网关 fanout，未跑 128 实际 peer |
| R08 | 部分 | quota 按 route 限额回滚与冷热推进；未逐一触顶每种话题级配额 |
| R09 | 通过 | snapshot 最后 Ready/重订 epoch，reassembly/dedup 退役测试 |
| R10 | 通过 | T13 + 当前 CTest topic_cat：远端无本机 pub，应用 UDP=0 |
| R11 | 通过 | snapshot 活跃/候选/引用旧目录各配额及原子替换 |
| R12 | 通过 | outbox 所有 loan 档位、实际容量边界与 shm_capacity |
| R13 | 部分 | dedup stream 触顶不 LRU、discovery epoch 历史触顶；独立细分诊断计数不齐 |
| R14 | 通过 | end_to_end：本机和远端各收一次；source_injections=0 |
| R15 | 通过 | end_to_end 的 SIGSTOP；failure/restart 真实 SIGKILL 后既有本机收发 |
| R16 | 通过 | outbox/credit：等待者先登记、已有信用时 credit_requests=0，无 BEGIN |
| R17 | 部分 | shard 所有权源码 + 错误 shard 测试 + 不同 shard 热冷并行；完整多 shard 阶段计时未补齐 |
| R18 | 通过 | outbox_failure 全局/会话预算、credit 记录进度不提前归还缓存预算 |
| F01 | 通过 | reliable 首轮 DATA 全失状态机恢复与期限终结 |
| F02 | 通过 | final-fault-loss1/loss5，确定性实际 UDP 丢片，逐字节正确 |
| F03 | 通过 | reliable + final-fault-mixed，ACK 丢失后只 commit 一次 |
| F04 | 通过 | reliable 冻结目标/一端成功另一端失联，保留部分交付 |
| F05 | 通过 | reassembly ACK 只跟随真实 SHM commit |
| F06 | 通过 | reassembly Indeterminate 不重复提交 |
| F07 | 通过 | reliable/client_lifecycle 终态单次、早/晚回执与 timeout |
| F08 | 通过 | outbox_failure/failure/restart 真实 SIGKILL、旧会话不重放 |
| F09 | 通过 | gateway_lock 暂停持锁进程与第二实例互斥 |
| F10 | 通过 | reliable PeerRestarted + restart 新身份，旧事务不迁移 |
| F11 | 通过 | prebuilt 坏段 false 后普通 publish 只收一次 |
| F12 | 通过 | outbox_failure/partial_submit：已可见提交不能允许安全回退 |
| F13 | 通过 | snapshot/reassembly 最后订阅者撤销、旧代次拒收 |
| F14 | 通过 | failure/shm_capacity 借样跨网关/所有者销毁 |
| F15 | 通过 | client_lifecycle/local_direct fork 在继承锁前拒绝 |
| F16 | 部分 | 接收、候选/旧目录、发送信用等主要配额测试通过；全部分项触顶与精确独立错误计数未齐 |
| F17 | 通过 | discovery/dedup 租约恢复要求目录同步，保留同 epoch 去重 |
| F18 | 通过 | discovery retired epoch 不重新接纳 |
| F19 | 通过 | dedup 回执 TTL/窗口滑移后不重复，不伪造 ACK |
| F20 | 通过 | partial_submit tm=0、credit 慢编码期限；发送前再次检查原期限；晚包不承诺撤回 |
| F21 | 通过 | credit 超时零 ACK 仍 possible_remote_delivery |
| F22 | 通过 | client_lifecycle、outbox_failure 累计通知重放/进度越界/写满会话失效 |
| F23 | 通过 | local_direct/partial_submit 本机成功网络失败不回退 |
| F24 | 通过 | partial_submit 本机池失败而网络接管，仅一腿尝试 |
| F25 | 通过 | partial_submit 全部两腿三态组合 + reassembly 不重复提交 |
| F26 | 通过 | client_lifecycle 重复 GRANT；credit TX_PROGRESS 不提前归还载荷额度 |
| F27 | 通过 | outbox 零信用补充、client_lifecycle 断连；冷启动成本尚无正式分位数 |
| F28 | 通过 | endpoint/reliable 部分 sendmmsg 前缀及 EAGAIN 原身份保留；有界延期 |
| F29 | 部分 | fairness 重传份额、control_pressure NACK/ACK 风暴模型、30 秒冷热目录更新；未合并真实丢包风暴与目录更新并统计恢复 p99 |
| F30 | 通过 | public_api/restart 失败重置关闭两腿，不保留混合类型 |

# 性能场景

| 第 17.4 节场景 | 结果 |
|---|---|
| 纯本机 1→1/8/32 | 54 个正式窗口；9 组三轮中位数门槛通过，5/27 逐轮配对不满足，详见 performance/summary.md |
| 本机与网络同时活跃 | 功能/网关暂停独立性通过；未形成全部阶段的 3×30 秒性能矩阵 |
| 1 个远端、多订阅进程 | 1/8/32 交付和单次注入通过；实际物理跨机性能未执行 |
| 2/8 个远端 | fanout 确认线性单播放大并记录重传；非正式吞吐/延迟分位数 |
| 1000 个低频话题 | 创建/关闭、FD/线程/RSS/SHM/UDP 有采样；1000 活跃低频流和 30 秒空闲 CPU 矩阵未执行 |
| 同/不同 shard 冷热混合 | 各 3 个 30 秒窗口；公共 publish_blocking、预构造 GenericMessage、1000 ms deadline，详见 fairness-*.json |
| 同话题 8 发布者 | 功能和退出存活通过；竞争阶段计时与饱和吞吐未执行 |
| 冷启动/零信用/首次大包 | 功能测试通过；冷启动延迟分位数未执行 |
| 丢片/ACK 丢失 | 确定性真实 UDP 故障正确性通过；物理网卡恢复 p99/额外带宽未执行 |

物理跨机、PTP 误差界、真实网卡吞吐/CPU/流量、完整阶段计时/复制字节/上下文切换、普通对象编码的全负载矩阵仍缺少证据。不得将本表解释为第 17 节全项验收完成。
