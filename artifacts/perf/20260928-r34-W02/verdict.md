# W02 统一跨进程基准 · 判定（verdict）

> run_id: `20260928-r34-W02`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `3e1b409413ee5b3c4e618d31ec8d6adc70cb021950f4bfe411eb210a46af5e8f`；libipc sha256: `cf209393773d51ee4762dd95bfd1b04f873ec54aa8f61cf5a7ed92be2990fa46`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=9.97384 syscall_ns_per_call=81.4984 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

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
| `r0_tlv_timestamp_blocking_64B_1000Hz` | tlv | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.153 | 0 | 0 | 0/0/0/0/0 | 0/2204 | `samples/r0_tlv_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r1_tlv_crc_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.007 | 0 | 0 | 0/0/0/0/0 | 0/2206 | `samples/r1_tlv_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r2_tlv_full_blocking_64B_1000Hz` | tlv | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.017 | 0 | 0 | 0/0/0/0/0 | 0/2206 | `samples/r2_tlv_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | tlv | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.036 | 0 | 0 | 0/0/0/0/0 | 0/2203 | `samples/r3_tlv_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | tlv | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.053 | 0 | 0 | 0/0/0/0/0 | 0/2202 | `samples/r4_tlv_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r5_tlv_crc_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0.217 | 0 | 0 | 0/0/0/0/0 | 0/2202 | `samples/r5_tlv_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r6_tlv_crc_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.007 | 0 | 0 | 0/0/0/0/0 | 0/2202 | `samples/r6_tlv_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r7_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.134 | 0 | 0 | 0/0/0/0/0 | 0/2205 | `samples/r7_tlv_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r8_tlv_full_busy_1024B_1000Hz` | tlv | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.097 | 0 | 0 | 0/0/0/0/0 | 0/2203 | `samples/r8_tlv_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | tlv | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.060 | 0 | 0 | 0/0/0/0/0 | 0/2098 | `samples/r9_tlv_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r10_tlv_crc_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 0/2119 | `samples/r10_tlv_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r11_tlv_full_blocking_65536B_1000Hz` | tlv | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.015 | 0 | 0 | 0/0/0/0/0 | 0/2120 | `samples/r11_tlv_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | tlv | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.035 | 0 | 0 | 0/0/0/0/0 | 0/1116 | `samples/r12_tlv_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r13_tlv_crc_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 0/1096 | `samples/r13_tlv_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r14_tlv_full_blocking_1048576B_500Hz` | tlv | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.010 | 0 | 0 | 0/0/0/0/0 | 0/1112 | `samples/r14_tlv_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | dzflat-a | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r15_dzflat-a_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 2200/2 | `samples/r16_dzflat-a_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | dzflat-a | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.072 | 0 | 0 | 0/0/0/0/0 | 2199/2 | `samples/r17_dzflat-a_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | dzflat-a | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.022 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r18_dzflat-a_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | dzflat-a | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0.144 | 0 | 0 | 0/0/0/0/0 | 2200/3 | `samples/r19_dzflat-a_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.054 | 0 | 0 | 0/0/0/0/0 | 2200/2 | `samples/r20_dzflat-a_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.031 | 0 | 0 | 0/0/0/0/0 | 2201/2 | `samples/r21_dzflat-a_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | dzflat-a | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r22_dzflat-a_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | dzflat-a | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.156 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r23_dzflat-a_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | dzflat-a | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.043 | 0 | 0 | 0/0/0/0/0 | 2129/3 | `samples/r24_dzflat-a_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.008 | 0 | 0 | 0/0/0/0/0 | 2130/2 | `samples/r25_dzflat-a_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | dzflat-a | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2120/2 | `samples/r26_dzflat-a_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | dzflat-a | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.002 | 0 | 0 | 0/0/0/0/0 | 1192/2 | `samples/r27_dzflat-a_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.010 | 0 | 0 | 0/0/0/0/0 | 1173/2 | `samples/r28_dzflat-a_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | dzflat-a | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 1 | 0.124 | 0 | 0 | 0/0/0/0/0 | 1177/2 | `samples/r29_dzflat-a_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | dzflat-b | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 5 | 0.121 | 0 | 0 | 0/0/0/0/0 | 2204/2 | `samples/r30_dzflat-b_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.044 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r31_dzflat-b_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | dzflat-b | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2202/2 | `samples/r32_dzflat-b_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | dzflat-b | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 2205/2 | `samples/r33_dzflat-b_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | dzflat-b | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 2200/2 | `samples/r34_dzflat-b_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.103 | 0 | 0 | 0/0/0/0/0 | 2199/2 | `samples/r35_dzflat-b_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.047 | 0 | 0 | 0/0/0/0/0 | 2201/2 | `samples/r36_dzflat-b_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | dzflat-b | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.149 | 0 | 0 | 0/0/0/0/0 | 2200/2 | `samples/r37_dzflat-b_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | dzflat-b | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.004 | 0 | 0 | 0/0/0/0/0 | 2203/2 | `samples/r38_dzflat-b_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | dzflat-b | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.091 | 0 | 0 | 0/0/0/0/0 | 2102/2 | `samples/r39_dzflat-b_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 13 | 0.152 | 0 | 0 | 0/0/0/0/0 | 2092/2 | `samples/r40_dzflat-b_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | dzflat-b | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.018 | 0 | 0 | 0/0/0/0/0 | 2090/2 | `samples/r41_dzflat-b_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | dzflat-b | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.055 | 0 | 0 | 0/0/0/0/0 | 1020/2 | `samples/r42_dzflat-b_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.009 | 0 | 0 | 0/0/0/0/0 | 1021/2 | `samples/r43_dzflat-b_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | dzflat-b | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 994/2 | `samples/r44_dzflat-b_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | cyclonedds-udp | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r46_cyclonedds-udp_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | cyclonedds-udp | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.055 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r47_cyclonedds-udp_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0.116 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | cyclonedds-udp | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0.181 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.026 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r50_cyclonedds-udp_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.012 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r51_cyclonedds-udp_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.052 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r52_cyclonedds-udp_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | cyclonedds-udp | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r53_cyclonedds-udp_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | cyclonedds-udp | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.052 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.006 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r55_cyclonedds-udp_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | cyclonedds-udp | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r56_cyclonedds-udp_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | cyclonedds-udp | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 2 | 0.115 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.035 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r58_cyclonedds-udp_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | cyclonedds-udp | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 1 | 0.286 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r59_cyclonedds-udp_full_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | cyclonedds-iox | transport | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 3 | 0.120 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.087 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r61_cyclonedds-iox_crc_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | cyclonedds-iox | full-read | 63B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.005 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r62_cyclonedds-iox_full_blocking_64B_1000Hz.samples.csv` | **通过** |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.283 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | cyclonedds-iox | transport | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.043 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.093 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r65_cyclonedds-iox_crc_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.015 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r66_cyclonedds-iox_crc_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 2 | 0.170 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r67_cyclonedds-iox_full_blocking_1024B_1000Hz.samples.csv` | **通过** |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | cyclonedds-iox | full-read | 1021B | busy | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.019 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r68_cyclonedds-iox_full_busy_1024B_1000Hz.samples.csv` | **通过** |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | cyclonedds-iox | transport | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.093 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 0 | 0.076 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r70_cyclonedds-iox_crc_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | cyclonedds-iox | full-read | 65533B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 8 | 2.062 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r71_cyclonedds-iox_full_blocking_65536B_1000Hz.samples.csv` | **通过** |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | cyclonedds-iox | transport | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.057 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.003 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r73_cyclonedds-iox_crc_blocking_1048576B_500Hz.samples.csv` | **通过** |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | cyclonedds-iox | full-read | 1048573B | blocking | 500/500/500/0 | 500 | 0 | 0 | 0 | 0 | 0 | 0.054 | 0 | 0 | 0/0/0/0/0 | 0/0 | `samples/r74_cyclonedds-iox_full_blocking_1048576B_500Hz.samples.csv` | **通过** |

（gate 列 = 「该 gate 判失败的次数」，0 = 通过；阈值见 `manifest.failure_thresholds`。所列为**判定结果**，不是原始量 —— 原始量在同行的迟发/积压/结构异常/段头矛盾列。）

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_timestamp_blocking_64B_1000Hz` | 4683/19278 | 33793/139028 | 85/185 | 39545/147088 |
| `r1_tlv_crc_blocking_64B_1000Hz` | 2556/16561 | 6807/30686 | 20/144 | 13037/40842 |
| `r2_tlv_full_blocking_64B_1000Hz` | 3294/17532 | 7875/35249 | 14/151 | 13177/43822 |
| `r3_tlv_timestamp_blocking_1024B_1000Hz` | 4266/21840 | 12432/126115 | 20/237 | 20194/130113 |
| `r4_tlv_timestamp_busy_1024B_1000Hz` | 4822/19543 | 7555/54897 | 11/114 | 13881/72614 |
| `r5_tlv_crc_blocking_1024B_1000Hz` | 8727/32888 | 16979/106105 | 25/221 | 25686/116995 |
| `r6_tlv_crc_busy_1024B_1000Hz` | 3234/19291 | 5875/54802 | 11/21 | 13438/65465 |
| `r7_tlv_full_blocking_1024B_1000Hz` | 3865/17458 | 9403/54505 | 16/170 | 15377/60966 |
| `r8_tlv_full_busy_1024B_1000Hz` | 3628/18025 | 6473/58003 | 11/54 | 10914/66328 |
| `r9_tlv_timestamp_blocking_65536B_1000Hz` | 13943/22880 | 21270/126516 | 20/245 | 54725/173334 |
| `r10_tlv_crc_blocking_65536B_1000Hz` | 13253/19625 | 19689/47515 | 31/114 | 60143/102845 |
| `r11_tlv_full_blocking_65536B_1000Hz` | 11704/40653 | 66989/191274 | 25/294 | 107038/218428 |
| `r12_tlv_timestamp_blocking_1048576B_500Hz` | 115759/213992 | 175859/276176 | 26/126 | 487772/720234 |
| `r13_tlv_crc_blocking_1048576B_500Hz` | 118365/175222 | 138445/299035 | 21/169 | 523364/726516 |
| `r14_tlv_full_blocking_1048576B_500Hz` | 119042/173520 | 185458/292606 | 14/146 | 484006/603331 |
| `r15_dzflat-a_timestamp_blocking_64B_1000Hz` | 1404/8536 | 7422/51768 | 130/617 | 10561/57525 |
| `r16_dzflat-a_crc_blocking_64B_1000Hz` | 2757/14024 | 8960/58255 | 188/873 | 12692/63402 |
| `r17_dzflat-a_full_blocking_64B_1000Hz` | 3037/15322 | 8594/96153 | 184/820 | 12647/100891 |
| `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` | 2774/15844 | 8985/59856 | 125/417 | 13202/71046 |
| `r19_dzflat-a_timestamp_busy_1024B_1000Hz` | 6053/24485 | 10177/29971 | 119/354 | 14809/40169 |
| `r20_dzflat-a_crc_blocking_1024B_1000Hz` | 1716/10435 | 6427/31057 | 322/1390 | 12551/41913 |
| `r21_dzflat-a_crc_busy_1024B_1000Hz` | 3159/14166 | 5493/82295 | 176/581 | 10664/85700 |
| `r22_dzflat-a_full_blocking_1024B_1000Hz` | 2021/9113 | 7580/35549 | 408/2482 | 12841/41110 |
| `r23_dzflat-a_full_busy_1024B_1000Hz` | 1885/10163 | 3836/22059 | 363/876 | 10158/30674 |
| `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` | 2830/10670 | 8968/116952 | 121/610 | 36863/134896 |
| `r25_dzflat-a_crc_blocking_65536B_1000Hz` | 4665/9990 | 7925/32226 | 5940/42359 | 47132/125558 |
| `r26_dzflat-a_full_blocking_65536B_1000Hz` | 4484/10270 | 9811/53545 | 21823/83323 | 52730/151228 |
| `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` | 27798/41351 | 9376/56223 | 164/787 | 205086/312924 |
| `r28_dzflat-a_crc_blocking_1048576B_500Hz` | 32680/116989 | 15019/134113 | 121681/189534 | 440859/648710 |
| `r29_dzflat-a_full_blocking_1048576B_500Hz` | 30371/113207 | 9579/114115 | 176398/342002 | 408494/678252 |
| `r30_dzflat-b_timestamp_blocking_64B_1000Hz` | 4012/10184 | 16190/114593 | 215/796 | 21721/115888 |
| `r31_dzflat-b_crc_blocking_64B_1000Hz` | 2016/9749 | 9816/109077 | 205/1544 | 15338/118933 |
| `r32_dzflat-b_full_blocking_64B_1000Hz` | 1401/6575 | 9614/59978 | 226/1296 | 13565/61998 |
| `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` | 1541/8369 | 13011/117660 | 188/777 | 16587/121122 |
| `r34_dzflat-b_timestamp_busy_1024B_1000Hz` | 1964/11110 | 5651/57444 | 101/251 | 8874/69456 |
| `r35_dzflat-b_crc_blocking_1024B_1000Hz` | 3035/14255 | 10822/36841 | 181/1118 | 15977/46611 |
| `r36_dzflat-b_crc_busy_1024B_1000Hz` | 1588/10096 | 5478/28413 | 194/724 | 10811/37940 |
| `r37_dzflat-b_full_blocking_1024B_1000Hz` | 5433/12520 | 12617/129708 | 569/3372 | 19618/137445 |
| `r38_dzflat-b_full_busy_1024B_1000Hz` | 1641/10792 | 4655/28554 | 340/823 | 10309/36684 |
| `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` | 31322/79418 | 9247/117763 | 130/528 | 46170/147737 |
| `r40_dzflat-b_crc_blocking_65536B_1000Hz` | 41820/101471 | 15257/117642 | 7814/42403 | 74283/200581 |
| `r41_dzflat-b_full_blocking_65536B_1000Hz` | 27088/61967 | 13223/112034 | 11367/76819 | 57812/161720 |
| `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` | 188634/307814 | 20529/163080 | 169/1341 | 217957/368672 |
| `r43_dzflat-b_crc_blocking_1048576B_500Hz` | 308101/509890 | 13761/65026 | 114657/199882 | 439411/673235 |
| `r44_dzflat-b_full_blocking_1048576B_500Hz` | 185729/311436 | 16899/150068 | 338053/385971 | 540902/615798 |
| `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` | 7225/45232 | 4752/36075 | 11/31 | 12287/71478 |
| `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` | 15201/68288 | 8995/69284 | 11/62 | 29007/101044 |
| `r47_cyclonedds-udp_full_blocking_64B_1000Hz` | 8353/49552 | 4960/62583 | 35/137 | 23488/100128 |
| `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` | 16141/77520 | 8866/90028 | 11/63 | 26206/121376 |
| `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` | 21710/76955 | 9222/48623 | 11/90 | 37282/106802 |
| `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` | 17464/64337 | 8941/22042 | 11/84 | 28700/76331 |
| `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` | 8874/56479 | 5206/36787 | 11/70 | 17521/81960 |
| `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` | 12133/61838 | 6465/123914 | 282/444 | 29369/134664 |
| `r53_cyclonedds-udp_full_busy_1024B_1000Hz` | 16075/79635 | 9217/118026 | 285/442 | 28164/133821 |
| `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` | 83175/166692 | 16611/103087 | 14/57 | 120421/212440 |
| `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` | 58340/116561 | 10698/86888 | 12/53 | 71439/167469 |
| `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` | 61468/176209 | 10890/96643 | 19058/20353 | 108275/239488 |
| `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` | 1040870/1396709 | 117835/544581 | 22/91 | 1220354/1816931 |
| `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` | 1023542/1958019 | 145381/589166 | 13/100 | 1192342/2275613 |
| `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` | 1040520/1982365 | 132408/550476 | 281440/945464 | 1481433/2510597 |
| `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` | 9453/48196 | 8703/86082 | 11/58 | 18262/116470 |
| `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` | 9181/38311 | 6657/55517 | 11/84 | 15029/64863 |
| `r62_cyclonedds-iox_full_blocking_64B_1000Hz` | 2625/17138 | 6723/105714 | 35/180 | 12464/107517 |
| `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` | 5453/24950 | 2844/23008 | 11/185 | 9385/44076 |
| `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` | 5973/30765 | 7333/86314 | 12/76 | 14278/99560 |
| `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` | 4442/25021 | 5238/84039 | 11/86 | 11310/91155 |
| `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` | 7006/15553 | 5850/59181 | 11/86 | 13726/60948 |
| `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` | 10583/30599 | 8982/32247 | 282/441 | 17508/49468 |
| `r68_cyclonedds-iox_full_busy_1024B_1000Hz` | 5990/28231 | 2478/58550 | 282/456 | 11287/67675 |
| `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` | 30666/120177 | 9518/63624 | 12/93 | 40856/142317 |
| `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` | 33307/93902 | 9893/53238 | 14/21 | 43752/107258 |
| `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` | 36394/101995 | 13732/54323 | 19378/60852 | 72655/153233 |
| `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` | 285786/452520 | 57242/92534 | 12/90 | 345370/507064 |
| `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` | 261987/362031 | 57769/91485 | 12/96 | 321750/428032 |
| `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` | 257102/449964 | 68258/183239 | 304240/315341 | 626649/889075 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

