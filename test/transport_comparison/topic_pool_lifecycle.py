#!/usr/bin/env python3
"""池生命周期隔离回归；构建、日志和资源数据均写入仓库外。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TESTS = """test_topic_chunk_pool test_loan test_dzflat_transport
test_chunk_capacity_backpressure test_adopt_loan_quota test_dzflat_sercli
test_shm_ser_backpressure test_shm_sniffer_control_name test_lifecycle_contract
test_shm_route_session test_dzflat_fallback_semantics test_shm_sub_dtor_gate
test_chunk_hold test_lap_safety test_pool_exhaust_observability test_recv_worker
test_recv_fragment_isolation test_dzflat_rx""".split()


def isolated(command):
    return ["unshare", "--user", "--map-root-user", "--mount", "--ipc", "--fork",
            "bash", "-c", 'mount --make-rprivate / && mount -t tmpfs -o size=4G tmpfs /dev/shm && exec "$@"',
            "pool-test", *map(str, command)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--skip-build", action="store_true")
    p.add_argument("--tests", nargs="+", default=TESTS)
    a = p.parse_args()
    w = a.work.resolve()
    if w == ROOT or ROOT in w.parents:
        p.error("工作目录必须在仓库外")
    results_dir = w / "lifecycle-results"
    results_dir.mkdir(parents=True, exist_ok=False)
    if not a.skip_build:
        subprocess.run(["python3", "-B", str(ROOT / "test/transport_comparison/repair_units.py"),
                        "--work", str(w / "units"), "--targets", *a.tests], check=True)
    build = w / "units/build"
    rows = []
    hashes = {}
    for name in a.tests:
        binary = build / "bin" / name
        hashes[name] = hashlib.sha256(binary.read_bytes()).hexdigest()
        with (results_dir / (name + ".log")).open("w") as log:
            try:
                code = subprocess.run(isolated([binary]), stdout=log, stderr=subprocess.STDOUT,
                                      timeout=240).returncode
            except subprocess.TimeoutExpired:
                code = 124
        rows.append({"test": name, "exit": code})
        (results_dir / "results.json").write_text(json.dumps(rows, indent=2))
        print(name, code, flush=True)
    lib = build / "lib/libipc.so"
    hashes["libipc.so"] = hashlib.sha256(lib.read_bytes()).hexdigest()
    (results_dir / "hashes.json").write_text(json.dumps(hashes, indent=2))
    probe = results_dir / "resource-probe"
    subprocess.run(["c++", "-std=c++17", "-O2", str(ROOT / "test/transport_comparison/topic_pool_resources.cpp"),
                    "-I" + str(ROOT / "include"), "-L" + str(lib.parent),
                    "-Wl,-rpath," + str(lib.parent), "-lipc", "-pthread", "-lrt", "-o", str(probe)], check=True)
    for size in [64, 1024, 1048577]:
        with (results_dir / f"resources-{size}.log").open("w") as log:
            code = subprocess.run(isolated([probe, "1000", size]), stdout=log,
                                  stderr=subprocess.STDOUT, timeout=180).returncode
        rows.append({"resource_bytes": size, "exit": code})
        (results_dir / "results.json").write_text(json.dumps(rows, indent=2))
        print("千话题资源", size, code, flush=True)
    return any(r["exit"] for r in rows)


if __name__ == "__main__":
    raise SystemExit(main())
