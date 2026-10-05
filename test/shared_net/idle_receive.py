#!/usr/bin/env python3
"""同库 assist=0/1 的空闲订阅 CPU/线程证据；在独占 SHM 中顺序执行。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from benchmark import sample
from end_to_end import Process, reserve_ports


def snapshot(pid):
    result = sample(pid)
    result['pid'] = pid
    result['threads_detail'] = []
    for task in sorted(Path(f'/proc/{pid}/task').iterdir()):
        fields = (task / 'stat').read_text().split(') ', 1)[1].split()
        result['threads_detail'].append({
            'tid': int(task.name), 'name': (task / 'comm').read_text().strip(),
            'wait_channel': (task / 'wchan').read_text().strip(),
            'cpu_ticks': int(fields[11]) + int(fields[12]),
        })
    return result


def host(args):
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=4g', 'tmpfs', '/dev/shm'], check=True)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError('证据目录必须为空')
    files = [Path(args.binary), Path(args.gateway), Path(args.binary).parent.parent / 'lib/libipc.so']
    hashes = lambda: {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    frozen = hashes()
    evidence = {'binary_sha256': frozen, 'seconds': args.seconds,
                'note': '订阅调用沿用基准的 get(20ms)，约50次/秒超时检查；发布者就绪但不发送。线程等待状态为瞬时快照。',
                'command': sys.argv, 'cases': []}
    for subscribers in (1, 8, 32):
        for assist in ('0', '1'):
            with tempfile.TemporaryDirectory(prefix='dzipc-idle-') as temp:
                control = temp + '/control.sock'
                env = dict(os.environ, DZIPC_SHM_MPMC='1', DZIPC_SHM_RECV_WORKERS='1',
                           DZIPC_NET_BACKEND='shared_v1', DZIPC_GATEWAY_CONTROL=control,
                           DZIPC_SHARED_RECV_ASSIST=assist, DZIPC_TEST_RECEIVE_TRACE='0')
                bases, discovery = reserve_ports()
                gateway = subprocess.Popen([args.gateway, 'serve', '--control', control,
                    '--interface', 'lo', '--listen-ip', '127.0.0.1', '--data-base-port', str(bases[0]),
                    '--control-port', str(bases[0] + 4), '--discovery-port', str(discovery)],
                    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                processes = []
                try:
                    deadline = time.monotonic() + 5
                    while not os.path.exists(control):
                        assert gateway.poll() is None and time.monotonic() < deadline
                        time.sleep(.01)
                    for index, role in enumerate(['sub'] * subscribers + ['pub']):
                        process = Process([args.binary, role, 'socket', 'idle-receive', '64',
                                           str(Path(temp) / f'{index}.csv'), '100'], env)
                        processes.append(process)
                        assert process.receive()['ready']
                    # 全部对象已Ready；让初始化开销离开空闲窗口。
                    time.sleep(1)
                    pids = [p.p.pid for p in processes] + [gateway.pid]
                    before = [snapshot(pid) for pid in pids]
                    start = time.monotonic()
                    time.sleep(args.seconds)
                    after = [snapshot(pid) for pid in pids]
                    wall = time.monotonic() - start
                    assert hashes() == frozen, '采样二进制已改变'
                    case = {'subscribers': subscribers, 'assist': int(assist), 'wall_seconds': wall,
                            'before': before, 'after': after,
                            'cpu_seconds': [b['cpu_seconds'] - a['cpu_seconds'] for a, b in zip(before, after)]}
                    case['cpu_percent_one_core'] = sum(case['cpu_seconds']) / wall * 100
                    case['subscribers_cpu_percent_one_core'] = sum(case['cpu_seconds'][:subscribers]) / wall * 100
                    assert all(a['threads'] == b['threads'] for a, b in zip(before, after))
                    assert all(x['udp'] == 0 for x in after[:-1])
                    evidence['cases'].append(case)
                    (output / 'results.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n')
                    print(json.dumps({k: v for k, v in case.items() if k not in ('before', 'after')}, ensure_ascii=False), flush=True)
                    for process in processes[:-1]:
                        result = process.request('stop')
                        assert result['received'] == 0 and result['invalid'] == 0
                        assert process.p.wait(timeout=5) == 0
                finally:
                    for process in processes:
                        process.close()
                    gateway.terminate()
                    try:
                        gateway.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        gateway.kill()
                        gateway.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--binary', required=True)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--seconds', type=int, default=10)
    args = parser.parse_args()
    if args.seconds < 10:
        parser.error('空闲窗口至少10秒')
    if args.host:
        host(args)
    else:
        sys.exit(subprocess.run(['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                                sys.executable, __file__, '--host', *sys.argv[1:]], start_new_session=True).returncode)
