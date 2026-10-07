#!/usr/bin/env python3
"""从全部原始 CSV 复算通知候选的两个平衡块，不把分段分位数相加。"""
import argparse
from collections import defaultdict
import csv
import gzip
import json
from pathlib import Path

from summarize_local_latency_d02 import stats, subtract_stats


ORDER = ('B', 'C', 'C', 'B', 'C', 'B', 'B', 'C')
SCENARIOS = ((1, 4096), (32, 64), (1, 1048576))
STAGES = ('notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns',
          'scheduled_to_wait_end_ns', 'waking_to_idle_exit_ns',
          'wait_end_to_recv_return_ns', 'recv_return_to_api_ns')


def read_csv(path):
    with gzip.open(path, 'rt', newline='') as stream:
        return list(csv.DictReader(stream))


def pool(windows):
    keys = sorted({key for window in windows for key in window['_values']})
    return {key: stats(value for window in windows for value in window['_values'].get(key, []))
            for key in keys}


def summarize(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    if manifest.get('suite') != 'candidate':
        raise ValueError('必须是 candidate 套件')
    windows = manifest['windows']
    expected_cases = [(f'sub{n}-bytes{size}-{i + 1}-{label}', n, size, label)
                      for n, size in SCENARIOS for i, label in enumerate(ORDER)]
    if len(windows) != len(expected_cases) or len(manifest['cases']) != len(expected_cases):
        raise ValueError('必须保留全部 24 个窗口')
    output = []
    for window, (name, subscribers, size, label) in zip(windows, expected_cases):
        if window['case'] != name or window['artifact'] != label or window['returncode']:
            raise ValueError(f'窗口顺序或状态错误: {name}')
        folder = directory / name
        result = json.loads((folder / 'result.json').read_text())
        if (result['subscribers'], result['bytes']) != (subscribers, size):
            raise ValueError(f'场景错误: {name}')
        expected = result['rate'] * result['seconds']
        pubs = read_csv(folder / 'pub.csv.gz')
        by_sequence = {int(row['sequence']): row for row in pubs}
        if len(pubs) != expected or len(by_sequence) != expected or any(row['success'] != '1' for row in pubs):
            raise ValueError(f'发布不完整: {name}')
        paths = sorted(folder.glob('sub*.csv.gz'))
        if len(paths) != subscribers:
            raise ValueError(f'订阅者数量错误: {name}')
        samples, subscriber_stats, clusters = [], {}, defaultdict(list)
        for path in paths:
            rows = read_csv(path)
            sequences = [int(row['sequence']) for row in rows]
            if len(rows) != expected or len(set(sequences)) != expected or set(sequences) != set(by_sequence):
                raise ValueError(f'接收不完整: {name}/{path.name}')
            for row in rows:
                row['subscriber'] = path.name.split('.')[0]
                elapsed = int(row['elapsed_ns'])
                if int(row['read_ns']) - int(by_sequence[int(row['sequence'])]['start_ns']) != elapsed or elapsed < 0:
                    raise ValueError(f'时间不守恒: {name}/{path.name}')
                clusters[int(row['sequence'])].append(elapsed)
            samples.extend(rows)
            subscriber_stats[path.name.split('.')[0]] = stats(int(row['elapsed_ns']) for row in rows)
        values = {
            'e2e': [int(row['elapsed_ns']) for row in samples],
            'publish': [int(row['elapsed_ns']) for row in pubs],
            'max_sequence_e2e': [max(entries) for entries in clusters.values()],
        }
        if all('notify_end_ns' in row for row in pubs):
            values['notify'] = [int(row['notify_end_ns']) - int(row['notify_begin_ns']) for row in pubs]
            values['copy_wall'] = [int(row['copy_end_ns']) - int(row['copy_begin_ns']) for row in pubs]
        if all('copy_cpu_end_ns' in row for row in pubs):
            values['copy_cpu'] = [int(row['copy_cpu_end_ns']) - int(row['copy_cpu_begin_ns']) for row in pubs]
        if all('recv_return_ns' in row for row in samples):
            values['publish_to_recv_return'] = [int(row['recv_return_ns']) - int(by_sequence[int(row['sequence'])]['start_ns']) for row in samples]
        joined_path = folder / 'joined.csv.gz'
        cohort_stages = {}
        if joined_path.exists():
            joined = read_csv(joined_path)
            matched = {(int(row['sequence']), row['subscriber']): row for row in joined if row['matched'] == '1'}
            for stage in STAGES:
                values[stage] = [int(row[stage]) for row in matched.values() if row.get(stage)]
            ranked = sorted(samples, key=lambda row: (int(row['elapsed_ns']), int(row['sequence']), row['subscriber']))
            count = len(ranked)
            cohorts = {'middle_10_percent': ranked[count * 45 // 100:(count * 55 + 99) // 100],
                       'slowest_1_percent': ranked[-((count + 99) // 100):]}
            for cohort, selected in cohorts.items():
                entries = [matched[(int(row['sequence']), row['subscriber'])] for row in selected
                           if (int(row['sequence']), row['subscriber']) in matched]
                cohort_stages[cohort] = {
                    'selected': len(selected), 'matched': len(entries),
                    'e2e': stats(int(row['elapsed_ns']) for row in selected),
                    'stages': {stage: stats(int(row[stage]) for row in entries if row.get(stage)) for stage in STAGES},
                }
        output.append({'case': name, 'artifact': label, 'subscribers': subscribers, 'bytes': size,
                       'wire_bytes': result['wire_bytes'], 'position': len(output) % 8 + 1,
                       'metrics': {key: stats(entries) for key, entries in values.items()},
                       'subscriber_stats': subscriber_stats, 'cohorts': cohort_stages, '_values': values})
    comparisons = []
    for subscribers, size in SCENARIOS:
        selected = [window for window in output if (window['subscribers'], window['bytes']) == (subscribers, size)]
        pairs = []
        for position in (0, 2, 4, 6):
            left, right = selected[position:position + 2]
            baseline = left if left['artifact'] == 'B' else right
            candidate = right if right['artifact'] == 'C' else left
            pairs.append({'positions': [position + 1, position + 2], 'order': left['artifact'] + right['artifact'],
                          'delta_us': {key: subtract_stats(candidate['metrics'][key], baseline['metrics'][key])
                                       for key in sorted(baseline['metrics'].keys() & candidate['metrics'].keys())}})
        blocks = []
        for position in (0, 4):
            block = selected[position:position + 4]
            metrics = {label: pool([window for window in block if window['artifact'] == label]) for label in ('B', 'C')}
            blocks.append({'positions': [position + 1, position + 4], **metrics,
                           'delta_us': {key: subtract_stats(metrics['C'][key], metrics['B'][key])
                                        for key in sorted(metrics['B'].keys() & metrics['C'].keys())}})
        comparisons.append({'subscribers': subscribers, 'bytes': size, 'pairs': pairs, 'blocks': blocks})
    for window in output:
        del window['_values']
    return {'instrumentation': manifest['instrumentation'], 'order': ORDER,
            'windows': output, 'scenarios': comparisons,
            'formal_acceptance': False,
            'limitations': '平衡块合并原始样本；跨窗口同号消息不配对；32路 max 按发布序号聚类。分段分位数不能相加。'}


def markdown(summary):
    lines = ['# 通知候选的完整平衡对照', '',
             f"观测层：`{summary['instrumentation']}`；顺序：`B C C B | C B B C`；单位：微秒。", '',
             '统计从全部订阅者原始 CSV 合并计算；32 路另按发布序号取最慢接收者。仅为候选筛查。', '',
             '## 全部相邻配对 C−B', '',
             '| 场景 | 位置/顺序 | 端到端均值 | p50 | p95 | p99 | 通知 p50 | 通知 p99 |',
             '|---|---|---:|---:|---:|---:|---:|---:|']
    def cell(value):
        return '未采样' if value is None else f'{value:+.3f}'
    for scene in summary['scenarios']:
        for pair in scene['pairs']:
            e2e = pair['delta_us']['e2e']
            notify = pair['delta_us'].get('notify', {})
            lines.append('| {} SUB/{}B | {}–{} {} | {} | {} | {} | {} | {} | {} |'.format(
                scene['subscribers'], scene['bytes'], *pair['positions'], pair['order'],
                *[cell(e2e[key]) for key in ('mean_us', 'p50_us', 'p95_us', 'p99_us')],
                cell(notify.get('p50_us')), cell(notify.get('p99_us'))))
    lines += ['', '## 两个平衡块', '',
              '| 场景 | 位置 | B p50/p99 | C p50/p99 | C−B p50/p99 | B 通知 p50/p99 | C 通知 p50/p99 |',
              '|---|---|---:|---:|---:|---:|---:|']
    def quantiles(metrics, key):
        value = metrics.get(key)
        return '未采样' if value is None else f"{value['p50_us']:.3f}/{value['p99_us']:.3f}"
    for scene in summary['scenarios']:
        for block in scene['blocks']:
            lines.append('| {} SUB/{}B | {}–{} | {} | {} | {} | {} | {} |'.format(
                scene['subscribers'], scene['bytes'], *block['positions'],
                quantiles(block['B'], 'e2e'), quantiles(block['C'], 'e2e'), quantiles(block['delta_us'], 'e2e'),
                quantiles(block['B'], 'notify'), quantiles(block['C'], 'notify')))
    lines += ['', '## 逐窗结果', '',
              '| 窗口 | 接收数 | 均值 | p50 | p95 | p99 | 发布 p50 | 通知 p50 | 最慢序号 p99 |',
              '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    for window in summary['windows']:
        metrics = window['metrics']; e2e = metrics['e2e']
        lines.append('| {} | {} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {} | {:.3f} |'.format(
            window['case'], e2e['count'], *[e2e[key] for key in ('mean_us', 'p50_us', 'p95_us', 'p99_us')],
            metrics['publish']['p50_us'], cell(metrics.get('notify', {}).get('p50_us')),
            metrics['max_sequence_e2e']['p99_us']))
    lines += ['', '同组中间 10%/最慢 1% 的调度段与覆盖数，以及各订阅者分布见 `candidate-summary.json`。',
              summary['limitations']]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    summary = summarize(args.directory)
    (args.directory / 'candidate-summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    (args.directory / 'candidate-summary.md').write_text(markdown(summary))
    print(json.dumps([{'subscribers': scene['subscribers'], 'bytes': scene['bytes'],
                      'block_delta_us': [block['delta_us']['e2e'] for block in scene['blocks']]}
                     for scene in summary['scenarios']], ensure_ascii=False))


if __name__ == '__main__':
    main()
