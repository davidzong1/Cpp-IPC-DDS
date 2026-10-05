#!/usr/bin/env python3
"""独占 SHM 命名空间、同源码/优化级别、逐轮保存全部样本。"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from end_to_end import Process, reserve_ports, udp_count


def sample(pid):
    root = Path(f'/proc/{pid}')
    fields = (root / 'stat').read_text().split(') ', 1)[1].split()
    status = dict(line.split(':', 1) for line in (root / 'status').read_text().splitlines())
    return {'cpu_seconds': (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK'),
            'rss_kib': int(status.get('VmRSS', '0 kB').split()[0]),
            'threads': int(status['Threads']), 'fd': len(list((root / 'fd').iterdir())),
             'udp': udp_count(pid),
            'thread_status': [dict(tid=int(task.name), **{key: value.strip() for key, value in
                (line.split(':', 1) for line in (task/'status').read_text().splitlines())
                if key in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches', 'Cpus_allowed_list')})
                for task in sorted((root/'task').iterdir())]}


def quantiles(values):
    values = sorted(values)
    if not values:
        return {'count': 0, 'p50_us': None, 'p95_us': None, 'p99_us': None}
    return {'count': len(values), **{f'p{p}_us': values[min(len(values)-1, math.ceil(len(values)*p/100)-1)]/1000
                                  for p in (50, 95, 99)}}


def host(args):
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=4g', 'tmpfs', '/dev/shm'], check=True)
    directory = Path(args.output)
    directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DZIPC_SHM_MPMC='1', DZIPC_SHM_RECV_WORKERS='1',
               DZIPC_NET_BACKEND='shared_v1' if args.mode == 'shared_v1' else 'legacy')
    processes = []
    gateway = None
    with tempfile.TemporaryDirectory(prefix='dzipc-bench-') as control_dir:
        control = control_dir + '/control.sock'
        env['DZIPC_GATEWAY_CONTROL'] = control
        try:
            if args.mode == 'shared_v1':
                bases, discovery = reserve_ports()
                gateway = subprocess.Popen([args.gateway, 'serve', '--control', control, '--interface', 'lo', '--listen-ip', '127.0.0.1',
                    '--data-base-port', str(bases[0]), '--control-port', str(bases[0]+4), '--discovery-port', str(discovery)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
                end = time.monotonic() + 5
                while not os.path.exists(control):
                    assert gateway.poll() is None and time.monotonic() < end
                    time.sleep(.01)
            transport = 'socket' if args.mode == 'shared_v1' else 'shm'
            for index in range(args.subscribers):
                sub = Process([args.binary, 'sub', transport, 'benchmark', str(args.bytes), str(directory/f'sub{index}.csv'), str(args.rate)], env)
                processes.append(sub)
                sub.receive()
            pub = Process([args.binary, 'pub', transport, 'benchmark', str(args.bytes), str(directory/'pub.csv'), str(args.rate)], env)
            processes.append(pub)
            pub.receive()
            pids = [p.p.pid for p in processes] + ([gateway.pid] if gateway else [])
            pub.p.stdin.write(str(args.seconds) + '\n')
            pub.p.stdin.flush()
            time.sleep(2)
            before = [sample(pid) for pid in pids]
            measured_start = time.monotonic()
            result = pub.receive(timeout=args.seconds + 10)
            after = [sample(pid) for pid in pids]
            wall = time.monotonic() - measured_start
            shm = os.statvfs('/dev/shm')
            peak_shm = (shm.f_blocks - shm.f_bfree) * shm.f_frsize
            gateway_metrics = {category: json.loads(subprocess.check_output(
                [args.gateway, 'status', '--control', control, '--metrics', category], env=env, text=True))
                for category in ('counters', 'quota', 'latency', 'shards')} if gateway else None
            sub_results = [p.request('stop') for p in processes[:-1]]
            for p in processes:
                if p.p.poll() is None:
                    if p is pub:
                        p.p.stdin.write('quit\n'); p.p.stdin.flush()
                    assert p.p.wait(timeout=5) == 0, p.errors
            time.sleep(.05)
            idle = sample(gateway.pid) if gateway else None
            status = json.loads(subprocess.check_output([args.gateway, 'status', '--control', control, '--json'], env=env, text=True)) if gateway else None
            with (directory/'pub.csv').open() as file:
                pub_rows = list(csv.DictReader(file))
            accepted = {int(row['sequence']) for row in pub_rows if row['success'] == '1'}
            receiver_stats = []
            for index, sub_result in enumerate(sub_results):
                with (directory/f'sub{index}.csv').open() as file:
                    rows = list(csv.DictReader(file))
                sequences = [int(row['sequence']) for row in rows]
                stats = quantiles([int(row['elapsed_ns']) for row in rows])
                stats.update(lost=len(accepted - set(sequences)), duplicates=len(sequences)-len(set(sequences)), invalid=sub_result['invalid'])
                receiver_stats.append(stats)
            evidence = {'host_loadavg_after': list(os.getloadavg()), 'mode': args.mode, 'bytes': args.bytes, 'subscribers': args.subscribers,
                'seconds': args.seconds, 'rate': args.rate, 'cpu_window_seconds': wall, 'publish': result,
                'wire_bytes': int(pub_rows[0]['bytes']) if pub_rows else 0,
                'publish_latency': quantiles([int(row['elapsed_ns']) for row in pub_rows]),
                'receivers': receiver_stats, 'before': before, 'after': after,
                'cpu_seconds': [b['cpu_seconds']-a['cpu_seconds'] for a,b in zip(before,after)],
                'gateway_metrics': gateway_metrics, 'shm_peak_sample_bytes': peak_shm, 'idle_gateway': idle, 'idle_status': status,
                'stage_window': '应用直方图含2秒预热，网关指标为进程累计；精确端到端CSV仅正式窗口',
                'environment': {'DZIPC_SHM_MPMC': '1', 'DZIPC_SHM_RECV_WORKERS': '1', 'nodelet': False, 'wire': 'prebuilt StdImage DZFlat'}}
            (directory/'result.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+'\n')
            assert result['accepted'] > 0 and all(x['count'] > 0 for x in receiver_stats), evidence
            assert all(not x['duplicates'] and not x['invalid'] for x in receiver_stats), evidence
            assert all(x['udp'] == 0 for x in after[:len(processes)]), evidence
            print(json.dumps({'output': str(directory), 'passed_content': True, 'accepted': result['accepted'],
                              'lost': sum(x['lost'] for x in receiver_stats)}), flush=True)
        finally:
            for p in processes:
                p.close()
            if gateway:
                gateway.terminate()
                try:
                    gateway.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    gateway.kill(); gateway.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--binary', required=True)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--mode', choices=('baseline', 'shared_v1'), required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--bytes', type=int, default=64)
    parser.add_argument('--subscribers', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--rate', type=int, default=100)
    args = parser.parse_args()
    if args.host:
        host(args)
    else:
        result = subprocess.run(['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                                 sys.executable, __file__, '--host', *sys.argv[1:]], start_new_session=True)
        sys.exit(result.returncode)
