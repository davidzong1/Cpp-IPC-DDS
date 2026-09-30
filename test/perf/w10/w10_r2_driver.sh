#!/bin/bash
# R2/t42 —— 方案 §6.2 最小验证矩阵「缺失 4 项」的驱动脚本（逐项**独立进程**）。
#
# 为什么必须逐项独立进程：池的 worker 数/预算是「首个成功 start() 的调用方」一次性决定的
# （契约 §8.1），四项需要不同预算（无 route 要长 idle；预算耗尽要 max_messages_per_route=1；
# 断开/注销要 wait_timeout=5000ms 才能把"被唤醒"与"等满超时"分开）。方案 §6.3 亦要求
# 「使用新的独立进程，避免池首次初始化配置影响后续实验」。
#
# 用法: bash test/perf/w10/w10_r2_driver.sh <out_dir>
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"; cd "$ROOT"
OUT=${1:?usage: w10_r2_driver.sh <out_dir>}
if [ -d "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "REFUSE: $OUT 非空（只追加）"; exit 2; fi
mkdir -p "$OUT"

BIN=build/bin/w10_r2_minimal
FORCE=build/bin/libforce_backend.so
DOM=8810

# ---- 自包含构建（与 W10 其余工装同一编译方式；⛔ 不进 CMake，理由同 W10 现状）----
CXX_INC="-I include -I src -I . -I 3rdparty"
CXX_LNK="-L build/lib -lipc -lpthread -lrt -Wl,-rpath,$ROOT/build/lib"
mkdir -p build/bin
echo "[build] w10_r2_minimal"; g++ -std=c++17 -O2 -DNDEBUG $CXX_INC test/perf/w10/w10_r2_minimal.cpp -o "$BIN" $CXX_LNK || exit 4
echo "[build] w10_r2_interval"; g++ -std=c++17 -O2 -DNDEBUG $CXX_INC test/perf/w10/w10_r2_interval.cpp -o build/bin/w10_r2_interval $CXX_LNK || exit 4
echo "[build] libforce_backend.so"; g++ -std=c++17 -O2 -fPIC -shared -I include test/perf/w10/force_backend_unavailable.cpp -o "$FORCE" || exit 4

{
  echo "# R2/t42 §6.2 最小验证矩阵（缺失 4 项）"
  echo "# 时间: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "## git"; git rev-parse HEAD
  echo "## 二进制与库指纹"; sha256sum "$BIN" "$FORCE" build/lib/libipc.so.1.3.0 src/dzIPC/threepools/recv_worker.cc
  echo "## 实际加载库"; ldd "$BIN" | grep ipc
  echo "## 环境变量清理"; echo "DZIPC_SHM_RECV_COMPAT=${DZIPC_SHM_RECV_COMPAT:-<unset>}"
} > "$OUT/fingerprint.txt"
# ⛔ 必须清掉会改变路径决策的继承变量，否则"后端错误"臂会被"强制兼容"掩盖
unset DZIPC_SHM_RECV_COMPAT DZIPC_SHM_RECV_WORKERS DZIPC_SHM_RECV_BUDGET_MSGS DZIPC_SHM_RECV_BUDGET_US

FAILS=0
run_case()
{
  local c="$1" name="$2" dom="$3"
  shift 3
  echo "=== case $c：$name (domain=$dom) ==="
  timeout 600 "$BIN" --case "$c" --domain "$dom" --out "$OUT/case$c" --run-id "r42-case$c" "$@" \
    > "$OUT/case$c.log" 2>&1
  local rc=$?
  grep -E "PASS|FAIL|DONE verdict|FAILURE" "$OUT/case$c.log" | grep -vE "view_queue|SubInfo" | sed 's/^/   /'
  echo "   rc=$rc"
  [ $rc -eq 0 ] || FAILS=$((FAILS+1))
}

run_case 1 "无 route"                $((DOM+1))
run_case 2 "预算耗尽重入 deferred"    $((DOM+2))
run_case 3 "断开/注销"               $((DOM+3))
run_case 4 "后端错误（对照臂）"        $((DOM+4)) --arm control
# 注入臂：同一二进制 + LD_PRELOAD，差异只来自注入
echo "=== case 4：后端错误（注入臂 LD_PRELOAD=libforce_backend.so） ==="
LD_PRELOAD="$PWD/$FORCE" timeout 600 "$BIN" --case 4 --domain $((DOM+5)) --arm inject \
  --out "$OUT/case4-inject" --run-id "r42-case4-inject" > "$OUT/case4-inject.log" 2>&1
RC=$?
grep -E "PASS|FAIL|DONE verdict|FAILURE" "$OUT/case4-inject.log" | grep -vE "view_queue|SubInfo" | sed 's/^/   /'
echo "   rc=$RC"
[ $RC -eq 0 ] || FAILS=$((FAILS+1))

# 缺口 3：scan_time_ns_total 的观测区间（把区间拆成可分离的两段）
echo "=== 缺口 3：scan_time_ns_total 观测区间拆分 ==="
timeout 300 build/bin/w10_r2_interval > "$OUT/scan_time_interval.log" 2>&1
tail -7 "$OUT/scan_time_interval.log" | sed 's/^/   /'
[ $(( $(grep -c "PROBE_INTERVAL_DONE" "$OUT/scan_time_interval.log") )) -eq 1 ] || FAILS=$((FAILS+1))

{
  echo
  echo "## 汇总"
  echo "失败项数=$FAILS"
  for f in "$OUT"/case*.log; do
    printf "%-28s %s\n" "$(basename "$f")" "$(grep -oE 'verdict=[A-Z]+ failures=[0-9]+' "$f" | tail -1)"
  done
  echo "R2_MINIMAL_DRIVER_RESULT=$([ $FAILS -eq 0 ] && echo PASS || echo FAIL)"
} | tee "$OUT/summary.txt"
exit $([ $FAILS -eq 0 ] && echo 0 || echo 1)
