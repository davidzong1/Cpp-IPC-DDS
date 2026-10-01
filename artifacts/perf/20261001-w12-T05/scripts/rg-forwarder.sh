#!/bin/sh
# T05 辅助：本机 PATH 上没有 rg，此处转发到被 codex 打包的 ripgrep 15.2.0。
# ⛔ 本文件只在 run 目录内，不参与产品/工装构建。
exec /home/zwc/.codex/packages/app-server-daemon/releases/0.158.0-x86_64-unknown-linux-musl/codex-path/rg "$@"
