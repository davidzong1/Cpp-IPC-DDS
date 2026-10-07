#!/usr/bin/env python3
"""将内核栈按事件时刻和目标 reader TID 关联至已核验的同消息调度链。"""
import argparse
from collections import Counter, defaultdict
import csv
import gzip
import json
from pathlib import Path
import re

from analyze_perf_stages import cohorts
from summarize_perf_scheduler import FIELD, LINE, stats

FRAME = re.compile(r'^\s+[0-9a-f]+\s+(.+)$')
EVENTS = {'sched:sched_waking', 'sched:sched_wakeup'}


def parse_stacks(source, wanted):
    stacks, unknown, lost = {}, [], []
    current = None
    for line in source:
        if not line.strip():
            continue
        if 'LOST' in line.upper():
            lost.append(line.strip())
            current = None
            continue
        match = LINE.match(line)
        if match:
            tid, cpu, seconds, fraction, event, payload = match.groups()
            current = None
            if event in EVENTS:
                fields = dict(FIELD.findall(payload))
                ns = int(seconds)*1000000000+int(fraction.ljust(9, '0'))
                key = (event, int(fields['pid']), ns)
                if key in wanted:
                    assert key not in stacks, key
                    stacks[key] = []
                    current = key
            continue
        frame = FRAME.match(line)
        if frame:
            if current is not None:
                stacks[current].append(frame.group(1))
        else:
            unknown.append(line.strip())
    return stacks, unknown, lost


def family(frames, event):
    if not frames:
        return 'missing_stack'
    if event == 'sched:sched_waking':
        return 'futex_wake' if 'futex_wake' in frames else 'other_waker'
    if 'sched_ttwu_pending' in frames:
        return 'remote_pending'
    if 'try_to_wake_up' in frames:
        return 'direct_activation'
    return 'other_activation'


def analyze(folder):
    with gzip.open(folder/'joined.csv.gz', 'rt') as f:
        joined = [{k: (int(v) if v and k != 'subscriber' and k != 'unmatched_reason' else v) for k, v in row.items()}
                  for row in csv.DictReader(f)]
    matched = [r for r in joined if r['matched']]
    wanted = {(event, r['reader_tid'], r[field]) for r in matched
              for event, field in (('sched:sched_waking', 'waking_ns'), ('sched:sched_wakeup', 'wakeup_ns'))}
    assert len(wanted) == len(matched)*2
    with gzip.open(folder/'stacks.txt.gz', 'rt') as source:
        stacks, unknown, lost = parse_stacks(source, wanted)
    missing = wanted-set(stacks)
    paths = {event: Counter() for event in EVENTS}
    for r in matched:
        for event, field, label in (('sched:sched_waking', 'waking_ns', 'waking_path'), ('sched:sched_wakeup', 'wakeup_ns', 'wakeup_path')):
            frames = stacks.get((event, r['reader_tid'], r[field]), [])
            r[label] = family(frames, event)
            paths[event][tuple(frames)] += 1
    def summarize(selected):
        stages = ('elapsed_ns', 'notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns')
        groups = defaultdict(list)
        for r in selected:
            groups[r['wakeup_path']].append(r)
        return {'count': len(selected), 'waking_paths': dict(Counter(r['waking_path'] for r in selected)),
                'wakeup_paths': {name: {key: stats([r[key] for r in group]) for key in stages} for name, group in groups.items()}}
    selected = cohorts(joined)
    out = {'case': folder.name, 'samples': len(joined), 'matched_scheduler_samples': len(matched),
           'expected_stack_events': len(wanted), 'matched_stack_events': len(stacks), 'missing_stack_events': len(missing),
           'empty_stack_events': sum(not v for v in stacks.values()), 'unknown_lines': unknown[:20], 'unknown_line_count': len(unknown), 'lost_lines': lost,
           'cohorts': {name: summarize([r for r in group if r['matched']]) for name, group in selected.items()},
           'unique_stacks': {event: [{'count': count, 'frames': list(frames)} for frames, count in counts.most_common()]
                             for event, counts in paths.items()}}
    fields = ['reader_tid', 'sequence', 'elapsed_ns', 'waking_ns', 'wakeup_ns', 'waking_path', 'wakeup_path']
    with gzip.open(folder/'stack-links.csv.gz', 'wt', newline='') as f:
        writer = csv.DictWriter(f, fields, extrasaction='ignore'); writer.writeheader(); writer.writerows(matched)
    (folder/'stack-summary.json').write_text(json.dumps(out, ensure_ascii=False, indent=2)+'\n')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path); args = parser.parse_args()
    folders = [args.directory] if (args.directory/'result.json').exists() else sorted(p.parent for p in args.directory.glob('*/result.json'))
    if not folders: raise ValueError('找不到完整窗口')
    out = [analyze(p) for p in folders]
    if len(out) > 1: (args.directory/'stack-summary.json').write_text(json.dumps(out, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps([{k: r[k] for k in ('case', 'samples', 'matched_stack_events', 'missing_stack_events', 'empty_stack_events', 'unknown_line_count', 'lost_lines', 'cohorts')} for r in out], ensure_ascii=False))


if __name__ == '__main__': main()
