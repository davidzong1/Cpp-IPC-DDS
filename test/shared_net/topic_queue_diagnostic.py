#!/usr/bin/env python3
"""Run fixed N09 cross-gateway queue-attribution cases serially."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

from end_to_end import Process, reserve_ports


SCENARIOS = (
    {"name": "same_process_multitopic", "source_roles": ["pubset"],
     "source_topic": "queue/a,queue/b", "receiver_roles": ["sub:queue/a", "sub:queue/b"],
     "receive_roles": ["sub:queue/a", "sub:queue/b"],
     "receiver_topic": "queue/a,queue/b", "source_route_topics": ["queue/a", "queue/b"]},
    {"name": "multiprocess_multitopic", "source_roles": ["pub:queue/a", "pub:queue/b"],
     "source_topic": "queue/a", "receiver_roles": ["sub:queue/a", "sub:queue/b"],
     "receive_roles": ["sub:queue/a", "sub:queue/b"],
     "receiver_topic": "queue/a", "source_route_topics": ["queue/a", "queue/b"]},
    {"name": "same_topic_multipublisher", "source_roles": ["pub:queue/shared", "pub:queue/shared"],
     "source_topic": "queue/shared", "receiver_roles": ["sub:queue/shared"],
     "receive_roles": ["sub:queue/shared"],
     "receiver_topic": "queue/shared", "source_route_topics": ["queue/shared"]},
)


def host_command(args, directory, index, base, discovery, configuration, scenario, side, trace):
    control = str(directory / f"host{index}" / "control.sock")
    data_shards = 4
    span = 64
    control_port = base + (data_shards if configuration["version"] == 1 else span)
    command = ["unshare", "--user", "--map-root-user", "--mount", "--ipc",
               sys.executable, str(Path(__file__).with_name("end_to_end.py")), "--host",
               "--gateway", args.gateway, "--probe", args.probe, "--control", control,
               "--base", str(base), "--control-port", str(control_port),
               "--discovery", str(discovery), "--network-version", str(configuration["version"]),
               "--data-mode", configuration["mode"], "--data-workers", "4",
               "--topic", scenario[f"{side}_topic"], "--lifecycle-roles",
               ",".join(scenario[f"{side}_roles"])]
    if configuration["version"] == 1:
        command += ["--data-shards", str(data_shards)]
    else:
        command += ["--data-port-range", f"{base}:{base + span - 1}"]
        if configuration["mode"] != "per-topic":
            command += ["--data-sockets", "4"]
    if trace:
        command.append("--message-trace")
    return command


def source_route_counts(scenario, sends):
    counts = {topic: 0 for topic in scenario["source_route_topics"]}
    if scenario["source_roles"] == ["pubset"]:
        total = sends[0].get("sent", 0) if sends else 0
        for index in range(total):
            counts[scenario["source_route_topics"][index % len(counts)]] += 1
    else:
        for role, result in zip(scenario["source_roles"], sends):
            _, topic = role.split(":", 1)
            counts[topic] += result.get("sent", 0)
    return counts


def publisher_count(scenario):
    if scenario["source_roles"] == ["pubset"]:
        return 1
    return len(scenario["source_roles"])


def run_case(args, case, output_path):
    record = {"case": case, "trace_enabled": case["trace_enabled"], "send_results": [],
              "receives": [], "host0_metrics": {}, "host1_metrics": {}}
    hosts = []
    try:
        with tempfile.TemporaryDirectory(prefix="dzipc-n09-") as temporary:
            directory = Path(temporary)
            (directory / "host0").mkdir(mode=0o700)
            (directory / "host1").mkdir(mode=0o700)
            bases, discovery = reserve_ports(4, 64)
            configuration = {"version": case["network_version"], "mode": case["data_mode"]}
            for index, side in enumerate(("source", "receiver")):
                diagnostic_env = dict(os.environ, DZIPC_TEST_LATENCY_SAMPLES="1")
                process = Process(host_command(args, directory, index, bases[index], discovery,
                                               configuration, case["scenario"], side,
                                               case["trace_enabled"]), env=diagnostic_env, group=True)
                hosts.append(process)
                ready = process.receive(timeout=20)
                if not ready.get("ready"):
                    raise AssertionError({"host": index, "ready": ready})

            def request(host_index, value):
                return hosts[host_index].request(json.dumps(value))

            end = time.monotonic() + 15
            while True:
                states = [request(0, ["probe", index, "state"])
                          for index in range(len(case["scenario"]["source_roles"]))]
                all_routes = [route for state in states for route in state.get("routes", [])]
                if all_routes and all(route.get("synchronized") and route.get("remote") == 1 for route in all_routes):
                    break
                if time.monotonic() >= end:
                    raise AssertionError({"remote_routes_not_ready": states})
                time.sleep(0.05)

            record["resources_before"] = [request(index, ["resources"]) for index in range(2)]
            receive_pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            receive_count = args.rate * args.seconds + 20
            receive_future = receive_pool.submit(
                request, 1, ["recv_all", receive_count, (args.seconds + 5) * 1000])
            # pubset deliberately rotates one process's publishers; the two
            # separate-probe cases provide the true cross-process overlap.
            record["send_results"] = request(0, ["load_all", args.seconds, args.rate,
                                                  args.payload_bytes, 1])
            if not record["send_results"] or any(result.get("failed") for result in record["send_results"]):
                raise AssertionError({"reliable_send_failure": record["send_results"]})
            expected = source_route_counts(case["scenario"], record["send_results"])
            try:
                receive_results = receive_future.result(timeout=args.seconds + 15)
            finally:
                receive_pool.shutdown(wait=True)
            for index, role in enumerate(case["scenario"]["receive_roles"]):
                _, topic = role.split(":", 1)
                samples = receive_results[index].get("received", [])
                result = {"received": len(samples),
                          "invalid": sum(not sample.get("valid") for sample in samples),
                          "duplicates": len(samples) - len({sample.get("seed") for sample in samples}),
                          "samples": samples}
                result["topic"] = topic
                result["expected"] = expected[topic]
                if result["received"] != expected[topic] or result["invalid"] or result["duplicates"]:
                    raise AssertionError({"receive_mismatch": result, "send_results": record["send_results"]})
                record["receives"].append(result)

            record["host0_metrics"] = request(0, ["metrics"])
            record["host1_metrics"] = request(1, ["metrics"])
            record["resources_after"] = [request(index, ["resources"]) for index in range(2)]
            record["passed_content"] = True
            record["clock_domain"] = "same-host CLOCK_MONOTONIC; separate IPC namespaces; actual loopback UDP"
    except Exception as error:
        record["passed_content"] = False
        record["failure"] = str(error)
    finally:
        for process in hosts:
            try:
                if process.p.poll() is None:
                    process.p.stdin.write(json.dumps(["quit"]) + "\n")
                    process.p.stdin.flush()
                    process.p.wait(timeout=12)
            except (OSError, subprocess.TimeoutExpired):
                process.close()
    record["finished_ns"] = time.monotonic_ns()
    output_path.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"case": case, "passed_content": record["passed_content"],
                      "output": str(output_path)}, ensure_ascii=False), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description="N09 分段队列归因诊断矩阵")
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=int, default=4)
    parser.add_argument("--rate", type=int, default=500, help="每发布者的固定速率")
    parser.add_argument("--payload-bytes", type=int, default=64)
    args = parser.parse_args()
    if args.seconds < 2 or args.seconds > 20 or not args.rate or args.payload_bytes < 48:
        parser.error("seconds/rate/payload-bytes 超出诊断范围")
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    runs = root / "runs"
    runs.mkdir()
    configurations = (
        {"name": "v1-pooled-S4-W4", "version": 1, "mode": "pooled", "trace": True},
        {"name": "v2-pooled-S4-W4", "version": 2, "mode": "pooled", "trace": True},
        {"name": "v2-per-topic-W4", "version": 2, "mode": "per-topic", "trace": True},
    )
    for round_number in (1, 2):
        for configuration in configurations:
            for scenario in SCENARIOS:
                case = {"configuration": configuration["name"], "network_version": configuration["version"],
                        "data_mode": configuration["mode"], "topology": scenario["name"],
                        "round": round_number, "trace_enabled": True, "scenario": scenario,
                        "seconds": args.seconds, "rate_per_publisher_hz": args.rate,
                        "planned_total_rate_hz": args.rate * publisher_count(scenario),
                        "payload_bytes": args.payload_bytes, "delivery": "reliable"}
                name = f"r{round_number}-{configuration['name']}-{scenario['name']}-trace.json"
                run_case(args, case, runs / name)
        scenario = SCENARIOS[2]
        configuration = {"name": "v2-per-topic-W4", "version": 2, "mode": "per-topic"}
        case = {"configuration": configuration["name"], "network_version": configuration["version"],
                "data_mode": configuration["mode"], "topology": scenario["name"],
                "round": round_number, "trace_enabled": False, "scenario": scenario,
                "seconds": args.seconds, "rate_per_publisher_hz": args.rate,
                "planned_total_rate_hz": args.rate * publisher_count(scenario),
                "payload_bytes": args.payload_bytes, "delivery": "reliable", "purpose": "trace-overhead-pair"}
        name = f"r{round_number}-{configuration['name']}-{scenario['name']}-trace-off.json"
        run_case(args, case, runs / name)
    def fingerprint(path):
        digest = hashlib.sha256()
        with Path(path).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"path": str(Path(path).resolve()), "sha256": digest.hexdigest()}

    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    worktree = subprocess.check_output(["git", "status", "--short"], text=True).splitlines()
    run_files = sorted(runs.glob("*.json"))
    failures = []
    for path in run_files:
        run = json.loads(path.read_text())
        if not run.get("passed_content"):
            failures.append(str(path))
    manifest = {"source_revision": revision, "source_worktree_status": worktree,
                "gateway": fingerprint(args.gateway), "probe": fingerprint(args.probe),
                "diagnostic_script": fingerprint(__file__),
                "analyzer_script": fingerprint(Path(__file__).with_name("analyze_queue_attribution.py")),
                "platform": platform.platform(),
                "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                "seconds": args.seconds, "rate_per_publisher_hz": args.rate,
                "payload_bytes": args.payload_bytes, "rounds": 2,
                "order": "round -> v1 pooled -> v2 pooled -> v2 per-topic; each configuration runs the three topologies serially",
                "message_samples_enabled": True, "data_workers": 4,
                "trace_capacity_per_process": 16384,
                "physical_cross_host": False, "all_failures_retained": True,
                "case_count": len(run_files), "failures": failures}
    (root / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
