#!/usr/bin/env python3
"""Correlate bounded shared-net message traces and summarize slow-message stages."""
import argparse
import json
import math
from pathlib import Path


def nearest_rank(values, percentile):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * percentile / 100) - 1)]


def summarize(values):
    if not values:
        return {"count": 0, "mean_ns": None, "p50_ns": None, "p95_ns": None,
                "p99_ns": None, "max_ns": None}
    return {"count": len(values), "mean_ns": sum(values) / len(values),
            "p50_ns": nearest_rank(values, 50), "p95_ns": nearest_rank(values, 95),
            "p99_ns": nearest_rank(values, 99), "max_ns": max(values)}


def trace_pages(value):
    if not value:
        return []
    if isinstance(value, dict) and "pages" in value:
        return value["pages"]
    return []


def load_records(run):
    merged = {}
    trace_sources = []
    trace_drops = {}
    for host_index in (0, 1):
        metrics = run.get(f"host{host_index}_metrics", {})
        sources = [(f"host{host_index}-gateway", metrics.get("trace"))]
        sources.extend((f"host{host_index}-app{index}", value)
                       for index, value in enumerate(metrics.get("application_traces", [])))
        for source, tree in sources:
            pages = trace_pages(tree)
            if not pages:
                continue
            trace_sources.append(source)
            first_page = pages[0]
            trace_drops[source] = first_page.get("dropped", 0)
            for page in pages:
                for row in page.get("records", []):
                    key = (str(row.get("gateway_epoch")), row.get("publisher_id"),
                           str(row.get("sequence")), row.get("route_scope"), row.get("msg_id"))
                    record = merged.setdefault(key, {"key": key, "stamps": {}, "sessions": set(), "nodes": set(), "reliable": False})
                    record["nodes"].add(first_page.get("node", "unknown"))
                    record["reliable"] = record["reliable"] or row.get("reliable", False)
                    if row.get("session_known") and row.get("session_id") is not None:
                        record["sessions"].add(str(row["session_id"]))
                    for point, stamp in row.get("stamps_ns", {}).items():
                        if stamp:
                            record["stamps"].setdefault(point, int(stamp))
    return list(merged.values()), trace_sources, trace_drops


SEGMENTS = {
    "publish_before_outbox": ("publish_begin", "outbox_submit_begin"),
    "outbox_submit_exclusive": ("outbox_submit_begin", "outbox_submit_end"),
    "credit_wait": ("credit_wait_begin", "credit_wait_end"),
    "outbox_shm_residence": ("outbox_submit_end", "drain_pulled"),
    "drain_pull_to_event_queue": ("drain_pulled", "drain_event_queued"),
    "control_event_queue": ("drain_event_queued", "control_dequeued"),
    "control_processing": ("control_dequeued", "data_queue_submitted"),
    "data_worker_queue": ("data_queue_submitted", "worker_dequeued"),
    "worker_to_first_send": ("worker_dequeued", "first_send"),
    "network_send_span": ("first_send", "last_send"),
    "remote_receive_commit": ("receive_first", "receive_commit"),
    "ack_queue_to_send": ("ack_queued", "ack_sent"),
    "ack_receive_to_gateway_result": ("ack_received", "reliable_result_queued"),
    "gateway_result_to_api_return": ("reliable_result_queued", "publish_api_return"),
}


def queue_peaks(run):
    peaks = []
    for host_index in (0, 1):
        metrics = run.get(f"host{host_index}_metrics", {})
        for shard in metrics.get("shards", {}).get("shards", []):
            peaks.append({"host": host_index, "worker": shard.get("index"),
                          "queued_peak": shard.get("queued_peak", 0)})
    return peaks


def api_overlap(records):
    events = []
    for record in records:
        stamps = record["stamps"]
        begin, end = stamps.get("publish_begin"), stamps.get("publish_api_return")
        if begin and end and end >= begin:
            key = (record["key"][1], record["key"][2])
            events.append((begin, 1, key))
            events.append((end, -1, key))
    events.sort(key=lambda event: (event[0], event[1]))
    active = set()
    overlapping = set()
    max_inflight = 0
    for _, delta, key in events:
        if delta < 0:
            active.discard(key)
            continue
        if active:
            overlapping.add(key)
            overlapping.update(active)
        active.add(key)
        max_inflight = max(max_inflight, len(active))
    return {"calls": max_inflight and len(events) // 2 or 0,
            "calls_with_overlap": len(overlapping), "max_inflight": max_inflight}


