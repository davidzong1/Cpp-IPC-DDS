# t50 验收对照（t48 五条残余收口）

现行 run：**`20260928-r34-W02`**（75/75 case_ok、valid_rx 75/75、child_killed=0、sample_count=67500）。

| # | 残余 | 本轮动作 | 状态（⛔ 不得写成"已解决"，见末列） | 证据 |
|---|---|---|---|---|
| R-3 | 不带参数会抢权威指针；纪律未成文 | 默认改为**安全**：缺 `--purpose` ⇒ 按 verification 处理且 `currently_citable=null` + `authority_undeclared=true`；认领权威改为**显式动作**（仅 `--purpose=evidence`）；非法值 rc=2 拒绝；纪律写入 README §5 + 格式文档 §11.55（含两条可机械核对判据） | ✅ 闭合 | `t50_R3_authority_probe.md`（四组探针）+ `manifest.run_identity` 7 键 + `evidence_discipline.authority_claim_rule` |
| R-1 | 结构闸动态半未进 ctest | 动态半做成 `--fixtures`（3 行固化样本 + **三条注入 F/G/A**，毫秒级）；登记需求精确 add_test 提交架构负责人并**已落地**；`ctest -N` **28→29**，`-j4` 全绿 | ✅ 闭合（登记已落地 + 我独立复核） | `t50_R1_结构闸动态半登记需求.md`、`.._落地回告.md`、`test/fixtures/w02_summary_schema/`、`ctest #24/#25` |
| R-2 | 证据归档依赖 `tmp/` | `w02_r24/r25/r26.out` **迁入现行 run 的 `superseded/`**，文档引用改正；原件保留；**依赖声明**成文（该档位不可按需复现：复核者 3 轮 + 4 路并发未复现） | ⚠️ **闭合但带依赖条件**（见右） | `r2_archive_note.md` + `superseded/w02_r24.out`（sha256 与 R1 副本逐字一致） |
| R-4 | `bad_header` 下沿 532 无实物 | 全量清点（r19/r20/r23 共 45 条 dzflat-b 记录）⇒ 实测 **686–1210/case**；`532` 出现 **0** 次；文档全部改为实测值并附复现命令 | ✅ 闭合（数字改为实测） | `r4_bad_header_inventory.txt` |
| R-5 | 新字段未进 `field_schema.json` | 规格提交 W03（`t50_R5_新字段规格提交W03.md`）；W03 已落地（第三类 `run_level_fields`，13 条，owner=W02/registered_by=W03）；本 run 的 schema **已含新段**并逐字段核对一致 | ✅ 闭合（待 R1 增量 verdict 确认） | `r5_schema_recheck.md` + 本 run `field_schema.json`（run_level_fields=13） |

## 两条中性观察

| 观察 | 处理 |
|---|---|
| `summary.csv:late` ↔ `results.json:late_sends` 命名不一致 | **不新增重复列**，而是机器可读声明映射：`manifest.field_aliases`（`late`/`late_threshold`/`bad_header`/`gate_results`，并注「同一来源，不是两个独立来源」）；文档 §6 同步 |
| f3 的「40 处名点」实为 72（迟发 40 + 积压 32） | `f3_before_after.md` 改为**按量分列**表（迟发 40 / 积压 32 / 合计 72，涉及 23 个 case，13 个两类同时亮） |

## 关键读数（现行 r34）

- 用例：75/75 `case_ok`；`valid_rx_count=75/75`；`child_killed_total=0`；`sample_count=67500`
- 5 gate 触发：{'late_ok': 0, 'backlog_ok': 0, 'send_blocked_ok': 0, 'abnormal_ok': 0, 'bad_header_ok': 0}
- `run_identity`：`{"this_run_id": "20260928-r34-W02", "purpose": "evidence", "purpose_explicit": true, "this_run_is_evidence": true, "authority_undeclared": false, "claims_authority": true, "citation_authority": "20260928-r34-W02"}`
- 样本表：files=75 rows=67500 cells=269868 mismatches=0 skipped_rows=33
- 结构闸：OK  : 表头列数 == 格式串字段数 == 93
- 动态半：`--fixtures` ⇒ rc=0（三条注入全部转红）
- schema：`run_level_fields=13`，6 列 + 5 键逐字段命中

## ⛔ 仍未闭合 / 不得外推

- **R-1 的 fixture 未被 git 跟踪**（架构负责人新登记）：若工作区被清理且未提交，`test_w02_summary_schema_dynamic` 会因缺 fixture 失败（实测**明确失败、不静默通过** ⇒ 失效方向安全）。需 W12 把 `test/fixtures/`、`test/w02_summary_schema_check.py`、`test/w02_sample_audit.py` 并入提交快照。
- R-1/R-5 的最终闭合仍待 **R1 增量 verdict**；本文件只登记"已落地 + 本包自测 + 我的独立复核"。
- R-2 的层② 依赖归档件；`r31`/`r32` 是**高负载**下 gate 亮起的实例，⛔ 不得据此判 DDS 1 MiB 档性能不达标。
