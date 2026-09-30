# W02 统一跨进程基准 · 判定（verdict）

> run_id: `20260928-r23-W02`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `d013ab2348f75e88db1275bfd1740617f7086af1418133e4857e0b15169f1695`；libipc sha256: `216a4b0c914544acd00b0a2622aeb6f84dbd3512d821f60ffd5ed6adc620ac25`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=9.98064 syscall_ns_per_call=83.9073 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

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
| `r0_tlv_timestamp_blocking_64B_1000Hz` | tlv | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2214 | `samples/r0_tlv_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r1_tlv_crc_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r1_tlv_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r2_tlv_full_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2211 | `samples/r2_tlv_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | tlv | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2213 | `samples/r3_tlv_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | tlv | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2214 | `samples/r4_tlv_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r5_tlv_crc_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2213 | `samples/r5_tlv_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r6_tlv_crc_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 9 | 0/2212 | `samples/r6_tlv_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r7_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r7_tlv_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r8_tlv_full_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 4 | 0/2213 | `samples/r8_tlv_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | tlv | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/2111 | `samples/r9_tlv_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r10_tlv_crc_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2123 | `samples/r10_tlv_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r11_tlv_full_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2088 | `samples/r11_tlv_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | tlv | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1129 | `samples/r12_tlv_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r13_tlv_crc_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1096 | `samples/r13_tlv_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r14_tlv_full_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1045 | `samples/r14_tlv_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | dzflat-a | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/2 | `samples/r15_dzflat-a_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2211/2 | `samples/r16_dzflat-a_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2210/2 | `samples/r17_dzflat-a_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | dzflat-a | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/2 | `samples/r18_dzflat-a_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | dzflat-a | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r19_dzflat-a_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2208/3 | `samples/r20_dzflat-a_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 2208/2 | `samples/r21_dzflat-a_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r22_dzflat-a_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2207/3 | `samples/r23_dzflat-a_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | dzflat-a | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2148/2 | `samples/r24_dzflat-a_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 7 | 2146/2 | `samples/r25_dzflat-a_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2142/2 | `samples/r26_dzflat-a_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | dzflat-a | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1177/2 | `samples/r27_dzflat-a_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1194/2 | `samples/r28_dzflat-a_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1191/2 | `samples/r29_dzflat-a_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | dzflat-b | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2210/2 | `samples/r30_dzflat-b_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r31_dzflat-b_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/3 | `samples/r32_dzflat-b_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | dzflat-b | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 2211/2 | `samples/r33_dzflat-b_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | dzflat-b | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2209/2 | `samples/r34_dzflat-b_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 7 | 2210/2 | `samples/r35_dzflat-b_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2211/2 | `samples/r36_dzflat-b_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 9 | 2211/2 | `samples/r37_dzflat-b_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 2209/2 | `samples/r38_dzflat-b_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | dzflat-b | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2149/2 | `samples/r39_dzflat-b_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2156/2 | `samples/r40_dzflat-b_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2162/3 | `samples/r41_dzflat-b_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | dzflat-b | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1247/2 | `samples/r42_dzflat-b_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1248/2 | `samples/r43_dzflat-b_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1246/2 | `samples/r44_dzflat-b_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | cyclonedds-udp | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r46_cyclonedds-udp_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r47_cyclonedds-udp_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r50_cyclonedds-udp_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r51_cyclonedds-udp_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r52_cyclonedds-udp_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r53_cyclonedds-udp_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | cyclonedds-udp | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r55_cyclonedds-udp_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 6 | 0/0 | `samples/r56_cyclonedds-udp_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | cyclonedds-udp | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r58_cyclonedds-udp_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r59_cyclonedds-udp_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | cyclonedds-iox | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0/0 | `samples/r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r61_cyclonedds-iox_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0/0 | `samples/r62_cyclonedds-iox_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 0/0 | `samples/r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r65_cyclonedds-iox_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r66_cyclonedds-iox_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r67_cyclonedds-iox_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r68_cyclonedds-iox_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | cyclonedds-iox | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r70_cyclonedds-iox_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 0/0 | `samples/r71_cyclonedds-iox_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | cyclonedds-iox | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r73_cyclonedds-iox_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r74_cyclonedds-iox_full_blocking_1048576B_500Hz.samples.csv` | **通过** |

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | 2452/14469 | 6814/110422 | 12/141 | 13741/116210 |
| `r1_tlv_crc_blocking_64B_1000Hz` | 4392/24807 | 8975/75980 | 24/135 | 15056/88058 |
| `r2_tlv_full_blocking_64B_1000Hz` | 3820/20007 | 7746/32915 | 19/141 | 11912/52411 |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | 3247/29608 | 9238/64782 | 20/151 | 15554/75647 |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | 2428/11174 | 5090/57147 | 12/23 | 8060/59793 |
| `r5_tlv_crc_blocking_1024B_1000Hz` | 4265/21844 | 7403/109540 | 15/148 | 17222/123727 |
| `r6_tlv_crc_busy_1024B_1000Hz` | 10215/38692 | 7225/166172 | 11/60 | 20106/175170 |
| `r7_tlv_full_blocking_1024B_1000Hz` | 4121/20422 | 10778/190700 | 20/380 | 19431/200307 |
| `r8_tlv_full_busy_1024B_1000Hz` | 4713/38499 | 5307/88475 | 11/73 | 15072/98476 |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | 13148/25509 | 19382/206270 | 20/294 | 57175/248561 |
| `r10_tlv_crc_blocking_65536B_1000Hz` | 10753/27969 | 54553/170872 | 19/177 | 90060/226922 |
| `r11_tlv_full_blocking_65536B_1000Hz` | 15157/21496 | 14695/74176 | 13/141 | 49772/134976 |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | 118153/175926 | 179485/264329 | 34/140 | 474708/613295 |
| `r13_tlv_crc_blocking_1048576B_500Hz` | 122126/225046 | 197562/368772 | 20/126 | 621623/925894 |
| `r14_tlv_full_blocking_1048576B_500Hz` | 163960/220349 | 179227/347819 | 21/248 | 513669/818501 |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | 7950/20083 | 5172/84247 | 109/591 | 13405/86183 |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | 2744/16235 | 6401/104619 | 120/640 | 12215/110538 |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | 5658/27370 | 11136/118410 | 190/2136 | 18062/123154 |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | 2958/16924 | 7869/107446 | 107/442 | 13852/116022 |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | 2436/13344 | 2975/123170 | 102/316 | 12749/130331 |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | 2844/14670 | 4794/108543 | 236/1169 | 14411/111320 |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | 7938/29897 | 18448/140064 | 229/946 | 22318/142619 |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | 2084/23252 | 4672/25606 | 351/2308 | 14013/42225 |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | 2721/16684 | 8073/15107 | 367/1055 | 15243/29726 |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | 5577/9095 | 8213/121768 | 102/1067 | 32753/141759 |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | 6441/16035 | 12661/132592 | 5868/43297 | 70105/191343 |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | 7058/12349 | 12019/103879 | 21843/77414 | 71451/157162 |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | 27458/58123 | 7461/62884 | 184/903 | 202683/383955 |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | 33599/96674 | 8037/43868 | 119535/142053 | 431439/595673 |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | 30180/131251 | 9919/104868 | 196450/257935 | 406586/649113 |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | 1626/10082 | 7981/69136 | 107/429 | 12593/71823 |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | 2632/13180 | 8249/101346 | 155/734 | 12685/104926 |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | 1285/8000 | 4336/87812 | 116/1037 | 6333/97138 |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | 7521/20419 | 16468/103544 | 218/802 | 25393/111132 |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | 3019/12314 | 9702/103039 | 109/238 | 20136/109829 |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | 7223/20389 | 17731/145801 | 322/2167 | 28092/153336 |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | 1757/13855 | 2947/101991 | 177/659 | 8905/103998 |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | 7539/20234 | 14806/36400 | 654/3473 | 23437/56216 |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | 7458/20639 | 2851/19070 | 350/1004 | 13311/41892 |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | 12759/48171 | 11059/128120 | 117/427 | 28449/150117 |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | 33787/71712 | 7072/62517 | 7536/41526 | 50187/129018 |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | 21426/55926 | 6864/28965 | 12432/77956 | 44117/136439 |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | 181288/323153 | 11210/148796 | 152/604 | 203075/356605 |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | 313899/349519 | 76126/161327 | 88351/163018 | 494095/613615 |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | 169534/227569 | 4871/55303 | 196139/266658 | 375012/485819 |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | 10760/40942 | 7006/23436 | 11/66 | 19297/52260 |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | 5527/50584 | 6733/119108 | 11/67 | 17151/137300 |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | 5456/35344 | 3590/33063 | 36/143 | 20602/56900 |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | 6294/41154 | 4268/65191 | 11/58 | 15357/88244 |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | 10629/55153 | 7689/49769 | 11/66 | 19608/79362 |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | 11115/52740 | 6972/64164 | 11/19 | 19584/89499 |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | 10836/56340 | 7569/101838 | 11/18 | 19785/124404 |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | 5045/32591 | 3777/28919 | 285/413 | 9324/54206 |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | 10231/64056 | 7301/138962 | 285/448 | 25944/147067 |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | 44607/95260 | 9645/73278 | 13/28 | 66668/153944 |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | 47496/92679 | 9427/109159 | 11/34 | 73576/169089 |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | 68901/144260 | 20782/100567 | 17544/18518 | 121035/234655 |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | 713856/916553 | 97510/363435 | 20/91 | 924788/1207179 |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | 668444/942734 | 114629/573184 | 20/103 | 933910/1464867 |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | 696641/1260763 | 106497/445007 | 281249/304405 | 1215022/1968712 |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | 5910/47833 | 5356/156831 | 11/56 | 12990/182177 |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | 1970/18870 | 3393/39333 | 10/57 | 7541/47041 |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | 3260/23221 | 2775/26901 | 35/135 | 11543/39313 |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | 8821/45963 | 2813/105634 | 11/41 | 13310/126508 |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | 10271/45222 | 3516/91860 | 10/34 | 13850/102386 |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | 1975/32162 | 2818/111286 | 10/54 | 5729/112974 |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | 2410/25940 | 2859/105922 | 10/57 | 6129/111072 |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | 2265/20885 | 2791/20982 | 285/370 | 6055/31975 |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | 4811/26706 | 2430/17849 | 284/412 | 8001/39979 |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | 33433/88750 | 9807/100638 | 12/68 | 45496/137918 |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | 31830/96135 | 8471/114192 | 14/60 | 43532/157065 |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | 37577/101135 | 10800/133317 | 17547/21080 | 69920/192725 |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | 255960/475803 | 56710/135513 | 12/84 | 314177/537513 |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | 252107/306919 | 56641/181591 | 14/107 | 309676/438247 |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | 254977/299857 | 54664/158286 | 281974/296180 | 591397/709723 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