def analyze_run(run):
    case = run.get("case", {})
    records, sources, drops = load_records(run)
    required = ("publish_begin", "outbox_submit_begin", "outbox_submit_end", "drain_pulled",
                "drain_event_queued", "control_dequeued", "data_queue_submitted",
                "worker_dequeued", "first_send", "last_send", "receive_first",
                "receive_commit", "ack_queued", "ack_sent", "ack_received",
                "reliable_result_queued", "reliable_completed", "publish_api_return")
    completed = []
    for record in records:
        stamps = record["stamps"]
        begin, end = stamps.get("publish_begin"), stamps.get("publish_api_return")
        if begin and end and end >= begin:
            record["api_elapsed_ns"] = end - begin
            record["chain_complete"] = record["reliable"] and all(stamps.get(point) for point in required)
            completed.append(record)
    completed.sort(key=lambda item: item["api_elapsed_ns"])
    top_count = max(1, math.ceil(len(completed) * 0.01)) if completed else 0
    slowest = completed[-top_count:] if top_count else []
    slowest_complete = sum(record.get("chain_complete", False) for record in slowest)
    segment_values = {}
    slow_shares = {}
    for name, (begin_name, end_name) in SEGMENTS.items():
        values = []
        for record in records:
            stamps = record["stamps"]
            begin, end = stamps.get(begin_name), stamps.get(end_name)
            if begin and end and end >= begin:
                duration = end - begin
                if name == "outbox_submit_exclusive":
                    credit_begin, credit_end = stamps.get("credit_wait_begin"), stamps.get("credit_wait_end")
                    if credit_begin and credit_end and credit_end >= credit_begin:
                        duration = max(0, duration - (credit_end - credit_begin))
                values.append(duration)
        segment_values[name] = summarize(values)
        durations = []
        elapsed = []
        for record in slowest:
            stamps = record["stamps"]
            begin, end = stamps.get(begin_name), stamps.get(end_name)
            if begin and end and end >= begin:
                duration = end - begin
                if name == "outbox_submit_exclusive":
                    credit_begin, credit_end = stamps.get("credit_wait_begin"), stamps.get("credit_wait_end")
                    if credit_begin and credit_end and credit_end >= credit_begin:
                        duration = max(0, duration - (credit_end - credit_begin))
                durations.append(duration)
                elapsed.append(record["api_elapsed_ns"])
        slow_shares[name] = {
            "matched": len(durations),
            "mean_share_pct": (sum(d / e for d, e in zip(durations, elapsed)) * 100 / len(durations)) if durations else None,
            "cumulative_share_pct": (sum(durations) * 100 / sum(elapsed)) if durations and sum(elapsed) else None,
        }
    complete_chains = sum(record.get("chain_complete", False) for record in completed)
    send_samples = [int(sample) for result in run.get("send_results", [])
                    for sample in result.get("latency_samples_ns", [])]
    return {
        "case": case, "round": case.get("round"), "trace_enabled": run.get("trace_enabled"),
        "passed_content": run.get("passed_content"), "failure": run.get("failure"),
        "send_results": run.get("send_results", []), "receives": run.get("receives", []),
        "trace_sources": sources, "trace_drops": drops,
        "record_count": len(records), "publish_return_samples": len(completed),
        "complete_reliable_chains": complete_chains,
        "api_latency": summarize([record["api_elapsed_ns"] for record in completed]),
        "probe_api_latency": summarize(send_samples),
        "segments": segment_values, "slowest_1_percent_count": top_count,
        "slowest_1_percent_complete_chains": slowest_complete,
        "slowest_1_percent_stage_shares": slow_shares,
        "api_overlap": api_overlap(records),
        "queue_peaks": queue_peaks(run),
        "ambiguous_known_sessions": sum(len(record["sessions"]) > 1 for record in records),
        "clock_domain": "same-host CLOCK_MONOTONIC; not transferable to physical cross-host one-way latency",
    }


