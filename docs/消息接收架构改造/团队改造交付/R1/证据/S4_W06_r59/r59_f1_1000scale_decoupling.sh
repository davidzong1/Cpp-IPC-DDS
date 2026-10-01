#!/bin/bash
# t59 独立判据：**「千路绿」不能区分有没有 W06-F1**。
# 做法：临时把 loop 还原成旧顺序（先 wait 再 drain），跑 1000 独立话题规模探针。
# 若旧顺序下 valid_rx 仍 = 1000/1000（而同一二进制在 --case=lat 上是 FAIL），
# 就直接证明"千路绿 ≠ 无 W06-F1"，与 W06 报告 §3.4 的声明一致。
set -u
ROOT=/home/zwc/cpp_ipc_dds; cd "$ROOT"
SRC=src/dzIPC/threepools/recv_worker.cc
RUN="docs/消息接收架构改造/团队改造交付/R1/证据/S4_W06_r59"
PROBE="$RUN/w06_scale_r59"; LAT="$RUN/w06_regress_r59"
LOG="$RUN/r59_f1_1000scale_decoupling.log"
BAK="$(mktemp /tmp/r59_dec.XXXXXX.cc)"; cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC" | cut -d' ' -f1)"
restore() {
  cp "$BAK" "$SRC"
  local a; a="$(sha256sum "$SRC" | cut -d' ' -f1)"
  echo "restore_sha256=$a" >> "$LOG"
  [ "$a" = "$BEFORE" ] || echo "FATAL restore mismatch" >> "$LOG"
  make -C build -j8 >/dev/null 2>&1 || echo "WARN rebuild" >> "$LOG"
  rm -f "$BAK"
}
trap restore EXIT

{
  echo "run_id=20260930-r59-R1-W06"; echo "date=$(date -Is)"; echo "before_sha256=$BEFORE"
  echo "== [独立] 修后（当前树）：延迟判据 与 千路规模 =="
} > "$LOG"
"$LAT" --case=lat --domain 2691 --rounds=20 2>&1 | grep -E "case=lat|verdict" >> "$LOG"
"$PROBE" --n 1000 --domain 2692 2>&1 | grep -E "registered_count|valid_rx_count|counters |worker_path_count|registered_after_rx" >> "$LOG"

python3 - "$SRC" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
i=s.index('        const std::uint64_t msgs_before =')
j=s.index('        idle_since = Clock::now();   // ', i)
open(p,'w',encoding='utf-8').write(s[:i]+'        wait_once(budget.wait_timeout);\n        drain_deferred();\n'+s[j:])
PY
make -C build -j8 >/dev/null 2>&1
{
  echo
  echo "== [独立] 反事实（旧顺序 = 有 W06-F1）：延迟判据 与 千路规模 =="
} >> "$LOG"
"$LAT" --case=lat --domain 2693 --rounds=10 2>&1 | grep -E "case=lat|verdict" >> "$LOG"
"$PROBE" --n 1000 --domain 2694 2>&1 | grep -E "registered_count|valid_rx_count|counters |worker_path_count|registered_after_rx" >> "$LOG"
