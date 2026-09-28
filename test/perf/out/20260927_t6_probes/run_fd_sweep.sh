#!/bin/bash
# t6 fd-boundary sweep with the ABI-valid probe
set -u
cd /home/zwc/cpp_ipc_dds
D=.t6_probe/logs/fd; mkdir -p $D
P=./.t6_probe/fdprobe
run() { # tag, mode(worker|compat), scale, domain, extra
  tag=$1; mode=$2; scale=$3; dom=$4; shift 4
  if [ "$mode" = compat ]; then export DZIPC_SOCKET_COMPAT_THREAD=1; else unset DZIPC_SOCKET_COMPAT_THREAD; fi
  unset DZIPC_SOCKET_RECV_WORKERS
  timeout 300 $P --scale=$scale --domain=$dom --idle-ms=600 "$@" > $D/$tag.log 2>&1
  rc=$?
  maxfd=$(grep -o 'all created:.*' $D/$tag.log | head -1)
  verdict=$(grep -cE 'buffer overflow detected|SURVIVED' $D/$tag.log)
  printf '%-26s rc=%-4s %s\n' "$tag" "$rc" "$maxfd"
}
run w100_pub   worker 100  101 --publish
run w477_pub   worker 477  102 --publish
run w511_pub   worker 511  103 --publish
run w1000_pub  worker 1000 104 --publish
echo "--- no-publish control (worker) ---"
run w511_nopub worker 511  105
echo "--- compat controls (no publish: compat recv blocks in chunk_rev_topic) ---"
run c511_nopub compat 511  106
echo "SWEEP_DONE"
