#!/bin/bash
# W10 失败复现脚本：一条命令复现本包记录的每一个失败/缺陷现象。
# 用法: bash test/perf/w10/w10_repro.sh <outdir>
# ⛔ 证据目录只追加：<outdir> 已存在且非空即拒绝。
set -u
OUT=${1:?usage: w10_repro.sh <outdir>}
cd /home/zwc/cpp_ipc_dds || exit 1
if [ -d "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "REFUSE: $OUT 非空（只追加）"; exit 2; fi
mkdir -p "$OUT"
B=build/bin
echo "## 0) 前置：指纹与加载库" | tee "$OUT/00_fingerprint.txt"
{ git rev-parse HEAD; sha256sum build/lib/libipc.so.1.3.0; ldd $B/w10_matrix | grep ipc; } >> "$OUT/00_fingerprint.txt" 2>&1

echo "## 1) socket 千路端口阻塞（既有缺陷 A）"
timeout 120 $B/w10_matrix --transport socket --topology independent --n 1000 --domain 3103 \
  --msgs 1 --run-id repro-port --out "$OUT/port" > "$OUT/01_socket_1000_hang.log" 2>&1
echo "rc=$? (124=SIGKILL by timeout ⇒ 无上界重试挂住)" | tee -a "$OUT/01_socket_1000_hang.log"
grep -c "Failed to connect subscriber" "$OUT/01_socket_1000_hang.log" | sed 's/^/重连次数=/'

echo "## 2) 端口冲突 A/B 反事实"
$B/w10_portconflict_scope --domain 3103 --n 1000 | tee "$OUT/02_port_scope.txt"
$B/w10_portconflict 48650 224.0.0.87 > "$OUT/02_port_ab.log" 2>&1 || true
cat "$OUT/02_port_ab.log"

echo "## 3) chunk 池耗尽（既有缺陷 D）：策略性回退可识别、无越界"
timeout 120 $B/w10_faults --domain 9001 --only F4 --run-id repro-f4 > "$OUT/03_pool_exhaust.log" 2>&1
grep -E "^(F4|FAILURE|NOTE)" "$OUT/03_pool_exhaust.log"

echo "## 4) 假就绪忙转（既有缺陷 E，W04-F3）：有界退避"
$B/test_lifecycle_contract --gtest_filter=*FruitlessReadiness* > "$OUT/04_fruitless.log" 2>&1
grep -E "W04-L4b|OK \]|FAILED" "$OUT/04_fruitless.log"

echo "## 5) 共享池跨运行残留（既有缺陷 A'）：未发布借样泄漏"
echo "见 tmp/w00_verify/leaker.cpp 与 test/perf/out/20260928_w00_gate3_calib/logs/gate3_calib.txt §8–§10" | tee "$OUT/05_shared_pool_residue.txt"

echo "## 6) 全量 ctest 串行（对照：串行应全绿，说明失败来自共享池状态而非用例本身）"
timeout 1800 ctest --test-dir build > "$OUT/06_ctest_serial.log" 2>&1
echo "rc=$? (0=串行全绿)"; tail -4 "$OUT/06_ctest_serial.log"
echo "## 7) 全量 ctest -j32（既有缺陷 B/C 的现场；⛔ 预期仍失败：RUN_SERIAL 不能消除共享池状态失败）"
timeout 900 ctest --test-dir build -j32 > "$OUT/07_ctest_j32.log" 2>&1
echo "rc=$?"; grep -E "tests passed|tests failed" "$OUT/07_ctest_j32.log" | tail -2
grep -E "^\s+[0-9]+ - " "$OUT/07_ctest_j32.log" | sed 's/^/  失败: /'
echo "REPRO_DONE"
