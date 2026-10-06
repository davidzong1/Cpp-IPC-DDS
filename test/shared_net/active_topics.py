#!/usr/bin/env python3
"""N12 双向 100 话题冷热负载、并发注册注销和资源采样。"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

from end_to_end import Process, reserve_ports, udp_count


def proc_sample(pid):
    proc = Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    return {
        "rss_kib": int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmRSS:"))),
        "fd": len(list((proc / "fd").iterdir())),
        "threads": int(next(line.split()[1] for line in status.splitlines() if line.startswith("Threads:"))),
    }


def read_gateway_status(executable, control, env):
    return json.loads(subprocess.check_output(
        [executable, "status", "--control", control, "--json"],
        env=env, text=True, stderr=subprocess.PIPE, timeout=15))


def status_summary(status):
    return {
        key: status.get(key) for key in (
            "state", "network_version", "data_mode", "udp_sockets", "data_sockets",
            "data_workers", "data_socket_cap", "active_routes", "registered_handles",
            "logical_publishers", "logical_subscribers", "endpoint_states", "resource_budget",
            "sent_messages", "committed_messages", "data_datagrams_rx", "data_bytes_tx",
            "data_bytes_rx", "invalid_packets", "message_crc_fail", "wrong_shard",
            "send_eagain", "send_error", "truncated", "duplicate_suppressed",
            "shm_commit_attempts", "shm_commit_not_submitted", "assembly_expired_or_revoked",
            "receive_quota_rejected", "queued_commands", "queued_command_bytes",
            "client_sessions", "allocated_send_bytes", "send_inflight_bytes", "send_inflight_records",
            "allocated_send_records", "outbox_records", "credit_requests", "rejected",
            "active_peers", "snapshot_version",
        )
    } | {"route_count": len(status.get("routes", []))}


def close_probe(probe):
    if probe.p.poll() is None:
        try:
            probe.p.stdin.write("quit\n")
            probe.p.stdin.flush()
            probe.p.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            probe.close()
    probe.close()


def host(args):
    subprocess.run(["mount", "-t", "tmpfs", "-o", "size=768m", "tmpfs", "/dev/shm"], check=True)
    env = dict(os.environ, DZIPC_SHM_MPMC="1", DZIPC_NET_BACKEND="shared_v1", DZIPC_NET_TRACE="0")
    gateway_args = [
        args.gateway, "serve", "--control", args.control, "--listen-ip", "127.0.0.1",
        "--interface", "lo", "--network-version", "2", "--data-mode", args.mode,
        "--data-port-range", f"{args.base}:{args.base + args.span - 1}",
        "--data-workers", "4", "--data-socket-cap", str(args.data_socket_cap),
        "--socket-buffer-budget-bytes", str(args.socket_buffer_budget_bytes),
        "--control-port", str(args.control_port), "--discovery-port", str(args.discovery),
    ]
    if args.mode != "per-topic":
        gateway_args += ["--data-sockets", str(args.data_sockets)]
    gateway = subprocess.Popen(gateway_args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               text=True, env=env, start_new_session=True)
    gateway_errors = []
    threading.Thread(target=lambda: gateway_errors.extend(gateway.stderr.readlines()), daemon=True).start()
    probes = []
    operation = {"thread": None, "result": None, "error": None}
    topics = ",".join(f"active_{index:03d}" for index in range(args.topics))

    def snapshot():
        status = read_gateway_status(args.gateway, args.control, env)
        return {"gateway": status_summary(status), "process": proc_sample(gateway.pid),
                "gateway_udp_fds": udp_count(gateway.pid),
                "application_udp_fds": [udp_count(probe.p.pid) for probe in probes if probe.p.poll() is None]}

    def run_traffic():
        try:
            started_ns = time.monotonic_ns()
            drain_result = {}
            drain_error = {}

            def drain():
                try:
                    drain_result["value"] = probes[1].request(f"drainset {args.seconds + 2}")
                except Exception as error:
                    drain_error["value"] = repr(error)

            drainer = threading.Thread(target=drain, daemon=True)
            drainer.start()
            time.sleep(0.15)
            load_started_ns = time.monotonic_ns()
            load = probes[0].request(
                f"loadsetmix {args.seconds} {args.hot_rate} {args.cold_rate} 64 0 {args.hot_topics}")
            load_finished_ns = time.monotonic_ns()
            drainer.join(timeout=args.seconds + 8)
            if drainer.is_alive():
                raise RuntimeError("订阅排空任务未在时限内结束")
            if "value" not in drain_result:
                raise RuntimeError({"drain_error": drain_error})
            operation["result"] = {
                "started_monotonic_ns": started_ns,
                "load_started_monotonic_ns": load_started_ns,
                "load_finished_monotonic_ns": load_finished_ns,
                "load": load,
                "drain": drain_result["value"],
            }
        except Exception:
            operation["error"] = traceback.format_exc()

    try:
        deadline = time.monotonic() + 20
        while not Path(args.control).exists():
            if gateway.poll() is not None:
                raise RuntimeError({"gateway_exit": gateway.returncode, "stderr": gateway_errors[-30:]})
            if time.monotonic() >= deadline:
                raise RuntimeError("网关控制路径启动超时")
            time.sleep(0.01)
        for role in ("pubset", "subset"):
            probe = Process([args.probe, args.control, topics, role], env=env)
            probes.append(probe)
            ready = probe.receive(timeout=45)
            if not ready.get("ready"):
                raise RuntimeError({"role": role, "ready": ready})
        print(json.dumps({"ready": True, "gateway_pid": gateway.pid,
                          "probe_pids": [probe.p.pid for probe in probes]}, ensure_ascii=False), flush=True)
        for line in sys.stdin:
            command = json.loads(line)
            if command[0] == "quit":
                break
            if command[0] == "state":
                state = probes[0].request("state")
                result = {"synchronized_routes": sum(
                    route["synchronized"] and route["remote"] == 1 for route in state["routes"]),
                    "route_count": len(state["routes"]), "healthy": state["healthy"]}
            elif command[0] == "snapshot":
                result = snapshot()
            elif command[0] == "start":
                if operation["thread"] is not None:
                    raise RuntimeError("本轮负载已启动")
                operation["thread"] = threading.Thread(target=run_traffic, daemon=True)
                operation["thread"].start()
                result = {"started": True, "host_start_monotonic_ns": time.monotonic_ns()}
            elif command[0] == "result":
                thread = operation["thread"]
                if thread is None:
                    raise RuntimeError("本轮负载尚未启动")
                thread.join(timeout=1)
                result = {"done": not thread.is_alive(), "operation_error": operation["error"],
                          "result": operation["result"] if not thread.is_alive() else None}
            elif command[0] == "churn":
                extra = Process([args.probe, args.control, f"churn_{args.mode}_{args.host_index}", "pub"], env=env)
                try:
                    ready = extra.receive(timeout=10)
                    assert ready.get("ready"), ready
                    expected = (args.data_sockets if args.mode != "per-topic" else args.topics) + (1 if args.mode == "per-topic" else 0)
                    deadline = time.monotonic() + 5
                    held = read_gateway_status(args.gateway, args.control, env)
                    while (held.get("data_sockets") != expected or
                           held.get("active_routes") != args.topics + 1):
                        assert time.monotonic() < deadline, status_summary(held)
                        time.sleep(0.02)
                        held = read_gateway_status(args.gateway, args.control, env)
                    held_snapshot = {"gateway": status_summary(held), "process": proc_sample(gateway.pid)}
                    time.sleep(args.churn_hold_ms / 1000)
                finally:
                    close_probe(extra)
                deadline = time.monotonic() + 5
                after = read_gateway_status(args.gateway, args.control, env)
                while (after.get("data_sockets") != (args.data_sockets if args.mode != "per-topic" else args.topics) or
                       after.get("active_routes") != args.topics):
                    assert time.monotonic() < deadline, status_summary(after)
                    time.sleep(0.02)
                    after = read_gateway_status(args.gateway, args.control, env)
                result = {"held": held_snapshot, "after": status_summary(after),
                          "elapsed_monotonic_ns": time.monotonic_ns()}
            else:
                raise RuntimeError(command)
            print(json.dumps(result, ensure_ascii=False), flush=True)
    finally:
        for probe in probes:
            close_probe(probe)
        if gateway.poll() is None:
            gateway.send_signal(signal.SIGTERM)
            try:
                gateway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait()


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def summarize_load(load, hot_topics):
    return [{"index": index, "kind": "hot" if index < hot_topics else "cold", **item}
            for index, item in enumerate(load)]


def run_mode(args, output, mode):
    span = max(args.topics * 2 + 8, args.data_sockets + 8)
    bases, discovery = reserve_ports(args.data_sockets, span)
    hosts = []
    events = []
    args.mode = mode
    args.span = span
    try:
        with tempfile.TemporaryDirectory(prefix=f"dzipc-active-{mode}-") as folder:
            for index in range(2):
                host_dir = Path(folder) / f"host{index}"
                host_dir.mkdir(mode=0o700)
                command = ["unshare", "--user", "--map-root-user", "--mount", "--ipc",
                           sys.executable, str(Path(__file__).resolve()), "--host",
                           "--gateway", args.gateway, "--probe", args.probe,
                           "--control", str(host_dir / "control.sock"), "--base", str(bases[index]),
                           "--span", str(span), "--control-port", str(bases[index] + span),
                           "--discovery", str(discovery), "--mode", mode,
                           "--data-sockets", str(args.data_sockets),
                           "--data-socket-cap", str(args.data_socket_cap),
                           "--socket-buffer-budget-bytes", str(args.socket_buffer_budget_bytes),
                           "--topics", str(args.topics), "--seconds", str(args.seconds),
                           "--hot-rate", str(args.hot_rate), "--cold-rate", str(args.cold_rate),
                           "--hot-topics", str(args.hot_topics), "--churn-hold-ms", str(args.churn_hold_ms),
                           "--host-index", str(index)]
                hosts.append(Process(command, group=True))
            for process in hosts:
                events.append({"event": "ready", "result": process.receive(timeout=60)})

            def request(index, operation):
                result = hosts[index].request(json.dumps(operation))
                events.append({"host": index, "command": operation[0], "result": result})
                return result

            deadline = time.monotonic() + 25
            states = []
            while True:
                states = [request(index, ["state"]) for index in range(2)]
                if all(state["healthy"] and state["route_count"] == args.topics and
                       state["synchronized_routes"] == args.topics for state in states):
                    break
                assert time.monotonic() < deadline, {"states": states, "errors": [p.errors[-20:] for p in hosts]}
                time.sleep(0.1)

            before = [request(index, ["snapshot"]) for index in range(2)]
            expected_sockets = args.data_sockets if mode != "per-topic" else args.topics
            for snapshot in before:
                status = snapshot["gateway"]
                assert status["data_mode"] == mode and status["data_sockets"] == expected_sockets, status
                assert status["active_routes"] == args.topics and status["route_count"] == args.topics, status
                assert status["registered_handles"] == args.topics * 2, status
                assert snapshot["application_udp_fds"] == [0, 0], snapshot

            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                started = list(pool.map(lambda i: request(i, ["start"]), range(2)))
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                churn = list(pool.map(lambda i: request(i, ["churn"]), range(2)))
            finish_deadline = time.monotonic() + args.seconds + 12
            finished = [request(index, ["result"]) for index in range(2)]
            while not all(item["done"] for item in finished):
                assert time.monotonic() < finish_deadline, finished
                time.sleep(0.1)
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    finished = list(pool.map(lambda i: request(i, ["result"]), range(2)))
            after = [request(index, ["snapshot"]) for index in range(2)]

            assert all(item["done"] and not item["operation_error"] and item["result"] for item in finished), finished
            loadsets = [item["result"]["load"] for item in finished]
            drains = [item["result"]["drain"] for item in finished]
            assert all(len(load) == args.topics for load in loadsets), [len(load) for load in loadsets]
            for load in loadsets:
                for index, item in enumerate(load):
                    expected = (args.hot_rate if index < args.hot_topics else args.cold_rate) * args.seconds
                    assert item["failed"] == 0 and item["sent"] >= expected * 0.90, {
                        "topic_index": index, "expected_at_least": expected * 0.90, "actual": item}
            for index in range(args.topics):
                expected_received = loadsets[0][index]["sent"] + loadsets[1][index]["sent"]
                for host_index in range(2):
                    sample = drains[host_index][index]
                    if sample["received"] < expected_received * 0.95:
                        events.append({"event": "receive_shortfall", "host": host_index,
                                       "topic_index": index, "expected": expected_received,
                                       "sample": sample})
                    assert sample["invalid"] == 0 and sample["duplicates"] == 0, sample
            for index, snapshot in enumerate(after):
                status = snapshot["gateway"]
                assert status["data_sockets"] == expected_sockets and status["active_routes"] == args.topics, status
                assert status["route_count"] == args.topics and status["registered_handles"] == args.topics * 2, status
                assert status["data_datagrams_rx"] > before[index]["gateway"]["data_datagrams_rx"], {
                    "before": before[index]["gateway"], "after": status}
                assert snapshot["application_udp_fds"] == [0, 0], snapshot
            return {
                "mode": mode, "topics": args.topics, "hot_topics": args.hot_topics,
                "hot_rate_per_topic_hz": args.hot_rate, "cold_rate_per_topic_hz": args.cold_rate,
                "seconds": args.seconds, "wire_bytes": 64,
                "topology": "两端独立用户/挂载/IPC 命名空间，真实 loopback UDP；非物理跨机",
                "synchronized_routes": states,
                "host_start_windows": started,
                "resource_before": before, "churn": churn,
                "load_by_host_and_topic": [summarize_load(load, args.hot_topics) for load in loadsets],
                "drain_by_host_and_topic": drains, "resource_after": after,
                "events": events,
            }
    finally:
        for process in hosts:
            if process.p.poll() is None:
                try:
                    process.p.stdin.write('["quit"]\n')
                    process.p.stdin.flush()
                    process.p.wait(timeout=12)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            process.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--output")
    parser.add_argument("--modes", default="pooled,per-topic")
    parser.add_argument("--topics", type=int, default=100)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--hot-topics", type=int, default=10)
    parser.add_argument("--hot-rate", type=float, default=100)
    parser.add_argument("--cold-rate", type=float, default=5)
    parser.add_argument("--data-sockets", type=int, default=4)
    parser.add_argument("--data-socket-cap", type=int, default=256)
    parser.add_argument("--socket-buffer-budget-bytes", type=int, default=268435456)
    parser.add_argument("--churn-hold-ms", type=int, default=150)
    parser.add_argument("--host", action="store_true")
    parser.add_argument("--mode")
    parser.add_argument("--base", type=int)
    parser.add_argument("--span", type=int)
    parser.add_argument("--control")
    parser.add_argument("--control-port", type=int)
    parser.add_argument("--discovery", type=int)
    parser.add_argument("--host-index", type=int, default=0)
    args = parser.parse_args()
    if args.host:
        try:
            host(args)
        except Exception as error:
            print(json.dumps({"error": str(error)}, ensure_ascii=False), flush=True)
            raise
        return

    if not args.output:
        parser.error("驱动模式必须指定 --output")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    modes = args.modes.split(",")
    assert modes and all(mode in ("pooled", "per-topic") for mode in modes), modes
    assert 1 <= args.topics <= 200 and 1 <= args.seconds <= 60
    assert 0 < args.hot_topics < args.topics and args.hot_rate > 0 and args.cold_rate > 0
    assert args.data_socket_cap >= args.topics and args.churn_hold_ms >= 0
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    uname = os.uname()
    manifest = {
        "argv": sys.argv, "physical_cross_host": False,
        "environment": {"uname": {key: getattr(uname, key) for key in
                                    ("sysname", "nodename", "release", "version", "machine")},
                        "page_size": os.sysconf("SC_PAGE_SIZE"),
                        "rlimit_nofile_soft": soft, "rlimit_nofile_hard": hard},
        "binary_sha256": {"gateway": file_hash(args.gateway), "probe": file_hash(args.probe)},
        "cases": [],
    }
    failures = []
    for mode in modes:
        case_path = output / f"{mode}.json"
        try:
            result = run_mode(args, output, mode)
            result["exit"] = 0
        except Exception as error:
            result = {"mode": mode, "exit": 1, "failure": repr(error),
                      "traceback": traceback.format_exc()}
            failures.append(result)
        case_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        manifest["cases"].append({"mode": mode, "path": case_path.name, "exit": result["exit"],
                                  "failure": result.get("failure")})
    manifest["failures"] = failures
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
