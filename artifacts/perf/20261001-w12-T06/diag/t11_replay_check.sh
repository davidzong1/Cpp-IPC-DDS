#!/usr/bin/env bash
# T11 —— 校验 W09 交付文档 §12.8 的复现块是否**逐字可跑**（同一二进制、只换库）
set -u
R=artifacts/perf/20261001-w12-T06
run() { LD_LIBRARY_PATH="$1" build/bin/test_chunk_capacity_backpressure \
        --gtest_filter='*OrphanReset*' --gtest_color=no > "$2" 2>&1; echo "rc=$?"; }
echo "[① 清干净]"; rm -f /dev/shm/w09c9alias__IPC_SHM__*
echo "[② 制造污染 V2NEG] $(run $R/libs/V2NEG $R/diag/t11_step2_v2neg.log)  $(grep -oE 'bad_rounds=[0-9]+ skipped=[0-9]+' $R/diag/t11_step2_v2neg.log)"
echo "[③ 假红 BASELINE]  $(run $R/libs/BASELINE $R/diag/t11_step3_baseline_polluted.log)  $(grep -oE 'bad_rounds=[0-9]+ skipped=[0-9]+' $R/diag/t11_step3_baseline_polluted.log)"
echo "[④ 清段]"; rm -f /dev/shm/w09c9alias__IPC_SHM__*
echo "[⑤ 同库全绿 BASELINE] $(run $R/libs/BASELINE $R/diag/t11_step5_baseline_clean.log)  $(grep -oE 'bad_rounds=[0-9]+ skipped=[0-9]+' $R/diag/t11_step5_baseline_clean.log)"
