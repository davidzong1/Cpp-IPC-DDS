#!/bin/bash
# t59 独立负控：临时去掉析构第 2 步 teardown_recv_path() ⇒ 期望 test_shm_sub_dtor_gate worker 臂**变红**；
# 还原后 sha256 逐字回到运行前值且复绿。
# ⛔ trap 保证无论如何都还原（W06 自带脚本 set -e 下 make 失败会把产品源留在被改状态）。
set -u
ROOT=/home/zwc/cpp_ipc_dds; cd "$ROOT"
SRC=src/dzIPC/shm_pub_sub_ipc.cc
RUN="docs/消息接收架构改造/团队改造交付/R1/证据/S4_W06_r59"
LOG="$RUN/r59_negative_control.log"
BAK="$(mktemp /tmp/r59_neg.XXXXXX.bak)"; cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC" | cut -d' ' -f1)"
restore() {
  cp "$BAK" "$SRC"
  local after; after="$(sha256sum "$SRC" | cut -d' ' -f1)"
  echo "restore_sha256=$after" >> "$LOG"
  [ "$after" = "$BEFORE" ] || echo "FATAL: restore mismatch" >> "$LOG"
  make -C build -j8 >/dev/null 2>&1 || echo "WARN: rebuild after restore failed" >> "$LOG"
  rm -f "$BAK"
}
trap restore EXIT

{
  echo "run_id=20260930-r59-R1-W06"; echo "date=$(date -Is)"
  echo "before_sha256=$BEFORE"
  echo "（注意：W06 交付时该值为 12131d8c…；t41 追加计数器点后已变化 —— 见报告 §5）"
  echo
  echo "--- [独立] 未改动前：test_shm_sub_dtor_gate（worker 臂）期望绿 ---"
} > "$LOG"
build/bin/test_shm_sub_dtor_gate 2>&1 | grep -E "Failure|arm=|PASSED  \]|FAILED  \]|OK \]" >> "$LOG"

python3 - "$SRC" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
assert s.count('    teardown_recv_path();')>=1, "anchor missing"
s2=s.replace('    teardown_recv_path();', '    /* t59 NEGATIVE CONTROL (temporary) */', 1)
assert s2!=s
open(p,'w',encoding='utf-8').write(s2)
PY
make -C build -j8 >/dev/null 2>&1
{
  echo
  echo "--- [独立] 去掉 teardown_recv_path() 后：期望**变红** ---"
  echo "negative_sha256=$(sha256sum $SRC | cut -d' ' -f1)"
} >> "$LOG"
build/bin/test_shm_sub_dtor_gate 2>&1 | grep -E "Failure|arm=|PASSED  \]|FAILED  \]|OK \]" >> "$LOG"
