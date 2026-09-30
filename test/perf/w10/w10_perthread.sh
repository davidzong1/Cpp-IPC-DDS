#!/bin/bash
# W10/t36 逐 TID CPU 归因采样（**外部**读 /proc，避免把采样器自身成本混入）。
#
# 与 R1/t31 的 w10_perthread.py 同口径，本脚本补两件事：
#   ① 用 /proc/<pid>/comm 而不是 pgrep -f 找 pid —— 本会话的进程被 bwrap 包了一层，
#      `pgrep -f` 会命中包装命令本身（实测拿到 pid=1、线程数=1 的假读数）；
#   ② 把**逐 TID 明细**落盘（r25 只存了汇总行，无法从证据目录自证归因 —— R1-F2 要求②）。
#
# 用法: w10_perthread.sh <out_prefix> <n> <domain> <state> [win] [settle]
set -u
cd /home/zwc/cpp_ipc_dds || exit 1
PREFIX=${1:?usage: w10_perthread.sh <out_prefix> <n> <domain> <state> [win] [settle]}
N=${2:?}; DOM=${3:?}; STATE=${4:?}; WIN=${5:-10}; SETTLE=${6:-6}

"$(dirname "$0")/../../..//tmp/t36/findpid.py" >/dev/null 2>&1 || true

( ./build/bin/w10_idle --n "$N" --domain "$DOM" --state "$STATE" --windows 1 --win-s $((SETTLE + WIN + 3)) --cpu-only \
    > "${PREFIX}.proc.log" 2>&1 ) &
JOB=$!
for _ in $(seq 1 400); do grep -q "^state=${STATE}" "${PREFIX}.proc.log" 2>/dev/null && break; sleep 0.25; done
sleep "$SETTLE"
PID=$(python3 tmp/t36/findpid.py w10_idle | head -1)
{
  echo "# 逐 TID CPU 归因（外部读 /proc/$PID/task/*/stat）"
  echo "# 目标: n=$N domain=$DOM state=$STATE window=${WIN}s"
  echo "# 进程内线程数（采样时刻）: $(ls /proc/${PID}/task 2>/dev/null | wc -l)"
  if [ -n "${PID:-}" ]; then python3 tmp/t36/perthread2.py "$PID" "$WIN"; fi
} | tee "${PREFIX}.tid.txt"
wait "$JOB" 2>/dev/null
grep -E "^state=|^0,|scheduler|pool running" "${PREFIX}.proc.log" | head -4 | tee -a "${PREFIX}.tid.txt"
