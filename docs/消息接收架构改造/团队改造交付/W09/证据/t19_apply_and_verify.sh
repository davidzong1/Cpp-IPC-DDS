#!/usr/bin/env bash
# W09/t19：把「三种回退语义分列」的调用点补丁应用到 shm_pub_sub_ipc.{h,cc}，并机械验证。
#
# 前置（必须）：t6/t7 已落地（EX-1 窗口重新开启），且工作区干净。
# 用法：  bash docs/消息接收架构改造/团队改造交付/W09/证据/t19_apply_and_verify.sh
#
# ⛔ 本脚本**只做计数器点**，不改任何发布/接收行为、不改任何既有函数签名与返回值。
set -euo pipefail
ROOT=/home/zwc/cpp_ipc_dds
cd "$ROOT"

echo "[0] 前置：记录改动前 sha256（EX-1 延续要求）"
sha256sum src/dzIPC/shm_pub_sub_ipc.cc include/dzIPC/shm_pub_sub_ipc.h

echo "[1] 应用补丁（callsite header / cc）"
patch -p0 < docs/消息接收架构改造/团队改造交付/W09/证据/t19_callsite_header.patch
patch -p0 < docs/消息接收架构改造/团队改造交付/W09/证据/t19_callsite_cc.patch

echo "[2] 重配 + 重编（aux_source_directory 是配置期展开，必须重配）"
cmake -S . -B build >/dev/null
make -C build -j"$(nproc)"

echo "[3] 机械核对调用点数量（期望值）"
hdr_hits=$(grep -c "note_dzflat_borrow_failed" include/dzIPC/shm_pub_sub_ipc.h || true)
cc_hits=$(grep -c "note_dzflat_attempt" src/dzIPC/shm_pub_sub_ipc.cc || true)
echo "  header 内 borrow_failed 落点 = $hdr_hits (期望 3)"
echo "  cc 内 attempt 分类落点      = $cc_hits (期望 3: publish_blocking / sniffer / prebuilt)"
[ "$hdr_hits" -ge 3 ] || { echo "FAIL: header 落点不足"; exit 1; }
[ "$cc_hits" -ge 3 ] || { echo "FAIL: cc 落点不足"; exit 1; }

echo "[4] 跑判据"
cd build && env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS \
  ctest -R "dzflat_fallback_semantics|w03_measurement|w08_dzflat_ab" --output-on-failure

echo "[5] 完成后 sha256（与 [0] 一并登记）"
cd "$ROOT" && sha256sum src/dzIPC/shm_pub_sub_ipc.cc include/dzIPC/shm_pub_sub_ipc.h
echo "OK：调用点已接线。请把 [0]/[5] 两组 sha256 写进回报。"
