#!/bin/bash
# W06/t53 —— R0-9/R0-10 结束值接线的**同臂复跑**驱动（新 run_id；只追加）。
#
# 跑什么：
#   ① gauge    判决性反例：单个 gauge 不能表达全池总量（Σ 每 worker vs 单一 gauge）
#   ② conserve 尾部常数标定（守恒判据 B5b 的输入；与接线同形态）
#   ③ scale    N=1/100/500/1000 × diag=on/off（**每档一个独立进程**，走真实产品路径）
#
# 用法: bash test/perf/w10/w10_o3_driver.sh <out_dir>
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"; cd "$ROOT"
OUT=${1:?usage: w10_o3_driver.sh <out_dir>}
if [ -d "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "REFUSE: $OUT 非空（只追加）"; exit 2; fi
mkdir -p "$OUT"
RUN_ID=$(basename "$OUT")
BIN=build/bin/w10_o3_endvalues

# ---- 自包含构建（与 W10 其余工装同一编译方式；⛔ 不进 GLOB）----
echo "[build] w10_o3_endvalues"
g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I . -I 3rdparty \
    test/perf/w10/w10_o3_endvalues.cpp -o "$BIN" \
    -L build/lib -lipc -lpthread -lrt -Wl,-rpath,"$ROOT/build/lib" || exit 4

{
  echo "# W06/t53 结束值接线同臂复跑（R0-9 / R0-10）"
  echo "# 时间: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "## git"; git rev-parse HEAD
  echo "## 库与工装指纹"; sha256sum build/lib/libipc.so.1.3.0 "$BIN" \
      src/dzIPC/threepools/recv_worker.cc include/dzIPC/threepools/recv_worker.h \
      include/dzIPC/measure/counters.h
  echo "## 实际加载库"; ldd "$BIN" | grep ipc
  echo "## 环境变量清理声明"
  echo "DZIPC_SHM_RECV_COMPAT=${DZIPC_SHM_RECV_COMPAT:-<unset>} DZIPC_SHM_RECV_WORKERS=${DZIPC_SHM_RECV_WORKERS:-<unset>}"
} > "$OUT/fingerprint.txt"
unset DZIPC_SHM_RECV_COMPAT DZIPC_SHM_RECV_WORKERS DZIPC_SHM_RECV_BUDGET_MSGS \
      DZIPC_SHM_RECV_BUDGET_US DZIPC_SHM_RECV_IDLE_MS

FAILS=0
DOM=9500

echo "=== ① gauge（判决性反例：单 gauge ≠ 全池总量） ==="
timeout 600 "$BIN" --case gauge --domain $((DOM++)) --out "$OUT/gauge" --run-id "$RUN_ID" \
    > "$OUT/gauge.log" 2>&1
RC=$?; grep -E "^(workers=|A[0-9])|FAILURE|DONE" "$OUT/gauge.log" | grep -vE "view_queue|SubInfo" | sed 's/^/   /'
echo "   rc=$RC"; [ $RC -eq 0 ] || FAILS=$((FAILS+1))

echo "=== ② conserve（尾部常数标定；B5b 的输入） ==="
timeout 900 "$BIN" --case conserve --domain $((DOM++)) --out "$OUT/conserve" --run-id "$RUN_ID" \
    > "$OUT/conserve.log" 2>&1
RC=$?; grep -E "^conserve:|^(C[0-9])|FAILURE|DONE" "$OUT/conserve.log" | sed 's/^/   /'
echo "   rc=$RC"; [ $RC -eq 0 ] || FAILS=$((FAILS+1))

echo "=== ③ scale：N=1/100/500/1000 × diag=on/off（逐档独立进程） ==="
for n in 1 100 500 1000; do
  for d in on off; do
    d_dir="$OUT/n${n}-diag${d}"
    timeout 900 "$BIN" --case scale --n "$n" --diag "$d" --domain $((DOM++)) --window-ms 6000 \
        --out "$OUT/scale" --run-id "$RUN_ID" > "$OUT/n${n}-diag${d}.log" 2>&1
    RC=$?
    printf "   n=%-5s diag=%-3s rc=%s  " "$n" "$d" "$RC"
    grep -E "^  resident " "$OUT/n${n}-diag${d}.log" | sed 's/^  resident //'
    grep -E "^  conservation " "$OUT/n${n}-diag${d}.log" | sed 's/^/      /'
    grep -cE "^FAILURE" "$OUT/n${n}-diag${d}.log" | sed 's/^/      failures=/'
    [ $RC -eq 0 ] || FAILS=$((FAILS+1))
  done
done

# ---- 汇总（机器判定，不靠人读日志）----
{
  echo
  echo "## 汇总"
  echo "失败档数=$FAILS"
  for f in "$OUT"/gauge.log "$OUT"/conserve.log; do
    printf "%-24s %s\n" "$(basename "$f")" "$(grep -oE 'verdict=[A-Z]+ failures=[0-9]+' "$f" | tail -1)"
  done
  for f in "$OUT"/n*-diag*.log; do
    printf "%-24s %s\n" "$(basename "$f")" "$(grep -oE 'verdict=[A-Z]+ failures=[0-9]+' "$f" | tail -1)"
  done
  echo "W10_O3_DRIVER_RESULT=$([ $FAILS -eq 0 ] && echo PASS || echo FAIL)"
} | tee "$OUT/summary.txt"
exit $([ $FAILS -eq 0 ] && echo 0 || echo 1)
