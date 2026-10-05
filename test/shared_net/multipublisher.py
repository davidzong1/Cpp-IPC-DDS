#!/usr/bin/env python3
"""2/8 个发布进程共享同一话题；退出一个后其余继续，远端无重复。"""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from end_to_end import Process, reserve_ports


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--probe', required=True)
    parser.add_argument('--publishers', type=int, choices=(2, 8), required=True)
    args = parser.parse_args()
    processes = []
    bases, discovery = reserve_ports()
    with tempfile.TemporaryDirectory(prefix='dzipc-multipub-') as folder:
        try:
            for i in range(2):
                os.mkdir(f'{folder}/host{i}', 0o700)
                p = Process(['unshare','--user','--map-root-user','--mount','--ipc',sys.executable,
                    str(Path(__file__).with_name('end_to_end.py')),'--host','--gateway',args.gateway,
                    '--probe',args.probe,'--control',f'{folder}/host{i}/control.sock','--base',str(bases[i]),
                    '--discovery',str(discovery),'--topic','multipublisher','--local-subscribers','1',
                    '--local-publishers',str(args.publishers if i == 0 else 1)], group=True)
                processes.append(p);p.receive()
            def request(i, command):return processes[i].request(json.dumps(command))
            end = time.monotonic()+10
            while not all(request(0,['probe',i,'state'])['remote']==1 for i in range(args.publishers)):
                assert time.monotonic()<end
                time.sleep(.02)
            deliveries = []
            for phase,count in enumerate((args.publishers,args.publishers-1)):
                if phase:assert request(0,['close_probe',args.publishers-1])['closed']
                for size in (64,4096,1048576):
                    for publisher in range(count):
                        sent=request(0,['probe',publisher,f'send {size} {publisher+1} {phase+1} 1'])
                        assert sent['success'],sent
                        for host in range(2):
                            received=request(host,['probe',0,'recv 1 3000'])['received']
                            assert len(received)==1 and received[0]['valid'] and received[0]['crc']==sent['crc'],received
                        deliveries.append({'phase':phase,'publisher':publisher,'size':size,'crc':sent['crc']})
            status=[request(i,['probe',0,'status']) for i in range(2)]
            assert status[0]['committed_messages']==0 and status[1]['committed_messages']==len(deliveries)
            resources=[request(i,['resources']) for i in range(2)]
            assert all(r['gateway_udp']==6 and not any(r['application_udp']) for r in resources)
            for i in range(2):assert not request(i,['probe',0,'recv 1 50'])['received']
            print(json.dumps({'publishers':args.publishers,'deliveries':deliveries,'resources':resources,'status':status},ensure_ascii=False))
        finally:
            for p in processes:
                if p.p.poll() is None:
                    p.p.stdin.write('["quit"]\n');p.p.stdin.flush()
                    try:p.p.wait(timeout=12)
                    except Exception:pass
                p.close()


if __name__=='__main__':main()
