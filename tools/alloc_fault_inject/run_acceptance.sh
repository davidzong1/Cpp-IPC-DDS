#!/usr/bin/env bash
# UF-000 验收执行器 —— 一键重放 0.3.1 要求的三条(构建/运行命令、阳性对照、拦截计数)。
#
# 用法:  tools/alloc_fault_inject/run_acceptance.sh
# 日志:  末尾把全过程写到 $LOG(默认 /tmp/uf000_acceptance.log)
#
# ⛔ 阳性对照**预期**以 SIGSEGV 结束, 所以全程**不用** set -e。
#    读结果时看每条 ### rc=:  rc=139 才是阳性对照通过。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
LOG="${LOG:-/tmp/uf000_acceptance.log}"
SO="${HERE}/bin/liballoc_fault_inject.so"

cd "${ROOT}"
: > "${LOG}"

step() {
    echo ""                        | tee -a "${LOG}"
    echo "### CMD: $*"             | tee -a "${LOG}"
    "$@" >>"${LOG}" 2>&1
    echo "### rc=$?"               | tee -a "${LOG}"
    return 0
}

banner() { echo "" | tee -a "${LOG}"; echo "===== $* =====" | tee -a "${LOG}"; }

banner "0. 基线"
step git rev-parse HEAD

banner "1. 构建工装 (0.3.1 第1条 之①)"
step bash "${HERE}/build.sh"

banner "2. 工装自测 (期望 rc=0)"
step env LD_PRELOAD="${SO}" "${HERE}/bin/fi_selftest"

banner "2b. 阴性对照: 不带 LD_PRELOAD 应报 BLOCKED (期望 rc=2)"
step "${HERE}/bin/fi_selftest"

banner "3. 阳性对照 dtor, 自标定 (期望 rc=139 SIGSEGV)"
step env LD_PRELOAD="${SO}" "${HERE}/bin/fi_positive_control"

banner "3b. 阳性对照 valid (期望 rc=139)"
step env LD_PRELOAD="${SO}" "${HERE}/bin/fi_positive_control" 0 0 0 valid

banner "3c. 阳性对照 显式档 [56,57) skip_n=0 (期望 rc=139)"
step env LD_PRELOAD="${SO}" "${HERE}/bin/fi_positive_control" 56 57 0 dtor

banner "3d. 档内尺寸直方图 (自证 56 = sizeof(handle_))"
step env LD_PRELOAD="${SO}" DZIPC_FI_ALLOC_SIZE_LO=8 DZIPC_FI_ALLOC_SIZE_HI=4096 \
     DZIPC_FI_ALLOC_MAX_FAILS=0 DZIPC_FI_HISTOGRAM=1 \
     "${HERE}/bin/fi_positive_control" 8 4096 0 dtor

banner "4. cmake 重配(GLOB 收进新用例) + 构建目标"
step cmake -S . -B build
step cmake --build build -j8 --target test_alloc_fault_inject

banner "5. 注入态跑判据用例 (期望 rc=139 —— UF-001/UF-002 的实测红)"
step env LD_PRELOAD="${SO}" ./build/bin/test_alloc_fault_inject

banner "5b. 不带 LD_PRELOAD: 应全部 GTEST_SKIP, 不是绿"
step ./build/bin/test_alloc_fault_inject

banner "5c. 注入态只跑健全性用例 (期望通过)"
step env LD_PRELOAD="${SO}" ./build/bin/test_alloc_fault_inject \
     --gtest_filter=AllocFaultInject.DisabledHarnessDoesNotIntervene

banner "6. git diff --check"
step git diff --check

banner "DONE; log = ${LOG}"
