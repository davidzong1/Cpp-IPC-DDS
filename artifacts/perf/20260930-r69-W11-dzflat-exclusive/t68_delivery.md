# t68 交付清单（W11 repair round 2）

修复者：验证与性能负责人（W11/t12 实现者本人；⛔ 本目录**不是**复审，复审归 t69）
队长裁定：blocker 采**方案 A（重跑）**

## 新 run（blocker 重跑）
- `artifacts/perf/20260930-r69-W11-dzflat-exclusive/`：DZFlat 6 轮**真独占窗口**
  - `rounds.tsv`（逐轮 rc/wall/loadavg_before/after/birth_epoch/end_epoch/samples）
  - `fingerprint.txt`（库+工装双指纹）
  - `manifest.json`（含 `round_windows`）
  - `C_dzflat_ab.md`（三实验组分列 + 轮间离散度 + 可比配对）
  - `OLD_vs_NEW_batch.md`（旧批 vs 新批逐格对照）
  - `t53_after_evidence/counters.json`（接线后四量现值）

## 旧 run（保留不动，作沿革）
- `artifacts/perf/20260930-r49-W11/`：63 run + `runs_annotated.tsv`（15 列，含 domain 回填）+ `identity_check.json`
- `artifacts/perf/20260930-r49-W11-dzflat/`：旧 DZFlat 批（与 state1 重叠 21 s，**已由新批取代**，不删）

## 新增/修改脚本
- `test/perf/w11/w11_dzflat_pair.sh`：ROOT 绝对化（修双路径缺陷）+ 逐轮 rounds.tsv
- `test/perf/w11/w11_cpu_scan_sweep.sh`：runs.tsv 加 la_after + domain + 表头
- `test/perf/w11/w11_manifest.py`：manifest 加 `domains` 与 `round_windows`
- `test/perf/w11/w11_identity_check.py`（新）：恒等式两口径可复算
- `test/perf/w11/w11_rate_claim_check.py`（新）：倍数声明检测器（排除禁令语境 + 阳性探针自证）
