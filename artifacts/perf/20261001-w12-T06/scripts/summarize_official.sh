#!/usr/bin/env bash
# T06 / W12 §8 —— 把官方工装各臂读数抽成矩阵（只读，不跑任何东西）。
#
# 用法: summarize_official.sh artifacts/perf/20261001-w12-T06/official-matrix
set -u
ROOT=${1:?matrix root}
printf "%-10s %-16s %-6s %-9s %-8s %-12s %-10s %s\n" \
       arm lib_sha16 rc gtest bad_rounds skipped orphan_reset prefix
for d in "$ROOT"/*/; do
  [ -f "$d/fingerprint.txt" ] || continue
  arm=$(basename "$d")
  fp=$d/fingerprint.txt
  lib=$(grep -m1 '^lib_sha256' "$fp" | awk '{print $3}')
  rc=$(grep -m1 '^exe_rc' "$fp" | awk '{print $3}')
  verdict=$(grep -m1 '^gtest_verdict' "$fp" | sed 's/^gtest_verdict *= *//')
  reading=$(grep -m1 '^reading' "$fp" | sed 's/^reading *= *//')
  bad=$(echo "$reading" | grep -oE 'bad_rounds=[0-9]+' | head -1)
  skip=$(echo "$reading" | grep -oE 'skipped=[0-9]+' | head -1)
  resets=$(grep -m1 '^orphan_segment_reset_count' "$fp" | awk '{print $3}')
  printf "%-18s %-16s %-3s %-8s %-12s %-10s %-4s %s\n" \
         "$arm" "${lib:0:16}" "$rc" "${verdict:+$(echo "$verdict" | grep -oE 'PASSED|FAILED')}" \
         "${bad:-?}" "${skip:-?}" "${resets:-?}" "w09c9alias"
done
