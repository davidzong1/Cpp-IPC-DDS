#!/bin/bash
# W05 验收跑批：控制面线程数（两条驱动臂）+ 有效收发 + A/B 计数可分 + 心跳频率 + 死亡检测 + fork 闸。
# 用法：bash artifacts/w05/run_w05_sweep.sh
#
# 两个"臂"分别回答两个不同的问题 —— 混在一起会得出错误结论：
#   · pubs-only：发布者没有收包线程，唯一可变线程来源就是控制面 ⇒ 最干净的读数；
#   · pub+sub  ：端到端有效收发（千路各自收发一条）。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
OUT="$ROOT/artifacts/w05"
P="$OUT/probe/scale"
A="$OUT/probe/ab_counters"
HB="$OUT/probe/heartbeat"
SP="$OUT/probe/stale_peer"
FG="$OUT/probe/fork_gate"
mkdir -p "$OUT"
bash "$OUT/probe/build.sh" >/dev/null

base=1000
run_scale() { # arm n tag extra...
  local arm=$1 n=$2 tag=$3; shift 3
  local log="$OUT/${tag}.log"
  if [ "$arm" = compat ]; then
    DZIPC_SHM_CONTROL_SCHEDULER=1 timeout 400 stdbuf -oL "$P" --n "$n" --domain "$((base++))" "$@" > "$log" 2>&1
  else
    env -u DZIPC_SHM_CONTROL_SCHEDULER timeout 300 stdbuf -oL "$P" --n "$n" --domain "$((base++))" "$@" > "$log" 2>&1
  fi
  echo "  $tag rc=$? | $(grep -E 'threads_after_create=|attached=|sent=|sched_entries=|destroy_total' "$log" | tr '\n' ' ')"
}

echo "== [臂 1] pubs-only：控制面线程数（无数据面线程干扰）"
for arm in sched compat; do
  for n in 1 100 1000; do run_scale "$arm" "$n" "cpubonly_${arm}_$n" --pubs 1 --subs 0; done
done

echo "== [臂 2] pub+sub：千路有效收发（各自收发一条）"
for arm in sched compat; do
  for n in 1 100 1000; do run_scale "$arm" "$n" "scale_${arm}_$n" --pubs 1 --subs 1 --verify 1; done
done

echo "== A/B/三路径计数可分（两条驱动臂）"
for m in tlv dzflat-a dzflat-b; do
  env -u DZIPC_SHM_CONTROL_SCHEDULER timeout 60 stdbuf -oL "$A" "$m" 2>/dev/null > "$OUT/ab_sched_$m.txt"
  echo "  sched/$m  : $(cat "$OUT/ab_sched_$m.txt")"
  DZIPC_SHM_CONTROL_SCHEDULER=1 timeout 60 stdbuf -oL "$A" "$m" 2>/dev/null > "$OUT/ab_compat_$m.txt"
  echo "  compat/$m : $(cat "$OUT/ab_compat_$m.txt")"
done

echo "== 心跳频率（不得降频）"
{
  env -u DZIPC_SHM_CONTROL_SCHEDULER timeout 60 "$HB" 2000 2>/dev/null
  DZIPC_SHM_CONTROL_SCHEDULER=1 timeout 60 "$HB" 2000 2>/dev/null
} | tee "$OUT/heartbeat.log"

echo "== 死亡检测（SIGKILL 子进程后 stale 槽位回收）"
{
  env -u DZIPC_SHM_CONTROL_SCHEDULER timeout 60 "$SP" 2>/dev/null
  DZIPC_SHM_CONTROL_SCHEDULER=1 timeout 60 "$SP" 2>/dev/null
} | tee "$OUT/stale_peer.log"

echo "== fork 闸（父进程先用过调度器，子进程仍能完成握手）"
timeout 90 "$FG" 2>/dev/null | tee "$OUT/fork_gate.log"
echo "SWEEP_DONE"
