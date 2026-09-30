#!/usr/bin/env python3
"""t51 量纲更正 —— 只读复算（⛔ 不改任何 run、不改任何原始读数）。

两口径（**恒不相等，差一个「注册项数」因子**）：
  每 tick 成本(µs/tick) = cpu_cores ÷ tick_rate × 1e6
  每注册项(ns/项·tick)  = 每 tick 成本(µs/tick) × 1000 ÷ 注册项数
                        = cpu_cores ÷ (注册项数 × tick_rate) × 1e9

注册项数定义（冻结三元组）：state2 → n（仅订阅）；state3 → 2n（订阅 + 发布）。

统计口径（⛔ 必须随数字一并给出，两者最小-最大可差 1.17×）：
  A = 先取 cpu 中位与 tick 中位，再算
  B = 逐 run 算，再取中位

用法: recompute.py [run_root]
"""
import csv, glob, json, os, statistics as st, sys

R = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260930-r47-W10-R6b'

def collect():
    out = []
    pats = sorted(glob.glob(R + '/*/windows.csv')) + sorted(glob.glob(R + '/bimod/*/windows.csv'))
    for p in pats:
        d = os.path.dirname(p)
        m = json.load(open(d + '/manifest.json'))
        r = list(csv.DictReader(open(p)))[0]
        out.append(dict(sub=os.path.relpath(d, R), state=str(m['state']),
                        n=int(m['topic_count']), cpu=float(r['cpu_cores']),
                        tick=float(r['tick_per_s'])))
    return out

rows = collect()

def blk(label, pred, mult):
    v = [x for x in rows if pred(x)]
    if not v:
        print(f"{label}: 无数据"); return None
    v = [x for x in v if x['tick'] > 0]
    if not v:
        print(f"{label}: 无 tick（如 state1）"); return None
    items = v[0]['n'] * mult
    At = st.median([x['cpu'] for x in v]) / st.median([x['tick'] for x in v]) * 1e6
    Bt = st.median([x['cpu'] / x['tick'] * 1e6 for x in v])
    Ai = At * 1000.0 / items
    Bi = st.median([x['cpu'] / x['tick'] * 1e9 / items for x in v])
    cpu = [x['cpu'] for x in v]
    print(f"{label} (k={len(v)}, 注册项数={items})")
    print(f"  cpu min/median/max = {min(cpu):.5f} / {st.median(cpu):.5f} / {max(cpu):.5f}"
          f"   超 0.20 = {sum(1 for c in cpu if c > 0.20)}/{len(cpu)}   max/0.20 = {max(cpu)/0.20:.2f}x")
    print(f"  每 tick 成本  A={At:.2f} µs   B={Bt:.2f} µs")
    print(f"  每注册项      A={Ai:.2f} ns   B={Bi:.2f} ns")
    return dict(At=At, Bt=Bt, Ai=Ai, Bi=Bi)

print("=== 量纲两口径复算（只读）===")
s2_10 = blk('state2 n=1000（项数=1000, A+B 共 10 run）',
            lambda x: x['state'] == '2' and x['n'] == 1000 and 'bimod' not in x['sub'], 1)
s2_22 = blk('state2 n=1000（项数=1000, 含 bimod 共 22 run）',
            lambda x: x['state'] == '2' and x['n'] == 1000, 1)
s3_10 = blk('state3 n=1000（项数=2000, 10 run）', lambda x: x['state'] == '3', 2)

print()
print("=== 比值（⛔ 必须注明分母）===")
if s2_10 and s3_10:
    print(f"  每 tick 成本口径：A={s3_10['At']/s2_10['At']:.2f}x   B={s3_10['Bt']/s2_10['Bt']:.2f}x   ← 报告 §3.3「3.8×」的真实分母")
    print(f"  每注册项口径    ：A={s3_10['Ai']/s2_10['Ai']:.2f}x   B={s3_10['Bi']/s2_10['Bi']:.2f}x   ← W00 R-3 采纳口径（记 1.93×）")
print()
print("=== 机械复核（两式恒不相等，差一个「注册项数」因子）===")
print("  state2: 11.85 × 1000 ÷ 1000 = 11.85  ⇒ 因子 1.00，碰巧相等（唯一盲区）")
print("  state3: 45.70 × 1000 ÷ 2000 = 22.85  ⇒ 因子 0.50，差 2.00×（差异暴露）")
print()
print("=== 误判量级 ===")
bad = 45.2e-9 * 2000 * 5903
print(f"  把 45.2 当 ns/项·tick 代入 k×项数×tick = {bad:.4f} core")
print(f"  实测                                  = 0.26977 core")
print(f"  ⇒ 高 {bad/0.26977:.2f}×（新判据会把 state3 正常读数误判为超标约 2 倍）")
print()
print("=== state3 逐 run 超标核对 ===")
badruns = [x for x in rows if x['state'] == '3']
for x in badruns:
    print(f"  {x['sub']:<20} cpu={x['cpu']:.5f} {'>0.20 ⚠️' if x['cpu'] > 0.20 else ''}")
c = [x['cpu'] for x in badruns]
print(f"  n={len(c)} min={min(c):.5f} median={st.median(c):.5f} max={max(c):.5f}"
      f"  超 0.20 = {sum(1 for v in c if v > 0.20)}/{len(c)}   max/0.20 = {max(c)/0.20:.2f}x")
