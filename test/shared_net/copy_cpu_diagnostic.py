#!/usr/bin/env python3
"""同库复制CPU诊断：ABBA/BAAB及固定CPU对照，禁止当作正式验收。"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--gateway', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('禁止覆盖已有证据')
    files = [args.binary.resolve(), args.library.resolve(), args.gateway.resolve()]
    hashes = lambda: {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    frozen = hashes()
    manifest = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'source_diff': subprocess.check_output(['git', 'diff'], text=True), 'binary_sha256': frozen,
        'benchmark_source_sha256': hashlib.sha256(Path('test/shared_net/benchmark.cc').read_bytes()).hexdigest(),
        'command': sys.argv, 'cpus': sorted(os.sched_getaffinity(0)),
        'cpu_max_khz': {str(p): p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/cpuinfo_max_freq')},
        'same_library_control': True, 'seconds': 10, 'rate': 100,
        'note': 'A为同库直接SHM；B为同库shared_v1。额外打点、固定CPU与短窗仅作诊断。', 'commands': []}
    cases, plan = [], []
    for n in (1, 8):
        for index, mode in enumerate(('baseline', 'shared_v1', 'shared_v1', 'baseline', 'shared_v1', 'baseline', 'baseline', 'shared_v1'), 1):
            plan.append(('all-cpus', n, index, mode, None))
    for cpu in (0, 16):
        affinity = {'publisher': cpu, 'gateway': 2, 'subscribers': [4]}
        for index, mode in enumerate(('baseline', 'shared_v1', 'shared_v1', 'baseline'), 1):
            plan.append((f'publisher-cpu{cpu}', 1, index, mode, affinity))
    for group, n, index, mode, affinity in plan:
        name = f'{group}-sub{n}-{index}-{mode}'
        folder = output / name
        command = [sys.executable, 'test/shared_net/benchmark.py', '--binary', str(files[0]),
            '--gateway', str(files[2]), '--mode', mode, '--output', str(folder),
            '--subscribers', str(n), '--bytes', '1048576', '--seconds', '10', '--rate', '100', '--publish-cpu-trace']
        if affinity:
            command += ['--affinity', json.dumps(affinity)]
        manifest['commands'].append(command)
        (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        with (output / (name + '.log')).open('w') as log:
            rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
        result = {'case': name, 'group': group, 'index': index, 'returncode': rc}
        if (folder / 'result.json').exists():
            result.update(json.loads((folder / 'result.json').read_text()))
        cases.append(result)
        (output / 'cases.json').write_text(json.dumps(cases, ensure_ascii=False, indent=2) + '\n')
        for path in folder.glob('*.csv'):
            with path.open('rb') as src, gzip.open(str(path) + '.gz', 'wb') as dst:
                shutil.copyfileobj(src, dst)
            path.unlink()
        if hashes() != frozen or rc:
            raise RuntimeError('诊断失败或二进制变化，已保留原始证据')
        for side in ('before', 'after'):
            if any({str(Path(p).resolve()) for p in process['loaded_libraries']} != {str(files[1])} for process in result[side]):
                raise RuntimeError('实际加载库与同库对照计划不符')
        print(json.dumps({'completed': len(cases), 'total': len(plan), 'case': name,
                          'publish': result['publish_latency']}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
