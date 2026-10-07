#!/usr/bin/env python3
"""Run frozen A/B local latency windows serially and retain each result."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import threading
import time


BENCHMARK = Path(__file__).with_name("benchmark.py").resolve()
ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "docs/shared_network_endpoint_evidence/20261007-local-root-cause"
A_WORKTREE = Path("/tmp/dzipc-root-cause-baseline")
B_WORKTREE = Path("/tmp/dzipc-local-latency-root-cause")
A_BINARY = A_WORKTREE / "build-latency-formal/bin/shared_net_benchmark_A"
A_LIBRARY = A_WORKTREE / "build-latency-formal/lib/libipc.so"
B_BINARY = B_WORKTREE / "build-latency-formal/bin/shared_net_benchmark"
B_LIBRARY = B_WORKTREE / "build-latency-formal/lib/libipc.so"
B_GATEWAY = B_WORKTREE / "build-latency-formal/bin/dzipc_gateway"
SCENARIOS = [(1, 4096), (32, 64), (1, 1048576)]
IDLE_SECONDS = 20


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_proc_stat(path):
    try:
        raw = Path(path).read_text()
        tail = raw[raw.rfind(")") + 2:].split()
        return {"comm": raw[raw.find("(") + 1:raw.rfind(")")],
                "utime_ticks": int(tail[11]), "stime_ticks": int(tail[12]),
                "start_ticks": int(tail[19])}
    except (OSError, ValueError, IndexError):
        return None


def cpu_totals():
    fields = Path("/proc/stat").read_text().splitlines()[0].split()[1:]
    values = [int(value) for value in fields]
    return {"total_ticks": sum(values), "idle_ticks": values[3] + (values[4] if len(values) > 4 else 0)}


def process_snapshot():
    processes = {}
    for item in Path("/proc").iterdir():
        if not item.name.isdigit():
            continue
        stat = read_proc_stat(item / "stat")
        if stat:
            processes[item.name] = stat
    return processes


def environment_snapshot():
    governors = {}
    frequencies = {}
    for path in Path("/sys/devices/system/cpu").glob("cpu*/cpufreq/scaling_governor"):
        try:
            governors[path.parent.parent.name] = path.read_text().strip()
        except OSError:
            pass
    for path in Path("/sys/devices/system/cpu").glob("cpu*/cpufreq/scaling_cur_freq"):
        try:
            frequencies[path.parent.parent.name] = int(path.read_text())
        except (OSError, ValueError):
            pass
    temperatures = {}
    for path in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            temperatures[path.parent.name] = int(path.read_text())
        except (OSError, ValueError):
            pass
    return {"timestamp_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns(),
            "loadavg": os.getloadavg(), "cpu": cpu_totals(), "processes": process_snapshot(),
            "governors": governors, "frequencies_khz": frequencies,
            "temperatures_millicelsius": temperatures,
            "allowed_cpus": sorted(os.sched_getaffinity(0))}


def activity_delta(before, after, ticks_per_second):
    changes = []
    for pid, current in after["processes"].items():
        previous = before["processes"].get(pid)
        if previous and current["start_ticks"] == previous["start_ticks"]:
            cpu_ticks = ((current["utime_ticks"] + current["stime_ticks"])
                         - (previous["utime_ticks"] + previous["stime_ticks"]))
        else:
            cpu_ticks = current["utime_ticks"] + current["stime_ticks"]
        cpu_seconds = cpu_ticks / ticks_per_second
        if cpu_seconds > 0:
            changes.append({"pid": int(pid), "comm": current["comm"],
                            "start_ticks": current["start_ticks"], "cpu_seconds": cpu_seconds})
    changes.sort(key=lambda item: (-item["cpu_seconds"], item["pid"]))
    return changes


def process_cwd(pid):
    try:
        return os.readlink(f"/proc/{pid}/cwd")
    except OSError:
        return None


def system_busy_delta(before, after):
    return ((after["cpu"]["total_ticks"] - before["cpu"]["total_ticks"])
            - (after["cpu"]["idle_ticks"] - before["cpu"]["idle_ticks"]))


def observed_competitors(activity, current_pid=None, minimum_cpu_seconds=1.0,
                         worktree_root=None):
    if current_pid is None:
        current_pid = os.getpid()
    worktree_root = str(worktree_root) if worktree_root else None
    return [item for item in activity if item["pid"] != current_pid
            and not (worktree_root and item.get("cwd")
                     and (item["cwd"] == worktree_root
                          or item["cwd"].startswith(worktree_root + "/")))
            and item["cpu_seconds"] >= minimum_cpu_seconds]


def aggregate_process_activity(samples):
    totals = {}
    for sample in samples:
        for item in sample["process_cpu_delta"]:
            key = (item["pid"], item["start_ticks"])
            current = totals.get(key)
            if current is None:
                totals[key] = dict(item)
            else:
                current["cpu_seconds"] += item["cpu_seconds"]
                current["comm"] = item["comm"]
                current["cwd"] = item.get("cwd") or current.get("cwd")
    result = list(totals.values())
    result.sort(key=lambda item: (-item["cpu_seconds"], item["pid"]))
    return result


def monitor_process_activity(stop_event, state, ticks_per_second):
    while not stop_event.wait(1.0):
        current = process_snapshot()
        delta = activity_delta(state["last_processes"], current, ticks_per_second)
        root = str(B_WORKTREE)
        for item in delta:
            item["cwd"] = process_cwd(item["pid"])
        state["samples"].append({
            "monotonic_ns": time.monotonic_ns(),
            "process_cpu_delta": delta,
        })
        state["last_processes"] = current


def wait_for_idle_gate(max_wait_seconds=300, trace_path=None):
    ticks = os.sysconf("SC_CLK_TCK")
    elapsed = 0
    attempts = []
    while elapsed < max_wait_seconds:
        before = environment_snapshot()
        time.sleep(IDLE_SECONDS)
        after = environment_snapshot()
        activity = activity_delta(before, after, ticks)
        external = observed_competitors(activity)
        attempt = {"seconds": IDLE_SECONDS, "before": before, "after": after,
                         "process_cpu_delta": activity, "observed_competitors": external,
                         "system_busy_ticks_delta": system_busy_delta(before, after)}
        attempts.append(attempt)
        if trace_path:
            with Path(trace_path).open("a") as stream:
                stream.write(json.dumps(attempt, ensure_ascii=False) + "\n")
        elapsed += IDLE_SECONDS
        if not external:
            return {"passed": True, "attempts": attempts,
                    "detector_limit": "可见进程单窗CPU >=1s视为竞争；低于阈值和内核/硬件噪声仍可能存在"}
    return {"passed": False, "attempts": attempts,
            "detector_limit": "可见进程单窗CPU >=1s视为竞争；低于阈值和内核/硬件噪声仍可能存在"}


def make_windows(phase):
    windows = []
    if phase == "smoke":
        order = [mode for subscribers, size in SCENARIOS for mode in ("A", "B")]
        for index, (subscribers, size) in enumerate(SCENARIOS):
            for mode in ("A", "B"):
                windows.append({"id": f"smoke-{mode}-sub{subscribers}-bytes{size}",
                                "mode": mode, "subscribers": subscribers, "bytes": size,
                                "seconds": 3, "rate_hz": 100, "warmup_seconds": 2,
                                "observation": "L0", "scenario_index": index,
                                "order": len(windows) + 1})
        assert len(order) == len(windows)
    elif phase == "d02":
        frozen_order = ["A", "B", "B", "A", "B", "A", "A", "B"]
        for scenario_index, (subscribers, size) in enumerate(SCENARIOS):
            for position, mode in enumerate(frozen_order):
                windows.append({"id": f"d02-{scenario_index + 1}-{mode}-sub{subscribers}-bytes{size}-p{position + 1}",
                                "mode": mode, "subscribers": subscribers, "bytes": size,
                                "seconds": 10, "rate_hz": 100, "warmup_seconds": 2,
                                "observation": "L0", "scenario_index": scenario_index,
                                "pair_block": 1 if position < 4 else 2,
                                "position": position + 1, "order": len(windows) + 1})
    else:
        raise ValueError(phase)
    return windows


def binary_for(mode):
    if mode == "A":
        return A_BINARY, A_LIBRARY, "baseline"
    return B_BINARY, B_LIBRARY, "shared_v1"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("smoke", "d02"), required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--validate-plan", action="store_true")
    parser.add_argument("--max-idle-wait-seconds", type=int, default=300)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"证据目录必须为空：{output}")
    output.mkdir(parents=True, exist_ok=True)
    windows = make_windows(args.phase)
    files = {"A_binary": A_BINARY, "A_library": A_LIBRARY,
             "B_binary": B_BINARY, "B_library": B_LIBRARY, "B_gateway": B_GATEWAY,
             "benchmark_source": BENCHMARK}
    missing = [str(path) for path in files.values() if not Path(path).is_file()]
    if missing:
        raise RuntimeError({"missing_artifacts": missing})
    frozen_hashes = {name: sha256(path) for name, path in files.items()}
    plan = {"schema": "local-latency-root-cause-window-plan/1", "phase": args.phase,
            "source_sha": "d064b62d0dda7413f76b858538315138be237e49e000637d7457d512ce64de10",
            "benchmark_path": str(BENCHMARK), "binary_hashes": frozen_hashes,
            "environment": {"uid": os.getuid(), "gid": os.getgid(),
                            "allowed_cpus": sorted(os.sched_getaffinity(0)),
                            "rlimit_nofile": list(resource.getrlimit(resource.RLIMIT_NOFILE)),
                            "idle_gate_seconds": IDLE_SECONDS,
                            "process_sample_interval_seconds": 1.0,
                            "process_competitor_cpu_seconds": 1.0},
            "windows": windows}
    plan_path = output / "plan.json"
    plan_path.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"phase": args.phase, "windows": windows,
                      "binary_hashes": frozen_hashes}, ensure_ascii=False), flush=True)
    if args.validate_plan:
        return 0

    results = []
    for window in windows:
        idle_gate = wait_for_idle_gate(args.max_idle_wait_seconds,
                                       output / f"{window['id']}.idle.jsonl")
        if not idle_gate["passed"]:
            result = {"window": window, "exit_code": None,
                      "failure": "未能取得连续20秒无可见竞争任务的静置窗口",
                      "idle_gate": idle_gate}
            results.append(result)
            (output / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
            return 2
        binary, library, mode = binary_for(window["mode"])
        run_dir = output / window["id"]
        if run_dir.exists():
            raise RuntimeError(f"窗口目录已存在：{run_dir}")
        run_dir.mkdir(parents=True)
        command = [sys.executable, str(BENCHMARK), "--binary", str(binary),
                   "--gateway", str(B_GATEWAY), "--mode", mode,
                   "--output", str(run_dir), "--bytes", str(window["bytes"]),
                   "--subscribers", str(window["subscribers"]),
                   "--seconds", str(window["seconds"]), "--rate", str(window["rate_hz"]),
                   "--data-shards", "4", "--data-workers", "4"]
        environment_before = environment_snapshot()
        started = time.time_ns()
        ticks = os.sysconf("SC_CLK_TCK")
        monitor_state = {"last_processes": environment_before["processes"], "samples": []}
        monitor_stop = threading.Event()
        monitor = threading.Thread(target=monitor_process_activity,
                                   args=(monitor_stop, monitor_state, ticks), daemon=True)
        monitor.start()
        try:
            completed = subprocess.run(command, cwd=B_WORKTREE, text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        finally:
            monitor_stop.set()
            monitor.join()
        ended = time.time_ns()
        environment_after = environment_snapshot()
        tail_activity = activity_delta(monitor_state["last_processes"],
                                       environment_after["processes"], ticks)
        if tail_activity:
            root = str(B_WORKTREE)
            for item in tail_activity:
                item["cwd"] = process_cwd(item["pid"])
            monitor_state["samples"].append({"monotonic_ns": environment_after["monotonic_ns"],
                                             "process_cpu_delta": tail_activity})
        process_activity = aggregate_process_activity(monitor_state["samples"])
        competitors = observed_competitors(process_activity, worktree_root=B_WORKTREE)
        sampling_environment = {
            "before": environment_before,
            "after": environment_after,
            "process_cpu_delta": process_activity,
            "process_activity_samples": monitor_state["samples"],
            "process_sample_interval_seconds": 1.0,
            "observed_competitors": competitors,
            "system_busy_ticks_delta": system_busy_delta(environment_before, environment_after),
            "detector_limit": "窗内每秒采集进程CPU增量；同一进程实例累计CPU >=1s且工作目录不在本测试worktree的进程标记为竞争。短于采样间隔且在两次采样间退出的任务、阈值以下活动及内核/硬件噪声可能漏检",
        }
        (run_dir / "environment.json").write_text(
            json.dumps(sampling_environment, ensure_ascii=False, indent=2) + "\n")
        log_path = output / f"{window['id']}.log"
        log_path.write_text(completed.stdout)
        window_result = {"window": window, "argv": command, "started_ns": started,
                         "ended_ns": ended, "exit_code": completed.returncode,
                         "idle_gate": idle_gate,
                         "environment_file": str(run_dir.relative_to(output) / "environment.json"),
                         "environment_sha256": sha256(run_dir / "environment.json"),
                         "observed_competitors": competitors,
                         "validity": "受干扰" if competitors else "待完整性审计",
                         "binary_sha256": sha256(binary), "library_sha256": sha256(library),
                         "log": log_path.name}
        result_path = run_dir / "result.json"
        if result_path.exists():
            window_result["result"] = json.loads(result_path.read_text())
        if sha256(binary) != frozen_hashes[f"{window['mode']}_binary"] or sha256(library) != frozen_hashes[f"{window['mode']}_library"]:
            window_result["failure"] = "采样二进制或动态库哈希发生变化"
            window_result["exit_code"] = 3
        results.append(window_result)
        (output / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps({"completed": len(results), "total": len(windows),
                          "window": window["id"], "exit_code": window_result["exit_code"],
                          "idle_gate_passed": idle_gate["passed"],
                          "observed_competitors": competitors}, ensure_ascii=False), flush=True)
        if competitors:
            window_result["failure"] = "采样期间检测到竞争任务；保留当前窗并停止该平衡批次"
            (output / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
            return 2
        if window_result["exit_code"] != 0:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
