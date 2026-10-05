# 指标与延迟修复验证

O01为1f9a520；O02为本目录验证记录所属的实现提交。均在仓库根执行，不使用团队MCP。全部共享目标曾完整重建（CircularQueue、RouteAdmission、OutboxRecord等布局改变，不能复用旧对象文件）。

```bash
cmake -S . -B build-shared-net
cmake --build build-shared-net -j 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
build-shared-net/bin/test_shared_net_metrics --gtest_output=xml:docs/shared_network_endpoint_evidence/20261005-optimization/validation/metrics.xml
cmake -S . -B build-shared-net-sanitize
# 已有配置：Debug，-fsanitize=address,undefined -fno-omit-frame-pointer
cmake --build build-shared-net-sanitize --target test_shared_net_metrics test_shared_net_reassembly test_shared_net_quota test_shared_net_public_api test_shared_net_failure test_shared_net_restart test_shared_net_teardown test_shared_net_outbox test_shared_net_credit test_shared_net_discovery -j 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R '^test_shared_net_(metrics|reassembly|quota|public_api|failure|restart|teardown|outbox|credit|discovery)$' --output-on-failure
# OFF、旧路径、安装及Python执行入口沿用T15的commands.md，均对本轮源码重新构建。
python3 test/shared_net/end_to_end.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --reliable
c++ -O2 -std=c++17 -fPIC -shared test/shared_net/faults.cc -ldl -o /tmp/dzipc-opt-faults.so
LD_PRELOAD=/tmp/dzipc-opt-faults.so DZIPC_TEST_FAULT_MODE=mixed DZIPC_TEST_FAULT_REPORT_DIR="$PWD/docs/shared_network_endpoint_evidence/20261005-optimization/validation/fault-mixed" python3 test/shared_net/end_to_end.py --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --reliable
```

新增单记录信用等待测试初版只降低session_send_records，未同步降低publisher_reliable，被配置层正确拒绝（credit-fixture-initial.log）。调整测试配额层级后8条指标测试及sanitizer均通过，未放宽生产配置校验。

这些功能/故障测试允许与构建并行，因此其耗时直方图用于证明实际采样与语义，不作空载性能结论。正式性能窗口另设独占输出目录，运行前冻结二进制，期间不重建、不并行压测。
