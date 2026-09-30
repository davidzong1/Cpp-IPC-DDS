#!/bin/bash
# F1/t35 同臂复跑驱动：五类各跑一次，落盘到 $1/
# 用法: run_all_arms.sh <outdir> <tag>
set -u
OUT="$1"; TAG="$2"
A=/home/zwc/cpp_ipc_dds/artifacts/perf/20260929-r33-W03-counterwiring
export LD_LIBRARY_PATH=/home/zwc/cpp_ipc_dds/build/lib:/home/zwc/cpp_ipc_dds/build/bin
mkdir -p "$OUT"
for arm in A B C D E2; do
  rm -f /dev/shm/* 2>/dev/null; rmdir /dev/shm/__IPC_SHM__RD_CONN__* 2>/dev/null
  case $arm in
    A)  timeout 200 $A/f1_wiring_probe A  > "$OUT/${TAG}_armA.log" 2>&1 ;;
    B)  timeout 200 $A/f1_wiring_probe B  > "$OUT/${TAG}_armB.log" 2>&1 ;;
    C)  timeout 300 $A/f1_wiring_probe C  > "$OUT/${TAG}_armC.log" 2>&1 ;;
    D)  DZIPC_SHM_RECV_WORKERS=1 timeout 300 $A/armD_waitset_full 140 > "$OUT/${TAG}_armD.log" 2>&1 ;;
    E2) timeout 200 $A/armE2_token_invalid state > "$OUT/${TAG}_armE2.log" 2>&1 ;;
  esac
  echo "$arm rc=$?"
done
rmdir /dev/shm/__IPC_SHM__RD_CONN__* 2>/dev/null
echo "ARMS_DONE tag=$TAG"
