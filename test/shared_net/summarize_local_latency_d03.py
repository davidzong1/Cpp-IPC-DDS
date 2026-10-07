#!/usr/bin/env python3
"""汇总 D03 L0/L1 接收缝对照，保留基线缺失的分段能力。"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


PERCENTILES = (50, 95, 99)
SCENARIO_ORDER = ((1, 4096), (32, 64))


def nearest_rank(values, percentile):
    ordered = sorted(values)
    if not ordered:
        return None
    return ordered[max(0, math.ceil(len(ordered) * percentile / 100) - 1)]


def stats(values):
    values = list(values)
    return {
        "count": len(values),
        "mean_us": sum(values) / len(values) / 1000 if values else None,
        **{f"p{p}_us": nearest_rank(values, p) / 1000 if values else None for p in PERCENTILES},
    }


def read_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def decode(row, mode):
    row = {key: int(value) for key, value in row.items()}
    start = row["read_ns"] - row["elapsed_ns"]
    received = row["recv_return_ns"]
    if not received or not start <= received <= row["read_ns"]:
        raise ValueError("recv_return 缺失或顺序错误")
    stages = {"publish_to_recv_return": received - start}
    if mode == "B":
        begin = row["recv_begin_ns"]
        enqueue = row["enqueue_before_ns"]
        dequeue = row["dequeue_after_ns"]
        if not begin or not enqueue or not dequeue or not begin <= received <= enqueue <= dequeue <= row["read_ns"]:
            raise ValueError("shared_v1 完整接收缝缺失或顺序错误")
        stages.update({"recv_return_to_enqueue_before": enqueue - received,
                       "enqueue_before_to_dequeue_after": dequeue - enqueue,
                       "dequeue_after_to_api_return": row["read_ns"] - dequeue})
    else:
        # 原基线只有 recv_return seam；其余字段为能力缺失，不能当作零开销。
        if any(row[key] for key in ("recv_begin_ns", "enqueue_before_ns", "dequeue_after_ns")):
            raise ValueError("基线出现未声明的内部接收缝")
        stages["recv_return_to_api_return"] = row["read_ns"] - received
    if sum(stages.values()) != row["elapsed_ns"]:
        raise ValueError("同一消息分段和不守恒")
    return {
        "elapsed_ns": row["elapsed_ns"], "stages": stages,
        "assisted": row.get("assisted", 0), "wait_cpu_ns": row.get("wait_cpu_ns", 0),
        "recv_cpu_ns": row.get("recv_cpu_ns", 0), "process_cpu_ns": row.get("process_cpu_ns", 0),
        "get_cpu_ns": row.get("get_cpu_ns", 0), "reader_tid": row.get("reader_tid", 0),
    }


def summarize_rows(rows, mode):
    records = [decode(row, mode) for row in rows]
    records.sort(key=lambda item: item["elapsed_ns"])
    stage_names = tuple(records[0]["stages"]) if records else ()
    slow = records[-max(1, math.ceil(len(records) / 100)):]
    return {
        "count": len(records),
        "e2e": stats(item["elapsed_ns"] for item in records),
        "stages": {name: stats(item["stages"][name] for item in records) for name in stage_names},
        "slowest_one_percent": {
            "count": len(slow),
            "mean_elapsed_us": sum(item["elapsed_ns"] for item in slow) / len(slow) / 1000 if slow else None,
            "stage_mean_us": {name: sum(item["stages"][name] for item in slow) / len(slow) / 1000
                              for name in stage_names},
        },
        "assisted_count": sum(item["assisted"] for item in records),
        "reader_tid_count": len({item["reader_tid"] for item in records if item["reader_tid"]}),
        "cpu_mean_us": {name: sum(item[name] for item in records) / len(records) / 1000
                        for name in ("wait_cpu_ns", "recv_cpu_ns", "process_cpu_ns", "get_cpu_ns")},
    }


def merge_stats(items, field):
    values = [value for item in items for value in item["_values"][field]]
    return stats(values)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    entries = json.loads((root / "results.json").read_text())
    if len(entries) != 16:
        raise ValueError(f"D03 结果窗口数为 {len(entries)}，预期 16")
    windows = []
    for entry in entries:
        window = entry["window"]
        folder = root / window["id"]
        mode = window["mode"]
        rows = []
        for path in sorted(folder.glob("sub*.csv"), key=lambda p: int(p.stem[3:])):
            rows.extend(read_rows(path))
        expected = int(window["seconds"]) * int(window["rate_hz"]) * int(window["subscribers"])
        if len(rows) != expected:
            raise ValueError(f"{window['id']} 接收数 {len(rows)}，预期 {expected}")
        summary = summarize_rows(rows if window["observation"] == "L1" else [
            {"sequence": row["sequence"], "read_ns": row["read_ns"],
             "elapsed_ns": row["elapsed_ns"], "recv_return_ns": row["read_ns"],
             "recv_begin_ns": 0, "enqueue_before_ns": 0, "dequeue_after_ns": 0,
             "assisted": 0, "wait_cpu_ns": 0, "recv_cpu_ns": 0,
             "process_cpu_ns": 0, "get_cpu_ns": 0, "reader_tid": 0}
            for row in rows], mode) if window["observation"] == "L1" else {
                "count": len(rows), "e2e": stats(int(row["elapsed_ns"]) for row in rows),
                "stages": {}, "slowest_one_percent": {}, "assisted_count": None,
                "reader_tid_count": None, "cpu_mean_us": {},
            }
        # L0 does not have receive trace; retain only endpoint values for overhead comparison.
        raw = {"e2e": [int(row["elapsed_ns"]) for row in rows]}
        if window["observation"] == "L1":
            raw["publish_to_recv_return"] = [item["stages"]["publish_to_recv_return"] for item in
                                               (decode(row, mode) for row in rows)]
            for name in summary["stages"]:
                raw[name] = [item["stages"][name] for item in (decode(row, mode) for row in rows)]
        item = {"id": window["id"], "mode": mode, "observation": window["observation"],
                "subscribers": int(window["subscribers"]), "bytes": int(window["bytes"]),
                "position": int(window["position"]), "summary": summary,
                "errors": [], "_values": raw}
        for receiver in entry.get("result", {}).get("receivers", []):
            if receiver.get("lost") or receiver.get("duplicates") or receiver.get("invalid"):
                item["errors"].append("接收完整性计数异常")
            if window["observation"] == "L1" and receiver.get("trace_overflow"):
                item["errors"].append("接收缝溢出")
            if window["observation"] == "L1" and receiver.get("trace_missing_or_unordered"):
                item["errors"].append("接收缝缺失或乱序")
        windows.append(item)

    groups = defaultdict(list)
    for item in windows:
        groups[(item["subscribers"], item["bytes"], item["mode"], item["observation"])].append(item)
    aggregate = []
    overhead = []
    for scenario in SCENARIO_ORDER:
        for mode in ("A", "B"):
            l0 = groups[scenario + (mode, "L0")]
            l1 = groups[scenario + (mode, "L1")]
            l0_e2e = merge_stats(l0, "e2e")
            l1_e2e = merge_stats(l1, "e2e")
            aggregate.append({"scenario": scenario, "mode": mode, "observation": "L0", "e2e": l0_e2e,
                              "window_p50_us": statistics.median(item["summary"]["e2e"]["p50_us"] for item in l0),
                              "window_p99_us": statistics.median(item["summary"]["e2e"]["p99_us"] for item in l0)})
            aggregate.append({"scenario": scenario, "mode": mode, "observation": "L1", "e2e": l1_e2e,
                              "window_p50_us": statistics.median(item["summary"]["e2e"]["p50_us"] for item in l1),
                              "window_p99_us": statistics.median(item["summary"]["e2e"]["p99_us"] for item in l1),
                              "stages": {name: merge_stats(l1, name) for name in l1[0]["summary"]["stages"]}})
            overhead.append({"scenario": scenario, "mode": mode,
                             "p50_delta_us": l1_e2e["p50_us"] - l0_e2e["p50_us"],
                             "p99_delta_us": l1_e2e["p99_us"] - l0_e2e["p99_us"],
                             "mean_delta_us": l1_e2e["mean_us"] - l0_e2e["mean_us"]})

    summary = {
        "schema": "local-latency-d03-summary/1", "windows": [{k: v for k, v in item.items() if k != "_values"} for item in windows],
        "aggregate": aggregate, "observation_overhead": overhead,
        "received_count": sum(item["summary"]["count"] for item in windows),
        "error_count": sum(len(item["errors"]) for item in windows),
        "capability_note": "A 基线 L1 仅有 recv_return seam；recv_begin/enqueue/dequeue/wait/assist 等字段缺失，不填零。",
    }
    (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    lines = ["# D03 应用接收链 L0/L1 对照", "",
             "L1 开启接收应用缝并有观测成本；每个场景/模式先合并两个窗口的全部订阅者原始样本，再计算总体分位数。",
             "基线 A 的能力矩阵只有 `recv_return`，内部 receiver/入队/get 等字段报告为未采样，不当作零。", "",
             "## L0/L1 端到端", "", "| 场景 | 模式 | 层 | 样本数 | p50 µs | p95 µs | p99 µs | 均值 µs | 窗口 p50 中位数 | 窗口 p99 中位数 |",
             "|---|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for item in aggregate:
        e = item["e2e"]
        lines.append("| {}/{}B | {} | {} | {} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} |".format(
            item["scenario"][0], item["scenario"][1], item["mode"], item["observation"], e["count"],
            e["p50_us"], e["p95_us"], e["p99_us"], e["mean_us"], item["window_p50_us"], item["window_p99_us"]))
    lines += ["", "## L1−L0 观测开销对照", "", "| 场景 | 模式 | p50 差值 µs | p99 差值 µs | 均值差值 µs |", "|---|---|---:|---:|---:|"]
    for item in overhead:
        lines.append("| {}/{}B | {} | {:+.3f} | {:+.3f} | {:+.3f} |".format(
            item["scenario"][0], item["scenario"][1], item["mode"], item["p50_delta_us"], item["p99_delta_us"], item["mean_delta_us"]))
    lines += ["", "## L1 分段（合并原始消息）", "", "| 场景 | 模式 | 分段 | p50 µs | p95 µs | p99 µs | 慢 1% 均值 µs |", "|---|---|---|---:|---:|---:|---:|"]
    for item in aggregate:
        if item["observation"] != "L1":
            continue
        for name, value in item["stages"].items():
            slow = [window["summary"]["slowest_one_percent"]["stage_mean_us"].get(name)
                    for window in windows if (window["subscribers"], window["bytes"], window["mode"], window["observation"]) == (*item["scenario"], item["mode"], "L1")]
            slow_value = statistics.mean(slow) if slow else None
            lines.append("| {}/{}B | {} | {} | {:.3f} | {:.3f} | {:.3f} | {} |".format(
                item["scenario"][0], item["scenario"][1], item["mode"], name,
                value["p50_us"], value["p95_us"], value["p99_us"], f"{slow_value:.3f}" if slow_value is not None else "未采样"))
    lines += ["", f"窗口：{len(windows)}/16；接收：{summary['received_count']}；错误：{summary['error_count']}。", "D03 是定位诊断，不替代 L0 正式验收。"]
    (root / "summary.md").write_text("\n".join(lines) + "\n")
    print(json.dumps({key: summary[key] for key in ("received_count", "error_count")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
