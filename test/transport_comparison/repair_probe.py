#!/usr/bin/env python3
"""同一产品版本的无插桩 U、插桩关闭 O、抽样开启 T 交错对照。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random

HERE=Path(__file__).resolve().parent
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result
base=module("comparison_runner",HERE/"run.py")
trace=module("trace_runner",HERE.parent/"latency_breakdown/run.py")

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--untraced",type=Path,required=True)
    p.add_argument("--traced",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--seconds",type=float,default=3)
    p.add_argument("--rounds",type=int,default=3)
    a=p.parse_args()
    variants={"U":a.untraced.resolve(),"O":a.traced.resolve(),"T":a.traced.resolve()}
    a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
    if any(q==base.ROOT or base.ROOT in q.parents for q in [a.out,*variants.values()]):
        p.error("运行目录必须在仓库外")
    config={"seconds":a.seconds,"rounds":a.rounds,"affinity":sorted(os.sched_getaffinity(0)),
            "runner":base.fingerprint(HERE/"run.py"),"script":base.fingerprint(Path(__file__)),
            "summarizer":base.fingerprint(HERE.parent/"latency_breakdown/run.py"),
            "variants":{v:{"binary":base.fingerprint(w/"build/comparison"),
                           "library":base.fingerprint(w/"build/lib/libipc.so")} for v,w in variants.items()}}
    manifest=a.out/"probe-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text())!=config:
        raise RuntimeError("探针对照配置变化，不能复用旧记录")
    base.atomic_json(manifest,config)
    rows=[]
    specifications=[(8,8,0),(8,8,500000),(1024,8,0),(1024,8,500000),
                    (65536,1,0),(1048576,1,0)]
    for repeat in range(1,a.rounds+1):
        jobs=[(v,b,n,w,rate) for v in variants for b in ("a","b","dds-iox") for n,w,rate in specifications]
        random.Random(20261002+repeat).shuffle(jobs)
        for v,b,n,window,rate in jobs:
            work=variants[v];(work/"cases").mkdir(exist_ok=True)
            os.environ.update(COMPARISON_WINDOW=str(window),COMPARISON_RATE=str(rate),
                              COMPARISON_NO_RETRY="1",COMPARISON_IDLE_NS="0",COMPARISON_COUNT="0",
                              BREAKDOWN_TRACE=str(int(v=="T")),BREAKDOWN_PHASE=str((repeat*17+11)%64))
            args=argparse.Namespace(suite=f"probe-{v}-w{window}-r{rate}",work=work,rerun=False,
                                    workers=32,publishers=1,domain=178)
            d=base.run_case(args,b,"pubsub",n,1,repeat,a.seconds)
            d.update(variant=v,window=window,offered_rate=rate)
            if v=="T" and d["status"]!="failed":
                d["breakdown"]=trace.summarize(work,d)
                bd=d["breakdown"]
                if bd["incomplete"] or bd["nonmonotonic"] or not bd["matched"]:
                    d["status"]="degraded"
                    d.setdefault("reasons",[]).append("分段关联不完整或时序错误")
            base.atomic_json(work/"cases"/(d["id"]+".json"),d)
            rows.append(d);base.atomic_json(a.out/"probe-results.json",rows)
    return any(x["status"]!="ok" for x in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
