#!/usr/bin/env python3
"""R1 分部复核 · W02 计时边界独立复算（t25 / 架构负责人）
用法: python3 verify_w02_timing_boundary.py <run_dir>   默认 artifacts/perf/20260928-r20-W02
判据: transport == transport_done - publish_enter
      delivery  == app_obtained  - transport_done
      app_read  == fully_consumed - app_obtained
      e2e       == fully_consumed - produced
跳过口径: 任一时间戳或派生列为空 ⇒ 整行跳过并分类计数（不静默、不按 0 计）
"""
import csv, glob, os, sys

run = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260928-r20-W02'
D = os.path.join(run, 'samples')
COLS = ['produced_ns', 'publish_enter_ns', 'transport_done_ns',
        'app_obtained_ns', 'fully_consumed_ns',
        'transport_ns', 'delivery_ns', 'app_read_ns', 'e2e_ns']

def isnull(v):
    return v in ('', None, 'null')

files = sorted(glob.glob(os.path.join(D, '*.samples.csv')))
tot = mism = rows = skipped = 0
empties = {}
for f in files:
    for r in csv.DictReader(open(f)):
        rows += 1
        blank = tuple(k for k in COLS if isnull(r.get(k, '')))
        if blank:
            skipped += 1
            empties[blank] = empties.get(blank, 0) + 1
            continue
        pr, pe, td, ao, fc = (int(r[k]) for k in COLS[:5])
        t, d, a, e = (int(r[k]) for k in COLS[5:])
        tot += 4
        if t != td - pe: mism += 1; print('transport mismatch', os.path.basename(f), r['seq'])
        if d != ao - td: mism += 1; print('delivery  mismatch', os.path.basename(f), r['seq'])
        if a != fc - ao: mism += 1; print('app_read  mismatch', os.path.basename(f), r['seq'])
        if e != fc - pr: mism += 1; print('e2e       mismatch', os.path.basename(f), r['seq'])

print(f'files={len(files)} rows={rows} cells={tot} mismatches={mism} skipped_rows={skipped}')
print('skip 的空值组合:', empties)
