# W02 结构闸 fixture（t50/R-1）

用途：把 `test/w02_summary_schema_check.py` 的**动态半**（逐行 gate ↔ 名点、gate 取值域、
表头列数 ↔ 数据行列数）变成**秒级可跑**的判据，以便进 CTest。
⛔ 它**不是**一次运行，也**不跑** 75-case 矩阵 —— 只是 3 行固化样本。

| 文件 | 角色 | 期望 |
|---|---|---|
| `summary_ok.csv` | 正控：1 个正常 case + 1 个 `gate=0` 且名点非空的 case | **通过**（若不通过 ⇒ 闸自身坏了，假红） |
| `summary_inject_f.csv` | 注入 F：某行 `gate=0` 而 `failure_reasons` 空 | **转红** |
| `summary_inject_g.csv` | 注入 G：某行 gate 取值 `2`（越界） | **转红** |
| `summary_inject_a.csv` | 注入 A：表头多一列而数据行未跟上（t47 的实际形态） | **转红** |
| `results_ok.json` | 与 `summary_ok.csv` 对应的最小 `results.json`（当前判据只读 summary.csv；留作扩展） | — |

复现：

```bash
python3 test/w02_summary_schema_check.py --fixtures   # 期望 rc=0，且三条注入均打印「转红（被拦）」
```

⚠️ 三条注入是**闸的有效性证据**：它们证明 F/G/A 三个形态**确实会被拦**，而不是"闸写了却不拦"。
