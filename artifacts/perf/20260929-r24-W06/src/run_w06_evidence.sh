#!/bin/bash
# W06 证据采集（可复跑；每次运行新建 run 目录，⛔ 不覆盖既有 run）。
# 用法：bash artifacts/perf/20260929-r24-W06/src/run_w06_evidence.sh [RUN_ID]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
RUN_ID="${1:-20260929-r24-W06}"
SRC="$ROOT/artifacts/perf/$RUN_ID/src"
OUT="$ROOT/artifacts/perf/$RUN_ID"
LOG="$OUT/logs"
mkdir -p "$LOG"

# 编译探针（产物不进仓库源码；探针本身是证据，不是交付源码）
g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I . -I 3rdparty "$SRC/w06_scale.cpp" -o "$SRC/w06_scale" \
    -L build/lib -lipc -lpthread -lrt -Wl,-rpath,"$ROOT/build/lib" || exit 1
g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I . -I 3rdparty "$SRC/w06_regress.cpp" -o "$SRC/w06_regress" \
    -L build/lib -lipc -lpthread -lrt -Wl,-rpath,"$ROOT/build/lib" || exit 1

FILTER='idle exit|SubInfo|view_queue'

echo "== A. 1000 独立话题有效收发（worker 臂 / 回退臂对照）"
timeout 900 "$SRC/w06_scale" --n 1000 --domain 220 2>&1 | grep -vE "$FILTER" | tee "$LOG/scale_1000_worker.log"
DZIPC_SHM_RECV_COMPAT=1 timeout 900 "$SRC/w06_scale" --n 1000 --domain 221 2>&1 | grep -vE "$FILTER" \
    | sed 's/reasons=1,1,.*/reasons=[1000x 1(kForcedCompatEnv)]/' | tee "$LOG/scale_1000_compat.log"

echo "== B. 竞态/预算/队头阻塞/回退可分"
DZIPC_SHM_RECV_BUDGET_MSGS=1 timeout 200 "$SRC/w06_regress" --case=budget --domain 382 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_budget.log"
timeout 300 "$SRC/w06_regress" --case=hol    --domain 383 --hot-ms=400 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_hol.log"
timeout 300 "$SRC/w06_regress" --case=dyn    --domain 384 --churn=20 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_dyn.log"
timeout 200 "$SRC/w06_regress" --case=idle   --domain 385 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_idle.log"
timeout 300 "$SRC/w06_regress" --case=fallback --domain 386 --n 60 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_fallback_worker.log"
DZIPC_SHM_RECV_COMPAT=1 timeout 300 "$SRC/w06_regress" --case=fallback --domain 387 --n 60 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_fallback_compat.log"
echo "  -- 交付延迟（worker 臂地板 = W06-F1 的直接证据）"
timeout 300 "$SRC/w06_regress" --case=lat --domain 388 --rounds=40 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_lat_worker.log"
DZIPC_SHM_RECV_COMPAT=1 timeout 300 "$SRC/w06_regress" --case=lat --domain 389 --rounds=40 2>&1 | grep -vE "$FILTER" | tee "$LOG/regress_lat_compat.log"
echo "  -- 容量满 ⇒ 显式回退（三计数可分）"
DZIPC_SHM_RECV_WORKERS=1 timeout 400 "$SRC/w06_regress" --case=wsetfull --domain 390 --n 130 2>&1 \
    | grep -vE "$FILTER" | tee "$LOG/regress_wsetfull.log"

echo "== C. 既有回归两条臂"
for t in test_recv_worker test_wakeup_artifact test_shm_ready_transition test_shm_sub_dtor_gate test_shm_i5_pop_buffer; do
  { echo "### $t (worker arm)";   timeout 300 "build/bin/$t" 2>/dev/null | grep -E "^\[  PASSED|^\[  FAILED|^\[  RUN|arm=" ; } | tee "$LOG/$t.worker.log"
  { echo "### $t (compat arm)"; DZIPC_SHM_RECV_COMPAT=1 timeout 300 "build/bin/$t" 2>/dev/null | grep -E "^\[  PASSED|^\[  FAILED|arm=" ; } | tee "$LOG/$t.compat.log"
done

echo "== D. 全量 CTest 两条臂"
( cd build && timeout 1800 ctest -j4 2>&1 | tail -6 ) | tee "$LOG/ctest_worker.log"
( cd build && DZIPC_SHM_RECV_COMPAT=1 timeout 1800 ctest -j4 2>&1 | tail -6 ) | tee "$LOG/ctest_compat.log"

echo "== E. 构建与树状态"
( cd build && make -j8 2>&1 | tail -2 ) | tee "$LOG/build.log"
{
  echo "--- git diff --stat -- src include test ---"; git diff --stat -- src include test | cat
  echo "--- recv_wait_set 干净性（共享层文件，W06 未改）---"
  git diff --stat -- src/libipc/recv_wait_set.cpp | cat
  echo "(以上若为空 = 未被改动)"
  echo "--- 调试残留自查（应为 0）---"
  grep -c "DBG\|fprintf(stderr, \"\[WS" src/dzIPC/threepools/recv_worker.cc src/libipc/recv_wait_set.cpp || true
} | tee "$LOG/tree_state.txt"
echo "== F. 反例（判据有牙）：临时去掉析构第 2 步 teardown_recv_path()"
cp src/dzIPC/shm_pub_sub_ipc.cc "$LOG/shm_pub_sub_ipc.cc.neg.bak"
python3 - src/dzIPC/shm_pub_sub_ipc.cc <<'PY2'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
open(p, 'w', encoding='utf-8').write(s.replace('    teardown_recv_path();', '    /* NEGATIVE CONTROL (temporary) */', 1))
PY2
make -C build -j8 >/dev/null 2>&1
{
  echo "=== W06 negative control: 析构第 2 步 teardown_recv_path() 被去掉（临时；验证按臂判据是否有牙）==="
  echo "--- test_shm_sub_dtor_gate (worker arm) : 期望变红 ---"
  timeout 300 build/bin/test_shm_sub_dtor_gate 2>&1 | grep -E "Failure$|arm=|PASSED  \]|FAILED  \]"
} > "$LOG/negative_control_teardown.log" 2>&1
cp "$LOG/shm_pub_sub_ipc.cc.neg.bak" src/dzIPC/shm_pub_sub_ipc.cc
make -C build -j8 >/dev/null 2>&1
{
  echo "--- 还原后（sha256 必须与改动前一致，且判据复绿）---"
  sha256sum src/dzIPC/shm_pub_sub_ipc.cc
  echo "--- test_shm_sub_dtor_gate (worker arm) ---"
  build/bin/test_shm_sub_dtor_gate 2>&1 | grep -E "Failure$|arm=|PASSED  \]|FAILED  \]"
  echo "--- test_shm_ready_transition (worker arm) ---"
  build/bin/test_shm_ready_transition 2>&1 | grep -E "Failure$|arm=|PASSED  \]|FAILED  \]"
} >> "$LOG/negative_control_teardown.log" 2>&1
cat "$LOG/negative_control_teardown.log"

echo "W06_EVIDENCE_DONE"
