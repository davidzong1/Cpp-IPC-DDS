# W02-F0 引用链审计（t47 验收；现行 run = `20260928-r30-W02`）

## 1) 交付面 run 引用分布

| 文件 | `20260928-rNN-W02` 出现次数 |
|---|---|
| `README.md` | r19×1, r30×3 |
| `W02_中间轮次留痕与r18去向.md` | r19×1, r20×1, r23×5, r30×2 |
| `W02_统一跨进程基准_结果格式与使用.md` | r30×4 |

⇒ 现行 run 的引用一律出现在「`currently_citable`」语境；旧 run 一律带「非现行 + 原因」。

## 2) `cells` / `skipped_rows` 逐 run 对照（本轮我全部用脚本重跑）

| run | 状态 | cells | skipped_rows | 口径/来源 |
|---|---|---|---|---|
| `r19` | **非现行**（t25 合同引用的数字） | 269896 | 26 | 只跳「时间戳字段为空」⇒ 判据偏窄 |
| `r20` | **非现行**（t25 的复核对象） | 269848 | 38 | 行口径（首个规范值） |
| `r23` | **非现行**（F3 修复前一代） | 269852 | 37 | 行口径 |
| `r30` | **现行**（`currently_citable`） | 269788 | 53 | 行口径 |

**`skipped` 的口径差异只有一个来源**：行判据 = `ao<td ∪ 任一派生列为空`。
- 只跳「时间戳字段为空」⇒ r19 = 26（**t25 合同引用的口径**）；
- 覆盖 `ao<td` 这一类 ⇒ r20 = 38 / r23 = 37 / r30 = 53（**行口径，规范**）。
- ⛔ `cells` **不是常量**：逐 run 波动（同一 run 内恒定，由恒等式 `cells == 4×(rows−skipped_rows)` 自校验）。

本轮脚本重跑（⛔ 不是引用 t39/R7 的数字）：
```
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r19-W02
  files=75 rows=67500 cells=269896 mismatches=0 skipped_rows=26
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r20-W02
  files=75 rows=67500 cells=269848 mismatches=0 skipped_rows=38
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r23-W02
  files=75 rows=67500 cells=269852 mismatches=0 skipped_rows=37
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r30-W02
  files=75 rows=67500 cells=269788 mismatches=0 skipped_rows=53
```

## 3) 单一权威指针（机器可读）

- `manifest.currently_citable` = `20260928-r30-W02`
- `manifest.run_identity` = `{"this_run_id": "20260928-r30-W02", "purpose": "evidence", "this_run_is_evidence": true, "citation_authority": "20260928-r30-W02"}`
- `manifest.superseded_runs.non_current_runs`（8 条，逐条 status+reason）：
  - `20260928-r19-W02` — *_messages 因 D-17 计数双写作废（约 2× 且回退场景下把「期望路径」记成「实际路径」）；cells=269896/skipped=26 是 t25 合同引用的数字
  - `20260928-r20-W02` — W02-F2(wire null 表示) 修复前对照；cells=269848/skipped=38
  - `20260928-r23-W02` — F3 修复（失败量进判定 + bad_header 补列）之前的一代；其 summary.csv 缺 bad_header 与 5 个 gate 列，⛔ 不得据此判「失败量已被判定
  - `20260928-r24-W02` — 首次带 F3 判定的冒烟轮：RouDi 不在线 ⇒ cyc-iox 被显式剔除（仅 60 case）；且当时 B 档预热帧形状缺陷已被本列暴露出来（bad_header 1194–
  - `20260928-r25-W02` — backlog 预算取 5 周期档 ⇒ 1 case 因真实排队（5.171 周期）判失败；该轮用于确定默认预算，不作为结论
  - `20260928-r26-W02` — late 预算取 5% 档 ⇒ 1 case（40/500 迟发）判失败；该轮用于确定默认预算
  - `20260928-r27-W02` — 同 r28 的矩阵与判定（75/75），但 manifest 尚缺 failure_thresholds.informational_only
  - `20260928-r28-W02` — t47 收口前一轮（含 informational_only）；最终可引用 run 见 currently_citable

纪律（同时写入 README 使用约定与 `manifest.evidence_discipline.current_run_authority`）：
> **任何引用必须是 `currently_citable` 指向的 run**；引用旧 run 必须同时标「非现行 + 原因」。
