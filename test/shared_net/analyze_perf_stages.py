#!/usr/bin/env python3
"""按同一消息拆解端到端，并分析中位数附近样本和广播唤醒顺序。"""
import argparse
from collections import Counter, defaultdict
import csv
import gzip
import json
import math
from pathlib import Path

from summarize_perf_scheduler import rows, stats


STAGES = ('before_notify_ns', 'notify_to_wait_end_ns', 'wait_end_to_recv_ns',
          'recv_ns', 'process_ns', 'enqueue_ns', 'handoff_to_dequeue_ns', 'return_ns')


def partition(pub, sub):
    assert sub['sequence'] == pub['sequence']
    assert sub['read_ns']-pub['start_ns'] == sub['elapsed_ns']
    points = [pub['start_ns'], pub['notify_begin_ns'], sub['wait_end_ns'],
              sub['recv_begin_ns'], sub['recv_return_ns'], sub['enqueue_before_ns'],
              sub['enqueue_after_ns'], sub['dequeue_after_ns'], sub['read_ns']]
    if not all(points):
        return None, 'missing_timestamp'
    if points != sorted(points):
        return None, 'nonmonotonic_or_no_wait'
    stages = dict(zip(STAGES, (b-a for a, b in zip(points, points[1:]))))
    assert sum(stages.values()) == sub['elapsed_ns']
    return stages, None


def cohorts(samples):
    ordered = sorted(samples, key=lambda r: (r['elapsed_ns'], r['reader_tid'], r['sequence']))
    count = len(ordered)
    return {'all': ordered, 'median_band_p45_p55': ordered[count*45//100:(count*55+99)//100],
            'slowest_one_percent': ordered[-((count+99)//100):]}


def summarize(selected):
    return {key: stats([r[key] for r in selected if key in r]) for key in
            ('elapsed_ns', *STAGES, 'notify_to_waking_ns', 'waking_to_wakeup_ns',
             'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns', 'copy_ns', 'loan_ns',
             'before_loan_ns', 'commit_to_notify_ns', 'get_cpu_ns', 'wait_cpu_ns',
             'recv_cpu_ns', 'process_cpu_ns', 'assist_acquire_ns', 'handoff_ns')}


def analyze(folder):
    pub_rows = rows(folder/'pub.csv.gz')
    assert len({r['sequence'] for r in pub_rows}) == len(pub_rows)
    pubs = {r['sequence']: r for r in pub_rows}
    samples = [r for p in sorted(folder.glob('sub*.csv.gz')) for r in rows(p)]
    assert len({(r['reader_tid'], r['sequence']) for r in samples}) == len(samples)
    joined = {}
    if (folder/'joined.csv.gz').exists():
        with gzip.open(folder/'joined.csv.gz', 'rt') as source:
            for r in csv.DictReader(source):
                key = (int(r['reader_tid']), int(r['sequence']))
                assert key not in joined
                joined[key] = r
    excluded = Counter()
    for r in samples:
        p = pubs[r['sequence']]
        stages, reason = partition(p, r)
        if reason:
            excluded[reason] += 1
        else:
            r.update(stages)
        r.update(copy_ns=p['copy_end_ns']-p['copy_begin_ns'], loan_ns=p['loan_end_ns']-p['loan_begin_ns'],
                 before_loan_ns=p['loan_begin_ns']-p['start_ns'], commit_to_notify_ns=p['notify_begin_ns']-p['commit_begin_ns'],
                 publisher_cpu=p['copy_begin_cpu'])
        j = joined.get((r['reader_tid'], r['sequence']))
        if j and int(j['matched']):
            for key in ('notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns'):
                r[key] = int(j[key])
    groups = cohorts(samples)
    by_cpu, by_second = defaultdict(list), defaultdict(list)
    first = min(r['start_ns'] for r in pub_rows)
    for r in samples:
        by_cpu[str(r['publisher_cpu'])].append(r)
        by_second[str((pubs[r['sequence']]['start_ns']-first)//1000000000)].append(r)
    broadcasts = defaultdict(list)
    for j in joined.values():
        if int(j['matched']):
            broadcasts[int(j['sequence'])].append(j)
    result = json.loads((folder/'result.json').read_text())
    rank_groups, migration_groups, receiver_cpu_groups = (defaultdict(list) for _ in range(3))
    sample_index = {(r['reader_tid'], r['sequence']): r for r in samples}
    complete_broadcasts = 0
    for sequence, values in broadcasts.items():
        if len(values) != result['subscribers']:
            continue
        complete_broadcasts += 1
        for rank, j in enumerate(sorted(values, key=lambda r: (int(r['waking_ns']), int(r['reader_tid']))), 1):
            r = sample_index[int(j['reader_tid']), sequence]
            rank_groups[str(rank)].append(r)
            migrated = j['waking_target_cpu'] != j['wakeup_target_cpu']
            migration_groups['migrated' if migrated else 'same_target'].append(r)
            receiver_cpu_groups[j['wakeup_target_cpu']].append(r)
    out = {'case': folder.name, 'samples': len(samples), 'partition_excluded': dict(excluded),
           'cohort_rule': '按端到端排序，p45到p55秩区间；各阶段为同组统计，不能相加独立分位数。CPU计时区间可能包含发布前等待的执行，不参与墙钟阶段求和。',
           'cohorts': {k: {'samples': len(v), 'publications': len({r['sequence'] for r in v}), 'stages': summarize(v)} for k, v in groups.items()},
           'publisher_cpu': {k: summarize(v) for k, v in by_cpu.items()},
           'seconds': {k: summarize(v) for k, v in by_second.items()},
           'complete_broadcasts': complete_broadcasts,
           'waking_rank': {k: summarize(v) for k, v in rank_groups.items()},
           'migration': {k: summarize(v) for k, v in migration_groups.items()},
           'wakeup_target_cpu': {k: summarize(v) for k, v in receiver_cpu_groups.items()}}
    (folder/'stage-analysis.json').write_text(json.dumps(out, ensure_ascii=False, indent=2)+'\n')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    folders = [args.directory] if (args.directory/'result.json').exists() else sorted(p.parent for p in args.directory.glob('*/result.json'))
    if not folders:
        raise ValueError('找不到完整窗口')
    results = [analyze(p) for p in folders]
    if len(results) > 1:
        (args.directory/'stage-analysis.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps([{'case': r['case'], 'samples': r['samples'], 'partition_excluded': r['partition_excluded'],
                       'median_band': r['cohorts']['median_band_p45_p55']['stages']} for r in results], ensure_ascii=False))


if __name__ == '__main__':
    main()
