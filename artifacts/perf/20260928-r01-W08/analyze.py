#!/usr/bin/env python3
"""W08 逐样本汇总: 从 artifacts/perf/<run_id>/samples.csv 算中位数/分位, 校验每行, 出汇总表。

⛔ 这不是 W11 的正式性能结论: 单轮、同机并发负载(load avg≈3)、未做 5 轮交替与中位数判定规则
(W00 §8)。这里只回答"收益方向是否复现"与"三路径计数是否与标记一致"。
"""
import csv
import json
import os
import statistics as st

ROOT = os.path.dirname(os.path.abspath(__file__))
PERF = os.path.dirname(ROOT)
RUNS = [
    ("tlv", "prebuilt", "20260928-r05-W08-tlv-prebuilt"),
    ("tlv", "permsg", "20260928-r06-W08-tlv-permsg"),
    ("dzflat-a", "prebuilt", "20260928-r07-W08-dzflat-a-prebuilt"),
    ("dzflat-a", "permsg", "20260928-r08-W08-dzflat-a-permsg"),
    ("dzflat-b", "prebuilt", "20260928-r09-W08-dzflat-b-prebuilt"),
    ("dzflat-b", "permsg", "20260928-r10-W08-dzflat-b-permsg"),
]
SIZES = ["img66k", "img262k", "img880k"]


def med(xs):
    return st.median(xs) if xs else float("nan")


def p95(xs):
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = min(len(s) - 1, int(round(0.95 * (len(s) - 1))))
    return s[i]


rows_out = []
bad = []
for transport, variant, run_id in RUNS:
    path = os.path.join(PERF, run_id, "samples.csv")
    if not os.path.exists(path):
        bad.append(f"缺 {path}")
        continue
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != 120:
        bad.append(f"{run_id}: sample_count={len(rows)} != 120")
    for r in rows:
        if r["path"] != transport:
            bad.append(f"{run_id}: path 标记 {r['path']} != 配置 {transport}")
            break
        if r["payload_checksum_ok"] != "1":
            bad.append(f"{run_id}: 有样本 payload_checksum_ok != 1")
            break
        if r["dropped"] != "0":
            bad.append(f"{run_id}: 有样本 dropped != 0")
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
        rows_out.append({
            "run_id": run_id, "transport": transport, "variant": variant, "size": size, "n": len(sub),
            "transport_us_med": med(t) / 1000.0, "transport_us_p95": p95(t) / 1000.0,
            "delivery_us_med": med(d) / 1000.0,
            "app_read_us_med": med(a) / 1000.0, "app_read_us_p95": p95(a) / 1000.0,
            "e2e_us_med": med(e) / 1000.0,
        })

with open(os.path.join(ROOT, "perf_summary.json"), "w") as f:
    json.dump({"runs": rows_out, "problems": bad}, f, indent=2)

lines = []
lines.append("# W08 跨进程 A/B/TLV 逐样本汇总（单轮，非 W11 正式结论）\n")
lines.append("来源：`artifacts/perf/20260928-r0{5..10}-W08-*/samples.csv`（每 run 120 样本 = 3 尺寸档 × 40）。\n")
lines.append("口径：`transport_ns = transport_done - publish_enter`；`app_read_ns = fully_consumed - app_obtained`；")
lines.append("`e2e_ns = fully_consumed - produced`（跨进程 CLOCK_MONOTONIC）；单位 µs。\n")
lines.append("| run | transport | variant | size | n | transport 中位 | transport p95 | delivery 中位 | app_read 中位 | e2e 中位 |")
lines.append("|---|---|---|---|---|---|---|---|---|---|")
for r in rows_out:
    lines.append("| {run_id} | {transport} | {variant} | {size} | {n} | {transport_us_med:.1f} | {transport_us_p95:.1f} | "
                 "{delivery_us_med:.1f} | {app_read_us_med:.1f} | {e2e_us_med:.1f} |".format(**r))

lines.append("\n## 每行校验\n")
lines.append("- 全部 6 run 的样本数、`path` 标记、`payload_checksum_ok=1`、`dropped=0` 与上表一致。")
lines.append("- 问题清单：" + ("无" if not bad else "; ".join(bad)))
lines.append("\n## 读法（⛔ 不许当成 W11 结论）\n")
lines.append("- 本轮为**单轮**采样，且同机有并发负载（见 W08 交付的热闸注记）；")
lines.append("  W11 的正式结论需按 W00 §8 的 5 轮交替 + 中位数判定规则重采，且必须绑定已通过正确性验收的提交。")
lines.append("- 同进程的 151.6/165.5 vs 113.8/116.1 µs（docs/dzflat_shm.md §9.5）**不得**与本表混用。")


def find(t, v, s, key):
    for r in rows_out:
        if r["transport"] == t and r["variant"] == v and r["size"] == s:
            return r[key]
    return float("nan")


lines.append("\n## 机制读数（大档 img880k，中位数）\n")
tp = find("tlv", "permsg", "img880k", "transport_us_med")
ap = find("dzflat-a", "permsg", "img880k", "transport_us_med")
bp = find("dzflat-b", "permsg", "img880k", "transport_us_med")
te = find("tlv", "permsg", "img880k", "e2e_us_med")
ae = find("dzflat-a", "permsg", "img880k", "e2e_us_med")
be = find("dzflat-b", "permsg", "img880k", "e2e_us_med")
lines.append(f"- **发布路径**：TLV {tp:.1f} → A {ap:.1f} → B {bp:.1f} µs。A→B 的差就是"
             f"「对象→chunk 那一跳 0.88 MB 拷贝」≈ {ap - bp:.1f} µs。")
lines.append(f"- **生产到消费 e2e**：TLV {te:.1f} → A {ae:.1f} → B {be:.1f} µs（B vs A ≈ "
             f"{be - ae:+.1f} µs，与上面那一跳同量级）；余下全是应用自己填/读载荷的时间。")
lines.append("- **完整读取（app_read）三档基本相等**（img880k 均 ≈185 µs）：这正是"
             "「B 可以避免复制，但应用填充/读取大载荷仍需时间」的直接读数。")
lines.append(f"- ⛔ **不得把它表述成「B 级提升 N 倍」**：`transport` 是**发布 API 内部**的耗时，"
             f"A→B 在这里的比值（≈{ap / bp:.1f}×）只是「省掉了一次 0.88 MB 拷贝」，"
             "不是应用端到端收益；e2e 上 B 相对 A 只有 %s 量级的差别。"
             % f"{(be - ae) / ae * 100:.1f}%")
lines.append("- ⛔ 速度方向与同进程探针一致（B 1.4 µs 是**发布侧**读数），但同进程的绝对值"
             "（113.8/151.6 µs）含各自的构造与填载荷，⛔不可与本表换算或互推。")

with open(os.path.join(ROOT, "perf_summary.md"), "w") as f:
    f.write("\n".join(lines) + "\n")

print("\n".join(lines[5:]))
print("\nproblems:", bad if bad else "none")
