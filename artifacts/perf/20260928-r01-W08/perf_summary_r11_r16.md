# W08 跨进程 A/B/TLV 逐样本汇总 —— 新一版 run（r11..r16）

> **本表是 D-12 证据污染收口后的当前有效来源。** 表头字段与 W03 `field_schema.h` 同源；
> 旧表（r05..r10）所依据的 `samples.csv` 已于 2026-09-28 20:22:4x 被就地覆盖，**旧表已失效**；
> 覆盖前的派生记录保留在 `perf_summary.json`（19:57），仅作交叉核对。

口径：`transport_ns = transport_done − publish_enter`；`app_read_ns = fully_consumed − app_obtained`；
`e2e_ns = fully_consumed − produced`（跨进程 CLOCK_MONOTONIC）；单位 µs。⛔ 单轮，非 W11 结论。

| run_id | transport | variant | size | n | transport 中位 | transport p95 | delivery 中位 | app_read 中位 | e2e 中位 | 污染前同名 run 的 transport 中位 |
|---|---|---|---|---|---|---|---|---|---|---|
| 20260928-r11-W08-tlv-prebuilt | tlv | prebuilt | img66k | 40 | 11.0 | 12.0 | 12.3 | 14.5 | 67.9 | 6.6 |
| 20260928-r11-W08-tlv-prebuilt | tlv | prebuilt | img262k | 40 | 30.5 | 54.4 | 34.3 | 53.2 | 239.9 | 17.2 |
| 20260928-r11-W08-tlv-prebuilt | tlv | prebuilt | img880k | 40 | 61.1 | 65.6 | 91.0 | 186.7 | 504.0 | 62.7 |
| 20260928-r12-W08-tlv-permsg | tlv | permsg | img66k | 40 | 11.0 | 16.3 | 12.2 | 14.6 | 69.6 | 6.3 |
| 20260928-r12-W08-tlv-permsg | tlv | permsg | img262k | 40 | 17.4 | 32.4 | 27.7 | 53.1 | 149.9 | 17.1 |
| 20260928-r12-W08-tlv-permsg | tlv | permsg | img880k | 40 | 180.1 | 191.8 | 61.4 | 185.2 | 726.3 | 179.8 |
| 20260928-r13-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img66k | 40 | 3.9 | 8.0 | 4.0 | 14.4 | 34.1 | 3.8 |
| 20260928-r13-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img262k | 40 | 7.0 | 7.3 | 4.0 | 53.0 | 111.4 | 7.0 |
| 20260928-r13-W08-dzflat-a-prebuilt | dzflat-a | prebuilt | img880k | 40 | 20.7 | 23.3 | 4.2 | 184.3 | 375.1 | 20.2 |
| 20260928-r14-W08-dzflat-a-permsg | dzflat-a | permsg | img66k | 40 | 6.6 | 11.1 | 4.0 | 14.6 | 57.5 | 3.8 |
| 20260928-r14-W08-dzflat-a-permsg | dzflat-a | permsg | img262k | 40 | 16.7 | 18.0 | 4.3 | 53.1 | 200.9 | 6.9 |
| 20260928-r14-W08-dzflat-a-permsg | dzflat-a | permsg | img880k | 40 | 20.3 | 21.4 | 4.7 | 184.7 | 384.5 | 20.1 |
| 20260928-r15-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img66k | 40 | 2.2 | 2.4 | 3.7 | 14.6 | 36.4 | 1.4 |
| 20260928-r15-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img262k | 40 | 2.2 | 2.7 | 3.9 | 53.2 | 121.9 | 1.4 |
| 20260928-r15-W08-dzflat-b-prebuilt | dzflat-b | prebuilt | img880k | 40 | 2.4 | 3.5 | 4.0 | 186.2 | 408.6 | 1.4 |
| 20260928-r16-W08-dzflat-b-permsg | dzflat-b | permsg | img66k | 40 | 1.3 | 2.4 | 3.8 | 14.6 | 31.8 | 1.4 |
| 20260928-r16-W08-dzflat-b-permsg | dzflat-b | permsg | img262k | 40 | 1.3 | 1.5 | 3.9 | 53.0 | 104.7 | 1.4 |
| 20260928-r16-W08-dzflat-b-permsg | dzflat-b | permsg | img880k | 40 | 1.3 | 1.4 | 4.3 | 187.6 | 357.8 | 1.4 |

## 每行校验

- 6 个 run 的样本数(=120)、`path` 标记、`payload_checksum_ok=1`、`dropped=0` 全部通过。
- 问题清单：无

## 机制读数（大档 img880k，`permsg`，中位数 µs）

- 发布路径：TLV 180.1 → A 20.3 → **B 1.3**。A→B 的差 ≈ 19.0 µs，即「对象 → chunk 的那一跳 0.88 MB 拷贝」。
- 生产到消费 e2e：TLV 726.3 → A 384.5 → **B 357.8**（B vs A ≈ -26.7 µs）。
- **B 的 transport 三档基本恒定**（与载荷大小无关）是「省掉最后一次拷贝」的签名；A 随载荷线性增长。完整读取（app_read）三档基本相等 ⇒ 消费者工作量对齐。
- ⛔ 不得写成“B 级提升 N 倍”：A→B 在 transport 上 ≈15.1× 只是省掉一次拷贝；e2e 上 B 相对 A 只有 -6.9% 量级。
- ⛔ 与同进程探针（113.8/151.6 µs，docs/dzflat_shm.md §9.5）不可换算或互推。
