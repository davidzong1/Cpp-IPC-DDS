#!/usr/bin/env python3
"""须以 sudo 执行；仅 perf 提权，业务工装降回原用户；不修改系统设置。"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
COMM = 'shared_net_benc'


def snapshot():
    result = {'time_ns': time.monotonic_ns(), 'load': os.getloadavg(), 'cpus': sorted(os.sched_getaffinity(0)), 'cpu': {}}
    result['kernel'] = os.uname().release
    result['cpuidle_driver'] = Path('/sys/devices/system/cpu/cpuidle/current_driver').read_text().strip()
    result['core_types'] = {name: (Path('/sys/devices')/name/'cpus').read_text().strip()
                            for name in ('cpu_core', 'cpu_atom') if (Path('/sys/devices')/name/'cpus').exists()}
    for cpu in sorted(Path('/sys/devices/system/cpu').glob('cpu[0-9]*')):
        result['cpu'][cpu.name] = {'idle': {s.name: {key: (s/key).read_text().strip() for key in ('name', 'latency', 'disable', 'usage', 'time')} for s in (cpu/'cpuidle').glob('state*')},
            'frequency': {key: (cpu/'cpufreq'/key).read_text().strip() for key in ('scaling_governor', 'scaling_driver', 'scaling_cur_freq', 'cpuinfo_max_freq') if (cpu/'cpufreq'/key).exists()}}
    return result


def activity():
    result = []
    for p in Path('/proc').glob('[0-9]*'):
        try:
            name = (p/'comm').read_text().strip()
            if name in ('cc1plus', 'cc1', 'cmake', 'ninja', 'make', 'gmake', 'ctest', COMM, 'dzipc_gateway', 'perf'):
                item = {'pid': int(p.name), 'name': name, 'cwd': str((p/'cwd').resolve())}
                if name == COMM and item['cwd'] == str(ROOT):
                    status = dict(line.split(':', 1) for line in (p/'status').read_text().splitlines())
                    item['uid'] = status['Uid'].strip(); item['gid'] = status['Gid'].strip()
                result.append(item)
        except OSError:
            pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('smoke', 'paired', 'paired32', 'copy', 'stacks'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quiet-seconds', type=int, default=0, help='每窗前连续无编译/压测的秒数')
    parser.add_argument('--kernel-stacks', action='store_true', help='仅唤醒事件增加内核栈；独立诊断，不作无栈配对')
    args = parser.parse_args()
    if os.geteuid() != 0 or not os.environ.get('SUDO_UID'):
        raise RuntimeError('请通过 sudo 运行，业务工装必须降回原用户')
    uid, gid = int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID'])
    if not uid:
        raise RuntimeError('业务工装不能以 root 采样')
    os.chdir(ROOT)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    os.chown(args.output, uid, gid)
    if args.suite == 'smoke':
        cases = [('smoke', 1, 4096, True, None, 3)]
    elif args.suite == 'stacks':
        if not args.kernel_stacks:
            raise ValueError('stacks 批次必须指定 --kernel-stacks')
        cases = [(f'r{r}-sub{n}-stacks', n, b, True, None, 10)
                 for r in (1, 2) for n, b in ((1, 4096), (32, 64))]
    elif args.suite in ('paired', 'paired32'):
        sizes = ((1, 4096), (32, 64)) if args.suite == 'paired' else ((32, 64),)
        cases = [(f'r{r}-sub{n}-perf{int(enabled)}', n, b, enabled, None, 10)
                 for r in (1, 2) for n, b in sizes
                 for enabled in ((False, True) if r == 1 else (True, False))]
    else:
        placements = [('pp', 0, 4), ('pe', 0, 16), ('ep', 16, 4)]
        cases = [(f'r{r}-{name}', 1, 1048576, True, {'publisher': p, 'subscribers': [s], 'gateway': 2}, 10)
                 for r in (1, 2) for name, p, s in (placements if r == 1 else placements[::-1])]
    paths = [ROOT/'test/shared_net/benchmark.cc', ROOT/'test/shared_net/benchmark.py', Path(__file__), ROOT/'build-local-latency/bin/shared_net_benchmark', ROOT/'build-local-latency/bin/dzipc_gateway', ROOT/'build-local-latency/lib/libipc.so']
    def hashes():
        return {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    frozen = hashes()
    manifest = {'head': subprocess.check_output(['git', '-c', f'safe.directory={ROOT}', 'rev-parse', 'HEAD'], text=True).strip(), 'hashes': frozen,
                'perf_version': subprocess.check_output(['perf', '--version'], text=True).strip(), 'clock': 'CLOCK_MONOTONIC / perf --clockid mono',
                'workload_uid': uid, 'kernel_stacks': args.kernel_stacks, 'quiet_seconds': args.quiet_seconds,
                'cases': cases, 'windows': [], 'settings_before': snapshot()}
    for event in ('sched/sched_waking', 'sched/sched_wakeup', 'sched/sched_switch', 'sched/sched_migrate_task', 'power/cpu_idle'):
        source = Path('/sys/kernel/tracing/events')/event/'format'
        (args.output/(event.replace('/', '-')+'.format')).write_text(source.read_text())
    try:
        for name, n, b, enabled, affinity, seconds in cases:
            quiet_since = time.monotonic()
            while True:
                active = activity()
                if active:
                    quiet_since = time.monotonic()
                    print('等待无其他编译/压测窗口', active, flush=True)
                elif time.monotonic() - quiet_since >= args.quiet_seconds:
                    break
                time.sleep(5)
            folder = args.output/name
            folder.mkdir()
            os.chown(folder, uid, gid)
            workload = ['setpriv', f'--reuid={uid}', f'--regid={gid}', '--init-groups', '/usr/bin/python3', 'test/shared_net/benchmark.py',
                        '--binary', 'build-local-latency/bin/shared_net_benchmark', '--gateway', 'build-local-latency/bin/dzipc_gateway',
                        '--mode', 'shared_v1', '--output', str(folder), '--subscribers', str(n), '--bytes', str(b), '--seconds', str(seconds), '--rate', '100', '--receive-trace', '--publish-cpu-trace']
            if affinity:
                workload += ['--affinity', json.dumps(affinity)]
            command = workload
            if enabled:
                stack_term = '/call-graph=fp/' if args.kernel_stacks else ''
                command = ['perf', 'record', '-a', '--clockid', 'mono', '--no-buildid', '--no-buildid-cache', '-m', '1024', '-o', str(folder/'perf.data'),
                           *(['--kernel-callchains', '--no-user-callchains'] if args.kernel_stacks else []),
                           '-e', 'sched:sched_waking'+stack_term, '--filter', f'comm == "{COMM}"',
                           '-e', 'sched:sched_wakeup'+stack_term, '--filter', f'comm == "{COMM}"',
                           '-e', 'sched:sched_switch', '--filter', f'prev_comm == "{COMM}" || next_comm == "{COMM}"',
                           '-e', 'sched:sched_migrate_task', '--filter', f'comm == "{COMM}"', '-e', 'power:cpu_idle', '--', *workload]
            window = {'case': name, 'command': command, 'before': snapshot(), 'activity': []}
            with (folder/'run.log').open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                while process.poll() is None:
                    window['activity'].append({'time_ns': time.monotonic_ns(), 'processes': activity()})
                    time.sleep(1)
            window['returncode'] = process.returncode
            window['after'] = snapshot()
            if hashes() != frozen:
                raise RuntimeError('采样二进制或工装改变')
            if enabled and (folder/'perf.data').exists():
                with gzip.open(folder/'events.txt.gz', 'wt') as events, (folder/'decode.log').open('w') as errors:
                    decode = subprocess.Popen(['perf', 'script', '--ns', '--show-lost-events', '--no-call-graph', '-i', str(folder/'perf.data'), '-F', 'trace:tid,cpu,time,event,trace'], stdout=subprocess.PIPE, stderr=errors, text=True)
                    shutil.copyfileobj(decode.stdout, events)
                    window['decode_returncode'] = decode.wait()
                if args.kernel_stacks:
                    with gzip.open(folder/'stacks.txt.gz', 'wt') as stacks, (folder/'stacks-decode.log').open('w') as errors:
                        decode = subprocess.Popen(['perf', 'script', '--ns', '--show-lost-events', '-i', str(folder/'perf.data'),
                                                   '-F', 'trace:tid,cpu,time,event,trace,ip,sym'], stdout=subprocess.PIPE, stderr=errors, text=True)
                        shutil.copyfileobj(decode.stdout, stacks)
                        window['stacks_decode_returncode'] = decode.wait()
                with (folder/'perf-header.txt').open('w') as header:
                    subprocess.run(['perf', 'report', '--stdio', '--header-only', '-i', str(folder/'perf.data')], stdout=header, stderr=subprocess.STDOUT, check=True)
            for pattern in ('*.csv', 'perf.data'):
                for source in folder.glob(pattern):
                    with source.open('rb') as a, gzip.open(str(source)+'.gz', 'wb') as z:
                        shutil.copyfileobj(a, z)
                    source.unlink()
            window['files'] = {str(p.relative_to(args.output)): hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir() if p.is_file()}
            manifest['windows'].append(window)
            (args.output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
            print(name, '退出码', process.returncode, flush=True)
            if process.returncode or window.get('decode_returncode', 0) or window.get('stacks_decode_returncode', 0):
                raise RuntimeError('采样或解析失败，保留证据后停止')
    finally:
        manifest['settings_after'] = snapshot()
        (args.output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
        for path in [args.output, *args.output.rglob('*')]:
            os.chown(path, uid, gid)


if __name__ == '__main__':
    main()
