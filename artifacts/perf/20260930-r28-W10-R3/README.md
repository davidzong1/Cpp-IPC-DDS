# t37 / W10-R3 交付证据目录（run_id `20260930-r28-W10-R3`）

> 任务：t37（W10-R3 验收工装修正，H1–H6 + F4/F6）｜负责人：验证与性能负责人
> 目的：让工装从「会给出错误结论」变为**可靠裁判**，先于任何正式千路采集。
> 逐项说明见 `docs/消息接收架构改造/团队改造交付/W10/R3_工装修正.md`。

## 内容

| 目录/文件 | 内容 |
|---|---|
| `counterexamples/` | **H1/H2/H3/H4/H6/F6 的修前/修后对拍**（`w10_r3_counterexamples` 输出 + H1 独立复现脚本与输出） |
| `acceptance/` | **H5 批跑失败传播验收**（注入失败 ⇒ 批次非零退出；rc=124/139/verdict=FAIL/缺文件五类边界） |
| `f4_abi/` | **F4 反例**：用【旧头】编译工装并链接【新库】⇒ 可编译可链接、运行时 SIGSEGV（含 gdb 栈） |
| `fingerprint_harness.txt` | **库 ↔ 工装成对指纹**（引用任何读数时必须成对引用） |
| `source_fingerprint.txt` | `w10_matrix.cpp` 的 sha256 |
| `regress/` | 修后回归：`bash test/perf/w10/w10_r3_regress.sh` ⇒ SHM 1/1000、广播 32、hotcold（含 F6 的 sent 列 1000/1000 非零）、socket 1 **全部 PASS** |
| `final/` | 收口读数：SHM 千路 ×2 + **socket 千路**（`verdict_assertions c1..c5=1`） |
| `negctl/` | **H2 负控**：不建发布端 ⇒ `confirmed=0/20`、`registered=0/20`、rc=1（修前会宣称 20/20） |
| `portev/` | **H1 落盘物**：`port_table.txt`（原始 /proc 表）+ `port_parse.txt`（解析结果与错误） |

## 一键复跑
```bash
bash test/perf/w10/build_harnesses.sh                 # 重编全部工装（含库/工装 mtime 配对检查）
bash test/perf/w10/build_harnesses.sh --check-only    # 只做配对检查（不配对即非零退出）
./build/bin/w10_r3_counterexamples --out <dir>        # H1–H6/F6 对拍
bash test/perf/w10/w10_r3_batch_acceptance.sh <work_root>   # H5 批次失败传播验收
bash test/perf/w10/w10_run_matrix.sh <run_root> --run-id-prefix <p>   # 参数化批跑（失败会传播）
```
