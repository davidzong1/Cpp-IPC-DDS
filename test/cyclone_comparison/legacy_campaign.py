#!/usr/bin/env python3
"""重跑既有速度/千话题工况；每窗独立命名空间，新增共享端点路径与原始时间值。"""
import argparse
import fcntl
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
from run import ROOT, HERE, dump, fingerprint, activity, environment, process_sample

spec=importlib.util.spec_from_file_location('legacy_comparison_run',ROOT/'test/transport_comparison/run.py')
base=importlib.util.module_from_spec(spec); spec.loader.exec_module(base)

def inside(path):
    c=json.loads(path.read_text()); work=path.parent
    (work/'build').symlink_to(c['comparison_build'],target_is_directory=True)
    (work/'cases').mkdir()
    os.environ.update(DZIPC_SHM_MPMC='1',DZIPC_NET_BACKEND='shared_v1' if c['backend']=='shared' else 'legacy',DZIPC_NET_TRACE='0')
    for key in list(os.environ):
        if key.startswith(('BREAKDOWN_','COMPARISON_','DZIPC_TEST_')): os.environ.pop(key)
    services={}; logs=[]; snapshots=[]; stop=threading.Event()
    try:
        if c['backend']=='dds-iox':
            config=work/'roudi.toml'
            config.write_text('[general]\nversion = 1\n[[segment]]\n'+''.join(f'[[segment.mempool]]\nsize = {s}\ncount = {n}\n' for s,n in ((128,10000),(1024,5000),(16384,1000),(131072,200),(1048576,50),(2097152,50))))
            log=(work/'roudi.log').open('w'); logs.append(log)
            services['roudi']=subprocess.Popen([c['roudi'],'-c',str(config)],stdout=log,stderr=subprocess.STDOUT)
            time.sleep(1); assert services['roudi'].poll() is None
            os.environ['COMPARISON_ROUDI_PID']=str(services['roudi'].pid)
        if c['backend']=='shared':
            private=Path(tempfile.mkdtemp(prefix='cc-speed-')); control=private/'control.sock'
            os.environ['DZIPC_GATEWAY_CONTROL']=str(control)
            ports,disc=base_ports()
            cmd=[c['gateway'],'serve','--control',str(control),'--interface','lo','--listen-ip','127.0.0.1','--data-base-port',str(ports[0]),
                 '--data-shards','4','--data-workers','4','--control-port',str(ports[0]+4),'--discovery-port',str(disc)]
            log=(work/'gateway.log').open('w'); logs.append(log)
            services['gateway']=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
            deadline=time.monotonic()+10
            while not control.exists():
                assert services['gateway'].poll() is None and time.monotonic()<deadline
                time.sleep(.01)
        def monitor():
            while not stop.is_set():
                children=Path(f'/proc/{os.getpid()}/task/{os.getpid()}/children').read_text().split()
                values=[]
                for pid in children:
                    try: values.append({'pid':int(pid),**process_sample(int(pid))})
                    except (FileNotFoundError,ProcessLookupError): pass
                snapshots.append({'time_ns':time.monotonic_ns(),'processes':values}); stop.wait(.5)
        t=threading.Thread(target=monitor); t.start()
        args=SimpleNamespace(work=work,suite=c['kind'],rerun=False,workers=32,publishers=4,domain=183)
        result=base.run_case(args,c['backend'],'stress' if c['kind']=='stress' else 'pubsub',c['bytes'],c['topics'],c['repeat'],c['seconds'],full=c['full'])
        stop.set(); t.join()
        result['runtime_snapshots']=snapshots
        if c['backend']=='shared':
            result['gateway_status']=json.loads(subprocess.check_output([c['gateway'],'status','--control',str(control),'--json'],text=True))
        dump(work/'result.json',result)
        return 0 if result['status']=='ok' else 1
    finally:
        stop.set()
        for p in services.values():
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        for log in logs: log.close()

def base_ports():
    from end_to_end import reserve_ports
    return reserve_ports(4)

