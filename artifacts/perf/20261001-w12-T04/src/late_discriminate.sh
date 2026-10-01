#!/usr/bin/env bash
# T04 判别实验 v2：两种机制分开看
#   (a) 内容错配（cross-talk：收到的是"别的"消息）—— v6 打印 MISMATCH
#   (b) 迟到（放宽容内到达）—— v6 打印 LATE + rx_late
#   (c) 真丢失（放宽容耗尽仍不到）—— MISS + extended_wait exhausted
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
OUT=$A/flake/late_runs.csv
{ echo "arm,idx,rc,n,received,misses,first_miss_at,c1_reaped,reap_ms,rx_late,late_max_ms,mismatch_total,first_mismatch_at,wait_ms,late_ms,mean_us"; } > $OUT
for i in $(seq 1 200); do
  env DZIPC_SHM_CONTROL_SCHEDULER=1 T04_LATE_MS=1500 timeout 120 $R/bin/t04_probe_v6 $((50000+i)) 100 > $A/logs/late_L1_$i.log 2>&1
  rc=$?
  g(){ grep -o "$1=[-0-9]*" $A/logs/late_L1_$i.log | head -1 | cut -d= -f2; }
  rec=$(g "$A/logs/late_L1_$i.log" 2>/dev/null || true)
  rec=$(grep -o 'received=[0-9]*' $A/logs/late_L1_$i.log | head -1 | cut -d= -f2)
  mu=$(grep -o 'mean=[0-9]*us' $A/logs/late_L1_$i.log | head -1 | tr -dc '0-9')
  echo "L1,$i,$rc,100,${rec:-?},$(grep -o 'misses=[0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'first_miss_at=[-0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'criterion1_reaped=[01]' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'reap_ms=[-0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'rx_late=[0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'late_max_ms=[0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'mismatch_total=[0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),$(grep -o 'first_mismatch_at=[-0-9]*' $A/logs/late_L1_$i.log|head -1|cut -d= -f2),200,1500,${mu:-?}" >> $OUT
  if [ "${rec:-0}" != "100" ]; then cp $A/logs/late_L1_$i.log $A/flake/FAILED_late_L1_$i.log; fi
done
echo T04_LATE_DONE
