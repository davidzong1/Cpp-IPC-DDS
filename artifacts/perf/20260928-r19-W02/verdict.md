# W02 统一跨进程基准 · 判定（verdict）

> run_id: `20260928-r19-W02`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `5cb75857fc4ba6eddff753c69b876119ba29717d295a145ac1e50eaa59d7da9d`；libipc sha256: `5b75538c05562a492e2759b80f1b7dca795f87df5673188f1d9a14737132b1fc`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=15.3019 syscall_ns_per_call=86.2745 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

## 1. 机器判定摘要

```
process_model : cross-process (pub/sub 均为 fork+exec 的新映像)
config_hash   : 19c46fe0c69ac096
payload_shapes: 64B(实际 63B + 头 32B), 1024B(实际 1021B + 头 32B), 65536B(实际 65533B + 头 32B), 1048576B(实际 1048573B + 头 32B)
cases         : 75 通过 75 / 未通过 0
```

## 2. 逐用例判定（每行引用原始文件）

| case | 路径 | 组 | 载荷 | 等待 | 计划/尝试/成功/失败 | 接收 | 丢失 | 重复 | 乱序 | 校验失败 | 迟发 | dzflat/回退 | 文件 | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | tlv | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r0_tlv_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r1_tlv_crc_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2210 | `samples/r1_tlv_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r2_tlv_full_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r2_tlv_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | tlv | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2210 | `samples/r3_tlv_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | tlv | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 4 | 0/2213 | `samples/r4_tlv_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r5_tlv_crc_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2210 | `samples/r5_tlv_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r6_tlv_crc_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2210 | `samples/r6_tlv_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r7_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2211 | `samples/r7_tlv_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r8_tlv_full_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 8 | 0/2212 | `samples/r8_tlv_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | tlv | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2131 | `samples/r9_tlv_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r10_tlv_crc_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2121 | `samples/r10_tlv_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r11_tlv_full_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2130 | `samples/r11_tlv_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | tlv | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1123 | `samples/r12_tlv_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r13_tlv_crc_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1112 | `samples/r13_tlv_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r14_tlv_full_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1121 | `samples/r14_tlv_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | dzflat-a | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/2 | `samples/r15_dzflat-a_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 2208/2 | `samples/r16_dzflat-a_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2210/2 | `samples/r17_dzflat-a_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | dzflat-a | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2210/2 | `samples/r18_dzflat-a_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | dzflat-a | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2209/3 | `samples/r19_dzflat-a_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2211/2 | `samples/r20_dzflat-a_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2211/2 | `samples/r21_dzflat-a_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 2211/2 | `samples/r22_dzflat-a_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2209/2 | `samples/r23_dzflat-a_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | dzflat-a | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2144/2 | `samples/r24_dzflat-a_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2120/2 | `samples/r25_dzflat-a_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 2143/2 | `samples/r26_dzflat-a_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | dzflat-a | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1191/2 | `samples/r27_dzflat-a_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1179/2 | `samples/r28_dzflat-a_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 2 | 1187/2 | `samples/r29_dzflat-a_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | dzflat-b | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r30_dzflat-b_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2209/2 | `samples/r31_dzflat-b_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/3 | `samples/r32_dzflat-b_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | dzflat-b | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r33_dzflat-b_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | dzflat-b | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2210/2 | `samples/r34_dzflat-b_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/2 | `samples/r35_dzflat-b_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/3 | `samples/r36_dzflat-b_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/2 | `samples/r37_dzflat-b_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/3 | `samples/r38_dzflat-b_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | dzflat-b | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 2155/2 | `samples/r39_dzflat-b_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 6 | 2152/2 | `samples/r40_dzflat-b_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 2147/2 | `samples/r41_dzflat-b_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | dzflat-b | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1217/2 | `samples/r42_dzflat-b_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1231/2 | `samples/r43_dzflat-b_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1196/2 | `samples/r44_dzflat-b_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | cyclonedds-udp | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0/0 | `samples/r46_cyclonedds-udp_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r47_cyclonedds-udp_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 0/0 | `samples/r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r50_cyclonedds-udp_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r51_cyclonedds-udp_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r52_cyclonedds-udp_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r53_cyclonedds-udp_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | cyclonedds-udp | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r55_cyclonedds-udp_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r56_cyclonedds-udp_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | cyclonedds-udp | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 9 | 0/0 | `samples/r58_cyclonedds-udp_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r59_cyclonedds-udp_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | cyclonedds-iox | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r61_cyclonedds-iox_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r62_cyclonedds-iox_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 4 | 0/0 | `samples/r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r65_cyclonedds-iox_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r66_cyclonedds-iox_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r67_cyclonedds-iox_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r68_cyclonedds-iox_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | cyclonedds-iox | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r70_cyclonedds-iox_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r71_cyclonedds-iox_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | cyclonedds-iox | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r73_cyclonedds-iox_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r74_cyclonedds-iox_full_blocking_1048576B_500Hz.samples.csv` | **通过** |

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | 3200/20845 | 7003/112271 | 14/291 | 14462/119187 |
| `r1_tlv_crc_blocking_64B_1000Hz` | 3858/20239 | 8728/87234 | 22/258 | 14317/97074 |
| `r2_tlv_full_blocking_64B_1000Hz` | 2659/18438 | 5548/28604 | 13/141 | 13488/38382 |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | 4220/21630 | 8650/117753 | 19/159 | 13510/125807 |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | 4223/22940 | 5147/84317 | 11/20 | 9809/90599 |
| `r5_tlv_crc_blocking_1024B_1000Hz` | 3902/20859 | 9241/57238 | 29/219 | 16200/66106 |
| `r6_tlv_crc_busy_1024B_1000Hz` | 2455/19171 | 5519/49083 | 11/19 | 9778/57823 |
| `r7_tlv_full_blocking_1024B_1000Hz` | 3163/18349 | 5695/46336 | 13/291 | 16261/49463 |
| `r8_tlv_full_busy_1024B_1000Hz` | 10440/38729 | 6867/21600 | 11/25 | 17674/52062 |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | 13657/24552 | 22109/73530 | 20/129 | 55680/122057 |
| `r10_tlv_crc_blocking_65536B_1000Hz` | 15033/27488 | 19496/64406 | 25/135 | 61282/118175 |
| `r11_tlv_full_blocking_65536B_1000Hz` | 13770/20644 | 24620/87352 | 13/139 | 59248/134236 |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | 174629/235425 | 171685/250234 | 29/156 | 639192/749957 |
| `r13_tlv_crc_blocking_1048576B_500Hz` | 115442/225226 | 173588/339826 | 14/93 | 570885/783271 |
| `r14_tlv_full_blocking_1048576B_500Hz` | 114742/179518 | 115280/221917 | 13/118 | 419553/643447 |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | 2420/12649 | 6440/42691 | 106/508 | 13109/49325 |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | 2637/29560 | 5141/57145 | 120/1138 | 11648/70549 |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | 3394/16696 | 5674/109231 | 149/872 | 13405/111628 |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | 8249/29285 | 12068/63848 | 119/758 | 19734/70300 |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | 2309/15567 | 2896/97008 | 98/353 | 9747/98786 |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | 2578/14737 | 9222/58141 | 367/1443 | 16567/66740 |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | 2255/13068 | 2879/15784 | 191/675 | 8786/28674 |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | 6302/31531 | 11203/124516 | 511/3287 | 20085/139255 |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | 3154/18042 | 3225/107469 | 333/1003 | 13341/109819 |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | 3646/16744 | 16757/84415 | 147/693 | 56037/129844 |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | 5355/12005 | 7619/45680 | 7789/48939 | 51775/132283 |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | 7629/14341 | 5171/23909 | 14452/75404 | 58001/147147 |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | 26415/38933 | 8838/85472 | 119/693 | 206199/318579 |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | 31300/105916 | 18534/132254 | 119162/187249 | 474755/684624 |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | 38866/150159 | 16422/112685 | 199129/389800 | 484491/804405 |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | 2018/8469 | 8212/41190 | 113/464 | 13785/50145 |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | 6279/19253 | 11463/126342 | 222/1859 | 19724/134886 |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | 1720/9513 | 7631/57182 | 193/1135 | 12461/64949 |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | 2576/13933 | 8696/164109 | 114/438 | 13111/167514 |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | 2640/13586 | 5546/62819 | 100/261 | 10015/74510 |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | 2650/12068 | 10182/113289 | 395/2208 | 19015/117042 |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | 3027/13713 | 5080/58527 | 198/966 | 13890/70059 |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | 3327/14615 | 14606/76292 | 544/3808 | 23050/90385 |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | 2589/12930 | 4314/18105 | 304/700 | 8414/34842 |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | 21558/67959 | 7960/54910 | 108/469 | 31987/100603 |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | 40731/90342 | 17562/151980 | 7996/43541 | 83927/226453 |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | 32132/78068 | 7702/94356 | 13092/77337 | 64505/168646 |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | 174296/313755 | 11271/110481 | 136/639 | 200912/390797 |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | 315349/347268 | 7594/41692 | 133146/181349 | 455363/536125 |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | 188501/328820 | 10979/55643 | 334454/362333 | 539765/684057 |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | 19866/55321 | 7314/66308 | 11/57 | 33733/101150 |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | 13761/54230 | 6154/59966 | 11/71 | 26274/94407 |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | 11885/53439 | 4607/90019 | 35/151 | 23232/107552 |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | 14763/44183 | 6062/138150 | 11/73 | 27844/154335 |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | 12847/74427 | 9734/92866 | 11/57 | 29064/125878 |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | 11375/50542 | 7268/57611 | 11/79 | 20943/84914 |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | 11183/53989 | 7423/63674 | 11/51 | 19400/89966 |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | 12028/52723 | 7199/123891 | 283/359 | 21100/130986 |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | 12064/54521 | 7464/35647 | 283/483 | 21144/71661 |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | 40809/95418 | 9508/76436 | 13/21 | 51546/161362 |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | 55705/131827 | 12652/130183 | 13/58 | 86869/192297 |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | 79368/126622 | 15229/67138 | 19049/19761 | 114088/167420 |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | 794251/1287969 | 132093/494027 | 21/100 | 928595/1651235 |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | 850557/1503632 | 119917/491600 | 17/130 | 1027142/1920201 |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | 793973/1228199 | 165487/667249 | 281548/321305 | 1224923/1968534 |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | 4107/23633 | 2536/16007 | 11/47 | 7133/34161 |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | 4914/27473 | 5065/129878 | 11/50 | 11350/139024 |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | 8766/38928 | 2884/114703 | 36/180 | 12255/122494 |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | 4924/27739 | 6069/103030 | 11/61 | 14716/107208 |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | 7249/34539 | 7344/121749 | 11/87 | 16166/124708 |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | 10322/21930 | 3838/23706 | 13/88 | 13983/39975 |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | 4180/24992 | 6174/70110 | 11/85 | 11881/79615 |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | 6108/25310 | 5819/51080 | 282/346 | 14947/65027 |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | 7682/30989 | 7281/54080 | 282/907 | 16955/69397 |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | 32368/89089 | 9048/88234 | 14/60 | 41735/130097 |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | 32356/81314 | 7856/22095 | 14/85 | 41620/93289 |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | 18711/67515 | 10163/95712 | 19030/20115 | 54000/132237 |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | 251251/329632 | 50202/70578 | 14/162 | 303881/383125 |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | 256981/348170 | 51193/70058 | 14/78 | 310809/401476 |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | 254291/300740 | 50160/69779 | 281217/291359 | 586113/640121 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

