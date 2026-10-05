# T09：本机直达与 best-effort 数据路径

基线 `f9977cf`，本记录与 T09 实现同提交。实现及本机隔离环境验证完成；物理跨机验收待采集。

- 固定 K 个数据 shard 独占数据 socket、重组、发送队列和 SHM 提交。源出站只分片发送远端，
  不经过本机 bridge；接收端同一远端消息只重组/注入一次。批量 IO 只推进成功前缀。
- 不可变目录与 bridge 引用通过有界命令同步到 shard。退订响应等待全部 shard 的旧工作结束；
  新 bridge 初始化等待前次屏障，避免旧提交跨越业务通道重建。临时 peer 离线不清去重历史。
- PublisherEndpoint 先执行本机腿，再独立提交 DZTX。健康零需求与网关离线均保留直接写入路径；
  不因网络失败撤销本机结果，不在本机失败后自动补投。prebuilt 的返回遵循两腿接管边界。
- 发送记录、目标、命令/事件的条目和字节均有上限；释放 WireBlob 后才返网络信用。
  reliable 发送仍明确拒绝，T10 接入 ACK/NACK 与重传，不将 best-effort 接管宣称为可靠交付。
- shared_net_probe 输出两腿结果、序号及校验和；接收时重建期望载荷并比较全部字节。

验证命令（退出码均为 0）：

```sh
cmake --build build-shared-net --target test_shared_net_partial_submit shared_net_probe dzipc_gateway test_shared_net_client_lifecycle test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
python3 test/shared_net/end_to_end.py --gateway /home/zwc/cpp_ipc_dds/build-shared-net/bin/dzipc_gateway --probe /home/zwc/cpp_ipc_dds/build-shared-net/bin/shared_net_probe
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(partial_submit|reassembly|dedup|quota|end_to_end)' --output-on-failure
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

新增 **4 条 GTest + 1 个多进程集成驱动**，共享 CTest **21/21**，ASan/UBSan **5/5**，OFF **2/2**，无跳过。
共享回归首轮使用了旧 ReceiveAdmission 布局的测试二进制，重建受影响目标后通过；旧 status 断言
从 ControlReady 更新为数据面 Ready。所有最终计数对应重新构建后的运行。

集成驱动使用两套独立用户/挂载/IPC 命名空间，各挂载独占 tmpfs 到自己的 /dev/shm；两个真实网关
经 lo 上 UDP 双向通信，四个独立应用进程订阅。共 12 条源消息、48 次订阅交付，覆盖
64/1023/1024/1025/4096/1048576B，每次校验完整载荷及 CRC；额外读取确认没有重复/回流。
每个网关实际 UDP FD 数为 **6**，每个应用为 **0**；每端远端注入计数为 6，源出站注入为 0。
SIGSTOP 网关并等待客户端健康超时后，原发布进程仍向两个原本机订阅进程各提交一次正确消息。
仅清理测试命名空间、临时目录及其自己创建的进程组。故障种子固定为 20261005。

部分提交测试覆盖 16 种两腿组合、本机池耗尽但网络接管、本机成功但 GatewayLost、可靠返回失败
而本机消息保留、零超时、无效 prebuilt 不可见、健康零远端不创建出站记录。

这不是两台物理主机的网络、网卡或时钟验收。T09 的物理跨机退出项与 T14 的跨机性能项仍待外部
测试环境；不阻止在已验证的数据路径上继续实现 T10～T13。默认公共工厂仍不选择 shared_v1。