def campaign(a):
    out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    if any(out.iterdir()): raise ValueError('输出目录非空')
    files=[a.comparison_build/'comparison',a.comparison_build/'lib/libipc.so',a.gateway,a.roudi,a.dds_root/'lib/libddsc.so',
           ROOT/'test/transport_comparison/comparison.cpp',ROOT/'test/transport_comparison/run.py',*HERE.glob('*.py'),HERE/'isolate.sh']
    frozen={str(p.resolve()):fingerprint(p) for p in files}
    jobs=[]
    for repeat in range(1,a.rounds+1):
        for topics in a.topics if a.kind=='stress' else [1]:
            for size in [64] if a.kind=='stress' else a.sizes:
                order=list(a.backends); shift=(repeat+size.bit_length())%len(order);order=order[shift:]+order[:shift]
                for backend in order:
                    jobs.append({'id':f'{a.kind}-r{repeat}-{backend}-b{size}-topics{topics}','kind':a.kind,'backend':backend,'bytes':size,'topics':topics,
                                 'repeat':repeat,'seconds':a.seconds,'full':a.full,'comparison_build':str(a.comparison_build.resolve()),'roudi':str(a.roudi.resolve()),'gateway':str(a.gateway.resolve())})
    manifest={'jobs':jobs,'windows':[],'sha256':frozen,'environment':environment(),
              'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'flags':(a.comparison_build/'ipc/CMakeFiles/ipc.dir/flags.make').read_text(),
              'benchmark_flags':(a.comparison_build/'CMakeFiles/comparison.dir/flags.make').read_text(),
              'loaded':subprocess.check_output(['ldd',str(a.comparison_build/'comparison')],text=True)}
    dump(out/'manifest.json',manifest)
    for index,c in enumerate(jobs,1):
        time.sleep(2)
        assert all(fingerprint(p)==sha for p,sha in frozen.items()), '产物发生变化'
        active=activity()
        assert not any(p['comm'] in ('git-maintenance','cc1plus','cc1','make','ninja','cmake','comparison','xproc_benchmark','benchmark') for p in active['processes']), '存在竞争负载'
        folder=out/c['id']; folder.mkdir(); dump(folder/'case.json',c)
        cmd=['unshare','--user','--map-root-user','--mount','--ipc','--fork','bash',str(HERE/'isolate.sh'),sys.executable,str(HERE/'legacy_campaign.py'),'--window',str(folder/'case.json')]
        state={'id':c['id'],'command':cmd,'activity':[]}
        with (folder/'console.log').open('w') as log:
            p=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
            while p.poll() is None: state['activity'].append(activity());time.sleep(1)
            state['returncode']=p.returncode
        result=json.loads((folder/'result.json').read_text()) if (folder/'result.json').exists() else {'status':'failed'}
        state['status']=result['status'];state['raw_sha256']={}
        for raw in folder.glob('runs/*/*.timing.csv'):
            state['raw_sha256'][str(raw.relative_to(folder))]=fingerprint(raw)
            with raw.open('rb') as src,gzip.open(str(raw)+'.gz','wb') as dst: shutil.copyfileobj(src,dst)
            raw.unlink()
        for control in folder.glob('runs/*/control'): control.unlink()
        if (folder/'build').is_symlink(): (folder/'build').unlink()
        manifest['windows'].append(state);dump(out/'manifest.json',manifest)
        print(json.dumps({'completed':index,'total':len(jobs),'case':c['id'],'status':state['status'],
                          'sent':result.get('pub',{}).get('sent'),'received':result.get('sub',{}).get('received')},ensure_ascii=False),flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--window',type=Path)
    p.add_argument('--kind',choices=['speed','stress','smoke'],default='speed');p.add_argument('--comparison-build',type=Path)
    p.add_argument('--gateway',type=Path);p.add_argument('--roudi',type=Path);p.add_argument('--dds-root',type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--backends',nargs='+',default=['shm','socket','a','b','prebuilt','shared','dds-udp','dds-iox'])
    p.add_argument('--topics',nargs='+',type=int,default=[1,100,1000]);p.add_argument('--sizes',nargs='+',type=int,default=[8<<k for k in range(18)])
    p.add_argument('--seconds',type=float,default=1);p.add_argument('--rounds',type=int,default=3);p.add_argument('--full',action='store_true')
    a=p.parse_args()
    if a.window:return inside(a.window)
    with open('/var/tmp/cppipc-comparison.run.lock','a') as guard:
        fcntl.flock(guard.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB);campaign(a)
    return 0

if __name__=='__main__':sys.exit(main())