（无）

## 5. 跨进程身份与正常退出

- `r0_tlv_timestamp_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=50 sub_pid=49 | pub_ready_ns=18276646038689 in [18276641624436,18276697112527] | sub_ready_ns=18276645064611 in [18276641550779,18276697112450] | pub_starttime_ticks=1827664 sub_starttime_ticks=1827664 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r1_tlv_crc_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=56 sub_pid=55 | pub_ready_ns=18278470104084 in [18278465327394,18278520802004] | sub_ready_ns=18278469214307 in [18278465248509,18278520801969] | pub_starttime_ticks=1827846 sub_starttime_ticks=1827846 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r2_tlv_full_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=62 sub_pid=61 | pub_ready_ns=18280294775186 in [18280291131070,18280346593331] | sub_ready_ns=18280294648612 in [18280291044270,18280346593113] | pub_starttime_ticks=1828029 sub_starttime_ticks=1828029 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r3_tlv_timestamp_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=68 sub_pid=67 | pub_ready_ns=18282119571618 in [18282114874985,18282170345387] | sub_ready_ns=18282118646122 in [18282114783486,18282170345178] | pub_starttime_ticks=1828211 sub_starttime_ticks=1828211 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r4_tlv_timestamp_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=74 sub_pid=73 | pub_ready_ns=18283946572168 in [18283942919091,18283998465415] | sub_ready_ns=18283946640599 in [18283942803455,18283998465344] | pub_starttime_ticks=1828394 sub_starttime_ticks=1828394 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r5_tlv_crc_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=80 sub_pid=79 | pub_ready_ns=18285772405977 in [18285767609379,18285823120350] | sub_ready_ns=18285771383691 in [18285767523552,18285823120322] | pub_starttime_ticks=1828576 sub_starttime_ticks=1828576 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r6_tlv_crc_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=86 sub_pid=85 | pub_ready_ns=18287596789320 in [18287591925812,18287647401899] | sub_ready_ns=18287595801426 in [18287591839503,18287647401850] | pub_starttime_ticks=1828759 sub_starttime_ticks=1828759 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r7_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=92 sub_pid=91 | pub_ready_ns=18289423793691 in [18289418959185,18289474549994] | sub_ready_ns=18289422860494 in [18289418837796,18289474549938] | pub_starttime_ticks=1828941 sub_starttime_ticks=1828941 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r8_tlv_full_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=98 sub_pid=97 | pub_ready_ns=18291247985179 in [18291244252748,18291299743944] | sub_ready_ns=18291247729515 in [18291244169232,18291299743897] | pub_starttime_ticks=1829124 sub_starttime_ticks=1829124 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r9_tlv_timestamp_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=104 sub_pid=103 | pub_ready_ns=18293072997724 in [18293070150025,18293123656328] | sub_ready_ns=18293072311656 in [18293070057939,18293123656276] | pub_starttime_ticks=1829307 sub_starttime_ticks=1829307 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r10_tlv_crc_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=110 sub_pid=109 | pub_ready_ns=18294898217619 in [18294895338193,18294948773158] | sub_ready_ns=18294897775934 in [18294895258971,18294948773117] | pub_starttime_ticks=1829489 sub_starttime_ticks=1829489 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r11_tlv_full_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=116 sub_pid=115 | pub_ready_ns=18296724230849 in [18296720549895,18296773988743] | sub_ready_ns=18296723322605 in [18296720473741,18296773988537] | pub_starttime_ticks=1829672 sub_starttime_ticks=1829672 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r12_tlv_timestamp_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=122 sub_pid=121 | pub_ready_ns=18298550338916 in [18298544826076,18298600327849] | sub_ready_ns=18298548728369 in [18298544737662,18298600327807] | pub_starttime_ticks=1829854 sub_starttime_ticks=1829854 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r13_tlv_crc_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=128 sub_pid=127 | pub_ready_ns=18300379813773 in [18300374472251,18300430067970] | sub_ready_ns=18300378193851 in [18300374386127,18300430067901] | pub_starttime_ticks=1830037 sub_starttime_ticks=1830037 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r14_tlv_full_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=134 sub_pid=133 | pub_ready_ns=18302207319007 in [18302203010291,18302258466674] | sub_ready_ns=18302206173852 in [18302202913151,18302258466644] | pub_starttime_ticks=1830220 sub_starttime_ticks=1830220 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=140 sub_pid=139 | pub_ready_ns=18304029748818 in [18304025531308,18304081075046] | sub_ready_ns=18304029201482 in [18304025438815,18304081074833] | pub_starttime_ticks=1830402 sub_starttime_ticks=1830402 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r16_dzflat-a_crc_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=146 sub_pid=145 | pub_ready_ns=18305873272299 in [18305869815136,18305923216503] | sub_ready_ns=18305872599046 in [18305869726216,18305923216474] | pub_starttime_ticks=1830586 sub_starttime_ticks=1830586 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r17_dzflat-a_full_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=152 sub_pid=151 | pub_ready_ns=18307720439205 in [18307715628006,18307771224951] | sub_ready_ns=18307719459218 in [18307715499746,18307771224880] | pub_starttime_ticks=1830771 sub_starttime_ticks=1830771 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=158 sub_pid=157 | pub_ready_ns=18309567342516 in [18309562814431,18309618349848] | sub_ready_ns=18309566600596 in [18309562689252,18309618349566] | pub_starttime_ticks=1830956 sub_starttime_ticks=1830956 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=164 sub_pid=163 | pub_ready_ns=18311412972854 in [18311409188913,18311464887132] | sub_ready_ns=18311413064445 in [18311409056931,18311464887076] | pub_starttime_ticks=1831140 sub_starttime_ticks=1831140 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r20_dzflat-a_crc_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=170 sub_pid=169 | pub_ready_ns=18313237294884 in [18313232511588,18313287975723] | sub_ready_ns=18313236320743 in [18313232425305,18313287975664] | pub_starttime_ticks=1831323 sub_starttime_ticks=1831323 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r21_dzflat-a_crc_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=176 sub_pid=175 | pub_ready_ns=18315082268087 in [18315077461133,18315132994465] | sub_ready_ns=18315081223525 in [18315077374208,18315132994244] | pub_starttime_ticks=1831507 sub_starttime_ticks=1831507 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r22_dzflat-a_full_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=182 sub_pid=181 | pub_ready_ns=18316906649563 in [18316901664308,18316957175824] | sub_ready_ns=18316905689743 in [18316901575529,18316957175794] | pub_starttime_ticks=1831690 sub_starttime_ticks=1831690 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r23_dzflat-a_full_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=188 sub_pid=187 | pub_ready_ns=18318753519554 in [18318748994810,18318804570196] | sub_ready_ns=18318752853395 in [18318748863335,18318804570126] | pub_starttime_ticks=1831874 sub_starttime_ticks=1831874 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=194 sub_pid=193 | pub_ready_ns=18320579939175 in [18320576352754,18320629915483] | sub_ready_ns=18320578951448 in [18320576219005,18320629915416] | pub_starttime_ticks=1832057 sub_starttime_ticks=1832057 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r25_dzflat-a_crc_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=200 sub_pid=199 | pub_ready_ns=18322424658986 in [18322419824114,18322475277762] | sub_ready_ns=18322423583581 in [18322419736749,18322475277701] | pub_starttime_ticks=1832241 sub_starttime_ticks=1832241 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r26_dzflat-a_full_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=206 sub_pid=205 | pub_ready_ns=18324271419016 in [18324266525366,18324322016244] | sub_ready_ns=18324270336708 in [18324266435627,18324322016022] | pub_starttime_ticks=1832426 sub_starttime_ticks=1832426 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=212 sub_pid=211 | pub_ready_ns=18326118700420 in [18326114484121,18326167951794] | sub_ready_ns=18326116864345 in [18326114387506,18326167951751] | pub_starttime_ticks=1832611 sub_starttime_ticks=1832611 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r28_dzflat-a_crc_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=218 sub_pid=217 | pub_ready_ns=18327962440951 in [18327958082943,18328013618191] | sub_ready_ns=18327961370106 in [18327957982918,18328013618037] | pub_starttime_ticks=1832795 sub_starttime_ticks=1832795 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r29_dzflat-a_full_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=224 sub_pid=223 | pub_ready_ns=18329806959837 in [18329801392230,18329856852022] | sub_ready_ns=18329805267479 in [18329801302450,18329856852003] | pub_starttime_ticks=1832980 sub_starttime_ticks=1832980 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=230 sub_pid=229 | pub_ready_ns=18331653719179 in [18331649729528,18331705216723] | sub_ready_ns=18331652896508 in [18331649642450,18331705216681] | pub_starttime_ticks=1833164 sub_starttime_ticks=1833164 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r31_dzflat-b_crc_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=236 sub_pid=235 | pub_ready_ns=18333501270710 in [18333496433101,18333551994050] | sub_ready_ns=18333500315006 in [18333496269417,18333551994007] | pub_starttime_ticks=1833349 sub_starttime_ticks=1833349 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r32_dzflat-b_full_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=242 sub_pid=241 | pub_ready_ns=18335345809786 in [18335343664420,18335397065026] | sub_ready_ns=18335345830953 in [18335343573436,18335397064818] | pub_starttime_ticks=1833534 sub_starttime_ticks=1833534 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=248 sub_pid=247 | pub_ready_ns=18337191501094 in [18337188039663,18337241619813] | sub_ready_ns=18337190746451 in [18337187931910,18337241619768] | pub_starttime_ticks=1833718 sub_starttime_ticks=1833718 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=254 sub_pid=253 | pub_ready_ns=18339036005126 in [18339032237383,18339087642851] | sub_ready_ns=18339035694381 in [18339032138653,18339087642808] | pub_starttime_ticks=1833903 sub_starttime_ticks=1833903 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r35_dzflat-b_crc_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=260 sub_pid=259 | pub_ready_ns=18340863619588 in [18340858823962,18340914281138] | sub_ready_ns=18340862667955 in [18340858686204,18340914281109] | pub_starttime_ticks=1834085 sub_starttime_ticks=1834085 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r36_dzflat-b_crc_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=266 sub_pid=265 | pub_ready_ns=18342708361152 in [18342704998029,18342760569810] | sub_ready_ns=18342709383897 in [18342704859310,18342760569780] | pub_starttime_ticks=1834270 sub_starttime_ticks=1834270 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r37_dzflat-b_full_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=272 sub_pid=271 | pub_ready_ns=18344537145880 in [18344533660761,18344587122806] | sub_ready_ns=18344536043997 in [18344533567647,18344587122777] | pub_starttime_ticks=1834453 sub_starttime_ticks=1834453 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r38_dzflat-b_full_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=278 sub_pid=277 | pub_ready_ns=18346381102069 in [18346378506174,18346431897651] | sub_ready_ns=18346381479599 in [18346378409439,18346431897445] | pub_starttime_ticks=1834637 sub_starttime_ticks=1834637 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=284 sub_pid=283 | pub_ready_ns=18348207713364 in [18348202927965,18348258444817] | sub_ready_ns=18348206792306 in [18348202784978,18348258444746] | pub_starttime_ticks=1834820 sub_starttime_ticks=1834820 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r40_dzflat-b_crc_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=290 sub_pid=289 | pub_ready_ns=18350053891419 in [18350049085739,18350104530652] | sub_ready_ns=18350052883332 in [18350048979735,18350104530614] | pub_starttime_ticks=1835004 sub_starttime_ticks=1835004 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r41_dzflat-b_full_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=296 sub_pid=295 | pub_ready_ns=18351901576832 in [18351898050728,18351951556464] | sub_ready_ns=18351900741093 in [18351897902698,18351951556399] | pub_starttime_ticks=1835189 sub_starttime_ticks=1835189 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=302 sub_pid=301 | pub_ready_ns=18353750829554 in [18353745100683,18353800900749] | sub_ready_ns=18353749037234 in [18353744957791,18353800900686] | pub_starttime_ticks=1835374 sub_starttime_ticks=1835374 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r43_dzflat-b_crc_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=308 sub_pid=307 | pub_ready_ns=18355593881928 in [18355589335662,18355644889376] | sub_ready_ns=18355593176571 in [18355589185241,18355644889317] | pub_starttime_ticks=1835558 sub_starttime_ticks=1835558 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r44_dzflat-b_full_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=314 sub_pid=313 | pub_ready_ns=18357442105087 in [18357436505981,18357491983933] | sub_ready_ns=18357440320665 in [18357436414298,18357491983790] | pub_starttime_ticks=1835743 sub_starttime_ticks=1835743 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=323 sub_pid=322 | pub_ready_ns=18359290470717 in [18359282521893,18359301061423] | sub_ready_ns=18359288870693 in [18359282437432,18359301061378] | pub_starttime_ticks=1835928 sub_starttime_ticks=1835928 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=342 sub_pid=341 | pub_ready_ns=18361493750104 in [18361486250320,18361504814354] | sub_ready_ns=18361493748746 in [18361486115231,18361504814294] | pub_starttime_ticks=1836148 sub_starttime_ticks=1836148 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=361 sub_pid=360 | pub_ready_ns=18363688840121 in [18363683866768,18363700312458] | sub_ready_ns=18363689279074 in [18363683734763,18363700312415] | pub_starttime_ticks=1836368 sub_starttime_ticks=1836368 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=380 sub_pid=379 | pub_ready_ns=18365886803073 in [18365878791020,18365897538211] | sub_ready_ns=18365887110906 in [18365878655023,18365897538150] | pub_starttime_ticks=1836587 sub_starttime_ticks=1836587 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=399 sub_pid=398 | pub_ready_ns=18368084681459 in [18368079074181,18368095598921] | sub_ready_ns=18368083752191 in [18368078919062,18368095598880] | pub_starttime_ticks=1836807 sub_starttime_ticks=1836807 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=418 sub_pid=417 | pub_ready_ns=18370280357434 in [18370274501975,18370291015969] | sub_ready_ns=18370280366313 in [18370274361723,18370291015761] | pub_starttime_ticks=1837027 sub_starttime_ticks=1837027 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=437 sub_pid=436 | pub_ready_ns=18372476865661 in [18372470292747,18372488867495] | sub_ready_ns=18372476572395 in [18372470156322,18372488867451] | pub_starttime_ticks=1837247 sub_starttime_ticks=1837247 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=456 sub_pid=455 | pub_ready_ns=18374673318262 in [18374666177923,18374684852780] | sub_ready_ns=18374673208406 in [18374666035088,18374684852712] | pub_starttime_ticks=1837466 sub_starttime_ticks=1837466 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=475 sub_pid=474 | pub_ready_ns=18376868802653 in [18376861346149,18376879913418] | sub_ready_ns=18376869396638 in [18376861205275,18376879913350] | pub_starttime_ticks=1837686 sub_starttime_ticks=1837686 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=494 sub_pid=493 | pub_ready_ns=18379065814513 in [18379059828514,18379076362028] | sub_ready_ns=18379065815801 in [18379059689402,18379076361952] | pub_starttime_ticks=1837905 sub_starttime_ticks=1837905 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=513 sub_pid=512 | pub_ready_ns=18381260559485 in [18381253547913,18381272113012] | sub_ready_ns=18381260155810 in [18381253399296,18381272112972] | pub_starttime_ticks=1838125 sub_starttime_ticks=1838125 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=532 sub_pid=531 | pub_ready_ns=18383458803091 in [18383451646108,18383470246968] | sub_ready_ns=18383458802511 in [18383451496583,18383470246802] | pub_starttime_ticks=1838345 sub_starttime_ticks=1838345 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=551 sub_pid=550 | pub_ready_ns=18385659882559 in [18385650988454,18385667524989] | sub_ready_ns=18385659239691 in [18385650845177,18385667524948] | pub_starttime_ticks=1838565 sub_starttime_ticks=1838565 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=570 sub_pid=569 | pub_ready_ns=18387852741254 in [18387842955741,18387861489135] | sub_ready_ns=18387852254452 in [18387842805287,18387861489096] | pub_starttime_ticks=1838784 sub_starttime_ticks=1838784 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=589 sub_pid=588 | pub_ready_ns=18390044589769 in [18390038750998,18390059310870] | sub_ready_ns=18390043648146 in [18390038663863,18390059310801] | pub_starttime_ticks=1839003 sub_starttime_ticks=1839003 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=608 sub_pid=607 | pub_ready_ns=18392244991094 in [18392237211796,18392255857738] | sub_ready_ns=18392243186608 in [18392237055125,18392255857667] | pub_starttime_ticks=1839223 sub_starttime_ticks=1839223 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=631 sub_pid=630 | pub_ready_ns=18394451508226 in [18394437178619,18394461921738] | sub_ready_ns=18394449900420 in [18394437032726,18394461921485] | pub_starttime_ticks=1839443 sub_starttime_ticks=1839443 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz` identity: parent_pid=13 pub_pid=654 sub_pid=653 | pub_ready_ns=18396649631806 in [18396641551057,18396660130310] | sub_ready_ns=18396651426429 in [18396641398061,18396660130236] | pub_starttime_ticks=1839664 sub_starttime_ticks=1839664 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=677 sub_pid=676 | pub_ready_ns=18398851187122 in [18398839612926,18398862671576] | sub_ready_ns=18398851221577 in [18398839432128,18398862671512] | pub_starttime_ticks=1839883 sub_starttime_ticks=1839883 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=700 sub_pid=699 | pub_ready_ns=18401051410357 in [18401041631269,18401062340203] | sub_ready_ns=18401051446600 in [18401041476781,18401062340156] | pub_starttime_ticks=1840104 sub_starttime_ticks=1840104 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=723 sub_pid=722 | pub_ready_ns=18403250602221 in [18403241328131,18403261980925] | sub_ready_ns=18403250714413 in [18403241182244,18403261980735] | pub_starttime_ticks=1840324 sub_starttime_ticks=1840324 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=746 sub_pid=745 | pub_ready_ns=18405448766086 in [18405441539293,18405460067844] | sub_ready_ns=18405447639636 in [18405441397191,18405460067651] | pub_starttime_ticks=1840544 sub_starttime_ticks=1840544 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` identity: parent_pid=13 pub_pid=769 sub_pid=768 | pub_ready_ns=18407648999023 in [18407638754914,18407659563522] | sub_ready_ns=18407649649707 in [18407638591551,18407659563454] | pub_starttime_ticks=1840763 sub_starttime_ticks=1840763 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz` identity: parent_pid=13 pub_pid=792 sub_pid=791 | pub_ready_ns=18409848637717 in [18409840626312,18409859176406] | sub_ready_ns=18409848201026 in [18409840519534,18409859176376] | pub_starttime_ticks=1840984 sub_starttime_ticks=1840984 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=815 sub_pid=814 | pub_ready_ns=18412043466232 in [18412037494085,18412054128238] | sub_ready_ns=18412044564916 in [18412037342890,18412054128166] | pub_starttime_ticks=1841203 sub_starttime_ticks=1841203 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=838 sub_pid=837 | pub_ready_ns=18414249138679 in [18414235298051,18414260067055] | sub_ready_ns=18414249223370 in [18414235145950,18414260066781] | pub_starttime_ticks=1841423 sub_starttime_ticks=1841423 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` identity: parent_pid=13 pub_pid=861 sub_pid=860 | pub_ready_ns=18416453297364 in [18416439946911,18416464807205] | sub_ready_ns=18416451975899 in [18416439798870,18416464807146] | pub_starttime_ticks=1841644 sub_starttime_ticks=1841643 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=884 sub_pid=883 | pub_ready_ns=18418652652740 in [18418640898708,18418655402256] | sub_ready_ns=18418651821974 in [18418640741950,18418655402220] | pub_starttime_ticks=1841864 sub_starttime_ticks=1841864 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=907 sub_pid=906 | pub_ready_ns=18420842439089 in [18420832550715,18420845133133] | sub_ready_ns=18420842416083 in [18420832397639,18420845133058] | pub_starttime_ticks=1842083 sub_starttime_ticks=1842083 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` identity: parent_pid=13 pub_pid=930 sub_pid=929 | pub_ready_ns=18423034551974 in [18423025439933,18423037854950] | sub_ready_ns=18423032998576 in [18423025286326,18423037854890] | pub_starttime_ticks=1842302 sub_starttime_ticks=1842302 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- `r0_tlv_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 17591 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r1_tlv_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 57017 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r2_tlv_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 69704 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r3_tlv_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 50535 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r4_tlv_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 140250 ns（迟发 4 次）—— 该轮延迟含排队成分
- `r5_tlv_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 30413 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r6_tlv_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 5363 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r7_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 39690 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r8_tlv_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 213856 ns（迟发 8 次）—— 该轮延迟含排队成分
- `r9_tlv_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 7969 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r10_tlv_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5628 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r11_tlv_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 43146 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r12_tlv_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 28387 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r13_tlv_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 59615 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r14_tlv_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 92525 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 26808 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r16_dzflat-a_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 132018 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r17_dzflat-a_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 20559 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 97467 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 161873 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r20_dzflat-a_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 4576 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r21_dzflat-a_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 5111 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r22_dzflat-a_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 111198 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r23_dzflat-a_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 397713 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 112642 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r25_dzflat-a_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 45744 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r26_dzflat-a_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 187547 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 29564 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r28_dzflat-a_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5003 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r29_dzflat-a_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 993942 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 18416 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r31_dzflat-b_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 121284 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r32_dzflat-b_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4881 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 37230 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 42772 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r35_dzflat-b_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 27537 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r36_dzflat-b_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 63497 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r37_dzflat-b_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 74254 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r38_dzflat-b_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 59809 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 429702 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r40_dzflat-b_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 120027 ns（迟发 6 次）—— 该轮延迟含排队成分
- `r41_dzflat-b_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 138139 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 12863 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r43_dzflat-b_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5690 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r44_dzflat-b_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 51058 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 108186 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 190181 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 80864 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 54977 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 109770 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 20951 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 118858 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 6229 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 50293 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4970 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 96088 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 20815 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 121708 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 10809569 ns（迟发 9 次）—— 该轮延迟含排队成分
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 24399 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 52321 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 64882 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 84198 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 66883 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 125094 ns（迟发 4 次）—— 该轮延迟含排队成分
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 53040 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 26588 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 46499 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 163563 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 62034 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 48975 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4585 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 85183 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5060 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5756 ns（迟发 0 次）—— 该轮延迟含排队成分

