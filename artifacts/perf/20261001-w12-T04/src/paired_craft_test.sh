#!/usr/bin/env bash
# T04 判别实验（决定性）：**交错**跑 craft=1（有陈旧槽位）与 craft=0（干净）同一 L1 臂同一规模，
# 同一时刻同一负载 ⇒ 直接比较"陈旧槽位是否让判据②更易失败"。
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
P=$R/bin/t04_probe_v6
OUT=$A/flake/paired_craft.csv
{ echo "round,idx,craft,rc,received,misses,first_miss_at,c1_reaped,reap_ms,rx_late,mismatch_total,mean_us,loadavg1"; } > $OUT
for i in $(seq 1 60); do
  for c in 1 0; do
    m=""; [ "$c" = "0" ] && m=nocraft
    env DZIPC_SHM_CONTROL_SCHEDULER=1 T04_LATE_MS=1200 timeout 120 $P $((90000 + i*2 + c)) 100 $m > $A/logs/paired_${i}_craft${c}.log 2>&1
    rc=$?
    echo "$i,$i,$c,$rc,$(grep -o 'received=[0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'misses=[0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'first_miss_at=[-0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'criterion1_reaped=[01]' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'reap_ms=[-0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'rx_late=[0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'mismatch_total=[0-9]*' $A/logs/paired_${i}_craft${c}.log|head -1|cut -d= -f2),$(grep -o 'mean=[0-9]*us' $A/logs/paired_${i}_craft${c}.log|head -1|tr -dc '0-9'),$(cut -d' ' -f1 /proc/loadavg)" >> $OUT
  done
done
echo T04_PAIRED_DONE
