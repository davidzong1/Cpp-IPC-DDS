#!/bin/bash
# W10/t36 判据：空闲 CPU 归因的 **2×2 反事实**（调度器 ON/OFF × 接收池 ON/OFF）。
#
# 为什么需要它：我在 W10 交付 §3.2/§3.3 曾断言「0.58 core 中 ≈0.51 core 来自 RecvWorker 池的
# collect_pending() O(route) 全扫」。R1 分部复核（t31）用 2×2 反事实判定该归因**与反事实相反**
# —— 池整体停用后 CPU 不降反升。本脚本是**独立复现**（⛔ 不采信转述、不照抄结论）。
#
# 判据：C 格（调度器 ON + 池 **OFF**，`DZIPC_SHM_RECV_COMPAT=1` ⇒ pool running=0/route_count=0，
#       即完全没有 collect_pending()）的 CPU ≥ A 格（两者都 ON）⇒ 归因必须指向控制面调度器。
# 采样方式：**外部**逐 TID 读 /proc/<pid>/task/<tid>/stat（第 14+15 字段），
#       ⛔ 不用进程内采样（进程内会把采样器自身的 CPU 混入读数）。
#
# 用法: w10_idle_attribution.sh <outdir>
set -u
OUT=${1:?usage: w10_idle_attribution.sh <outdir>}
cd /home/zwc/cpp_ipc_dds || exit 1
mkdir -p "$OUT"
N=${N:-1000}
WIN=${WIN:-10}
SETTLE=${SETTLE:-6}

cell() {  # cell <tag> <sch ON|OFF> <pool ON|OFF> <domain>
  local tag=$1 sch=$2 pool=$3 dom=$4
  local log="$OUT/cell_${tag}.log"
  if [ "$sch" = "OFF" ]; then export DZIPC_SHM_CONTROL_SCHEDULER=1; else unset DZIPC_SHM_CONTROL_SCHEDULER; fi
  if [ "$pool" = "OFF" ]; then export DZIPC_SHM_RECV_COMPAT=1; else unset DZIPC_SHM_RECV_COMPAT; fi

  # 后台启动被测进程；先前台模式不好拿 pid，这里用 --windows 1 --win-s 足够长
  ( ./build/bin/w10_idle --n "$N" --domain "$dom" --state 3 --windows 1 --win-s $((SETTLE+WIN+2)) --cpu-only \
      > "$log" 2>&1 ) &
  local job=$!
  # 等握手完成
  for _ in $(seq 1 400); do grep -q '^state=3' "$log" 2>/dev/null && break; sleep 0.25; done
  sleep "$SETTLE"
  local pid
  pid=$(pgrep -f "w10_idle --n $N --domain $dom" | head -1)
  echo "### cell $tag : scheduler=$sch pool=$pool  pid=${pid:-none}"
  if [ -n "${pid:-}" ]; then
    python3 tmp/t36/perthread.py "$pid" "$WIN" | sed 's/^/    /'
  fi
  wait "$job" 2>/dev/null
  grep -E '^state=|^0,|scheduler' "$log" | head -3 | sed 's/^/    /'
  unset DZIPC_SHM_CONTROL_SCHEDULER DZIPC_SHM_RECV_COMPAT
  echo
}

{
  echo "# W10/t36 2×2 反事实（外部逐 TID 读 /proc；N=$N win=${WIN}s settle=${SETTLE}s）"
  echo "# 变量：DZIPC_SHM_CONTROL_SCHEDULER=1 ⇒ 控制面改走 per-topic 兼容线程（调度器路径 OFF）"
  echo "#       DZIPC_SHM_RECV_COMPAT=1     ⇒ 接收改走 per-topic 兼容线程（RecvWorker 池 OFF）"
  echo
  cell A "${SCH_A:-ON}"  ON  "$((28100))"
  cell C "${SCH_C:-ON}"  OFF "$((28200))"
  cell B OFF ON  "$((28300))"
  cell D OFF OFF "$((28400))"
  echo "# 判定：若 C 的总 CPU ≥ A（且 A/C 的唯一非零 TID 都是调度器线程），"
  echo "#       则『主成本来自 RecvWorker 池 collect_pending 全扫』被证伪。"
} | tee "$OUT/attribution.txt"
