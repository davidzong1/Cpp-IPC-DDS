#!/usr/bin/env python3
"""复制内层墙钟、线程CPU与采样包围成本；保留有符号差值。"""
import argparse
from collections import defaultdict
import csv
import gzip
import json
import math
from pathlib import Path

from benchmark import quantiles
from summarize_publish import decode


def decode_cpu(row):
    stages = decode(row)
    start, end = int(row['copy_begin_ns']), int(row['copy_end_ns'])
    outer_start, outer_end = int(row['copy_outer_begin_ns']), int(row['copy_outer_end_ns'])
    cpu_start, cpu_end = int(row['copy_cpu_begin_ns']), int(row['copy_cpu_end_ns'])
    first, last = int(row['copy_begin_cpu']), int(row['copy_end_cpu'])
    if not (int(row['loan_end_ns']) <= outer_start <= start <= end <= outer_end <= int(row['commit_begin_ns'])):
        raise ValueError('CPU诊断墙钟包围区间不完整或乱序')
    if cpu_start <= 0 or cpu_end < cpu_start or first < 0 or last < 0:
        raise ValueError('CPU时钟或CPU编号缺失/乱序')
    wall, cpu = end - start, cpu_end - cpu_start
    return {'sequence': int(row['sequence']), 'start_ns': int(row['start_ns']),
            'wall_ns': wall, 'cpu_ns': cpu, 'gap_ns': wall - cpu,
            'probe_bracket_ns': outer_end - outer_start - wall,
            'begin_cpu': first, 'end_cpu': last, 'publish_ns': sum(stages.values())}


def statistics(records):
    if not records:
        raise ValueError('没有复制CPU样本')
    return {'count': len(records), **{key: dict(quantiles([r[key] for r in records]),
                mean_us=sum(r[key] for r in records) / len(records) / 1000)
            for key in ('wall_ns', 'cpu_ns', 'gap_ns', 'probe_bracket_ns', 'publish_ns')},
            'cpu_over_wall_sum': sum(r['cpu_ns'] for r in records) / sum(r['wall_ns'] for r in records),
            'cpu_minus_probe_bracket_over_wall_sum': sum(r['cpu_ns'] - r['probe_bracket_ns'] for r in records) / sum(r['wall_ns'] for r in records),
            'different_endpoint_cpu': sum(r['begin_cpu'] != r['end_cpu'] for r in records)}


def summarize(rows):
    records = [decode_cpu(row) for row in rows]
    result = statistics(records)
    slow = sorted(records, key=lambda r: r['wall_ns'])[-max(1, math.ceil(len(records) * .1)):]
    result['slowest_copy_ten_percent'] = statistics(slow)
    groups = defaultdict(list)
    for r in records:
        groups[(r['begin_cpu'], r['end_cpu'])].append(r)
    result['by_endpoint_cpus'] = [{'begin_cpu': a, 'end_cpu': b, **statistics(group)}
                                  for (a, b), group in sorted(groups.items())]
    groups.clear()
    origin = min(r['start_ns'] for r in records)
    for r in records:
        groups[(r['start_ns'] - origin) // 1_000_000_000].append(r)
    result['seconds'] = [{'second': second, **statistics(group)} for second, group in sorted(groups.items())]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    results = []
    for case in json.loads((args.directory / 'cases.json').read_text()):
        with gzip.open(args.directory / case['case'] / 'pub.csv.gz', 'rt') as stream:
            rows = list(csv.DictReader(stream))
        expected = case['rate'] * case['seconds']
        if case['returncode'] or len(rows) != expected or len({r['sequence'] for r in rows}) != expected:
            raise ValueError('窗口失败或发布样本不完整/重复')
        result = summarize(rows)
        result.update(case=case['case'], mode=case['mode'], subscribers=case['subscribers'], group=case['group'])
        results.append(result)
    (args.directory / 'copy-cpu-summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    for r in results:
        print(r['case'], '复制墙钟/CPU均值µs', round(r['wall_ns']['mean_us'], 3), round(r['cpu_ns']['mean_us'], 3),
              '最慢10%CPU占比', round(r['slowest_copy_ten_percent']['cpu_over_wall_sum'], 4),
              '起止CPU不同', r['different_endpoint_cpu'])


if __name__ == '__main__':
    main()
