# T06：本机直达与远端原始注入

基线 `f7b5c6a`，本记录与 T06 实现同提交。

- ShmWireWriter 按应用发布对象持有 MPMC 发布端；ShmWireBridge 使用同一生命周期但隐藏内部诊断登记。
  通过非虚适配复用 shm_pub_ipc；没有改动公共虚表或该类对象尺寸。
- TLV 和 DZFlat 原样进入业务 SHM，不携带 DZTX/DZMX，不经过 nodelet 扇出或业务反序列化。
  原始完整 TLV 的逻辑长度不是 loan 容量，新增 `publish_loan_size` 验证同一尺寸档后按真实长度提交，保留原页尾。
- 本机 DZFlat 可以直接写入 loan；测试验证一次编码、零 TLV 序列化。编码失败和异常返回 NotSubmitted，不自动换编码重投。
  正常无接收者为 NotRequired，提交异常为 Indeterminate 并封住 writer；上层保持两腿结果。
- 关闭用生命周期锁等在途调用退出；PID 检查先于旧锁。已收到的 Sample/loan 保持原池租约。

## 验证

```sh
cmake --build build-shared-net --target test_shared_net_shm_bridge test_shared_net_local_direct --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(shm_bridge|local_direct)$' --output-on-failure
ctest --test-dir build-shared-net -R 'test_shm_multi_publisher$|test_dzflat_rx$' --output-on-failure
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

新增 **8 条 GTest**，共享 CTest **14/14**，新增 ASan/UBSan **2/2**，旧路径 **2/2**，后端 OFF **2/2**。

覆盖：TLV 跨页与 DZFlat 全字节核验；普通 SHM 发布者、writer、bridge 共用一个话题，两个进程各收到一次；
三个真实 MPMC 发布槽只展示两个业务发布者；退出 bridge 后其他发布者继续发送；持样跨发布者退出；
错误 schema、无接收者、错误逻辑长度/尺寸档、普通订阅者两类编码接收、本机成功与网络失败的返回合并；
编码返回失败/抛异常、并发关闭、池耗尽和 fork 后旧对象拒绝。

首轮混合测试将旧 publish_prebuilt 的 loan 容量误当成新接口的真实长度，已改为按段头与完整有效载荷核验，
保留旧 API 行为；新 writer/bridge 的精确长度仍单独断言。测试只清理独占话题与自己的子进程。

跨进程完整名称、类型冲突、PUB/SUB 注册和 Ready 事务属于 T08；本卡验证传入描述和载荷的一致性。
尚未接入网关网络数据流、公共共享工厂或性能验收，不能据此宣称跨机交付完成。
