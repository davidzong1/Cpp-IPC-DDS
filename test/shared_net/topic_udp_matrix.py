#!/usr/bin/env python3
"""按话题 UDP 通道方案运行冻结计划的校验、冒烟和正式窗口。

N01 先提供可独立验证的计划/统计工装。生产网关参数通过 argv 数组传递，
不会猜测旧 performance_matrix.py 的固定参数；正式窗口 runner 在后续节点接入。
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


SCHEMA_VERSION = 1
REQUIRED_CASE_KEYS = {"id", "data_shards", "payload_bytes", "rate_hz", "delivery",
                      "warmup_s", "duration_s", "repetitions"}
VALID_DELIVERY = {"best_effort", "reliable"}


def nearest_rank(values, percentile):
    """返回冻结的 nearest-rank 分位数，不插值。"""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(len(ordered) * percentile / 100.0))
    return ordered[rank - 1]


def summary(values):
    if not values:
        return {"count": 0, "mean_ns": None, "p50_ns": None, "p95_ns": None,
                "p99_ns": None, "max_ns": None}
    return {"count": len(values), "mean_ns": sum(values) / len(values),
            "p50_ns": nearest_rank(values, 50), "p95_ns": nearest_rank(values, 95),
            "p99_ns": nearest_rank(values, 99), "max_ns": max(values)}


def overlap(left, right):
    """计算两个 [start,end) 调用区间是否真实重叠。"""
    return max(0, min(left[1], right[1]) - max(left[0], right[0]))


def validate_plan(plan):
    errors = []
    if not isinstance(plan, dict):
        return ["计划必须是 JSON 对象"]
    if plan.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version 必须为 {SCHEMA_VERSION}")
    for key in ("source", "host", "gateway", "routes", "publishers", "subscribers",
                "cases", "thresholds", "case_order"):
        if key not in plan:
            errors.append(f"缺少顶层字段: {key}")
    source = plan.get("source", {})
    for key in ("revision", "binary", "gateway", "library"):
        if not isinstance(source.get(key), str) or not source[key]:
            errors.append(f"source.{key} 必须为非空字符串")
    gateway = plan.get("gateway", {})
    if gateway.get("network_version") not in (1, 2):
        errors.append("gateway.network_version 必须为 1 或 2")
    if gateway.get("data_mode") not in ("pooled", "hybrid", "per-topic"):
        errors.append("gateway.data_mode 无效")
    shards = gateway.get("data_shards")
    workers = gateway.get("data_workers")
    if not isinstance(shards, int) or shards < 1:
        errors.append("gateway.data_shards 必须为正整数")
    if not isinstance(workers, int) or workers < 1:
        errors.append("gateway.data_workers 必须为正整数")
    if isinstance(shards, int) and isinstance(workers, int) and workers > plan.get("host", {}).get("cpu_budget", workers):
        errors.append("data_workers 超过 host.cpu_budget")
    if gateway.get("network_version") == 1 and gateway.get("data_mode") != "pooled":
        errors.append("网络 v1 只允许 pooled")
    routes = plan.get("routes", [])
    if not isinstance(routes, list) or not routes:
        errors.append("routes 必须为非空数组")
    else:
        seen_routes = set()
        for index, route in enumerate(routes):
            if not isinstance(route, dict):
                errors.append(f"routes[{index}] 必须为对象")
                continue
            identity = (route.get("topic"), str(route.get("domain")), route.get("msg_id"))
            if identity in seen_routes:
                errors.append(f"重复 RouteKey: {identity}")
            seen_routes.add(identity)
            if not isinstance(route.get("topic"), str) or not route["topic"]:
                errors.append(f"routes[{index}].topic 无效")
            if not isinstance(route.get("msg_id"), int) or route["msg_id"] < 0 or route["msg_id"] > 0xffffffff:
                errors.append(f"routes[{index}].msg_id 无效")
            if route.get("endpoint") not in ("pooled", "dedicated"):
                errors.append(f"routes[{index}].endpoint 无效")
            if gateway.get("data_mode") == "pooled" and route.get("endpoint") != "pooled":
                errors.append("pooled 模式禁止 dedicated RouteKey")
            if gateway.get("data_mode") == "per-topic" and route.get("endpoint") != "dedicated":
                errors.append("per-topic 模式要求 dedicated RouteKey")
    cases = plan.get("cases", [])
    ids = []
    if not isinstance(cases, list) or not cases:
        errors.append("cases 必须为非空数组")
    else:
        for index, case in enumerate(cases):
            if not isinstance(case, dict):
                errors.append(f"cases[{index}] 必须为对象")
                continue
            ids.append(case.get("id"))
            missing = REQUIRED_CASE_KEYS - set(case)
            errors.extend(f"cases[{index}] 缺少字段: {key}" for key in sorted(missing))
            if case.get("delivery") not in VALID_DELIVERY:
                errors.append(f"cases[{index}].delivery 无效")
            for key in ("data_shards", "payload_bytes", "rate_hz", "warmup_s", "duration_s", "repetitions"):
                value = case.get(key)
                if not isinstance(value, (int, float)) or value <= 0:
                    errors.append(f"cases[{index}].{key} 必须为正数")
            if isinstance(case.get("data_shards"), int) and isinstance(shards, int) and case["data_shards"] != shards:
                errors.append(f"cases[{index}].data_shards 与 gateway.data_shards 不一致")
    if len(ids) != len(set(ids)):
        errors.append("case id 必须唯一")
    if plan.get("case_order") != ids:
        errors.append("case_order 必须与 cases 顺序一致")
    publishers = plan.get("publishers", {})
    if publishers.get("count") not in (1, 2, 8):
        errors.append("publishers.count 当前只支持 1/2/8")
    if publishers.get("mode") not in ("round_robin", "concurrent"):
        errors.append("publishers.mode 必须为 round_robin 或 concurrent")
    if publishers.get("processes", 0) < 1:
        errors.append("publishers.processes 必须为正数")
    if publishers.get("count", 0) > 1 and publishers.get("processes", 0) < publishers["count"]:
        errors.append("多进程发布者测试的 processes 必须覆盖每个发布者")
    subscribers = plan.get("subscribers", {})
    if subscribers.get("count", 0) < 1 or subscribers.get("processes", 0) < 1:
        errors.append("subscribers.count/processes 必须为正数")
    thresholds = plan.get("thresholds", {})
    if thresholds.get("nearest_rank") is not True:
        errors.append("thresholds.nearest_rank 必须显式为 true")
    return errors


def file_sha256(path):
    p = Path(path)
    if not p.is_file():
        return None
    digest = hashlib.sha256()
    with p.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_preflight(plan, output):
    output.mkdir(parents=True, exist_ok=False)
    binary = plan["source"]["binary"]
    gateway = plan["source"]["gateway"]
    record = {"kind": "preflight", "formal": False, "started_ns": time.monotonic_ns(),
              "binary_sha256": file_sha256(binary), "gateway_sha256": file_sha256(gateway),
              "plan_revision": plan["source"]["revision"], "checks": []}
    for path, label in ((binary, "binary"), (gateway, "gateway")):
        if not Path(path).is_file():
            record["checks"].append({"name": label, "ok": False, "error": "文件不存在"})
        else:
            record["checks"].append({"name": label, "ok": True})
    record["checks"].append({"name": "publisher_schedule", "ok": True,
                              "mode": plan["publishers"]["mode"],
                              "publisher_count": plan["publishers"]["count"]})
    record["finished_ns"] = time.monotonic_ns()
    record["ok"] = all(item["ok"] for item in record["checks"])
    (output / "preflight.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n")
    return 0 if record["ok"] else 1


def main():
    parser = argparse.ArgumentParser(description="按话题 UDP 数据通道冻结计划运行 N01 工装")
    parser.add_argument("--plan", required=True)
    parser.add_argument("--output")
    parser.add_argument("--validate-plan", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--diagnostics", action="store_true")
    args = parser.parse_args()
    plan = json.loads(Path(args.plan).read_text())
    errors = validate_plan(plan)
    if args.validate_plan or not args.output:
        print(json.dumps({"valid": not errors, "errors": errors}, ensure_ascii=False, indent=2))
        if errors or args.validate_plan:
            return 1 if errors else 0
    if not args.output:
        parser.error("正式运行需要 --output")
    output = Path(args.output)
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("证据目录必须为空，禁止混合或覆盖旧轮次")
    if args.preflight:
        return run_preflight(plan, output)
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema_version": SCHEMA_VERSION, "formal": not args.diagnostics,
                "diagnostics": args.diagnostics, "plan": str(Path(args.plan).resolve()),
                "plan_sha256": file_sha256(args.plan), "source": plan["source"],
                "cases": plan["case_order"], "started_ns": time.monotonic_ns(),
                "status": "not_implemented"}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": "not_implemented", "message": "N01 仅完成计划校验和 preflight；正式窗口待 N02 接入网关参数", "formal": manifest["formal"]}, ensure_ascii=False))
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, json.JSONDecodeError, RuntimeError) as error:
        print(json.dumps({"status": "infrastructure_error", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        sys.exit(3)
