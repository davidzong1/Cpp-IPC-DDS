# t47 验收对照（W02 修复：撤回 PASS 后的收口）

| 验收项 | 状态 | 证据 |
|---|---|---|
| 全仓 W02 引用统一指向现行 run | ✅ | `manifest.currently_citable=20260928-r30-W02`；文档引用审计见 `f0_reference_audit.md`（r19/r20/r23 仅出现在「非现行 + 原因」语境） |
| `currently_citable`/`superseded_run_ids` 齐备并写入 README 使用约定 | ✅ | manifest 两字段 + `run_identity` + `evidence_discipline`；README §5 新增「任何引用必须是 currently_citable」一行 |
| F3 四个失败量进判定通路 | ✅ | `summary.csv` 的 `late_ok/backlog_ok/send_blocked_ok/abnormal_ok`（0 case 触发失败） |
| `bad_header` 补列 | ✅ | `summary.csv:bad_header` + `bad_header_ok`；本 run 非零 0 case（修前 B 档 686–1210/case 且不落列） |
| 阈值入 manifest | ✅ | `manifest.failure_thresholds`：6 阈值 + `cli_overridden` + `gates_in_judgement` + `informational_only` |
| 前后对照 | ✅ | `f3_before_after.md`（r23 17/75 全漏判 → r30 逐 case 可判；三层证据：人工负控 / 真实负载 40 处名点 / 默认档 0 触发） |
| C)② A/B 可分性更正 | ✅ | 《结果格式与使用》§7.1：A/B 消费者侧**同形**（各 `via_view=13500`、`via_object=0`）⇒ 可分性只在发布侧 |
| B)⑥ 两套基准更正（含队长错误撤回） | ✅ | 《结果格式与使用》§7.3 + README §5bis：W02 `xproc_benchmark`（合并计数）vs W08 `test_w08_dzflat_ab`（A/B 分列）；跨包引用规则 4 条 |
| 三条纪律写入交付 | ✅ | `manifest.evidence_discipline` + README §5 表 + 《结果格式与使用》§11.6 |
| `ctest -j4` 全绿 | ✅ | 28/28 通过（含新 `test_w02_summary_schema` 结构闸） |

## 关键读数

- 用例：75/75 `case_ok`；`valid_rx_count=75/75`；`child_killed_total=0`；`sample_count=67500`
- 路径 × 尺寸档：['cyclonedds-iox', 'cyclonedds-udp', 'dzflat-a', 'dzflat-b', 'tlv'] × [64, 1024, 65536, 1048576]
- 5 gate 触发计数：{'late_ok': 0, 'backlog_ok': 0, 'send_blocked_ok': 0, 'abnormal_ok': 0, 'bad_header_ok': 0}
- 样本表：files=75 rows=67500 cells=269788 mismatches=0 skipped_rows=53
- summary 结构闸：OK  : 表头列数 == 格式串字段数 == 93
- 二进制/库指纹：exe `7d77172381ad3ce4…` / lib `cf209393773d51ee…`（与 build/ 现值一致）

## 边界

- ⛔ 未改 `counters.h`/`src/libipc/**`/他人 WP；⛔ 未覆盖 r19/r20/r23（只追加新 run_id `20260928-r30-W02`）。
- ⛔ 本工作包仍只主张 **S3**；S4 归 t14/R1 主体的**增量** verdict（⛔ 不重开 t11）。
