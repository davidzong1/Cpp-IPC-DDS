# 本机延迟根因测试执行台账

## D00

- A：`f066a82c1b4b10ea28322f86042bf6da5e122318`，`/tmp/dzipc-root-cause-baseline`，baseline/IPC_SHM。
- B：`94856f7bcef69e054ce679420dd8b7ee8e6b0c1e`，`/tmp/dzipc-local-latency-root-cause`，shared_v1/本机 MPMC。
- C：尚不存在。
- 本批证据：`docs/shared_network_endpoint_evidence/20261007-local-root-cause/`。
- 工作区、版本和环境清单见 `plan.json`、`provenance.json`、`environment.json`。
- 当前只使用独立工作区，不改主工作区和整合交付工作区。

## 计划

D01 先完成双版本构建、工装自检和能力矩阵；D02 运行三失败场景的 8 窗 ABBA|BAAB L0 同期对照。采样、解码和回归串行执行。

## 开放项

真实正式 54 窗、生产候选 C、D10-D13 尚未执行；三项失败的根因需由新证据确认。

## D01

- B 配置/构建：`cmake -S . -B build-latency-formal -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDZIPC_BUILD_SHARED_NET=ON -DLIBIPC_BUILD_TESTS=ON -DLIBIPC_BUILD_PYTHON=OFF -DLIBIPC_BUILD_DEMOS=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF`，构建退出码 0。
- A 生产库构建：冻结 A 不含 shared-net；首次全量 CMake 生成因忽略的 `test/perf/w10/w10_rebuild_crash.cpp` 缺失而失败，随后以 `LIBIPC_BUILD_TESTS=OFF` 构建 `ipc` 目标成功。A 的同一 benchmark 工装使用 A 头文件/生成消息头和 A `libipc.so` 手工链接，未复制 B 生产代码。
- 解析器自检：现有 `test/shared_net/test_*.py` 10 项全部通过；能力清单见 `tool-validation/capability.json`。
- 六个 L0 冒烟：`tool-validation/d01-smoke-002/`，A/B 各覆盖 1 SUB/4KiB、32 SUB/64B、1 SUB/1MiB，3 秒正式窗口、100 Hz、2 秒预热；6/6 退出码 0，发布/接收完整、无重复/损坏/丢失，业务进程 UDP FD 为 0。中断的首次尝试 `d01-smoke-001/` 保留，未计入通过样本。
- 运行器：`test/shared_net/run_local_latency_experiments.py`，串行窗口、20 秒静置检查、每窗哈希和环境快照；静置检测只能排除超过 1 秒 CPU 的可见进程，不能声称系统绝对空闲。

D02 进入条件满足：A/B 双侧工装完整性成立，开始三失败场景的 24 窗 L0 `ABBA|BAAB` 对照。
