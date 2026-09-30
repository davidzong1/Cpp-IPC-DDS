# W02 现行 run（t47 收口）—— `20260928-r30-W02`

> 本目录是 **W02 的现行证据 run**（`manifest.currently_citable` 指向它）。
> 生成命令见 `command.txt`；原始控制台见 `console.log`；中间轮次（r24…r29）留痕见 `superseded/`。
> 自评等级 **S3**；⛔ S4 归 t14/R1 主体（**t25 的 PASS 已被 t14/R1 撤回**，本目录按 t47 收口重出）。

## 关键读数（自检）
- 75/75 `case_ok`、`valid_rx_count=75/75`、`child_killed_total=0`、`sample_count=67500`
- 路径：['cyclonedds-iox', 'cyclonedds-udp', 'dzflat-a', 'dzflat-b', 'tlv']
- 尺寸档：[64, 1024, 65536, 1048576]
- 5 个 gate 触发：{'late_ok': 0, 'backlog_ok': 0, 'send_blocked_ok': 0, 'abnormal_ok': 0, 'bad_header_ok': 0}
- `bad_header` 非零 case：0（修前 B 档 686–1210/case，且当时该量**不落列**）
- 样本表：`files=75 rows=67500 cells=269788 mismatches=0 skipped_rows=53`（恒等式 `cells == 4×(rows−skipped_rows)` 由脚本自校验）

## 内容
| 文件/目录 | 内容 |
|---|---|
| `manifest.json` | 运行元数据 + **`currently_citable`（唯一权威指针）** + `superseded_runs.non_current_runs`（8 条，逐条「非现行 + 原因」）+ `failure_thresholds`（6 阈值 + `cli_overridden` + `gates_in_judgement` + `informational_only`）+ `evidence_discipline`（三条纪律） |
| `summary.csv` | **93 列**：新增 `bad_header` 与 5 个 gate 布尔（`late_ok/backlog_ok/send_blocked_ok/abnormal_ok/bad_header_ok`） |
| `results.json` | 逐 case 判定（含 `gates` 子对象）；`counters.json` / `path_evidence.json` / `config.json`（含阈值副本）/ `environment.json` / `topology.json` / `field_schema.json` |
| `samples/` | 逐样本表 + 两角色原始表 + `.dropped.txt` + `.counters.json`（540 文件） |
| `stdout/`、`stderr/`、`verdict.md` | 角色进程日志与判定表（含 gate 列 + 「判定来源」声明 + A/B 可分性与阈值说明） |
| `f3_before_after.md` | **修复前后对照**（失败量是否进判定）+ 阈值依据与敏感度表 |
| `f0_reference_audit.md` | **引用链审计**（r19/r20/r23/r30 的 cells/skipped 口径 + 权威指针） |
| `f3_negctl/`、`f3_negctl_late/` | 两条负控原始输出（阈值压到 0 ⇒ `rc=1`、gate 判失败） |
| `superseded/` | 中间轮次 r24…r29 的控制台日志 |

## 窗口声明
- 每 case `duration=2 s`（1 MiB 档自动降 500 Hz）+ `warmup=0.5 s`；静默窗 3 s（`--smoke-dds`）
- ⛔ 单次运行、无置信区间 ⇒ 不得作为 W11 的正式性能结论

## 复现
```bash
cmake -S . -B build && cmake --build build --target xproc_benchmark test_w02_xproc_benchmark -j8
# RouDi 必须在**同一 shell 会话**内启动（否则 cyc-iox 档会被显式剔除，见 manifest.dds_precheck_skipped）
/home/zwc/branch/lejurobot-core/src/common/dds/3rd_party/bin/x86_64/iox-roudi &
./build/bin/xproc_benchmark --smoke-dds --run-id=<新 run_id> --out-dir=<新目录> \
    --dds-uri-udp=file://$PWD/test/w02_dds_udp.xml --dds-uri-iox=file://$PWD/test/w02_dds_iox.xml
python3 test/w02_sample_audit.py <run_dir>
python3 test/w02_summary_schema_check.py <run_dir>
```
