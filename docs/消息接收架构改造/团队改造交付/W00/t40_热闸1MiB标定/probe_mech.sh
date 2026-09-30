#!/usr/bin/env bash
OUT="${1:-build/t40/probe}"; mkdir -p "$OUT"
clear_pool(){ local s; for s in /dev/shm/*CHUNK_INFO__*; do [ -e "$s" ] || continue
  grep -lq "$(basename "$s")" /proc/[0-9]*/maps 2>/dev/null || rm -f "$s"; done; }
run(){ # $1=tag  $2..=extra args
  local tag="$1"; shift
  for i in 1 2 3 4 5 6; do
    clear_pool
    timeout 200 ./build/bin/dzipc_perf_benchmark --cases=pubsub_shm --payloads=1048576 \
      --duration=5 --pin=5,6 --out="$OUT/${tag}_r$i" "$@" > "$OUT/${tag}_r$i.log" 2>&1
    python3 - "$OUT/${tag}_r$i" "$tag" "$i" <<'PY'
import json,sys
d=json.load(open(f"{sys.argv[1]}/results.json"))
cs={c.get("test_kind"):c for c in d.get("cases",d.get("results",[]))}
t=cs.get("tput",{}); l=cs.get("lat",{})
print(f"{sys.argv[2]:<18} r{sys.argv[3]:<2} lat={l.get('recv')}/5s  tput: sent={t.get('sent'):<6} recv={t.get('recv'):<6} failed={t.get('failed'):<4} cpu={t.get('cpu_cores'):.3f}")
PY
  done
}
echo "# 机制探针  开始 $(date '+%T')"
run queue1024_default
run queue4096        --queue=4096
run pubtimeout5      --pub-timeout=5
run besteffort       --best-effort
run payload64        --payloads=64
echo "# 结束 $(date '+%T')"
