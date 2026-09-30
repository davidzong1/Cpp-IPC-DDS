# run_id `20260929-r41-W10-R2b` — t41 证据目录（只追加）

任务：t41（t19 窗口开启：调用点补丁应用 + registration_rejected 语义裁定）
交付文档：`docs/消息接收架构改造/团队改造交付/W10/R2b_t19窗口开启与rejected语义.md`

| 文件 | 内容 |
|---|---|
| `fingerprint_before.txt` | 改动前后四组 sha256（产品源 ×2 + counters.h + libipc.so） |
| `t19_callsite_cc.patch` / `_header.patch` 副本 | 见 W09/证据/（本包**原样应用未重生成**） |
| `t41_semantics_arms.cpp` | 三语义同臂探针源码（ARM1..ARM5） |
| `arms_after.out.txt` / `arms_x5.out.txt` | 同臂读数（单轮 + 5 轮逐字一致） |
| `no_behavior_change.txt` | 「仅计数器点、无行为变更」A–F 六条机械判据 |
| `check_wiring.sh` / `check_wiring.out.txt` | 接线机械核对（5 组，PASS） |
| `rejected_coverage.txt` | `registration_rejected` 无剩余语义空间的机械覆盖表 |
| `ctest_after.log` | 全量回归 27/27 Passed |

判定：**S3（已接入 + 单项验证）**。⛔ 不主张 S4（独立验收归 R1）。
