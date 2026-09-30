#!/usr/bin/env python3
"""R1 分部复核 · W02 D-17 计数器恒等式独立复算（t25 / 架构负责人）
用法: python3 verify_w02_counter_identity.py <run_dir>
判据: family := tlv_messages + dzflat_a_messages + dzflat_b_messages
      family == pub_dzflat + pub_fallback，残差 ∈ [-8, 8]
"""
import sys, re

run = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260928-r20-W02'
rows = []
for l in open(run + '/counter_identity_check.txt'):
    l = l.strip()
    if not l.startswith('r') or l.count(',') < 9:
        continue
    f = l.split(',')
    if not f[2].strip().isdigit():
        continue
    rows.append(tuple(int(x) for x in (f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9])))
# sent_ok, dz, fb, tlv, a, b, family, diff
bad = [r for r in rows if not (-8 <= r[7] <= 8)]
bad2 = [r for r in rows if r[6] != r[3] + r[4] + r[5]]
print(f'cases={len(rows)} |diff|<=8: {len(rows)-len(bad)}/{len(rows)} '
      f'max|diff|={max(abs(r[7]) for r in rows)} '
      f'max|family-(dz+fb)|={max(abs(r[6]-(r[1]+r[2])) for r in rows)} '
      f'family!=tlv+a+b: {len(bad2)}')
