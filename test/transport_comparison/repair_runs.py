#!/usr/bin/env python3
"""修复验证矩阵；借用原隔离运行器，逐格保存结果并在配置一致时续跑。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("comparison_runner", HERE / "run.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--stage", choices=["quick", "progress", "baseline", "stress", "full", "control", "idle", "correctness"], required=True)
    p.add_argument("--backends", default="a,b,dds-iox")
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--workers", type=int, default=32)
    p.add_argument("--trace", type=int, choices=[0, 1], default=0)
    p.add_argument("--tag", default="repair")
    p.add_argument("--topics", default="1,100,1000", help="压力档话题数，逗号分隔")
    p.add_argument("--count", type=int, default=1000, help="静默档目标条数")
    p.add_argument("--idle-ms", type=float, default=100, help="静默档确认交付后等待毫秒数")
    a = p.parse_args()
    a.work = a.work.resolve()
    backends = a.backends.split(",")
    topic_counts = [int(x) for x in a.topics.split(",")]
    if any(b not in base.BACKENDS for b in backends) or any(n<1 or n>1000 for n in topic_counts):
        p.error("后端或话题数无效")
    if a.seconds<=0 or a.rounds<1 or a.count<1 or a.idle_ms<=0:
        p.error("时长、轮次、条数与静默时间必须为正数")
    if a.stage=="idle" and a.seconds < a.count*a.idle_ms/1000*1.1:
        p.error("静默档总时长需留至少 10% 调度余量")
    if a.work == base.ROOT or base.ROOT in a.work.parents:
        p.error("运行目录必须在仓库外")
    (a.work / "cases").mkdir(exist_ok=True)
    jobs = []
    for repeat in range(1, a.rounds + 1):
        block = []
        for backend in backends:
            if a.stage == "quick":
                block += [(backend, "pubsub", n, 1, "w1", 1, 0, True, repeat) for n in (8, 1024, 1048576)]
                if backend not in ("b", "prebuilt"):
                    block += [(backend, "rpc", n, 1, "rpc", 1, 0, True, repeat) for n in (8, 1048576)]
            elif a.stage in ("progress", "baseline"):
                profiles = [("w1", 1, 0), ("w8", 8, 0)] if a.stage == "progress" else [("r100k", 8, 100000), ("r500k", 8, 500000), ("w8", 8, 0)]
                block += [(backend, "pubsub", n, 1, label, window, rate, False, repeat)
                          for n in (8, 1024) for label, window, rate in profiles]
            elif a.stage == "stress":
                block += [(backend, "stress", 64, n, "stress", 8, 0, False, repeat) for n in topic_counts]
            elif a.stage == "control":
                block += [(backend, "pubsub", n, 1, f"w{w}-{'bytes' if full else 'view'}", w, 0, full, repeat)
                          for n in (65536, 1048576) for w in (1,8) for full in (False,True)]
            elif a.stage == "idle":
                block += [(backend, "pubsub", n, 1, "idle100", 1, 0, False, repeat) for n in (8,1024)]
            else:
                block += [(backend, mode, n, 1, a.stage, 8, 0, a.stage=="correctness", repeat)
                          for mode in ("pubsub", "rpc") for n in base.SIZES]
        random.Random(20261002 + repeat).shuffle(block)
        jobs += block
    config = dict(vars(a), work=str(a.work), binary=base.fingerprint(a.work / "build/comparison"),
                  library=base.fingerprint(a.work / "build/lib/libipc.so"),
                  runner=base.fingerprint(HERE / "run.py"), matrix=base.fingerprint(Path(__file__)),
                  affinity=sorted(os.sched_getaffinity(0)))
    manifest = a.work / f"{a.tag}-{a.stage}-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text()) != config:
        raise RuntimeError("续跑配置或二进制变化，请使用新的 tag/目录")
    base.atomic_json(manifest, config)
    for index, (backend, mode, n, topics, profile, window, rate, full, repeat) in enumerate(jobs, 1):
        os.environ.update(COMPARISON_WINDOW=str(window), COMPARISON_RATE=str(rate),
                          COMPARISON_NO_RETRY="1", BREAKDOWN_TRACE=str(a.trace),
                          BREAKDOWN_PHASE=str((repeat * 17 + 11) % 64),
                          COMPARISON_IDLE_NS=str(int(a.idle_ms*1e6) if a.stage=="idle" else 0),
                          COMPARISON_COUNT=str(a.count if a.stage=="idle" else 0))
        args = argparse.Namespace(suite=f"{a.tag}-{a.stage}-{profile}", work=a.work,
                                  rerun=False, workers=a.workers, publishers=4, domain=176)
        d = base.run_case(args, backend, mode, n, topics, repeat, a.seconds, full=full)
        if a.trace and mode=="pubsub" and backend in ("a","b","dds-iox") and d["status"]!="failed":
            trace_spec=importlib.util.spec_from_file_location("breakdown_runner",HERE.parent/"latency_breakdown/run.py")
            trace=importlib.util.module_from_spec(trace_spec);trace_spec.loader.exec_module(trace)
            d["breakdown"]=trace.summarize(a.work,d)
            bd=d["breakdown"]
            if bd["incomplete"] or bd["nonmonotonic"] or not bd["matched"]:
                d["status"]="degraded"
                d.setdefault("reasons",[]).append("分段样本关联不完整或时序错误")
            base.atomic_json(a.work/"cases"/(d["id"]+".json"),d)
        print(f"节点进度 {index}/{len(jobs)}：{d['status']}", flush=True)
    # 异常仍保留完整账本；退出码供上层区别“运行结束”和“验收通过”。
    results = [json.loads(q.read_text()) for q in (a.work / "cases").glob(f"{a.tag}-{a.stage}-*.json")]
    bad = [d["id"] for d in results if d["status"] not in ("ok", "unsupported")]
    base.atomic_json(a.work / f"{a.tag}-{a.stage}-summary.json", {"cases": len(results), "failed": bad})
    return bool(bad)

if __name__ == "__main__":
    with open("/var/tmp/cppipc-comparison.run.lock", "a") as guard:
        fcntl.flock(guard.fileno(), fcntl.LOCK_EX)
        raise SystemExit(main())
