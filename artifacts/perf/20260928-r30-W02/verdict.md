# W02 统一跨进程基准 · 判定（verdict）

> run_id: `20260928-r30-W02`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `7d77172381ad3ce44ee94dcf783dabe023637125070cfc979e3c94903f4ee616`；libipc sha256: `cf209393773d51ee4762dd95bfd1b04f873ec54aa8f61cf5a7ed92be2990fa46`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=10.151 syscall_ns_per_call=81.4185 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

## 1. 机器判定摘要

```
process_model : cross-process (pub/sub 均为 fork+exec 的新映像)
config_hash   : 19c46fe0c69ac096
payload_shapes: 64B(实际 63B + 头 32B), 1024B(实际 1021B + 头 32B), 65536B(实际 65533B + 头 32B), 1048576B(实际 1048573B + 头 32B)
cases         : 75 通过 75 / 未通过 0
```

## 2. 逐用例判定（每行引用原始文件）

> 判定来源：本节所有「判定」列**只读** `run_one_case()` 里唯一一处判定逻辑写下的 `failure_reasons`/gate 布尔；⛔ 本渲染不重算任何判据（纪律：同一事实只有一处判定逻辑）。

| case | 路径 | 组 | 载荷 | 等待 | 计划/尝试/成功/失败 | 接收 | 丢失 | 重复 | 乱序 | 校验失败 | 迟发 | 积压(周期) | 结构异常 | 段头矛盾 | gate(late/backlog/blocked/abn/hdr) | dzflat/回退 | 文件 | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | tlv | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0.124 | 0 | 0 | 0/0/0/0/0 | 0/2207 | `samples/r0_tlv_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r1_tlv_crc_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.028 | 0 | 0 | 0/0/0/0/0 | 0/2203 | `samples/r1_tlv_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r2_tlv_full_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.054 | 0 | 0 | 0/0/0/0/0 | 0/2206 | `samples/r2_tlv_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | tlv | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.033 | 0 | 0 | 0/0/0/0/0 | 0/2205 | `samples/r3_tlv_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | tlv | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.154 | 0 | 0 | 0/0/0/0/0 | 0/2207 | `samples/r4_tlv_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r5_tlv_crc_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.044 | 0 | 0 | 0/0/0/0/0 | 0/2205 | `samples/r5_tlv_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r6_tlv_crc_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.091 | 0 | 0 | 0/0/0/0/0 | 0/2202 | `samples/r6_tlv_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r7_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.056 | 0 | 0 | 0/0/0/0/0 | 0/2201 | `samples/r7_tlv_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r8_tlv_full_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.018 | 0 | 0 | 0/0/0/0/0 | 0/2205 | `samples/r8_tlv_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | tlv | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.034 | 0 | 0 | 0/0/0/0/0 | 0/2120 | `samples/r9_tlv_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r10_tlv_crc_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 0/2129 | `samples/r10_tlv_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r11_tlv_full_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.008 | 0 | 0 | 0/0/0/0/0 | 0/2085 | `samples/r11_tlv_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | tlv | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 0/1122 | `samples/r12_tlv_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r13_tlv_crc_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.007 | 0 | 0 | 0/0/0/0/0 | 0/1116 | `samples/r13_tlv_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r14_tlv_full_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 0/1120 | `samples/r14_tlv_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | dzflat-a | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.038 | 0 | 0 | 0/0/0/0/0 | 2203/3 | `samples/r15_dzflat-a_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.023 | 0 | 0 | 0/0/0/0/0 | 2204/3 | `samples/r16_dzflat-a_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.007 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r17_dzflat-a_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | dzflat-a | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0.129 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r18_dzflat-a_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | dzflat-a | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.004 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r19_dzflat-a_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.023 | 0 | 0 | 0/0/0/0/0 | 2204/3 | `samples/r20_dzflat-a_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.114 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r21_dzflat-a_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.053 | 0 | 0 | 0/0/0/0/0 | 2201/2 | `samples/r22_dzflat-a_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.086 | 0 | 0 | 0/0/0/0/0 | 2204/3 | `samples/r23_dzflat-a_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | dzflat-a | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.064 | 0 | 0 | 0/0/0/0/0 | 2145/2 | `samples/r24_dzflat-a_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.015 | 0 | 0 | 0/0/0/0/0 | 2129/2 | `samples/r25_dzflat-a_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2130/2 | `samples/r26_dzflat-a_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | dzflat-a | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.002 | 0 | 0 | 0/0/0/0/0 | 1178/2 | `samples/r27_dzflat-a_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 1146/2 | `samples/r28_dzflat-a_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.002 | 0 | 0 | 0/0/0/0/0 | 1170/2 | `samples/r29_dzflat-a_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | dzflat-b | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.053 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r30_dzflat-b_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.012 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r31_dzflat-b_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.045 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r32_dzflat-b_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | dzflat-b | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.030 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r33_dzflat-b_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | dzflat-b | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.070 | 0 | 0 | 0/0/0/0/0 | 2201/2 | `samples/r34_dzflat-b_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.018 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r35_dzflat-b_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.054 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r36_dzflat-b_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.023 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r37_dzflat-b_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.069 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r38_dzflat-b_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | dzflat-b | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.035 | 0 | 0 | 0/0/0/0/0 | 2089/2 | `samples/r39_dzflat-b_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 2103/2 | `samples/r40_dzflat-b_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2081/2 | `samples/r41_dzflat-b_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | dzflat-b | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.033 | 0 | 0 | 0/0/0/0/0 | 1020/2 | `samples/r42_dzflat-b_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 1007/2 | `samples/r43_dzflat-b_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.029 | 0 | 0 | 0/0/0/0/0 | 1016/2 | `samples/r44_dzflat-b_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | cyclonedds-udp | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.084 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.050 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r46_cyclonedds-udp_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.162 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r47_cyclonedds-udp_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.108 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.074 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.043 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r50_cyclonedds-udp_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.059 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r51_cyclonedds-udp_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.026 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r52_cyclonedds-udp_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.018 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r53_cyclonedds-udp_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | cyclonedds-udp | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 11 | 0.164 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.032 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r55_cyclonedds-udp_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.091 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r56_cyclonedds-udp_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | cyclonedds-udp | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 8 | 0.204 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.028 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r58_cyclonedds-udp_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 9 | 3.999 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r59_cyclonedds-udp_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | cyclonedds-iox | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0.116 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.079 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r61_cyclonedds-iox_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.016 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r62_cyclonedds-iox_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.106 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.061 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.149 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r65_cyclonedds-iox_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.007 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r66_cyclonedds-iox_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.055 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r67_cyclonedds-iox_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.142 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r68_cyclonedds-iox_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | cyclonedds-iox | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.072 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.050 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r70_cyclonedds-iox_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.069 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r71_cyclonedds-iox_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | cyclonedds-iox | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 13 | 2.036 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.072 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r73_cyclonedds-iox_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.020 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r74_cyclonedds-iox_full_blocking_1048576B_500Hz.samples.csv` | **通过** |

