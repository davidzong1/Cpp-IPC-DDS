# W11 串行配对实验 —— 独立结论（机器可判 + 人工复核）

run_root=artifacts/perf/20260930-r49-W11（DZFlat 批：artifacts/perf/20260930-r49-W11-dzflat/）

## 判定
- **控制面 CPU（组 A）**：**归一化形态自验通过**（k × 项数 × tick 率 = 实测，7 格最大偏差 1.23e-14% ⇒ 该形态是定义式）；但 **state3/n=1000 两档各 5/5 超 0.20**（W=32 → 1.41×，W=4 → 1.73×）⇒ **门槛 3 的 CPU 子项仍「待校准/未达标」**（⛔ 未改任何门槛数值）。
- **扫描成本（组 B）**：五项派生值与 t38/R5 同格差 ≤8%（本批系统性略低）；恒等式「均扫条目/轮 = n/W」在 n≥100 精确成立；**t44 结束值四量仍未接线（全 0，⛔ 不得读作「未发生」）**；扫描成本与控制面 CPU **分列**（本批 446×、t32 归一 3112×）。
- **DZFlat（组 C）**：§10.7 三实验组分列；B 的 transport 跨载荷**两模式均不动**（{≈0.9, ≈1.5} µs）；**⛔ 无倍数声明**，统一表述为「省掉一次 0.88 MB 拷贝」+ e2e B vs A ≈ −7.6%（0.88 MB permsg 档）。

## 失败项（方案 §14.2）
- `scan/scan-st3-n1000-W32-diagon-r5`：工装内部自洽门禁失败（d_scan_rounds 差 2、d_scanned_routes_total 差 68）；**归因 = 收尾快照非原子**（snap_gated → stats → snap_resident 三次独立读取，w10_r5_matrix.cpp:543/545），**非产品缺陷**；独立复跑 **3/3 自洽**。该 run 保留在目录内、汇总中单列剔除。

## 逐项文件引用
- CPU 逐 run（含 lib/tool 双指纹）：`A_cpu_tickrate_normalized.md`
- 扫描逐 run：`B_scan_cost.md`
- 运行记录/命令：`command_and_counts.txt`、`runs.tsv`、`fingerprint.txt`
- DZFlat：`../20260930-r49-W11-dzflat/C_dzflat_ab.md` + 各轮 `samples.csv`/`manifest.json`
- 口径自证：两个 `manifest.json` 的 `caliber` 段（冻结三元组 / 双口径不得互换 / t51 参考值）

## ⛔ 不可外推（摘要，详见报告 §5）
X-1 state2/n=1000 不可稳定测量（本批 5 轮全落低簇）；X-2 「每 tick 选中项数 7/23」未闭环；X-3 r31/r32 高负载 gate 亮起非性能结论；X-4 未接线计数的 0 不得读作「未发生」；X-5 ⛔ 不用 attached==n 代表规模（本批逐 route 确认 60/60 全满足）；X-6 工装崩溃不计入产品失败率；X-7 t44 四量未接线；X-8 两套基准不得互替、A/B 可分性只在发布侧。
