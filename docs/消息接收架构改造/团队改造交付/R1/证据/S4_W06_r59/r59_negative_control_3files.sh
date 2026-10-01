#!/bin/bash
# t59 独立负控（全部三个按臂改判的用例）：去掉析构第 2 步 teardown_recv_path()
# ⇒ **逐文件**观察 worker 臂是否变红。W06 只给了 test_shm_sub_dtor_gate 一个负控，
# 本脚本把三个都覆盖，以判断另两个的 worker 臂判据是否有牙。
set -u
ROOT=/home/zwc/cpp_ipc_dds; cd "$ROOT"
SRC=src/dzIPC/shm_pub_sub_ipc.cc
RUN="docs/消息接收架构改造/团队改造交付/R1/证据/S4_W06_r59"
LOG="$RUN/r59_negative_control_3files.log"
BAK="$(mktemp /tmp/r59_neg3.XXXXXX.bak)"; cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC" | cut -d' ' -f1)"
restore() {
  cp "$BAK" "$SRC"
  local a; a="$(sha256sum "$SRC" | cut -d' ' -f1)"
  echo "restore_sha256=$a" >> "$LOG"
  [ "$a" = "$BEFORE" ] || echo "FATAL restore mismatch" >> "$LOG"
  make -C build -j8 >/dev/null 2>&1 || echo "WARN rebuild" >> "$LOG"
  rm -f "$BAK"
}
trap restore EXIT
const_run() { for t in test_wakeup_artifact test_shm_ready_transition test_shm_sub_dtor_gate; do
    printf "%-28s " "$t"
    timeout 300 build/bin/$t 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]" | tr '\n' ' '
    echo
  done; }

{ echo "run_id=20260930-r59-R1-W06"; echo "date=$(date -Is)"; echo "before_sha256=$BEFORE"
  echo; echo "=== [独立] 未改动前（对照组）==="; } > "$LOG"
const_run >> "$LOG"
python3 - "$SRC" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
s2=s.replace('    teardown_recv_path();', '    /* t59 NEGATIVE CONTROL (temporary, 3-files) */', 1)
assert s2!=s
open(p,'w',encoding='utf-8').write(s2)
PY
make -C build -j8 >/dev/null 2>&1
{ echo; echo "=== [独立] 去掉 teardown_recv_path() 后（负控）==="; } >> "$LOG"
const_run >> "$LOG"
