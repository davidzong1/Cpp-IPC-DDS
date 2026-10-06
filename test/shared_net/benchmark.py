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
            'loaded_libraries': sorted({line.split()[-1] for line in (root / 'maps').read_text().splitlines() if 'libipc.so' in line}),
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
    env['DZIPC_TEST_RECEIVE_TRACE'] = '1' if args.receive_trace else '0'
    env['DZIPC_TEST_PUBLISH_TRACE'] = '1' if args.publish_trace else '0'
    env['DZIPC_TEST_PUBLISH_CPU_TRACE'] = '1' if args.publish_cpu_trace else '0'
    env['DZIPC_SHARED_RECV_ASSIST'] = args.receive_assist
    processes = []
    gateway = None
    with tempfile.TemporaryDirectory(prefix='dzipc-bench-') as control_dir:
        control = control_dir + '/control.sock'
        env['DZIPC_GATEWAY_CONTROL'] = control
        affinity = json.loads(args.affinity) if args.affinity else None
        if affinity:
            allowed = os.sched_getaffinity(0)
            assert set(affinity['subscribers'] + [affinity['publisher'], affinity['gateway']]) <= allowed
        def launched(command, cpu):
            return ['taskset', '-c', str(cpu), *command] if affinity else command
        try:
            if args.mode == 'shared_v1':
                bases, discovery = reserve_ports(args.data_shards)
                gateway = subprocess.Popen(launched([args.gateway, 'serve', '--control', control, '--interface', 'lo', '--listen-ip', '127.0.0.1',
                    '--data-base-port', str(bases[0]), '--data-shards', str(args.data_shards),
                    *([] if args.legacy_gateway else ['--data-workers', str(args.data_workers)]),
                    '--control-port', str(bases[0]+args.data_shards),
                    '--discovery-port', str(discovery)], affinity['gateway'] if affinity else 0),
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
                end = time.monotonic() + 5
                while not os.path.exists(control):
                    assert gateway.poll() is None and time.monotonic() < end
                    time.sleep(.01)
            transport = 'socket' if args.mode == 'shared_v1' else 'shm'
            for index in range(args.subscribers):
                sub = Process(launched([args.binary, 'sub', transport, 'benchmark', str(args.bytes), str(directory/f'sub{index}.csv'), str(args.rate)], affinity['subscribers'][index % len(affinity['subscribers'])] if affinity else 0), env)
                processes.append(sub)
                sub.receive()
            pub = Process(launched([args.binary, 'pub', transport, 'benchmark', str(args.bytes), str(directory/'pub.csv'), str(args.rate)], affinity['publisher'] if affinity else 0), env)
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
            gateway_status = json.loads(subprocess.check_output(
                [args.gateway, 'status', '--control', control, '--json'], env=env, text=True)) if gateway else None
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
            if args.publish_trace:
                for row in pub_rows:
                    stamps = [int(row[key]) for key in ('loan_begin_ns', 'loan_end_ns', 'copy_begin_ns', 'copy_end_ns',
                        'commit_begin_ns', 'notify_begin_ns', 'notify_end_ns', 'commit_end_ns')]
                    assert all(stamps) and stamps == sorted(stamps), row
                    assert int(row['start_ns']) <= stamps[0] <= stamps[-1] <= int(row['start_ns']) + int(row['elapsed_ns']), row
                    if args.publish_cpu_trace:
                        from summarize_copy_cpu import decode_cpu
                        decode_cpu(row)
            receiver_stats = []
            for index, sub_result in enumerate(sub_results):
                with (directory/f'sub{index}.csv').open() as file:
                    rows = list(csv.DictReader(file))
                sequences = [int(row['sequence']) for row in rows]
                stats = quantiles([int(row['elapsed_ns']) for row in rows])
                stats.update(lost=len(accepted - set(sequences)), duplicates=len(sequences)-len(set(sequences)), invalid=sub_result['invalid'])
                if args.receive_trace:
                    stats['trace_overflow'] = sub_result['trace_overflow']
                    stats['trace_missing_or_unordered'] = 0
                    for row in rows:
                        read = int(row['read_ns'])
                        start = read - int(row['elapsed_ns'])
                        stamps = [int(row[key]) for key in ('recv_begin_ns', 'recv_return_ns', 'enqueue_before_ns', 'dequeue_after_ns')]
                        if args.mode == 'baseline':
                            valid_trace = start <= stamps[1] <= read and stamps[1] != 0
                        else:
                            # recv可能在发布起点前已开始，不能把真实并发交叠误判为坏样本。
                            valid_trace = all(stamps) and stamps == sorted(stamps) and start <= stamps[1] and stamps[-1] <= read
                        if not valid_trace:
                            stats['trace_missing_or_unordered'] += 1
                receiver_stats.append(stats)
            evidence = {'affinity_plan': affinity, 'host_loadavg_after': list(os.getloadavg()), 'mode': args.mode, 'bytes': args.bytes, 'subscribers': args.subscribers,
                'seconds': args.seconds, 'rate': args.rate, 'cpu_window_seconds': wall, 'publish': result,
                'wire_bytes': int(pub_rows[0]['bytes']) if pub_rows else 0,
                'publish_latency': quantiles([int(row['elapsed_ns']) for row in pub_rows]),
                'receivers': receiver_stats, 'before': before, 'after': after,
                'cpu_seconds': [b['cpu_seconds']-a['cpu_seconds'] for a,b in zip(before,after)],
                'gateway_status': gateway_status, 'gateway_metrics': gateway_metrics, 'shm_peak_sample_bytes': peak_shm, 'idle_gateway': idle, 'idle_status': status,
                'stage_window': '应用直方图含2秒预热，网关指标为进程累计；精确端到端CSV仅正式窗口',
                'environment': {'DZIPC_SHM_MPMC': '1', 'DZIPC_SHM_RECV_WORKERS': '1', 'DZIPC_SHARED_RECV_ASSIST': args.receive_assist,
                    'receive_trace': args.receive_trace, 'publish_trace': args.publish_trace,
                    'publish_cpu_trace': args.publish_cpu_trace, 'nodelet': False, 'wire': 'prebuilt StdImage DZFlat'}}
            (directory/'result.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+'\n')
            assert result['accepted'] == args.seconds * args.rate and result['rejected'] == 0, evidence
            assert all(x['count'] == result['accepted'] and not x['lost'] and not x['duplicates'] and not x['invalid'] for x in receiver_stats), evidence
            if args.receive_trace:
                assert all(not x['trace_overflow'] and not x['trace_missing_or_unordered'] for x in receiver_stats), evidence
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
    parser.add_argument('--receive-trace', action='store_true', help='开启接收缝分段诊断；不是无观测成本的正式验收')
    parser.add_argument('--publish-trace', action='store_true', help='开启发布分段，仅支持带诊断能力的库')
    parser.add_argument('--publish-cpu-trace', action='store_true', help='同时开启发布分段与复制线程CPU时间诊断；不用于正式验收')
    parser.add_argument('--receive-assist', choices=('0', '1'), default='1', help='共享后端调用线程协作接收开关；0用于同库回退对照')
    parser.add_argument('--affinity', help='可选JSON：publisher/gateway CPU及subscribers CPU列表；两模式必须一致')
    parser.add_argument('--binary', required=True)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--mode', choices=('baseline', 'shared_v1'), required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--bytes', type=int, default=64)
    parser.add_argument('--subscribers', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--rate', type=int, default=100)
    parser.add_argument('--data-shards', type=int, default=4,
                        help='shared_v1 网关数据 socket 数（v1 HELLO 仍公告该数）')
    parser.add_argument('--data-workers', type=int,
                        help='shared_v1 网关数据 worker 数；可与 data-shards 不同')
    parser.add_argument('--legacy-gateway', action='store_true',
                        help='兼容改造前网关：不转发 data-workers')
    args = parser.parse_args()
    if args.data_workers is None:
        args.data_workers = args.data_shards
    if args.publish_cpu_trace:
        args.publish_trace = True
    if args.host:
        host(args)
    else:
        result = subprocess.run(['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                                 sys.executable, __file__, '--host', *sys.argv[1:]], start_new_session=True)
        sys.exit(result.returncode)
