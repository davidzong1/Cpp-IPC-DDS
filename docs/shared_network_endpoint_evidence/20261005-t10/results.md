# T10：可靠发送、独立信用与有限调度

基线 `796129d`，本记录与 T10 实现同提交。

- ReliableSession 在所属 shard 冻结全部目标和确认元数据，单次终结 Completed/失败；目标离开
  不缩小成功集合。ACK 匹配来源地址、端口、两端身份/epoch、发布者/序号、大小、CRC、schema 和路由代次。
- 预登记等待者后直接提交 DZTX。没有 SEND_BEGIN/BEGIN_READY；已保留的 DZLC 15/16 仍被协议层拒绝。
  首发按目标轮转，缺片用位图合并，2ms 起始探测重发首片，最大 100ms 退避；NACK 有最小间隔。
- 批量发送只推进成功前缀，EAGAIN 后保留后缀；等待 ACK 或 EAGAIN 时不忙转。
  控制接收/发送分别使用全局与 peer 令牌预算，数据、控制、目录均有限轮次。
- TX_PROGRESS 仅回收出站容量，ACK/拒绝/超时终结后释放网络缓存并授予信用；每发布者可靠在途
  与全局目标状态有上限。重复 DZTX 请求不再次终结；登记对象的序号窗口拒绝重新启动旧发送。
- 部分交付即使 acked_count=0 也报告可能交付。tm=0 无提交，编码后检查原期限；本机 blob 复制
  也使用同一 deadline。无远端与 GatewayLost 的本机结果合并不同。
- 目录的 PUB/SUB 权限分别撤销：只增加发布角色不会错误取消仍有效的订阅目标。
  status 新增已终结事务的重传、NACK 和错误确认计数。

验证命令（退出码均为 0）：

```sh
cmake --build build-shared-net --target test_shared_net_reliable test_shared_net_credit test_shared_net_fairness test_shared_net_control_pressure test_shared_net_partial_submit test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota test_shared_net_discovery test_shared_net_subscription_snapshot shared_net_probe dzipc_gateway --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
python3 test/shared_net/end_to_end.py --reliable --gateway /home/zwc/cpp_ipc_dds/build-shared-net/bin/dzipc_gateway --probe /home/zwc/cpp_ipc_dds/build-shared-net/bin/shared_net_probe
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(reliable|credit|fairness|control_pressure|reassembly|dedup|quota|subscription_snapshot)' --output-on-failure
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

新增 **14 条 GTest（6+5+2+1）+ 1 个可靠多进程驱动**，共享 CTest **26/26**，ASan/UBSan **9/9**，
OFF **2/2**，无跳过。故障种子为 20261005。

纯状态机注入首轮 DATA 全丢、5% 丢片、首次 ACK 丢失，均只提交一次且全字节一致；覆盖无订阅、
所有 ACK 缺失、错误确认各字段、部分发送/EAGAIN、双目标部分成功、新 peer epoch、NACK 风暴、
八目标轮转以及单次终结。真实网关测试在 TX_PROGRESS 后观察信用仍不变，ACK 后恰好归还一份；
本机成功/网关断开、30ms 无 ACK 超时、20ms 慢编码配 1ms deadline、重复出站序号均符合边界。
两套独立 SHM 命名空间的可靠集成测试同样完成 12 条双向消息、48 次全字节交付和应用 UDP=0 检查。

本卡验证有界调度和短定时器语义，不替代 T14 的 30 秒冷热负载、p99、真实跨机网卡和故障带宽测量。
公共工厂将在 T11 接入；更完整的关闭/析构/重启资源回落在 T12 验证。
