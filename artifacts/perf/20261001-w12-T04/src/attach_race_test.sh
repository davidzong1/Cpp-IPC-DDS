#!/usr/bin/env bash
# T04 判别实验（机制定向）：L1 回退臂判据②偶发的"首条丢失"是否为
# 「peer_count>=1（控制面登记）已成立，但数据面 route 接线尚未完成」的竞态？
# 预测：在 peer_count>=1 之后再加 30ms 静默期 ⇒ 失败率显著下降。
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
P=$R/bin/t04_probe_v7
OUT=$A/flake/attach_race.csv
{ echo "group,idx,received,misses,first_miss_at,mismatch_total,rx_late,max_us,settle_ms,loadavg1"; } > $OUT
for i in $(seq 1 150); do
  env DZIPC_SHM_CONTROL_SCHEDULER=1 T04_LATE_MS=1500 T04_SETTLE_MS=0 timeout 60 $P $((110000+i)) 100 nocraft > $A/logs/ar_s0_$i.log 2>&1
  echo "settle0,$i,$(grep -o 'received=[0-9]*' $A/logs/ar_s0_$i.log|head -1|cut -d= -f2),$(grep -o 'misses=[0-9]*' $A/logs/ar_s0_$i.log|head -1|cut -d= -f2),$(grep -o 'first_miss_at=[-0-9]*' $A/logs/ar_s0_$i.log|head -1|cut -d= -f2),$(grep -o 'mismatch_total=[0-9]*' $A/logs/ar_s0_$i.log|head -1|cut -d= -f2),$(grep -o 'rx_late=[0-9]*' $A/logs/ar_s0_$i.log|head -1|cut -d= -f2),$(grep -o 'max=[0-9]*us' $A/logs/ar_s0_$i.log|head -1|tr -dc '0-9'),0,$(cut -d' ' -f1 /proc/loadavg)" >> $OUT
done
for i in $(seq 1 150); do
  env DZIPC_SHM_CONTROL_SCHEDULER=1 T04_LATE_MS=1500 T04_SETTLE_MS=30 timeout 60 $P $((120000+i)) 100 nocraft > $A/logs/ar_s30_$i.log 2>&1
  echo "settle30,$i,$(grep -o 'received=[0-9]*' $A/logs/ar_s30_$i.log|head -1|cut -d= -f2),$(grep -o 'misses=[0-9]*' $A/logs/ar_s30_$i.log|head -1|cut -d= -f2),$(grep -o 'first_miss_at=[-0-9]*' $A/logs/ar_s30_$i.log|head -1|cut -d= -f2),$(grep -o 'mismatch_total=[0-9]*' $A/logs/ar_s30_$i.log|head -1|cut -d= -f2),$(grep -o 'rx_late=[0-9]*' $A/logs/ar_s30_$i.log|head -1|cut -d= -f2),$(grep -o 'max=[0-9]*us' $A/logs/ar_s30_$i.log|head -1|tr -dc '0-9'),30,$(cut -d' ' -f1 /proc/loadavg)" >> $OUT
done
echo T04_ATTACH_RACE_DONE
