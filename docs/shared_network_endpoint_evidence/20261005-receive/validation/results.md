# O07接收分段验证

- 共享CTest36/36，ASan/UBSan（detect_leaks=1）public_api/teardown 2/2。
- 汇总器单元检查3/3：缺失/乱序拒绝；recv可早于发布开始；端到端最慢1%同一批消息的分段守恒。
- baseline-smoke与sanitize-smoke各2秒、100Hz、64B、1订阅者；每个窗口200次正式接收，无丢失/错误/重复；trace_missing_or_unordered=0、trace_overflow=0。
- 基线只提供recv返回点，新增点为未采样；候选四个点全部非零且顺序成立。冒烟不是性能结论。

命令：`ctest --test-dir build-shared-net -R shared_net --output-on-failure`；`ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-shared-net-sanitize -R 'shared_net_(public_api|teardown)$' --output-on-failure`；`python3 -m unittest discover -s test/shared_net -p test_receive_summary.py -v`。

冒烟调用`test/shared_net/benchmark.py --receive-trace --subscribers 1 --bytes 64 --seconds 2 --rate 100`，分别使用`/tmp/dzipc-receive-baseline/shared_net_benchmark --mode baseline`和`build-shared-net-sanitize/bin/shared_net_benchmark --mode shared_v1`；网关均为`build-shared-net/bin/dzipc_gateway`，真实加载库映射在result.json。
