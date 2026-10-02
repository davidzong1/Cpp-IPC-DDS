#!/usr/bin/env python3
"""独立构建修复回归，避开顶层生成器和缺失的安装目标。"""
import argparse
from pathlib import Path
import subprocess
import json
import os
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ["test_recv_worker", "test_recv_fragment_isolation", "test_lifecycle_contract", "test_recv_wait_set",
           "test_shm_route_session", "test_w08_dzflat_ab", "test_dzflat_rx",
           "test_socket_reliable_crc", "test_socket_ser_concurrency",
           "test_socket_recv_worker", "test_socket_wait_set", "test_socket_readable"]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--targets", nargs="+", default=DEFAULT)
    p.add_argument("--run", action="store_true")
    a = p.parse_args()
    a.source, a.work = a.source.resolve(), a.work.resolve()
    if a.work == ROOT or ROOT in a.work.parents:
        p.error("构建目录必须在仓库外")
    project = a.work / "project"
    project.mkdir(parents=True, exist_ok=True)
    (project / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.16)
project(repair_units LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(LIBIPC_BUILD_SHARED_LIBS ON)
set(ROSBUILD OFF)
include(GNUInstallDirs)
enable_testing()
add_subdirectory("${LIBIPC_PROJECT_DIR}/src" ipc)
add_subdirectory("${LIBIPC_PROJECT_DIR}/3rdparty/gtest" gtest)
add_subdirectory("${LIBIPC_PROJECT_DIR}/test" test)
""")
    build = a.work / "build"
    subprocess.run(["cmake", "-S", str(project), "-B", str(build),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DLIBIPC_PROJECT_DIR={a.source}"], check=True)
    subprocess.run(["cmake", "--build", str(build), "-j6", "--target", *a.targets], check=True)
    if a.run:
        results = []
        for name in a.targets:
            env = os.environ.copy()
            if name == "test_w08_dzflat_ab":
                env["W08_ARTIFACT_ROOT"] = tempfile.mkdtemp(prefix="w08-", dir=a.work)
            with (a.work / f"{name}.log").open("w") as log:
                result = subprocess.run([str(build / "bin" / name)], stdout=log,
                                        stderr=subprocess.STDOUT, timeout=180, env=env)
            results.append({"test": name, "exit": result.returncode})
            (a.work / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2))
            print(f"{name}：退出码 {result.returncode}", flush=True)
        return any(x["exit"] for x in results)
    return False

if __name__ == "__main__":
    raise SystemExit(main())
