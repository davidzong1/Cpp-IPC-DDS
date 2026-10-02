#!/usr/bin/env bash
# 私有 /dev/shm 与 /tmp：不停止、不修改宿主机 RouDi；退出即由内核回收。
set -euo pipefail
if [[ "${1:-}" != "--inside" ]]; then
  exec unshare --user --map-root-user --mount --ipc --fork bash "$0" --inside "$@"
fi
shift
roudi="${1:?需要 RouDi 路径}"; shift
config="${1:?需要 RouDi 配置路径}"; shift
log="${1:?需要日志路径}"; shift
mount --make-rprivate /
mount -t tmpfs -o size=4G tmpfs /dev/shm
mount -t tmpfs -o size=256M tmpfs /tmp
"$roudi" -c "$config" > "$log" 2>&1 &
roudi_pid=$!
trap 'kill "$roudi_pid" 2>/dev/null || true; wait "$roudi_pid" 2>/dev/null || true' EXIT
sleep 1
kill -0 "$roudi_pid"
export COMPARISON_ROUDI_PID="$roudi_pid"
"$@"
