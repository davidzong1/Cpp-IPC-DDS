#!/usr/bin/env python3
"""按实际接收 TID 和 CLOCK_MONOTONIC 关联调度、空闲态与同序号样本。"""
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


def _open_text(path):
    """Open either the archived gzip stream or a live plain-text capture."""
    path = Path(path)
    if path.suffix == '.gz':
        return gzip.open(path, 'rt')
    return path.open('r')


def rows(path):
    with _open_text(path) as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def event_source(folder):
    """Return the decoded perf stream, accepting D05's uncompressed smoke files."""
    for base in (Path(folder), Path(folder).parent):
        for name in ('events.txt.gz', 'events.txt'):
            path = base / name
            if path.exists():
                return _open_text(path)
    return None


def stats(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), 'mean_us': sum(values)/len(values)/1000,
            **{f'p{q}_us': values[math.ceil(len(values)*q/100)-1]/1000 for q in (50, 99)}}


def analyze(folder):
    result = json.loads((folder/'result.json').read_text())
    pub_path = folder/'pub.csv.gz' if (folder/'pub.csv.gz').exists() else folder/'pub.csv'
    pub = {r['sequence']: r for r in rows(pub_path)}
    sample_paths = sorted(folder.glob('sub*.csv.gz')) or sorted(folder.glob('sub*.csv'))
    samples = [dict(r, subscriber=p.stem.split('.')[0]) for p in sample_paths for r in rows(p)]
    for row in samples:
        row['scheduler_tid'] = row.get('receiver_tid', row['reader_tid'])
        row['scheduler_wait_begin_ns'] = row.get('receiver_wait_begin_ns', row['wait_begin_ns'])
        row['scheduler_wait_end_ns'] = row.get('receiver_wait_end_ns', row['wait_end_ns'])
    tids = {r['reader_tid'] for r in samples}
    publisher_tid = min(t['tid'] for t in result['before'][result['subscribers']]['thread_status'])
    expected = result['seconds']*result['rate']
    assert len(pub) == expected and all(p['success'] == 1 for p in pub.values())
    assert len(samples) == expected*result['subscribers'] and len(tids) == result['subscribers']
    for tid in tids:
        selected = [r for r in samples if r['reader_tid'] == tid]
        assert len(selected) == expected and {r['sequence'] for r in selected} == set(pub)
    assert all(r['read_ns']-pub[r['sequence']]['start_ns'] == r['elapsed_ns'] >= 0 for r in samples)
    waking, wakeup, switched, idle, exits, migrations = (defaultdict(list) for _ in range(6))
    counts = Counter()
    lost_lines, unknown = [], []
    source = event_source(folder)
    # D05's raw perf smoke stores events beside the workload directory.  Keep
    # the output artifacts beside the CSVs while retaining the raw source.
    source_path = None
    for base in (Path(folder), Path(folder).parent):
        for name in ('events.txt.gz', 'events.txt'):
            candidate = base / name
            if candidate.exists():
                source_path = candidate
                break
        if source_path is not None:
            break
    if source is not None:
        with source:
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
                    waking[int(fields['pid'])].append((ns, int(fields['target_cpu']), tid, cpu))
                elif event == 'sched:sched_wakeup':
                    wakeup[int(fields['pid'])].append((ns, int(fields['target_cpu'])))
                elif event == 'sched:sched_switch':
                    switched[int(fields['next_pid'])].append((ns, cpu))
                elif event == 'sched:sched_migrate_task':
                    migrations[int(fields['pid'])].append((ns, int(fields['orig_cpu']), int(fields['dest_cpu'])))
    for data in (waking, wakeup, switched, idle, exits, migrations):
        for values in data.values(): values.sort()
    def in_interval(data, key, start, end):
        """Select events by target TID and interval without implicit nearest pairing."""
        values = data.get(key, ())
        i = bisect_left(values, (start,))
        j = bisect_right(values, (end, 2**64, 2**64, 2**64))
        return values[i:j]

    def chain_for(row, notify_ns, start, end):
        """Build one complete wake chain or return an explicit ambiguity/miss reason.

        A wake chain is valid only when the same receiver TID has one waking,
        one following wakeup, and one following sched_switch(next_pid).  We do
        not choose the first/nearest event when more than one complete chain is
        present; that would silently attach a kernel event to the wrong sample.
        """
        tid = row['scheduler_tid']
        chains = []
        for w in in_interval(waking, tid, max(notify_ns, start), end):
            # The first wakeup after a waking and the first switch to the
            # receiver after that wakeup are the kernel's one activation
            # chain.  Later switches belong to the receiver's work and must
            # not manufacture extra candidate chains for this sample.
            ups = in_interval(wakeup, tid, w[0], end)
            if not ups:
                continue
            up = ups[0]
            runs = in_interval(switched, tid, up[0], end)
            if runs:
                chains.append((w, up, runs[0]))
        if not chains:
            if not in_interval(waking, tid, max(notify_ns, start), end):
                return None, 'no_waking_in_interval'
            if not any(in_interval(wakeup, tid, w[0], end) for w in in_interval(waking, tid, max(notify_ns, start), end)):
                return None, 'no_wakeup_in_interval'
            return None, 'no_switch_in_interval'
        # A single wake can have more than one switch record only when the
        # target ran and was switched out/requeued in the same wait interval.
        # Such a sample is not safe for a one-shot activation decomposition.
        unique = {(w[0], up[0], run[0]) for w, up, run in chains}
        if len(unique) != 1:
            return None, 'ambiguous_wake_chain'
        return chains[0], ''
    matched, misses = [], Counter()
    joined = []
    for row in samples:
        p = pub[row['sequence']]
        entry = {k: row[k] for k in ('subscriber', 'reader_tid', 'sequence', 'elapsed_ns', 'assisted', 'wait_begin_ns', 'wait_end_ns')}
        entry.update(scheduler_tid=row['scheduler_tid'], scheduler_wait_begin_ns=row['scheduler_wait_begin_ns'],
                     scheduler_wait_end_ns=row['scheduler_wait_end_ns'])
        for key in ('receiver_tid', 'receiver_cpu', 'recv_return_ns', 'generation'):
            if key in row: entry[key] = row[key]
        entry.update(notify_ns=p['notify_begin_ns'], matched=0)
        end = row['scheduler_wait_end_ns']
        start = row['scheduler_wait_begin_ns']
        reason = '' if source_path is not None else 'perf_disabled'
        if not reason and (not end or start >= end): reason = 'no_wait_interval'
        if not reason and end < p['notify_begin_ns']: reason = 'wait_ended_before_notify'
        chain, chain_reason = (chain_for(row, p['notify_begin_ns'], start, end)
                               if not reason else (None, ''))
        if not reason and chain is None:
            reason = chain_reason
        w, up, run = chain if chain is not None else (None, None, None)
        if reason:
            misses[reason] += 1; entry['unmatched_reason'] = reason
        else:
            entry.update(matched=1, waking_ns=w[0], wakeup_ns=up[0], scheduled_ns=run[0],
                         waker_tid=w[2], waking_target_cpu=w[1], wakeup_target_cpu=up[1], scheduled_cpu=run[1],
                         waker_cpu=w[3], publisher_cpu=p.get('copy_end_cpu', -1),
                         publisher_wake=int(w[2] == publisher_tid),
                         notify_to_waking_ns=w[0]-p['notify_begin_ns'], waking_to_wakeup_ns=up[0]-w[0],
                         wakeup_to_scheduled_ns=run[0]-up[0], scheduled_to_wait_end_ns=end-run[0],
                         notify_to_wait_end_ns=end-p['notify_begin_ns'])
            if 'recv_return_ns' in row:
                entry['wait_end_to_recv_return_ns'] = row['recv_return_ns']-end
                entry['notify_to_recv_return_ns'] = row['recv_return_ns']-p['notify_begin_ns']
                entry['recv_return_to_api_ns'] = row['read_ns']-row['recv_return_ns']
            moves = migrations[row['scheduler_tid']]
            left = bisect_left(moves, (w[0],))
            right = bisect_right(moves, (row.get('recv_return_ns', end), 2**64, 2**64))
            entry['migration_count'] = right-left
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
    stages = ('elapsed_ns', 'notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns', 'notify_to_wait_end_ns', 'waking_to_idle_exit_ns', 'wait_end_to_recv_return_ns', 'notify_to_recv_return_ns', 'recv_return_to_api_ns')
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
    out['receive_roles'] = dict(Counter('getter' if r['scheduler_tid'] == r['reader_tid'] else 'background' for r in samples))
    out['receiver_tids'] = sorted({r['scheduler_tid'] for r in samples})
    per_sequence = defaultdict(list)
    for entry in matched: per_sequence[entry['sequence']].append(entry)
    for entries in per_sequence.values():
        for rank, entry in enumerate(sorted(entries, key=lambda r: (r['waking_ns'], r['scheduler_tid'])), 1):
            entry['waking_rank'] = rank
    complete_sequences = [entries for entries in per_sequence.values() if len(entries) == result['subscribers']]
    out['sequence_clusters'] = {
        'complete_count': len(complete_sequences),
        'waking_first_to_last': stats([max(r['waking_ns'] for r in entries)-min(r['waking_ns'] for r in entries) for entries in complete_sequences]),
        'max_elapsed': stats([max(r['elapsed_ns'] for r in entries) for entries in complete_sequences]),
    }
    start_ns = min(p['start_ns'] for p in pub.values())
    per_second = defaultdict(list)
    for row in samples: per_second[(pub[row['sequence']]['start_ns']-start_ns)//1000000000].append(row)
    out['per_second'] = {str(second): {'latency': stats([r['elapsed_ns'] for r in selected]),
                                     'stages': summarize([r for r in matched if (pub[r['sequence']]['start_ns']-start_ns)//1000000000 == second])}
                         for second, selected in sorted(per_second.items())}
    for name, key in (('publisher_cpu', 'publisher_cpu'), ('wakeup_cpu', 'wakeup_target_cpu'),
                      ('waking_rank', 'waking_rank'), ('migration_count', 'migration_count')):
        grouped = defaultdict(list)
        for entry in matched: grouped[entry[key]].append(entry)
        out['by_'+name] = {str(k): summarize(v) for k, v in grouped.items()}
    if source is not None:
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
