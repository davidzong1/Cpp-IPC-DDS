# N06 网络 v2 codec 与目录

N06 新增显式 v2 codec 入口，v1 `encode_*`/`decode_*` 和既有 golden vectors 保持原字节布局。v2 DATA 头为 176B，新增 source/target endpoint epoch；HELLO 仍为 64B 但版本为 2，base/shards 为零、capabilities 为 3；catalog 版本为 2；目录条目固定区扩展为 64B，带 data port、endpoint flags 和 endpoint epoch。v1 和 v2 入口不会互相接受对方报文。

验证命令和结果：

```text
python3 test/shared_net/v2_vectors.py                         # passed
build-shared-net/bin/test_shared_net_wire                  # 8/8
build-shared-net/bin/test_shared_net_endpoint              # 6/6
ctest --test-dir build-shared-net -L shared_net --output-on-failure -j 1 # 36/36
```

完整构建退出码为 0。v1 golden vector、CRC、目录角色和所有 shared_net 端到端/可靠测试均通过；v2 负例覆盖版本错配、零 endpoint epoch、非法 endpoint flags、长度和 CRC。真实网关仍只启动 v1，v2 codec 在 N07 端点生命周期完成前不作为生产模式暴露。
