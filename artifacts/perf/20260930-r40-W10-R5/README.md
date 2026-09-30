# W10-R5 §10.2 与三态复验 —— **定版**证据目录（run_id `20260930-r40-W10-R5`）

> 任务 t38｜负责人：验证与性能负责人｜交付报告：`docs/消息接收架构改造/团队改造交付/W10/R5_扫描与三态复验.md`
> 工装/库指纹**冻结**（见 `frozen_fingerprint.txt`）；每个 run 的 manifest 内含 `lib_sha256`/`tool_sha256` 自证。

## 结果

| 项 | 读数 |
|---|---|
| 批跑 | **`W10_R5_SWEEP_OK runs=72`** + B 档 diag=off **8/8** ⇒ 定版共 **80 run** |
| 异常轮 | **0/80**（第一代 r38 的 1/72 见 `abnormal_round_audit_from_r38.md`） |
| 库漂移 | **无**（`lib_drift.txt` 不存在） |
| 逐 run 指纹 | 全部同一组合：`lib=558f47ed… tool=25bef0ab…` |

## 内容

| 目录/文件 | 内容 |
|---|---|
| `A-*/` | 主对照：state 3 × n(1/100/500/1000) × diag(off/on) × **5 轮** |
| `B-*/` | 三态补齐：state 1/2 × n(1/100/500/1000) × diag(on/off) |
| `C-*/` | 有效消息负载：state 3 + 持续发布 × n × diag × 2 轮 |
| `D-*/` | worker 对照：W(4/8/16/32) × n=1000 × 2 轮（W=4 为容量受限档） |
| `minimal/` | 方案 §6.2 最小验证集八用例（6 通过 / 2 如实登记 N/A） |
| `tidcpu/` | **外部**逐 TID CPU 采样（含观测器开销与进程级交叉核对） |
| `summary.tsv` | 逐 run 退出码/判定/必需文件 |
| `matrix_summary.tsv` | 聚合：逐 run 配置 + 五项派生值 + 三态必录 + 指纹列 |
| `derived_summary.md` | 派生值按格的均值与极值 |
| `null_audit.txt` | 所有 `null` 及其原因（⛔ 零分母/未采集不写 0） |
| `frozen_fingerprint.txt` | 冻结的库/工装/源文件 sha256 |
| `lib_drift.txt` | （应不存在）库指纹跨 run 漂移记录 |

## 每 run 的产物
`manifest.json`（含诊断开关/端口策略/判据版本/期限/容量模型/指纹）、`phases.csv`（阶段化）、
`windows.csv`（五项派生值 + 门禁）、`windows.jsonl`、`counters.json`、`threestate.json`（三态必录 + 逐 route 确认/恢复首包）、`verdict.md`。

## 一键复跑
```bash
bash test/perf/w10/build_harnesses.sh                      # 重编（含 mtime 配对检查）
bash test/perf/w10/w10_r5_sweep.sh <new_root> --window-s 60 --rounds 5
python3 test/perf/w10/w10_r5_aggregate.py <new_root>        # 聚合
./build/bin/w10_r5_minimal --out <dir>                      # 最小验证集
```
