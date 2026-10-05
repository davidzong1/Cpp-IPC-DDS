#!/usr/bin/env python3
"""冷热话题 30 秒窗口：真实网关、独立发布进程、同/不同 shard。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from end_to_end import Process, reserve_ports


def shard(topic):
    def fnv(data):
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64)-1)
        return value
    key = f'DZSC2:1:0:{len(topic)}:{topic}'.encode()
    scope = b'DZS2' + (1).to_bytes(4,'big') + bytes(8) + fnv(key).to_bytes(8,'big') + fnv(b'identity:'+key).to_bytes(8,'big')
    return fnv(scope+(71).to_bytes(4,'big')) % 4


def host(args):
    subprocess.run(['mount','-t','tmpfs','-o','size=1g','tmpfs','/dev/shm'],check=True)
    env = dict(os.environ,DZIPC_SHM_MPMC='1',DZIPC_SHM_RECV_WORKERS='1',
               DZIPC_NET_BACKEND='shared_v1',DZIPC_GATEWAY_CONTROL=args.control)
    gateway = subprocess.Popen([args.gateway,'serve','--control',args.control,'--listen-ip','127.0.0.1','--interface','lo',
        '--data-base-port',str(args.base),'--control-port',str(args.base+4),'--discovery-port',str(args.discovery)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    probes = []
    try:
        end=time.monotonic()+5
        while not os.path.exists(args.control):
            assert gateway.poll() is None and time.monotonic()<end
            time.sleep(.01)
        for topic in (args.hot,args.cold):
            process=Process([args.probe,args.control,topic,'pub' if args.sender else 'sub'],env)
            probes.append(process);process.receive()
        print(json.dumps({'ready':True}),flush=True)
        for line in sys.stdin:
            command=json.loads(line)
            if command[0]=='state':
                result=[p.request('state') for p in probes]
            elif command[0]=='start':
                seconds,rate,calibrate=command[1:]
                for index,p in enumerate(probes[:1] if calibrate else probes):
                    text=f'load {seconds} {rate if index==0 else 100} {1048576 if index==0 else 64}' if args.sender else f'drain {seconds+1}'
                    p.p.stdin.write(text+'\n');p.p.stdin.flush()
                result={'running':True}
            elif command[0]=='collect':
                result=[p.receive(timeout=40) for p in (probes[:1] if command[1] else probes)]
            elif command[0]=='status':
                result=json.loads(subprocess.check_output([args.gateway,'status','--control',args.control,'--json'],env=env,text=True))
                if args.sender:
                    churn=Process([args.probe,args.control,'fairness-churn','sub'],env);churn.receive()
                    churn.p.stdin.write('quit\n');churn.p.stdin.flush();assert churn.p.wait(timeout=5)==0;churn.close()
            else:
                raise AssertionError(command)
            print(json.dumps({'result':result}),flush=True)
    finally:
        for p in probes:p.close()
        gateway.terminate()
        try:gateway.wait(timeout=5)
        except subprocess.TimeoutExpired:gateway.kill();gateway.wait()


def driver(args):
    hot='fairness-hot'
    cold=next('fairness-cold-'+str(i) for i in range(100) if (shard('fairness-cold-'+str(i))==shard(hot)) == args.same_shard)
    bases,discovery=reserve_ports();processes=[]
    with tempfile.TemporaryDirectory(prefix='dzipc-fair-') as directory:
        try:
            for index in range(2):
                os.mkdir(f'{directory}/host{index}',0o700)
                command=['unshare','--user','--map-root-user','--mount','--ipc',sys.executable,__file__,'--host',
                    '--gateway',args.gateway,'--probe',args.probe,'--control',f'{directory}/host{index}/control.sock',
                    '--base',str(bases[index]),'--discovery',str(discovery),'--hot',hot,'--cold',cold]
                if index==0:command.append('--sender')
                p=Process(command,group=True);processes.append(p);p.receive()
            def request(index,command):return processes[index].request(json.dumps(command))['result']
            end=time.monotonic()+10
            while not all(s['remote']==1 and s['synchronized'] for s in request(0,['state'])):
                assert time.monotonic()<end;time.sleep(.02)
            request(1,['start',1,0,True]);request(0,['start',1,0,True])
            calibrated=request(0,['collect',True])[0];request(1,['collect',True])
            hot_rate=max(.7,calibrated['sent']*.7)
            request(1,['start',30,hot_rate,False]);request(0,['start',30,hot_rate,False])
            snapshots=[]
            for n in range(30):
                time.sleep(1)
                snapshots.append([request(index,['status']) for index in range(2)])
            sent=request(0,['collect',False]);received=request(1,['collect',False])
            result={'same_shard':args.same_shard,'topics':[hot,cold],'shards':[shard(hot),shard(cold)],
                'calibration':calibrated,'hot_rate':hot_rate,'seconds':30,'sent':sent,'received':received,'status':snapshots}
            Path(args.output).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
            assert not calibrated['failed']
            assert all(not x['failed'] and x['max_ns']<=1250000000 for x in sent),sent
            assert all(not x['invalid'] and not x['duplicates'] for x in received),received
            assert all(s['sent']==r['received'] for s,r in zip(sent,received)),(sent,received)
            assert received[1]['max_gap_ns']<1000000000,received[1]
            assert all(snapshots[-1][i]['reassembly_bytes']==0 and snapshots[-1][i]['target_states']==0 for i in range(2))
            print(json.dumps({'same_shard':args.same_shard,'hot_rate':hot_rate,'cold':received[1],'passed':True},ensure_ascii=False))
        finally:
            for p in processes:p.close()


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--gateway',required=True);parser.add_argument('--probe',required=True)
    parser.add_argument('--same-shard',action='store_true');parser.add_argument('--output')
    parser.add_argument('--host',action='store_true');parser.add_argument('--sender',action='store_true')
    parser.add_argument('--control');parser.add_argument('--hot');parser.add_argument('--cold')
    parser.add_argument('--base',type=int);parser.add_argument('--discovery',type=int)
    args=parser.parse_args();host(args) if args.host else driver(args)
