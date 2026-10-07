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
    result['system_cpu_ticks'] = [int(value) for value in Path('/proc/stat').read_text().splitlines()[0].split()[1:]]
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
    parser.add_argument('--suite', choices=('smoke', 'paired', 'paired32', 'copy', 'stacks', 'stacks-affinity', 'stacks-receiver-affinity', 'd04', 'd04-smoke', 'candidate'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quiet-seconds', type=int, default=0, help='每窗前连续无编译/压测的秒数')
    parser.add_argument('--kernel-stacks', action='store_true', help='仅唤醒事件增加内核栈；独立诊断，不作无栈配对')
    parser.add_argument('--binary', type=Path, default=ROOT/'build-local-latency/bin/shared_net_benchmark')
    parser.add_argument('--gateway', type=Path, default=ROOT/'build-local-latency/bin/dzipc_gateway')
    parser.add_argument('--library', type=Path, default=ROOT/'build-local-latency/lib/libipc.so')
    parser.add_argument('--mode', choices=('baseline', 'shared_v1'), default='shared_v1')
    parser.add_argument('--baseline-binary', type=Path)
    parser.add_argument('--baseline-library', type=Path)
    parser.add_argument('--baseline-worktree', type=Path, default=Path('/tmp/dzipc-d04-baseline'))
    parser.add_argument('--binary-worktree', type=Path, default=ROOT)
    parser.add_argument('--baseline-mode', choices=('baseline', 'shared_v1'), default='baseline')
    parser.add_argument('--baseline-gateway', type=Path)
    parser.add_argument('--instrumentation', choices=('l0', 'l1', 'l2'), default='l1', help='仅 candidate 套件；l2含perf')
    parser.add_argument('--receive-assist', choices=('0', '1'), default='1')
    args = parser.parse_args()
    if os.geteuid() != 0 or not os.environ.get('SUDO_UID'):
        raise RuntimeError('请通过 sudo 运行，业务工装必须降回原用户')
    uid, gid = int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID'])
    if not uid:
        raise RuntimeError('业务工装不能以 root 采样')
    if args.suite in ('d04', 'd04-smoke', 'candidate') and not (args.baseline_binary and args.baseline_library):
        parser.error('d04 必须指定 baseline-binary 和 baseline-library')
    if args.suite == 'candidate' and args.baseline_mode == 'shared_v1' and not args.baseline_gateway:
        parser.error('shared_v1 对照必须指定 baseline-gateway')
    os.chdir(ROOT)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    os.chown(args.output, uid, gid)
    if args.suite == 'candidate':
        order = ('B', 'C', 'C', 'B', 'C', 'B', 'B', 'C')
        cases = [(f'sub{n}-bytes{b}-{i+1}-{label}', n, b, args.instrumentation == 'l2', None, 10, label)
                 for n, b in ((1, 4096), (32, 64), (1, 1048576)) for i, label in enumerate(order)]
    elif args.suite == 'd04-smoke':
        cases = [('smoke-A', 1, 4096, True, None, 3, 'A')]
    elif args.suite == 'd04':
        order = [('A', False), ('B', False), ('B', True), ('A', True),
                 ('A', True), ('B', True), ('B', False), ('A', False)]
        cases = [(f'sub{n}-{i+1}-{label}-perf{int(enabled)}', n, b, enabled, None, 10, label)
                 for n, b in ((1, 4096), (32, 64))
                 for i, (label, enabled) in enumerate(order)]
    elif args.suite == 'smoke':
        cases = [('smoke', 1, 4096, True, None, 3)]
    elif args.suite == 'stacks':
        if not args.kernel_stacks:
            raise ValueError('stacks 批次必须指定 --kernel-stacks')
        cases = [(f'r{r}-sub{n}-stacks', n, b, True, None, 10)
                 for r in (1, 2) for n, b in ((1, 4096), (32, 64))]
    elif args.suite == 'stacks-affinity':
        if not args.kernel_stacks:
            raise ValueError('stacks-affinity 批次必须指定 --kernel-stacks')
        # Affinity is a diagnostic variable only.  Balance unpinned and pinned
        # placement in both ABBA and BAAB blocks; do not infer a portable
        # production scheduler policy from this host-specific experiment.
        orders = (('unpinned', 'pinned', 'pinned', 'unpinned'),
                  ('pinned', 'unpinned', 'unpinned', 'pinned'))
        affinity_plans = {
            1: {'publisher': 6, 'subscribers': [4], 'gateway': 8},
            32: {'publisher': 12, 'subscribers': list(range(32)), 'gateway': 14},
        }
        cases = []
        for n, b in ((1, 4096), (32, 64)):
            for block, order in enumerate(orders, 1):
                for position, placement in enumerate(order, 1):
                    affinity = affinity_plans[n] if placement == 'pinned' else None
                    name = f'b{block}-p{position}-sub{n}-{placement}'
                    cases.append((name, n, b, True, affinity, 10))
    elif args.suite == 'stacks-receiver-affinity':
        if not args.kernel_stacks:
            raise ValueError('stacks-receiver-affinity 批次必须指定 --kernel-stacks')
        # 只固定基准实际接收线程；发布者、网关和其余订阅线程继续由调度器放置。
        # 该实验仅诊断线程放置敏感度，不直接推出生产 CPU 绑定策略。
        orders = (('unpinned', 'pinned', 'pinned', 'unpinned'),
                  ('pinned', 'unpinned', 'unpinned', 'pinned'))
        allowed_cpus = sorted(os.sched_getaffinity(0))
        if len(allowed_cpus) < 32:
            raise RuntimeError('接收线程放置对照需要 32 个允许 CPU')
        receiver_cpu_plans = {
            1: [4 if 4 in allowed_cpus else allowed_cpus[0]],
            32: allowed_cpus[:32],
        }
        cases = []
        for n, b in ((1, 4096), (32, 64)):
            for block, order in enumerate(orders, 1):
                for position, placement in enumerate(order, 1):
                    name = f'b{block}-p{position}-sub{n}-{placement}'
                    cases.append((name, n, b, True, None, 10))
    elif args.suite in ('paired', 'paired32'):
        sizes = ((1, 4096), (32, 64)) if args.suite == 'paired' else ((32, 64),)
        cases = [(f'r{r}-sub{n}-perf{int(enabled)}', n, b, enabled, None, 10)
                 for r in (1, 2) for n, b in sizes
                 for enabled in ((False, True) if r == 1 else (True, False))]
    else:
        placements = [('pp', 0, 4), ('pe', 0, 16), ('ep', 16, 4)]
        cases = [(f'r{r}-{name}', 1, 1048576, True, {'publisher': p, 'subscribers': [s], 'gateway': 2}, 10)
                 for r in (1, 2) for name, p, s in (placements if r == 1 else placements[::-1])]
    if args.suite not in ('d04', 'd04-smoke', 'candidate'): cases = [(*case, 'B') for case in cases]
    target = 'C' if args.suite == 'candidate' else 'B'
    artifacts = {target: dict(binary=str(args.binary.resolve()), library=str(args.library.resolve()),
                             gateway=str(args.gateway.resolve()), mode=args.mode, worktree=str(args.binary_worktree.resolve()))}
    if args.suite in ('d04', 'd04-smoke', 'candidate'):
        comparison = 'B' if args.suite == 'candidate' else 'A'
        artifacts[comparison] = dict(binary=str(args.baseline_binary.resolve()), library=str(args.baseline_library.resolve()),
                                    gateway=str(args.baseline_gateway.resolve()) if args.baseline_gateway else None,
                                    mode=args.baseline_mode, worktree=str(args.baseline_worktree.resolve()))
    paths = [ROOT/'test/shared_net/benchmark.cc', ROOT/'test/shared_net/benchmark.py', Path(__file__),
             ROOT/'test/shared_net/summarize_perf_scheduler.py', ROOT/'test/shared_net/audit_perf_scheduler.py']
    for artifact in artifacts.values():
        paths += [Path(artifact['binary']), Path(artifact['library'])]
        if artifact['mode'] == 'shared_v1': paths.append(Path(artifact['gateway']))
        worktree = Path(artifact['worktree'])
        artifact['head'] = subprocess.check_output(['git', '-c', f'safe.directory={worktree}', 'rev-parse', 'HEAD'], cwd=worktree, text=True).strip()
        for relative in ('CMakeLists.txt', 'src/libipc/recv_wait_set.cpp', 'src/libipc/ipc.cpp', 'src/libipc/waiter.h',
                         'src/dzIPC/shm_pub_sub_ipc.cc', 'include/libipc/recv_wait_set.h',
                         'include/dzIPC/detail/shm_sub_seam.h', 'include/ipc_msg/std_msgs/std_image.hpp',
                         'include/ipc_msg/ipc_msg_base/dzflat.h', 'generator/batch_msg_srv_generator.py'):
            paths.append(worktree/relative)
        for relative in ('src/libipc/publish_trace.cpp', 'include/libipc/detail/publish_trace.h'):
            if (worktree/relative).exists(): paths.append(worktree/relative)
        commands = Path(artifact['binary']).parent.parent/'compile_commands.json'
        if commands.exists(): paths.append(commands)
    def hashes():
        return {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    frozen = hashes()
    snapshots = args.output/'source-snapshots'
    snapshots.mkdir()
    source_snapshots = {}
    for path in paths:
        if path.suffix in ('.py', '.cc', '.cpp', '.h', '.hpp', '.txt', '.json'):
            sha = frozen[str(path.resolve())]
            saved = snapshots/sha
            saved.write_bytes(path.read_bytes())
            source_snapshots[str(path.resolve())] = str(saved.relative_to(args.output))
    manifest = {'head': subprocess.check_output(['git', '-c', f'safe.directory={ROOT}', 'rev-parse', 'HEAD'], text=True).strip(), 'hashes': frozen,
                'perf_version': subprocess.check_output(['perf', '--version'], text=True).strip(), 'clock': 'CLOCK_MONOTONIC / perf --clockid mono',
                'workload_uid': uid, 'kernel_stacks': args.kernel_stacks, 'quiet_seconds': args.quiet_seconds,
                'artifacts': artifacts, 'source_snapshots': source_snapshots,
                'suite': args.suite, 'instrumentation': args.instrumentation,
                'cases': cases, 'windows': [], 'settings_before': snapshot()}
    for event in ('sched/sched_waking', 'sched/sched_wakeup', 'sched/sched_switch', 'sched/sched_migrate_task', 'power/cpu_idle'):
        source = Path('/sys/kernel/tracing/events')/event/'format'
        (args.output/(event.replace('/', '-')+'.format')).write_text(source.read_text())
    try:
        for name, n, b, enabled, affinity, seconds, label in cases:
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
            artifact = artifacts[label]
            workload = ['setpriv', f'--reuid={uid}', f'--regid={gid}', '--init-groups', '/usr/bin/python3', str(ROOT/'test/shared_net/benchmark.py'),
                        '--binary', artifact['binary'], '--gateway', artifact['gateway'] or '/unused-baseline-gateway',
                        '--mode', artifact['mode'], '--output', str(folder), '--subscribers', str(n), '--bytes', str(b),
                        '--seconds', str(seconds), '--rate', '100', '--receive-assist', args.receive_assist]
            if args.suite != 'candidate' or args.instrumentation != 'l0':
                workload += ['--receive-trace', '--publish-cpu-trace']
            if affinity:
                workload += ['--affinity', json.dumps(affinity)]
            if args.suite == 'stacks-receiver-affinity' and name.endswith('-pinned'):
                workload += ['--receiver-affinity', json.dumps(receiver_cpu_plans[n])]
            command = workload
            if enabled:
                stack_term = '/call-graph=fp/' if args.kernel_stacks else ''
                command = ['perf', 'record', '-a', '--clockid', 'mono', '--no-buildid', '--no-buildid-cache', '-m', '1024', '-o', str(folder/'perf.data'),
                           *(['--kernel-callchains', '--no-user-callchains'] if args.kernel_stacks else []),
                           '-e', 'sched:sched_waking'+stack_term, '--filter', f'comm == "{COMM}"',
                           '-e', 'sched:sched_wakeup'+stack_term, '--filter', f'comm == "{COMM}"',
                           '-e', 'sched:sched_switch', '--filter', f'prev_comm == "{COMM}" || next_comm == "{COMM}"',
                           '-e', 'sched:sched_migrate_task', '--filter', f'comm == "{COMM}"', '-e', 'power:cpu_idle', '--', *workload]
            window = {'case': name, 'artifact': label, 'command': command, 'before': snapshot(), 'activity': []}
            environment = dict(os.environ, LD_LIBRARY_PATH=str(Path(artifact['library']).parent))
            with (folder/'run.log').open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment, start_new_session=True)
                while process.poll() is None:
                    window['activity'].append({'time_ns': time.monotonic_ns(), 'processes': activity()})
                    time.sleep(1)
            window['returncode'] = process.returncode
            window['after'] = snapshot()
            if not process.returncode:
                result = json.loads((folder/'result.json').read_text())
                for side in ('before', 'after'):
                    if any(p['loaded_libraries'] != [artifact['library']] for p in result[side]):
                        raise RuntimeError('实际加载库与指定 library 不一致')
            if hashes() != frozen:
                raise RuntimeError('采样二进制或工装改变')
            if enabled and (folder/'perf.data').exists():
                with gzip.open(folder/'events.txt.gz', 'wt') as events, (folder/'decode.log').open('w') as errors:
                    decode = subprocess.Popen(['perf', 'script', '--ns', '--show-lost-events', '--hide-call-graph', '-i', str(folder/'perf.data'), '-F', 'trace:tid,cpu,time,event,trace'], stdout=subprocess.PIPE, stderr=errors, text=True)
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
