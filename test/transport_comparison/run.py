#!/usr/bin/env python3
"""串行、可续跑的跨进程测试；所有中间文件默认位于仓库外。"""
import argparse
import fcntl
import hashlib
import json
import os
import pathlib
import signal
import struct
import subprocess
import time
from typing import Any

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BACKENDS = ["shm", "socket", "a", "b", "prebuilt", "dds-udp", "dds-iox"]
SIZES = [8 << k for k in range(18)]

def atomic_json(path, value):
    tmp = path.with_suffix(".new")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2))
    tmp.replace(path)

def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def xml(iox):
    return f"""<CycloneDDS><Domain Id="any">
<SharedMemory><Enable>{str(iox).lower()}</Enable><LogLevel>warn</LogLevel></SharedMemory>
<General><Interfaces><NetworkInterface name="lo" multicast="true"/></Interfaces>
<AllowMulticast>true</AllowMulticast><EnableMulticastLoopback>true</EnableMulticastLoopback></General>
<Discovery><Peers><Peer Address="127.0.0.1"/></Peers></Discovery>
</Domain></CycloneDDS>"""

def terminate(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()

def command_output(args):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False).stdout.strip()

def own_roudi():
    if os.environ.get("COMPARISON_ROUDI_PID"):
        return int(os.environ["COMPARISON_ROUDI_PID"])
    namespace = os.readlink("/proc/self/ns/mnt")
    for p in pathlib.Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        try:
            if (p / "comm").read_text().strip() == "iox-roudi" and os.readlink(p / "ns/mnt") == namespace:
                return int(p.name)
        except (OSError, ProcessLookupError):
            continue
    return None

def proc_usage(pid):
    if pid is None:
        return None
    fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return dict(cpu=(int(fields[11])+int(fields[12])) / os.sysconf("SC_CLK_TCK"),
                threads=int(fields[17]), rss_kib=int(fields[21])*os.sysconf("SC_PAGE_SIZE")/1024)

