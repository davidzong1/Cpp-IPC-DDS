# W02 统一跨进程基准 · 判定（verdict）

> run_id: `20260928-r20-W02`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `096b77d67b947b21946d5f6b17e7ba9d54cd48a9bba6688b5db50430cd202f51`；libipc sha256: `acde21e630e4c8f3ad4e44c930314ab524d0ba701f7c6a9f7f3ddcbcdd2850d3`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=10.1566 syscall_ns_per_call=83.7633 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

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
| `r1_tlv_crc_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2213 | `samples/r1_tlv_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r2_tlv_full_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r2_tlv_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | tlv | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2213 | `samples/r3_tlv_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | tlv | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r4_tlv_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r5_tlv_crc_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/2213 | `samples/r5_tlv_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r6_tlv_crc_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r6_tlv_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r7_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2212 | `samples/r7_tlv_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r8_tlv_full_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2213 | `samples/r8_tlv_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | tlv | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2108 | `samples/r9_tlv_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r10_tlv_crc_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2126 | `samples/r10_tlv_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r11_tlv_full_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/2136 | `samples/r11_tlv_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | tlv | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1112 | `samples/r12_tlv_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r13_tlv_crc_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1074 | `samples/r13_tlv_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r14_tlv_full_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/1087 | `samples/r14_tlv_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | dzflat-a | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2222/2 | `samples/r15_dzflat-a_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2214/2 | `samples/r16_dzflat-a_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2214/2 | `samples/r17_dzflat-a_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | dzflat-a | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2216/3 | `samples/r18_dzflat-a_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | dzflat-a | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2214/2 | `samples/r19_dzflat-a_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2211/3 | `samples/r20_dzflat-a_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2215/3 | `samples/r21_dzflat-a_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2213/3 | `samples/r22_dzflat-a_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2217/2 | `samples/r23_dzflat-a_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | dzflat-a | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2155/2 | `samples/r24_dzflat-a_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2159/2 | `samples/r25_dzflat-a_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2137/2 | `samples/r26_dzflat-a_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | dzflat-a | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1178/2 | `samples/r27_dzflat-a_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1182/3 | `samples/r28_dzflat-a_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1169/2 | `samples/r29_dzflat-a_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | dzflat-b | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2216/2 | `samples/r30_dzflat-b_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2217/2 | `samples/r31_dzflat-b_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2215/2 | `samples/r32_dzflat-b_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | dzflat-b | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 2220/3 | `samples/r33_dzflat-b_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | dzflat-b | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2215/2 | `samples/r34_dzflat-b_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2213/2 | `samples/r35_dzflat-b_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2217/2 | `samples/r36_dzflat-b_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2217/3 | `samples/r37_dzflat-b_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2213/2 | `samples/r38_dzflat-b_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | dzflat-b | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2148/3 | `samples/r39_dzflat-b_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2131/2 | `samples/r40_dzflat-b_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 2146/2 | `samples/r41_dzflat-b_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | dzflat-b | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1217/2 | `samples/r42_dzflat-b_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1232/2 | `samples/r43_dzflat-b_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 1219/2 | `samples/r44_dzflat-b_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | cyclonedds-udp | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r46_cyclonedds-udp_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r47_cyclonedds-udp_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0/0 | `samples/r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r50_cyclonedds-udp_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r51_cyclonedds-udp_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r52_cyclonedds-udp_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r53_cyclonedds-udp_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | cyclonedds-udp | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r55_cyclonedds-udp_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r56_cyclonedds-udp_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | cyclonedds-udp | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 7 | 0/0 | `samples/r58_cyclonedds-udp_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r59_cyclonedds-udp_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | cyclonedds-iox | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0/0 | `samples/r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r61_cyclonedds-iox_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r62_cyclonedds-iox_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r65_cyclonedds-iox_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0/0 | `samples/r66_cyclonedds-iox_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r67_cyclonedds-iox_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 9 | 0/0 | `samples/r68_cyclonedds-iox_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | cyclonedds-iox | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r70_cyclonedds-iox_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 4 | 0/0 | `samples/r71_cyclonedds-iox_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | cyclonedds-iox | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r73_cyclonedds-iox_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0/0 | `samples/r74_cyclonedds-iox_full_blocking_1048576B_500Hz.samples.csv` | **通过** |

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | 2566/13541 | 6747/29664 | 17/171 | 13655/40326 |
| `r1_tlv_crc_blocking_64B_1000Hz` | 2374/16060 | 6280/26768 | 19/183 | 10502/37044 |
| `r2_tlv_full_blocking_64B_1000Hz` | 3795/22088 | 8193/31208 | 19/139 | 12180/53068 |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | 4100/23466 | 8649/50257 | 32/184 | 13233/59386 |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | 3954/22091 | 5684/29316 | 11/75 | 10236/48892 |
| `r5_tlv_crc_blocking_1024B_1000Hz` | 10224/35203 | 10968/41636 | 20/151 | 22016/57836 |
| `r6_tlv_crc_busy_1024B_1000Hz` | 4346/21437 | 3624/36452 | 11/27 | 9384/48750 |
| `r7_tlv_full_blocking_1024B_1000Hz` | 2401/12317 | 9299/81418 | 31/195 | 18958/89276 |
| `r8_tlv_full_busy_1024B_1000Hz` | 2454/19629 | 5439/59280 | 11/22 | 10200/66918 |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | 16947/24174 | 16296/123519 | 11/92 | 62046/161933 |
| `r10_tlv_crc_blocking_65536B_1000Hz` | 9443/20829 | 16892/115267 | 14/142 | 60361/148910 |
| `r11_tlv_full_blocking_65536B_1000Hz` | 9062/19692 | 23797/97276 | 16/196 | 52926/132518 |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | 121707/175637 | 164474/286047 | 14/166 | 479856/658640 |
| `r13_tlv_crc_blocking_1048576B_500Hz` | 120197/200347 | 170867/234926 | 24/93 | 548777/794037 |
| `r14_tlv_full_blocking_1048576B_500Hz` | 122258/226300 | 118850/216046 | 13/97 | 415606/691835 |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | 3394/12888 | 4195/21569 | 56/336 | 7834/27521 |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | 3124/9268 | 7701/18386 | 192/612 | 11505/26275 |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | 3259/13809 | 4506/22966 | 106/633 | 8203/30921 |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | 3115/12670 | 7243/20342 | 104/390 | 10272/27935 |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | 2971/11234 | 5001/13905 | 85/161 | 8426/20650 |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | 3583/8826 | 5301/23380 | 239/922 | 9716/29844 |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | 3411/9394 | 5195/15105 | 183/607 | 9468/19735 |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | 3406/12760 | 8649/21048 | 526/1755 | 13421/33747 |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | 3151/13476 | 5148/14733 | 313/773 | 9182/26401 |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | 3326/13132 | 4468/20404 | 80/292 | 19984/81653 |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | 4083/12264 | 5486/24194 | 5853/42862 | 41380/100773 |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | 4581/12199 | 6921/22731 | 11860/83492 | 42499/134955 |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | 27585/61632 | 5403/22185 | 96/401 | 205826/359177 |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | 31174/93449 | 4255/22453 | 89891/167693 | 382165/566049 |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | 31395/92645 | 4388/23621 | 185865/267709 | 390878/574502 |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | 2602/8203 | 7074/22420 | 64/399 | 11048/30130 |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | 2543/8005 | 4046/20248 | 63/393 | 7605/25293 |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | 2252/7761 | 4319/21893 | 106/606 | 8184/28390 |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | 2743/13459 | 8600/23831 | 110/343 | 12826/34118 |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | 1839/12211 | 3072/18021 | 104/239 | 7300/27400 |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | 2594/10539 | 8113/16760 | 277/783 | 11550/27459 |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | 2897/9082 | 4946/13678 | 197/583 | 8875/20346 |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | 2649/7568 | 8316/20578 | 495/1645 | 12568/26013 |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | 2551/7927 | 4733/15035 | 314/688 | 8484/21772 |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | 21444/57638 | 6148/22656 | 118/374 | 29222/65445 |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | 22220/86847 | 5870/28329 | 7591/43567 | 44903/112757 |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | 12888/63288 | 4081/26698 | 12776/78903 | 36977/120429 |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | 204060/316517 | 4428/20086 | 85/388 | 211169/330069 |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | 322369/371794 | 4526/22449 | 89892/161530 | 417950/512009 |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | 176444/314587 | 5750/23955 | 332409/349183 | 512338/587385 |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | 5734/38351 | 3749/26963 | 11/52 | 10023/54767 |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | 5560/35757 | 3866/27800 | 11/55 | 9724/53684 |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | 11603/42064 | 7382/24091 | 37/110 | 20332/54872 |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | 12248/49959 | 7616/16487 | 11/45 | 20642/60515 |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | 9338/53565 | 4187/28621 | 11/118 | 24249/69640 |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | 7529/42474 | 4017/29551 | 11/53 | 23090/63972 |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | 11361/40811 | 8879/37369 | 11/58 | 20872/68375 |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | 11904/51324 | 7897/17874 | 293/886 | 20818/61964 |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | 11521/59529 | 8007/29981 | 296/936 | 20614/87179 |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | 46297/117721 | 9248/78212 | 12/52 | 61947/176627 |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | 65939/131991 | 26184/138420 | 14/92 | 92736/213355 |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | 44885/115780 | 8942/81244 | 17533/18316 | 73261/194548 |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | 778642/1303144 | 103408/463361 | 21/94 | 934424/1707127 |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | 794993/1464945 | 102752/486515 | 13/115 | 940533/1618693 |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | 680079/949913 | 89154/328948 | 280146/288508 | 1196764/1536585 |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | 4971/25240 | 5517/62547 | 11/50 | 12373/80692 |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | 5025/30780 | 5584/25898 | 11/24 | 11145/51740 |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | 5109/26331 | 6830/35833 | 36/145 | 13910/52636 |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | 5877/33426 | 9328/30980 | 11/81 | 18389/54135 |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | 4873/25969 | 6715/122840 | 11/74 | 12043/140556 |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | 5107/31741 | 4068/125388 | 11/83 | 10161/128350 |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | 5392/27692 | 7293/54942 | 11/47 | 14989/69042 |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | 5105/32370 | 5185/33443 | 287/480 | 12184/51074 |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | 8887/45439 | 6717/37142 | 284/445 | 15724/53894 |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | 31854/84349 | 10459/30853 | 14/56 | 42631/96869 |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | 32139/101961 | 9378/32454 | 13/81 | 43177/120670 |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | 36563/118896 | 9578/124934 | 17833/22968 | 66097/184578 |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | 297801/506477 | 53215/83188 | 14/49 | 354350/559830 |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | 276554/484974 | 55788/120097 | 21/96 | 340746/564959 |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | 277735/471305 | 55742/113135 | 285124/960731 | 627194/1367557 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

