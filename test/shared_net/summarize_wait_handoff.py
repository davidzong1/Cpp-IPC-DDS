#!/usr/bin/env python3
"""按同一序号关联发布/接收阶段；不把不同样本的分位数相减。"""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path


def rows(path):
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rt') as source:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(source)]


def stats(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), 'mean_us': sum(values) / len(values) / 1000,
            **{f'p{q}_us': values[math.ceil(len(values) * q / 100) - 1] / 1000 for q in (50, 99)}}


def summarize(folder):
    pub_files = sorted(folder.glob('pub.csv*'))
    if len(pub_files) != 1:
        raise ValueError(f'{folder}: 发布原始文件必须唯一')
    pubs = {row['sequence']: row for row in rows(pub_files[0])}
    samples = [row for path in sorted(folder.glob('sub*.csv*')) for row in rows(path)]
    if any(row['sequence'] not in pubs for row in samples):
        raise ValueError(f'{folder}: 存在未关联发布的接收样本')

    def intervals(selected):
        values = {key: [] for key in ('e2e', 'notify_to_wait_end', 'wait_end_to_recv',
                  'recv', 'dispatch', 'handoff', 'assist_acquire', 'get_cpu')}
        for row in selected:
            values['e2e'].append(row['elapsed_ns'])
            pairs = {'notify_to_wait_end': (pubs[row['sequence']].get('notify_begin_ns', 0), row.get('wait_end_ns', 0)),
                     'wait_end_to_recv': (row.get('wait_end_ns', 0), row.get('recv_begin_ns', 0)),
                     'recv': (row.get('recv_begin_ns', 0), row.get('recv_return_ns', 0)),
                     'dispatch': (row.get('recv_return_ns', 0), row.get('enqueue_before_ns', 0))}
            for key, (begin, end) in pairs.items():
                if begin and end >= begin:
                    values[key].append(end - begin)
            for key, field in (('handoff', 'handoff_ns'), ('assist_acquire', 'assist_acquire_ns'), ('get_cpu', 'get_cpu_ns')):
                if row.get(field, 0):
                    values[key].append(row[field])
        return {key: stats(value) for key, value in values.items()}

    return {'case': folder.name, 'publish': stats([row['elapsed_ns'] for row in pubs.values()]),
            'copy': stats([row['copy_end_ns'] - row['copy_begin_ns'] for row in pubs.values()
                           if row.get('copy_begin_ns', 0) and row['copy_end_ns'] >= row['copy_begin_ns']]),
            'all': intervals(samples),
            'slowest_one_percent': intervals(sorted(samples, key=lambda row: row['elapsed_ns'], reverse=True)[:math.ceil(len(samples) / 100)]),
            'assisted_count': sum(row.get('assisted', 0) for row in samples)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    folders = [args.directory] if (args.directory / 'result.json').exists() else sorted(path.parent for path in args.directory.glob('*/result.json'))
    if not folders:
        raise ValueError('未找到采样窗口')
    print(json.dumps([summarize(path) for path in folders], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
