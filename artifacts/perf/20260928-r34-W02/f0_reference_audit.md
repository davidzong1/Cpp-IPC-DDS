# W02-F0 引用链审计（t50 复核，现行 run = `20260928-r34-W02`）

## 1) 交付面 run 引用分布

| 文件 | `20260928-rNN-W02` 出现次数 |
|---|---|
| `README.md` | r19×1, r34×3 |
| `W02_中间轮次留痕与r18去向.md` | r19×1, r20×1, r23×5, r34×2 |
| `W02_统一跨进程基准_结果格式与使用.md` | r34×4 |
| `t50_R1_结构闸动态半登记_落地回告.md` |  |
| `t50_R1_结构闸动态半登记需求.md` |  |
| `t50_R5_新字段规格提交W03.md` |  |

⇒ 现行 run 的引用一律在「`currently_citable`」语境；旧 run 一律带「非现行 + 原因」。

## 2) `cells` / `skipped_rows` 逐 run 对照（本轮全部用脚本重跑）

| run | 状态 | cells | skipped_rows |
|---|---|---|---|
| `r19` | 非现行 | 269896 | 26 |
| `r20` | 非现行 | 269848 | 38 |
| `r23` | 非现行 | 269852 | 37 |
| `r34` | **现行** | 269868 | 33 |

**口径差异唯一来源**：行判据 = `ao<td ∪ 任一派生列为空`。只跳「时间戳空」⇒ r19=26（t25 合同引用的口径）；
覆盖 `ao<td` ⇒ r20=38 / r23=37 / r34=33。⛔ `cells` **不是常量**（逐 run 波动），恒等式 `cells == 4×(rows−skipped_rows)` 才是判据。

脚本重跑输出：
```
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r19-W02
  files=75 rows=67500 cells=269896 mismatches=0 skipped_rows=26
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r20-W02
  files=75 rows=67500 cells=269848 mismatches=0 skipped_rows=38
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r23-W02
  files=75 rows=67500 cells=269852 mismatches=0 skipped_rows=37
$ python3 test/w02_sample_audit.py artifacts/perf/20260928-r34-W02
  files=75 rows=67500 cells=269868 mismatches=0 skipped_rows=33
```

## 3) 权威指针（t50/R-3 后的 `run_identity`，7 键）

- `currently_citable` = `20260928-r34-W02`
- `run_identity` = `{"this_run_id": "20260928-r34-W02", "purpose": "evidence", "purpose_explicit": true, "this_run_is_evidence": true, "authority_undeclared": false, "claims_authority": true, "citation_authority": "20260928-r34-W02"}`
- `superseded_run_ids` 共 **32** 条；`non_current_runs` 共 **13** 条（逐条 status+reason）
- `evidence_discipline.authority_claim_rule` = 认领权威是**显式动作**：只有 `--purpose=evidence` 才会让 currently_citable 指向本 run；不带参数（或 --purpose=verification/experiment 且未给 --citat…

三条纪律（t47）+ 认领规则（t50/R-3）均已写入 manifest 与文档（README §5、格式文档 §11.55/§11.6）。
