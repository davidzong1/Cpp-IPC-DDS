#!/usr/bin/env bash
# t40 任务2：验证「启动竞态」假设 —— warmup 加长是否消除 publish() 超时（failed>0）
OUT="${1:-build/t40/calib_warmup}"
mkdir -p "$OUT"
clear_pool(){ local s; for s in /dev/shm/*CHUNK_INFO__*; do [ -e "$s" ] || continue
  grep -lq "$(basename "$s")" /proc/[0-9]*/maps 2>/dev/null || rm -f "$s"; done; }
echo "# warmup 扫描（每档 6 轮，每轮清池）  开始 $(date '+%T')"
printf "%-8s %-4s %-8s %-8s %-8s %-8s\n" warmup 轮 sent failed recv/s "s/(5-.1f)"
for w in 0.5 1 3 5; do
  for i in 1 2 3 4 5 6; do
    clear_pool
    timeout 200 ./build/bin/dzipc_perf_benchmark --cases=pubsub_shm --payloads=1048576 \
      --duration=5 --warmup="$w" --pin=5,6 --out="$OUT/w${w}_r$i" > "$OUT/w${w}_r$i.log" 2>&1
    python3 - "$OUT/w${w}_r$i" "$w" "$i" <<'PY'
import json,sys,os
d=json.load(open(f"{sys.argv[1]}/results.json"))
for c in d.get("cases",d.get("results",[])):
    if c.get("test_kind")=="tput":
        s,f,r=c.get("sent",0),c.get("failed",0),c.get("recv",0)
        rem=5-0.1*f
        print(f"{sys.argv[2]:<8} {sys.argv[3]:<4} {s:<8} {f:<8} {r/5:<8.0f} {(r/rem if rem>0.05 else float('nan')):<8.0f}")
PY
  done
done
echo "# 结束 $(date '+%T')"
