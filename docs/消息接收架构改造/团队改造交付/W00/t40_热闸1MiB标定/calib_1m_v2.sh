#!/usr/bin/env bash
# t40 任务2 v2：门 1 的 1 MiB 档基线标定 —— 每轮前按 hotpath_gate.sh 的 clear_pool 清池
# （v1 未清池 ⇒ 4/20 轮命中 `chunk pool exhausted` ⇒ 那是**脚本差异**不是性能波动）
ROUNDS="${1:-30}"; OUT="${2:-build/t40/calib_v2}"
mkdir -p "$OUT"
clear_pool() {
    local seg
    for seg in /dev/shm/*CHUNK_INFO__*; do
        [ -e "$seg" ] || continue
        grep -lq "$(basename "$seg")" /proc/[0-9]*/maps 2>/dev/null || rm -f "$seg"
    done
}
echo "# t40 1MiB 档基线标定 v2（**每轮清池，与 hotpath_gate.sh 门 1 逐参一致**）"
echo "# 开始 $(date '+%F %T %z')  机器 $(nproc) 核  governor=$(cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor 2>/dev/null||echo NA)"
echo
printf "%-4s %-9s %-9s %-10s %-10s %-8s %-6s\n" 轮 轮前load1 轮后load1 rate_msg_s p50_us p99_us rc
for i in $(seq 1 "$ROUNDS"); do
    clear_pool
    l0=$(cut -d' ' -f1 /proc/loadavg)
    d="$OUT/r$i"
    timeout 200 ./build/bin/dzipc_perf_benchmark --cases=pubsub_shm --payloads=1048576 \
        --duration=5 --pin=5,6 --out="$d" > "$OUT/r$i.log" 2>&1
    rc=$?
    l1=$(cut -d' ' -f1 /proc/loadavg)
    line=$(grep -E "1048576B_tput" "$OUT/r$i.log" | head -1)
    rate=$(printf '%s' "$line" | grep -oE '[0-9]+ msg/s' | head -1 | grep -oE '[0-9]+')
    p50=$(printf '%s' "$line" | grep -oE 'p50=[ ]*[0-9.]+' | grep -oE '[0-9.]+' | head -1)
    p99=$(printf '%s' "$line" | grep -oE 'p99=[ ]*[0-9.]+' | grep -oE '[0-9.]+' | head -1)
    printf "%-4s %-9s %-9s %-10s %-10s %-8s %-6s\n" "$i" "$l0" "$l1" "${rate:-FAILED}" "${p50:-NA}" "${p99:-NA}" "$rc"
    printf "%s,%s,%s,%s,%s,%s\n" "$i" "$l0" "${rate:-}" "${p50:-}" "${p99:-}" "$rc" >> "$OUT/rounds.csv"
done
echo
echo "# 结束 $(date '+%F %T %z')"
