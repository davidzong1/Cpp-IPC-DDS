# W10-R7 独立复核证据目录（t39 / 基准负责人；**非实现者**）

- run_id：`20260930-r46-W10-R7`（只追加；⛔ 未覆盖 r25/r27/r28/r38/r40/r41/r42/r44/r45 任一既有 run）
- 边界：⛔ 未改产品代码、工装、他人 WP、既有证据；复核 + 只读重跑（新 domain/新 run_id）
- 无产品指纹变更 ⇒ 本目录不构成新的「库版本」证据；引用本目录数值时仍须同时给出**被复核 run** 的库/工装指纹

## 窗口声明（所有读数均须带）
- 千路重跑：阶段 `silence` = **3 s** 静默窗（与 r25/r27/r28 同口径；⛔ 与 R5 的 60 s 稳态**不可直接比较**）
- ctest：`-j32` × 3 轮，每轮 27 项
- 计数复算：静态（源码写入点）与运行期（五臂 / t41 臂读数）联合

## 内容
| 文件 | 内容 |
|---|---|
| `10_ledger_recompute.txt` | 9 个 run 的逐 route 台账独立重算（行数/唯一/registered/Σplanned/Σrx/dup/ooo/corrupt/timeout + tx_set==rx_set） |
| `11_payload_injectivity.txt` | 按 `fill_payload` 真实公式（kPayloadHeader=16）重算载荷，证明用到的 (route,seq) 对**两两不同** ⇒ 校验通过不可能来自别的 route/seq |
| `12_doc_ref_check.txt` | W10 文档引用的仓内路径机械核对（真缺文件 2 处） |
| `13_r40_matrix_recompute.txt` | r40 80 run 矩阵完整性（行↔目录差集 = ∅）、指纹一致性、confirm/recover 逐格 |
| `14_w4_thread_accounting.txt` | D-W4 容量受限档：residual==overflow==wait_set_full==492 与线程分账（4+1+1+492=498） |
| `15_run_summary_table.txt` | 9 run 汇总表（我重算） |
| `16_ctx_vs_frozen_threshold.txt` | state3 全部 40 格 ctx 对 R-1b 默认臂阈值 30480/s 的判定 |
| `17_fingerprint_pairing.txt` | 逐 run「库+工装」指纹配对可得性审计 |
| `21/22/23/30/31/34_*` | 计数接线复算（62 ID 三层分类、写入口径、具名 ID 抽查、与 t35/t43 的口径差异） |
| `24_aggregation_key_conflation.txt` | 派生值聚合键混淆的定量证据 |
| `32_worker_skew.txt` / `33_state3_conflict_evidence.txt` | 每 worker 归属数据可得性 / R-2b 锚点可得性 |
| `18_tlv_messages_reconcile.txt` | 发布侧路径计数 ↔ 逐 route 台账三方对账 |
| `25_h3_h6_and_sent_ok.txt` | H3 `first_packet_us` 语义 + `sent_ok` 列覆盖范围 |
| `26/27_*.txt` | 指纹缺口/线程成分/恢复不重新注册 |
| `30/31_*.tsv` | t35 只读脚本在**当前树**的原样重跑输出（audit / uniqueness） |
| `myrun/` | 我的独立重跑（SHM 1000 / socket 1000） |
| `arms/` | t35 五臂（A/B/C/D/E2）我在本轮重跑的原始输出 |
| `t41arms/` | t41 语义臂我**从源码重编**后的原始输出 |
| `ctest_j32/` | `-j32` × 3 轮原始日志 |
