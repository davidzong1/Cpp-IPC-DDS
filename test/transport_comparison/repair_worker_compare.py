#!/usr/bin/env python3
"""无采样交错比较接收 worker 数；产品、输入、发布者及 CPU 集合固定。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("runner",HERE/"run.py")
assert spec is not None and spec.loader is not None
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work",type=Path,required=True)
    parser.add_argument("--workers",type=int,nargs="+",default=[32,64])
    parser.add_argument("--rounds",type=int,default=5)
    parser.add_argument("--seconds",type=float,default=10)
    args=parser.parse_args();args.work=args.work.resolve()
    if args.work==base.ROOT or base.ROOT in args.work.parents:parser.error("运行目录必须在仓库外")
    if args.rounds<1 or args.seconds<=0 or any(n<8 or n>64 for n in args.workers):
        parser.error("千话题使用 8–64 个 worker，时长与轮次必须为正")
    config={**vars(args),"work":str(args.work),"binary":base.fingerprint(args.work/"build/comparison"),
            "library":base.fingerprint(args.work/"build/lib/libipc.so"),
            "script":base.fingerprint(Path(__file__)),"runner":base.fingerprint(HERE/"run.py"),
            "affinity":sorted(os.sched_getaffinity(0))}
    manifest=args.work/"worker-compare-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text())!=config:
        raise RuntimeError("配置或源码改变，请用新的独立目录")
    base.atomic_json(manifest,config)
    os.environ.update(COMPARISON_WINDOW="8",COMPARISON_RATE="0",COMPARISON_NO_RETRY="1",
                      COMPARISON_IDLE_NS="0",COMPARISON_COUNT="0",BREAKDOWN_TRACE="0")
    rows=[]
    for repeat in range(1,args.rounds+1):
        workers=args.workers.copy();random.Random(20261002+repeat).shuffle(workers)
        for count in workers:
            runargs=argparse.Namespace(work=args.work,suite=f"workers-w{count}",rerun=False,
                                       workers=count,publishers=4,domain=181)
            result=base.run_case(runargs,"shm","stress",64,1000,repeat,args.seconds)
            if result.get("sub",{}).get("pool_workers")!=count or result.get("sub",{}).get("pool_routes")!=1000:
                result["status"]="degraded"
                result.setdefault("reasons",[]).append("目标 worker/route 未全部建立")
            result["configured_workers"]=count
            base.atomic_json(args.work/"cases"/(result["id"]+".json"),result)
            rows.append(result);base.atomic_json(args.work/"worker-compare-results.json",rows)
            print(count,repeat,result["status"],flush=True)
    return any(r["status"]!="ok" for r in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
