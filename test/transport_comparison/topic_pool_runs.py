#!/usr/bin/env python3
"""话题十块池的回归和旧/新对照；在 isolated.sh 中执行，中间产物仅放仓库外。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("runner",HERE/"run.py")
assert spec and spec.loader
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
UNITS=["test_topic_chunk_pool","test_loan","test_chunk_capacity_backpressure","test_chunk_hold",
       "test_dzflat_transport","test_w08_dzflat_ab","test_adopt_loan_quota","test_recv_worker",
       "test_recv_fragment_isolation","test_lifecycle_contract","test_recv_wait_set",
       "test_shm_route_session","test_dzflat_rx","test_shm_sniffer_control_name",
       "test_dzflat_fallback_semantics","test_dzflat_sercli","test_shm_ser_backpressure"]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--stage",choices=["units","quick","compare","long"],required=True)
    p.add_argument("--tag",default="topic10")
    a=p.parse_args();w=a.work.resolve()
    if w==base.ROOT or base.ROOT in w.parents:p.error("运行目录必须在仓库外")
    if not a.tag.replace("-","").replace("_","").isalnum():p.error("tag 只能含字母数字和短横线/下划线")
    config={"stage":a.stage,"tag":a.tag,"script":base.fingerprint(Path(__file__)),
            "runner":base.fingerprint(HERE/"run.py"),"affinity":sorted(os.sched_getaffinity(0))}
    variants=["baseline","final"] if a.stage=="compare" else ["final"]
    if a.stage=="units":
        config["programs"]={n:base.fingerprint(w/"units/build/bin"/n) for n in UNITS}
        libs=list((w/"units/build").rglob("libipc.so"))
        config["libraries"]={str(q.relative_to(w)):base.fingerprint(q) for q in libs}
    else:
        for v in variants:
            (w/v/"cases").mkdir(parents=True,exist_ok=True)
            config[v]={n:base.fingerprint(w/v/"build"/n) for n in ["comparison","lib/libipc.so"]}
    manifest=w/f"{a.tag}-{a.stage}-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text())!=config:raise RuntimeError("配置/构建变化，请使用新 tag 保留旧证据")
    base.atomic_json(manifest,config)
    out=w/f"{a.tag}-{a.stage}-results.json"
    rows=[]
    if a.stage=="units":
        folder=w/f"{a.tag}-units";folder.mkdir(exist_ok=True)
        for name in UNITS:
            record=folder/(name+".json")
            if record.exists():r=json.loads(record.read_text())
            else:
                env=os.environ.copy()
                env["W08_ARTIFACT_ROOT"]=str(folder/"w08")
                with (folder/(name+".log")).open("w") as log:
                    try:
                        cp=subprocess.run([str(w/"units/build/bin"/name)],stdout=log,stderr=subprocess.STDOUT,env=env,timeout=240)
                        code=cp.returncode
                    except subprocess.TimeoutExpired:code=124
                r={"test":name,"exit":code};base.atomic_json(record,r)
            rows.append(r);base.atomic_json(out,rows);print(name,r["exit"],flush=True)
        return any(r["exit"] for r in rows)
    jobs=[]
    if a.stage=="quick":
        for backend in ["shm","a","b","prebuilt"]:
            jobs += [(backend,"pubsub",n,1,1,0,True,3,1,"final") for n in [8,1024,1048576]]
        jobs += [(b,"rpc",n,1,1,0,True,3,1,"final") for b in ["shm","a"] for n in [8,1048576]]
    elif a.stage=="compare":
        for repeat in range(1,4):
            block=[]
            for variant in variants:
                for backend in ["a","b"]:
                    block += [(backend,"pubsub",n,1,8,rate,False,5,repeat,variant)
                              for n,rate in [(8,500000),(8,0),(1024,500000),(1048576,0)]]
                    block += [(backend,"stress",64,1000,8,0,False,10,repeat,variant)]
            random.Random(20261002+repeat).shuffle(block);jobs+=block
    else:
        jobs=[(b,"stress",64,1000,8,0,False,60,r,"final") for r in range(1,4) for b in ["a","b"]]
    for i,(backend,mode,n,topics,window,rate,full,seconds,repeat,variant) in enumerate(jobs,1):
        os.environ.update(COMPARISON_WINDOW=str(window),COMPARISON_RATE=str(rate),
                          COMPARISON_COUNT="0",COMPARISON_IDLE_NS="0",COMPARISON_NO_RETRY="1",BREAKDOWN_TRACE="0")
        args=argparse.Namespace(suite=f"{a.tag}-{a.stage}-w{window}-r{rate}",work=w/variant,
                                rerun=False,workers=32,publishers=4,domain=185)
        result=base.run_case(args,backend,mode,n,topics,repeat,seconds,full)
        result["variant"]=variant
        base.atomic_json(args.work/"cases"/(result["id"]+".json"),result)
        rows.append(result);base.atomic_json(out,rows)
        print(f"{i}/{len(jobs)} {variant} {result['status']}",flush=True)
    return any(r["status"] not in ("ok","unsupported") for r in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
