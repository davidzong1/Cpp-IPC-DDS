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
