#!/bin/bash
# W10-R3 批跑失败传播的**验收脚本**（t37 / H5）。
#
# 三件事：
#   A) 真实构造一个"子运行失败"（--inject-fail）⇒ **批次必须非零退出**；
#   B) 用**从 w10_run_matrix.sh 中抽取的同一段 `record_and_tally`** 驱动边界输入
#      （rc=124 / rc=139 / verdict=FAIL / 必需文件缺失）⇒ 逐条必须判失败；
#   C) 真实构造"落盘被拒"（输出目录预先被占）⇒ 单运行非零退出（批次据此判失败）。
#
# 用法: w10_r3_batch_acceptance.sh <work_root>
set -u
W=${1:?usage: w10_r3_batch_acceptance.sh <work_root>}
cd "$(dirname "$0")/../../.." || exit 1
mkdir -p "$W"
fail=0
ok()   { echo "  [OK]   $1"; }
bad()  { echo "  [FAIL] $1"; fail=1; }

echo "=== A) 注入子运行失败 ⇒ 批次非零退出 ==="
A="$W/A"; rm -rf "$A"
bash test/perf/w10/w10_run_matrix.sh "$A" --run-id-prefix r3acc-A \
     --only shm-ind-1,shm-ind-100 --inject-fail shm-ind-100 > "$W/A.driver.log" 2>&1
rcA=$?
echo "  批次 rc=$rcA"; sed 's/^/    /' "$A/summary.tsv"
[ "$rcA" -ne 0 ] && ok "注入失败使批次非零退出（rc=$rcA）" || bad "注入失败却返回 0"
grep -q "W10_MATRIX_BATCH_FAIL sub=1" "$W/A.driver.log" && ok "批次汇总行明确列出失败子项" || bad "缺 BATCH_FAIL 汇总"

echo
echo "=== B) 用**同一段** record_and_tally 驱动边界输入 ==="
# 从真实脚本里抽出函数体（保证是同一份代码，不是重写）
awk '/^record_and_tally\(\) \{/,/^\}$/' test/perf/w10/w10_run_matrix.sh > "$W/tally_snippet.sh"
[ -s "$W/tally_snippet.sh" ] && ok "已从 w10_run_matrix.sh 抽出 record_and_tally（$(wc -l < "$W/tally_snippet.sh") 行）" \
                             || bad "抽取失败"
cat > "$W/drive.sh" <<'EOS'
#!/bin/bash
set -u
ROOT="$1"; shift
REQUIRED_TOPOLOGY="a.csv,b.csv"
declare -a RESULTS=(); declare -a EXPECTED_FAILS=()
printf '%s\trc=%s\tok=%s\t%s\tmissing=%s\n' "$@" >> /dev/null
EOS
# 直接在同一 shell 内定义并使用抽取的函数
. "$W/tally_snippet.sh"
ROOT="$W/B"; rm -rf "$ROOT"; mkdir -p "$ROOT/full"
printf 'x\n' > "$ROOT/full/a.csv"; printf 'x\n' > "$ROOT/full/b.csv"   # 非空才算有效读数
: > "$ROOT/summary.tsv"
REQUIRED_TOPOLOGY="a.csv,b.csv"
declare -a RESULTS=(); declare -a EXPECTED_FAILS=()
record_and_tally "t_rc124"     124 "verdict=FAIL failures=3" "$ROOT/full" 0
record_and_tally "t_rc139"     139 "verdict=PASS failures=0" "$ROOT/full" 0
record_and_tally "t_verdictfail" 0 "verdict=FAIL failures=1" "$ROOT/full" 0
mkdir -p "$ROOT/missing"; printf 'x\n' > "$ROOT/missing/a.csv"
record_and_tally "t_missing"     0 "verdict=PASS failures=0" "$ROOT/missing" 0
mkdir -p "$ROOT/zerobyte"; : > "$ROOT/zerobyte/a.csv"; : > "$ROOT/zerobyte/b.csv"   # 零字节占位 ⇒ 无效
record_and_tally "t_zerobyte"    0 "verdict=PASS failures=0" "$ROOT/zerobyte" 0
echo "  RESULTS=${RESULTS[*]:-<empty>}"
[ "${#RESULTS[@]}" -eq 5 ] && ok "rc=124 / rc=139 / verdict=FAIL / 缺文件 五类边界**全部**判失败（${#RESULTS[@]} 条）" \
                          || bad "五类边界未全部判失败（只判出 ${#RESULTS[@]} 条）"

echo
echo "=== C) 落盘被拒 ⇒ 单运行非零退出 ==="
C="$W/C"; rm -rf "$C"; mkdir -p "$C"; touch "$C/socket-ind-1000-stub"
# 直接调二进制，让 --out 指向一个非空目录（阶段 0 独占预占必须拒绝）
mkdir -p "$C/occupied"; : > "$C/occupied/junk"
./build/bin/w10_matrix --transport shm --topology independent --n 4 --domain 4900 --msgs 1 --run-id occ \
    --out "$C/occupied" > "$W/C.log" 2>&1
rcC=$?
[ "$rcC" -ne 0 ] && ok "输出目录被占 ⇒ 单运行非零退出（rc=$rcC）" || bad "输出目录被占却返回 0"
grep -q "ARTIFACT_DIR_NOT_EMPTY" "$W/C.log" && ok "诊断可识别（ARTIFACT_DIR_NOT_EMPTY）" || bad "缺诊断"

echo
[ "$fail" -eq 0 ] && echo "BATCH_ACCEPTANCE_OK" || echo "BATCH_ACCEPTANCE_FAIL"
exit "$fail"
