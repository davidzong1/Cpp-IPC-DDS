#!/usr/bin/env python3
"""小消息分段计时：串行运行、沿用原工装收发及正确性判定，按序号关联本地探针。"""
import argparse
import csv
import fcntl
import hashlib
import importlib.util
import json
import os
import pathlib
import random
import statistics
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("transport_runner", HERE.parent / "transport_comparison/run.py")
assert spec is not None and spec.loader is not None
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
PROFILES = {"w1": (1, 0), "w8": (8, 0), "r100k": (8, 100000), "r500k": (8, 500000)}
FIELDS = ["produced","publish_enter","submit_begin","publish_return","received",
          "lease_released","validated","allocated","queue_visible","take_begin","take_end","app","worker_enter"]

def stats(values):
    v = sorted(values)
    if not v:
        return None
    return {"n":len(v),"mean":statistics.mean(v),"p50":v[(len(v)-1)//2],
            "p99":v[min(len(v)-1, int(len(v)*.99))],"max":v[-1]}

def read_trace(path):
    with path.open() as f:
        return {int(d["seq"]): {k:int(d[k]) for k in FIELDS} for d in csv.DictReader(f)}

def summarize(work, d):
    directory = work / "runs" / d["id"]
    pub = read_trace(directory / "pub.json.trace.csv")
    sub = read_trace(directory / "sub.json.trace.csv")
    samples = {seq:p for seq,p in pub.items() if seq>32 and p["produced"]}
    segments = {}
    incomplete, invalid = [], []
    pairs = []
    for seq,p in samples.items():
        s = sub.get(seq, {})
        common = [p["produced"],p["publish_enter"]]
        if d["backend"] == "dds-iox":
            times = common + [s.get("take_end",0),s.get("app",0)]
            names = ["prepare","enter_to_take","take_to_app"]
        else:
            times = common + [p["submit_begin"],s.get("received",0),s.get("lease_released",0),
                              s.get("validated",0),s.get("allocated",0),s.get("queue_visible",0),s.get("app",0)]
            names = ["prepare","before_submit","submit_to_recv","release","validate","allocate","enqueue","queue_to_app"]
        if not all(times):
            incomplete.append(seq)
            continue
        if any(y<x for x,y in zip(times,times[1:])):
            invalid.append(seq)
            continue
        deltas = [y-x for x,y in zip(times,times[1:])]
        assert sum(deltas) == times[-1]-times[0]
        for name,delta in zip(names,deltas):
            segments.setdefault(name,[]).append(delta/1000)
        segments.setdefault("total",[]).append((times[-1]-times[0])/1000)
        segments.setdefault("publish_call",[]).append((p["publish_return"]-p["publish_enter"])/1000)
        if d["backend"] == "dds-iox":
            segments.setdefault("take_batch",[]).append((s["take_end"]-s["take_begin"])/1000)
        else:
            segments.setdefault("recv_to_app",[]).append((s["app"]-s["received"])/1000)
            worker_start=s["worker_enter"]
            if not worker_start or worker_start>s["received"]:
                raise RuntimeError("worker 起始点缺失或晚于取包")
            boundary=max(worker_start,p["submit_begin"])
            segments.setdefault("before_worker",[]).append((boundary-p["submit_begin"])/1000)
            segments.setdefault("worker_after_submit",[]).append((s["received"]-boundary)/1000)
            segments.setdefault("worker_service",[]).append((s["received"]-worker_start)/1000)
        pairs.append(seq)
    return {"sampled_sent":len(samples),"matched":len(pairs),"incomplete":incomplete,
            "nonmonotonic":invalid,"segments":{k:stats(v) for k,v in segments.items()}}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--work",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-breakdown-20261002"))
    ap.add_argument("--smoke",action="store_true")
    ap.add_argument("--seconds",type=float,default=3)
    a = ap.parse_args()
    a.work = a.work.resolve()
    if a.work == ROOT or ROOT in a.work.parents:
        ap.error("中间目录必须在仓库外")
    (a.work / "cases").mkdir(exist_ok=True)
    env = {
        "git":base.command_output(["git","rev-parse","HEAD"]),
        "date":base.command_output(["date","-Iseconds"]),
        "cpu":base.command_output(["lscpu"]),
        "uname":base.command_output(["uname","-a"]),
        "compiler":base.command_output(["c++","--version"]),
        "ldd":base.command_output(["ldd",str(a.work / "build/comparison")]),
        "binary_sha256":base.fingerprint(a.work / "build/comparison"),
        "library_sha256":base.fingerprint(a.work / "build/lib/libipc.so"),
        "affinity":sorted(os.sched_getaffinity(0)),
        "seconds":a.seconds,"sample_stride":64,
        "source_sha256":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in HERE.iterdir() if p.is_file()}
    }
    base.atomic_json(a.work / ("smoke_environment.json" if a.smoke else "environment.json"),env)
    specifications = []
    if a.smoke:
        specifications = [(0,8,"w8",True,b) for b in ("a","b","dds-iox")]
    else:
        for repeat in (1,2,3):
            block = []
            for n in (8,1024,65536):
                for profile in (("w1","w8") if n==65536 else PROFILES):
                    for b in ("a","b","dds-iox"):
                        block.append((repeat,n,profile,True,b))
                        if n==8:
                            block.append((repeat,n,profile,False,b))
            random.Random(20261002+repeat).shuffle(block)
            specifications.extend(block)
    for count,(repeat,n,profile,on,b) in enumerate(specifications,1):
        window,rate = PROFILES[profile]
        os.environ.update(BREAKDOWN_TRACE=str(int(on)),BREAKDOWN_PHASE=str((repeat*17+11)%64),
                          BREAKDOWN_WINDOW=str(window),BREAKDOWN_RATE=str(rate))
        args = argparse.Namespace(suite=f"{'probe' if a.smoke else 'stage'}-{profile}-{'on' if on else 'off'}",
                work=a.work,rerun=False,workers=32,publishers=1,domain=174)
        d = base.run_case(args,b,"pubsub",n,1,repeat,.4 if a.smoke else a.seconds,full=a.smoke)
        d.update(profile=profile,trace=on,window=window,offered_rate=rate)
        if d["status"] == "failed":
            raise RuntimeError(f"测试进程失败：{d['id']}，请查日志")
        if on:
            d["breakdown"] = summarize(a.work,d)
            if d["breakdown"]["nonmonotonic"] or not d["breakdown"]["matched"]:
                raise RuntimeError(f"探针时序不正确：{d['id']} {d['breakdown']}")
        base.atomic_json(a.work / "cases" / (d["id"]+".json"),d)
        print(f"进度 {count}/{len(specifications)}，有效分段样本 {d.get('breakdown',{}).get('matched','关闭')}",flush=True)

if __name__ == "__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        main()
