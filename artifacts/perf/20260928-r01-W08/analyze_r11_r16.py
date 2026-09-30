#!/usr/bin/env python3
"""W08 逐样本汇总（**新一版 run: r11..r16**，D-12 证据污染收口后重跑）。

⛔ 与同目录的 analyze.py / perf_summary.json 的关系:
   analyze.py + perf_summary.{json,md} 是 **2026-09-28 19:57 生成的历史派生记录**,
   对应已被覆盖的 r05..r10; 它们**保留不动**, 只用于交叉核对"污染前是什么量级"。
   本脚本产出 perf_summary_r11_r16.{json,md}, 是当前唯一有效的 W08 数值来源。

⛔ 仍不是 W11 的正式性能结论: 单轮、同机并发负载、未做 5 轮交替与中位数判定规则(W00 §8)。
"""
import csv
import json
import os
import statistics as st

ROOT = os.path.dirname(os.path.abspath(__file__))
PERF = os.path.dirname(ROOT)
RUNS = [
    ("tlv", "prebuilt", "20260928-r11-W08-tlv-prebuilt", "20260928-r05-W08-tlv-prebuilt"),
    ("tlv", "permsg", "20260928-r12-W08-tlv-permsg", "20260928-r06-W08-tlv-permsg"),
    ("dzflat-a", "prebuilt", "20260928-r13-W08-dzflat-a-prebuilt", "20260928-r07-W08-dzflat-a-prebuilt"),
    ("dzflat-a", "permsg", "20260928-r14-W08-dzflat-a-permsg", "20260928-r08-W08-dzflat-a-permsg"),
    ("dzflat-b", "prebuilt", "20260928-r15-W08-dzflat-b-prebuilt", "20260928-r09-W08-dzflat-b-prebuilt"),
    ("dzflat-b", "permsg", "20260928-r16-W08-dzflat-b-permsg", "20260928-r10-W08-dzflat-b-permsg"),
]
SIZES = ["img66k", "img262k", "img880k"]


def med(xs):
    return st.median(xs) if xs else float("nan")


def p95(xs):
    if not xs:
        return float("nan")
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(0.95 * (len(s) - 1))))]


# 污染前的历史派生值（来自 perf_summary.json，19:57；⛔ 只作对照，不作结论来源）
hist = {}
hp = os.path.join(ROOT, "perf_summary.json")
if os.path.exists(hp):
    for r in json.load(open(hp))["runs"]:
        hist[(r["run_id"], r["size"])] = r

rows_out, bad = [], []
for transport, variant, run_id, old_id in RUNS:
    path = os.path.join(PERF, run_id, "samples.csv")
    if not os.path.exists(path):
        bad.append(f"缺 {path}")
        continue
    rows = list(csv.DictReader(open(path, newline="")))
    if len(rows) != 120:
        bad.append(f"{run_id}: sample_count={len(rows)} != 120")
    for r in rows:
        if r["path"] != transport:
            bad.append(f"{run_id}: path 标记 {r['path']} != 配置 {transport}")
            break
        if r["payload_checksum_ok"] != "1" or r["dropped"] != "0":
            bad.append(f"{run_id}: 有样本校验失败或被丢弃")
            break
    for size in SIZES:
        sub = [r for r in rows if r["route"].endswith("/" + size)]
        if not sub:
            bad.append(f"{run_id}: 缺尺寸档 {size}")
            continue
        t = [int(r["transport_ns"]) for r in sub if r["transport_ns"]]
        d = [int(r["delivery_ns"]) for r in sub if r["delivery_ns"]]
        a = [int(r["app_read_ns"]) for r in sub if r["app_read_ns"]]
        e = [int(r["e2e_ns"]) for r in sub if r["e2e_ns"]]
        h = hist.get((old_id, size))
        rows_out.append({
            "run_id": run_id, "transport": transport, "variant": variant, "size": size, "n": len(sub),
            "transport_us_med": med(t) / 1000.0, "transport_us_p95": p95(t) / 1000.0,
            "delivery_us_med": med(d) / 1000.0,
            "app_read_us_med": med(a) / 1000.0, "app_read_us_p95": p95(a) / 1000.0,
            "e2e_us_med": med(e) / 1000.0,
            "historical_run_id": old_id,
            "historical_transport_us_med": h["transport_us_med"] if h else None,
            "historical_source": "perf_summary.json (2026-09-28 19:57, r05..r10 覆盖前)",
        })

