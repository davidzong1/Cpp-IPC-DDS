#!/usr/bin/env python3
"""W02 summary.csv 表头/格式串/数据行三方一致性校验（t47 回归闸）。

为什么需要：t47 给 summary.csv 插入 5 个 gate 列 + `bad_header` 列时，**格式串没同步**，
导致 `fprintf` 读越界 ⇒ 进程 SIGSEGV（rc=139）且 summary.csv 为 0 字节。
那一次是靠"跑一遍就崩"发现的；这条闸把它变成**每次 ctest 都能拦住**的机械判据。

判据（三条，任一不满足即失败）：
  ① 表头列数 == 数据行列数（同一 run 内每行都查）；
  ② 源码里 `write_summary_csv` 的表头字符串与 `fprintf` 格式串的**字段数相等**；
  ③ 必含列清单存在（防止新列只在文档里、实际没落盘）。

用法：
    python3 test/w02_summary_schema_check.py <run_dir>          # 校验一次运行（静态 + 动态）
    python3 test/w02_summary_schema_check.py --source-only      # 只校验源码 ②③
    python3 test/w02_summary_schema_check.py --fixtures         # 用固化小样本跑**动态半**（秒级，进 CTest）
退出码 0 = 通过；1 = 不合格。

⛔ 为什么还要 `--fixtures`（t50/R-1）：t47 只登记了 `--source-only`，于是**动态半**——逐行
「gate=0 ⇒ `failure_reasons` 非空」与「gate 取值必须 ∈ {0,1}」——**不在 CTest 里**，
注入 F（`gate=0` 而名点空）与 G（`gate=2`）都不会被自动拦住。
`--fixtures` 用 3 行固化样本覆盖：1 个正常 case + 1 个 gate=0 且名点非空（正控）+
1 个 gate=0 而名点空（必须转红）。⛔ 它**不跑 75-case 矩阵**，耗时毫秒级。
"""

import csv
import os
import re
import sys

# 判定通路必须落盘的列（t47/W02-F3 的验收面）
REQUIRED = [
    "bad_header",
    "late_ok", "backlog_ok", "send_blocked_ok", "abnormal_ok", "bad_header_ok",
    "late", "late_threshold_ns", "backlog_max_ns", "send_blocked", "abnormal",
    "failure_reasons", "case_ok",
]


def source_block():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xproc_benchmark.cpp")
    s = open(p, encoding="utf-8").read()
    i = s.index("static void write_summary_csv")
    j = s.index("static std::string case_target_label")
    return s[i:j]


def check_source():
    blk = source_block()
    hm = re.search(r'std::fprintf\(f,\s*((?:\s*"[^"]*"\s*)+)\)', blk, re.S)
    if not hm:
        print("FAIL: 找不到 summary 表头 fprintf")
        return 1
    hdr = "".join(re.findall(r'"([^"]*)"', hm.group(1)))
    cols = [c for c in hdr.replace("\\n", "").strip().split(",") if c]

    fm = re.findall(r'std::fprintf\(f,\s*((?:\s*"[^"]*"\s*)+)', blk, re.S)
    if len(fm) < 2:
        print("FAIL: 找不到数据行 fprintf")
        return 1
    fmt = "".join(re.findall(r'"([^"]*)"', fm[1]))
    # 计格式符：%[flags][width][.prec][len]conv；%% 不计
    specs = [m.group(0) for m in re.finditer(r'%(?!%)(?:[-+ #0]*)(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGaAcspn]', fmt)]

    rc = 0
    if len(cols) != len(specs):
        print("FAIL: 表头列数 %d != 格式串字段数 %d" % (len(cols), len(specs)))
        rc = 1
    else:
        print("OK  : 表头列数 == 格式串字段数 == %d" % len(cols))
    missing = [c for c in REQUIRED if c not in cols]
    if missing:
        print("FAIL: 表头缺必需列 %s" % missing)
        rc = 1
    else:
        print("OK  : 必需列齐备（%d 项）" % len(REQUIRED))
    return rc


FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "w02_summary_schema")


