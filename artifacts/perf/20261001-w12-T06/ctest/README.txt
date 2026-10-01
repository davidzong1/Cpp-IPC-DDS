=== §8.3 要求命令的落盘（2026-10-01 21:54:34 +0800）
命令: ctest --test-dir build -R test_chunk_capacity_backpressure --output-on-failure
说明: ctest 注册的命令不带 LD_LIBRARY_PATH，故走二进制 RUNPATH=/home/zwc/cpp_ipc_dds/build/lib（产品库本体现值）。
      这是**测试环境**默认路径，不是本复核选择的臂；本复核的换库读数见 official-matrix/ 与 scenarios/。
