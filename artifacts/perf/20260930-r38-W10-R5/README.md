# W10-R5 §10.2 与三态复验 —— 证据目录（run_id 20260930-r38-W10-R5）

## 内容
| 目录/文件 | 内容 |
|---|---|
| A-*/ | 主对照：state 3 × n(1/100/500/1000) × diag(off/on) × 5 轮 |
| B-*/ | 三态补齐：state 1/2 × n(1/100/500/1000) × diag=on × 1 轮 |
| C-*/ | 有效消息负载：state 3 + 持续发布 × n × diag × 2 轮 |
| D-*/ | worker 对照（单独实验）：W(4/8/16/32) × n=1000 |
| RETRY-A-st3-n1000-diagoff-r*/ | 异常轮回填（F4 纪律）：见 R5 报告 §7 |
| controlplane/ | 控制面 CPU 单列（state 2、settle 6 s，仅控制面） |
| tidcpu/ | **外部逐 TID CPU 采样**（含观测器开销） |
| minimal/ | 方案 §6.2 最小验证集八用例 |
| summary.tsv | 逐 run 退出码/判定/必需文件 |
| matrix_summary.tsv | 逐 run 配置 + 五项派生值 + 三态必录（聚合器输出） |
| derived_summary.md | 派生值按格的均值与极值 |
| null_audit.txt | 所有 null 及其原因（⛔ 零分母不写 0） |
| fingerprint.txt | 采集起始的库/工装成对指纹 |
| RETRY-fingerprint.txt | 异常轮回填时的成对指纹 |

## 引用纪律
- 引用任何读数必须同时引用**库↔工装成对指纹**（R0-A1）；
- 每个 run 的 manifest.json 现含 `lib_sha256`/`tool_sha256` 与运行前后一致性自证；
- ⛔ 不得把 `scan_time_ns_total` 换算成 CPU core（墙钟区间累计）；CPU 见 tidcpu/。
