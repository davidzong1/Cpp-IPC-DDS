#!/usr/bin/env python3
"""N12 专用 UDP 端点规模、cap 拒绝和资源回收验证。"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import shutil


def reserve_ports(first, last, control, discovery):
    sockets = []
    try:
        for port in range(first, last + 1):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", port))
            sockets.append(sock)
        for port in (control, discovery):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", port))
            sockets.append(sock)
    finally:
        for sock in sockets:
            sock.close()


def proc_sample(pid):
    proc = Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    rss = next((int(line.split()[1]) for line in status.splitlines()
                if line.startswith("VmRSS:")), 0)
    return {"rss_kib": rss, "fd": len(list((proc / "fd").iterdir())),
            "threads": int(next(line.split()[1] for line in status.splitlines()
                                 if line.startswith("Threads:")))}


def gateway_status(gateway, executable, control, env):
    try:
        return json.loads(subprocess.check_output(
            [executable, "status", "--control", control, "--json"],
            env=env, text=True, stderr=subprocess.PIPE))
    except subprocess.CalledProcessError as error:
        raise AssertionError({"status_returncode": error.returncode,
                              "gateway_returncode": gateway.poll(),
                              "gateway_stderr": gateway.stderr.read() if gateway.stderr else ""}) from error


def settled_status(gateway, executable, control, env, expected):
    deadline = time.monotonic() + 5
    status = gateway_status(gateway, executable, control, env)
    while (status.get("data_sockets") != expected or
           status.get("active_routes") != expected):
        if time.monotonic() >= deadline:
            raise AssertionError({"expected": expected, "status": summarize(status)})
        time.sleep(.1)
        status = gateway_status(gateway, executable, control, env)
    return status


def summarize(status):
    return {key: status.get(key) for key in (
        "state", "data_mode", "udp_sockets", "data_sockets", "data_socket_cap",
        "registered_handles", "active_routes", "logical_publishers", "directory_bytes",
        "last_endpoint_failure_code", "last_endpoint_failure", "endpoint_states",
        "resource_budget", "data_ports")}


def run_case(args, output, name, requested, cap=None, buffer_budget=536870912):
    first, last = args.port_first, args.port_last
    control_port, discovery = args.control_port, args.discovery
    reserve_ports(first, last, control_port, discovery)
    control_dir = Path(tempfile.mkdtemp(prefix=f"dzipc-n12-{name}-"))
    control = str(control_dir / "control.sock")
    env = dict(os.environ, DZIPC_NET_BACKEND="shared_v1", DZIPC_SHM_MPMC="1",
               DZIPC_GATEWAY_CONTROL=control)
    gateway_args = [args.gateway, "serve", "--control", control,
                    "--listen-ip", "127.0.0.1", "--interface", "lo",
                    "--network-version", "2", "--data-mode", "per-topic",
                    "--data-port-range", f"{first}:{last}", "--data-workers", "4",
                    "--control-port", str(control_port), "--discovery-port", str(discovery),
                    "--socket-buffer-budget-bytes", str(buffer_budget)]
    if cap is not None:
        gateway_args += ["--data-socket-cap", str(cap)]
    gateway = subprocess.Popen(gateway_args, env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
    probe = None
    try:
        deadline = time.monotonic() + 10
        while not os.path.exists(control):
            if gateway.poll() is not None:
                raise AssertionError(gateway.stderr.read())
            if time.monotonic() >= deadline:
                raise AssertionError("网关控制路径创建超时")
            time.sleep(.01)
        prefix = f"n12_{name}_"
        probe = subprocess.Popen([args.probe, control, str(requested), "5000", prefix],
                                 stdin=subprocess.PIPE,
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, bufsize=1)
        line = probe.stdout.readline()
        if not line:
            raise AssertionError({"probe_returncode": probe.poll(),
                                  "probe_stderr": probe.stderr.read()})
        registration = json.loads(line)
        held = settled_status(gateway, args.gateway, control, env, registration["registered"])
        held_sample = proc_sample(gateway.pid)
        probe.stdin.write("release\n")
        probe.stdin.flush()
        released = json.loads(probe.stdout.readline())
        assert released["phase"] == "released", released
        probe.stdin.write("register-again\n")
        probe.stdin.flush()
        reregistered = json.loads(probe.stdout.readline())
        assert reregistered["phase"] == "reregistered", reregistered
        probe.stdin.write("close-again\n")
        probe.stdin.flush()
        closed_again = json.loads(probe.stdout.readline())
        assert closed_again["phase"] == "closed-again", closed_again
        probe.wait(timeout=180)
        if probe.returncode != 0:
            raise AssertionError({"probe_returncode": probe.returncode,
                                  "probe_stderr": probe.stderr.read()})
        after = gateway_status(gateway, args.gateway, control, env)
        after_sample = proc_sample(gateway.pid)
        raw = {"registration": registration, "held": held,
               "released": released["status"], "reregistered": reregistered["status"],
               "closed_again": closed_again["status"], "after": after,
               "held_process": held_sample, "after_process": after_sample,
               "gateway_args": gateway_args}
        (output / f"{name}-status.json").write_text(json.dumps(raw, ensure_ascii=False, indent=2))
        result = {"case": name, "requested": requested, "registration": registration,
                  "held": summarize(held), "after": summarize(after),
                  "held_process": held_sample, "after_process": after_sample}
        if name == "cap256":
            assert registration["registered"] == 256 and registration["failed"] == 1, registration
            assert "SocketCap" in held["last_endpoint_failure"], held["last_endpoint_failure"]
            assert held["data_sockets"] == 256 and held["active_routes"] == 256, summarize(held)
            assert held["endpoint_states"]["ready"] == 256, held["endpoint_states"]
            assert held["udp_sockets"] == 258, held["udp_sockets"]
            assert after["data_sockets"] == 0 and after["active_routes"] == 0, summarize(after)
            assert after["udp_sockets"] == 2 and after["resource_budget"]["reserved_ports"] == 2, summarize(after)
        else:
            assert registration["registered"] == requested and registration["failed"] == 0, registration
            assert held["data_sockets"] == requested and held["active_routes"] == requested, summarize(held)
            assert held["endpoint_states"]["ready"] == requested, held["endpoint_states"]
            assert held["udp_sockets"] == requested + 2, held["udp_sockets"]
            assert after["data_sockets"] == 0 and after["active_routes"] == 0, summarize(after)
            assert after["udp_sockets"] == 2 and after["resource_budget"]["reserved_ports"] == 2, summarize(after)
        assert released["status"]["data_sockets"] == 0 and released["status"]["active_routes"] == 0, released
        assert reregistered["status"]["data_sockets"] == 1 and reregistered["status"]["active_routes"] == 1, reregistered
        assert closed_again["status"]["data_sockets"] == 0 and closed_again["status"]["active_routes"] == 0, closed_again
        return result
    finally:
        if probe is not None and probe.poll() is None:
            probe.kill()
            probe.wait()
        if gateway.poll() is None:
            gateway.terminate()
            try:
                gateway.wait(timeout=10)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait()
        shutil.rmtree(control_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port-first", type=int, default=30000)
    parser.add_argument("--port-last", type=int, default=32050)
    parser.add_argument("--control-port", type=int, default=32500)
    parser.add_argument("--discovery", type=int, default=32501)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = [run_case(args, output, "cap256", 257, buffer_budget=536870912),
               run_case(args, output, "dedicated1000", 1000, cap=1024,
                        buffer_budget=2147483648)]
    print(json.dumps({"cases": results}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
