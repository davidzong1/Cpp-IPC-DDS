# T05：出站 SHM 原子接管

前置提交 `90b53b7`，本记录与 T05 实现一起提交。网络业务路由尚未接入，公共工厂和默认模式不变。

## 原子性结论

- 每会话一个独占 `ipc::route` sender。ATTACH_TX 传非阻塞 eventfd；初始化线程建立 receiver，drain 注册后回 TX_READY。
- DZTX 直接编码到 loan，实际容量按 C(112+n) 扣费；接收严格裁掉容量填充，仅将 blob 的 W(n) 字节容量复制到私有缓存。
- 新增 `try_publish_loan`，队列满或接收者正常离开时不调用 force_push，false 对应本次记录未入队。
  256 个槽的信用窗口加上每档 10 块池，均由真实队列测试验证；满队列的第 257 次底层严格提交失败，前 256 条全字节保持。
- 修正既有 publish_loan 时序：`published` 标记移到槽位发布前回调内；可见之后不再按旧 chunk id 修改元数据。
  测试通过持有真实 RD 等待器锁，使接收者先消费、归还，再让发布返回，随后验证完整 10 块池容量。
- 出站专用收发跳过 RD/WT 等待器通知，只用 eventfd 和 TX_PROGRESS，实际持锁测试确认不会等待旧通知锁。
- 新增严格单 loan 接收，区分空队列与已经出队但取样失败，拒绝内部小包/分片路径。
- 异常发生在可能可见的边界时采用 Indeterminate，封住会话与本地 loan 归还，不以 false 允许回退。

## 信用与生命周期

- 两类账本独立。TX_PROGRESS 在 loan 释放后发布；私有 WireBlob 释放后才完成网络额度结算。
  `OutboxRecord` 用 RAII 保证最后释放缓存再结算；关闭会话先回收未用授予，在途余额随缓存释放回收。
- 编码和借样不持提交锁，短锁仅保护提交及账本；控制循环不能在 used 入账前应用提前到达的进度。
- 初始零信用、异步补充、旧/重复通知、会话/全局上限和关闭余额通过测试。
  有信用的 reliable 直接提交 DZTX，状态计数证明没有 CREDIT_REQUEST；DZLC 15/16 继续拒绝。
- 固定一个初始化线程和一个 drain 线程；有界队列，轮次上限 64 条 / 64 KiB / 200 us。
  达到预算保留 deferred 标志，不依赖下一条 eventfd 通知；最后一轮及时输出进度。
- SIGSTOP 后提交、SIGKILL 网关、重新启动验证旧可靠等待者 GatewayLost，新 epoch 使用全新段名且不读取旧队列。
  填充中杀应用进程，接收端始终看不到半条记录。

## 验证记录

```sh
cmake --build build-shared-net --target test_shared_net_outbox test_shared_net_outbox_failure --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R test_shared_net_outbox --output-on-failure
ctest --test-dir build-shared-net -R 'test_shm_mpmc_(channel|types|control_plane)$|test_shm_multi_publisher$|test_dzflat_(rx|fallback_semantics)$' --output-on-failure
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

- 新增 **18 条 GTest**，包括 221 个长度/档位边界（最大 16 MiB）、200 次真实跨进程完整记录、8 个发布线程共 240 条全字节核验。
- 共享 CTest **12/12**；新增 ASan/UBSan **2/2**（全部 18 条）；旧 MPMC/DZFlat 回归 **6/6**；后端 OFF **2/2**。
- 初轮夹具误用非 DZS2 scope 被协议拒绝，已改成合法独立测试身份；没有放松实现校验。
- 旧 `AppendedCountersDoNotShiftExistingIds` 断言仍期待 66，`e887d5e` 已追加三项 hybrid 计数；本卡同步为 69，
  并固定三项位于 66/67/68。生产计数器没有变动。首次构建命令误用了不存在的生命周期测试名，已改用实际注册目标。

## 未越过的边界

当前网关只证明接收出站记录及结算，尚未做跨机转发；可靠请求明确返回 Rejected，不能当作远端成功。
T06/T11 仍须合并本机腿后验证公共 prebuilt false 的回退边界；T09/T10 接入真正的 shard 与网络完成事件。
本卡没有声称性能目标或真实跨机验收已通过。SHM 档位的映射保留量仍与活跃信用占用分开核算。
