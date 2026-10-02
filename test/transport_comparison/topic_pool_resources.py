#!/usr/bin/env python3
"""串行构建并测量千话题十块池；临时产物写入仓库外，使用私有共享内存。"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--roudi",type=Path,required=True)
    p.add_argument("--config",type=Path,required=True)
    a=p.parse_args();w=a.work.resolve()
    if w==ROOT or ROOT in w.parents:p.error("运行目录必须在仓库外")
    folder=w/"resources";folder.mkdir(exist_ok=True)
    if (folder/"results.json").exists():p.error("已有结果，请归档后使用新的运行目录")
    source=HERE/"topic_pool_resources.cpp";binary=folder/"probe";lib=w/"final/build/lib"
    subprocess.run(["c++","-std=c++17","-O2",str(source),"-I"+str(ROOT/"include"),
                    "-L"+str(lib),"-Wl,-rpath,"+str(lib),"-lipc","-pthread","-lrt","-o",str(binary)],check=True)
    hashes={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [source,binary,lib/"libipc.so"]}
    (folder/"manifest.json").write_text(json.dumps(hashes,indent=2))
    rows=[]
    for size in [64,1024,1048577]:
        with (folder/f"{size}.out").open("w") as out,(folder/f"{size}.err").open("w") as err:
            cp=subprocess.run(["bash",str(HERE/"isolated.sh"),str(a.roudi.resolve()),str(a.config.resolve()),
                               str(folder/f"{size}-roudi.log"),str(binary),"1000",str(size)],
                              stdout=out,stderr=err,timeout=180)
        checkpoints=[json.loads(s) for s in (folder/f"{size}.out").read_text().splitlines() if s.startswith("{")]
        row={"topics":1000,"requested_bytes":size,"exit":cp.returncode,"checkpoints":checkpoints}
        rows.append(row)
        (folder/"results.json").write_text(json.dumps(rows,indent=2))
        print(json.dumps(row),flush=True)
    return any(r["exit"] or len(r["checkpoints"])!=4 for r in rows)

if __name__=="__main__":
    with open("/var/tmp/cppipc-comparison.run.lock","a") as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX)
        raise SystemExit(main())
