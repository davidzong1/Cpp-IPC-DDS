# T07：有界重组、提交与去重

基线 `8a04ebe`，本记录与 T07 实现同提交。

状态机逐包接纳，先核验来源目录、地址、目标代次、shard 和完整消息身份，再预留完整
W(n)、位图、stream 和可靠回执。完成 CRC 后直接接管缓冲，在所属 shard 的有限 tick
中提交；可靠明确未提交可以短间隔重试，不确定提交只终结一次。实际释放缓冲后才回收费用。

去重窗口不随空闲和临时离线删除；在途低序号与可靠回执独立于滑动窗口保留。
回执到期后仍拒绝旧消息，但不伪造成功 ACK。只有永久 route/peer epoch 退休才清除历史。
NACK 单包最多 256 个缺片号；5 秒绝对期限不被重复片延长。多 shard 共用全局预算。

验证命令（退出码均为 0）：

```sh
cmake --build build-shared-net --target test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
cmake --build build-shared-net-sanitize --target test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota --parallel 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(reassembly|dedup|quota)' --output-on-failure
cmake --build build-shared-net-off --target test_shared_net_config test_dzflat_fallback_semantics --parallel 4
ctest --test-dir build-shared-net-off -R 'test_(shared_net_config|dzflat_fallback_semantics)' --output-on-failure
```

新增 **17 条 GTest（7+5+5）**，共享 CTest **17/17**，ASan/UBSan **3/3**，OFF 回归 **2/2**，无跳过。
覆盖 1023/1024/1025B、1 MiB、16 MiB 逆序与重复分片的全部载荷字节；两话题/两发布者
相同序号交错；Collecting 与 CommitPending 冲突；真实 SHM 提交后才 ACK；有限重试、
不确定终结、NACK 分组、窗口绕回、大首序号、回执跨窗口及过期重放、低序号在途保活，
以及全局/peer/route/回执/stream/待提交配额与析构回收。故障输入固定，无随机故障种子。

首轮发现新序号会误命中旧槽位，已修为先检查最高序号；销毁顺序保证缓冲和位图先释放。
元数据由 assemblies、streams、receipts 条目上限与消息/窗口大小共同约束，位图字节可查询。

本卡是数据状态机验证。权限由后续目录层签发；T08/T09 必须让路由关闭与 SHM 提交在
所属 shard 排序，atomic active 本身不提供跨线程提交屏障。网络端到端及跨机性能尚未验收。
