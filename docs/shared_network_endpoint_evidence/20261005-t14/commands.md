# 复跑入口

在仓库根执行。各驱动只创建自己的临时目录和 IPC/mount 命名空间；不得全局清理 /dev/shm。

```bash
cmake -S . -B build-shared-net -DDZIPC_BUILD_SHARED_NET=ON -DLIBIPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-shared-net --target dzipc_gateway shared_net_probe shared_net_benchmark --parallel 4
# 另构建 test/test_shared_net_*.cpp 对应的全部 target，再执行：
ctest --test-dir build-shared-net -L shared_net --output-on-failure
python3 test/shared_net/scale.py --help
python3 test/shared_net/end_to_end.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --reliable --local-subscribers 32
python3 test/shared_net/multipublisher.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --publishers 8
python3 test/shared_net/fanout.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --targets 8
python3 test/shared_net/fairness.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --same-shard --output /tmp/fairness-same.json
# 不同 shard 去掉 --same-shard；各独立执行三轮。
c++ -O2 -std=c++17 -fPIC -shared test/shared_net/faults.cc -ldl -o /tmp/dzipc-shared-faults.so
# 为每种模式创建独占输出目录，mode 为 loss1/loss5/mixed/burst：
# LD_PRELOAD=/tmp/dzipc-shared-faults.so DZIPC_TEST_FAULT_MODE=mixed DZIPC_TEST_FAULT_REPORT_DIR=/独占目录 python3 test/shared_net/end_to_end.py ... --reliable
python3 test/shared_net/performance_matrix.py --baseline /tmp/dzipc-baseline-f066a82/build/shared_net_benchmark --current "$PWD/build-shared-net/bin/shared_net_benchmark" --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --output /新的独占证据目录
python3 test/shared_net/summarize_performance.py /新的独占证据目录
```

实际已执行的参数以 performance/manifest.json、*-cases.json 为准；baseline benchmark 由同一份 test/shared_net/benchmark.cc 单独以 -O2 链接 f066a82 库，生成消息头也使用该提交版本。不能在正式窗口中重建库或并发执行压测。
