#!/usr/bin/env python3
"""T06 / W12 §8 —— 逐臂把四类场景的读数抽成矩阵（只读产物；不写 /dev/shm，不调用产品 API）。

用法: python3 summarize_scenarios.py [scenarios_root] [> summary.tsv]

⛔ 只解析 `T06_*_RESULT` 汇总行（`T06_RACE_PROGRESS` 里的 executed= 是**逐轮**值，
   解析它会把 1200 轮误读成 1。本条是量具自身的坑，已在 t06 记录）。
"""
import os
import re
import sys

ARMS = ["FORCE", "FIX", "FIX_RB", "V2NEG", "FIXNEG", "BASELINE", "ABLATE"]
SCENS = ["fresh", "live", "dead", "race"]


def kv_file(text, key, cast=str):
    m = re.search(r"^%s\s*=\s*(.*)$" % re.escape(key), text, re.M)
    return cast(m.group(1).strip()) if m else None


def result_line(text, tag):
    m = re.search(r"^T06_%s_RESULT (.*)$" % tag, text, re.M)
    return m.group(1) if m else ""


def fields(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def num(d, k, cast=int):
    v = d.get(k)
    if v is None:
        return None
    try:
        return cast(v)
    except ValueError:
        return v


def main(root):
    rows = []
    for arm in ARMS:
        d = os.path.join(root, arm)
        if not os.path.isdir(d):
            continue
        fp = os.path.join(d, "fingerprint.txt")
        fpt = open(fp).read() if os.path.exists(fp) else ""
        texts = {}
        for s in SCENS:
            p = os.path.join(d, s + ".log")
            texts[s] = open(p).read() if os.path.exists(p) else ""
        row = {
            "arm": arm,
            "lib_sha256_16": (kv_file(fpt, "lib_sha256") or "?")[:16],
            "lib_size": kv_file(fpt, "lib_size"),
            "probe_sha256_16": (kv_file(fpt, "probe_sha256") or "?")[:16],
            "official_bin_sha16": (kv_file(fpt, "official_binary_sha256") or "?")[:16],
        }
        for s in SCENS:
            row[s + "_measer"] = len(re.findall(r"T06_MEAS_ERR", texts[s]))
            row[s + "_probe_exit"] = kv_file(texts[s], "probe_exit")

        f = fields(result_line(texts["fresh"], "FRESH"))
        row["fresh_ok"] = num(f, "ok")
        row["fresh_rounds_with_reset"] = num(f, "rounds_with_reset")
        row["fresh_reset_lines"] = num(f, "reset_lines_total")

        l = fields(result_line(texts["live"], "LIVE"))
        row["live_subject_ok"] = num(l, "subject_ok")
        row["live_rounds_with_reset"] = num(l, "subject_rounds_with_reset")
        row["live_holder_unchanged"] = num(l, "holder_unchanged")
        row["live_same_id_rounds"] = num(l, "same_id_rounds")

        dd = fields(result_line(texts["dead"], "DEAD"))
        row["dead_rounds_as_expected"] = num(dd, "rounds_as_expected")
        row["dead_borrowed_full"] = num(dd, "borrowed_full")
        row["dead_rounds_with_reset"] = num(dd, "rounds_with_reset")
        row["dead_pre_chain"] = num(dd, "pre_chain_last")

        r = fields(result_line(texts["race"], "RACE"))
        row["race_rounds"] = num(r, "rounds")
        row["race_executed"] = num(r, "executed")
        row["race_bad_rounds"] = num(r, "bad_rounds")
        row["race_same_id_pairs"] = num(r, "same_id_pairs")
        row["race_same_data_ptr"] = num(r, "same_data_ptr")
        row["race_alias_hit_rounds"] = num(r, "alias_hit_rounds")
        row["race_crash_rounds"] = num(r, "crash_rounds")
        row["race_first_alias_round"] = num(r, "first_alias_round")
        row["race_worker_reset_total"] = num(r, "worker_reset_total")
        row["race_worker_rounds_with_reset"] = num(r, "worker_rounds_with_reset")
        row["race_worker_rounds_without_reset"] = num(r, "worker_rounds_without_reset")
        rows.append(row)

    cols = list(rows[0].keys()) if rows else []
    print("\t".join(cols))
    for r in rows:
        print("\t".join("" if r.get(c) is None else str(r.get(c)) for c in cols))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "artifacts/perf/20261001-w12-T06/scenarios")