def n10_gate(summaries):
    candidates = ("outbox_shm_residence", "control_event_queue", "control_processing",
                  "data_worker_queue", "ack_receive_to_gateway_result")
    by_case = {}
    for result in summaries:
        if not result.get("trace_enabled") or not result.get("passed_content"):
            continue
        case = result.get("case", {})
        group = (case.get("configuration"), case.get("topology"))
        by_case.setdefault(group, []).append(result)
    decisions = []
    for group, rounds in by_case.items():
        rounds.sort(key=lambda item: item.get("round", 0))
        if len(rounds) < 2:
            continue
        left, right = rounds[:2]
        usable = all(
            not round_result.get("ambiguous_known_sessions", 0) and
            not any(round_result.get("trace_drops", {}).values()) and
            round_result.get("slowest_1_percent_count", 0) > 0 and
            round_result.get("slowest_1_percent_complete_chains", 0) ==
            round_result.get("slowest_1_percent_count")
            for round_result in (left, right))
        if not usable:
            continue
        for segment in candidates:
            lshare = left["slowest_1_percent_stage_shares"][segment]["cumulative_share_pct"]
            rshare = right["slowest_1_percent_stage_shares"][segment]["cumulative_share_pct"]
            lmatched = left["slowest_1_percent_stage_shares"][segment]["matched"]
            rmatched = right["slowest_1_percent_stage_shares"][segment]["matched"]
            if (lshare is not None and rshare is not None and lshare >= 20 and rshare >= 20 and
                    lmatched == left["slowest_1_percent_count"] and
                    rmatched == right["slowest_1_percent_count"]):
                decisions.append({"configuration": group[0], "topology": group[1], "stage": segment,
                                  "round_shares_pct": [lshare, rshare],
                                  "matched_slowest_samples": [lmatched, rmatched],
                                  "meets_threshold": True})
    excluded = []
    for result in summaries:
        if not result.get("trace_enabled"):
            continue
        reasons = []
        if not result.get("passed_content"):
            reasons.append("content_check_failed")
        if any(result.get("trace_drops", {}).values()):
            reasons.append("trace_records_dropped")
        if result.get("ambiguous_known_sessions", 0):
            reasons.append("ambiguous_session")
        if result.get("slowest_1_percent_complete_chains", 0) != result.get("slowest_1_percent_count", 0):
            reasons.append("incomplete_slowest_trace_chain")
        if reasons:
            case = result.get("case", {})
            excluded.append({"configuration": case.get("configuration"),
                             "topology": case.get("topology"),
                             "round": result.get("round"), "reasons": reasons})
    return {"entered": bool(decisions), "candidate_stages": decisions,
            "excluded_rounds": excluded,
            "basis": "只接受内容校验通过、trace 零丢弃、无 session 歧义且最慢1%可靠消息均有完整链路的轮次；同一阶段须在两个连续轮次占最慢1%累计耗时至少20%，并且慢样本阶段记录全覆盖。队列等待阶段按实测等待判定，控制处理阶段按串行处理耗时判定。"}


def trace_overhead_pairs(summaries):
    by_case = {}
    for result in summaries:
        case = result.get("case", {})
        if (case.get("configuration") != "v2-per-topic-W4" or
                case.get("topology") != "same_topic_multipublisher"):
            continue
        key = (case.get("configuration"), case.get("topology"), result.get("round"))
        by_case.setdefault(key, {})[bool(result.get("trace_enabled"))] = result
    pairs = []
    for (configuration, topology, round_number), values in sorted(by_case.items()):
        traced, untraced = values.get(True), values.get(False)
        if not traced or not untraced:
            pairs.append({"configuration": configuration, "topology": topology,
                          "round": round_number, "complete_pair": False})
            continue
        if not traced.get("passed_content") or not untraced.get("passed_content"):
            pairs.append({"configuration": configuration, "topology": topology,
                          "round": round_number, "complete_pair": False,
                          "failure": "trace-on 或 trace-off 内容校验失败"})
            continue
        traced_stats = traced.get("probe_api_latency", {})
        untraced_stats = untraced.get("probe_api_latency", {})
        delta = {}
        for field in ("mean_ns", "p50_ns", "p95_ns", "p99_ns"):
            left, right = traced_stats.get(field), untraced_stats.get(field)
            delta[field] = None if left is None or right is None else left - right
        pairs.append({"configuration": configuration, "topology": topology,
                      "round": round_number, "complete_pair": True,
                      "trace_on": traced_stats, "trace_off": untraced_stats,
                      "delta_ns": delta})
    return pairs


