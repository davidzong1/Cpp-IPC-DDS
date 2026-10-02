#!/usr/bin/env python3
"""独立采样 /proc，拆分接收线程的用户态/内核态 CPU；不修改产品，不需要 perf 权限。"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import random
import struct
import threading
import time

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("runner",HERE/"run.py")
assert spec is not None and spec.loader is not None
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
HZ=os.sysconf("SC_CLK_TCK")
def snapshot(pid,tid):
    path=Path(f"/proc/{pid}/task/{tid}")
    fields=(path/"stat").read_text().rsplit(")",1)[1].split()
    status={}
    for line in (path/"status").read_text().splitlines():
        if line.startswith(("voluntary_ctxt_switches:","nonvoluntary_ctxt_switches:")):
            key,value=line.split(":",1);status[key]=int(value)
    return {"when":time.monotonic_ns(),"user":int(fields[11]),"system":int(fields[12]),
            "start":int(fields[19]),"cpu":int(fields[36]),"status":status}

def profile(args,workers,repeat):
    stop=threading.Event();samples={};errors=[]
    binary=str(args.work/"build/comparison")
    def observe():
        while not stop.wait(.1):
            # run_case 在主线程创建的两个子进程；不遍历其他用户进程。
            children=Path(f"/proc/{os.getpid()}/task/{os.getpid()}/children").read_text().split()
            for pid in children:
                try:
                    command=Path(f"/proc/{pid}/cmdline").read_bytes().decode().split("\0")
                    if command[0]!=binary:continue
                    role=command[3];control=Path(command[8])
                    with control.open("rb") as f:
                        _,_,start,end=struct.unpack("QQQQ",f.read(32))
                    if not start<=time.monotonic_ns()<=end:continue
                    for task in Path(f"/proc/{pid}/task").iterdir():
                        s=snapshot(pid,task.name)
                        key=(role,int(task.name),s["start"])
                        if key not in samples:samples[key]={"first":s,"last":s,"n":1,"cpus":{s["cpu"]},"pid":int(pid)}
                        else:
                            row=samples[key];row["last"]=s;row["n"]+=1;row["cpus"].add(s["cpu"])
                except (FileNotFoundError,ProcessLookupError):
                    # 角色/线程在采样期间退出，不借用其他进程数据。
                    continue
                except Exception as exc:
                    errors.append(str(exc))
    begin_cpu=time.process_time();thread=threading.Thread(target=observe);thread.start()
    os.environ.update(COMPARISON_WINDOW="8",COMPARISON_RATE="0",COMPARISON_NO_RETRY="1",
                      COMPARISON_IDLE_NS="0",COMPARISON_COUNT="0",BREAKDOWN_TRACE="0")
    runargs=argparse.Namespace(suite=f"cpu-w{workers}",work=args.work,rerun=False,
                              workers=workers,publishers=4,domain=179)
    try:
        result=base.run_case(runargs,args.backend,"stress",64,args.topics,repeat,args.seconds)
        if args.backend=="shm" and (result.get("sub",{}).get("pool_routes")!=args.topics
                                   or result.get("sub",{}).get("pool_workers")!=workers):
            result["status"]="degraded"
            result.setdefault("reasons",[]).append("目标 worker/route 容量未全部建立")
    finally:
        stop.set();thread.join()
    if not samples:
        raise RuntimeError("未采到测量窗口；不要将已有 case 缓存当作新采样，请用新目录或轮次")
    rows=[]
    for (role,tid,_),s in sorted(samples.items()):
        first,last=s["first"],s["last"];seconds=(last["when"]-first["when"])/1e9
        rows.append({"role":role,"tid":tid,"main":tid==s["pid"],"samples":s["n"],"seconds":seconds,
                     "user_seconds":(last["user"]-first["user"])/HZ,
                     "system_seconds":(last["system"]-first["system"])/HZ,
                     "voluntary":last["status"].get("voluntary_ctxt_switches",0)-first["status"].get("voluntary_ctxt_switches",0),
                     "involuntary":last["status"].get("nonvoluntary_ctxt_switches",0)-first["status"].get("nonvoluntary_ctxt_switches",0),
                     "observed_cpus":sorted(s["cpus"])})
    roles={}
    for role in ("pub","sub"):
        selected=[r for r in rows if r["role"]==role and r["seconds"]>0]
        roles[role]={"user_cores":sum(r["user_seconds"]/r["seconds"] for r in selected),
                     "system_cores":sum(r["system_seconds"]/r["seconds"] for r in selected),
                     "threads_sampled":len(selected)}
    result["cpu_profile"]={"interval_ms":100,"clock_ticks_per_second":HZ,
                           "sampler_cpu_seconds":time.process_time()-begin_cpu,
                           "roles":roles,"threads":rows,"errors":errors}
    base.atomic_json(args.work/"cases"/(result["id"]+".json"),result)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--backend",default="shm",choices=["shm","dds-iox"])
    p.add_argument("--workers",type=int,nargs="+",default=[16,32,64])
    p.add_argument("--topics",type=int,default=1000)
    p.add_argument("--seconds",type=float,default=10)
    p.add_argument("--rounds",type=int,default=3)
    a=p.parse_args();a.work=a.work.resolve()
    if a.work==base.ROOT or base.ROOT in a.work.parents:p.error("运行目录必须在仓库外")
    a.work.joinpath("cases").mkdir(exist_ok=True)
    config={**vars(a),"work":str(a.work),"binary":base.fingerprint(a.work/"build/comparison"),
            "library":base.fingerprint(a.work/"build/lib/libipc.so"),"script":base.fingerprint(Path(__file__)),
            "runner":base.fingerprint(HERE/"run.py"),"affinity":sorted(os.sched_getaffinity(0))}
    manifest=a.work/"cpu-profile-manifest.json"
    if manifest.exists() and json.loads(manifest.read_text())!=config:raise RuntimeError("CPU 采样配置变化，不能复用旧数据")
    base.atomic_json(manifest,config)
    rows=[]
    for repeat in range(1,a.rounds+1):
        workers=a.workers.copy();random.Random(20261002+repeat).shuffle(workers)
        for n in workers:
            case=a.work/"cases"/f"cpu-w{n}-stress-{a.backend}-64-n{a.topics}-r{repeat}.json"
            if case.exists():
                r=json.loads(case.read_text())
                if "cpu_profile" not in r:raise RuntimeError("上次采样中断，请保留异常记录后选择新目录")
            else:r=profile(a,n,repeat)
            rows.append(r);base.atomic_json(a.work/"cpu-profile-results.json",rows)
            print(n,repeat,r["status"],r["cpu_profile"]["roles"],flush=True)
    return any(r["status"]!="ok" or r["cpu_profile"]["errors"] for r in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