（无）

## 5. 跨进程身份与正常退出

- `r0_tlv_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=48 sub_pid=47 | pub_ready_ns=19390224618772 in [19390221179414,19390274539877] | sub_ready_ns=19390224334203 in [19390221107347,19390274539811] | pub_starttime_ticks=1939022 sub_starttime_ticks=1939022 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r1_tlv_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=54 sub_pid=53 | pub_ready_ns=19392049677267 in [19392044905879,19392100383983] | sub_ready_ns=19392048714772 in [19392044825469,19392100383926] | pub_starttime_ticks=1939204 sub_starttime_ticks=1939204 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r2_tlv_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=60 sub_pid=59 | pub_ready_ns=19393873403053 in [19393869601874,19393925061796] | sub_ready_ns=19393873466399 in [19393869510577,19393925061642] | pub_starttime_ticks=1939386 sub_starttime_ticks=1939386 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r3_tlv_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=66 sub_pid=65 | pub_ready_ns=19395699472708 in [19395694644803,19395750156940] | sub_ready_ns=19395698506866 in [19395694556500,19395750156920] | pub_starttime_ticks=1939569 sub_starttime_ticks=1939569 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r4_tlv_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=72 sub_pid=71 | pub_ready_ns=19397524024861 in [19397519218301,19397574697703] | sub_ready_ns=19397523065541 in [19397519128433,19397574697669] | pub_starttime_ticks=1939751 sub_starttime_ticks=1939751 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r5_tlv_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=78 sub_pid=77 | pub_ready_ns=19399349274869 in [19399344512893,19399399948233] | sub_ready_ns=19399348325591 in [19399344424908,19399399948174] | pub_starttime_ticks=1939934 sub_starttime_ticks=1939934 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r6_tlv_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=84 sub_pid=83 | pub_ready_ns=19401176596755 in [19401171767492,19401227280943] | sub_ready_ns=19401175618321 in [19401171644653,19401227280890] | pub_starttime_ticks=1940117 sub_starttime_ticks=1940117 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r7_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=90 sub_pid=89 | pub_ready_ns=19403001592054 in [19402998109837,19403051483343] | sub_ready_ns=19403000683286 in [19402998011297,19403051483297] | pub_starttime_ticks=1940299 sub_starttime_ticks=1940299 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r8_tlv_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=96 sub_pid=95 | pub_ready_ns=19404826545909 in [19404822018984,19404877512289] | sub_ready_ns=19404825855678 in [19404821923032,19404877512228] | pub_starttime_ticks=1940482 sub_starttime_ticks=1940482 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r9_tlv_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=102 sub_pid=101 | pub_ready_ns=19406654298910 in [19406649518388,19406705064779] | sub_ready_ns=19406653300243 in [19406649398778,19406705064675] | pub_starttime_ticks=1940664 sub_starttime_ticks=1940664 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r10_tlv_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=108 sub_pid=107 | pub_ready_ns=19408479030321 in [19408473977104,19408529439329] | sub_ready_ns=19408477927124 in [19408473889318,19408529439289] | pub_starttime_ticks=1940847 sub_starttime_ticks=1940847 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r11_tlv_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=114 sub_pid=113 | pub_ready_ns=19410303429676 in [19410299783894,19410353226120] | sub_ready_ns=19410302946504 in [19410299695561,19410353226074] | pub_starttime_ticks=1941029 sub_starttime_ticks=1941029 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r12_tlv_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=120 sub_pid=119 | pub_ready_ns=19412128954661 in [19412123572212,19412179074248] | sub_ready_ns=19412127250622 in [19412123489330,19412179074035] | pub_starttime_ticks=1941212 sub_starttime_ticks=1941212 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r13_tlv_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=126 sub_pid=125 | pub_ready_ns=19413955362177 in [19413949579816,19414004944184] | sub_ready_ns=19413953696667 in [19413949433637,19414004944153] | pub_starttime_ticks=1941394 sub_starttime_ticks=1941394 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r14_tlv_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=132 sub_pid=131 | pub_ready_ns=19415780420690 in [19415775343976,19415830756892] | sub_ready_ns=19415779390429 in [19415775200524,19415830756858] | pub_starttime_ticks=1941577 sub_starttime_ticks=1941577 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=138 sub_pid=137 | pub_ready_ns=19417607808933 in [19417602875419,19417658272384] | sub_ready_ns=19417606854223 in [19417602740131,19417658272352] | pub_starttime_ticks=1941760 sub_starttime_ticks=1941760 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r16_dzflat-a_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=144 sub_pid=143 | pub_ready_ns=19419454112028 in [19419449231556,19419504542568] | sub_ready_ns=19419453177842 in [19419449092539,19419504542518] | pub_starttime_ticks=1941944 sub_starttime_ticks=1941944 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r17_dzflat-a_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=150 sub_pid=149 | pub_ready_ns=19421300965353 in [19421297293640,19421350646107] | sub_ready_ns=19421299875707 in [19421297159800,19421350646075] | pub_starttime_ticks=1942129 sub_starttime_ticks=1942129 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=156 sub_pid=155 | pub_ready_ns=19423146447594 in [19423142927075,19423198312957] | sub_ready_ns=19423146402769 in [19423142838203,19423198312923] | pub_starttime_ticks=1942314 sub_starttime_ticks=1942314 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=162 sub_pid=161 | pub_ready_ns=19424990770979 in [19424987202005,19425040424596] | sub_ready_ns=19424989807037 in [19424987109124,19425040424554] | pub_starttime_ticks=1942498 sub_starttime_ticks=1942498 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r20_dzflat-a_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=168 sub_pid=167 | pub_ready_ns=19426814959355 in [19426812150056,19426865493791] | sub_ready_ns=19426815259026 in [19426812065701,19426865493743] | pub_starttime_ticks=1942681 sub_starttime_ticks=1942681 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r21_dzflat-a_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=174 sub_pid=173 | pub_ready_ns=19428661049178 in [19428658328796,19428711697626] | sub_ready_ns=19428661517560 in [19428658188990,19428711697572] | pub_starttime_ticks=1942865 sub_starttime_ticks=1942865 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r22_dzflat-a_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=180 sub_pid=179 | pub_ready_ns=19430487312522 in [19430484355760,19430537744202] | sub_ready_ns=19430487550354 in [19430484215349,19430537744163] | pub_starttime_ticks=1943048 sub_starttime_ticks=1943048 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r23_dzflat-a_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=186 sub_pid=185 | pub_ready_ns=19432332409826 in [19432328580052,19432383997061] | sub_ready_ns=19432332151413 in [19432328484707,19432383997024] | pub_starttime_ticks=1943232 sub_starttime_ticks=1943232 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=192 sub_pid=191 | pub_ready_ns=19434156132651 in [19434153024492,19434206380597] | sub_ready_ns=19434155314050 in [19434152922000,19434206380566] | pub_starttime_ticks=1943415 sub_starttime_ticks=1943415 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r25_dzflat-a_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=198 sub_pid=197 | pub_ready_ns=19436001121307 in [19435996152320,19436051568858] | sub_ready_ns=19436000133767 in [19435996055553,19436051568838] | pub_starttime_ticks=1943599 sub_starttime_ticks=1943599 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r26_dzflat-a_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=204 sub_pid=203 | pub_ready_ns=19437846972911 in [19437842284369,19437897541903] | sub_ready_ns=19437846010236 in [19437842199371,19437897541882] | pub_starttime_ticks=1943784 sub_starttime_ticks=1943784 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=210 sub_pid=209 | pub_ready_ns=19439694954048 in [19439689298665,19439744769979] | sub_ready_ns=19439693298945 in [19439689158897,19439744769944] | pub_starttime_ticks=1943968 sub_starttime_ticks=1943968 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r28_dzflat-a_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=216 sub_pid=215 | pub_ready_ns=19441534592574 in [19441531468593,19441586872432] | sub_ready_ns=19441535035789 in [19441531372167,19441586872400] | pub_starttime_ticks=1944153 sub_starttime_ticks=1944153 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r29_dzflat-a_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=222 sub_pid=221 | pub_ready_ns=19443380226297 in [19443375582049,19443430993321] | sub_ready_ns=19443378789233 in [19443375493154,19443430993291] | pub_starttime_ticks=1944337 sub_starttime_ticks=1944337 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=228 sub_pid=227 | pub_ready_ns=19445227504749 in [19445222485512,19445277914729] | sub_ready_ns=19445226568106 in [19445222340849,19445277914698] | pub_starttime_ticks=1944522 sub_starttime_ticks=1944522 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r31_dzflat-b_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=234 sub_pid=233 | pub_ready_ns=19447075078489 in [19447070044319,19447125486421] | sub_ready_ns=19447074138705 in [19447069901039,19447125486387] | pub_starttime_ticks=1944707 sub_starttime_ticks=1944707 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r32_dzflat-b_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=240 sub_pid=239 | pub_ready_ns=19448922670376 in [19448917653223,19448973111699] | sub_ready_ns=19448921715777 in [19448917493276,19448973111679] | pub_starttime_ticks=1944891 sub_starttime_ticks=1944891 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=246 sub_pid=245 | pub_ready_ns=19450766148213 in [19450763551020,19450816801777] | sub_ready_ns=19450766501630 in [19450763450620,19450816801755] | pub_starttime_ticks=1945076 sub_starttime_ticks=1945076 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=252 sub_pid=251 | pub_ready_ns=19452613377283 in [19452608306929,19452663742104] | sub_ready_ns=19452612435262 in [19452608150857,19452663742059] | pub_starttime_ticks=1945260 sub_starttime_ticks=1945260 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r35_dzflat-b_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=258 sub_pid=257 | pub_ready_ns=19454440115056 in [19454435213641,19454490590235] | sub_ready_ns=19454439167162 in [19454435071799,19454490590174] | pub_starttime_ticks=1945443 sub_starttime_ticks=1945443 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r36_dzflat-b_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=264 sub_pid=263 | pub_ready_ns=19456285506933 in [19456281910914,19456335108657] | sub_ready_ns=19456284762919 in [19456281814508,19456335108617] | pub_starttime_ticks=1945628 sub_starttime_ticks=1945628 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r37_dzflat-b_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=270 sub_pid=269 | pub_ready_ns=19458109823573 in [19458106832074,19458160165641] | sub_ready_ns=19458110048526 in [19458106689402,19458160165585] | pub_starttime_ticks=1945810 sub_starttime_ticks=1945810 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r38_dzflat-b_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=276 sub_pid=275 | pub_ready_ns=19459953876598 in [19459950278441,19460003520620] | sub_ready_ns=19459953253482 in [19459950181669,19460003520580] | pub_starttime_ticks=1945995 sub_starttime_ticks=1945995 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=282 sub_pid=281 | pub_ready_ns=19461779836079 in [19461776900791,19461830274931] | sub_ready_ns=19461780108051 in [19461776753510,19461830274882] | pub_starttime_ticks=1946177 sub_starttime_ticks=1946177 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r40_dzflat-b_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=288 sub_pid=287 | pub_ready_ns=19463627086940 in [19463622101814,19463677590647] | sub_ready_ns=19463626114430 in [19463621946808,19463677590614] | pub_starttime_ticks=1946362 sub_starttime_ticks=1946362 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r41_dzflat-b_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=294 sub_pid=293 | pub_ready_ns=19465473558026 in [19465468606027,19465523969495] | sub_ready_ns=19465472570005 in [19465468504954,19465523969433] | pub_starttime_ticks=1946546 sub_starttime_ticks=1946546 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=300 sub_pid=299 | pub_ready_ns=19467320506158 in [19467315178453,19467370531815] | sub_ready_ns=19467319144357 in [19467315069222,19467370531760] | pub_starttime_ticks=1946731 sub_starttime_ticks=1946731 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r43_dzflat-b_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=306 sub_pid=305 | pub_ready_ns=19469169058819 in [19469163306546,19469218786769] | sub_ready_ns=19469167355240 in [19469163194334,19469218786618] | pub_starttime_ticks=1946916 sub_starttime_ticks=1946916 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r44_dzflat-b_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=312 sub_pid=311 | pub_ready_ns=19471017749966 in [19471012483124,19471067901468] | sub_ready_ns=19471016504301 in [19471012330099,19471067901438] | pub_starttime_ticks=1947101 sub_starttime_ticks=1947101 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=321 sub_pid=320 | pub_ready_ns=19472869398287 in [19472863891777,19472880326521] | sub_ready_ns=19472868338949 in [19472863752116,19472880326470] | pub_starttime_ticks=1947286 sub_starttime_ticks=1947286 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=340 sub_pid=339 | pub_ready_ns=19475064438709 in [19475058526022,19475075007520] | sub_ready_ns=19475064014862 in [19475058377615,19475075007488] | pub_starttime_ticks=1947505 sub_starttime_ticks=1947505 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=359 sub_pid=358 | pub_ready_ns=19477262843103 in [19477255701560,19477274270194] | sub_ready_ns=19477261429359 in [19477255563127,19477274270141] | pub_starttime_ticks=1947725 sub_starttime_ticks=1947725 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=378 sub_pid=377 | pub_ready_ns=19479460331984 in [19479452737471,19479471270472] | sub_ready_ns=19479460335375 in [19479452590177,19479471270441] | pub_starttime_ticks=1947945 sub_starttime_ticks=1947945 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=397 sub_pid=396 | pub_ready_ns=19481658730724 in [19481651363912,19481669920427] | sub_ready_ns=19481658731550 in [19481651223164,19481669920369] | pub_starttime_ticks=1948165 sub_starttime_ticks=1948165 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=416 sub_pid=415 | pub_ready_ns=19483854885793 in [19483849923386,19483866451572] | sub_ready_ns=19483856368499 in [19483849779206,19483866451516] | pub_starttime_ticks=1948385 sub_starttime_ticks=1948384 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=435 sub_pid=434 | pub_ready_ns=19486051150637 in [19486045093756,19486061608694] | sub_ready_ns=19486051149271 in [19486044952037,19486061608637] | pub_starttime_ticks=1948604 sub_starttime_ticks=1948604 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=454 sub_pid=453 | pub_ready_ns=19488247449616 in [19488242654441,19488259163024] | sub_ready_ns=19488247893135 in [19488242517122,19488259162963] | pub_starttime_ticks=1948824 sub_starttime_ticks=1948824 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=473 sub_pid=472 | pub_ready_ns=19490447488466 in [19490439914134,19490458487192] | sub_ready_ns=19490447494730 in [19490439770779,19490458487132] | pub_starttime_ticks=1949043 sub_starttime_ticks=1949043 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=492 sub_pid=491 | pub_ready_ns=19492651637058 in [19492635408945,19492662177241] | sub_ready_ns=19492649055123 in [19492635316909,19492662177218] | pub_starttime_ticks=1949263 sub_starttime_ticks=1949263 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=511 sub_pid=510 | pub_ready_ns=19494846377109 in [19494840888656,19494857383253] | sub_ready_ns=19494846595123 in [19494840748026,19494857383213] | pub_starttime_ticks=1949484 sub_starttime_ticks=1949484 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=530 sub_pid=529 | pub_ready_ns=19497046134760 in [19497037531020,19497058291800] | sub_ready_ns=19497046042305 in [19497037381913,19497058291743] | pub_starttime_ticks=1949703 sub_starttime_ticks=1949703 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=549 sub_pid=548 | pub_ready_ns=19499245214616 in [19499237715430,19499254298460] | sub_ready_ns=19499245049571 in [19499237566249,19499254298407] | pub_starttime_ticks=1949923 sub_starttime_ticks=1949923 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=568 sub_pid=567 | pub_ready_ns=19501435409228 in [19501428092950,19501444622210] | sub_ready_ns=19501435355008 in [19501427923010,19501444622151] | pub_starttime_ticks=1950142 sub_starttime_ticks=1950142 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=587 sub_pid=586 | pub_ready_ns=19503627912677 in [19503618833043,19503637402244] | sub_ready_ns=19503627267925 in [19503618677213,19503637402183] | pub_starttime_ticks=1950361 sub_starttime_ticks=1950361 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=606 sub_pid=605 | pub_ready_ns=19505822296759 in [19505809254507,19505833953852] | sub_ready_ns=19505822336009 in [19505809097738,19505833953795] | pub_starttime_ticks=1950580 sub_starttime_ticks=1950580 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=629 sub_pid=628 | pub_ready_ns=19508024975316 in [19508012122962,19508036845298] | sub_ready_ns=19508025020651 in [19508011962790,19508036845231] | pub_starttime_ticks=1950801 sub_starttime_ticks=1950801 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=652 sub_pid=651 | pub_ready_ns=19510227633243 in [19510216055175,19510238746320] | sub_ready_ns=19510225815597 in [19510215901319,19510238746262] | pub_starttime_ticks=1951021 sub_starttime_ticks=1951021 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=675 sub_pid=674 | pub_ready_ns=19512437529473 in [19512424678706,19512449458079] | sub_ready_ns=19512438525396 in [19512424521171,19512449458024] | pub_starttime_ticks=1951242 sub_starttime_ticks=1951242 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=698 sub_pid=697 | pub_ready_ns=19514634895933 in [19514628141895,19514646722001] | sub_ready_ns=19514634152936 in [19514627960527,19514646721975] | pub_starttime_ticks=1951462 sub_starttime_ticks=1951462 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=721 sub_pid=720 | pub_ready_ns=19516834905430 in [19516823280918,19516845950885] | sub_ready_ns=19516834573834 in [19516823182498,19516845950677] | pub_starttime_ticks=1951682 sub_starttime_ticks=1951682 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=744 sub_pid=743 | pub_ready_ns=19519031178577 in [19519025515642,19519042002637] | sub_ready_ns=19519032114455 in [19519025416693,19519042002617] | pub_starttime_ticks=1951902 sub_starttime_ticks=1951902 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=767 sub_pid=766 | pub_ready_ns=19521229824576 in [19521219332607,19521241901390] | sub_ready_ns=19521228338303 in [19521219235939,19521241901358] | pub_starttime_ticks=1952121 sub_starttime_ticks=1952121 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=790 sub_pid=789 | pub_ready_ns=19523429794154 in [19523420916208,19523441536304] | sub_ready_ns=19523428800939 in [19523420764805,19523441536244] | pub_starttime_ticks=1952342 sub_starttime_ticks=1952342 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=813 sub_pid=812 | pub_ready_ns=19525631939500 in [19525621739557,19525642326718] | sub_ready_ns=19525631442945 in [19525621587533,19525642326663] | pub_starttime_ticks=1952562 sub_starttime_ticks=1952562 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=836 sub_pid=835 | pub_ready_ns=19527830979751 in [19527822765271,19527841347199] | sub_ready_ns=19527831013891 in [19527822614750,19527841347014] | pub_starttime_ticks=1952782 sub_starttime_ticks=1952782 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=859 sub_pid=858 | pub_ready_ns=19530028362185 in [19530021124636,19530039728289] | sub_ready_ns=19530028997195 in [19530020963690,19530039728267] | pub_starttime_ticks=1953002 sub_starttime_ticks=1953002 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=882 sub_pid=881 | pub_ready_ns=19532229997295 in [19532218985187,19532243853155] | sub_ready_ns=19532231457072 in [19532218816048,19532243852973] | pub_starttime_ticks=1953221 sub_starttime_ticks=1953221 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=905 sub_pid=904 | pub_ready_ns=19534432369607 in [19534419716903,19534436221824] | sub_ready_ns=19534432347181 in [19534419564598,19534436221777] | pub_starttime_ticks=1953441 sub_starttime_ticks=1953441 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=928 sub_pid=927 | pub_ready_ns=19536624500348 in [19536614030565,19536638703067] | sub_ready_ns=19536625574940 in [19536613860781,19536638703004] | pub_starttime_ticks=1953661 sub_starttime_ticks=1953661 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- `r0_tlv_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4560 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r1_tlv_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5385 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r2_tlv_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5809 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r3_tlv_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 54711 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r4_tlv_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 3822 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r5_tlv_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 351988 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r6_tlv_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 44538 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r7_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 55379 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r8_tlv_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 36638 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r9_tlv_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 54157 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r10_tlv_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 7258 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r11_tlv_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 6871 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r12_tlv_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 116074 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r13_tlv_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4960 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r14_tlv_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4631 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 20930 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r16_dzflat-a_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4483 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r17_dzflat-a_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5681 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 6618 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6351 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r20_dzflat-a_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 39939 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r21_dzflat-a_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 3908 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r22_dzflat-a_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 4730 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r23_dzflat-a_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 66265 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4941 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r25_dzflat-a_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4509 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r26_dzflat-a_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 6119 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4350 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r28_dzflat-a_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 16682 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r29_dzflat-a_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4884 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 6337 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r31_dzflat-b_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4905 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r32_dzflat-b_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 4859 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 102661 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 55584 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r35_dzflat-b_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 7321 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r36_dzflat-b_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 5934 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r37_dzflat-b_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 4217 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r38_dzflat-b_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6109 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 7237 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r40_dzflat-b_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 23635 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r41_dzflat-b_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5265 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 20679 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r43_dzflat-b_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4572 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r44_dzflat-b_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4588 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5770 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 14437 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 14328 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 6032 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 197500 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 25241 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 3996 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 5712 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 94762 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 313667 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 89506 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 26820 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 9240 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 7958162 ns（迟发 7 次）—— 该轮延迟含排队成分
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4479 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 141010 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 19498 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 25653 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 44808 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 68936 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 63711 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 101814 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 80165 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 177206 ns（迟发 9 次）—— 该轮延迟含排队成分
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 65065 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 69712 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 1447652 ns（迟发 4 次）—— 该轮延迟含排队成分
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 104388 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4966 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: wire 字节未采集（见 case 的 wire_bytes_source）
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 147948 ns（迟发 0 次）—— 该轮延迟含排队成分