def write_report(root, run_summaries, gate):
    lines = ["# N09 出站与 ACK 排队归因", "", "诊断记录按 `(gateway_epoch, publisher_id, sequence, RouteKey)` 合并；源应用和源网关保留 session，接收网关将不可见 session 标为未知。所有样本来自同机隔离 IPC 命名空间和真实 loopback UDP，时钟域相同；不构成物理跨机单向延迟证据。", "",
             "| 配置 | 布置 | 轮次 | trace 消息 | 完整可靠链 | API p99 (us) | 重叠/最大在途 | 丢弃 trace | 发送/接收 |", "|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for result in run_summaries:
        case = result.get("case", {})
        send_count = sum(item.get("sent", 0) for item in result.get("send_results", []))
        receive_count = sum(item.get("received", 0) for item in result.get("receives", []))
        p99 = result["api_latency"].get("p99_ns")
        p99_us = "n/a" if p99 is None else f"{p99 / 1000:.2f}"
        drops = sum(result.get("trace_drops", {}).values())
        overlap = result.get("api_overlap", {})
        lines.append(f"| {case.get('configuration')} | {case.get('topology')} | {result.get('round')} | {result.get('record_count')} | {result.get('complete_reliable_chains')} | {p99_us} | {overlap.get('calls_with_overlap', 0)}/{overlap.get('max_inflight', 0)} | {drops} | {send_count}/{receive_count} |")
    lines.extend(["", "## 打点开销配对", "", "| 配置 | 布置 | 轮次 | 无打点/有打点样本 | 均值差 (us) | p99 差 (us) |", "|---|---|---:|---:|---:|---:|"])
    for pair in trace_overhead_pairs(run_summaries):
        delta = pair.get("delta_ns", {})
        mean_delta = "n/a" if delta.get("mean_ns") is None else f"{delta['mean_ns'] / 1000:.2f}"
        p99_delta = "n/a" if delta.get("p99_ns") is None else f"{delta['p99_ns'] / 1000:.2f}"
        left_count = pair.get("trace_on", {}).get("count", 0)
        right_count = pair.get("trace_off", {}).get("count", 0)
        lines.append(f"| {pair.get('configuration')} | {pair.get('topology')} | {pair.get('round')} | {right_count}/{left_count} | {mean_delta} | {p99_delta} |")
    lines.extend(["", f"N10 判据：**{'满足，进入对应 owner 设计' if gate['entered'] else '不满足，保留现有单 drain 与控制循环'}**。", ""])
    if gate["candidate_stages"]:
        for decision in gate["candidate_stages"]:
            lines.append(f"- `{decision['configuration']}` / `{decision['topology']}` 的 `{decision['stage']}` 在两轮分别占最慢 1% API 总耗时 {decision['round_shares_pct'][0]:.1f}% 与 {decision['round_shares_pct'][1]:.1f}%。")
    else:
        lines.append("没有同一排队阶段在两个合格连续轮次中都达到 20%；本轮不因单次尾样本改造出站 owner。原始 JSON 中保留每条消息阶段、worker 队列峰值、trace 丢弃计数和接收完整性。")
        for excluded in gate.get("excluded_rounds", []):
            lines.append(f"- 第 {excluded['round']} 轮 `{excluded['configuration']}` / `{excluded['topology']}` 排除：{', '.join(excluded['reasons'])}。")
    failed = [item for item in run_summaries if not item.get("passed_content")]
    if failed:
        lines.extend(["", "失败轮次已保留，且不参与 N10 判据："])
        for item in failed:
            case = item.get("case", {})
            lines.append(f"- `{case.get('configuration')}` / `{case.get('topology')}` / 第 {item.get('round')} 轮：{item.get('failure') or '内容校验失败'}")
    lines.extend(["", "诊断开销配对结果与所有原始分页记录见 `runs/` 和 `queue-attribution.json`。本短窗口用于 N10 选点，不替代 N13 的 3×60 秒正式性能验收。", ""])
    (root / "queue-attribution.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="queue diagnostic 根目录")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(args.input)
    summaries = [analyze_run(json.loads(path.read_text())) for path in sorted((root / "runs").glob("*.json"))]
    gate = n10_gate(summaries)
    output = {"runs": summaries, "n10_decision": gate}
    Path(args.output).write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n")
    write_report(root, summaries, gate)
    print(json.dumps({"runs": len(summaries), "n10_entered": gate["entered"], "output": args.output}, ensure_ascii=False))


if __name__ == "__main__":
    main()
