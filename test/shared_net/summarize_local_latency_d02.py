#!/usr/bin/env python3
"""汇总 D02 ABBA|BAAB 窗口，并按全部订阅者原始 CSV 计算总体分位数。"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


PAIR_POSITIONS = ((1, 2), (3, 4), (5, 6), (7, 8))
METRICS = ("e2e", "publish")
PERCENTILES = ("p50_us", "p95_us", "p99_us")


def nearest_rank(values, percentile):
    values = sorted(values)
    if not values:
        return None
    rank = max(1, math.ceil(len(values) * percentile / 100))
    return values[rank - 1]


def stats(values):
    values = list(values)
    return {
        "count": len(values),
        "mean_us": (sum(values) / len(values) / 1000) if values else None,
        **{f"p{p}_us": (nearest_rank(values, p) / 1000 if values else None)
           for p in (50, 95, 99)},
    }


def read_csv(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def process_ambient(environment_path):
    if not environment_path.exists():
        return {"cpu_seconds": 0.0, "processes": [], "roots": []}
    data = json.loads(environment_path.read_text())
    activity = data.get("ambient_activity", [])
    by_process = defaultdict(float)
    for item in activity:
        key = (item.get("pid"), item.get("start_ticks"), item.get("comm"), item.get("cwd"))
        by_process[key] += float(item.get("cpu_seconds", 0))
    processes = [
        {"pid": key[0], "start_ticks": key[1], "comm": key[2], "cwd": key[3],
         "cpu_seconds": value}
        for key, value in by_process.items()
    ]
    processes.sort(key=lambda item: (-item["cpu_seconds"], item["pid"] or -1))
    return {
        "cpu_seconds": sum(item["cpu_seconds"] for item in processes),
        "processes": processes,
        "roots": data.get("ambient_roots", []),
        "observed_competitors": data.get("observed_competitors", []),
        "system_busy_ticks_delta": data.get("system_busy_ticks_delta"),
    }


def validate_window(entry, root):
    window = entry["window"]
    folder = root / window["id"]
    expected = int(window["seconds"]) * int(window["rate_hz"])
    errors = []
    pub_rows = read_csv(folder / "pub.csv")
    published = {
        int(row["sequence"]): row for row in pub_rows if row.get("success") == "1"
    }
    if len(pub_rows) != expected or len(published) != expected:
        errors.append(f"发布CSV数量 {len(pub_rows)}/{len(published)}，预期 {expected}")
    if len({int(row["sequence"]) for row in pub_rows}) != len(pub_rows):
        errors.append("发布序号重复")
    publish_values = [int(row["elapsed_ns"]) for row in pub_rows if row.get("success") == "1"]
    receive_values = []
    subscriber_stats = []
    subscriber_paths = sorted(folder.glob("sub*.csv"),
                              key=lambda path: int(path.stem[3:]))
    if len(subscriber_paths) != int(window["subscribers"]):
        errors.append(f"订阅CSV数量 {len(subscriber_paths)}，预期 {window['subscribers']}")
    for path in subscriber_paths:
        rows = read_csv(path)
        values = []
        sequences = []
        for row in rows:
            sequence = int(row["sequence"])
            elapsed = int(row["elapsed_ns"])
            sequences.append(sequence)
            values.append(elapsed)
            if sequence not in published:
                errors.append(f"{path.name} 存在未发布序号 {sequence}")
            else:
                expected_elapsed = int(row["read_ns"]) - int(published[sequence]["start_ns"])
                if expected_elapsed != elapsed:
                    errors.append(f"{path.name} 序号 {sequence} 时间戳不守恒")
        if len(rows) != expected:
            errors.append(f"{path.name} 数量 {len(rows)}，预期 {expected}")
        if len(set(sequences)) != len(sequences):
            errors.append(f"{path.name} 存在重复序号")
        if set(sequences) != set(published):
            errors.append(f"{path.name} 序号集合缺失或多出")
        receive_values.extend(values)
        subscriber_stats.append({"subscriber": path.stem, **stats(values)})

    result = entry.get("result", {})
    if entry.get("exit_code") != 0:
        errors.append(f"运行器退出码 {entry.get('exit_code')}")
    if result.get("publish", {}).get("accepted") != expected:
        errors.append("发布 accepted 不等于预期")
    if result.get("publish", {}).get("rejected"):
        errors.append("发布存在 rejected")
    if any(item for item in entry.get("observed_competitors", [])):
        errors.append("运行器检测到未声明竞争者")
    ambient = process_ambient(folder / "environment.json")
    return {
        "id": window["id"],
        "mode": window["mode"],
        "subscribers": int(window["subscribers"]),
        "bytes": int(window["bytes"]),
        "position": int(window["position"]),
        "pair_block": int(window["pair_block"]),
        "wire_bytes": int(result.get("wire_bytes", 0)),
        "e2e": stats(receive_values),
        "publish": stats(publish_values),
        "subscriber_stats": subscriber_stats,
        "published_count": len(published),
        "received_count": len(receive_values),
        "ambient": ambient,
        "errors": errors,
    }


def subtract_stats(current, baseline):
    return {key: (current[key] - baseline[key] if current[key] is not None and baseline[key] is not None else None)
            for key in ("mean_us", *PERCENTILES)}


def aggregate_windows(windows):
    result = {}
    for mode, window_mode in (("baseline", "A"), ("shared_v1", "B")):
        selected = [item for item in windows if item["mode"] == window_mode]
        result[mode] = {
            "windows": len(selected),
            "e2e": stats(value for item in selected for value in item["_e2e_values"]),
            "publish": stats(value for item in selected for value in item["_publish_values"]),
            "window_p50_us": statistics.median(item["e2e"]["p50_us"] for item in selected),
            "window_p99_us": statistics.median(item["e2e"]["p99_us"] for item in selected),
        }
    return result


def make_markdown(summary):
    lines = [
        "# D02 本机 L0 ABBA|BAAB 汇总", "",
        "每个窗口的端到端分位数均由该窗口全部订阅者原始 CSV 合并后计算；没有平均订阅者分位数。",
        "运行器声明的环境根为 `/home/zwc`，因此该目录下（含用户声明的 `/home/zwc/MPC_GPU`）活动只记录、不触发中止。",
        "", "## 场景总体（四个同模式窗口的原始样本池）", "",
        "| 场景 | 模式 | 样本数 | p50 µs | p95 µs | p99 µs | 均值 µs | 窗口 p50 中位数 | 窗口 p99 中位数 |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario in summary["scenarios"]:
        label = f"{scenario['subscribers']} SUB / {scenario['bytes']}B"
        for mode in ("baseline", "shared_v1"):
            value = scenario["pooled"][mode]
            e2e = value["e2e"]
            lines.append("| {} | {} | {} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} |".format(
                label, mode, e2e["count"], e2e["p50_us"], e2e["p95_us"], e2e["p99_us"],
                e2e["mean_us"], value["window_p50_us"], value["window_p99_us"]))
    lines += ["", "## 相邻配对的 B−A（端到端）", "",
              "| 场景 | 配对位置 | 顺序 | p50 µs | p95 µs | p99 µs | 均值 µs |",
              "|---|---:|---|---:|---:|---:|---:|"]
    for pair in summary["pairs"]:
        delta = pair["e2e_delta_us"]
        lines.append("| {} | {} | {} | {:+.3f} | {:+.3f} | {:+.3f} | {:+.3f} |".format(
            f"{pair['subscribers']} SUB / {pair['bytes']}B",
            f"{pair['positions'][0]}→{pair['positions'][1]}", pair["order"],
            delta["p50_us"], delta["p95_us"], delta["p99_us"], delta["mean_us"]))
    lines += ["", "## 逐窗完整性", "",
              "| 窗口 | 模式 | 订阅者 | 合并接收数 | p50 µs | p99 µs | 环境 CPU 秒 | 状态 |",
              "|---|---|---:|---:|---:|---:|---:|---|"]
    for item in summary["windows"]:
        lines.append("| {} | {} | {} | {} | {:.3f} | {:.3f} | {:.3f} | {} |".format(
            item["id"], item["mode"], item["subscribers"], item["received_count"],
            item["e2e"]["p50_us"], item["e2e"]["p99_us"], item["ambient"]["cpu_seconds"],
            "通过" if not item["errors"] else "；".join(item["errors"])))
    lines += ["", f"窗口：{len(summary['windows'])}/24；接收：{summary['received_count']}；错误：{summary['error_count']}。",
              f"环境活动记录 CPU 总量：{summary['ambient_cpu_seconds']:.3f} 秒；未声明竞争者窗口数：{summary['undeclared_competitor_windows']}",
              "这些是 D02 的定位对照结果，不替代 D12 正式 54 窗验收。"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    entries = json.loads((root / "results.json").read_text())
    if len(entries) != 24:
        raise ValueError(f"D02 结果窗口数为 {len(entries)}，预期 24")
    windows = []
    for entry in entries:
        item = validate_window(entry, root)
        item["_e2e_values"] = []
        item["_publish_values"] = []
        folder = root / item["id"]
        item["_e2e_values"] = [int(row["elapsed_ns"]) for path in sorted(folder.glob("sub*.csv"), key=lambda p: int(p.stem[3:])) for row in read_csv(path)]
        item["_publish_values"] = [int(row["elapsed_ns"]) for row in read_csv(folder / "pub.csv") if row.get("success") == "1"]
        windows.append(item)

    scenarios = []
    for key in sorted({(item["subscribers"], item["bytes"]) for item in windows}):
        selected = [item for item in windows if (item["subscribers"], item["bytes"]) == key]
        pooled = aggregate_windows(selected)
        scenarios.append({"subscribers": key[0], "bytes": key[1], "wire_bytes": selected[0]["wire_bytes"], "pooled": pooled})

    by_key = {(item["subscribers"], item["bytes"], item["position"]): item for item in windows}
    pairs = []
    for scenario in scenarios:
        key = (scenario["subscribers"], scenario["bytes"])
        for left, right in PAIR_POSITIONS:
            first = by_key[key + (left,)]
            second = by_key[key + (right,)]
            if first["mode"] == "A":
                a, b, order = first, second, "AB"
            else:
                a, b, order = second, first, "BA"
            pairs.append({"subscribers": key[0], "bytes": key[1], "positions": (left, right), "order": order,
                          "e2e_delta_us": subtract_stats(b["e2e"], a["e2e"]),
                          "publish_delta_us": subtract_stats(b["publish"], a["publish"]),
                          "baseline": a["id"], "shared_v1": b["id"]})

    ambient_cpu = sum(item["ambient"]["cpu_seconds"] for item in windows)
    summary = {
        "schema": "local-latency-d02-summary/1",
        "source": str(root),
        "windows": [{key: value for key, value in item.items() if not key.startswith("_")} for item in windows],
        "scenarios": scenarios,
        "pairs": pairs,
        "received_count": sum(item["received_count"] for item in windows),
        "error_count": sum(len(item["errors"]) for item in windows),
        "ambient_cpu_seconds": ambient_cpu,
        "undeclared_competitor_windows": sum(bool(item["ambient"].get("observed_competitors")) for item in windows),
    }
    (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    (root / "summary.md").write_text(make_markdown(summary))
    print(json.dumps({key: summary[key] for key in ("received_count", "error_count", "ambient_cpu_seconds", "undeclared_competitor_windows")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
