# W10/t36 更正 run：run_id 20260929-r26-W10-correction
# 目的：F2 的 2×2 反事实独立复现 + F4 的 ABI 复现 + F3/F5/F6 证据读取纪律
# 性质：**只追加**，⛔ 不改 r25 任何文件、不改 t11 终态

库指纹  e1c2a7e99f8bd2e8dd05b393c05d9fe00e35c640efccbe7a8dfbbf991dc174c4
w10_idle  065270467000148a59ecfa097a3df17a81df3272f081f5e384113483dea5bb7b
w10_matrix 1876ad2c538ebc2183703ed3f0508fae31151874ce2809013a3290adeec5e13a

## 目录内容（本 run 自身）

| 文件 | 用途 |
|---|---|
| `attribution.txt` | 2×2 反事实（A/C/B/D 四格）原始输出 |
| `A_schON_poolON.tid.txt` / `.proc.log` | A 格逐 TID 明细 + 进程内读数 |
| `C_schON_poolOFF.tid.txt` / `.proc.log` | **C 格（池整体停用）** 逐 TID 明细 + 进程内读数 |
| `A_rep1..3.tid.txt` / `C_rep1..3.tid.txt` | A vs C 三轮重复 |
| `state2_100/500/1000.log`、`state2_scale.txt` | 决定性标定：池完全不参与时的 CPU-项数曲线 |
| `state3_perthread.tid.txt` / `.proc.log` | state3 逐 TID 明细（r25 缺，本次补齐；`*.proc.log` 是进程内对照读数） |
| `F2_repro_summary.txt` | F2 复现汇总（含 A/C 三轮、state2 三档、B/D 差异登记） |

## 附：本更正的判定依据（三句话）
1. F2：C 格（池整体停用）CPU 0.6025–0.6180 ≥ A 格（池 ON）0.5775–0.5925，3/3 轮，唯一非零 TID 恒为调度器线程；
   且 state2（池完全不参与、仅 2 线程）1000 项即 0.293 core，state3(entries=2000) ≈ 0.293×2 ⇒ 主成本在控制面。
2. F3：§13.2#6 五类资源只测了 route/fd 两类 ⇒ 该条改判「不可判定（部分测量）」。
3. F5：6/6 臂的 .log 与目录 first_packet_us 相差 643.8 s，且 .log 均含 ARTIFACT_DIR_NOT_EMPTY ⇒ 复算以目录为准。
