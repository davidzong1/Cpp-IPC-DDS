# T08：发现、目录与两阶段订阅

基线 `2837522`，本记录与 T08 实现同提交。

- 网关从显式接口发送启动/周期 HELLO，经固定控制 socket 交换 DZGC 分页目录。
  PUB 与 Ready SUB 合并公告，只有 SUB 位参与目标选择。ROUTE_STATE 有版本且按有限轮次推送。
- REGISTER_SUB 通过现有唯一初始化线程创建 bridge，返回实际 SHM generation；SUB_READY
  校验 generation 后才公告。最后一个 Ready 离开撤销旧 route epoch，PUB-only 来源保留。
  新旧初始化任务交替调度，不增加每话题线程。名称与 msg_id/非零 schema 契约冲突被拒绝。
- 目录完整收齐、全量 CRC、排序/完整名称校验通过后才安装。部分目录不替换旧表。
  编码体 CRC 每版计算一次；发送端抓住不可变版本，按 peer 轮转及控制令牌预算逐页发送。
- 租约过期不清同 epoch 的权限身份和去重所需状态，恢复需重新校验完整目录；新 epoch
  先保留必要历史并永久撤销旧权限，延迟旧 HELLO/PAGE 不回退。历史无 LRU/TTL 淘汰。
- 候选、已安装、解析临时索引和仍有引用的旧目录分别计费；条目上限约束元数据。
  注册失败回滚。若撤销时资源不足，先撤销投递权限，再使网关失败退出，不能留下有效旧订阅。

本卡校正了方案中的期限矛盾：4096 个最长名称约 4305 页，在默认每 peer 1000 包/秒下
无法满足 2 秒绝对期限。现在 2 秒表示无新页面期限，重复页面不续期，另设 30 秒绝对上限；
执行方案第 8.2 节已同步。极低自定义控制速率仍可能导致目录同步超时，应相应配置容量。

验证命令（退出码均为 0）：

```sh
cmake --build build-shared-net --target test_shared_net_discovery test_shared_net_subscription_snapshot test_shared_net_gateway_lock --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
cmake --build build-shared-net-sanitize --target test_shared_net_discovery test_shared_net_subscription_snapshot test_shared_net_outbox_failure --parallel 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(discovery|subscription_snapshot|outbox_failure)' --output-on-failure
cmake --build build-shared-net-off --target test_shared_net_config --parallel 4
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

新增 **15 条 GTest（6+9）**，共享 CTest **19/19**，ASan/UBSan **3/3**，OFF **2/2**，无跳过。
覆盖 128 peer、4096 topic、完整名称逐项核验、乱序/重复/缺页/冲突/CRC/版本切换、旧目录引用、
候选与历史触顶、租约同 epoch 恢复、随机新 epoch 与旧 HELLO/PAGE 重放、PUB-only、两阶段登记、
最后订阅者退订与重建、SHM generation、不确定撤销时停止；真实 socket 测试观察显式 lo 接口
组播 HELLO，并用独立 UDP peer 完成快照握手，真实 Unix 会话完成 bridge 注册和 SUB_READY。

网关仍报告 ControlReady / data_plane_ready=false；数据提交与路由关闭的 shard 排序在 T09
接入。没有将本机模拟 peer 称为真实跨机验收。当前组播测试可用，未启用可选静态 peer 模式。
