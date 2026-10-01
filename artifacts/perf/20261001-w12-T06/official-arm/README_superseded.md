# ⚠️ 本目录是**卫生修正前**的早期单臂读数，正式读数见 ../official-matrix/

保留原因：它记录了「跨臂 chunk 池段污染」现象的原始形态与最小复现三连。

| 子目录 | 库 sha16 | rc | bad_rounds | 说明 |
|---|---|---|---|---|
| `POLDEMO-V2NEG` | `3380016ba38084d8` | 1 | 1 | 负控臂，在段里制造重复 id |
| `POLDEMO-BASELINE-polluted` | `4fc0e95dc316ac01` | 1 | 1 | 不清段 ⇒ 继承重复 id（同库！） |
| `POLDEMO-BASELINE-clean` | `4fc0e95dc316ac01` | 0 | 0 | 清本用例专属前缀池段 ⇒ 全绿 |
| `BASELINE-probe1/2`、`FIXNEG-timing1/2` | — | — | — | 早期对照与 FIXNEG 首轮命中的重复确认 |

机械根因、段链读数与处置：`../diag/arm_pollution.md`。
收集窗口：2026-10-01 21:45:09 → 21:52 +0800。
