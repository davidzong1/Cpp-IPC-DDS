#!/usr/bin/env python3
"""关联无打点矩阵中的发布/接收CSV，拆分同条消息的发布返回前后区间。"""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path

from benchmark import quantiles


def split(publish, receive):
    start = int(publish['start_ns'])
    elapsed = int(receive['elapsed_ns'])
    read = int(receive['read_ns'])
    duration = int(publish['elapsed_ns'])
    if read - elapsed != start or duration < 0 or elapsed < 0:
        raise ValueError('发布/接收时间戳不一致')
    overlap = min(duration, elapsed)
    return elapsed, overlap, elapsed - overlap


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    results = []
    for case in json.loads((args.directory / 'cases.json').read_text()):
        folder = args.directory / case['case']
        with gzip.open(folder / 'pub.csv.gz', 'rt') as stream:
            published = {int(row['sequence']): row for row in csv.DictReader(stream) if row['success'] == '1'}
        records = []
        for path in sorted(folder.glob('sub*.csv.gz')):
            with gzip.open(path, 'rt') as stream:
                for row in csv.DictReader(stream):
                    records.append(split(published[int(row['sequence'])], row))
        if len(records) != sum(r['count'] for r in case['receivers']):
            raise ValueError('接收原始样本数量不一致')
        records.sort()
        slow = records[-max(1, math.ceil(len(records) * .01)):]
        total = sum(r[0] for r in slow)
        results.append({'case': case['case'], 'count': len(records),
            'end_to_end': quantiles([r[0] for r in records]),
            'before_publish_return': quantiles([r[1] for r in records]),
            'after_publish_return': quantiles([r[2] for r in records]),
            'slowest_one_percent': {'count': len(slow), 'mean_elapsed_us': total / len(slow) / 1000,
                'before_publish_return_fraction': sum(r[1] for r in slow) / total,
                'after_publish_return_fraction': sum(r[2] for r in slow) / total}})
    (args.directory / 'local-stages.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'windows': len(results), 'received': sum(r['count'] for r in results)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
