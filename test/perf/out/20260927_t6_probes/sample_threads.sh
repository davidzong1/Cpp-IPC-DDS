#!/bin/bash
# t6 external thread-count sampler: samples /proc/<pid>/status Threads while the target runs.
# usage: sample_threads.sh <interval_ms> <out.log> <cmd> [args...]
set -u
iv="$1"; out="$2"; shift 2
: > "$out"
"$@" > "$out.cmdlog" 2>&1 &
pid=$!
start=$(date +%s%3N)
max=0; min=999999
while kill -0 "$pid" 2>/dev/null; do
  n=$(awk '/^Threads:/{print $2}' /proc/$pid/status 2>/dev/null)
  if [ -n "$n" ]; then
    now=$(date +%s%3N)
    echo "$((now-start)) $n" >> "$out"
    if [ "$n" -gt "$max" ]; then max=$n; fi
    if [ "$n" -lt "$min" ]; then min=$n; fi
  fi
  sleep "$(awk "BEGIN{print $iv/1000}")"
done
wait "$pid"; rc=$?
echo "# rc=$rc threads_min=$min threads_max=$max" >> "$out"
echo "sampler: rc=$rc min=$min max=$max lines=$(wc -l < $out)"