（gate 列 = 「该 gate 判失败的次数」，0 = 通过；阈值见 `manifest.failure_thresholds`。所列为**判定结果**，不是原始量 —— 原始量在同行的迟发/积压/结构异常/段头矛盾列。）

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | 4780/27961 | 13643/154386 | 28/294 | 20030/164720 |
| `r1_tlv_crc_blocking_64B_1000Hz` | 3643/19388 | 8858/80240 | 25/161 | 14792/88384 |
| `r2_tlv_full_blocking_64B_1000Hz` | 3747/17676 | 9048/84067 | 21/141 | 16643/95151 |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | 3686/20930 | 9345/116437 | 24/381 | 15347/120919 |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | 8583/34418 | 7724/50613 | 11/71 | 17046/60122 |
| `r5_tlv_crc_blocking_1024B_1000Hz` | 4038/19424 | 13645/94302 | 19/323 | 21554/98252 |
| `r6_tlv_crc_busy_1024B_1000Hz` | 8513/33938 | 8203/89358 | 11/53 | 17625/100873 |
| `r7_tlv_full_blocking_1024B_1000Hz` | 3480/20661 | 10077/57246 | 13/138 | 17151/66707 |
| `r8_tlv_full_busy_1024B_1000Hz` | 4212/21075 | 4596/117653 | 11/92 | 12240/122883 |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | 13958/23302 | 20046/86460 | 20/110 | 54635/127465 |
| `r10_tlv_crc_blocking_65536B_1000Hz` | 11522/21201 | 24332/120539 | 19/140 | 67231/160705 |
| `r11_tlv_full_blocking_65536B_1000Hz` | 8957/16198 | 18581/75310 | 20/182 | 49445/122065 |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | 123406/174990 | 115316/182769 | 24/227 | 414229/528172 |
| `r13_tlv_crc_blocking_1048576B_500Hz` | 112961/203974 | 130411/254583 | 22/151 | 520375/807060 |
| `r14_tlv_full_blocking_1048576B_500Hz` | 115623/190946 | 180387/282367 | 14/168 | 476674/686023 |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | 3012/14560 | 14667/94166 | 167/588 | 18842/98366 |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | 2043/11520 | 5385/31735 | 154/823 | 10903/35820 |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | 1892/10417 | 7127/48626 | 134/633 | 12040/55681 |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | 1772/26570 | 5763/81174 | 126/595 | 10281/82590 |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | 1950/12083 | 4314/103155 | 101/363 | 10085/112985 |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | 2877/15557 | 9572/106449 | 303/1326 | 13674/112220 |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | 1889/10937 | 4272/49875 | 194/685 | 10796/58643 |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | 6266/25203 | 13544/109051 | 644/3899 | 21893/119804 |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | 2993/14890 | 6198/56241 | 296/723 | 11216/69506 |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | 4751/11941 | 9307/101472 | 164/942 | 35772/137384 |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | 4652/8859 | 9075/63169 | 7864/42914 | 42056/127591 |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | 4637/9413 | 9982/89078 | 21589/79157 | 61045/151689 |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | 27586/60194 | 12495/123266 | 172/1193 | 209652/377656 |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | 30933/105473 | 12568/83922 | 122564/185184 | 420276/613546 |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | 36250/104178 | 14234/109786 | 184225/303012 | 409483/625745 |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | 1260/6469 | 6374/32955 | 112/491 | 10424/38779 |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | 1776/7693 | 8461/36891 | 235/994 | 12559/44036 |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | 1813/8395 | 11690/98628 | 290/1841 | 17210/103474 |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | 5558/13073 | 16184/47889 | 226/731 | 23698/59539 |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | 5593/12550 | 6118/40737 | 125/383 | 13197/53876 |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | 1716/9638 | 5062/25680 | 276/1291 | 11423/36503 |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | 2368/11472 | 2875/16982 | 176/617 | 7251/29414 |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | 1684/8886 | 14716/118320 | 1104/4360 | 19315/125684 |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | 1935/10490 | 4778/138531 | 305/725 | 9563/141802 |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | 20491/56368 | 8904/90689 | 142/609 | 32920/125451 |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | 33186/77834 | 7600/53991 | 7744/40411 | 50378/136118 |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | 26886/56769 | 14981/43351 | 22077/84783 | 79278/158111 |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | 305425/346981 | 23990/206859 | 275/1139 | 330087/508863 |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | 317870/516323 | 9565/55179 | 122199/177966 | 459333/690578 |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | 172468/306486 | 8678/48158 | 336206/389423 | 526592/672161 |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | 15286/71478 | 8638/96247 | 11/72 | 25875/146255 |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | 25767/56940 | 23274/90944 | 11/57 | 41755/116966 |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | 17531/77653 | 8596/103101 | 37/257 | 30834/132157 |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | 16131/71973 | 8826/59179 | 11/46 | 26942/107942 |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | 18044/74466 | 9158/65251 | 11/60 | 29560/115914 |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | 27409/76456 | 20091/52643 | 11/79 | 45085/101899 |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | 15797/65709 | 9065/69173 | 11/67 | 27628/106507 |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | 15792/75361 | 8741/47072 | 285/751 | 29304/109871 |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | 16656/67390 | 9992/37232 | 285/865 | 30466/84252 |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | 88802/159430 | 20867/102831 | 13/61 | 119660/224378 |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | 62297/135754 | 12355/132267 | 12/55 | 95711/210915 |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | 106991/177002 | 16752/86176 | 19059/20280 | 144774/229720 |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | 1001420/2013690 | 101995/530188 | 13/92 | 1145273/2382768 |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | 1000739/1795416 | 101711/483387 | 15/80 | 1148169/2130074 |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | 1024801/1300072 | 94665/361647 | 285091/317953 | 1478127/1994217 |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | 4547/24680 | 2743/20971 | 11/87 | 10835/41146 |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | 5150/28020 | 6368/83676 | 11/100 | 13707/93512 |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | 3267/21143 | 3284/24356 | 35/113 | 8806/36882 |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | 6415/33609 | 7537/98621 | 11/76 | 15328/119619 |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | 5371/27328 | 7284/103364 | 11/66 | 14177/108991 |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | 5795/27510 | 7093/78251 | 11/132 | 13780/82817 |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | 3527/25568 | 4151/119852 | 11/74 | 10964/132247 |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | 5612/31387 | 5823/66602 | 287/455 | 13674/73045 |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | 5523/28731 | 6935/76035 | 286/563 | 13707/88138 |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | 31655/83802 | 8290/62617 | 11/53 | 39431/110212 |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | 26553/105466 | 10575/41560 | 14/89 | 41600/120010 |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | 20402/90887 | 9900/117734 | 17570/19719 | 54795/167242 |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | 282687/1457558 | 56363/169078 | 12/132 | 342652/644963 |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | 281311/528009 | 62401/151366 | 30/101 | 346618/628868 |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | 256482/467824 | 60379/127517 | 286773/315402 | 609758/840821 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

