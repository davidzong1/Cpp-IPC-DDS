#!/usr/bin/env bash
set -euo pipefail
mount --make-rprivate /
mount -t tmpfs -o size=4G tmpfs /dev/shm
mount -t tmpfs -o size=128M tmpfs /tmp
exec "$@"
