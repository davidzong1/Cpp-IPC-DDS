# t37 / W10-R3 最小反例集（可执行对拍）

## 内容
| 文件 | 说明 |
|---|---|
| `counterexamples.txt` / `run.log` | `w10_r3_counterexamples` 的完整输出（**修前/修后对拍**，H1/H2/H3/H4/H6/F6） |
| `h1_buggy_parse.py` | H1 的**逐字复现脚本**：按修前逻辑解析 `/proc/net/udp`，打印实际 `used` 集合 |
| `h1_buggy_parse.txt` | 上面脚本的输出（实测 `used = [0], |used| = 1`） |

## 一键复跑
```bash
bash test/perf/w10/build_harnesses.sh          # 重编工装并与库配对（F4）
./build/bin/w10_r3_counterexamples --out <dir> # H1–H6/F6 对拍
python3 test/perf/w10/w10_r3_counterexamples.cpp  # ← 不是脚本；解析脚本见下方
python3 <dir>/h1_buggy_parse.py                    # H1 的独立复现
bash test/perf/w10/w10_r3_batch_acceptance.sh <work_root>   # H5 批次失败传播验收
```
