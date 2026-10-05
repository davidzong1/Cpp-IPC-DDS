# O07接收分段验证

- 共享CTest36/36，ASan/UBSan（detect_leaks=1）public_api/teardown 2/2。
- 汇总器单元检查3/3：缺失/乱序拒绝；recv可早于发布开始；端到端最慢1%同一批消息的分段守恒。
- baseline-smoke与sanitize-smoke各2秒、100Hz、64B、1订阅者；每个窗口200次正式接收，无丢失/错误/重复；trace_missing_or_unordered=0、trace_overflow=0。
- 基线只提供recv返回点，新增点为未采样；候选四个点全部非零且顺序成立。冒烟不是性能结论。

命令：`ctest --test-dir build-shared-net -R shared_net --output-on-failure`；`ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-shared-net-sanitize -R 'shared_net_(public_api|teardown)$' --output-on-failure`；`python3 -m unittest discover -s test/shared_net -p test_receive_summary.py -v`。

冒烟调用`test/shared_net/benchmark.py --receive-trace --subscribers 1 --bytes 64 --seconds 2 --rate 100`，分别使用`/tmp/dzipc-receive-baseline/shared_net_benchmark --mode baseline`和`build-shared-net-sanitize/bin/shared_net_benchmark --mode shared_v1`；网关均为`build-shared-net/bin/dzipc_gateway`，真实加载库映射在result.json。

## 采样后追加回归

OFF构建及CTest2/2通过。新增的四个旧SHM聚焦目标中，`test_dzflat_transport`与`test_dzflat_rx`通过；`test_shm_i5_pop_buffer`（3个断言用例失败）和`test_shm_sub_dtor_gate`（1个用例失败）未通过，原始输出为legacy-receive.log。

使用同一当前测试可执行文件、独立IPC/mount命名空间、`LD_LIBRARY_PATH=/tmp/dzipc-crc-before-2b90e28`加载保留旧库，两个目标同样失败，见legacy-2b90e28.log。旧库SHA256与O04正式矩阵和CRC前后对照清单一致，动态加载器核对`libipc.so.6`解析到该目录。失败名称也在既有[MPMC交付记录](../../../shm_multi_publisher_execution_plan.md)中列出。本轮不将历史失败改判通过，也不声称完整旧路径回归全绿。各指纹见legacy-comparison.json。

比较命令：`unshare --user --map-root-user --mount --ipc bash -c 'mount -t tmpfs -o size=1g tmpfs /dev/shm && env -u DZIPC_NET_BACKEND LD_LIBRARY_PATH=/tmp/dzipc-crc-before-2b90e28 ctest --test-dir build-shared-net -R "^test_(shm_i5_pop_buffer|shm_sub_dtor_gate)$" --output-on-failure'`。当前四目标使用相同命名空间命令，去掉LD_LIBRARY_PATH并在正则中增加`dzflat_transport|dzflat_rx`。

最初也用已安装库做过探测（legacy-before.log），失败目标相同；该安装库指纹与正式CRC库不同，所以正式版本归因只使用已核对指纹的2b90e28比较。
