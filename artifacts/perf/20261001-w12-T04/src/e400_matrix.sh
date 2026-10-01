#!/usr/bin/env bash
# T04 承重判据在**验收规模 400 条**上的重复测量（含宿主负载对照）
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
P=$R/bin/t04_probe_v6
OUT=$A/flake/e400_matrix.csv
{ echo "group,idx,arm,craft,rc,received,misses,first_miss_at,c1_reaped,reap_ms,rx_late,mismatch_total,loadavg1"; } > $OUT
one(){ g=$1; arm=$2; craft=$3; dom=$4; i=$5
  if [ "$arm" = L1 ]; then env DZIPC_SHM_CONTROL_SCHEDULER=1 T04_LATE_MS=1000 timeout 180 $P $dom 400 $craft > $A/logs/e400_${g}_$i.log 2>&1
  else T04_LATE_MS=1000 timeout 180 $P $dom 400 $craft > $A/logs/e400_${g}_$i.log 2>&1; fi
  rc=$?
  echo "$g,$i,$arm,${craft:-craft},$rc,$(grep -o 'received=[0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'misses=[0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'first_miss_at=[-0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'criterion1_reaped=[01]' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'reap_ms=[-0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'rx_late=[0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(grep -o 'mismatch_total=[0-9]*' $A/logs/e400_${g}_$i.log|head -1|cut -d= -f2),$(cut -d' ' -f1 /proc/loadavg)" >> $OUT
}
# G1: L1 臂 × 400 条（验收规模，无人工负载）
for i in $(seq 1 20); do one L1_400 L1 "" $((80000+i)) $i; done
# G2: 默认臂 × 400 条
for i in $(seq 1 10); do one DEF_400 DEF "" $((80100+i)) $i; done
# G3: L1 臂 × 400 条，宿主 32 路 CPU 加压
for i in $(seq 1 32); do ( while :; do :; done ) & done
LOADPIDS=$(jobs -p)
for i in $(seq 1 10); do one L1_400_load L1 "" $((80200+i)) $i; done
kill $LOADPIDS 2>/dev/null; wait 2>/dev/null
# G4: 干净路径（nocraft）L1 臂 × 400 条，宿主加压
for i in $(seq 1 32); do ( while :; do :; done ) & done
LOADPIDS=$(jobs -p)
for i in $(seq 1 10); do one L1_400_nocraft_load L1 nocraft $((80300+i)) $i; done
kill $LOADPIDS 2>/dev/null; wait 2>/dev/null
echo T04_E400_DONE
