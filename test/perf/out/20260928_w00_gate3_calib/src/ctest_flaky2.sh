#!/bin/bash
# 不中断的并发 ctest 循环：累计每个用例的失败次数（D-6 失败率判据）
OUT=$1; N=$2; J=$3
mkdir -p "$OUT"
: > "$OUT/summary.tsv"; : > "$OUT/failures.txt"
for i in $(seq 1 "$N"); do
  (cd /home/zwc/cpp_ipc_dds && ctest --test-dir build -j"$J" --output-on-failure) > "$OUT/run_$i.log" 2>&1
  rc=$?
  f=$(grep -E '^\s+[0-9]+ - ' "$OUT/run_$i.log" | sed 's/^[[:space:]]*//' | tr '\n' '|')
  printf '%s\trc=%s\t%s\n' "$i" "$rc" "$f" >> "$OUT/summary.tsv"
  [ "$rc" != "0" ] && { echo "=== run $i rc=$rc: $f" >> "$OUT/failures.txt"; }
done
echo DONE >> "$OUT/summary.tsv"
