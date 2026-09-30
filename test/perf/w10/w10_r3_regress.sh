#!/bin/bash
# W10-R3 修后回归矩阵（t37）：SHM 1/1000、广播 32、hotcold（含 F6 的 sent 列）、socket 1。
# 用法: w10_r3_regress.sh [<run_root>]
set -u
cd /home/zwc/cpp_ipc_dds || exit 1
R=${1:-artifacts/perf/20260930-r28-W10-R3/regress2}
mkdir -p "$R"
B=build/bin/w10_matrix
run(){ n=$1; shift; d=$R/$n; rm -rf $d; timeout 900 $B "$@" --out $d > $R/$n.log 2>&1; rc=$?
  printf '%-16s rc=%-4s %s\n' "$n" "$rc" "$(grep -oE 'verdict=(PASS|FAIL) failures=[0-9]+' $R/$n.log|tail -1)"; }
run shm-ind-1     --transport shm    --topology independent --n 1    --domain 3001 --msgs 3 --payload 64 --run-id h-shm-1
run shm-ind-1000  --transport shm    --topology independent --n 1000 --domain 3003 --msgs 3 --payload 64 --run-id h-shm-1000
run shm-bcast-32  --transport shm    --topology broadcast   --n 32   --domain 3010 --msgs 3 --payload 64 --run-id h-bc-32
run shm-hotcold   --transport shm    --topology hotcold     --n 1000 --domain 3020 --hot-msgs 20000 --run-id h-hc
run socket-ind-1  --transport socket --topology independent --n 1    --domain 3101 --msgs 3 --payload 64 --run-id h-sk-1
echo REGRESS_DONE
