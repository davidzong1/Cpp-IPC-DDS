#!/usr/bin/env python3
"""W10-R5 证据聚合器（t38）：把复验矩阵的逐 run 证据汇总成可引用的表格。

读 `artifacts/perf/<run>/<sub>/{manifest.json,windows.csv,windows.jsonl,threestate.json,verdict.md}`
⇒ 输出：
  · matrix_summary.tsv  逐 run：配置 + 五项派生值 + 门禁 + 三态必录
  · derived_summary.md  派生值按 (state, phase, n, diag) 的聚合（含 null 计数与原因）
  · null_audit.txt      所有 null 及其原因（⛔ 零分母不得写 0）

用法: w10_r5_aggregate.py <run_root>
"""
import csv
import json
import os
import sys
from collections import defaultdict


def read_json(p):
    try:
        return json.load(open(p, encoding="utf-8"))
    except Exception:
        return None


def main():
    root = sys.argv[1]
    rows = []
    nulls = []
    for sub in sorted(os.listdir(root)):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        m = read_json(os.path.join(d, "manifest.json"))
        t = read_json(os.path.join(d, "threestate.json"))
        wcsv = os.path.join(d, "windows.csv")
        if not m or not os.path.exists(wcsv):
            continue
        with open(wcsv, encoding="utf-8") as f:
            wr = list(csv.DictReader(f))
        if not wr:
            continue
        w = wr[0]
        rec = {
            "sub": sub,
            "state": m.get("state"),
            "phase": m.get("phase"),
            "n": m.get("topic_count"),
            "W": m.get("effective_workers"),
            "diag": m.get("diagnostics_enabled"),
            "round": m.get("round"),
            "capacity_limited": (m.get("capacity_model") or {}).get("capacity_limited"),
            "wall_s": w.get("wall_s"),
            "routes": w.get("routes"),
            "control_entries": w.get("control_entries"),
            "cpu_cores": w.get("cpu_cores"),
            "threads": w.get("threads"),
            "d_scan_rounds": w.get("d_scan_rounds"),
            "d_scanned": w.get("d_scanned_routes_total"),
            "d_ready": w.get("d_ready_rounds"),
            "d_idle_exits": w.get("d_idle_exits"),
            "d_restarts": w.get("d_thread_restarts"),
            "avg_scanned_per_round": w.get("avg_scanned_per_round"),
            "avg_ns_per_round": w.get("avg_ns_per_round"),
            "avg_ns_per_entry": w.get("avg_ns_per_entry"),
            "ready_round_ratio": w.get("ready_round_ratio"),
            "wait_timeouts_per_s": w.get("wait_timeouts_per_s"),
            "consistent": w.get("resident_gated_consistent"),
            "gates": w.get("gates_passed"),
            "confirm_ok": (t or {}).get("confirm", {}).get("confirmed"),
            "confirm_missing": (t or {}).get("confirm", {}).get("missing_count"),
            "recover_ok": (t or {}).get("recover_ok_routes"),
            "recover_lost": (t or {}).get("recover_lost"),
            "recover_arr_len": len((t or {}).get("recover_first_packet_us", [])),
            "window_idle_exits": (t or {}).get("window_idle_exits"),
            "lib_sha256": (m.get("lib_sha256") or "")[:16],
            "self_verified_fp": "yes" if m.get("lib_sha256") and not m.get("note_fingerprint_backfill") else "backfilled",
        }
        rows.append(rec)
        for k in ("avg_ns_per_round", "avg_ns_per_entry", "wait_timeouts_per_s", "avg_scanned_per_round",
                  "ready_round_ratio"):
            if w.get(k) in ("null", "", None):
                # null 的**原因**必须可区分：诊断未采集 / 分母为 0（无 route ⇒ 无扫描轮）
                if str(m.get("diagnostics_enabled")) == "False" and k in (
                        "avg_ns_per_round", "avg_ns_per_entry", "wait_timeouts_per_s"):
                    why = ("诊断关闭 ⇒ **未采集**（⛔ 非零成本）"
                           if k != "wait_timeouts_per_s"
                           else "诊断关闭 ⇒ 门控 `wait_timeout_count` **未采集**（⛔ 非零成本；常驻孪生见 resident_wait_timeouts_per_s）")
                elif w.get("routes") == "0" or w.get("routes") == 0:
                    why = "routes=0 ⇒ 该态无 route 归属池、无扫描轮 ⇒ 分母为 0"
                elif w.get("d_scan_rounds") == "0" or w.get("d_scan_rounds") == 0:
                    why = "Δscan_rounds=0 ⇒ 窗口内无扫描轮"
                else:
                    why = "diag=" + str(m.get("diagnostics_enabled")) + " routes=" + str(w.get("routes"))
                nulls.append((sub, k, why))

    cols = ["sub", "state", "phase", "n", "W", "diag", "round", "capacity_limited", "wall_s", "routes",
            "control_entries", "cpu_cores", "threads", "d_scan_rounds", "d_scanned", "d_ready", "d_idle_exits",
            "d_restarts", "avg_scanned_per_round", "avg_ns_per_round", "avg_ns_per_entry", "ready_round_ratio",
            "wait_timeouts_per_s", "consistent", "gates", "confirm_ok", "confirm_missing", "recover_ok",
            "recover_lost", "recover_arr_len", "window_idle_exits", "lib_sha256", "self_verified_fp"]
    with open(os.path.join(root, "matrix_summary.tsv"), "w", encoding="utf-8") as f:
        f.write("\t".join(cols) + "\n")
        for r in rows:
            f.write("\t".join(str(r.get(c, "")) for c in cols) + "\n")

    # 派生值聚合（按 state/phase/n/diag）
    agg = defaultdict(list)
    for r in rows:
        agg[(r["state"], r["phase"], r["n"], r["diag"], r["W"])].append(r)
    with open(os.path.join(root, "derived_summary.md"), "w", encoding="utf-8") as f:
        f.write("# W10-R5 派生值聚合（按 state / phase / n / diag / W）\n\n")
        f.write("> 每格给出轮数、均值与极值；`null` 表示**未采集**（诊断关）或分母为 0。\n\n")
        f.write("| state | phase | n | diag | W | 轮数 | scan/round | ns/round | ns/entry | ready 比率 | timeouts/s |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|---|\n")

        def stat(vals, key):
            v = [float(x[key]) for x in vals if x.get(key) not in ("null", "", None)]
            if not v:
                return "null"
            return f"{sum(v) / len(v):.4f} [{min(v):.4f},{max(v):.4f}]"

        for k in sorted(agg, key=lambda x: (str(x[0]), str(x[1]), str(x[2]), str(x[3]), str(x[4]))):
            vals = agg[k]
            f.write(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {k[4]} | {len(vals)} | "
                    f"{stat(vals,'avg_scanned_per_round')} | {stat(vals,'avg_ns_per_round')} | "
                    f"{stat(vals,'avg_ns_per_entry')} | {stat(vals,'ready_round_ratio')} | "
                    f"{stat(vals,'wait_timeouts_per_s')} |\n")

    with open(os.path.join(root, "null_audit.txt"), "w", encoding="utf-8") as f:
        f.write("# null 审计（⛔ 零分母/未采集不得写 0）\n\n")
        cnt = defaultdict(int)
        for sub, k, why in nulls:
            cnt[(k, why)] += 1
        for (k, why), c in sorted(cnt.items()):
            f.write(f"{k}: {c} 次，原因 {why}\n")
        if not nulls:
            f.write("（无 null）\n")

    print(f"runs={len(rows)} nulls={len(nulls)}")
    print(f"  matrix_summary.tsv / derived_summary.md / null_audit.txt 已写入 {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
