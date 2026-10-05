# 复跑入口

```bash
cmake --build build-shared-net --target ipc dzipc_gateway shared_net_probe test_shared_net_wire -j 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
c++ -O2 -g -std=c++17 test/shared_net/crc_benchmark.cc -Iinclude -Lbuild-shared-net/lib -Wl,-rpath,"$PWD/build-shared-net/lib" -lipc -pthread -o /tmp/dzipc-crc-benchmark
LD_LIBRARY_PATH=/tmp/dzipc-crc-before-2b90e28 /tmp/dzipc-crc-benchmark
LD_LIBRARY_PATH="$PWD/build-shared-net/lib" /tmp/dzipc-crc-benchmark
python3 test/shared_net/crc_matrix.py --before-library /tmp/dzipc-crc-before-2b90e28 --after-library "$PWD/build-shared-net/lib" --gateway "$PWD/build-shared-net/bin/dzipc_gateway" --probe "$PWD/build-shared-net/bin/shared_net_probe" --output /新的空目录
```

旧库在CRC修改前从2b90e28构建目录复制，SHA256与上一轮正式54窗口指纹一致；不是另行构建的无指纹基线。工具实际加载路径由每窗口/proc映射复核。恢复环境时可用2b90e28独立构建旧库，但新编译器/参数会改变指纹，应作为新一次试验记录。

每次采样都保持库和可执行文件不变，不同时构建或运行其他压力测试。网络12窗口源码为9039231；保存全部成功/失败轮次。基准驱动只清理自己的临时控制目录、进程与IPC/mount命名空间。
