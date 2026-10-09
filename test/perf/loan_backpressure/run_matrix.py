#!/usr/bin/env python3
"""固定容量 loan 背压验证：每窗固定尝试数，串行运行以避免互扰。"""
import argparse
import csv
import itertools
import json
import pathlib
import subprocess
import time


def parse_rows(text, marker):
    return [dict(item.split('=', 1) for item in line.split()[1:])
            for line in text.splitlines() if line.startswith(marker + ' ')]


def validate(messages, pools, code, attempts, subscribers, require_all):
    """仅将明确的池信用超时视作合法拒绝，交付与回收始终是硬判据。"""
    try:
        if code != 0 or len(messages) != 3 * subscribers or len(pools) != 3:
            return False
        if {(int(row['window']), int(row['subscriber'])) for row in messages} != {
                (window, subscriber) for window in range(1, 4) for subscriber in range(subscribers)}:
            return False
        if {int(row['window']) for row in pools} != {1, 2, 3}:
            return False
        for row in messages:
            published, rejected = int(row['published']), int(row['pool_exhausted'])
            if not (int(row['attempted']) == attempts and 0 < published <= attempts and
                    published + rejected == attempts and int(row['publish_failed']) == rejected and
                    int(row['received']) == published):
                return False
            if require_all and rejected:
                return False
            if any(int(row[key]) for key in ('other_failed', 'missing', 'unexpected', 'duplicate',
                                              'corrupt', 'out_of_order')):
                return False
            if int(row.get('queue_evicted', 0)) or int(row.get('consumer_lag', 0)):
                return False
        for row in pools:
            first = next(message for message in messages if message['window'] == row['window'])
            if not (int(row['capacity']) == 10 and int(row['final_free']) == 10 and
                    int(row['consistent']) == 1 and int(row['loan_attempt']) == attempts and
                    int(row['loan_success']) == int(first['published']) and
                    int(row['loan_reject']) == int(first['pool_exhausted'])):
                return False
            if any(int(row[key]) for key in ('waiters', 'duplicate_return', 'invalid_storage_id',
                                              'pool_chain_corrupt')):
                return False
            for message in (message for message in messages if message['window'] == row['window']):
                if any(message[key] != first[key] for key in ('published', 'publish_failed', 'pool_exhausted')):
                    return False
        return True
    except (KeyError, ValueError, StopIteration):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/bin/loan_phase_latency_measure')
    parser.add_argument('--output', default='test/perf/loan_backpressure/optimized')
    parser.add_argument('--messages', type=int, default=512)
    parser.add_argument('--timeout', type=int, default=1000)
    parser.add_argument('--stress', action='store_true')
    parser.add_argument('--require-all', action='store_true', help='额外要求全部尝试成功，不允许信用超时拒绝')
    args = parser.parse_args()
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    runs = []
    configs = list(itertools.product(('raw', 'dzflat'), (1, 4, 8, 32), (1, 8),
                                    (64, 4096, 11000, 1048576), (0, 800)))
    if args.stress:
        configs = [('raw', 32, 8, 11000, 0), ('dzflat', 32, 8, 11000, 0),
                   ('raw', 32, 8, 1048576, 0), ('dzflat', 32, 8, 1048576, 0)]
    for i, (transport, pubs, subs, payload, rate) in enumerate(configs, 1):
        name = f'{transport}_p{pubs}_s{subs}_b{payload}_r{rate}'
        cmd = [args.binary, f'--transport={transport}', f'--pubs={pubs}', f'--subs={subs}',
               f'--msgs={args.messages}', f'--payload={payload}', '--windows=3',
               f'--rate={rate}', f'--timeout={args.timeout}', f'--require-all={int(args.require_all)}',
               f'--csv-dir={out / "occupancy"}']
        started = time.monotonic()
        try:
            result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=120)
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or ''
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors='replace')
            code, stderr = 124, str(error)
        (out / (name + '.txt')).write_text(stdout + stderr)
        messages, pools = parse_rows(stdout, 'RESULT'), parse_rows(stdout, 'POOL')
        reliable = validate(messages, pools, code, args.messages, subs, args.require_all)
        run = dict(name=name, command=cmd, exit_code=code, reliable=bool(reliable),
                   seconds=time.monotonic() - started, messages=messages, pools=pools)
        runs.append(run)
        (out / 'runs.json').write_text(json.dumps(runs, indent=2, ensure_ascii=False) + '\n')
        print(f'{i}/{len(configs)} {name} {"通过" if reliable else "失败"}', flush=True)
    fields = sorted({key for run in runs for row in run['messages'] for key in row})
    with (out / 'results.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=['configuration'] + fields)
        writer.writeheader()
        for run in runs:
            for row in run['messages']:
                writer.writerow(dict(configuration=run['name'], **row))
    return 0 if all(run['reliable'] for run in runs) else 1


if __name__ == '__main__':
    raise SystemExit(main())
