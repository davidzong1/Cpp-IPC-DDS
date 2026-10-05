#!/usr/bin/env python3
"""安装目录 shared_v1 → legacy/默认模式；旧模式不得连接控制监听哨兵。"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from end_to_end import reserve_ports


def main(args):
    subprocess.run(['mount','-t','tmpfs','-o','size=768m','tmpfs','/dev/shm'],check=True)
    bases,discovery=reserve_ports()
    with tempfile.TemporaryDirectory(prefix='dzipc-rollback-') as directory:
        control=directory+'/control.sock'
        env=dict(os.environ,DZIPC_NET_BACKEND='shared_v1',DZIPC_SHM_MPMC='1',DZIPC_GATEWAY_CONTROL=control)
        gateway=subprocess.Popen([args.gateway,'serve','--control',control,'--interface','lo','--listen-ip','127.0.0.1',
            '--data-base-port',str(bases[0]),'--control-port',str(bases[0]+4),'--discovery-port',str(discovery)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        results=[]
        try:
            end=time.monotonic()+5
            while not os.path.exists(control):
                assert gateway.poll() is None and time.monotonic()<end
                time.sleep(.01)
            def run(mode):
                if mode is None:env.pop('DZIPC_NET_BACKEND',None)
                else:env['DZIPC_NET_BACKEND']=mode
                result=subprocess.run([args.example],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
                assert result.returncode==0,(mode,result.returncode,result.stdout,result.stderr)
                payload=next(json.loads(line) for line in result.stdout.splitlines() if line.startswith('{'))
                assert payload['backend']==('shared_v1' if mode=='shared_v1' else 'legacy')
                results.append({'environment':mode,'result':payload})
            run('shared_v1')
        finally:
            gateway.terminate()
            try:gateway.wait(timeout=5)
            except subprocess.TimeoutExpired:gateway.kill();gateway.wait()
        assert not os.path.exists(control)
        with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as sentinel:
            sentinel.bind(control);sentinel.listen();sentinel.setblocking(False)
            run('legacy');run(None)
            try:
                connection,_=sentinel.accept();connection.close()
                raise AssertionError('旧模式意外连接共享网关')
            except BlockingIOError:pass
        Path(control).unlink()
        print(json.dumps({'passed':3,'control_connections_after_rollback':0,'results':results},ensure_ascii=False))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--gateway',required=True);parser.add_argument('--example',required=True);parser.add_argument('--host',action='store_true')
    args=parser.parse_args()
    if args.host:main(args)
    else:sys.exit(subprocess.call(['unshare','--user','--map-root-user','--mount','--ipc',sys.executable,__file__,'--host',*sys.argv[1:]]))
