# socket 作用域分析、优化及 SHM 统一结果

日期：2026-10-02。结论：已统一 domain、通信类型、原始话题身份，先完成 socket 隔离验证，再用于 SHM。接口中的 domain 仍由调用方传入，不增加运行时开关。

## 原实现是否合理

| 原行为 | 评价及问题 | 本轮处理 |
|---|---|---|
| socket 用 domain 乘话题哈希得到端口，再折回合法范围 | domain=0 时端口基址恒为 11451；同话题 domain 相差 54081 时可确定性同址；乘法表达不了独立命名空间 | 完整作用域键散列并混合后寻址 |
| 组播地址只使用 topic | 不同 domain 共用组，仅靠有限端口范围隔离；碰撞时缺少报文身份兜底 | 组播也包含 domain、通信类型 |
| 每个端点基址独立散列，另加 0…4 | 一个基址可能落进另一个作用域的 ACK/握手端口范围 | 五端口对齐分配，部分重叠消失；整组碰撞仍由身份头隔离 |
| 基址包含默认临时端口范围 | 本轮曾实际遇到端口 34086 被占用、订阅端绑定失败 | 新范围 11451…32765，避开本机默认临时范围 32768…60999，不修改宿主网络配置 |
| size_t domain 传 int，注册池保存 int32 | 只在高位不同的 domain 可能丢失区别；自动选路可能误认对端 | 寻址及注册池使用 uint64，自动选路/本地匹配不再截断 |
| 进程内 ChannelKey 已含 domain | 原做法合理，继续保留 | 不把本地快速路径误当跨进程隔离验证 |
| SHM 已使用 domain 派生数据/控制/服务段名 | 因此“SHM 完全没使用 domain”并不符合当前源码；但斜杠等字符清洗有别名碰撞 | 保留 domain 显式前缀，增加同一完整作用域的固定长度身份 |

未仅复制旧 socket 的端口公式：旧公式本身会退化和折回，移植会把缺陷一起带入 SHM。

## 实施方案与结果

作用域键为 `DZSC2:<通信类型>:<uint64 domain>:<UTF-8字节数>:<原始topic>`。长度字段使名字中的冒号等字符不产生拼接歧义；消息类型 `msg_id` 继续独立校验。pub/sub 与 RPC 分属不同通信类型，RPC 请求、响应、握手及 ACK 使用各自端口偏移。

socket 每个报文增加 32 字节网络序身份头：版本标记、通信类型、完整 64 位 domain、原始作用域的两路 64 位 FNV 身份值。先校验身份，再把内层数据交给原有重组/ACK/握手处理；即使强制相同 IP 和端口，其他 domain、其他话题、其他通信类型及裸旧帧也不能直接进入本作用域的消息解析。

域字段独立比较。话题双散列用于事故隔离，不能视为密码学认证或零碰撞数学证明。公网恶意发送者仍可构造同一身份头；本特性不承担鉴权。

寻址在构造时计算；加 avalanche 混合改善连续话题编号直接 FNV 取模的聚集。每作用域占五端口槽位，端口组与组播地址都有限，仍可能碰撞或被其他程序占用，但碰撞不再等同于跨域交付。若系统自定义临时端口范围低于 32768，新范围也不能保证完全避开；绑定失败继续按原有有界重试显式报告。

SHM 使用 `dz_ipc_d<domain>_s2_<32位十六进制身份>_topic`；服务通道采用 Service 身份并追加 `_ser_r`/`_ser_w`，控制面仍由同一通道作用域派生。`/a/b` 与 `_a_b` 不再共用段，长原始名字不会拉长段名。独享池身份仍由最终 `topic_name_.c_str()` 经 fnv1a64 得到，不改变上轮每话题每尺寸档十块、活跃复用及最后引用回收的规则。

注册池改为 V3；domain 扩为 uint64。PoolEntry 仍是 296 字节，但 extra 字段偏移变化，已同步更换段名和版本；新进程不读取/改写旧注册池。C++ topic_cat 和 Python dzplot/dzviz 的寻址/嗅探同步升级，topic_cat 握手探针同样校验作用域。

## 成本与兼容性

