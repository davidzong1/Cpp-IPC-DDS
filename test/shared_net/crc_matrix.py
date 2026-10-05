#!/usr/bin/env python3
"""CRC前后库的固定负载网络对照；冻结库和可执行文件，保留全部样本。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--before-library', required=True)
    parser.add_argument('--after-library', required=True)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--probe', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise RuntimeError('输出目录必须为空，不能覆盖旧窗口')
    libraries = {mode: Path(path).resolve() for mode, path in
                 [('before', args.before_library), ('after', args.after_library)]}
    files = [Path(args.gateway), Path(args.probe), *(path/'libipc.so' for path in libraries.values())]
    def hashes():
        return {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    frozen = hashes()
    manifest = {'before_source': '2b90e28', 'after_source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                'hashes': frozen, 'hot_rate': 70, 'cold_rate': 100, 'seconds': 30, 'commands': []}
    results = []
    for repeat in (1, 2, 3):
        for same in (True, False):
            for mode in (('before', 'after') if repeat % 2 else ('after', 'before')):
                case = f'{mode}-{"same" if same else "different"}-{repeat}'
                command = [sys.executable, 'test/shared_net/fairness.py', '--gateway', args.gateway, '--probe', args.probe,
                           '--hot-rate', '70', '--output', str(root/(case+'.json'))]
                if same:
                    command.append('--same-shard')
                manifest['commands'].append({'case': case, 'command': command, 'library_path': str(libraries[mode])})
                (root/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
                with (root/(case+'.log')).open('w') as log:
                    run = subprocess.run(command, env=dict(os.environ, LD_LIBRARY_PATH=str(libraries[mode]), DZIPC_TEST_LATENCY_SAMPLES='1'), stdout=log, stderr=subprocess.STDOUT)
                assert hashes() == frozen, '二进制在采样中改变'
                data = json.loads((root/(case+'.json')).read_text()) if (root/(case+'.json')).exists() else {}
                if data:
                    expected = str((libraries[mode]/'libipc.so').resolve())
                    assert all(m['loaded_library'] == [expected] for m in data['metrics']), data['metrics']
                    assert all(len(s['latency_samples_ns']) == s['sent'] for s in data['sent'])
                result = {'case': case, 'repeat': repeat, 'same_shard': same, 'mode': mode, 'returncode': run.returncode,
                          'cold_p99_ns': data.get('sent', [{}, {}])[1].get('p99_ns')}
                results.append(result)
                (root/'cases.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                print(json.dumps(result), flush=True)
    return int(any(r['returncode'] for r in results))


if __name__ == '__main__':
    sys.exit(main())
