#!/usr/bin/env python3
"""复用正式矩阵原始样本，输出跨轮波动、逐秒趋势和可独立查看的图表。"""
import argparse
from collections import defaultdict
import csv
import gzip
import hashlib
import json
from pathlib import Path
import statistics

from benchmark import quantiles
from summarize_local_stages import split
from summarize_performance import content_errors


MODES = ('baseline', 'shared_v1')
SIZES = {64: '64B', 4096: '4KiB', 1048576: '1MiB'}


def read_csv(path):
    with gzip.open(path, 'rt') as stream:
        return list(csv.DictReader(stream))


def bounds(values):
    return {'min': min(values), 'max': max(values),
            'max_over_min': max(values) / min(values) if min(values) else None}


def context_switches(before, after):
    old = {x['tid']: x for x in before['thread_status']}
    new = {x['tid']: x for x in after['thread_status']}
    # 线程集合改变时不能把新线程的累计计数当作整个窗口增量。
    if old.keys() != new.keys():
        return None
    return {key: sum(int(new[tid][key]) - int(old[tid][key]) for tid in old)
            for key in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches')}


def analyze(directory):
    cases = json.loads((directory / 'cases.json').read_text())
    summary = json.loads((directory / 'summary.json').read_text())
    stages = {x['case']: x for x in json.loads((directory / 'local-stages.json').read_text())}
    expected = {(m, n, b, r) for m in MODES for n in (1, 8, 32) for b in SIZES for r in (1, 2, 3)}
    keys = [(c['mode'], c['subscribers'], c['bytes'], c['repeat']) for c in cases]
    if len(keys) != 54 or set(keys) != expected or summary['window_errors']:
        raise ValueError('只接受完整且交付检查通过的54窗口矩阵')
    windows = []
    for case in cases:
        folder = directory / case['case']
        rows = read_csv(folder / 'pub.csv.gz')
        published = {int(row['sequence']): row for row in rows}
        count = case['rate'] * case['seconds']
        if len(rows) != count or len(published) != count or any(row['success'] != '1' for row in rows):
            raise ValueError(f"{case['case']}: 发布记录缺失、重复或失败")
        origin = min(int(row['start_ns']) for row in rows)
        bucket = lambda row: (int(row['start_ns']) - origin) // 1_000_000_000
        pub_seconds, recv_seconds = defaultdict(list), defaultdict(list)
        for row in rows:
            pub_seconds[bucket(row)].append(int(row['elapsed_ns']))
        received, before_return, after_return = [], [], []
        files = sorted(folder.glob('sub*.csv.gz'))
        for path in files:
            recv = read_csv(path)
            sequences = [int(row['sequence']) for row in recv]
            if len(sequences) != count or set(sequences) != published.keys():
                raise ValueError(f'{path}: 逐订阅者序号集合不完整或重复')
            for row in recv:
                pub = published[int(row['sequence'])]
                elapsed, before, after = split(pub, row)
                received.append(elapsed)
                before_return.append(before)
                after_return.append(after)
                recv_seconds[bucket(pub)].append(elapsed)
        errors = content_errors(case, received, len(files))
        if errors:
            raise ValueError(f"{case['case']}: {errors}")
        end_to_end = quantiles(received)
        if end_to_end != stages[case['case']]['end_to_end']:
            raise ValueError('与独立分段汇总的全量分位数不一致')
        seconds = [{'second': second, 'publish': quantiles(pub_seconds[second]),
                    'end_to_end': quantiles(recv_seconds[second])} for second in sorted(pub_seconds)]
        windows.append({'case': case['case'], 'mode': case['mode'], 'subscribers': case['subscribers'],
            'bytes': case['bytes'], 'repeat': case['repeat'], 'start_ns': origin,
            'end_to_end': end_to_end, 'publish': quantiles([int(r['elapsed_ns']) for r in rows]),
            'before_publish_return': quantiles(before_return), 'after_publish_return': quantiles(after_return),
            'mean_end_to_end_us': statistics.mean(received) / 1000,
            'mean_before_publish_return_us': statistics.mean(before_return) / 1000,
            'mean_after_publish_return_us': statistics.mean(after_return) / 1000,
            'slowest_one_percent': stages[case['case']]['slowest_one_percent'],
            'publisher_context_switches': context_switches(case['before'][case['subscribers']], case['after'][case['subscribers']]),
            'receiver_p50_us': [r['p50_us'] for r in case['receivers']],
            'host_loadavg_after': case['host_loadavg_after'], 'seconds': seconds,
            'second_p50_range_us': bounds([s['end_to_end']['p50_us'] for s in seconds])})
    indexed = {(w['mode'], w['subscribers'], w['bytes'], w['repeat']): w for w in windows}
    paired, ranges = [], []
    for row in summary['rows']:
        n, size = row['subscribers'], row['payload_bytes']
        for mode in MODES:
            group = [indexed[(mode, n, size, r)] for r in (1, 2, 3)]
            ranges.append({'subscribers': n, 'bytes': size, 'mode': mode,
                **{key: bounds([w['end_to_end'][key] for w in group]) for key in ('p50_us', 'p99_us')}})
        for repeat in (1, 2, 3):
            a, b = [indexed[(mode, n, size, repeat)] for mode in MODES]
            metrics = {key: {'baseline': a['end_to_end'][key], 'current': b['end_to_end'][key],
                'delta_us': b['end_to_end'][key] - a['end_to_end'][key],
                'allowance_us': max(a['end_to_end'][key] * .1, 5)} for key in ('p50_us', 'p99_us')}
            passed = all(v['delta_us'] <= v['allowance_us'] for v in metrics.values())
            if passed != row['paired_round_gates'][repeat - 1]:
                raise ValueError('与正式汇总的逐轮门槛不一致')
            paired.append({'subscribers': n, 'bytes': size, 'repeat': repeat, 'passed': passed, 'metrics': metrics})
    return {'method': '最近秩分位数；每轮合并全部接收CSV；逐秒按发布起点分桶，末桶保留；三轮极差不代表置信区间。',
        'source_sha256': {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                          for name in ('cases.json', 'summary.json', 'local-stages.json', 'manifest.json')},
        'windows': windows, 'paired_rounds': paired, 'round_ranges': ranges}


def figures(result, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    font = Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
    if font.exists():
        font_manager.fontManager.addfont(str(font))
        plt.rcParams['font.family'] = font_manager.FontProperties(fname=str(font)).get_name()
    plt.rcParams.update({'axes.unicode_minus': False, 'font.size': 10, 'axes.spines.top': False,
                         'axes.spines.right': False, 'svg.fonttype': 'path'})
    colors = {'baseline': '#64748b', 'shared_v1': '#007f86'}
    labels = {'baseline': '基线', 'shared_v1': '候选'}
    def save(fig, name, ext):
        path = output / f'{name}.{ext}'
        fig.savefig(path, dpi=160)
        if ext == 'svg':
            # matplotlib路径数据默认带行末空格；保留换行分隔并通过仓库空白检查。
            path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines()) + '\n')
    windows = result['windows']
    fig, axes = plt.subplots(3, 3, figsize=(13, 9), layout='constrained')
    for ax, (n, size) in zip(axes.flat, ((n, b) for n in (1, 8, 32) for b in SIZES)):
        for mode in MODES:
            group = sorted((w for w in windows if (w['mode'], w['subscribers'], w['bytes']) == (mode, n, size)), key=lambda w: w['repeat'])
            ax.plot([1, 2, 3], [w['end_to_end']['p50_us'] for w in group], 'o-', color=colors[mode], label=labels[mode])
        ax.set(title=f'{n}订阅者 / {SIZES[size]}', xticks=[1, 2, 3], xlabel='轮次', ylabel='端到端 p50（µs）')
        ax.grid(alpha=.2)
    axes[0, 0].legend()
    fig.suptitle('同一实现也存在跨轮漂移：全部九场景、全部三轮', fontsize=16)
    for ext in ('png', 'svg'):
        save(fig, 'round-variation', ext)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(12, 4), layout='constrained')
    for ax, (size, repeat) in zip(axes, ((4096, 1), (64, 3))):
        for mode, offset in zip(MODES, (-.18, .18)):
            w = next(w for w in windows if (w['mode'], w['subscribers'], w['bytes'], w['repeat']) == (mode, 8, size, repeat))
            ax.bar([i + offset for i in range(8)], w['receiver_p50_us'], width=.36, color=colors[mode], label=labels[mode])
            ax.axhline(w['end_to_end']['p50_us'], color=colors[mode], linestyle='--', linewidth=1)
        ax.set(title=f'8订阅者 / {SIZES[size]} / 第{repeat}轮', xlabel='订阅进程编号', ylabel='各进程 p50（µs）', xticks=range(8))
        ax.grid(axis='y', alpha=.2)
        ax.legend()
    fig.suptitle('同一窗口内，订阅进程之间也有差异；虚线为全部接收样本的 p50', fontsize=14)
    for ext in ('png', 'svg'):
        save(fig, 'subscriber-spread', ext)
    plt.close(fig)

    pairs = result['paired_rounds']
    fig, axes = plt.subplots(2, 1, figsize=(14, 7), sharex=True, layout='constrained')
    for ax, key in zip(axes, ('p50_us', 'p99_us')):
        values = [p['metrics'][key]['delta_us'] / p['metrics'][key]['allowance_us'] for p in pairs]
        ax.bar(range(len(pairs)), values, color=['#c0392b' if v > 1 else '#007f86' for v in values])
        ax.axhline(1, color='#c0392b', linestyle='--', label='原验收上限 = 1')
        ax.axhline(0, color='#64748b', linewidth=.6)
        ax.set_ylabel(key.split('_')[0] + ' 差值 / 允许差值')
        ax.grid(axis='y', alpha=.2)
        ax.legend(loc='lower right')
    axes[-1].set_xticks(range(len(pairs)), [f"{p['subscribers']}/{SIZES[p['bytes']]}\n第{p['repeat']}轮" for p in pairs], rotation=60, ha='right', fontsize=8)
    fig.suptitle('逐轮门槛：红柱表示超标；负值表示候选更快', fontsize=16)
    for ext in ('png', 'svg'):
        save(fig, 'paired-gates', ext)
    plt.close(fig)

    for n, size in ((1, 4096), (1, 1048576), (8, 4096), (8, 1048576), (32, 4096)):
        fig, axes = plt.subplots(2, 3, figsize=(14, 7), sharex=True, sharey='row', layout='constrained')
        for repeat in (1, 2, 3):
            for mode in MODES:
                w = next(w for w in windows if (w['mode'], w['subscribers'], w['bytes'], w['repeat']) == (mode, n, size, repeat))
                for ax, metric in zip(axes[:, repeat - 1], ('end_to_end', 'publish')):
                    points = w['seconds']
                    ax.plot([s['second'] for s in points], [s[metric]['p50_us'] for s in points], color=colors[mode], label=labels[mode] + ' p50')
                    ax.plot([s['second'] for s in points], [s[metric]['p99_us'] for s in points], '--', color=colors[mode], alpha=.65, label=labels[mode] + ' p99')
                    ax.grid(alpha=.2)
                    ax.set_xlabel('各自窗口内秒数')
            axes[0, repeat - 1].set_title(f'第{repeat}轮')
        axes[0, 0].set_ylabel('端到端（µs）')
        axes[1, 0].set_ylabel('发布调用（µs）')
        axes[0, 0].legend(fontsize=8)
        fig.suptitle(f'{n}订阅者 / {SIZES[size]}：逐秒趋势（配对窗口先后运行，非同时采样）', fontsize=15)
        fig.supxlabel('每秒约100次发布；接收合并全部订阅者。同一消息的多接收样本有关联，逐秒 p99 仅作诊断。', fontsize=10)
        for ext in ('png', 'svg'):
            save(fig, f'timeline-sub{n}-bytes{size}', ext)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'variation.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    figures(result, args.output)
    print(json.dumps({'windows': len(result['windows']), 'failed_pairs': sum(not p['passed'] for p in result['paired_rounds']),
                      'output': str(args.output)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
