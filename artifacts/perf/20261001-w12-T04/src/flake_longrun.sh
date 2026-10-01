#!/usr/bin/env bash
# T04 L1 偶发独立复现长跑（串行，quiet 环境；⛔ 不与其他同名用例并发 —— 见报告 §偶发判别）
# 输出：$A/logs/flake_*.log（逐次原始日志）、$A/flake/*.csv（逐次读数）
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
B=$R/wt/b
P=$R/bin/t04_probe
mkdir -p $A/flake
G=$A/flake/gate_runs.csv
AR=$A/flake/arm_runs.csv
PR=$A/flake/probe_runs.csv
{
  echo "kind,idx,rc,ms,criterion1_reaped,reap_ms,rx,note"
} > $G
{
  echo "kind,idx,rc,ms,arm_default,arm_L1,note"
} > $AR
{
  echo "arm,idx,rc,n,received,misses,first_miss_at,criterion1_reaped,reap_ms"
} > $PR

ms() { date +%s%3N; }

# ---- A) L1 回退臂门控用例：200 次（T02 读数 1/158 的独立复现样本） ----
for i in $(seq 1 200); do
  s=$(ms)
  timeout 60 env DZIPC_SHM_CONTROL_SCHEDULER=1 $B/bin/test_w05_stale_slot_gate > $A/logs/flake_gate_L1_$i.log 2>&1
  rc=$?
  e=$(ms)
  rx=$(grep -oP 'Which is: \K[0-9]+' $A/logs/flake_gate_L1_$i.log | head -1)
  c1=$(grep -c ':146' $A/logs/flake_gate_L1_$i.log)
  echo "gate_L1,$i,$rc,$((e-s)),criterion1_assert_failures=$c1,rx=${rx:-100},$( [ $rc -eq 0 ] && echo pass || echo FAIL )" >> $G
  if [ $rc -ne 0 ]; then cp $A/logs/flake_gate_L1_$i.log $A/flake/FAILED_gate_L1_$i.log; fi
done

# ---- B) 默认臂门控用例：80 次（对照） ----
for i in $(seq 1 80); do
  s=$(ms)
  timeout 60 $B/bin/test_w05_stale_slot_gate > $A/logs/flake_gate_DEFAULT_$i.log 2>&1
  rc=$?; e=$(ms)
  echo "gate_default,$i,$rc,$((e-s)),$( [ $rc -eq 0 ] && echo pass || echo FAIL )" >> $G
  if [ $rc -ne 0 ]; then cp $A/logs/flake_gate_DEFAULT_$i.log $A/flake/FAILED_gate_DEFAULT_$i.log; fi
done

# ---- C) 双臂用例：50 次（解析哪一臂失败：失败信息 "默认臂失败码 X" / "L1 回退臂失败码 X"） ----
for i in $(seq 1 50); do
  s=$(ms)
  timeout 90 $B/bin/test_w05_stale_slot_gate_arm > $A/logs/flake_arm_$i.log 2>&1
  rc=$?; e=$(ms)
  d=$(grep -c '默认臂失败码' $A/logs/flake_arm_$i.log)
  l=$(grep -c 'L1 回退臂失败码' $A/logs/flake_arm_$i.log)
  echo "arm,$i,$rc,$((e-s)),$d,$l,$( [ $rc -eq 0 ] && echo pass || echo FAIL )" >> $AR
  if [ $rc -ne 0 ]; then cp $A/logs/flake_arm_$i.log $A/flake/FAILED_arm_$i.log; fi
done

# ---- D) 本复核探针（判据①/②分离读数）：默认臂 40 次 + L1 臂 100 次，各 100 条 ----
for i in $(seq 1 40); do
  timeout 90 $P $((20000+i)) 100 > $A/logs/flake_probe_DEFAULT_$i.log 2>&1
  rc=$?
  grep '^RESULT' $A/logs/flake_probe_DEFAULT_$i.log \
    | sed -E 's/.*n=([0-9]+) received=([0-9]+) misses=([0-9]+) first_miss_at=(-?[0-9]+) criterion1_reaped=([01]) reap_ms=(-?[0-9]+).*/default,'"$i"',$rc,\1,\2,\3,\4,\5,\6/' >> $PR
done
for i in $(seq 1 100); do
  timeout 90 env DZIPC_SHM_CONTROL_SCHEDULER=1 $P $((21000+i)) 100 > $A/logs/flake_probe_L1_$i.log 2>&1
  rc=$?
  grep '^RESULT' $A/logs/flake_probe_L1_$i.log \
    | sed -E 's/.*n=([0-9]+) received=([0-9]+) misses=([0-9]+) first_miss_at=(-?[0-9]+) criterion1_reaped=([01]) reap_ms=(-?[0-9]+).*/L1,'"$i"',$rc,\1,\2,\3,\4,\5,\6/' >> $PR
  if [ $rc -ne 0 ]; then cp $A/logs/flake_probe_L1_$i.log $A/flake/FAILED_probe_L1_$i.log; fi
done
echo "T04_FLAKE_RUN_DONE"