（无）

## 5. 跨进程身份与正常退出

- `r0_tlv_timestamp_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=47 sub_pid=46 | pub_ready_ns=23106620854112 in [23106618444368,23106671908137] | sub_ready_ns=23106621261174 in [23106618369797,23106671908110] | pub_starttime_ticks=2310661 sub_starttime_ticks=2310661 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r1_tlv_crc_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=53 sub_pid=52 | pub_ready_ns=23108450905008 in [23108446021374,23108501478353] | sub_ready_ns=23108449840567 in [23108445932998,23108501478312] | pub_starttime_ticks=2310844 sub_starttime_ticks=2310844 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r2_tlv_full_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=59 sub_pid=58 | pub_ready_ns=23110275897169 in [23110272087699,23110327577653] | sub_ready_ns=23110275947766 in [23110271992039,23110327577633] | pub_starttime_ticks=2311027 sub_starttime_ticks=2311027 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r3_tlv_timestamp_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=65 sub_pid=64 | pub_ready_ns=23112102998572 in [23112097868138,23112153535140] | sub_ready_ns=23112101881592 in [23112097745229,23112153535089] | pub_starttime_ticks=2311209 sub_starttime_ticks=2311209 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r4_tlv_timestamp_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=71 sub_pid=70 | pub_ready_ns=23113928797318 in [23113925450336,23113980926728] | sub_ready_ns=23113929890059 in [23113925320868,23113980926608] | pub_starttime_ticks=2311392 sub_starttime_ticks=2311392 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r5_tlv_crc_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=77 sub_pid=76 | pub_ready_ns=23115759412114 in [23115754562253,23115810003328] | sub_ready_ns=23115758455802 in [23115754483303,23115810003272] | pub_starttime_ticks=2311575 sub_starttime_ticks=2311575 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r6_tlv_crc_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=83 sub_pid=82 | pub_ready_ns=23117585194007 in [23117580375801,23117635852614] | sub_ready_ns=23117584101000 in [23117580290992,23117635852575] | pub_starttime_ticks=2311758 sub_starttime_ticks=2311758 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r7_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=89 sub_pid=88 | pub_ready_ns=23119410696649 in [23119405881312,23119461540686] | sub_ready_ns=23119409712724 in [23119405750162,23119461540624] | pub_starttime_ticks=2311940 sub_starttime_ticks=2311940 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r8_tlv_full_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=95 sub_pid=94 | pub_ready_ns=23121238112205 in [23121233272981,23121288827072] | sub_ready_ns=23121237052737 in [23121233142922,23121288827000] | pub_starttime_ticks=2312123 sub_starttime_ticks=2312123 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r9_tlv_timestamp_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=101 sub_pid=100 | pub_ready_ns=23123062074429 in [23123058647913,23123114160531] | sub_ready_ns=23123061796529 in [23123058557560,23123114160498] | pub_starttime_ticks=2312305 sub_starttime_ticks=2312305 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r10_tlv_crc_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=107 sub_pid=106 | pub_ready_ns=23124888764497 in [23124884965436,23124940506736] | sub_ready_ns=23124888597239 in [23124884836150,23124940506673] | pub_starttime_ticks=2312488 sub_starttime_ticks=2312488 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r11_tlv_full_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=113 sub_pid=112 | pub_ready_ns=23126717105455 in [23126711966614,23126767493729] | sub_ready_ns=23126715965556 in [23126711842623,23126767493702] | pub_starttime_ticks=2312671 sub_starttime_ticks=2312671 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r12_tlv_timestamp_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=119 sub_pid=118 | pub_ready_ns=23128545698257 in [23128539906414,23128595393300] | sub_ready_ns=23128543897530 in [23128539780920,23128595393247] | pub_starttime_ticks=2312853 sub_starttime_ticks=2312853 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r13_tlv_crc_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=125 sub_pid=124 | pub_ready_ns=23130371054346 in [23130365448622,23130420918976] | sub_ready_ns=23130369396509 in [23130365367895,23130420918941] | pub_starttime_ticks=2313036 sub_starttime_ticks=2313036 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r14_tlv_full_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=131 sub_pid=130 | pub_ready_ns=23132198771339 in [23132194093918,23132249708821] | sub_ready_ns=23132196869606 in [23132193826086,23132249708547] | pub_starttime_ticks=2313219 sub_starttime_ticks=2313219 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=137 sub_pid=136 | pub_ready_ns=23134024478554 in [23134019547103,23134075176878] | sub_ready_ns=23134023499414 in [23134019396012,23134075176838] | pub_starttime_ticks=2313401 sub_starttime_ticks=2313401 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r16_dzflat-a_crc_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=143 sub_pid=142 | pub_ready_ns=23135869769768 in [23135865268075,23135920764450] | sub_ready_ns=23135868786514 in [23135865190535,23135920764394] | pub_starttime_ticks=2313586 sub_starttime_ticks=2313586 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r17_dzflat-a_full_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=149 sub_pid=148 | pub_ready_ns=23137716051753 in [23137711088027,23137766522267] | sub_ready_ns=23137715016346 in [23137710993199,23137766522219] | pub_starttime_ticks=2313771 sub_starttime_ticks=2313771 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=155 sub_pid=154 | pub_ready_ns=23139561828193 in [23139556985605,23139612444758] | sub_ready_ns=23139560832011 in [23139556899810,23139612444727] | pub_starttime_ticks=2313955 sub_starttime_ticks=2313955 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=161 sub_pid=160 | pub_ready_ns=23141408954182 in [23141405382936,23141458858213] | sub_ready_ns=23141407954815 in [23141405253754,23141458858146] | pub_starttime_ticks=2314140 sub_starttime_ticks=2314140 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r20_dzflat-a_crc_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=167 sub_pid=166 | pub_ready_ns=23143233392910 in [23143230155462,23143285702101] | sub_ready_ns=23143234366853 in [23143230031482,23143285701921] | pub_starttime_ticks=2314323 sub_starttime_ticks=2314323 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r21_dzflat-a_crc_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=173 sub_pid=172 | pub_ready_ns=23145084205688 in [23145080717713,23145134172373] | sub_ready_ns=23145083476785 in [23145080589912,23145134172325] | pub_starttime_ticks=2314508 sub_starttime_ticks=2314508 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r22_dzflat-a_full_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=179 sub_pid=178 | pub_ready_ns=23146908802813 in [23146904052076,23146959539256] | sub_ready_ns=23146907819354 in [23146903965721,23146959539230] | pub_starttime_ticks=2314690 sub_starttime_ticks=2314690 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r23_dzflat-a_full_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=185 sub_pid=184 | pub_ready_ns=23148753987024 in [23148750198327,23148805613186] | sub_ready_ns=23148753988013 in [23148750106224,23148805613151] | pub_starttime_ticks=2314875 sub_starttime_ticks=2314875 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=191 sub_pid=190 | pub_ready_ns=23150579389445 in [23150574492959,23150629979212] | sub_ready_ns=23150578349330 in [23150574402014,23150629979001] | pub_starttime_ticks=2315057 sub_starttime_ticks=2315057 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r25_dzflat-a_crc_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=197 sub_pid=196 | pub_ready_ns=23152425835485 in [23152421419424,23152476923171] | sub_ready_ns=23152425312217 in [23152421322495,23152476922963] | pub_starttime_ticks=2315242 sub_starttime_ticks=2315242 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r26_dzflat-a_full_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=203 sub_pid=202 | pub_ready_ns=23154272435042 in [23154267662592,23154323075323] | sub_ready_ns=23154271416586 in [23154267555956,23154323075114] | pub_starttime_ticks=2315426 sub_starttime_ticks=2315426 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=209 sub_pid=208 | pub_ready_ns=23156119607785 in [23156113985214,23156169441056] | sub_ready_ns=23156117896321 in [23156113879669,23156169441014] | pub_starttime_ticks=2315611 sub_starttime_ticks=2315611 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r28_dzflat-a_crc_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=215 sub_pid=214 | pub_ready_ns=23157963997561 in [23157959491189,23158014943819] | sub_ready_ns=23157963357759 in [23157959397855,23158014943665] | pub_starttime_ticks=2315795 sub_starttime_ticks=2315795 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r29_dzflat-a_full_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=221 sub_pid=220 | pub_ready_ns=23159814573320 in [23159810139982,23159865650390] | sub_ready_ns=23159813912966 in [23159809971092,23159865650348] | pub_starttime_ticks=2315981 sub_starttime_ticks=2315981 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=227 sub_pid=226 | pub_ready_ns=23161662757849 in [23161657986801,23161713441732] | sub_ready_ns=23161661802471 in [23161657892110,23161713441711] | pub_starttime_ticks=2316165 sub_starttime_ticks=2316165 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r31_dzflat-b_crc_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=233 sub_pid=232 | pub_ready_ns=23163509267912 in [23163504465028,23163559998859] | sub_ready_ns=23163508274630 in [23163504369671,23163559998703] | pub_starttime_ticks=2316350 sub_starttime_ticks=2316350 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r32_dzflat-b_full_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=239 sub_pid=238 | pub_ready_ns=23165354207867 in [23165350911169,23165406377588] | sub_ready_ns=23165355245623 in [23165350818486,23165406377537] | pub_starttime_ticks=2316535 sub_starttime_ticks=2316535 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=245 sub_pid=244 | pub_ready_ns=23167205291941 in [23167201925163,23167255446223] | sub_ready_ns=23167204578670 in [23167201791105,23167255446171] | pub_starttime_ticks=2316720 sub_starttime_ticks=2316720 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=251 sub_pid=250 | pub_ready_ns=23169045434068 in [23169040510092,23169096018184] | sub_ready_ns=23169044422528 in [23169040421351,23169096017958] | pub_starttime_ticks=2316904 sub_starttime_ticks=2316904 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r35_dzflat-b_crc_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=257 sub_pid=256 | pub_ready_ns=23170873006634 in [23170869481854,23170922999576] | sub_ready_ns=23170872549811 in [23170869355829,23170922999538] | pub_starttime_ticks=2317086 sub_starttime_ticks=2317086 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r36_dzflat-b_crc_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=263 sub_pid=262 | pub_ready_ns=23172717632494 in [23172712826416,23172768280002] | sub_ready_ns=23172716633690 in [23172712735314,23172768279946] | pub_starttime_ticks=2317271 sub_starttime_ticks=2317271 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r37_dzflat-b_full_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=269 sub_pid=268 | pub_ready_ns=23174543278243 in [23174538402854,23174593838872] | sub_ready_ns=23174542142643 in [23174538315507,23174593838816] | pub_starttime_ticks=2317453 sub_starttime_ticks=2317453 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r38_dzflat-b_full_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=275 sub_pid=274 | pub_ready_ns=23176390055759 in [23176384953994,23176440413817] | sub_ready_ns=23176388994487 in [23176384857781,23176440413772] | pub_starttime_ticks=2317638 sub_starttime_ticks=2317638 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=281 sub_pid=280 | pub_ready_ns=23178214840399 in [23178210998741,23178266475902] | sub_ready_ns=23178214810746 in [23178210891094,23178266475838] | pub_starttime_ticks=2317821 sub_starttime_ticks=2317821 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r40_dzflat-b_crc_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=287 sub_pid=286 | pub_ready_ns=23180060338806 in [23180055573658,23180111010860] | sub_ready_ns=23180059349071 in [23180055474167,23180111010688] | pub_starttime_ticks=2318005 sub_starttime_ticks=2318005 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r41_dzflat-b_full_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=293 sub_pid=292 | pub_ready_ns=23181905660865 in [23181902577753,23181958072963] | sub_ready_ns=23181906660082 in [23181902480202,23181958072741] | pub_starttime_ticks=2318190 sub_starttime_ticks=2318190 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=299 sub_pid=298 | pub_ready_ns=23183759761617 in [23183753933871,23183809477760] | sub_ready_ns=23183757915433 in [23183753792728,23183809477684] | pub_starttime_ticks=2318375 sub_starttime_ticks=2318375 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r43_dzflat-b_crc_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=305 sub_pid=304 | pub_ready_ns=23185604127469 in [23185599133483,23185654654925] | sub_ready_ns=23185603008598 in [23185598983841,23185654654899] | pub_starttime_ticks=2318559 sub_starttime_ticks=2318559 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r44_dzflat-b_full_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=311 sub_pid=310 | pub_ready_ns=23187450704238 in [23187446573930,23187500113996] | sub_ready_ns=23187449039880 in [23187446433044,23187500113947] | pub_starttime_ticks=2318744 sub_starttime_ticks=2318744 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=320 sub_pid=319 | pub_ready_ns=23189302540815 in [23189296797141,23189313332392] | sub_ready_ns=23189302791956 in [23189296658582,23189313332360] | pub_starttime_ticks=2318929 sub_starttime_ticks=2318929 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=339 sub_pid=338 | pub_ready_ns=23191499758988 in [23191491349535,23191511921358] | sub_ready_ns=23191499756074 in [23191491245037,23191511921215] | pub_starttime_ticks=2319149 sub_starttime_ticks=2319149 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=358 sub_pid=357 | pub_ready_ns=23193694431741 in [23193689699322,23193706182186] | sub_ready_ns=23193693137163 in [23193689609089,23193706182157] | pub_starttime_ticks=2319368 sub_starttime_ticks=2319368 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=377 sub_pid=376 | pub_ready_ns=23195894738880 in [23195886435156,23195905025085] | sub_ready_ns=23195895312358 in [23195886305800,23195905024921] | pub_starttime_ticks=2319588 sub_starttime_ticks=2319588 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=396 sub_pid=395 | pub_ready_ns=23198090510461 in [23198084949667,23198101450571] | sub_ready_ns=23198091069067 in [23198084850790,23198101450351] | pub_starttime_ticks=2319808 sub_starttime_ticks=2319808 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=415 sub_pid=414 | pub_ready_ns=23200288261477 in [23200280133678,23200298701252] | sub_ready_ns=23200288268666 in [23200279984402,23200298701197] | pub_starttime_ticks=2320028 sub_starttime_ticks=2320028 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=434 sub_pid=433 | pub_ready_ns=23202484780848 in [23202477955576,23202496540690] | sub_ready_ns=23202485131271 in [23202477814965,23202496540640] | pub_starttime_ticks=2320247 sub_starttime_ticks=2320247 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=453 sub_pid=452 | pub_ready_ns=23204681659629 in [23204675742519,23204692286596] | sub_ready_ns=23204683317471 in [23204675614440,23204692286548] | pub_starttime_ticks=2320467 sub_starttime_ticks=2320467 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=472 sub_pid=471 | pub_ready_ns=23206879627603 in [23206872522111,23206891108137] | sub_ready_ns=23206879730457 in [23206872390211,23206891108088] | pub_starttime_ticks=2320687 sub_starttime_ticks=2320687 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=491 sub_pid=490 | pub_ready_ns=23209078695707 in [23209070740241,23209089378134] | sub_ready_ns=23209079323568 in [23209070605829,23209089378080] | pub_starttime_ticks=2320907 sub_starttime_ticks=2320907 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=510 sub_pid=509 | pub_ready_ns=23211277855862 in [23211269352743,23211290074884] | sub_ready_ns=23211277301903 in [23211269224386,23211290074643] | pub_starttime_ticks=2321126 sub_starttime_ticks=2321126 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=529 sub_pid=528 | pub_ready_ns=23213476389970 in [23213467349787,23213487882127] | sub_ready_ns=23213475715336 in [23213467258031,23213487882085] | pub_starttime_ticks=2321346 sub_starttime_ticks=2321346 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=548 sub_pid=547 | pub_ready_ns=23215673771193 in [23215665832947,23215688485623] | sub_ready_ns=23215672032634 in [23215665739114,23215688485564] | pub_starttime_ticks=2321566 sub_starttime_ticks=2321566 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=567 sub_pid=566 | pub_ready_ns=23217876196032 in [23217867607250,23217890381162] | sub_ready_ns=23217876242025 in [23217867457418,23217890380953] | pub_starttime_ticks=2321786 sub_starttime_ticks=2321786 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=586 sub_pid=585 | pub_ready_ns=23220078444670 in [23220069447487,23220088118035] | sub_ready_ns=23220077777466 in [23220069300127,23220088117999] | pub_starttime_ticks=2322006 sub_starttime_ticks=2322006 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=605 sub_pid=604 | pub_ready_ns=23222271836918 in [23222262851078,23222283564358] | sub_ready_ns=23222274695579 in [23222262700698,23222283564292] | pub_starttime_ticks=2322226 sub_starttime_ticks=2322226 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=628 sub_pid=627 | pub_ready_ns=23224472341833 in [23224462843625,23224483546166] | sub_ready_ns=23224472396068 in [23224462699188,23224483546128] | pub_starttime_ticks=2322446 sub_starttime_ticks=2322446 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz` identity: parent_pid=10 pub_pid=651 sub_pid=650 | pub_ready_ns=23226676545425 in [23226663852505,23226688582906] | sub_ready_ns=23226677366988 in [23226663711065,23226688582847] | pub_starttime_ticks=2322666 sub_starttime_ticks=2322666 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=674 sub_pid=673 | pub_ready_ns=23228874840914 in [23228866423231,23228885014171] | sub_ready_ns=23228877561310 in [23228866278635,23228885014097] | pub_starttime_ticks=2322886 sub_starttime_ticks=2322886 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=697 sub_pid=696 | pub_ready_ns=23231076721633 in [23231065785213,23231088583368] | sub_ready_ns=23231077064359 in [23231065633551,23231088583305] | pub_starttime_ticks=2323106 sub_starttime_ticks=2323106 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=720 sub_pid=719 | pub_ready_ns=23233279641862 in [23233266575691,23233291393888] | sub_ready_ns=23233279981154 in [23233266439488,23233291393831] | pub_starttime_ticks=2323326 sub_starttime_ticks=2323326 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=743 sub_pid=742 | pub_ready_ns=23235486830436 in [23235469816344,23235498751496] | sub_ready_ns=23235484944716 in [23235469660649,23235498751438] | pub_starttime_ticks=2323546 sub_starttime_ticks=2323546 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` identity: parent_pid=10 pub_pid=766 sub_pid=765 | pub_ready_ns=23237689489274 in [23237675590694,23237700448115] | sub_ready_ns=23237687864201 in [23237675436614,23237700448059] | pub_starttime_ticks=2323767 sub_starttime_ticks=2323767 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz` identity: parent_pid=10 pub_pid=789 sub_pid=788 | pub_ready_ns=23239892225615 in [23239878731135,23239903647677] | sub_ready_ns=23239893128192 in [23239878577490,23239903647645] | pub_starttime_ticks=2323987 sub_starttime_ticks=2323987 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=812 sub_pid=811 | pub_ready_ns=23242097271247 in [23242083890089,23242108527162] | sub_ready_ns=23242096380155 in [23242083783496,23242108527141] | pub_starttime_ticks=2324208 sub_starttime_ticks=2324208 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=835 sub_pid=834 | pub_ready_ns=23244301695041 in [23244288754204,23244313631400] | sub_ready_ns=23244301721666 in [23244288601476,23244313631346] | pub_starttime_ticks=2324428 sub_starttime_ticks=2324428 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` identity: parent_pid=10 pub_pid=858 sub_pid=857 | pub_ready_ns=23246506425674 in [23246492688667,23246517598143] | sub_ready_ns=23246505271046 in [23246492536924,23246517598090] | pub_starttime_ticks=2324649 sub_starttime_ticks=2324649 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=881 sub_pid=880 | pub_ready_ns=23248710400071 in [23248698143606,23248712633699] | sub_ready_ns=23248706284764 in [23248697992463,23248712633631] | pub_starttime_ticks=2324869 sub_starttime_ticks=2324869 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=904 sub_pid=903 | pub_ready_ns=23250902678731 in [23250889771242,23250906281889] | sub_ready_ns=23250899028596 in [23250889620692,23250906281836] | pub_starttime_ticks=2325088 sub_starttime_ticks=2325088 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` identity: parent_pid=10 pub_pid=927 sub_pid=926 | pub_ready_ns=23253095333416 in [23253083886572,23253108665499] | sub_ready_ns=23253095876496 in [23253083731407,23253108665473] | pub_starttime_ticks=2325308 sub_starttime_ticks=2325308 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- `r0_tlv_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4021 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r1_tlv_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 18510 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r2_tlv_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 52171 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r3_tlv_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 93024 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r4_tlv_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 62889 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r5_tlv_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 81288 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r6_tlv_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 158500 ns（迟发 9 次）—— 该轮延迟含排队成分
- `r7_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 65885 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r8_tlv_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 124742 ns（迟发 4 次）—— 该轮延迟含排队成分
- `r9_tlv_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 107949 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r10_tlv_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 51328 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r11_tlv_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 39191 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r12_tlv_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 17559 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r13_tlv_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 97759 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r14_tlv_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 21551 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 98241 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r16_dzflat-a_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 48351 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r17_dzflat-a_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 149762 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 89359 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 8206 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r20_dzflat-a_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 67282 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r21_dzflat-a_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 126413 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r22_dzflat-a_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 59807 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r23_dzflat-a_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 17690 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 58506 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r25_dzflat-a_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 182165 ns（迟发 7 次）—— 该轮延迟含排队成分
- `r26_dzflat-a_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 28242 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 46853 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r28_dzflat-a_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 21650 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r29_dzflat-a_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 18521 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 70356 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r31_dzflat-b_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 28578 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r32_dzflat-b_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 56664 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 122809 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 5993 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r35_dzflat-b_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 187844 ns（迟发 7 次）—— 该轮延迟含排队成分
- `r36_dzflat-b_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6826 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r37_dzflat-b_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 209758 ns（迟发 9 次）—— 该轮延迟含排队成分
- `r38_dzflat-b_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 103468 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 3716 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r40_dzflat-b_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 70640 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r41_dzflat-b_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 24798 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 48608 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r43_dzflat-b_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 33284 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r44_dzflat-b_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 31441 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 14965 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 85644 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 6410 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 72493 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 46747 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 73151 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 52923 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 16437 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 91800 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 37398 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 65951 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 145511 ns（迟发 6 次）—— 该轮延迟含排队成分
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 90217 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 69521 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 103691 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 166551 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 19765 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 120579 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 108242 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 116147 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 73171 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 14627 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 79823 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 89405 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 39793 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 141663 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 127045 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 54052 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 47500 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5519 ns（迟发 0 次）—— 该轮延迟含排队成分

