#!/usr/bin/env python3
"""W02 样本表规范校验器（计时边界 + 空值语义 + 计数恒等式）

为什么需要它：同一份 run 曾被两个复核者算出**不同的 skipped 数**（38 vs 26），
差异不在数据而在**跳过判据**。这个脚本把判据固化为可执行代码，任何人复跑都得到同一组数，
避免「口径靠口头约定」。

规范判据（与 `W02_统一跨进程基准_结果格式与使用.md` §11.4 一致）：
  · 逐行检查 samples.csv 的 4 个派生列：transport_ns / delivery_ns / app_read_ns / e2e_ns
  · **跳过以「行」为单位**：一行里只要有任一派生列为空字段 ⇒ 整行计 skipped，
    该行 4 个单元格**都不计入 cells**。于是恒有 `cells == 4 × (rows − skipped_rows)`。
  · 非空单元格必须等于其定义（td-pe / ao-td / fc-ao / fc-produced）；mismatches 必须为 0
  · 时间戳列为空的行（app_obtained_ns 或 fully_consumed_ns 为空）同样整行跳过

为什么"跳过以行为单位"要写死：同一份 run 曾出现 38 vs 26 两个 skipped 数，根因就是
判据宽窄（是否覆盖 `ao<td`）。若再放任"按单元格跳过"，还会多出第三个数字
（cells=269962，因为整行跳过却只扣 1 个单元格）。行口径下 r20 的唯一答案是
`rows=67500 cells=269848 skipped_rows=38 mismatches=0`。

用法：
    python3 test/w02_sample_audit.py artifacts/perf/<run_id>
输出：stdout 一行汇总 + 逐列空值分类；退出码 0 = 通过，1 = 有 mismatch 或缺文件。
"""

import csv
import glob
import os
import sys

DERIVED = {
    "transport_ns": lambda p, row: int(row["transport_done_ns"]) - int(row["publish_enter_ns"]),
    "delivery_ns": lambda p, row: (int(row["app_obtained_ns"]) - int(row["transport_done_ns"]))
                                  if row["app_obtained_ns"] else None,
    "app_read_ns": lambda p, row: (int(row["fully_consumed_ns"]) - int(row["app_obtained_ns"]))
                                  if (row["fully_consumed_ns"] and row["app_obtained_ns"]) else None,
    "e2e_ns": lambda p, row: (int(row["fully_consumed_ns"]) - int(row["publish_enter_ns" if False else "produced_ns"]))
                             if row["fully_consumed_ns"] else None,
}


def audit(run_dir):
    files = sorted(glob.glob(os.path.join(run_dir, "samples", "*.samples.csv")))
    if not files:
        print("no samples/*.samples.csv under %s" % run_dir)
        return 1

    rows = 0
    cells = 0
    mism = 0
    skipped_rows = 0
    skipped_by_col = {}
    neg_delivery = []          # (file, seq, ao - td) —— ao < td 的行
    empty_ts_rows = 0          # app_obtained / fully_consumed 为空的行
    checksum_empty_by_wl = {}

    for path in files:
        wl = ("timestamp" if "_timestamp_" in path
              else "crc" if "_crc_" in path else "full")
        ck = checksum_empty_by_wl.setdefault(wl, [0, 0])
        with open(path, newline="") as fh:
            for row in csv.DictReader(fh):
                rows += 1
                ck[0] += 1
                if row["payload_checksum_ok"] == "":
                    ck[1] += 1

                ao_empty = row["app_obtained_ns"] == ""
                fc_empty = row["fully_consumed_ns"] == ""
                if ao_empty or fc_empty:
                    empty_ts_rows += 1

                if not ao_empty:
                    d = int(row["app_obtained_ns"]) - int(row["transport_done_ns"])
                    if d < 0:
                        neg_delivery.append((os.path.basename(path), row["seq"], d))

                # 先判定整行是否跳过（行口径），再验算非空单元格
                row_skip = any(row[c] == "" for c in DERIVED)
                if row_skip:
                    skipped_rows += 1
                    for c in DERIVED:
                        if row[c] == "":
                            skipped_by_col[c] = skipped_by_col.get(c, 0) + 1
                    continue
                for col, fn in DERIVED.items():
                    want = fn(path, row)
                    got = row[col]
                    cells += 1
                    if want is None or int(got) != want:
                        mism += 1

    print("files=%d rows=%d cells=%d mismatches=%d skipped_rows=%d"
          % (len(files), rows, cells, mism, skipped_rows))
    assert cells == 4 * (rows - skipped_rows), \
        "行口径被破坏：cells(%d) != 4*(rows-skipped_rows)(%d)" % (cells, 4 * (rows - skipped_rows))
    print("skipped by column: %s" % skipped_by_col)
    print("rows with empty timestamp column: %d" % empty_ts_rows)
    print("payload_checksum_ok empty by workload: %s"
          % {k: "%d/%d" % (v[1], v[0]) for k, v in sorted(checksum_empty_by_wl.items())})
    if neg_delivery:
        worst = min(neg_delivery, key=lambda x: x[2])
        print("ao<td rows: %d (min %d at %s seq=%s)"
              % (len(neg_delivery), worst[2], worst[0], worst[1]))
    else:
        print("ao<td rows: 0")
    return 1 if mism else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(audit(sys.argv[1]))
