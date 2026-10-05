#!/usr/bin/env python3
"""顺序跟踪发布者和首个订阅进程，保存前后版本所有futex唤醒（仅诊断）。"""
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


def summarize(path):
    pending, groups = {}, defaultdict(Counter)
    raw = path.read_text()
    for line in raw.splitlines():
        match = re.match(r'^(\d+)\s+(.*)$', line)
        if not match:
            continue
        tid, body = match.groups()
        if '<unfinished ...>' in body:
            pending[tid] = body.split('<unfinished ...>')[0]
            continue
        if body.startswith('<... futex resumed>'):
            body = pending.pop(tid, '') + body.split('resumed>', 1)[1]
        match = re.search(r'futex\((0x[0-9a-f]+), (FUTEX_WAKE(?:_PRIVATE)?), (\d+).*\)\s+=\s+(\d+)', body)
        if match:
            address, operation, limit, result = match.groups()
            groups[(address, operation, int(limit))][int(result)] += 1
    rows = [{'address': address, 'operation': operation, 'limit': limit,
             'calls': sum(counts.values()), 'woken': sum(n * count for n, count in counts.items()),
             'zero_wake_calls': counts[0], 'return_histogram': dict(sorted(counts.items()))}
            for (address, operation, limit), counts in groups.items()]
    rows.sort(key=lambda r: r['calls'], reverse=True)
    if len(re.findall(r'FUTEX_WAKE(?:_PRIVATE)?,', raw)) != sum(r['calls'] for r in rows):
        raise ValueError(f'{path}: 存在失败、未完成或无法解析的唤醒调用')
    return {'calls': sum(r['calls'] for r in rows), 'woken': sum(r['woken'] for r in rows),
            'zero_wake_calls': sum(r['zero_wake_calls'] for r in rows), 'addresses': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before-binary', required=True, type=Path)
    parser.add_argument('--before-library', required=True, type=Path)
    parser.add_argument('--current-binary', required=True, type=Path)
    parser.add_argument('--current-library', required=True, type=Path)
    parser.add_argument('--gateway', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('证据目录必须为空')
    versions = {'before': (args.before_binary.resolve(), args.before_library.resolve()),
                'current': (args.current_binary.resolve(), args.current_library.resolve())}
    files = [args.gateway.resolve()] + [p for pair in versions.values() for p in pair]
    hashes = lambda: {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    frozen = hashes()
    evidence = {'binary_sha256': frozen, 'command': sys.argv, 'cases': [],
        'note': '100Hz，预热2秒+测量2秒；strace覆盖初始化和退出，跟踪发布者及sub0全部线程。只比较唤醒计数，延迟不用于验收；地址不跨进程匹配。'}
    for subscribers in (1, 32):
        for version, (binary, library) in versions.items():
            name = f'{version}-sub{subscribers}'
            prefix = output / name
            wrapper = output / (name + '.sh')
            # 所有应用进程显式选库；网关由benchmark独立启动，使用冻结的当前网关。
            wrapper.write_text('#!/bin/sh\n' + f'export LD_LIBRARY_PATH={shlex.quote(str(library.parent))}\n' +
                'if [ "$1" = pub ]; then\n' +
                f'  exec strace -D -qq -f -e trace=futex,futex_waitv -o {shlex.quote(str(prefix) + ".pub.strace")} {shlex.quote(str(binary))} "$@"\n' +
                'fi\ncase "$5" in\n  */sub0.csv)\n' +
                f'    exec strace -D -qq -f -e trace=futex,futex_waitv -o {shlex.quote(str(prefix) + ".sub0.strace")} {shlex.quote(str(binary))} "$@" ;;\n' +
                'esac\n' + f'exec {shlex.quote(str(binary))} "$@"\n')
            wrapper.chmod(0o755)
            command = [sys.executable, 'test/shared_net/benchmark.py', '--binary', str(wrapper),
                       '--gateway', str(args.gateway.resolve()), '--mode', 'shared_v1', '--output', str(prefix),
                       '--subscribers', str(subscribers), '--bytes', '64', '--seconds', '2', '--rate', '100']
            with (output / (name + '.log')).open('w') as log:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            if hashes() != frozen:
                raise ValueError('二进制变化')
            result = json.loads((prefix / 'result.json').read_text())
            for snapshot in result['after'][:subscribers + 1]:
                if {str(Path(p).resolve()) for p in snapshot['loaded_libraries']} != {str(library)}:
                    raise ValueError('实际加载库不符')
            row = {'case': name, 'subscribers': subscribers, 'version': version, 'command': command,
                   'publisher': summarize(Path(str(prefix) + '.pub.strace')),
                   'sub0': summarize(Path(str(prefix) + '.sub0.strace'))}
            for role in ('publisher', 'sub0'):
                if not row[role]['calls']:
                    raise ValueError('缺少唤醒样本')
            for path in prefix.glob('*.csv'):
                with path.open('rb') as source, gzip.open(str(path) + '.gz', 'wb') as target:
                    shutil.copyfileobj(source, target)
                path.unlink()
            evidence['cases'].append(row)
            (output / 'wake-counts.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n')
            print(json.dumps({'case': name, **{role: {k: v for k, v in row[role].items() if k != 'addresses'}
                                              for role in ('publisher', 'sub0')}}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
