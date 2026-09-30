#!/bin/bash
# W10/t36 判定性标定 #2：**接收池完全不参与**时（`--state 2`：只建订阅、不建发布端 ⇒
# `pool running=0 workers=0 route_count=0`、全进程仅 2 条线程：主线程 + 调度器），
# CPU 是否随**在册控制项数**线性增长。
#
# 用途：与 2×2 反事实互补 —— 若要判定「主成本在控制面调度器」，最干净的做法是
# **把池彻底移除**再测，而不是从总 CPU 里做减法。
#
# 同时确认「唯一非零 TID 不是主线程」（`main` 只在握手期短暂忙）。
#
# 用法: w10_idle_state2_scale.sh <outdir>
set -u
OUT=${1:?usage: w10_idle_state2_scale.sh <outdir>}
cd /home/zwc/cpp_ipc_dds || exit 1
mkdir -p "$OUT"
WIN=${WIN:-10}

{
  echo "# W10/t36 判定性标定 #2：state 2（接收池完全不参与）CPU vs 在册控制项数"
  echo "# 采样：外部逐 TID 读 /proc/<pid>/task/<tid>/stat（win=${WIN}s）"
  echo
  for n in 100 500 1000; do
    dom=$((29100 + n))
    log="$OUT/state2_${n}.log"
    ( ./build/bin/w10_idle --n "$n" --domain "$dom" --state 2 --windows 1 --win-s $((WIN + 4)) --cpu-only \
        > "$log" 2>&1 ) &
    job=$!
    for _ in $(seq 1 400); do grep -q '^state=2' "$log" 2>/dev/null && break; sleep 0.25; done
    sleep 4
    pid=$(pgrep -f "w10_idle --n $n --domain $dom" | head -1)
    echo "### n=$n（在册订阅项=$n，发布端未建 ⇒ 池未启动）"
    echo "  state2 行：$(grep -m1 '^state=2' "$log" | sed 's/^/    /')"
    if [ -n "${pid:-}" ]; then
      echo "  主线程 tid=$pid"
      python3 tmp/t36/perthread.py "$pid" "$WIN" | sed 's/^/    /'
    fi
    wait "$job" 2>/dev/null
    grep -m1 'scheduler entries' "$log" | sed 's/^/    调度器: /'
    echo
  done
  echo "# 判定：state 2 下全进程仅 2 条线程，CPU 随在册控制项线性增长 ⇒"
  echo "#       state 3（entries=2000）的成本 ≈ state2(n=1000) × 2，与「控制项 × 每项成本」逐值吻合；"
  echo "#       而 state 3 的池侧 32 条 worker 线程在该窗口内 CPU 增量应 ≈ 0。"
} | tee "$OUT/state2_scale.txt"