- UDP 上限仍为 1472 字节；分片页从 1472 改为 1440 字节，为身份头预留 32 字节。TLV 纯载荷页从 1460 改为 1428，GenericMessage、DzFlat 预构建分帧、NACK 位图单页上限、嗅探解析一起同步，避免改动后触发 MTU 1500 下的额外 IP 分片。
- 对满页数据，协议有效载荷减少约 2.2%；小包额外增加固定 32 字节。发送端增加一次有界栈内载荷复制，接收端比较固定头并返回原缓冲区视图；临时 ipc::buffer 句柄仍沿用库分配器，不能宣称零分配或延迟必然下降。优化目标是作用域正确性与可用性。
- 库版本 **1.5.0 / SONAME libipc.so.5**。socket 寻址、报文格式、公共消息序列化、SHM 段名及注册池布局均变化。通信双方、消息头、Python 扩展及外部嗅探工具须整体重新构建升级，不能与旧端混跑。
- 原 `udp_discovery_*` 辅助函数保留给裸 UDP/旧诊断使用；高层新通道必须使用 `socket_scope_*` 与 `channel_scope_token`，旧辅助函数不再描述高层新通道。
- 本轮在 Linux x86_64 验证；Windows 未运行验证。worker 的 32 位 domain 参数仍是调度散列输入，其完整 route key 含 domain；隔离依据是完整作用域而非单独的 worker 参数。日志模块的历史 32 位 domain 字段未扩展，不用于路由判定。

## 测试结果

27 组 C++ 回归、212 个用例全部通过。测试在私有 user/mount/IPC namespace 内使用独立 tmpfs；未改宿主 /dev/shm、网络 sysctl 或 RouDi。域测试关闭 nodelet，避免进程内投递掩盖实际传输路径。

| 程序 | 用例数 | 结果 |
|---|---:|---|
| `test_channel_scope` | 10 | 通过 |
| `test_socket_reliable_crc` | 5 | 通过 |
| `test_socket_borrow` | 6 | 通过 |
| `test_socket_recv_worker` | 7 | 通过 |
| `test_socket_ser_concurrency` | 3 | 通过 |
| `test_udp_port_boundary` | 8 | 通过 |
| `test_shm_domain_isolation` | 3 | 通过 |
| `test_shm_sniffer_control_name` | 3 | 通过 |
| `test_dzflat_transport` | 10 | 通过 |
| `test_dzflat_sercli` | 4 | 通过 |
| `test_sercli_auto_path` | 15 | 通过 |
| `test_topic_chunk_pool` | 25 | 通过 |
| `test_ipc_info_pool` | 10 | 通过 |
| `test_ipc_info_pool_layout` | 3 | 通过 |
| `test_ipc_info_pool_version` | 2 | 通过 |
| `test_socket_endpoint_split` | 18 | 通过 |
| `test_handshake_probe` | 6 | 通过 |
| `test_loan` | 10 | 通过 |
| `test_lifecycle_contract` | 10 | 通过 |
| `test_shm_route_session` | 15 | 通过 |
| `test_dzflat_rx` | 8 | 通过 |
| `test_dzflat_fallback_semantics` | 6 | 通过 |
| `test_recv_fragment_isolation` | 2 | 通过 |
| `test_socket_wait_set` | 9 | 通过 |
| `test_socket_nodelet` | 11 | 通过 |
| `test_socket_topic_isolation` | 1 | 通过 |
| `test_shm_ser_backpressure` | 2 | 通过 |

新增关键判据：强制端点碰撞后拒绝外域/外话题/外类型/裸帧；domain=0 与高位 domain 的身份区别；五端口对齐与临时端口区间避让；原始名字别名、长名、UTF-8 固定向量；注册池 domain 高位往返；socket/SHM 实际 pub/sub 与跨进程 RPC 的 domain=0、domain=2³² 各收自己的载荷。

Python 工具：`tools/dzplot/test/test_dzplot.py` **129/129 通过**，含与 C++ 共用的固定命名向量、控制面及清理 glob。未安装当前 Python 二进制扩展，因此不能据此声称真实 Python 扩展跨语言收发验证；C++ 嗅探器完整编译成功，握手探针和 SHM 嗅探实际回归通过。

传输验证：SHM/A/B/prebuilt 的 pub/sub × 8 B/1 KiB/1 MiB，以及 SHM/A 的 RPC × 8 B/1 MiB，共 **16 格全载荷校验通过**。socket RPC 8 B/1 MiB 通过。socket 小包 pub/sub 通过；socket 1 MiB best-effort 突发测试有下述既有限制，未计入“全通过”。

### UDP 大包对照与限制

修改前版本由本轮开始时的源码状态重建（包含上轮独享池生命周期改造）；新旧程序串行运行，domain=0、每格 3 秒、1 MiB 全载荷检查，各三轮。下表全部保留失败格，不将“发送 API 成功”当作完整交付。

