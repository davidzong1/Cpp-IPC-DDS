#!/usr/bin/env python3
"""正确性修复基线与单个优化候选交错配对；不并行运行性能任务。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("base_runner",HERE/"run.py")
base=importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline",type=Path,required=True)
    p.add_argument("--candidate",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--tag",default="o1")
    p.add_argument("--rounds",type=int,default=5)
    p.add_argument("--seconds",type=float,default=10)
    a=p.parse_args()
    variants={"baseline":a.baseline.resolve(),"candidate":a.candidate.resolve()}
    a.out=a.out.resolve()
    if any(q==base.ROOT or base.ROOT in q.parents for q in [a.out,*variants.values()]):
        p.error("所有结果目录必须位于仓库外")
    a.out.mkdir(parents=True,exist_ok=True)
    config={"tag":a.tag,"rounds":a.rounds,"seconds":a.seconds,
            "affinity":sorted(os.sched_getaffinity(0)),
            "runner":base.fingerprint(HERE/"run.py"),
            "script":base.fingerprint(Path(__file__)),
            "variants":{name:{"work":str(w),"binary":base.fingerprint(w/"build/comparison"),
                              "library":base.fingerprint(w/"build/lib/libipc.so")} for name,w in variants.items()}}
    manifest=a.out/f"{a.tag}-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text())!=config:
        raise RuntimeError("配对实验配置变化，不能复用旧结果")
    base.atomic_json(manifest,config)
    rows=[]
    cases=[(b,"pubsub",8,1,label,rate) for b in ("a","b") for label,rate in (("r500k",500000),("w8",0))]
    cases.append(("shm","stress",64,1000,"million",0))
    for repeat in range(1,a.rounds+1):
        block=[(name,case) for case in cases for name in variants]
        random.Random(20261002+repeat).shuffle(block)
        for name,(backend,mode,n,topics,profile,rate) in block:
            work=variants[name];(work/"cases").mkdir(exist_ok=True)
            os.environ.update(COMPARISON_WINDOW="8",COMPARISON_RATE=str(rate),COMPARISON_NO_RETRY="1",BREAKDOWN_TRACE="0")
            args=argparse.Namespace(suite=f"{a.tag}-{name}-{profile}",work=work,rerun=False,workers=32,publishers=4,domain=177)
            d=base.run_case(args,backend,mode,n,topics,repeat,a.seconds)
            rows.append({"variant":name,"profile":profile,**d})
            base.atomic_json(a.out/f"{a.tag}-results.json",rows)
    return any(d["status"]!="ok" for d in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
