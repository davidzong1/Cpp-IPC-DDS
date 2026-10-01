#!/bin/bash
set -u
ROOT=/home/zwc/cpp_ipc_dds; cd "$ROOT"
SRC=src/dzIPC/shm_pub_sub_ipc.cc
RUN="docs/消息接收架构改造/团队改造交付/R1/证据/S4_W06_r59"
LOG="$RUN/r59_neg_ready_only.log"
BAK="$(mktemp /tmp/r59_nr.XXXXXX.bak)"; cp "$SRC" "$BAK"
BEFORE="$(sha256sum "$SRC"|cut -d' ' -f1)"
restore(){ cp "$BAK" "$SRC"; echo "restore_sha256=$(sha256sum $SRC|cut -d' ' -f1)" >> "$LOG"; make -C build -j8 >/dev/null 2>&1; rm -f "$BAK"; }
trap restore EXIT
{ echo "before_sha256=$BEFORE"; } > "$LOG"
python3 - "$SRC" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
open(p,'w',encoding='utf-8').write(s.replace('    teardown_recv_path();','    /* t59 NC */',1))
PY
make -C build -j8 >/dev/null 2>&1
{ echo "=== 负控下 test_shm_ready_transition（worker 臂）完整输出 ==="; } >> "$LOG"
set +e
timeout 180 build/bin/test_shm_ready_transition >> "$LOG" 2>&1
echo "exit_code=$?" >> "$LOG"
