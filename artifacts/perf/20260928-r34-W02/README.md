# W02 现行 run（t50 收口）—— `20260928-r34-W02`

> 本目录是 **W02 的现行证据 run**（`manifest.currently_citable` 指向它）。
> 生成命令见 `command.txt`（含 `--purpose=evidence`）；原始控制台见 `console.log`；中间轮次（r24…r33）留痕见 `superseded/`。
> 自评等级 **S3**；⛔ S4 归 t14/R1 主体（t25 的 PASS 已被撤回；t48 判 W02 收口项闭合，本 run 是 t50 收口后的现行 run）。

## 关键读数（自检）
- 75/75 `case_ok`、`valid_rx_count=75/75`、`child_killed_total=0`、`sample_count=67500`
- 5 gate 触发：{'late_ok': 0, 'backlog_ok': 0, 'send_blocked_ok': 0, 'abnormal_ok': 0, 'bad_header_ok': 0}
- `run_identity`：`{"this_run_id": "20260928-r34-W02", "purpose": "evidence", "purpose_explicit": true, "this_run_is_evidence": true, "authority_undeclared": false, "claims_authority": true, "citation_authority": "20260928-r34-W02"}`
- 样本表：`files=75 rows=67500 cells=269868 mismatches=0 skipped_rows=33`
- `field_schema.json`：`run_level_fields=13`（W03 已登记 6 列 + 5 个 manifest 键，逐字段核对一致）

## 内容
| 文件/目录 | 内容 |
|---|---|
| `manifest.json` | `currently_citable`（唯一权威指针）+ `run_identity`（7 键，含 `purpose_explicit`/`authority_undeclared`/`claims_authority`）+ `superseded_runs`（32 个 run 编号 / 13 条 `non_current_runs` 逐条原因）+ `failure_thresholds` + `field_aliases` + `evidence_discipline`（三条纪律 + **认领规则**） |
| `summary.csv` | **93 列**（含 `bad_header` 与 5 个 gate 布尔） |
| `results.json` | 逐 case 判定（含 `bad_header` 与 `gates` 子对象） |
| `samples/`、`stdout/`、`stderr/`、`verdict.md` | 逐样本表、角色日志、判定表（含 gate 列 + 「判定来源」声明） |
| `f3_before_after.md` | 失败量进判定的前后对照 + 三层证据（含**按量分列**的 72 处名点）+ 依赖声明 |
| `f0_reference_audit.md` | 引用链审计（逐 run cells/skipped + 权威指针） |
| `r2_archive_note.md` | R-2：`tmp/` 迁出记录 + 与 R1 副本的 sha256 对照 + **依赖声明** |
| `r4_bad_header_inventory.txt` | R-4：`bad_header` 全量清点（实测 **686–1210/case**；`532` 出现 0 次） |
| `r5_schema_recheck.md` | R-5：与 W03 `field_schema.json` 的逐字段核对 |
| `t50_R3_authority_probe.md` | R-3：权威指针的四组探针与前后对照 |
| `t50_acceptance.md` | t50 五条残余的逐条对照（⛔ 含未闭合项） |
| `f3_negctl/` | 人工负控（阈值→0 ⇒ 两 gate 判亮、rc=1） |
| `superseded/` | 中间轮次控制台（r24–r33，含 R-2 的归档件 `w02_r24/r25/r26.out`） |

## 窗口声明
- 每 case `duration=2 s`（1 MiB 档自动降 500 Hz）+ `warmup=0.5 s`；静默窗 3 s
- ⛔ 单次运行、无置信区间 ⇒ 不得作为 W11 的正式性能结论；`r31`/`r32`（高负载轮）不得作为性能证据

## 复现
```bash
cmake -S . -B build && cmake --build build --target xproc_benchmark test_w02_xproc_benchmark -j8
/home/zwc/branch/lejurobot-core/src/common/dds/3rd_party/bin/x86_64/iox-roudi &   # 必须同一 shell 会话
./build/bin/xproc_benchmark --smoke-dds --run-id=<新 run_id> --out-dir=<新目录> --purpose=evidence \
    --dds-uri-udp=file://$PWD/test/w02_dds_udp.xml --dds-uri-iox=file://$PWD/test/w02_dds_iox.xml
# ⛔ 复核轮改用：--purpose=verification --citation-authority=<证据 run>（否则 currently_citable=null）
python3 test/w02_sample_audit.py <run_dir>
python3 test/w02_summary_schema_check.py <run_dir>        # 静态半 + 动态半
python3 test/w02_summary_schema_check.py --fixtures       # 三条注入必须转红
```
