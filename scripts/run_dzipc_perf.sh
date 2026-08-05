#!/usr/bin/env bash
#
# dzIPC 通信性能测试一键脚本
#
#   构建 (Release) -> 运行 4 种通信组合的性能测试 -> 绘制性能曲线并生成报告
#
# 用法:
#   bash scripts/run_dzipc_perf.sh [传给 dzipc_perf_benchmark 的额外参数...]
#
# 例:
#   bash scripts/run_dzipc_perf.sh                       # 默认全量测试
#   bash scripts/run_dzipc_perf.sh --duration=5          # 每用例测 5 秒
#   bash scripts/run_dzipc_perf.sh --cases=pubsub_shm    # 只测 SHM pub-sub
#   bash scripts/run_dzipc_perf.sh --payloads=64,1024,65536
#
# 环境变量:
#   BUILD_DIR   构建目录 (默认 build_perf)
#   JOBS        并行编译任务数 (默认 nproc)

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/build_perf}"
JOBS="${JOBS:-$(nproc)}"

cd "${REPO_DIR}"

echo "==> Configuring Release build (${BUILD_DIR})"
cmake -S "${REPO_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLIBIPC_BUILD_PYTHON=OFF \
      >/dev/null

echo "==> Building dzipc_perf_benchmark (-j${JOBS})"
cmake --build "${BUILD_DIR}" --target dzipc_perf_benchmark -j"${JOBS}"

BIN="${BUILD_DIR}/bin/dzipc_perf_benchmark"
if [[ ! -x "${BIN}" ]]; then
    echo "error: executable not found: ${BIN}" >&2
    exit 1
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${REPO_DIR}/perf_results/${STAMP}"

# 让动态库可被找到 (libipc 默认构建为共享库)
export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${LD_LIBRARY_PATH:-}"

echo "==> Removing stale shared memory segments"
rm -f /dev/shm/*perfbench* /dev/shm/*perfsrv* 2>/dev/null || true

echo "==> Running benchmark, output directory: ${OUT_DIR}"
set +e
"${BIN}" --out="${OUT_DIR}" "$@"
BENCH_RC=$?
set -e
if [[ ${BENCH_RC} -ne 0 ]]; then
    echo "warning: some cases failed (exit code ${BENCH_RC}); plotting with whatever data exists" >&2
fi

echo "==> Plotting performance curves and generating report"
python3 "${REPO_DIR}/scripts/plot_dzipc_perf.py" "${OUT_DIR}"

echo
echo "======================================================================"
echo " Done. Results: ${OUT_DIR}"
echo "   report.md      report (hardware config + charts + all data)"
echo "   results.csv    raw metrics"
echo "   results.json   raw metrics + hardware config"
echo "   hardware.txt   hardware and system configuration"
echo "   charts/        performance curves"
echo "   samples/       latency samples (CDF data)"
echo "======================================================================"
