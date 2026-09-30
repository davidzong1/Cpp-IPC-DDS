#!/bin/bash
# R2/t42 —— **可复跑**的 §10.2 扫描接线反例控制（重做，非凭记忆补写）。
#
# 做什么：临时把 `collect_pending()` 里 `scan_rounds` 的**常驻**写入拿掉（其余一律不动），
#   重编库 → 跑 w10_scancost 两档 ⇒ 断言必须**转红**（打印具体报红条目）；
#   还原源文件 → 重编 → 复跑 ⇒ 必须**复绿**，并核对 recv_worker.cc 的 sha256。
# 为什么必须真跑：反例控制是判据的承重部分；没有它，「接线确实生效」只剩实现者自述。
#
# 用法: bash test/perf/w10/w10_r2_negative_control.sh <out_dir>
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"; cd "$ROOT"
OUT=${1:?usage: negative_control_scan_wiring.sh <out_dir>}
SRC=src/dzIPC/threepools/recv_worker.cc
EXPECT_RESTORED_SHA=0dde3eabd02e23bafffa3142c20c525535f65b1da2635704c5b25458f742650c
mkdir -p "$OUT"
# 基线快照放进**本 run 目录**（自包含：不依赖 build/ 下的临时文件，任何人可复跑）
cp "$SRC" "$OUT/recv_worker.before.cc"

{
echo "# R2/t42 反例控制：§10.2 扫描接线（去接线 ⇒ 报红 ⇒ 还原 ⇒ 复绿）"
echo "# 时间: $(date '+%Y-%m-%d %H:%M:%S %z')"
echo "# 被改文件: $SRC（唯一改动：删除 1 行 scan_rounds.fetch_add）"
echo
echo "## 0. 基线指纹"
sha256sum "$SRC" build/lib/libipc.so.1.3.0 build/bin/w10_scancost
} > "$OUT/negative_control_scan_wiring.log"

SHA_BEFORE=$(sha256sum "$SRC" | cut -d' ' -f1)
if [ "$SHA_BEFORE" != "$EXPECT_RESTORED_SHA" ]; then
  echo "REFUSE: 基线 $SRC 的 sha256=$SHA_BEFORE != 期望 $EXPECT_RESTORED_SHA（先确认工作区）" | tee -a "$OUT/negative_control_scan_wiring.log"
  exit 3
fi

# ---------- 1. 注入反例：只删 scan_rounds 那一行常驻写入 ----------
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
needle = "    scan_rounds.fetch_add(1, std::memory_order_relaxed);"
assert s.count(needle) == 1, "期望恰好 1 处 scan_rounds.fetch_add"
s = s.replace(needle, "    /* R2/t42 NEGATIVE CONTROL: scan_rounds 写入已临时移除 */")
open(p, 'w', encoding='utf-8').write(s)
print("injected: 1 line removed")
PY
{
echo
echo "## 1. 反例注入后指纹（与基线只差这一行的删除）"
sha256sum "$SRC"
echo "diff:"; diff <(sed -n '/scan_rounds.fetch_add/p' "$OUT/recv_worker.before.cc") <(sed -n '/scan_rounds.fetch_add/p' "$SRC") || true
} >> "$OUT/negative_control_scan_wiring.log"

make -C build -j8 > "$OUT/inject_build.log" 2>&1
echo "make(inject) rc=$?" >> "$OUT/negative_control_scan_wiring.log"

# ---------- 2. 断言必须转红（两档都给具体报红条目） ----------
for diag in off on; do
  {
  echo
  echo "## 2.$diag 反例档 diag=$diag（期望：FAIL 且条目如下）"
  } >> "$OUT/negative_control_scan_wiring.log"
  timeout 300 build/bin/w10_scancost --n 100 --workers 4 --diag "$diag" --window-ms 2000 --domain "86$([ $diag = on ] && echo 31 || echo 30)" \
    > "$OUT/inject_diag_$diag.log" 2>&1
  echo "rc=$?" >> "$OUT/negative_control_scan_wiring.log"
  grep -E "^resident |^gated |^derived |^FAILURE|DONE verdict" "$OUT/inject_diag_$diag.log" >> "$OUT/negative_control_scan_wiring.log"
done

# ---------- 3. 还原（git 取回基线内容 + 逐字核对 sha256） ----------
cp "$OUT/recv_worker.before.cc" "$SRC"
make -C build -j8 > "$OUT/restore_build.log" 2>&1
RC_RESTORE=$?
SHA_AFTER=$(sha256sum "$SRC" | cut -d' ' -f1)
{
echo
echo "## 3. 还原"
echo "make(restore) rc=$RC_RESTORE"
echo "sha256(restored)=$SHA_AFTER"
echo "expect          =$EXPECT_RESTORED_SHA"
if [ "$SHA_AFTER" = "$EXPECT_RESTORED_SHA" ]; then echo "sha256_match=YES"; else echo "sha256_match=NO"; fi
sha256sum "$SRC" build/lib/libipc.so.1.3.0 build/bin/w10_scancost
} >> "$OUT/negative_control_scan_wiring.log"

# ---------- 4. 复绿 ----------
for diag in off on; do
  {
  echo
  echo "## 4.$diag 还原后 diag=$diag（期望：PASS）"
  } >> "$OUT/negative_control_scan_wiring.log"
  timeout 300 build/bin/w10_scancost --n 100 --workers 4 --diag "$diag" --window-ms 2000 --domain "86$([ $diag = on ] && echo 41 || echo 40)" \
    > "$OUT/restore_diag_$diag.log" 2>&1
  echo "rc=$?" >> "$OUT/negative_control_scan_wiring.log"
  grep -E "^resident |^gated |^derived |^FAILURE|DONE verdict" "$OUT/restore_diag_$diag.log" >> "$OUT/negative_control_scan_wiring.log"
done

# ---------- 5. 机器判定 ----------
FAIL_RED_OFF=$(grep -c "^FAILURE" "$OUT/inject_diag_off.log")
FAIL_RED_ON=$(grep -c "^FAILURE" "$OUT/inject_diag_on.log")
GREEN_OFF=$(grep -c "DONE verdict=PASS" "$OUT/restore_diag_off.log")
GREEN_ON=$(grep -c "DONE verdict=PASS" "$OUT/restore_diag_on.log")
{
echo
echo "## 5. 机器判定"
echo "inject diag=off 报红条目数 = $FAIL_RED_OFF （期望 >0）"
echo "inject diag=on  报红条目数 = $FAIL_RED_ON  （期望 >0）"
echo "restore diag=off 复绿 = $GREEN_OFF （期望 1）"
echo "restore diag=on  复绿 = $GREEN_ON  （期望 1）"
echo "sha256 还原一致 = $([ "$SHA_AFTER" = "$EXPECT_RESTORED_SHA" ] && echo YES || echo NO)"
if [ "$FAIL_RED_OFF" -gt 0 ] && [ "$FAIL_RED_ON" -gt 0 ] && [ "$GREEN_OFF" -eq 1 ] && [ "$GREEN_ON" -eq 1 ] && [ "$SHA_AFTER" = "$EXPECT_RESTORED_SHA" ]; then
  echo "NEGATIVE_CONTROL_RESULT=PASS"
else
  echo "NEGATIVE_CONTROL_RESULT=FAIL"
fi
} >> "$OUT/negative_control_scan_wiring.log"
tail -8 "$OUT/negative_control_scan_wiring.log"