（无）

## 5. 跨进程身份与正常退出

- `r0_tlv_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=48 sub_pid=47 | pub_ready_ns=165294382070746 in [165294377309506,165294393846101] | sub_ready_ns=165294381051564 in [165294377222842,165294393846050] | pub_starttime_ticks=16529437 sub_starttime_ticks=16529437 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r1_tlv_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=85 sub_pid=84 | pub_ready_ns=165296173198059 in [165296168289020,165296184767057] | sub_ready_ns=165296172234990 in [165296168197216,165296184766911] | pub_starttime_ticks=16529616 sub_starttime_ticks=16529616 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r2_tlv_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=122 sub_pid=121 | pub_ready_ns=165297962311588 in [165297957338960,165297973813681] | sub_ready_ns=165297961336950 in [165297957249148,165297973813643] | pub_starttime_ticks=16529795 sub_starttime_ticks=16529795 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r3_tlv_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=159 sub_pid=158 | pub_ready_ns=165299754555829 in [165299749552756,165299766013822] | sub_ready_ns=165299753621836 in [165299749463329,165299766013606] | pub_starttime_ticks=16529974 sub_starttime_ticks=16529974 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r4_tlv_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=196 sub_pid=195 | pub_ready_ns=165301544687513 in [165301541583583,165301562169574] | sub_ready_ns=165301544804729 in [165301541449145,165301562169514] | pub_starttime_ticks=16530154 sub_starttime_ticks=16530154 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r5_tlv_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=233 sub_pid=232 | pub_ready_ns=165303342698272 in [165303337880662,165303354397800] | sub_ready_ns=165303341912156 in [165303337753922,165303354397731] | pub_starttime_ticks=16530333 sub_starttime_ticks=16530333 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r6_tlv_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=270 sub_pid=269 | pub_ready_ns=165305133836649 in [165305129778902,165305150382771] | sub_ready_ns=165305133839819 in [165305129690718,165305150382725] | pub_starttime_ticks=16530512 sub_starttime_ticks=16530512 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r7_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=307 sub_pid=306 | pub_ready_ns=165306930313059 in [165306926423113,165306947117788] | sub_ready_ns=165306930290474 in [165306926287885,165306947117731] | pub_starttime_ticks=16530692 sub_starttime_ticks=16530692 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r8_tlv_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=344 sub_pid=343 | pub_ready_ns=165308725213364 in [165308720275154,165308736766009] | sub_ready_ns=165308724157214 in [165308720182312,165308736765969] | pub_starttime_ticks=16530872 sub_starttime_ticks=16530872 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r9_tlv_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=381 sub_pid=380 | pub_ready_ns=165310515187418 in [165310512319921,165310530898191] | sub_ready_ns=165310515638918 in [165310512190572,165310530898150] | pub_starttime_ticks=16531051 sub_starttime_ticks=16531051 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r10_tlv_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=418 sub_pid=417 | pub_ready_ns=165312310760177 in [165312307067187,165312323564056] | sub_ready_ns=165312309964577 in [165312306974419,165312323564033] | pub_starttime_ticks=16531230 sub_starttime_ticks=16531230 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r11_tlv_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=455 sub_pid=454 | pub_ready_ns=165314103467774 in [165314098444543,165314115391407] | sub_ready_ns=165314102653888 in [165314098355964,165314115391264] | pub_starttime_ticks=16531409 sub_starttime_ticks=16531409 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r12_tlv_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=492 sub_pid=491 | pub_ready_ns=165315895215747 in [165315889587756,165315908139923] | sub_ready_ns=165315893535449 in [165315889498991,165315908139867] | pub_starttime_ticks=16531588 sub_starttime_ticks=16531588 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r13_tlv_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=529 sub_pid=528 | pub_ready_ns=165317687422927 in [165317681593510,165317700254328] | sub_ready_ns=165317685695053 in [165317681454459,165317700254263] | pub_starttime_ticks=16531768 sub_starttime_ticks=16531768 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r14_tlv_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=566 sub_pid=565 | pub_ready_ns=165319478604045 in [165319472867168,165319493465072] | sub_ready_ns=165319476911217 in [165319472725577,165319493465041] | pub_starttime_ticks=16531947 sub_starttime_ticks=16531947 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=603 sub_pid=602 | pub_ready_ns=165321270389859 in [165321265313426,165321281836971] | sub_ready_ns=165321269359482 in [165321265174973,165321281836920] | pub_starttime_ticks=16532126 sub_starttime_ticks=16532126 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r16_dzflat-a_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=640 sub_pid=639 | pub_ready_ns=165323082011240 in [165323077027665,165323093551873] | sub_ready_ns=165323081027544 in [165323076890937,165323093551852] | pub_starttime_ticks=16532307 sub_starttime_ticks=16532307 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r17_dzflat-a_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=677 sub_pid=676 | pub_ready_ns=165324893478769 in [165324888540827,165324905099819] | sub_ready_ns=165324892469420 in [165324888407974,165324905099779] | pub_starttime_ticks=16532488 sub_starttime_ticks=16532488 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=714 sub_pid=713 | pub_ready_ns=165326704931446 in [165326699886324,165326716369601] | sub_ready_ns=165326703957365 in [165326699786036,165326716369572] | pub_starttime_ticks=16532669 sub_starttime_ticks=16532669 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=751 sub_pid=750 | pub_ready_ns=165328516248033 in [165328511784893,165328532383775] | sub_ready_ns=165328516865476 in [165328511690239,165328532383556] | pub_starttime_ticks=16532851 sub_starttime_ticks=16532851 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r20_dzflat-a_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=788 sub_pid=787 | pub_ready_ns=165330313791353 in [165330308760652,165330325270452] | sub_ready_ns=165330312796437 in [165330308621527,165330325270432] | pub_starttime_ticks=16533030 sub_starttime_ticks=16533030 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r21_dzflat-a_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=825 sub_pid=824 | pub_ready_ns=165332124428897 in [165332120544122,165332137058630] | sub_ready_ns=165332124065111 in [165332120402799,165332137058573] | pub_starttime_ticks=16533212 sub_starttime_ticks=16533212 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r22_dzflat-a_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=862 sub_pid=861 | pub_ready_ns=165333916252824 in [165333912406372,165333928898393] | sub_ready_ns=165333915939617 in [165333912313654,165333928898355] | pub_starttime_ticks=16533391 sub_starttime_ticks=16533391 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r23_dzflat-a_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=899 sub_pid=898 | pub_ready_ns=165335727355469 in [165335722469341,165335738963368] | sub_ready_ns=165335726431630 in [165335722363898,165335738963204] | pub_starttime_ticks=16533572 sub_starttime_ticks=16533572 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=936 sub_pid=935 | pub_ready_ns=165337516509117 in [165337513549002,165337534128900] | sub_ready_ns=165337516818441 in [165337513444696,165337534128697] | pub_starttime_ticks=16533751 sub_starttime_ticks=16533751 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r25_dzflat-a_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=973 sub_pid=972 | pub_ready_ns=165339333117619 in [165339328174694,165339344657163] | sub_ready_ns=165339332082717 in [165339328078848,165339344657138] | pub_starttime_ticks=16533932 sub_starttime_ticks=16533932 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r26_dzflat-a_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1010 sub_pid=1009 | pub_ready_ns=165341145115508 in [165341139958614,165341156489462] | sub_ready_ns=165341144108704 in [165341139805490,165341156489431] | pub_starttime_ticks=16534114 sub_starttime_ticks=16534113 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1047 sub_pid=1046 | pub_ready_ns=165342954959266 in [165342950335030,165342966818682] | sub_ready_ns=165342954237684 in [165342950233715,165342966818644] | pub_starttime_ticks=16534295 sub_starttime_ticks=16534295 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r28_dzflat-a_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1084 sub_pid=1083 | pub_ready_ns=165344763778098 in [165344759387886,165344775868714] | sub_ready_ns=165344762023984 in [165344759293940,165344775868508] | pub_starttime_ticks=16534475 sub_starttime_ticks=16534475 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r29_dzflat-a_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1121 sub_pid=1120 | pub_ready_ns=165346574073794 in [165346569557697,165346586058914] | sub_ready_ns=165346572166444 in [165346569413390,165346586058873] | pub_starttime_ticks=16534656 sub_starttime_ticks=16534656 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1158 sub_pid=1157 | pub_ready_ns=165348383431067 in [165348379737253,165348396222065] | sub_ready_ns=165348382669639 in [165348379621821,165348396222043] | pub_starttime_ticks=16534837 sub_starttime_ticks=16534837 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r31_dzflat-b_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1195 sub_pid=1194 | pub_ready_ns=165350193840185 in [165350190157182,165350204595229] | sub_ready_ns=165350192910790 in [165350190052188,165350204595207] | pub_starttime_ticks=16535019 sub_starttime_ticks=16535019 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r32_dzflat-b_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1232 sub_pid=1231 | pub_ready_ns=165352005272581 in [165352000321218,165352016819294] | sub_ready_ns=165352004335798 in [165352000214235,165352016819073] | pub_starttime_ticks=16535200 sub_starttime_ticks=16535200 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1269 sub_pid=1268 | pub_ready_ns=165353813752857 in [165353810231182,165353824668659] | sub_ready_ns=165353812931625 in [165353810129643,165353824668616] | pub_starttime_ticks=16535381 sub_starttime_ticks=16535381 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1306 sub_pid=1305 | pub_ready_ns=165355626092910 in [165355620976186,165355637511647] | sub_ready_ns=165355625077734 in [165355620864461,165355637511509] | pub_starttime_ticks=16535562 sub_starttime_ticks=16535562 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r35_dzflat-b_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1343 sub_pid=1342 | pub_ready_ns=165357418555454 in [165357413416734,165357429921124] | sub_ready_ns=165357417528898 in [165357413317252,165357429921068] | pub_starttime_ticks=16535741 sub_starttime_ticks=16535741 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r36_dzflat-b_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1380 sub_pid=1379 | pub_ready_ns=165359233027664 in [165359227825240,165359244331032] | sub_ready_ns=165359231832711 in [165359227682441,165359244330980] | pub_starttime_ticks=16535922 sub_starttime_ticks=16535922 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r37_dzflat-b_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1417 sub_pid=1416 | pub_ready_ns=165361024655095 in [165361019750252,165361036227471] | sub_ready_ns=165361023638992 in [165361019657029,165361036227304] | pub_starttime_ticks=16536101 sub_starttime_ticks=16536101 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r38_dzflat-b_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1454 sub_pid=1453 | pub_ready_ns=165362836199836 in [165362832273084,165362848769302] | sub_ready_ns=165362835842565 in [165362832175738,165362848769268] | pub_starttime_ticks=16536283 sub_starttime_ticks=16536283 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1491 sub_pid=1490 | pub_ready_ns=165364629406143 in [165364624364926,165364640867155] | sub_ready_ns=165364628431874 in [165364624213382,165364640867123] | pub_starttime_ticks=16536462 sub_starttime_ticks=16536462 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r40_dzflat-b_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1528 sub_pid=1527 | pub_ready_ns=165366440566574 in [165366436781609,165366453271329] | sub_ready_ns=165366439737259 in [165366436628034,165366453271299] | pub_starttime_ticks=16536643 sub_starttime_ticks=16536643 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r41_dzflat-b_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1565 sub_pid=1564 | pub_ready_ns=165368252364886 in [165368247028958,165368263520216] | sub_ready_ns=165368251228524 in [165368246916771,165368263520186] | pub_starttime_ticks=16536824 sub_starttime_ticks=16536824 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1602 sub_pid=1601 | pub_ready_ns=165370064363759 in [165370058945883,165370077485368] | sub_ready_ns=165370062870507 in [165370058839051,165370077485327] | pub_starttime_ticks=16537005 sub_starttime_ticks=16537005 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r43_dzflat-b_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1639 sub_pid=1638 | pub_ready_ns=165371874132704 in [165371869454283,165371885943843] | sub_ready_ns=165371873292092 in [165371869346389,165371885943812] | pub_starttime_ticks=16537186 sub_starttime_ticks=16537186 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r44_dzflat-b_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1676 sub_pid=1675 | pub_ready_ns=165373684219501 in [165373679585136,165373698133908] | sub_ready_ns=165373683085109 in [165373679426113,165373698133889] | pub_starttime_ticks=16537367 sub_starttime_ticks=16537367 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1716 sub_pid=1715 | pub_ready_ns=165375497508566 in [165375491294909,165375507789377] | sub_ready_ns=165375497500360 in [165375491193674,165375507789337] | pub_starttime_ticks=16537549 sub_starttime_ticks=16537549 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1735 sub_pid=1734 | pub_ready_ns=165377695946864 in [165377688275932,165377706830036] | sub_ready_ns=165377695953539 in [165377688171467,165377706829880] | pub_starttime_ticks=16537768 sub_starttime_ticks=16537768 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=1754 sub_pid=1753 | pub_ready_ns=165379894270051 in [165379887769889,165379906279234] | sub_ready_ns=165379894403388 in [165379887674378,165379906279192] | pub_starttime_ticks=16537988 sub_starttime_ticks=16537988 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1773 sub_pid=1772 | pub_ready_ns=165382094242606 in [165382086446289,165382105011609] | sub_ready_ns=165382094249607 in [165382086290010,165382105011432] | pub_starttime_ticks=16538208 sub_starttime_ticks=16538208 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1792 sub_pid=1791 | pub_ready_ns=165384293783973 in [165384287693187,165384304236146] | sub_ready_ns=165384294403476 in [165384287535102,165384304236095] | pub_starttime_ticks=16538428 sub_starttime_ticks=16538428 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1811 sub_pid=1810 | pub_ready_ns=165386489948066 in [165386484005710,165386500506814] | sub_ready_ns=165386489359429 in [165386483900217,165386500506791] | pub_starttime_ticks=16538648 sub_starttime_ticks=16538648 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1830 sub_pid=1829 | pub_ready_ns=165388686789911 in [165388679969698,165388698514280] | sub_ready_ns=165388686795227 in [165388679826514,165388698514233] | pub_starttime_ticks=16538868 sub_starttime_ticks=16538867 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=1849 sub_pid=1848 | pub_ready_ns=165390883136932 in [165390877618316,165390894181070] | sub_ready_ns=165390883144258 in [165390877474806,165390894181023] | pub_starttime_ticks=16539087 sub_starttime_ticks=16539087 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=1868 sub_pid=1867 | pub_ready_ns=165393081289803 in [165393073581938,165393092187462] | sub_ready_ns=165393081291867 in [165393073434629,165393092187322] | pub_starttime_ticks=16539307 sub_starttime_ticks=16539307 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1887 sub_pid=1886 | pub_ready_ns=165395278057675 in [165395269855648,165395288442150] | sub_ready_ns=165395278050273 in [165395269688609,165395288442093] | pub_starttime_ticks=16539526 sub_starttime_ticks=16539526 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1906 sub_pid=1905 | pub_ready_ns=165397475782845 in [165397469504678,165397488003108] | sub_ready_ns=165397475775995 in [165397469388405,165397488003058] | pub_starttime_ticks=16539746 sub_starttime_ticks=16539746 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=1925 sub_pid=1924 | pub_ready_ns=165399673803123 in [165399667456771,165399685990030] | sub_ready_ns=165399673376926 in [165399667369889,165399685990000] | pub_starttime_ticks=16539966 sub_starttime_ticks=16539966 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1944 sub_pid=1943 | pub_ready_ns=165401872195042 in [165401864426313,165401887060487] | sub_ready_ns=165401872162165 in [165401864331066,165401887060466] | pub_starttime_ticks=16540186 sub_starttime_ticks=16540186 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1963 sub_pid=1962 | pub_ready_ns=165404078606384 in [165404070841564,165404091419163] | sub_ready_ns=165404078523065 in [165404070733183,165404091419124] | pub_starttime_ticks=16540407 sub_starttime_ticks=16540407 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=1982 sub_pid=1981 | pub_ready_ns=165406269122508 in [165406262631827,165406283272640] | sub_ready_ns=165406269552629 in [165406262475600,165406283272575] | pub_starttime_ticks=16540626 sub_starttime_ticks=16540626 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=2001 sub_pid=2000 | pub_ready_ns=165408479640012 in [165408465720033,165408490532536] | sub_ready_ns=165408479659947 in [165408465573164,165408490532371] | pub_starttime_ticks=16540846 sub_starttime_ticks=16540846 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=2024 sub_pid=2023 | pub_ready_ns=165410682663239 in [165410669154564,165410693848569] | sub_ready_ns=165410681942259 in [165410668993824,165410693848548] | pub_starttime_ticks=16541066 sub_starttime_ticks=16541066 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz` identity: parent_pid=11 pub_pid=2047 sub_pid=2046 | pub_ready_ns=165412885797095 in [165412870707051,165412897508795] | sub_ready_ns=165412884160955 in [165412870548825,165412897508751] | pub_starttime_ticks=16541287 sub_starttime_ticks=16541287 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=2070 sub_pid=2069 | pub_ready_ns=165415086366953 in [165415075612990,165415098337682] | sub_ready_ns=165415086413901 in [165415075513070,165415098337647] | pub_starttime_ticks=16541507 sub_starttime_ticks=16541507 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=2093 sub_pid=2092 | pub_ready_ns=165417292956017 in [165417278528433,165417303352100] | sub_ready_ns=165417293006236 in [165417278345252,165417303352014] | pub_starttime_ticks=16541727 sub_starttime_ticks=16541727 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=2116 sub_pid=2115 | pub_ready_ns=165419495123644 in [165419483181936,165419505875576] | sub_ready_ns=165419494402539 in [165419483031774,165419505875547] | pub_starttime_ticks=16541948 sub_starttime_ticks=16541948 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=2139 sub_pid=2138 | pub_ready_ns=165421694699911 in [165421685052397,165421705691958] | sub_ready_ns=165421696267367 in [165421684888554,165421705691936] | pub_starttime_ticks=16542168 sub_starttime_ticks=16542168 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz` identity: parent_pid=11 pub_pid=2162 sub_pid=2161 | pub_ready_ns=165423896684272 in [165423884479096,165423896906875] | sub_ready_ns=165423895488673 in [165423884322611,165423896906826] | pub_starttime_ticks=16542388 sub_starttime_ticks=16542388 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz` identity: parent_pid=11 pub_pid=2185 sub_pid=2184 | pub_ready_ns=165426086263056 in [165426078090987,165426096672429] | sub_ready_ns=165426086706304 in [165426077925889,165426096672380] | pub_starttime_ticks=16542607 sub_starttime_ticks=16542607 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=2208 sub_pid=2207 | pub_ready_ns=165428289624303 in [165428277084399,165428301835265] | sub_ready_ns=165428289655288 in [165428276919200,165428301835199] | pub_starttime_ticks=16542827 sub_starttime_ticks=16542827 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=2231 sub_pid=2230 | pub_ready_ns=165430512426677 in [165430480212223,165430523408119] | sub_ready_ns=165430512068753 in [165430480108329,165430523407973] | pub_starttime_ticks=16543048 sub_starttime_ticks=16543048 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz` identity: parent_pid=11 pub_pid=2254 sub_pid=2253 | pub_ready_ns=165432715510817 in [165432702814718,165432727572297] | sub_ready_ns=165432713280892 in [165432702641515,165432727572228] | pub_starttime_ticks=16543270 sub_starttime_ticks=16543270 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=2277 sub_pid=2276 | pub_ready_ns=165434920862893 in [165434909220796,165434936025943] | sub_ready_ns=165434922925570 in [165434909041800,165434936025695] | pub_starttime_ticks=16543490 sub_starttime_ticks=16543490 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=2300 sub_pid=2299 | pub_ready_ns=165437124696834 in [165437113377177,165437127863520] | sub_ready_ns=165437124671449 in [165437113205979,165437127863400] | pub_starttime_ticks=16543711 sub_starttime_ticks=16543711 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz` identity: parent_pid=11 pub_pid=2323 sub_pid=2322 | pub_ready_ns=165439314828331 in [165439305863576,165439328551033] | sub_ready_ns=165439318083480 in [165439305704545,165439328551002] | pub_starttime_ticks=16543930 sub_starttime_ticks=16543930 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- **A/B 可分性只在发布侧成立**：实测 `dzflat-a` 与 `dzflat-b` 两档的消费者侧`via_view`/`via_object` **完全相同**（各 13500 / 0，10 轮 full 档合计）⇒ `via_view` **不是** B 档的区分判据（早前文档把它写成 B 档专属判据，已更正）。可分性证据在**发布侧**：`wire_bytes_source`（A=`对象 dzflat_size()` / B=`B 借样 chunk 容量`）与实际路径条数计数。
- **失败量阈值是本基准自定的预算**（`manifest.failure_thresholds`）：迟发默认「次数>5 **且** 比率>0.5%」才判失败。它是**可判**而非**无条件失败**；引用时必须连阈值一起引。
- `r0_tlv_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 152910 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r1_tlv_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 7039 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r2_tlv_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 16729 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r3_tlv_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 36337 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r4_tlv_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 53391 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r5_tlv_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 216724 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r6_tlv_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6660 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r7_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 134234 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r8_tlv_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 97298 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r9_tlv_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 60102 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r10_tlv_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 5255 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r11_tlv_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 14898 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r12_tlv_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 70687 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r13_tlv_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 6175 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r14_tlv_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 19312 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r15_dzflat-a_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 6078 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r16_dzflat-a_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5030 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r17_dzflat-a_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 72396 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r18_dzflat-a_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 21709 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r19_dzflat-a_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 143914 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r20_dzflat-a_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 53880 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r21_dzflat-a_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 31469 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r22_dzflat-a_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 5234 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r23_dzflat-a_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 155817 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r24_dzflat-a_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 43003 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r25_dzflat-a_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 8028 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r26_dzflat-a_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 6375 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r27_dzflat-a_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 4483 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r28_dzflat-a_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 20226 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r29_dzflat-a_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 247459 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r30_dzflat-b_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 120791 ns（迟发 5 次）—— 该轮延迟含排队成分
- `r31_dzflat-b_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 43509 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r32_dzflat-b_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5505 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r33_dzflat-b_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 5196 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r34_dzflat-b_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6480 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r35_dzflat-b_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 102537 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r36_dzflat-b_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 47296 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r37_dzflat-b_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 148756 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r38_dzflat-b_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 4118 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r39_dzflat-b_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 91104 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r40_dzflat-b_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 151741 ns（迟发 13 次）—— 该轮延迟含排队成分
- `r41_dzflat-b_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 18474 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r42_dzflat-b_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 110091 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r43_dzflat-b_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 17038 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r44_dzflat-b_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5200 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r45_cyclonedds-udp_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5763 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r46_cyclonedds-udp_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5129 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r47_cyclonedds-udp_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 54886 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r48_cyclonedds-udp_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 115917 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r49_cyclonedds-udp_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 180631 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r50_cyclonedds-udp_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 25963 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r51_cyclonedds-udp_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 11521 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r52_cyclonedds-udp_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 52023 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r53_cyclonedds-udp_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 6323 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r54_cyclonedds-udp_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 51771 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r55_cyclonedds-udp_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 6421 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r56_cyclonedds-udp_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 4675 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r57_cyclonedds-udp_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 229943 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r58_cyclonedds-udp_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 70709 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r59_cyclonedds-udp_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 572607 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r60_cyclonedds-iox_timestamp_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 119599 ns（迟发 3 次）—— 该轮延迟含排队成分
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r61_cyclonedds-iox_crc_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 87408 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r62_cyclonedds-iox_full_blocking_64B_1000Hz`: 定速档存在迟发，最大积压 5485 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r63_cyclonedds-iox_timestamp_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 283014 ns（迟发 1 次）—— 该轮延迟含排队成分
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r64_cyclonedds-iox_timestamp_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 42999 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r65_cyclonedds-iox_crc_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 93368 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r66_cyclonedds-iox_crc_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 14786 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r67_cyclonedds-iox_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 169920 ns（迟发 2 次）—— 该轮延迟含排队成分
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r68_cyclonedds-iox_full_busy_1024B_1000Hz`: 定速档存在迟发，最大积压 18874 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r69_cyclonedds-iox_timestamp_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 92908 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r70_cyclonedds-iox_crc_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 76489 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r71_cyclonedds-iox_full_blocking_65536B_1000Hz`: 定速档存在迟发，最大积压 2062068 ns（迟发 8 次）—— 该轮延迟含排队成分
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r72_cyclonedds-iox_timestamp_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 114592 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r73_cyclonedds-iox_crc_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 5236 ns（迟发 0 次）—— 该轮延迟含排队成分
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；来源见 `wire_bytes_source`）
- `r74_cyclonedds-iox_full_blocking_1048576B_500Hz`: 定速档存在迟发，最大积压 107208 ns（迟发 0 次）—— 该轮延迟含排队成分

