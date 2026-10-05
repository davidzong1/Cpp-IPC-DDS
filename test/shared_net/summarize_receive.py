#!/usr/bin/env python3
"""接收诊断：同一批最慢1%消息分解；不将独立阶段p99相加。"""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path
import statistics
from benchmark import quantiles


def decode(row, complete):
    row = {key: int(value) for key, value in row.items()}
    read, elapsed = row['read_ns'], row['elapsed_ns']
    start, received = read - elapsed, row['recv_return_ns']
    if not received or not start <= received <= read:
        raise ValueError('接收打点缺失或顺序错误')
    stages = {'publish_to_recv_return': received - start}
    overlap = False
    call = None
    if complete:
        begin, enqueue, dequeue = (row[key] for key in ('recv_begin_ns', 'enqueue_before_ns', 'dequeue_after_ns'))
        if not all((begin, enqueue, dequeue)) or not begin <= received <= enqueue <= dequeue <= read:
            raise ValueError('完整接收打点缺失或顺序错误')
        stages.update(recv_return_to_enqueue_before=enqueue - received,
                      enqueue_before_to_dequeue_after=dequeue - enqueue,
                      dequeue_after_to_api_return=read - dequeue)
        overlap = begin < start
        call = received - begin
    else:
        stages['recv_return_to_api_return'] = read - received
    if sum(stages.values()) != elapsed:
        raise ValueError('分段和不等于同条消息端到端耗时')
    return {'elapsed_ns': elapsed, 'stages': stages, 'recv_call_ns': call, 'recv_overlaps_publish': overlap}


def summarize(rows, complete):
    records = sorted((decode(row, complete) for row in rows), key=lambda row: row['elapsed_ns'])
    if not records:
        raise ValueError('没有接收样本')
    slow = records[-max(1, math.ceil(len(records) * .01)):]
    sums = {key: sum(record['stages'][key] for record in slow) for key in records[0]['stages']}
    total = sum(record['elapsed_ns'] for record in slow)
    return {'count': len(records), 'complete_trace': complete,
            'end_to_end': quantiles([record['elapsed_ns'] for record in records]),
            'stages': {key: quantiles([record['stages'][key] for record in records]) for key in sums},
            'recv_call': quantiles([record['recv_call_ns'] for record in records if record['recv_call_ns'] is not None]),
            'recv_overlaps_publish': sum(record['recv_overlaps_publish'] for record in records),
            'slowest_one_percent': {'count': len(slow), 'mean_elapsed_us': total / len(slow) / 1000,
                'stage_mean_us': {key: value / len(slow) / 1000 for key, value in sums.items()},
                'stage_fraction': {key: value / total for key, value in sums.items()}}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    cases = json.loads((args.directory / 'cases.json').read_text())
    results = []
    for case in cases:
        if case['returncode'] != 0:
            raise ValueError('存在失败窗口：' + case['case'])
        rows = []
        for path in sorted((args.directory / case['case']).glob('sub*.csv.gz')):
            with gzip.open(path, 'rt') as stream:
                rows.extend(csv.DictReader(stream))
        result = summarize(rows, case['mode'] == 'shared_v1')
        result.update({key: case[key] for key in ('case', 'mode', 'repeat', 'subscribers', 'bytes')})
        if result['count'] != sum(receiver['count'] for receiver in case['receivers']):
            raise ValueError('原始记录与窗口汇总样本数不一致')
        result['lost'] = sum(receiver['lost'] for receiver in case['receivers'])
        result['invalid'] = sum(receiver['invalid'] for receiver in case['receivers'])
        result['duplicates'] = sum(receiver['duplicates'] for receiver in case['receivers'])
        results.append(result)
    (args.directory / 'receive-summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    lines = ['# 接收路径分段诊断', '',
             '显式开启内部测试缝，有观测成本，不能替代无打点正式验收。各行先逐窗口合并接收样本，再取三轮分位数的中位数。', '',
             '| 模式 | 订阅者/载荷B | 端到端p50/p99 µs | 发布→recv返回p99 µs | recv返回→入队前p99 µs | 入队前→出队后p99 µs | 出队后→API返回p99 µs |',
             '|---|---:|---:|---:|---:|---:|---:|']
    for subscribers, size in sorted({(case['subscribers'], case['bytes']) for case in results}):
        for mode in ('baseline', 'shared_v1'):
            group = [case for case in results if (case['subscribers'], case['bytes'], case['mode']) == (subscribers, size, mode)]
            median = lambda getter: statistics.median(getter(case) for case in group)
            latency = f"{median(lambda c: c['end_to_end']['p50_us']):.3f}/{median(lambda c: c['end_to_end']['p99_us']):.3f}"
            stages = []
            for key in ('publish_to_recv_return', 'recv_return_to_enqueue_before', 'enqueue_before_to_dequeue_after', 'dequeue_after_to_api_return'):
                stages.append(f"{median(lambda c: c['stages'][key]['p99_us']):.3f}" if key in group[0]['stages'] else '未采样')
            lines.append(f"| {mode} | {subscribers}/{size} | {latency} | " + ' | '.join(stages) + ' |')
    lines.extend(['', '发布→recv返回包含发布提交、共享内存等待、worker调度、lease取得和recv本身；不能单独解释为内核唤醒耗时。入队前→出队后包含入队操作、队列驻留、唤醒与出队。recv_call单独报告，不与上述分段重复累加。', '',
                  '下表选择每个shared_v1窗口端到端最慢的1%消息，再累计同一批消息的各段耗时占比；不同阶段的独立p99不能相加。', '',
                  '| 窗口 | 样本数 | 最慢1%平均µs | 发布→recv返回 | recv返回→入队前 | 入队前→出队后 | 出队后→API返回 |',
                  '|---|---:|---:|---:|---:|---:|---:|'])
    for case in results:
        if case['mode'] != 'shared_v1':
            continue
        tail = case['slowest_one_percent']
        fractions = ' | '.join(f'{value * 100:.1f}%' for value in tail['stage_fraction'].values())
        lines.append(f"| {case['case']} | {case['count']} | {tail['mean_elapsed_us']:.3f} | {fractions} |")
    (args.directory / 'receive-summary.md').write_text('\n'.join(lines) + '\n')
    print(json.dumps({'windows': len(results), 'received': sum(case['count'] for case in results),
                      'lost': sum(case['lost'] for case in results), 'invalid': sum(case['invalid'] for case in results),
                      'duplicates': sum(case['duplicates'] for case in results)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
