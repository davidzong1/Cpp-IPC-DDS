# W08 跨进程 A/B/TLV 逐样本汇总（单轮，非 W11 正式结论）

来源：`artifacts/perf/20260928-r0{5..10}-W08-*/samples.csv`（每 run 120 样本 = 3 尺寸档 × 40）。

口径：`transport_ns = transport_done - publish_enter`；`app_read_ns = fully_consumed - app_obtained`；
`e2e_ns = fully_consumed - produced`（跨进程 CLOCK_MONOTONIC）；单位 µs。

| run | transport | variant | size | n | transport 中位 | transport p95 | delivery 中位 | app_read 中位 | e2e 中位 |
|---|---|---|---|---|---|---|---|---|---|
| 20260928-r05-W08-tlv-prebuilt | tlv | prebuilt | img66k | 40 | 6.6 | 7.7 | 8.7 | 14.5 | 41.8 |
| 20260928-r05-W08-tlv-prebuilt | tlv | prebuilt | img262k | 40 | 17.2 | 23.2 | 21.5 | 55.1 | 142.4 |
| 20260928-r05-W08-tlv-prebuilt | tlv | prebuilt | img880k | 40 | 62.7 | 74.6 | 88.6 | 192.9 | 524.9 |
| 20260928-r06-W08-tlv-permsg | tlv | permsg | img66k | 40 | 6.3 | 6.4 | 8.1 | 14.5 | 41.3 |
| 20260928-r06-W08-tlv-permsg | tlv | permsg | img262k | 40 | 17.1 | 18.6 | 32.1 | 53.0 | 152.5 |
| 20260928-r06-W08-tlv-permsg | tlv | permsg | img880k | 40 | 179.8 | 187.0 | 82.8 | 185.9 | 744.6 |
| 20260928-r07-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img66k | 40 | 3.8 | 6.5 | 3.8 | 14.4 | 33.8 |
| 20260928-r07-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img262k | 40 | 7.0 | 7.7 | 4.2 | 53.0 | 112.2 |
| 20260928-r07-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img880k | 40 | 20.2 | 22.3 | 4.2 | 184.0 | 373.1 |
| 20260928-r08-W08-dzflat-a-permsg | dzflat-a | permsg | img66k | 40 | 3.8 | 9.2 | 3.8 | 14.5 | 34.4 |
| 20260928-r08-W08-dzflat-a-permsg | dzflat-a | permsg | img262k | 40 | 6.9 | 7.0 | 4.0 | 53.8 | 114.5 |
| 20260928-r08-W08-dzflat-a-permsg | dzflat-a | permsg | img880k | 40 | 20.1 | 25.3 | 4.5 | 184.2 | 384.5 |
| 20260928-r09-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img66k | 40 | 1.4 | 1.5 | 3.7 | 14.3 | 31.3 |
| 20260928-r09-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img262k | 40 | 1.4 | 1.6 | 4.1 | 53.0 | 105.0 |
| 20260928-r09-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img880k | 40 | 1.4 | 3.2 | 4.5 | 185.5 | 354.8 |
| 20260928-r10-W08-dzflat-b-permsg | dzflat-b | permsg | img66k | 40 | 1.4 | 2.3 | 4.4 | 14.5 | 31.9 |
| 20260928-r10-W08-dzflat-b-permsg | dzflat-b | permsg | img262k | 40 | 1.4 | 1.5 | 4.4 | 53.1 | 105.4 |
| 20260928-r10-W08-dzflat-b-permsg | dzflat-b | permsg | img880k | 40 | 1.4 | 1.5 | 4.8 | 186.0 | 355.2 |

## 每行校验

- 全部 6 run 的样本数、`path` 标记、`payload_checksum_ok=1`、`dropped=0` 与上表一致。
- 问题清单：无

## 读法（⛔ 不许当成 W11 结论）

- 本轮为**单轮**采样，且同机有并发负载（见 W08 交付的热闸注记）；
  W11 的正式结论需按 W00 §8 的 5 轮交替 + 中位数判定规则重采，且必须绑定已通过正确性验收的提交。
- 同进程的 151.6/165.5 vs 113.8/116.1 µs（docs/dzflat_shm.md §9.5）**不得**与本表混用。

## 机制读数（大档 img880k，中位数）

- **发布路径**：TLV 179.8 → A 20.1 → B 1.4 µs。A→B 的差就是「对象→chunk 那一跳 0.88 MB 拷贝」≈ 18.7 µs。
- **生产到消费 e2e**：TLV 744.6 → A 384.5 → B 355.2 µs（B vs A ≈ -29.3 µs，与上面那一跳同量级）；余下全是应用自己填/读载荷的时间。
- **完整读取（app_read）三档基本相等**（img880k 均 ≈185 µs）：这正是「B 可以避免复制，但应用填充/读取大载荷仍需时间」的直接读数。
- ⛔ **不得把它表述成「B 级提升 N 倍」**：`transport` 是**发布 API 内部**的耗时，A→B 在这里的比值（≈14.2×）只是「省掉了一次 0.88 MB 拷贝」，不是应用端到端收益；e2e 上 B 相对 A 只有 -7.6% 量级的差别。
- ⛔ 速度方向与同进程探针一致（B 1.4 µs 是**发布侧**读数），但同进程的绝对值（113.8/151.6 µs）含各自的构造与填载荷，⛔不可与本表换算或互推。
