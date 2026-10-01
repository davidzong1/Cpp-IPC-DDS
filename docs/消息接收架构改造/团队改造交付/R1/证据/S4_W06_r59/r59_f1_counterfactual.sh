#!/bin/bash
# t59（R1 独立复核）W06-F1 反事实**独立复跑**。
# 与 W06 自带脚本的差异：本脚本用 EXIT trap 保证"无论如何都还原 + 校验 sha256"，
# 避免 W06 脚本 `set -e` 下 make 失败会把产品源留在被改状态。
# ⛔ 只临时改 recv_worker.cc 的 loop 顺序一处；还原后 sha256 必须逐字回到运行前值。
set -u
ROOT=/home/zwc/cpp_ipc_dds
cd "$ROOT"
SRC=src/dzIPC/threepools/recv_worker.cc
RUN="docs/消息接收架构改造/团队改造交付/R1/证据/S4_W06_r59"
PROBE="$RUN/w06_regress_r59"
LOG="$RUN/r59_f1_counterfactual.log"
BAK="$(mktemp /tmp/r59_recv_worker.XXXXXX.cc)"
cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC" | cut -d' ' -f1)"

restore() {
  cp "$BAK" "$SRC"
  local after; after="$(sha256sum "$SRC" | cut -d' ' -f1)"
  echo "restore_sha256=$after" >> "$LOG"
  if [ "$after" != "$BEFORE" ]; then echo "FATAL: restore mismatch ($BEFORE -> $after)" >> "$LOG"; RETURN_BAD=1; fi
  make -C build -j8 >/dev/null 2>&1 || echo "WARN: restore rebuild failed" >> "$LOG"
  rm -f "$BAK"
}
trap restore EXIT

{
  echo "run_id=20260930-r59-R1-W06"
  echo "date=$(date -Is)"
  echo "before_sha256=$BEFORE"
  echo "库指纹(反事实前)=$(sha256sum build/lib/libipc.so.1.3.0 | cut -d' ' -f1)"
} > "$LOG"

echo "--- [独立] 修复后（当前树，先 drain 再按需 wait）---" >> "$LOG"
"$PROBE" --case=lat --domain 2591 --rounds=20 2>&1 | grep -E "case=lat|verdict" >> "$LOG"

python3 - "$SRC" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
i=s.index('        const std::uint64_t msgs_before =')
j=s.index('        idle_since = Clock::now();   // ', i)
new=('        /* t59 counterfactual: back to old order (wait first, then drain) */\n'
     '        wait_once(budget.wait_timeout);\n'
     '        drain_deferred();\n')
open(p,'w',encoding='utf-8').write(s[:i]+new+s[j:])
PY
echo "--- [独立] 反事实（旧顺序：先 wait 再 drain）---" >> "$LOG"
make -C build -j8 >> "$LOG" 2>&1 || echo "WARN: counterfactual rebuild failed" >> "$LOG"
"$PROBE" --case=lat --domain 2592 --rounds=10 2>&1 | grep -E "case=lat|verdict" >> "$LOG"
