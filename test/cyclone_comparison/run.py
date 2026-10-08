#!/usr/bin/env python3
"""当前 dzIPC 与系统 Cyclone DDS 的串行阻塞式本机对照。"""
import argparse
import csv
import fcntl
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import traceback

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT/'test/shared_net'))
from end_to_end import Process, reserve_ports
from benchmark import sample

BACKENDS = ('shm', 'shared', 'dds-udp', 'dds-iox')
def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n')

def fingerprint(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def rows(path):
    opener = gzip.open if str(path).endswith('.gz') else open
    with opener(path, 'rt') as f:
        return [{k:int(v) for k,v in row.items()} for row in csv.DictReader(f)]

def stats(values):
    ordered = sorted(values)
    if not ordered:
        return {'count':0}
    return {'count':len(ordered),'mean_us':statistics.mean(ordered)/1000,
            **{f'p{p}_us':ordered[(len(ordered)*p+99)//100-1]/1000 for p in (50,95,99)},'max_us':ordered[-1]/1000}

def audit_raw(folder, c, result):
    def csv_path(name):
        path=folder/name
        return path if path.exists() else Path(str(path)+'.gz')
    publishers=c.get('publishers',1)
    pubs = [r for i in range(publishers) for r in rows(csv_path(f'pub{i}.csv'))]
    expected = {(i,s) for i in range(publishers) for s in range(2*c['rate'], (2+c['seconds'])*c['rate'])}
    key=lambda r:(r['publisher_id'],r['sequence'])
    errors=[]
    if len(pubs)!=len(expected) or {key(r) for r in pubs}!=expected or any(r['success']!=1 for r in pubs):
        errors.append('发布序号/计数/成功状态错误')
    indexed={key(r):r for r in pubs}
    latencies=[]; per_sub=[]; in_window=0; delivery=[]
    accepted_keys={key(r) for r in pubs if r['success']==1}
    for i in range(c['subscribers']):
        rr=rows(csv_path(f'sub{i}.csv'))
        if len(rr)!=len(expected) or {key(r) for r in rr}!=expected:
            errors.append(f'sub{i} 缺失、重复或意外序号')
        actual={key(r) for r in rr}; missing=accepted_keys-actual
        delivery.append({'subscriber':i,'missing_accepted':len(missing),'extra':len(actual-expected),'duplicates':len(rr)-len(actual),
                         'missing_first20':[list(k) for k in sorted(missing)[:20]],
                         'missing_by_publisher':{str(p):sum(k[0]==p for k in missing) for p in range(publishers)}})
        for r in rr:
            p=indexed.get(key(r))
            if not p or r['read_ns']-p['start_ns']!=r['elapsed_ns'] or r['elapsed_ns']<0 or r['payload_bytes']!=c['bytes'] or p['payload_bytes']!=c['bytes']:
                errors.append(f'sub{i} 时间/消息长度不一致'); break
        vals=[r['elapsed_ns'] for r in rr]
        latencies.extend(vals); per_sub.append(stats(vals))
        in_window+=sum(r['read_ns']<result['publisher']['window_end_ns'] for r in rr)
    if result['publisher']['rejected'] or any(s['invalid'] for s in result['receivers']):
        errors.append('拒收或逐字节校验失败')
    if result['publisher']['late_periods']:
        errors.append('发布迟发超过一个周期')
    if 'after' in result:
        after=result['after']; names=[f'pub{i}' for i in range(publishers)]+[f'sub{i}' for i in range(c['subscribers'])]
        if c['backend'] in ('shm','shared') and any(after[name]['udp'] for name in names):
            errors.append('dzIPC 业务进程出现 UDP socket')
        expected_lib=str((Path(c['build'])/'lib/libipc.so').resolve())
        if any(after[name]['loaded_libraries'] != [expected_lib] for name in names):
            errors.append('实际 libipc 路径不符')
        if any(t['Cpus_allowed_list']!='0-31' for name in names for t in after[name]['thread_status']):
            errors.append('业务线程 CPU 允许集合变化')
    events=sorted((t,kind,key(r)) for r in pubs for t,kind in ((r['start_ns'],1),(r['start_ns']+r['elapsed_ns'],-1)))
    active=set(); overlapped=set(); maximum=0
    for _,kind,k in events:
        if kind<0: active.discard(k)
        else:
            if active: overlapped.update(active); overlapped.add(k)
            active.add(k); maximum=max(maximum,len(active))
    return {'errors':errors,'publish':stats([r['elapsed_ns'] for r in pubs]),
            'api':stats([r['api_ns'] for r in pubs]),'receive':stats(latencies),'per_subscriber':per_sub,
            'received_in_window':in_window,'delivered_total':len(latencies),
            'accepted_total':sum(r['success'] for r in pubs),
            'aggregate_receive_rate':in_window/c['seconds'],
            'publishers':{str(i):stats([r['elapsed_ns'] for r in pubs if r['publisher_id']==i]) for i in range(publishers)},
            'concurrent_publish_max':maximum,'overlapped_publications':len(overlapped),
            'delivery':delivery,
            'schedule_late':stats([r['schedule_late_ns'] for r in pubs])}

def activity():
    found=[]
    for root in Path('/proc').iterdir():
        if not root.name.isdigit(): continue
        try:
            fields=(root/'stat').read_text().rsplit(')',1)[1].split()
            comm=(root/'comm').read_text().strip()
            if comm in ('git','git-pack-object','git-repack','git-gc'):
                argv=(root/'cmdline').read_bytes().split(b'\0')
                if any(x in argv for x in (b'gc',b'repack',b'pack-objects',b'maintenance')) or comm!='git':
                    comm='git-maintenance'
            if comm in ('git-maintenance','cc1plus','cc1','ninja','cmake','make','comparison','xproc_benchmark','shared_net_ben','benchmark','cpptools','python3','python','LSDHelper'):
                found.append({'pid':int(root.name),'comm':comm,'cpu_ticks':int(fields[11])+int(fields[12])})
        except (FileNotFoundError,ProcessLookupError,PermissionError): pass
    return {'time_ns':time.monotonic_ns(),'loadavg':list(os.getloadavg()),'processes':found}

def environment():
    idle=Path('/sys/devices/system/cpu/cpuidle/current_driver')
    return {'kernel':platform.platform(),'cpus':sorted(os.sched_getaffinity(0)),
            'idle_driver':idle.read_text().strip() if idle.exists() else None,
            'governors':{str(p):p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')},
            'core_types':{name:(Path('/sys/devices')/name/'cpus').read_text().strip() for name in ('cpu_core','cpu_atom') if (Path('/sys/devices')/name/'cpus').exists()},
            'activity':activity()}

def process_sample(proc):
    pid=proc if isinstance(proc,int) else proc.pid
    value=sample(pid)
    value['libraries']=sorted({line.split()[-1] for line in Path(f'/proc/{pid}/maps').read_text().splitlines()
                               if any(x in line for x in ('libipc.so','libddsc.so','libiceoryx'))})
    value['uid']=next(line for line in Path(f'/proc/{pid}/status').read_text().splitlines() if line.startswith('Uid:'))
    return value

def window(path):
    c=json.loads(path.read_text()); folder=path.parent
    binary=c['binary']; build=Path(c['build']); backend=c['backend']
    env=dict(os.environ)
    for k in list(env):
        if k.startswith(('DZIPC_', 'BREAKDOWN_', 'COMPARISON_')): env.pop(k)
    env.update(DZIPC_SHM_MPMC='1',DZIPC_SHM_RECV_WORKERS='1',DZIPC_SHARED_RECV_ASSIST='1',
               DZIPC_NET_BACKEND='shared_v1' if backend=='shared' else 'legacy',DZIPC_NET_TRACE='0')
    uri=folder/'dds.xml'
    uri.write_text(f'''<CycloneDDS><Domain Id="any"><SharedMemory><Enable>{str(backend=='dds-iox').lower()}</Enable><LogLevel>warn</LogLevel></SharedMemory><General><Interfaces><NetworkInterface name="lo" multicast="true"/></Interfaces><AllowMulticast>true</AllowMulticast><EnableMulticastLoopback>true</EnableMulticastLoopback></General><Discovery><Peers><Peer Address="127.0.0.1"/></Peers></Discovery></Domain></CycloneDDS>''')
    env['CYCLONEDDS_URI']='file://'+str(uri)
    processes={}; services={}; logs=[]; result={'case':c,'status':'failed','commands':[],'uid_map':Path('/proc/self/uid_map').read_text(),'environment':{k:v for k,v in env.items() if k.startswith(('DZIPC_','CYCLONEDDS_'))}}
    try:
        if backend=='dds-iox':
            cfg=folder/'roudi.toml'
            cfg.write_text('[general]\nversion = 1\n[[segment]]\n'+''.join(f'[[segment.mempool]]\nsize = {s}\ncount = {n}\n' for s,n in ((128,10000),(1024,5000),(16384,1000),(131072,200),(1048576,50),(2097152,50))))
            log=(folder/'roudi.log').open('w'); logs.append(log)
            cmd=[c['roudi'],'-c',str(cfg)]; result['commands'].append(cmd)
            services['roudi']=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env)
            time.sleep(1)
            assert services['roudi'].poll() is None, 'RouDi 启动失败'
        if backend=='shared':
            private=Path(tempfile.mkdtemp(prefix='ccbench-'))
            control=str(private/'control.sock'); env['DZIPC_GATEWAY_CONTROL']=control
            bases, discovery=reserve_ports(4)
            cmd=[str(build/'bin/dzipc_gateway'),'serve','--control',control,'--interface','lo','--listen-ip','127.0.0.1',
                 '--data-base-port',str(bases[0]),'--data-shards','4','--data-workers','4','--control-port',str(bases[0]+4),'--discovery-port',str(discovery)]
            log=(folder/'gateway.log').open('w'); logs.append(log)
            result['commands'].append(cmd); services['gateway']=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env)
            deadline=time.monotonic()+10
            while not Path(control).exists():
                assert services['gateway'].poll() is None and time.monotonic()<deadline, '网关启动失败'
                time.sleep(.01)
        result['ready']={}
        for name,role in [(f'sub{i}','sub') for i in range(c['subscribers'])]+[(f'pub{i}','pub') for i in range(c['publishers'])]:
            cmd=[binary,role,backend,'cc_'+c['token'],str(c['bytes']),str(folder/f'{name}.csv'),str(c['rate']),str(c['subscribers']),str(c['publishers']),name[3:] if role=='pub' else '0']
            if c.get('trace') and role=='pub':
                cmd=['strace','-f','-qq','-xx','-s','128','-e','trace=sendmsg,sendto','-o',str(folder/'transport.strace'),*cmd]
            result['commands'].append(cmd)
            proc=Process(cmd,env); processes[name]=proc; result['ready'][name]=proc.receive(timeout=25)
        all_procs={name:result['ready'][name]['pid'] for name in processes}|services
        before={name:process_sample(p) for name,p in all_procs.items()}
        begin=time.monotonic(); result['before']=before
        scheduled=time.monotonic_ns()+500000000
        for i in range(c['publishers']):
            processes[f'pub{i}'].p.stdin.write(f"{c['seconds']} {scheduled}\n"); processes[f'pub{i}'].p.stdin.flush()
        result['publishers']=[processes[f'pub{i}'].receive(timeout=c['seconds']+40) for i in range(c['publishers'])]
        result['publisher']={k:sum(p[k] for p in result['publishers']) for k in ('accepted','rejected','late_periods')}
        result['publisher'].update(window_start_ns=min(p['window_start_ns'] for p in result['publishers']),
                                   window_end_ns=max(p['window_end_ns'] for p in result['publishers']))
        result['resource_seconds']=time.monotonic()-begin
        after={name:process_sample(p) for name,p in all_procs.items()}; result['after']=after
        result['cpu_seconds']={name:after[name]['cpu_seconds']-before[name]['cpu_seconds'] for name in before}
        if backend=='shared':
            result['gateway_status']=json.loads(subprocess.check_output([str(build/'bin/dzipc_gateway'),'status','--control',control,'--json'],env=env,text=True))
        time.sleep(1)
        result['receivers']=[processes[f'sub{i}'].request('stop') for i in range(c['subscribers'])]
        for i in range(c['publishers']):
            processes[f'pub{i}'].p.stdin.write('quit\n'); processes[f'pub{i}'].p.stdin.flush()
        for name,p in processes.items():
            assert p.p.wait(timeout=10)==0, f'{name} 进程失败：{p.errors[-5:]}'
        result['summary']=audit_raw(folder,c,result)
        result['status']='ok' if not result['summary']['errors'] else 'degraded'
    except Exception:
        result['error']=traceback.format_exc()
    finally:
        for name,p in processes.items():
            p.close(); (folder/f'{name}.stderr').write_text(''.join(p.errors)); result.setdefault('exit',{})[name]=p.p.returncode
        for p in services.values():
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
        for log in logs: log.close()
        dump(folder/'result.json',result)
    return 0 if result['status']=='ok' else 1

def campaign(a):
    a.output=a.output.resolve(); a.output.mkdir(parents=True,exist_ok=True)
    if any(a.output.iterdir()): raise RuntimeError('输出目录必须为空')
    build=a.build.resolve(); binary=build/'cyclone-comparison/benchmark'
    paths=[binary,build/'lib/libipc.so',build/'bin/dzipc_gateway',a.roudi,Path(a.dds_root)/'lib/libddsc.so']
    paths += list(HERE.glob('*.py'))+[HERE/'benchmark.cc',HERE/'isolate.sh']
    frozen={str(p.resolve()):fingerprint(p) for p in paths}
    before=environment()
    if before['cpus']!=list(range(32)): raise RuntimeError('本轮要求 CPU0～31 全部允许')
    jobs=[]
    for repeat in range(1,a.rounds+1):
      for publishers in a.publishers:
        rate=a.rate if a.rate_policy=='per-publisher' else a.rate//publishers
        if a.rate_policy=='aggregate' and a.rate%publishers: raise ValueError('总速率须能整除发布者数')
        for subscribers in a.subscribers:
            for size in a.sizes:
                order=list(a.backends)
                shift=(repeat-1)%len(order); order=order[shift:]+order[:shift]
                if repeat%2==0: order.reverse()
                for backend in order:
                    name=f'r{repeat}-pub{publishers}-sub{subscribers}-bytes{size}-{backend}'
                    jobs.append({'id':name,'repeat':repeat,'backend':backend,'publishers':publishers,'subscribers':subscribers,'bytes':size,
                                 'seconds':a.seconds,'rate':rate,'rate_policy':a.rate_policy,'aggregate_rate':rate*publishers,'binary':str(binary),'build':str(build),'roudi':str(a.roudi),
                                 'token':hashlib.sha256((str(a.output)+name).encode()).hexdigest()[:14],'trace':a.trace})
    manifest={'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'production_commit':'131f37c5','build':json.loads((build/'cyclone-comparison/build.json').read_text()),
              'sha256':frozen,'environment_before':before,'jobs':jobs,'purpose':a.purpose,'windows':[]}
    dump(a.output/'manifest.json',manifest); shutil.copyfile(build/'CMakeCache.txt',a.output/'CMakeCache.txt')
    for index,c in enumerate(jobs,1):
        time.sleep(a.quiet)
        if any(fingerprint(p)!=h for p,h in frozen.items()): raise RuntimeError('工装/产物在批次中发生变化')
        active=activity(); competing=[p for p in active['processes'] if p['comm'] in ('git-maintenance','cc1plus','cc1','ninja','cmake','make','comparison','xproc_benchmark','shared_net_ben','benchmark')]
        if competing: raise RuntimeError(f'发现编译或性能竞争任务：{competing}')
        folder=a.output/c['id']; folder.mkdir(); dump(folder/'case.json',c)
        command=['unshare','--user','--map-root-user','--mount','--ipc','--fork','bash',str(HERE/'isolate.sh'),sys.executable,str(HERE/'run.py'),'--window',str(folder/'case.json')]
        state={'id':c['id'],'command':command,'before':active,'activity':[],'started_ns':time.monotonic_ns()}
        with (folder/'console.log').open('w') as log:
            proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
            while proc.poll() is None:
                state['activity'].append(activity()); time.sleep(1)
            state['returncode']=proc.returncode
        state['finished_ns']=time.monotonic_ns()
        if (folder/'result.json').exists():
            result=json.loads((folder/'result.json').read_text()); state['status']=result['status']
            if 'summary' in result: state['summary']=result['summary']
            if 'error' in result: state['error']=result['error']
        else: state['status']='failed'
        state['competing_activity']=[p for snap in state['activity'] for p in snap['processes'] if p['comm'] in ('git-maintenance','cc1plus','cc1','ninja','cmake','make','comparison','xproc_benchmark','shared_net_ben')]
        state['files']={}
        for file in folder.glob('*.csv'):
            state['files'][file.name]=fingerprint(file)
            with file.open('rb') as source,gzip.open(str(file)+'.gz','wb') as target: shutil.copyfileobj(source,target)
            file.unlink()
        manifest['windows'].append(state); dump(a.output/'manifest.json',manifest)
        print(json.dumps({'completed':index,'total':len(jobs),'case':c['id'],'status':state['status'],
                          'latency':state.get('summary',{}).get('receive',{}),'error':state.get('error')},ensure_ascii=False),flush=True)
    manifest['environment_after']=environment(); dump(a.output/'manifest.json',manifest)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--window',type=Path)
    p.add_argument('--build',type=Path)
    p.add_argument('--dds-root',type=Path)
    p.add_argument('--roudi',type=Path)
    p.add_argument('--output',type=Path)
    p.add_argument('--backends',nargs='+',choices=BACKENDS,default=list(BACKENDS))
    p.add_argument('--sizes',nargs='+',type=int,default=[64,4096,1048576])
    p.add_argument('--subscribers',nargs='+',type=int,default=[1,8,32])
    p.add_argument('--publishers',nargs='+',type=int,default=[1])
    p.add_argument('--rate-policy',choices=['per-publisher','aggregate'],default='per-publisher')
    p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--seconds',type=int,default=10)
    p.add_argument('--rate',type=int,default=100)
    p.add_argument('--quiet',type=int,default=2)
    p.add_argument('--trace',action='store_true')
    p.add_argument('--purpose',default='benchmark')
    a=p.parse_args()
    if a.window: return window(a.window)
    if any(v is None for v in (a.build,a.dds_root,a.roudi,a.output)): p.error('需要 build/dds-root/roudi/output')
    with open('/var/tmp/cppipc-comparison.run.lock','a') as lock:
        fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB); campaign(a)
    return 0

if __name__=='__main__': sys.exit(main())
