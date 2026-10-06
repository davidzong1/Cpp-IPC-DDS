#!/usr/bin/env python3
"""按 reader TID 和 CLOCK_MONOTONIC 关联调度、空闲态与同序号样本。"""
import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import csv
import gzip
import json
import math
from pathlib import Path
import re

LINE = re.compile(r'^\s*(-?\d+)\s+\[(\d+)\]\s+(\d+)\.(\d+):\s+(\S+):\s+(.*)$')
FIELD = re.compile(r'\b(\w+)=([^ ]+)')


def rows(path):
    with gzip.open(path, 'rt') as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def stats(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), 'mean_us': sum(values)/len(values)/1000,
            **{f'p{q}_us': values[math.ceil(len(values)*q/100)-1]/1000 for q in (50, 99)}}


def analyze(folder):
    result = json.loads((folder/'result.json').read_text())
    pub = {r['sequence']: r for r in rows(folder/'pub.csv.gz')}
    samples = [dict(r, subscriber=p.stem.split('.')[0]) for p in sorted(folder.glob('sub*.csv.gz')) for r in rows(p)]
    tids = {r['reader_tid'] for r in samples}
    publisher_tid = min(t['tid'] for t in result['before'][result['subscribers']]['thread_status'])
    expected = result['seconds']*result['rate']
    assert len(pub) == expected and all(p['success'] == 1 for p in pub.values())
    assert len(samples) == expected*result['subscribers'] and len(tids) == result['subscribers']
    for tid in tids:
        selected = [r for r in samples if r['reader_tid'] == tid]
        assert len(selected) == expected and {r['sequence'] for r in selected} == set(pub)
    assert all(r['read_ns']-pub[r['sequence']]['start_ns'] == r['elapsed_ns'] >= 0 for r in samples)
    waking, wakeup, switched, idle, exits = (defaultdict(list) for _ in range(5))
    counts = Counter()
    lost_lines, unknown = [], []
    if (folder/'events.txt.gz').exists():
        with gzip.open(folder/'events.txt.gz', 'rt') as source:
            for line in source:
                if 'LOST' in line.upper():
                    lost_lines.append(line.strip())
                match = LINE.match(line)
                if not match:
                    if line.strip(): unknown.append(line.strip())
                    continue
                tid, cpu, seconds, fraction, event, payload = match.groups()
                ns = int(seconds)*1000000000+int(fraction.ljust(9, '0'))
                cpu, tid = int(cpu), int(tid)
                fields = dict(FIELD.findall(payload))
                counts[event] += 1
                if event == 'power:cpu_idle':
                    state = int(fields['state'])
                    idle[int(fields['cpu_id'])].append((ns, state))
                    if state >= 2**31: exits[int(fields['cpu_id'])].append(ns)
                elif event == 'sched:sched_waking':
                    waking[int(fields['pid'])].append((ns, int(fields['target_cpu']), tid))
                elif event == 'sched:sched_wakeup':
                    wakeup[int(fields['pid'])].append((ns, int(fields['target_cpu'])))
                elif event == 'sched:sched_switch':
                    switched[int(fields['next_pid'])].append((ns, cpu))
    for data in (waking, wakeup, switched, idle, exits):
        for values in data.values(): values.sort()
    def first(data, key, start, end):
        values = data[key]
        i = bisect_left(values, (start,))
        return values[i] if i < len(values) and values[i][0] <= end else None
    matched, misses = [], Counter()
    joined = []
    for row in samples:
        p = pub[row['sequence']]
        entry = {k: row[k] for k in ('subscriber', 'reader_tid', 'sequence', 'elapsed_ns', 'assisted', 'wait_begin_ns', 'wait_end_ns')}
        entry.update(notify_ns=p['notify_begin_ns'], matched=0)
        end = row['wait_end_ns']
        reason = '' if (folder/'events.txt.gz').exists() else 'perf_disabled'
        if not reason and (not end or row['wait_begin_ns'] >= end): reason = 'no_wait_interval'
        w = first(waking, row['reader_tid'], max(p['notify_begin_ns'], row['wait_begin_ns']), end) if not reason else None
        if not reason and not w: reason = 'no_waking_in_interval'
        up = first(wakeup, row['reader_tid'], w[0], end) if not reason else None
        if not reason and not up: reason = 'no_wakeup_in_interval'
        run = first(switched, row['reader_tid'], up[0], end) if not reason else None
        if not reason and not run: reason = 'no_switch_in_interval'
        if reason:
            misses[reason] += 1; entry['unmatched_reason'] = reason
        else:
            entry.update(matched=1, waking_ns=w[0], wakeup_ns=up[0], scheduled_ns=run[0],
                         waker_tid=w[2], waking_target_cpu=w[1], wakeup_target_cpu=up[1], scheduled_cpu=run[1],
                         publisher_wake=int(w[2] == publisher_tid),
                         notify_to_waking_ns=w[0]-p['notify_begin_ns'], waking_to_wakeup_ns=up[0]-w[0],
                         wakeup_to_scheduled_ns=run[0]-up[0], scheduled_to_wait_end_ns=end-run[0],
                         notify_to_wait_end_ns=end-p['notify_begin_ns'])
            # 使用最终唤醒目标 CPU；waking 中的 target_cpu 是当时位置，后续可能迁移。
            states = idle[up[1]]
            i = bisect_right(states, (w[0], 2**64))-1
            state = states[i][1] if i >= 0 else None
            entry['idle_at_waking'] = '' if state is None else (-1 if state >= 2**31 else state)
            entry['idle_state_since_ns'] = states[i][0] if i >= 0 else ''
            if state is not None and state < 2**31:
                j = bisect_left(exits[up[1]], w[0])
                if j < len(exits[up[1]]) and exits[up[1]][j] <= run[0]:
                    entry['idle_exit_ns'] = exits[up[1]][j]
                    entry['waking_to_idle_exit_ns'] = exits[up[1]][j]-w[0]
            matched.append(entry)
        joined.append(entry)
    slow_keys = {(r['reader_tid'], r['sequence']) for r in sorted(samples, key=lambda r: r['elapsed_ns'], reverse=True)[:math.ceil(len(samples)/100)]}
    stages = ('elapsed_ns', 'notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns', 'notify_to_wait_end_ns', 'waking_to_idle_exit_ns')
    def summarize(selected): return {k: stats([r[k] for r in selected if k in r]) for k in stages}
    groups = defaultdict(list)
    for entry in matched: groups[entry['idle_at_waking']].append(entry)
    out = {'case': folder.name, 'samples': len(samples), 'publisher_tid': publisher_tid,
           'latency': stats([r['elapsed_ns'] for r in samples]), 'publish': stats([r['elapsed_ns'] for r in pub.values()]),
           'copy_wall': stats([r['copy_end_ns']-r['copy_begin_ns'] for r in pub.values()]),
           'copy_cpu': stats([r['copy_cpu_end_ns']-r['copy_cpu_begin_ns'] for r in pub.values()]),
           'copy_cpus': dict(Counter(f"{r['copy_begin_cpu']}->{r['copy_end_cpu']}" for r in pub.values())),
           'matched': len(matched), 'unmatched': dict(misses), 'event_counts': dict(counts),
           'lost_event_lines': lost_lines, 'unknown_event_lines': unknown[:20], 'unknown_event_count': len(unknown),
           'all_matched': summarize(matched), 'slowest_one_percent_matched': summarize([r for r in matched if (r['reader_tid'], r['sequence']) in slow_keys]),
           'by_idle_at_waking': {str(k): summarize(v) for k, v in groups.items()},
           'publisher_wake_count': sum(r['publisher_wake'] for r in matched), 'affinity': result['affinity_plan']}
    if (folder/'events.txt.gz').exists():
        fields = sorted({k for entry in joined for k in entry})
        with gzip.open(folder/'joined.csv.gz', 'wt', newline='') as f:
            writer = csv.DictWriter(f, fields); writer.writeheader(); writer.writerows(joined)
    (folder/'scheduler-summary.json').write_text(json.dumps(out, ensure_ascii=False, indent=2)+'\n')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path); args = parser.parse_args()
    folders = [args.directory] if (args.directory/'result.json').exists() else sorted(p.parent for p in args.directory.glob('*/result.json'))
    if not folders: raise ValueError('找不到完整采样窗口')
    results = [analyze(p) for p in folders]
    (args.directory/'scheduler-summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n') if len(results) > 1 else None
    print(json.dumps([{k: r[k] for k in ('case', 'samples', 'latency', 'matched', 'unmatched', 'lost_event_lines', 'unknown_event_count')} for r in results], ensure_ascii=False))


if __name__ == '__main__': main()
