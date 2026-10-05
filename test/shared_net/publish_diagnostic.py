#!/usr/bin/env python3
"""同库SHM/shared_v1分段控制；显式诊断，不能替代f066a82正式对照。"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=10)
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('不得覆盖已有证据')
    files = [build/'bin/shared_net_benchmark', build/'bin/dzipc_gateway', build/'lib/libipc.so']
    def hashes():
        return {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    frozen = hashes()
    manifest = {'source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                'working_diff': subprocess.check_output(['git', 'diff'], text=True),
                'binary_sha256': frozen, 'same_library_control': True, 'commands': []}
    results = []
    for repeat in (1, 2, 3):
        for n, size in [(1, 1048576), (32, 4096)]:
            for mode in (('baseline', 'shared_v1') if repeat % 2 else ('shared_v1', 'baseline')):
                case = f'{mode}-sub{n}-bytes{size}-run{repeat}'
                command = [sys.executable, 'test/shared_net/benchmark.py', '--binary', str(files[0]),
                    '--gateway', str(files[1]), '--mode', mode, '--output', str(output/case),
                    '--subscribers', str(n), '--bytes', str(size), '--seconds', str(args.seconds),
                    '--rate', '100', '--publish-trace', '--receive-trace']
                manifest['commands'].append(command)
                (output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
                with (output/(case+'.log')).open('w') as log:
                    rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
                result = {'case': case, 'returncode': rc, 'repeat': repeat}
                if (output/case/'result.json').exists():
                    result.update(json.loads((output/case/'result.json').read_text()))
                results.append(result)
                (output/'cases.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                for path in (output/case).glob('*.csv'):
                    with path.open('rb') as source, gzip.open(str(path)+'.gz', 'wb') as target:
                        shutil.copyfileobj(source, target)
                    path.unlink()
                print(case, rc, result.get('publish_latency'), flush=True)
                if hashes() != frozen or rc:
                    raise RuntimeError('诊断失败或二进制变化，保留已有记录')


if __name__ == '__main__':
    main()
