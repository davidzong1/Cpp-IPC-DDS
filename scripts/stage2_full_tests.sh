#!/usr/bin/env bash
# 阶段 2 全量测试清单执行器 —— 落点 docs/消息接收架构改造/阶段2_全量测试方案.md §6。
#
# 为什么要有这个脚本(而不是照抄方案 §6 的那段 bash):
#   ⛔ 方案 §6 / §6.1 原文给的是 `"build/bin/$t" --gtest_brief=1`。本仓 gtest 是
#      **1.10.0**(CMakeLists.txt:131 的 GOOGLETEST_VERSION), 而 `--gtest_brief` 是
#      1.11 才加的旗标 —— 实测直接 usage error + 非零退出, 照抄会让**每个目标都判失败**。
#      本脚本不用它, 改为逐目标统计 `[       OK ]` / `[  FAILED  ]` / GTEST_SKIP。
#   其余纪律照旧: 先 unset LD_LIBRARY_PATH(否则会加载到变异实验残留的库, 见 v4 §1 的
#   污染陷阱), 逐目标 timeout, 失败即非零退出并汇总。
#
# 用法:
#   scripts/stage2_full_tests.sh              # 全量清单 + 聚焦 CTest
#   scripts/stage2_full_tests.sh --no-ctest   # 只跑手工清单
#   scripts/stage2_full_tests.sh --only focused
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-/tmp/stage2_full}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-120}"

# 与方案 §6 的三组一致。顺序即执行顺序: 先确定性协议/伪影, 再 SHM 集成, 最后退出/探针。
focused=(
  test_shm_route_session
  test_wakeup_artifact
  test_shm_control_scheduler
  # 阶段 2 §4 的三个补强套件(方案 §6 写于它们落地之前, 这里补进 focused 组):
  test_shm_i5_pop_buffer
  test_shm_sub_dtor_gate
  test_shm_ready_transition
)
integration=(
  test_dzipc_shm
  test_shm_nodelet
  test_shm_receiver_cap
  test_dzflat_rx
  test_wire_accept
  test_shm_domain_isolation
  test_shm_sniffer_control_name
  test_chunk_hold
  test_lap_safety
)
lifecycle=(
  test_uf003_crash_reclaim
  test_uf004_shutdown_monitor_optout
  test_uf009_graceful_exit
  test_handshake_probe
)

RUN_CTEST=1
ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-ctest) RUN_CTEST=0 ;;
    --only) shift; ONLY="${1:-}" ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$OUT_DIR"

# ---- 前置: 钉库。方案 §5.2 —— 全绿必须对应本次构建的二进制和库。 ----
unset LD_LIBRARY_PATH
echo "===== 环境 ====="
echo "build dir : $BUILD_DIR"
echo "logs      : $OUT_DIR"
echo "LD_LIBRARY_PATH: [${LD_LIBRARY_PATH:-<unset>}]"

probe="$BUILD_DIR/bin/test_shm_route_session"
if [ -x "$probe" ]; then
  libpath="$(ldd "$probe" 2>/dev/null | awk '/libipc\.so/ {print $3; exit}')"
  echo "libipc 实际加载: ${libpath:-<未解析到>}"
  case "$libpath" in
    "$BUILD_DIR"/lib/*) ;;
    "") echo "  ⛔ 未解析到 libipc —— 先构建" ;;
    *) echo "  ⛔ 加载的不是本次构建树的库, 结果不可信" ;;
  esac
else
  echo "⛔ 缺少 $probe —— 先 cmake -S . -B build && cmake --build build"
fi

fail=0
declare -a summary=()

run_one() {
  local t="$1"
  local bin="$BUILD_DIR/bin/$t"
  local log="$OUT_DIR/$t.txt"
  if [ ! -x "$bin" ]; then
    echo "===== $t ====="
    echo "  ⛔ 缺少可执行文件 $bin（先核对源码是否存在、cmake 是否重新配置）"
    summary+=("$t: MISSING")
    fail=1
    return
  fi
  echo "===== $t ====="
  timeout "${PER_TEST_TIMEOUT}s" "$bin" >"$log" 2>&1
  local rc=$?
  local ok bad skip
  ok=$(grep -cE '^\[       OK \]' "$log" || true)
  bad=$(grep -cE '^\[  FAILED  \]' "$log" || true)
  skip=$(grep -cE '^\[  SKIPPED \]' "$log" || true)
  echo "  rc=$rc  OK=$ok  FAILED=$bad  SKIPPED=$skip"
  if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
    echo "  ---- 失败详情(末 40 行) ----"
    tail -n 40 "$log" | sed 's/^/  /'
    summary+=("$t: FAIL(rc=$rc bad=$bad)")
    fail=1
  elif [ "$rc" -eq 124 ]; then
    summary+=("$t: TIMEOUT")
    fail=1
  else
    summary+=("$t: PASS($ok ok, $skip skipped)")
  fi
}

if [ -z "$ONLY" ] || [ "$ONLY" = "focused" ]; then
  for t in "${focused[@]}"; do run_one "$t"; done
fi
if [ -z "$ONLY" ] || [ "$ONLY" = "integration" ]; then
  for t in "${integration[@]}"; do run_one "$t"; done
fi
if [ -z "$ONLY" ] || [ "$ONLY" = "lifecycle" ]; then
  for t in "${lifecycle[@]}"; do run_one "$t"; done
fi

if [ "$RUN_CTEST" -eq 1 ] && [ -z "$ONLY" ]; then
  echo "===== 聚焦 CTest ====="
  ctest --test-dir "$BUILD_DIR" \
    -R 'test_(udp_port_boundary|shm_control_scheduler|shm_route_session|wakeup_artifact|shm_i5_pop_buffer|shm_sub_dtor_gate|shm_ready_transition)$' \
    --output-on-failure 2>&1 | tee "$OUT_DIR/ctest.txt" | tail -n 15
  ctest_rc=${PIPESTATUS[0]}
  if [ "$ctest_rc" -ne 0 ]; then
    summary+=("ctest: FAIL(rc=$ctest_rc)")
    fail=1
  else
    summary+=("ctest: PASS")
  fi
fi

echo
echo "===== 汇总 ====="
for s in "${summary[@]}"; do echo "  $s"; done
if [ "$fail" -ne 0 ]; then
  echo "结果: 失败（逐项见上；日志在 $OUT_DIR）"
  exit 1
fi
echo "结果: 全部通过（日志在 $OUT_DIR）"