def run_case(args, backend, mode, size, topics, repeat, seconds, full=False):
    case_id = f"{args.suite}-{mode}-{backend}-{size}-n{topics}-r{repeat}"
    result_path = args.work / "cases" / f"{case_id}.json"
    if result_path.exists() and not args.rerun:
        old = json.loads(result_path.read_text())
        if old.get("status") != "unsupported" and (
            old.get("duration") != seconds or old.get("full") != full
            or old.get("workers", args.workers) != args.workers
            or old.get("publishers", args.publishers) != args.publishers
            or old.get("binary_sha256") != fingerprint(args.work / "build/comparison")
        ):
            raise RuntimeError(f"{case_id} 的配置或二进制已变化，请使用新的 --work 或显式 --rerun")
        return old
    d = args.work / "runs" / case_id
    d.mkdir(parents=True, exist_ok=True)
    token = hashlib.sha256((case_id + str(time.time_ns())).encode()).hexdigest()[:10]
    result: dict[str, Any] = dict(id=case_id, backend=backend, mode=mode, bytes=size, topics=topics, repeat=repeat,
                  duration=seconds, full=full, token=token, workers=args.workers, publishers=args.publishers)
    if mode == "rpc" and backend in ("b", "prebuilt"):
        result.update(status="unsupported", reason="原生 ser-cli 没有借样/预构造段发送入口")
        atomic_json(result_path, result)
        return result
    # 断点重跑必须清除旧角色输出，防止新运行早期失败时读到旧统计。
    for role in ("pub", "sub"):
        (d / f"{role}.json").unlink(missing_ok=True)
    control = d / "control"
    control.write_bytes(struct.pack("QQQQ", 0, 0, (1 << 64)-1, (1 << 64)-1) + bytes(1000*8*4+8))
    uri = d / "dds.xml"
    uri.write_text(xml(backend == "dds-iox"))
    env = os.environ.copy()
    env.update(CYCLONEDDS_URI=f"file://{uri}", DZIPC_SHM_CONTROL_SCHEDULER="0",
               DZIPC_SHM_RECV_COMPAT="0", DZIPC_SOCKET_COMPAT_THREAD="0",
               DZIPC_SHM_RECV_WORKERS=str(args.workers), COMPARISON_PUBLISHERS=str(args.publishers))
    logs, procs = {}, {}
    start = time.monotonic()
    result["started_unix_ns"] = time.time_ns()
    roudi_pid = own_roudi()
    roudi_start = proc_usage(roudi_pid)
    try:
        for role in ["sub", "pub"]:
            logs[role] = (d / f"{role}.log").open("w")
            cmd = [str(args.work / "build/comparison"), backend, mode, role, str(size), str(topics),
                   str(seconds), str(args.domain), str(control), str(d / f"{role}.json"), token, str(int(full))]
            procs[role] = subprocess.Popen(cmd, env=env, stdout=logs[role], stderr=subprocess.STDOUT,
                                           start_new_session=True)
            if role == "sub":
                time.sleep(.05)
        deadline = start + max(seconds + 35, 180 if topics > 1 else 45)
        while time.monotonic() < deadline:
            codes = {r: p.poll() for r, p in procs.items()}
            if all(v is not None for v in codes.values()):
                break
            if any(v is not None and v != 0 for v in codes.values()):
                break
            time.sleep(.1)
        else:
            result["timeout"] = True
    finally:
        for p in procs.values():
            terminate(p)
        for f in logs.values():
            f.close()
    result["binary_sha256"] = fingerprint(args.work / "build/comparison")
    result["wall_seconds"] = time.monotonic()-start
    result["finished_unix_ns"] = time.time_ns()
    roudi_end = proc_usage(roudi_pid)
    if roudi_start and roudi_end:
        result["roudi"] = dict(cpu_seconds=roudi_end["cpu"]-roudi_start["cpu"],
                              threads=roudi_end["threads"], rss_kib=roudi_end["rss_kib"],
                              window_seconds=result["wall_seconds"])
    result["exit"] = {r: p.returncode for r, p in procs.items()}
    for role in ["pub", "sub"]:
        p = d / f"{role}.json"
        if p.exists() and p.stat().st_mtime_ns >= result["started_unix_ns"]:
            result[role] = json.loads(p.read_text())
        lines = (d / f"{role}.log").read_text(errors="replace").splitlines()
        result[f"{role}_log_tail"] = lines[-12:]
        result[f"{role}_compat_lines"] = [s for s in lines if "compat" in s.lower() or "fallback" in s.lower()][-10:]
    result["status"] = "ok" if all(v == 0 for v in result["exit"].values()) and "pub" in result and "sub" in result else "failed"
    if result["status"] == "ok":
        pub, sub = result["pub"], result["sub"]
        reasons = []
        if mode != "rpc":
            if sub["bad"] or sub["duplicate"]:
                reasons.append("载荷错误或重复")
            if sub.get("gaps", 0):
                reasons.append("接收序号存在缺口（另核发送失败数）")
            if sub["received"] != pub["sent"]:
                reasons.append("发送接收数不守恒")
            key = {"shm": "tlv", "a": "a", "b": "b", "prebuilt": "prebuilt"}.get(backend)
            if key and pub[key] != pub["sent"]:
                reasons.append("请求路径计数不等于成功发送数")
            if sub["topics_covered"] != topics:
                reasons.append("存在未收消息的话题")
        if sub.get("reply_bad", 0):
            reasons.append("服务请求内容校验失败")
        if backend == "dds-iox" and (not pub["dds_shm_endpoints"] or not sub["dds_shm_endpoints"] or not pub.get("loan_in_shm", 0)):
            reasons.append("DDS 共享内存端点不可用")
        if pub.get("stalled", 0):
            reasons.append("在途窗口停止推进")
        if pub["sent"] != pub["plan"]:
            reasons.append("实际输入未达计划")
        if mode != "rpc" and any(t["expected"] != t["delivered"] for t in sub.get("topic_delivery", [])):
            reasons.append("每话题最终交付不守恒")
        if pub["failed"]:
            reasons.append("发送/请求失败")
        if reasons:
            result["status"] = "degraded"
            result["reasons"] = reasons
    atomic_json(result_path, result)
    print(f'{case_id}: {result["status"]}, {result["wall_seconds"]:.1f}s', flush=True)
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("suite", choices=["smoke", "speed", "stress"])
    p.add_argument("--work", type=pathlib.Path, default=pathlib.Path("/var/tmp/cppipc-comparison-current"))
    p.add_argument("--dds-root", type=pathlib.Path, default=pathlib.Path("/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2"))
    p.add_argument("--backends", default=",".join(BACKENDS))
    p.add_argument("--modes", default="pubsub,rpc")
    p.add_argument("--sizes", default="")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--seconds", type=float, default=None)
    p.add_argument("--topics", default="1,100,1000")
    p.add_argument("--domain", type=int, default=173)
    p.add_argument("--workers", type=int, default=32)
    p.add_argument("--publishers", type=int, default=4)
    p.add_argument("--build", action="store_true")
    p.add_argument("--rerun", action="store_true")
    args = p.parse_args()
    args.work = args.work.resolve()
    if args.work == ROOT or ROOT in args.work.parents:
        p.error("中间目录必须在仓库外，防止中间产物进入交付")
    args.work.mkdir(parents=True, exist_ok=True)
    (args.work / "cases").mkdir(exist_ok=True)
    if args.build:
        subprocess.run(["cmake", "-S", str(HERE), "-B", str(args.work / "build"), f"-DDDS_ROOT={args.dds_root}"], check=True)
        subprocess.run(["cmake", "--build", str(args.work / "build"), "-j8"], check=True)
    environment = args.work / "environment.json"
    if True:
        atomic_json(environment, dict(
            started=command_output(["date", "-Iseconds"]), git=command_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
            git_status=command_output(["git", "-C", str(ROOT), "status", "--short"]),
            uname=command_output(["uname", "-a"]), cpu=command_output(["lscpu"]),
            memory=command_output(["free", "-h"]), compiler=command_output(["c++", "--version"]),
            loaded=command_output(["ldd", str(args.work / "build/comparison")]),
            binary_sha256=fingerprint(args.work / "build/comparison"),
            library_sha256=fingerprint(args.work / "build/lib/libipc.so"),
            roudi=command_output(["pgrep", "-a", "iox-roudi"]),
            roudi_config=pathlib.Path(os.environ.get("COMPARISON_ROUDI_CONFIG", "/etc/iceoryx/roudi_config.toml")).read_text(),
            sources={f.name:fingerprint(f) for f in HERE.iterdir() if f.is_file()},
            flags=(args.work / "build/ipc/CMakeFiles/ipc.dir/flags.make").read_text(),
            workers=args.workers, domain=args.domain))
    backends = args.backends.split(",")
    if args.suite == "smoke":
        sizes = [int(s) for s in args.sizes.split(",")] if args.sizes else [8, 1048576]
        for mode in args.modes.split(","):
            for size in sizes:
                for backend in backends:
                    run_case(args, backend, mode, size, 1, 0, args.seconds or .15, full=True)
    elif args.suite == "speed":
        sizes = [int(s) for s in args.sizes.split(",")] if args.sizes else SIZES
        for repeat in range(1,args.rounds+1):
            for mode in args.modes.split(","):
                for size in sizes:
                    # 确定性轮换顺序，避免总把同一后端放在升温前/后。
                    shift=(repeat+sizes.index(size))%len(backends)
                    for backend in backends[shift:]+backends[:shift]:
                        run_case(args, backend, mode, size, 1, repeat, args.seconds or 1.0)
    else:
        for repeat in range(1,args.rounds+1):
            for topics in map(int,args.topics.split(",")):
                for backend in backends:
                    run_case(args, backend, "stress", 64, topics, repeat, args.seconds or 10.0)

if __name__ == "__main__":
    # /var/tmp 在本工装的私有挂载命名空间间共享，故可阻止速度/压力意外并行。
    with open("/var/tmp/cppipc-comparison.run.lock", "a") as guard:
        fcntl.flock(guard.fileno(), fcntl.LOCK_EX)
        main()
