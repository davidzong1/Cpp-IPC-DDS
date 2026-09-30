#!/bin/bash
# W06-F1 反事实证明：把 recv_worker.cc 的「先 drain 再决定让出」临时还原成
# 「先等 wait_timeout 再 drain」，同一探针的交付延迟必须回到 100 ms 地板。
#
# ⛔ 本脚本会**临时修改并还原** src/dzIPC/threepools/recv_worker.cc；运行前后各存 sha256，
#    还原后必须与运行前逐字一致（脚本最后会断言）。
# 用法：bash artifacts/perf/20260929-r24-W06/src/w06_f1_counterfactual.sh
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
SRC=src/dzIPC/threepools/recv_worker.cc
OUT=artifacts/perf/20260929-r24-W06/logs
SRCDIR=artifacts/perf/20260929-r24-W06/src
mkdir -p "$OUT"

BAK="$OUT/recv_worker.cc.w06f1.bak"
cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC" | cut -d' ' -f1)"
echo "before_sha256=$BEFORE" | tee "$OUT/f1_counterfactual.log"

# 反事实 = 旧顺序（先阻塞一个等待切片），但**保留 drain_deferred()**：
# 旧实现的语义是"先 wait 再 drain"，不是"只 wait 不 drain"。
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
i = s.index('        const std::uint64_t msgs_before =')
j = s.index('        idle_since = Clock::now();   // ', i)
new = ('        /* back-to-old-order (counterfactual): wait first, then drain */\n'
       '        wait_once(budget.wait_timeout);\n'
       '        drain_deferred();\n')
open(p, 'w', encoding='utf-8').write(s[:i] + new + s[j:])
PY

echo "--- counterfactual (old order restored) ---" | tee -a "$OUT/f1_counterfactual.log"
make -C build -j8 >/dev/null 2>&1
g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I . -I 3rdparty "$SRCDIR/w06_regress.cpp" \
    -o "$SRCDIR/w06_regress" -L build/lib -lipc -lpthread -lrt -Wl,-rpath,"$ROOT/build/lib"
timeout 300 "$SRCDIR/w06_regress" --case=lat --domain 395 --rounds=10 2>&1 \
    | grep -E "case=lat|verdict" | tee -a "$OUT/f1_counterfactual.log" || true

cp "$BAK" "$SRC"
echo "--- restored W06-F1 ---" | tee -a "$OUT/f1_counterfactual.log"
make -C build -j8 >/dev/null 2>&1
AFTER="$(sha256sum "$SRC" | cut -d' ' -f1)"
echo "after_sha256=$AFTER" | tee -a "$OUT/f1_counterfactual.log"
if [ "$BEFORE" != "$AFTER" ]; then
    echo "FATAL: restore mismatch" | tee -a "$OUT/f1_counterfactual.log"
    exit 1
fi
timeout 300 "$SRCDIR/w06_regress" --case=lat --domain 396 --rounds=20 2>&1 \
    | grep -E "case=lat|verdict" | tee -a "$OUT/f1_counterfactual.log"
echo F1_COUNTERFACTUAL_DONE