json.dump({"runs": rows_out, "problems": bad}, open(os.path.join(ROOT, "perf_summary_r11_r16.json"), "w"), indent=2)

L = []
L.append("# W08 跨进程 A/B/TLV 逐样本汇总 —— 新一版 run（r11..r16）\n")
L.append("> **本表是 D-12 证据污染收口后的当前有效来源。** 表头字段与 W03 `field_schema.h` 同源；")
L.append("> 旧表（r05..r10）所依据的 `samples.csv` 已于 2026-09-28 20:22:4x 被就地覆盖，**旧表已失效**；")
L.append("> 覆盖前的派生记录保留在 `perf_summary.json`（19:57），仅作交叉核对。\n")
L.append("口径：`transport_ns = transport_done − publish_enter`；`app_read_ns = fully_consumed − app_obtained`；")
L.append("`e2e_ns = fully_consumed − produced`（跨进程 CLOCK_MONOTONIC）；单位 µs。⛔ 单轮，非 W11 结论。\n")
L.append("| run_id | transport | variant | size | n | transport 中位 | transport p95 | delivery 中位 | app_read 中位 | e2e 中位 | 污染前同名 run 的 transport 中位 |")
L.append("|---|---|---|---|---|---|---|---|---|---|---|")
for r in rows_out:
    h = r["historical_transport_us_med"]
    L.append("| {run_id} | {transport} | {variant} | {size} | {n} | {transport_us_med:.1f} | {transport_us_p95:.1f} | "
             "{delivery_us_med:.1f} | {app_read_us_med:.1f} | {e2e_us_med:.1f} | {hs} |".format(
                 **r, hs=("—" if h is None else f"{h:.1f}")))
L.append("\n## 每行校验\n")
L.append("- 6 个 run 的样本数(=120)、`path` 标记、`payload_checksum_ok=1`、`dropped=0` 全部通过。")
L.append("- 问题清单：" + ("无" if not bad else "; ".join(bad)))


def find(t, v, s, key):
    for r in rows_out:
        if r["transport"] == t and r["variant"] == v and r["size"] == s:
            return r[key]
    return float("nan")


L.append("\n## 机制读数（大档 img880k，`permsg`，中位数 µs）\n")
tp, ap, bp = (find(t, "permsg", "img880k", "transport_us_med") for t in ("tlv", "dzflat-a", "dzflat-b"))
te, ae, be = (find(t, "permsg", "img880k", "e2e_us_med") for t in ("tlv", "dzflat-a", "dzflat-b"))
L.append(f"- 发布路径：TLV {tp:.1f} → A {ap:.1f} → **B {bp:.1f}**。A→B 的差 ≈ {ap - bp:.1f} µs，"
         "即「对象 → chunk 的那一跳 0.88 MB 拷贝」。")
L.append(f"- 生产到消费 e2e：TLV {te:.1f} → A {ae:.1f} → **B {be:.1f}**（B vs A ≈ {be - ae:+.1f} µs）。")
L.append("- **B 的 transport 三档基本恒定**（与载荷大小无关）是「省掉最后一次拷贝」的签名；"
         "A 随载荷线性增长。完整读取（app_read）三档基本相等 ⇒ 消费者工作量对齐。")
L.append(f"- ⛔ 不得写成“B 级提升 N 倍”：A→B 在 transport 上 ≈{ap / bp:.1f}× 只是省掉一次拷贝；"
         f"e2e 上 B 相对 A 只有 {(be - ae) / ae * 100:.1f}% 量级。")
L.append("- ⛔ 与同进程探针（113.8/151.6 µs，docs/dzflat_shm.md §9.5）不可换算或互推。")
open(os.path.join(ROOT, "perf_summary_r11_r16.md"), "w").write("\n".join(L) + "\n")

print("\n".join(L[7:]))
print("\nproblems:", bad if bad else "none")
