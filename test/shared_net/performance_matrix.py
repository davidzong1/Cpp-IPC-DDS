#!/usr/bin/env python3
"""54 个顺序采样窗口；原始 CSV 无损压缩，保留失败轮次。"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--current', required=True)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError('证据目录必须为空，禁止混合或覆盖旧轮次')
    files = [Path(args.baseline), Path(args.current), Path(args.gateway),
             Path(args.current).parent.parent/'lib/libipc.so', Path(args.baseline).parent/'lib/libipc.so']
    def hashes(): return {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    frozen = hashes()
    (output/'binary_manifest.json').write_text(json.dumps(frozen, indent=2)+'\n')
    manifest = {'baseline': 'f066a82', 'current': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'benchmark_sha256': hashlib.sha256(Path('test/shared_net/benchmark.cc').read_bytes()).hexdigest(),
        'compiler': subprocess.check_output(['c++', '--version'], text=True).splitlines()[0],
        'flags': '-O2 -g -DNDEBUG -std=c++17', 'platform': platform.platform(),
        'cpus': sorted(os.sched_getaffinity(0)), 'binary_sha256': frozen,
        'cpu_governors': {str(p): p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')}, 'clock': '同主机 CLOCK_MONOTONIC', 'commands': []}
    results = []
    for repeat in (1, 2, 3):
        for subscribers in (1, 8, 32):
            for size in (64, 4096, 1048576):
                for mode in (('baseline', 'shared_v1') if repeat % 2 else ('shared_v1', 'baseline')):
                    case = f'{mode}-sub{subscribers}-bytes{size}-run{repeat}'
                    folder = output/case
                    command = [sys.executable, 'test/shared_net/benchmark.py', '--binary', args.baseline if mode == 'baseline' else args.current,
                        '--gateway', args.gateway, '--mode', mode, '--output', str(folder), '--subscribers', str(subscribers),
                        '--bytes', str(size), '--seconds', '30', '--rate', '100']
                    manifest['commands'].append(command)
                    (output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
                    with (output/(case+'.log')).open('w') as log:
                        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
                    if hashes() != frozen: raise RuntimeError('采样二进制已改变，终止并保留证据')
                    result = {'case': case, 'returncode': completed.returncode, 'repeat': repeat}
                    if (folder/'result.json').exists():
                        result.update(json.loads((folder/'result.json').read_text()))
                    results.append(result)
                    (output/'cases.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                    for csv in folder.glob('*.csv'):
                        with csv.open('rb') as source, gzip.open(str(csv)+'.gz', 'wb') as target:
                            shutil.copyfileobj(source, target)
                        csv.unlink()
                    print(json.dumps({'completed': len(results), 'total': 54, 'case': case,
                                      'returncode': completed.returncode, 'publish': {k: v for k, v in result.get('publish', {}).items() if k != 'diagnostics'},
                                      'first_receiver': result.get('receivers', [None])[0]}, ensure_ascii=False), flush=True)
    sys.exit(1 if any(case['returncode'] for case in results) else 0)


if __name__ == '__main__':
    main()
