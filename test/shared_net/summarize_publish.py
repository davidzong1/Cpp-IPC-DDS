#!/usr/bin/env python3
"""发布分段：同次调用守恒；不把独立分位数相加。"""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path
from benchmark import quantiles

STAMPS = ('loan_begin_ns', 'loan_end_ns', 'copy_begin_ns', 'copy_end_ns',
          'commit_begin_ns', 'notify_begin_ns', 'notify_end_ns', 'commit_end_ns')
STAGES = ('prepare', 'loan', 'before_copy', 'copy', 'before_commit',
          'commit_before_notify', 'notify', 'commit_after_notify', 'return')


def decode(row):
    start, elapsed = int(row['start_ns']), int(row['elapsed_ns'])
    stamps = [int(row[key]) for key in STAMPS]
    bounds = [start, *stamps, start + elapsed]
    if not all(stamps) or bounds != sorted(bounds) or row['success'] != '1':
        raise ValueError('发布失败、分段缺失或时间乱序')
    return dict(zip(STAGES, (b - a for a, b in zip(bounds, bounds[1:]))))


def summarize(rows):
    records = [decode(row) for row in rows]
    if not records:
        raise ValueError('没有发布样本')
    slow = sorted(records, key=lambda r: sum(r.values()))[-max(1, math.ceil(len(records) * .01)):]
    total = sum(sum(r.values()) for r in slow)
    return {'count': len(records), 'stages': {
        key: dict(quantiles([r[key] for r in records]), mean_us=sum(r[key] for r in records)/len(records)/1000)
        for key in STAGES}, 'slowest_one_percent': {
            'count': len(slow), 'mean_elapsed_us': total/len(slow)/1000,
            'stage_fraction': {key: sum(r[key] for r in slow)/total for key in STAGES}}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    results = []
    for path in sorted(args.directory.glob('*/pub.csv*')):
        opener = gzip.open if path.suffix == '.gz' else open
        with opener(path, 'rt') as stream:
            result = summarize(list(csv.DictReader(stream)))
        result['case'] = path.parent.name
        results.append(result)
    if not results:
        raise ValueError('没有原始CSV')
    (args.directory/'publish-summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
    for result in results:
        print(result['case'], {k: round(v['mean_us'], 3) for k, v in result['stages'].items()})


if __name__ == '__main__':
    main()