def check_fixtures():
    """用固化小样本跑动态半（t50/R-1）。

    判据（每条都必须能被"注入即转红"验证）：
      ① 正控 fixture（`summary_ok.csv`）⇒ **必须通过**（否则闸自身坏了 ⇒ 假红）；
      ② 注入 F（`summary_inject_f.csv`：某行 gate=0 而 `failure_reasons` 空）⇒ **必须失败**；
      ③ 注入 G（`summary_inject_g.csv`：某行 gate=2，非 0/1）⇒ **必须失败**；
      ④ 注入 A（`summary_inject_a.csv`：表头多一列而数据行未跟上）⇒ **必须失败**。
    """
    import contextlib
    import io
    rc = 0
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        ok = check_run(FIXTURE_DIR, csv_name="summary_ok.csv", quiet=True)
    detail_ok = [l for l in buf.getvalue().split("\n") if l.startswith("FAIL")]
    if ok == 0:
        print("OK  : 正控 fixture 通过（闸自身没问题）")
    else:
        print("FAIL: 正控 fixture 竟然不通过 ⇒ 闸自身坏了（假红）")
        rc = 1
    for name, tag in (("summary_inject_f.csv", "F(gate=0 而名点空)"),
                      ("summary_inject_g.csv", "G(gate=2 越界取值)"),
                      ("summary_inject_a.csv", "A(表头加列、数据行未跟上)")):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            got = check_run(FIXTURE_DIR, csv_name=name, quiet=True)
        reason = next((l for l in buf.getvalue().split("\n") if l.startswith("FAIL")), "")
        if got != 0:
            print("OK  : 注入 %-26s ⇒ **转红**（被拦）  | %s" % (tag, reason[6:90]))
        else:
            print("FAIL: 注入 %-26s ⇒ **未转红**（该形态逃过结构闸，闸无效）" % tag)
            rc = 1
    if detail_ok:
        print("      （正控内部的失败明细：%s）" % detail_ok[0][:80])
    return rc


def check_run(run_dir, csv_name="summary.csv", quiet=False):
    p = os.path.join(run_dir, csv_name)
    rows = list(csv.reader(open(p, newline="", encoding="utf-8")))
    if not rows:
        print("FAIL: %s 为空（0 字节 ⇒ 可能是格式串越界崩溃）" % p)
        return 1
    hdr, data = rows[0], rows[1:]
    rc = 0
    bad = [(i + 2, len(r)) for i, r in enumerate(data) if len(r) != len(hdr)]
    if bad:
        print("FAIL: %d/%d 行列数与表头(%d)不符，前 3 例: %s（注入 A：表头加列未同步数据行）"
              % (len(bad), len(data), len(hdr), bad[:3]))
        rc = 1
    elif not quiet:
        print("OK  : summary.csv 表头 %d 列，数据 %d 行，逐行列数一致" % (len(hdr), len(data)))
    missing = [c for c in REQUIRED if c not in hdr]
    if missing:
        print("FAIL: summary.csv 缺必需列 %s" % missing)
        rc = 1
    elif not quiet:
        print("OK  : summary.csv 必需列齐备")
    # gate 列必须是 0/1 且与 failure_reasons 一致（同一判定逻辑：gate_ok=0 ⇒ 必有名点）
    try:
        i_ok = [hdr.index(c) for c in ("late_ok", "backlog_ok", "send_blocked_ok", "abnormal_ok", "bad_header_ok")]
        i_rs = hdr.index("failure_reasons")
    except ValueError:
        return rc
    bad2 = 0
    bad_gate_value = 0
    for r in data:
        if any(r[i] not in ("0", "1") for i in i_ok):
            bad_gate_value += 1
        elif any(r[i] == "0" for i in i_ok) and r[i_rs].strip() == "":
            bad2 += 1
    if bad_gate_value:
        print("FAIL: %d 行的 gate 列取值不是 0/1（注入 G：越界取值会被读成「通过」或「未知」）" % bad_gate_value)
        rc = 1
    if bad2:
        print("FAIL: %d 行出现「gate=0 但 failure_reasons 为空」（判定逻辑不一致，注入 F）" % bad2)
        rc = 1
    elif not quiet:
        print("OK  : 逐行 gate 布尔与 failure_reasons 一致（gate=0 ⇒ 必有名点）")
    return rc


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--source-only":
        sys.exit(check_source())
    if len(sys.argv) >= 2 and sys.argv[1] == "--fixtures":
        r = check_source()
        r |= check_fixtures()
        sys.exit(r)
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    r = check_source()
    r |= check_run(sys.argv[1])
    sys.exit(r)