| 窗口/限速 | 版本 | 轮次 | 发送 | 接收 | 校验错误 | 缺口 | 结果 |
|---|---|---:|---:|---:|---:|---:|---|
| 8/0 msg/s | 修改前 | 1 | 13 | 2 | 1 | 3 | degraded |
| 8/0 msg/s | 修改后 | 1 | 333 | 332 | 0 | 1 | degraded |
| 8/0 msg/s | 修改前 | 2 | 149 | 135 | 2 | 6 | degraded |
| 8/0 msg/s | 修改后 | 2 | 333 | 330 | 0 | 3 | degraded |
| 8/0 msg/s | 修改前 | 3 | 340 | 338 | 0 | 2 | degraded |
| 8/0 msg/s | 修改后 | 3 | 334 | 331 | 0 | 3 | degraded |
| 1/30 msg/s | 修改前 | 1 | 90 | 90 | 0 | 0 | ok |
| 1/30 msg/s | 修改后 | 1 | 90 | 90 | 0 | 0 | ok |
| 1/30 msg/s | 修改前 | 2 | 90 | 90 | 0 | 0 | ok |
| 1/30 msg/s | 修改后 | 2 | 90 | 90 | 0 | 0 | ok |
| 1/30 msg/s | 修改前 | 3 | 90 | 90 | 0 | 0 | ok |
| 1/30 msg/s | 修改后 | 3 | 90 | 90 | 0 | 0 | ok |

限速 0 表示不叠加应用限速。窗口 1、30 msg/s 下，新旧版本各三轮均为 90/90 条、零坏消息、零缺口。窗口 8 突发下两版均有丢包，旧版也复现错误载荷；开发中的新版本首轮突发也曾出现 1 条校验错误。此次没有修复既有 best-effort 丢片/重组问题，也不以最终三轮没有错误载荷承诺突发可靠性。需要可靠交付的业务应使用已有可靠模式；相关 CRC/ACK/端点分离回归通过。上述短测不能用于宣称相对 CycloneDDS 的速度排名或吞吐提升。

中途一次未限流复测因 34086 端口被占用而超时，已据此把新端口区间移到默认临时端口以下，并重新完成最终 27 组回归。未改动宿主端口配置。

## 复现与交付

```bash
python3 -B test/transport_comparison/domain_scope_runs.py --work /var/tmp/cppipc-scope-new
cmake -S test/transport_comparison -B /var/tmp/cppipc-scope-new/final/build
cmake --build /var/tmp/cppipc-scope-new/final/build -j6
unshare --user --map-root-user --mount --ipc --fork bash -c 'mount --make-rprivate / && mount -t tmpfs -o size=4G tmpfs /dev/shm && python3 -B test/transport_comparison/domain_scope_socket.py --work /var/tmp/cppipc-scope-new'
PYTHONDONTWRITEBYTECODE=1 python3 -m pytest -p no:cacheprovider tools/dzplot/test/test_dzplot.py -q
```

`domain_scope_socket.py --baseline` 额外使用 `baseline/build/comparison`；需自行准备修改前构建。窗口 8 是诊断格，脚本按窗口 1/30 msg/s 的失败返回非零；所有格状态都写出。CMake 的 DDS 安装前缀沿用原 comparison 工程默认配置。

改动保留在工作区，未提交；上轮池改造保留。新增仅有作用域头文件、测试源码/脚本与本报告/执行记录。临时源码副本、构建、二进制、JSON 与原始日志在结果摘录后清理。此前生命周期报告是 1.4.0 历史验证，本报告描述当前 1.5.0。

## 验证摘要

- 修改前comparison SHA256：`a2d2efbd4930fab2f77ce2e794357a68f55fe2ed3f079b4112a31d1ad9c49c6e`
- 修改后comparison SHA256：`a25bcead0de4794d3bd110bd5e7ab1f45bdae2773f46d5dc06204cef7ae00730`
- 修改后动态库 SHA256：`04bae89537462fd9419586a2aea2732fbbd756264191b82ffdf7186c343d948a`
- 单测结果 SHA256：`daddbdd911ec18eb679a5d399e97003f0de3b3702c17a06eda0ad9ee3b2f2c22`
- 全载荷16格 SHA256：`8e6a8cb238a51029b12f7b66718a86588bddde3371001929b4911e2179239a9d`
- UDP新旧对照 SHA256：`38dfc4ba6d9b71f46c67ce5cfe390ba3985e2cce6675bf8538b89dbad4492172`
- 基线工作区补丁 SHA256：`c10cfac16e7129d46d6fb13d1e926489c488590deaa957da190b254fafe53fe5`
