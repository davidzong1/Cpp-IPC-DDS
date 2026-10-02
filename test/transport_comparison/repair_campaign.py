#!/usr/bin/env python3
"""执行修复验证阶段；逐阶段记录退出码，异常不删除，也不阻止独立路径继续验证。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ORIGINAL_ROUDI=Path("/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi")
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--phase",choices=["correctness","comparison","full"],required=True)
    p.add_argument("--roudi",type=Path,default=ORIGINAL_ROUDI)
    a=p.parse_args();w=a.work.resolve()
    if w==HERE.parents[1] or HERE.parents[1] in w.parents:
        p.error("运行目录必须在仓库外")
    steps=[]
    def add(name,build,stage,backends,seconds,rounds,extra=()):
        steps.append((name,build,[sys.executable,"-B",str(HERE/"repair_runs.py"),
                     "--work",str(w/build),"--stage",stage,"--backends",backends,
                     "--seconds",str(seconds),"--rounds",str(rounds),"--tag",name,*extra]))
    if a.phase=="correctness":
        add("final-quick","final","quick","shm,socket,a,b,prebuilt,dds-udp,dds-iox",3,2)
        add("final-idle","final","idle","a,b",120,1)
        add("final-short","final","stress","shm",10,5)
        add("final-long","final","stress","shm",60,3,("--topics","1000"))
        add("final-capacity","final","stress","a,b",10,3,("--topics","1000"))
        add("dds-original-capacity","final","stress","dds-iox",2,1,("--topics","1000"))
        add("dds-scaled-short","scaled-final","stress","dds-iox",10,5,("--topics","1000"))
        add("dds-scaled-long","scaled-final","stress","dds-iox",60,3,("--topics","1000"))
    elif a.phase=="comparison":
        steps.append(("final-socket-speed","final",[sys.executable,"-B",str(HERE/"run.py"),"speed",
                      "--work",str(w/"final"),"--backends","socket","--modes","pubsub",
                      "--sizes","131072,262144,524288,1048576","--seconds","3","--rounds","3"]))
        add("final-input","final","baseline","a,b,dds-iox",10,5)
        add("final-control","final","control","a,b,dds-iox",10,5)
        steps.append(("final-probe","final",[sys.executable,"-B",str(HERE/"repair_probe.py"),
                      "--untraced",str(w/"final"),"--traced",str(w/"trace-final"),
                      "--out",str(w/"probe-final"),"--seconds","3","--rounds","3"]))
    else:
        add("final-full","final","full","shm,socket,a,b,prebuilt,dds-udp,dds-iox",10,3)
        add("final-bytes","final","correctness","shm,socket,a,b,prebuilt,dds-udp,dds-iox",1,1)
    results=[]
    for name,build,cmd in steps:
        target=w/build
        config=target/"roudi.toml"
        if not config.exists():
            subprocess.run([sys.executable,"-B",str(HERE/"prepare_roudi.py"),str(config)],check=True)
        roudi=w/"scaled-deps/prefix/bin/iox-roudi" if build=="scaled-final" else a.roudi
        command=["bash",str(HERE/"isolated.sh"),str(roudi),str(config),str(w/(name+"-roudi.log")),*cmd]
        print("开始阶段："+name,flush=True)
        env=os.environ.copy()
        env.update(COMPARISON_ROUDI_CONFIG=str(config),COMPARISON_WINDOW="8",COMPARISON_RATE="0",
                   COMPARISON_IDLE_NS="0",COMPARISON_COUNT="0",COMPARISON_NO_RETRY="1")
        with (w/(name+".log")).open("a") as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env)
        status_code=result.returncode
        if name=="final-socket-speed":
            rows=[json.loads(q.read_text()) for q in (target/"cases").glob("speed-pubsub-socket-*.json")]
            if len(rows)!=12 or any(r["status"]!="ok" for r in rows):status_code=1
        results.append({"name":name,"exit":status_code,"command":command,
                        "roudi_sha256":hashlib.sha256(roudi.read_bytes()).hexdigest()})
        out=w/(a.phase+"-campaign.json");tmp=out.with_suffix(".new")
        tmp.write_text(json.dumps(results,ensure_ascii=False,indent=2));tmp.replace(out)
        print(f"结束阶段：{name}，进程退出码 {result.returncode}，验收状态码 {status_code}",flush=True)
    return any(x["exit"] for x in results)

if __name__=="__main__":
    raise SystemExit(main())