（无）

## 5. 跨进程身份与正常退出

- `r0_tlv_timestamp_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=49 sub_pid=48 | pub_ready_ns=150464307478271 in [150464303919802,150464324546439] | sub_ready_ns=150464307477239 in [150464303836369,150464324546402] | pub_starttime_ticks=15046430 sub_starttime_ticks=15046430 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r1_tlv_crc_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=86 sub_pid=85 | pub_ready_ns=150466103217677 in [150466098314077,150466114797929] | sub_ready_ns=150466102180484 in [150466098223895,150466114797910] | pub_starttime_ticks=15046609 sub_starttime_ticks=15046609 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r2_tlv_full_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=123 sub_pid=122 | pub_ready_ns=150467893931334 in [150467889168727,150467905656073] | sub_ready_ns=150467892991445 in [150467889081018,150467905656037] | pub_starttime_ticks=15046788 sub_starttime_ticks=15046788 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r3_tlv_timestamp_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=160 sub_pid=159 | pub_ready_ns=150469685979460 in [150469682378944,150469698908838] | sub_ready_ns=150469685469453 in [150469682250057,150469698908818] | pub_starttime_ticks=15046968 sub_starttime_ticks=15046968 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r4_tlv_timestamp_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=197 sub_pid=196 | pub_ready_ns=150471476396155 in [150471472604198,150471493178163] | sub_ready_ns=150471476374740 in [150471472511474,150471493178127] | pub_starttime_ticks=15047147 sub_starttime_ticks=15047147 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r5_tlv_crc_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=234 sub_pid=233 | pub_ready_ns=150473271763014 in [150473267314072,150473283825816] | sub_ready_ns=150473270791740 in [150473267192422,150473283825794] | pub_starttime_ticks=15047326 sub_starttime_ticks=15047326 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r6_tlv_crc_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=271 sub_pid=270 | pub_ready_ns=150475061624545 in [150475056538390,150475073020404] | sub_ready_ns=150475060538531 in [150475056445609,150475073020190] | pub_starttime_ticks=15047505 sub_starttime_ticks=15047505 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r7_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=308 sub_pid=307 | pub_ready_ns=150476851620559 in [150476848012923,150476862437638] | sub_ready_ns=150476850455070 in [150476847920440,150476862437497] | pub_starttime_ticks=15047684 sub_starttime_ticks=15047684 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r8_tlv_full_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=345 sub_pid=344 | pub_ready_ns=150478640411906 in [150478637144888,150478651570517] | sub_ready_ns=150478639545043 in [150478637052870,150478651570474] | pub_starttime_ticks=15047863 sub_starttime_ticks=15047863 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r9_tlv_timestamp_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=382 sub_pid=381 | pub_ready_ns=150480430274295 in [150480426760444,150480443224111] | sub_ready_ns=150480429993591 in [150480426669992,150480443224082] | pub_starttime_ticks=15048042 sub_starttime_ticks=15048042 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r10_tlv_crc_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=419 sub_pid=418 | pub_ready_ns=150482221216543 in [150482217325978,150482233876793] | sub_ready_ns=150482220024641 in [150482217169293,150482233876768] | pub_starttime_ticks=15048221 sub_starttime_ticks=15048221 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r11_tlv_full_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=456 sub_pid=455 | pub_ready_ns=150484014243746 in [150484009242010,150484025760782] | sub_ready_ns=150484013225404 in [150484009121210,150484025760727] | pub_starttime_ticks=15048400 sub_starttime_ticks=15048400 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r12_tlv_timestamp_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=493 sub_pid=492 | pub_ready_ns=150485804550832 in [150485798861834,150485817382850] | sub_ready_ns=150485802762763 in [150485798757231,150485817382810] | pub_starttime_ticks=15048579 sub_starttime_ticks=15048579 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r13_tlv_crc_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=530 sub_pid=529 | pub_ready_ns=150487594179729 in [150487589604870,150487608120371] | sub_ready_ns=150487593476500 in [150487589512580,150487608120312] | pub_starttime_ticks=15048758 sub_starttime_ticks=15048758 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r14_tlv_full_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=567 sub_pid=566 | pub_ready_ns=150489383650605 in [150489379613317,150489396118242] | sub_ready_ns=150489382994346 in [150489379514214,150489396118201] | pub_starttime_ticks=15048937 sub_starttime_ticks=15048937 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=604 sub_pid=603 | pub_ready_ns=150491172073622 in [150491168227091,150491188808584] | sub_ready_ns=150491172051026 in [150491168125971,150491188808544] | pub_starttime_ticks=15049116 sub_starttime_ticks=15049116 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r16_dzflat-a_crc_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=641 sub_pid=640 | pub_ready_ns=150492986246578 in [150492982443537,150493003018839] | sub_ready_ns=150492986269096 in [150492982347787,150493003018693] | pub_starttime_ticks=15049298 sub_starttime_ticks=15049298 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r17_dzflat-a_full_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=678 sub_pid=677 | pub_ready_ns=150494801007457 in [150494796125335,150494812608464] | sub_ready_ns=150494800045623 in [150494796026264,150494812608445] | pub_starttime_ticks=15049479 sub_starttime_ticks=15049479 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=715 sub_pid=714 | pub_ready_ns=150496612590279 in [150496607651661,150496624195058] | sub_ready_ns=150496611630148 in [150496607527856,150496624194946] | pub_starttime_ticks=15049660 sub_starttime_ticks=15049660 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=752 sub_pid=751 | pub_ready_ns=150498425794321 in [150498420834574,150498437316538] | sub_ready_ns=150498424798351 in [150498420705730,150498437316481] | pub_starttime_ticks=15049842 sub_starttime_ticks=15049842 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r20_dzflat-a_crc_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=789 sub_pid=788 | pub_ready_ns=150500215849673 in [150500211911282,150500232561764] | sub_ready_ns=150500215877623 in [150500211779035,150500232561624] | pub_starttime_ticks=15050021 sub_starttime_ticks=15050021 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r21_dzflat-a_crc_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=826 sub_pid=825 | pub_ready_ns=150502033800222 in [150502028987572,150502045506540] | sub_ready_ns=150502032833008 in [150502028858067,150502045506488] | pub_starttime_ticks=15050202 sub_starttime_ticks=15050202 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r22_dzflat-a_full_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=863 sub_pid=862 | pub_ready_ns=150503823741585 in [150503818838470,150503835335913] | sub_ready_ns=150503822699379 in [150503818744730,150503835335869] | pub_starttime_ticks=15050381 sub_starttime_ticks=15050381 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r23_dzflat-a_full_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=900 sub_pid=899 | pub_ready_ns=150505634301121 in [150505630406034,150505651117983] | sub_ready_ns=150505634243910 in [150505630271706,150505651117934] | pub_starttime_ticks=15050563 sub_starttime_ticks=15050563 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=937 sub_pid=936 | pub_ready_ns=150507433128222 in [150507428063556,150507444586782] | sub_ready_ns=150507432033572 in [150507427918529,150507444586762] | pub_starttime_ticks=15050742 sub_starttime_ticks=15050742 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r25_dzflat-a_crc_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=974 sub_pid=973 | pub_ready_ns=150509244715469 in [150509239699630,150509256157641] | sub_ready_ns=150509243669953 in [150509239596001,150509256157598] | pub_starttime_ticks=15050923 sub_starttime_ticks=15050923 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r26_dzflat-a_full_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1011 sub_pid=1010 | pub_ready_ns=150511055233217 in [150511051593382,150511066004596] | sub_ready_ns=150511054327999 in [150511051501737,150511066004543] | pub_starttime_ticks=15051105 sub_starttime_ticks=15051105 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1048 sub_pid=1047 | pub_ready_ns=150512866998929 in [150512861260954,150512879800377] | sub_ready_ns=150512865194824 in [150512861175480,150512879800347] | pub_starttime_ticks=15051286 sub_starttime_ticks=15051286 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r28_dzflat-a_crc_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1085 sub_pid=1084 | pub_ready_ns=150514677739569 in [150514671977447,150514690522405] | sub_ready_ns=150514675977274 in [150514671881031,150514690522369] | pub_starttime_ticks=15051467 sub_starttime_ticks=15051467 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r29_dzflat-a_full_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1122 sub_pid=1121 | pub_ready_ns=150516489852637 in [150516484180996,150516502754569] | sub_ready_ns=150516488113314 in [150516484046942,150516502754522] | pub_starttime_ticks=15051648 sub_starttime_ticks=15051648 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1159 sub_pid=1158 | pub_ready_ns=150518298870722 in [150518293950330,150518310427772] | sub_ready_ns=150518297890659 in [150518293850363,150518310427752] | pub_starttime_ticks=15051829 sub_starttime_ticks=15051829 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r31_dzflat-b_crc_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1196 sub_pid=1195 | pub_ready_ns=150520107079251 in [150520103620149,150520118002026] | sub_ready_ns=150520106080212 in [150520103524914,150520118001997] | pub_starttime_ticks=15052010 sub_starttime_ticks=15052010 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r32_dzflat-b_full_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1233 sub_pid=1232 | pub_ready_ns=150521919287430 in [150521914315165,150521930832725] | sub_ready_ns=150521918329592 in [150521914178546,150521930832672] | pub_starttime_ticks=15052191 sub_starttime_ticks=15052191 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1270 sub_pid=1269 | pub_ready_ns=150523728672398 in [150523724982133,150523741460658] | sub_ready_ns=150523728187468 in [150523724875098,150523741460607] | pub_starttime_ticks=15052372 sub_starttime_ticks=15052372 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1307 sub_pid=1306 | pub_ready_ns=150525539710245 in [150525534739917,150525551226913] | sub_ready_ns=150525538669451 in [150525534635641,150525551226861] | pub_starttime_ticks=15052553 sub_starttime_ticks=15052553 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r35_dzflat-b_crc_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1344 sub_pid=1343 | pub_ready_ns=150527329165974 in [150527325378264,150527341809876] | sub_ready_ns=150527328800614 in [150527325277099,150527341809798] | pub_starttime_ticks=15052732 sub_starttime_ticks=15052732 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r36_dzflat-b_crc_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1381 sub_pid=1380 | pub_ready_ns=150529140547435 in [150529136914184,150529153449875] | sub_ready_ns=150529139748338 in [150529136774564,150529153449821] | pub_starttime_ticks=15052913 sub_starttime_ticks=15052913 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r37_dzflat-b_full_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1418 sub_pid=1417 | pub_ready_ns=150530932224788 in [150530927300237,150530943743730] | sub_ready_ns=150530931243727 in [150530927195976,150530943743687] | pub_starttime_ticks=15053092 sub_starttime_ticks=15053092 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r38_dzflat-b_full_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1455 sub_pid=1454 | pub_ready_ns=150532743079049 in [150532738139804,150532754618899] | sub_ready_ns=150532742070754 in [150532738038725,150532754618856] | pub_starttime_ticks=15053273 sub_starttime_ticks=15053273 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1492 sub_pid=1491 | pub_ready_ns=150534536803315 in [150534531760595,150534548296914] | sub_ready_ns=150534535740347 in [150534531621052,150534548296858] | pub_starttime_ticks=15053453 sub_starttime_ticks=15053453 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r40_dzflat-b_crc_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1529 sub_pid=1528 | pub_ready_ns=150536348748392 in [150536345619761,150536360066289] | sub_ready_ns=150536348080441 in [150536345480314,150536360066268] | pub_starttime_ticks=15053634 sub_starttime_ticks=15053634 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r41_dzflat-b_full_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1566 sub_pid=1565 | pub_ready_ns=150538159337483 in [150538154397616,150538170879775] | sub_ready_ns=150538158301242 in [150538154294798,150538170879733] | pub_starttime_ticks=15053815 sub_starttime_ticks=15053815 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1603 sub_pid=1602 | pub_ready_ns=150539970540475 in [150539964970938,150539983534464] | sub_ready_ns=150539968793455 in [150539964864909,150539983534245] | pub_starttime_ticks=15053996 sub_starttime_ticks=15053996 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r43_dzflat-b_crc_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1640 sub_pid=1639 | pub_ready_ns=150541782643538 in [150541776975325,150541795585107] | sub_ready_ns=150541780925545 in [150541776820549,150541795585075] | pub_starttime_ticks=15054177 sub_starttime_ticks=15054177 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r44_dzflat-b_full_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1677 sub_pid=1676 | pub_ready_ns=150543592294972 in [150543587594125,150543606170707] | sub_ready_ns=150543591405466 in [150543587447983,150543606170684] | pub_starttime_ticks=15054358 sub_starttime_ticks=15054358 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1717 sub_pid=1716 | pub_ready_ns=150545408614138 in [150545400741599,150545419335674] | sub_ready_ns=150545408408338 in [150545400607555,150545419335642] | pub_starttime_ticks=15054540 sub_starttime_ticks=15054540 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1736 sub_pid=1735 | pub_ready_ns=150547610662109 in [150547599662776,150547622457495] | sub_ready_ns=150547605195381 in [150547599514760,150547622457357] | pub_starttime_ticks=15054759 sub_starttime_ticks=15054759 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=1755 sub_pid=1754 | pub_ready_ns=150549811164087 in [150549804159475,150549822736947] | sub_ready_ns=150549811043561 in [150549804010284,150549822736892] | pub_starttime_ticks=15054980 sub_starttime_ticks=15054980 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1774 sub_pid=1773 | pub_ready_ns=150552019238172 in [150552002070464,150552030937823] | sub_ready_ns=150552012773752 in [150552001920066,150552030937796] | pub_starttime_ticks=15055200 sub_starttime_ticks=15055200 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1793 sub_pid=1792 | pub_ready_ns=150554220943028 in [150554211860923,150554232508057] | sub_ready_ns=150554220941411 in [150554211709308,150554232507935] | pub_starttime_ticks=15055421 sub_starttime_ticks=15055421 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1812 sub_pid=1811 | pub_ready_ns=150556441668288 in [150556410805252,150556452082786] | sub_ready_ns=150556440578982 in [150556410661619,150556452082599] | pub_starttime_ticks=15055641 sub_starttime_ticks=15055641 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1831 sub_pid=1830 | pub_ready_ns=150558640032255 in [150558632316436,150558650891590] | sub_ready_ns=150558640040914 in [150558632163033,150558650891525] | pub_starttime_ticks=15055863 sub_starttime_ticks=15055863 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=1850 sub_pid=1849 | pub_ready_ns=150560837653392 in [150560831040498,150560849621232] | sub_ready_ns=150560838443651 in [150560830874580,150560849621210] | pub_starttime_ticks=15056083 sub_starttime_ticks=15056083 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=1869 sub_pid=1868 | pub_ready_ns=150563037204364 in [150563029814396,150563048513294] | sub_ready_ns=150563037202798 in [150563029660931,150563048513273] | pub_starttime_ticks=15056302 sub_starttime_ticks=15056302 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1888 sub_pid=1887 | pub_ready_ns=150565235409967 in [150565227728619,150565246423560] | sub_ready_ns=150565234832228 in [150565227585209,150565246423523] | pub_starttime_ticks=15056522 sub_starttime_ticks=15056522 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1907 sub_pid=1906 | pub_ready_ns=150567433242590 in [150567426698398,150567445278284] | sub_ready_ns=150567433240553 in [150567426532050,150567445278233] | pub_starttime_ticks=15056742 sub_starttime_ticks=15056742 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=1926 sub_pid=1925 | pub_ready_ns=150569632908609 in [150569622804259,150569643434197] | sub_ready_ns=150569632237701 in [150569622652168,150569643434133] | pub_starttime_ticks=15056962 sub_starttime_ticks=15056962 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1945 sub_pid=1944 | pub_ready_ns=150571833760782 in [150571824917983,150571847640029] | sub_ready_ns=150571833681184 in [150571824771187,150571847639792] | pub_starttime_ticks=15057182 sub_starttime_ticks=15057182 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1964 sub_pid=1963 | pub_ready_ns=150574037093167 in [150574029491404,150574050118882] | sub_ready_ns=150574036924317 in [150574029333183,150574050118848] | pub_starttime_ticks=15057402 sub_starttime_ticks=15057402 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=1983 sub_pid=1982 | pub_ready_ns=150576238273105 in [150576232895992,150576251543118] | sub_ready_ns=150576238665262 in [150576232745671,150576251543060] | pub_starttime_ticks=15057623 sub_starttime_ticks=15057623 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=2002 sub_pid=2001 | pub_ready_ns=150578473032365 in [150578446373763,150578486600701] | sub_ready_ns=150578471434957 in [150578446247997,150578486600664] | pub_starttime_ticks=15057844 sub_starttime_ticks=15057844 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=2025 sub_pid=2024 | pub_ready_ns=150580681527912 in [150580663966050,150580692829104] | sub_ready_ns=150580680169436 in [150580663802689,150580692828903] | pub_starttime_ticks=15058066 sub_starttime_ticks=15058066 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz` identity: parent_pid=12 pub_pid=2048 sub_pid=2047 | pub_ready_ns=150582881849956 in [150582873539850,150582892120612] | sub_ready_ns=150582888536552 in [150582873400952,150582892120581] | pub_starttime_ticks=15058287 sub_starttime_ticks=15058287 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=2071 sub_pid=2070 | pub_ready_ns=150585096411081 in [150585074179280,150585107076592] | sub_ready_ns=150585081665219 in [150585074078017,150585107076383] | pub_starttime_ticks=15058507 sub_starttime_ticks=15058507 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=2094 sub_pid=2093 | pub_ready_ns=150587299288651 in [150587287586756,150587300005267] | sub_ready_ns=150587298271578 in [150587287428434,150587300005208] | pub_starttime_ticks=15058728 sub_starttime_ticks=15058728 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=2117 sub_pid=2116 | pub_ready_ns=150589527202325 in [150589480233929,150589537760398] | sub_ready_ns=150589526411406 in [150589480073267,150589537760352] | pub_starttime_ticks=15058948 sub_starttime_ticks=15058948 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=2140 sub_pid=2139 | pub_ready_ns=150591730139170 in [150591719647486,150591732085864] | sub_ready_ns=150591729337757 in [150591719508980,150591732085839] | pub_starttime_ticks=15059171 sub_starttime_ticks=15059171 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` identity: parent_pid=12 pub_pid=2163 sub_pid=2162 | pub_ready_ns=150593923057373 in [150593910985029,150593933712211] | sub_ready_ns=150593923722200 in [150593910826729,150593933712188] | pub_starttime_ticks=15059391 sub_starttime_ticks=15059391 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz` identity: parent_pid=12 pub_pid=2186 sub_pid=2185 | pub_ready_ns=150596123922637 in [150596113079446,150596135765432] | sub_ready_ns=150596123245158 in [150596112901081,150596135765399] | pub_starttime_ticks=15059611 sub_starttime_ticks=15059611 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=2209 sub_pid=2208 | pub_ready_ns=150598329059111 in [150598315321761,150598340002357] | sub_ready_ns=150598328520414 in [150598315224886,150598340002316] | pub_starttime_ticks=15059831 sub_starttime_ticks=15059831 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=2232 sub_pid=2231 | pub_ready_ns=150600530913746 in [150600521143541,150600541789987] | sub_ready_ns=150600530318383 in [150600520975054,150600541789954] | pub_starttime_ticks=15060052 sub_starttime_ticks=15060052 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` identity: parent_pid=12 pub_pid=2255 sub_pid=2254 | pub_ready_ns=150602733896732 in [150602721666117,150602744515270] | sub_ready_ns=150602734549591 in [150602721495983,150602744515248] | pub_starttime_ticks=15060272 sub_starttime_ticks=15060272 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=2278 sub_pid=2277 | pub_ready_ns=150604934398073 in [150604926080363,150604936451217] | sub_ready_ns=150604933083195 in [150604925917634,150604936451158] | pub_starttime_ticks=15060492 sub_starttime_ticks=15060492 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=2301 sub_pid=2300 | pub_ready_ns=150607153692258 in [150607114957922,150607156139445] | sub_ready_ns=150607153684871 in [150607114783223,150607156139292] | pub_starttime_ticks=15060711 sub_starttime_ticks=15060711 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` identity: parent_pid=12 pub_pid=2324 sub_pid=2323 | pub_ready_ns=150609343841750 in [150609336302057,150609356942809] | sub_ready_ns=150609344203082 in [150609336137891,150609356942632] | pub_starttime_ticks=15060933 sub_starttime_ticks=15060933 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- **A/B 可分性只在发布侧成立**：实测 `dzflat-a` 与 `dzflat-b` 两档的消费者侧`via_view`/`via_object` **完全相同**（各 13500 / 0，10 轮 full 档合计）⇒ `via_view` **不是** B 档的区分判据（早前文档把它写成 B 档专属判据，已更正）。可分性证据在**发布侧**：`wire_bytes_source`（A=`对象 dzflat_size()` / B=`B 借样 chunk 容量`）与实际路径条数计数。
- **失败量阈值是本基准自定的预算**（`manifest.failure_thresholds`）：迟发默认「次数>5 **且** 比率>0.5%」才判失败。它是**可判**而非**无条件失败**；引用时必须连阈值一起引。
- `r0_tlv_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 123523 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r1_tlv_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 27741 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r2_tlv_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 54424 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r3_tlv_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 33447 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r4_tlv_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 153880 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r5_tlv_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 44339 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r6_tlv_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 90517 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r7_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 56296 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r8_tlv_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 18310 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r9_tlv_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 34329 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r10_tlv_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5081 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r11_tlv_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 7629 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r12_tlv_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5281 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r13_tlv_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 13743 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r14_tlv_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 6563 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 37956 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r16_dzflat-a_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 22809 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r17_dzflat-a_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 7089 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 129421 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 4150 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r20_dzflat-a_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 23225 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r21_dzflat-a_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 113546 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r22_dzflat-a_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 52640 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r23_dzflat-a_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 86165 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 63705 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r25_dzflat-a_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 15357 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r26_dzflat-a_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5818 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4529 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r28_dzflat-a_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5333 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r29_dzflat-a_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4595 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 53448 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r31_dzflat-b_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 11622 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r32_dzflat-b_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 45406 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 30355 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 69678 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r35_dzflat-b_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 18184 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r36_dzflat-b_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 54253 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r37_dzflat-b_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 23474 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r38_dzflat-b_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 69239 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 35199 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r40_dzflat-b_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4600 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r41_dzflat-b_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5814 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 65925 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r43_dzflat-b_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5675 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r44_dzflat-b_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 58022 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 84334 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 49876 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 162277 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 108001 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 73949 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 43273 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 58790 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 26184 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 18269 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 163731 ns（迟发 11 次）—— 该轮延迟含排队成分
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 31869 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 91415 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 407649 ns（迟发 8 次）—— 该轮延迟含排队成分
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 55796 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 7998685 ns（迟发 9 次）—— 该轮延迟含排队成分
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 116485 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 79148 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 15891 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 105963 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 61306 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 148871 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 7368 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 55336 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 141605 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 72224 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 49975 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 68538 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4071584 ns（迟发 13 次）—— 该轮延迟含排队成分
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 143133 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 40753 ns（迟发 0 次）—— 该轮延迟含排队成分

