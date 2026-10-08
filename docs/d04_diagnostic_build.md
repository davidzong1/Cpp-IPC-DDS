# D04 基线诊断构建

父版本为 `f066a82c1b4b10ea28322f86042bf6da5e122318`。本工作区独立于
`/tmp/dzipc-root-cause-baseline`，不覆盖冻结的 `build-latency-formal`。

补丁仅加入与 B 同编号的发布、接收和 wait-set 阻塞起止诊断钩子。
保留原 wait-set 的容器、扫描、阻塞、通知和返回协议，不移植 B 的协作接收。
关闭钩子时不读取时钟；诊断开启时的耗时属于 L1/L2，不作为正式成绩。

消息头由本版本 `generator/batch_msg_srv_generator.py` 生成。StdImage 头文件的
SHA256 与 B 相同；采集 manifest 保存头文件、生成器及编译命令的哈希。
两侧编译同一份 `/tmp/dzipc-local-latency-root-cause/test/shared_net/benchmark.cc`。
A 使用 `IPC_SHM`，不启动网关。

构建参数：`RelWithDebInfo`、`LIBIPC_BUILD_TESTS=OFF`、
`LIBIPC_BUILD_PYTHON=OFF`、`LIBIPC_BUILD_DEMOS=OFF`、
`UPDATA_MSG_SRV_GENERATOR=OFF`，以及显式的
`DZIPC_DIAGNOSTIC_BENCHMARK` 文件路径。实际编译选项以保存的
`build-d04-diagnostic/compile_commands.json` 为准。
