# T11 公共接口与兼容

基于 f884f38；新后端仅在显式 `DZIPC_NET_BACKEND=shared_v1`、MPMC 和构建条件满足时接入 `IPC_SOCKET`。默认 legacy、IPC_SHM、IPC_SOCKET_ONLY 及服务工厂的分支保持原样。

实现全部发布接口；订阅采用 REGISTER_SUB → reader generation 连接 → SUB_READY。重置先撤销旧登记，失败后封闭整个句柄；旧 getter 被唤醒退出。发布遇到正在重置的独占锁立即失败，不在锁后重新开始完整超时。普通编码遵循 DZFlat 开关，预构造段仍直接传送。

公共类型未新增虚函数或旧基类成员；`connected_generation()` 为非虚访问器。修正 Sample 前置声明的命名空间，使公共头单独包含时签名与既有 dzIPC::Sample 一致。该结论是源码与构建检查，不宣称跨编译器 ABI 保证。构造失败清理工厂 pimpl，无缺网关 FD 泄漏。

验证：

- 新增 8 条 GTest：四种 publish、Sample/clone/等待、重置取消与失败封闭、GenericMessage 与生成 StdImage、无效段单次回退、离线本机已提交不回退、旧 transport 无网关可构造。
- 全共享 CTest 29/29；旧 MPMC、控制调度、RouteSession、UDP 端口回归 7/7。
- ASan/UBSan（含 leak 检查）公共 API/prebuilt/compatibility 3/3；关闭共享构建 2/2。
- Python 3.10 实际加载新 Release 扩展，公共工厂预构造 336B 段逐字节一致，坏段返回 false 后普通 TLV 只接收一次，2/2。命令如下。

```bash
cmake --build build-shared-net-python --target _dzipc_core dzipc_gateway --parallel 4
unshare --user --map-root-user --mount --ipc bash -c 'mount -t tmpfs -o size=768m tmpfs /dev/shm && /usr/bin/python3.10 test/shared_net/public_python.py --gateway "$PWD/build-shared-net-python/bin/dzipc_gateway"'
```

仅使用测试独占 IPC/mount 命名空间和临时目录，未清理其他业务段。仅出站接管/未知提交的合并逻辑由 T05/T09 测试继续覆盖；真实崩溃与并发关闭在 T12 补齐。
