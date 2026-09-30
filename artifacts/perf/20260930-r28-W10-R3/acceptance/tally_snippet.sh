record_and_tally() {  # record_and_tally <sub> <rc> <verdict> <dir> <expect_fail>
  local sub="$1" rc="$2" verdict="$3" dir="$4" expect_fail="$5"
  local missing=""
  if [ -n "$dir" ] && [ -d "$dir" ]; then
    local IFS=','
    # ⛔ 必须是普通文件：同名目录会被 -f 过滤掉；另要求非零大小（零字节占位不算有效读数）。
    for f in $REQUIRED_TOPOLOGY; do { [ -f "$dir/$f" ] && [ -s "$dir/$f" ]; } || missing="$missing$f,"; done
  else
    missing="(no dir)"
  fi
  local ok=1
  [ "$rc" -eq 0 ] || ok=0
  case "$verdict" in *verdict=PASS*) ;; *) ok=0;; esac
  [ -z "$missing" ] || ok=0
  [ "$rc" -eq 124 ] && ok=0      # watchdog 超时 ⇒ 直接失败（R0-3）
  [ "$rc" -eq 139 ] && ok=0      # ABI 不匹配 SIGSEGV ⇒ 直接失败（R0-A1）

  local tag="OK"
  if [ "$expect_fail" -eq 1 ]; then
    if [ "$ok" -eq 0 ]; then tag="EXPECTED_FAIL(正确识别)"; ok=1
    else tag="UNEXPECTED_PASS"; ok=0; fi
  elif [ "$ok" -eq 0 ]; then tag="FAIL"; fi

  printf '%-24s rc=%-4s %-28s %-24s missing=%s\n' "$sub" "$rc" "${verdict:-<no verdict>}" "$tag" "${missing:-none}"
  printf '%s\trc=%s\tok=%s\t%s\tmissing=%s\n' "$sub" "$rc" "$ok" "${verdict:-none}" "${missing:-none}" >> "$ROOT/summary.tsv"
  if [ "$ok" -eq 0 ]; then RESULTS+=("$sub"); else EXPECTED_FAILS+=("$sub"); fi
}
