#!/usr/bin/env python3
"""W11 (t68/F5) —— **归一化形态恒等式的可复算校验**（只读）。

为什么要单独一个脚本：W11 报告 §2.2 曾写「7 格全部恒等，最大偏差 1.23e-14%」，
而该数字在脚本与证据文件中 **0 命中**（t66 的 F5）⇒ 属「无实物来源的数值」。
本脚本把该式做成**可从 run 目录逐 run 复算**的判据：

  恒等式：cpu ≈ k × 注册项数 × tick_率  ÷ 1e9
    k = cpu ÷ tick_率 × 1e9 ÷ 注册项数      （ns / 项·tick）
    注册项数（冻结定义）：state2 → n；state3 → **2n**

对**每个 run** 分别计算
  pred  = k_i × entries_i × tick_i / 1e9
  dev%  = |pred − cpu_i| / cpu_i × 100
并输出**覆盖 run 数**、**最大偏差**、**中位偏差**、以及分布（供报告引用）。

⚠️ 覆盖范围：只统计 `tick_per_s > 0` 且 `cpu_cores > 0` 的 run
（state1 无控制项 ⇒ tick=0 ⇒ 该式对它是 0/0，**不参与**；报告必须写明这一点）。

用法: w11_identity_check.py <cpu_run_root> [--json <out.json>]
"""
import csv, glob, json, os, statistics as st, sys

root = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260930-r49-W11'
out_json = None
if '--json' in sys.argv:
    out_json = sys.argv[sys.argv.index('--json') + 1]

rows = []
for p in sorted(glob.glob(root + '/*/*/windows.csv')):
    d = os.path.dirname(p)
    mf = os.path.join(d, 'manifest.json')
    if not os.path.exists(mf):
        continue
    m = json.load(open(mf))
    r = list(csv.DictReader(open(p)))[0]
    state = int(m['state'])
    n = int(m['topic_count'])
    entries = 2 * n if state == 3 else n
    tick = float(r['tick_per_s'] or 0.0)
    cpu = float(r['cpu_cores'])
    rows.append(dict(sub=os.path.relpath(d, root), state=state, n=n, entries=entries,
                     tick=tick, cpu=cpu))

checked, skipped, devs = [], [], []
for r in rows:
    if r['tick'] <= 0 or r['cpu'] <= 0:
        skipped.append(r['sub'])          # state1（无控制项）⇒ 0/0，不适用
        continue
    k = r['cpu'] / r['tick'] * 1e9 / r['entries']
    pred = k * r['entries'] * r['tick'] / 1e9
    dev = abs(pred - r['cpu']) / r['cpu'] * 100.0
    devs.append(dev)
    checked.append(dict(sub=r['sub'], state=r['state'], n=r['n'], entries=r['entries'],
                        tick=r['tick'], cpu=r['cpu'], k_ns_per_entry_tick=k,
                        pred=pred, dev_pct=dev))

print(f"run 总数            = {len(rows)}")
print(f"参与恒等式校验的 run = {len(checked)}（tick>0 且 cpu>0）")
print(f"不参与的 run         = {len(skipped)}（tick=0，state1 无控制项 ⇒ 0/0）")
if devs:
    print(f"最大偏差            = {max(devs):.6e} %   ← 报告应引用此值（附覆盖 run 数）")
    print(f"中位偏差            = {st.median(devs):.6e} %")
    print(f"零偏差 run 数        = {sum(1 for d in devs if d == 0.0)} / {len(devs)}")
    print(f"偏差 >1e-13% 的 run 数 = {sum(1 for d in devs if d > 1e-13)}")
print()
if len(sys.argv) > 2 and sys.argv[2] != '--json':
    print("（第二个位置参数被忽略；如需 JSON 请用 --json <path>）")

# ── 口径 A：先取每格（state,n,W）的中位再代恒等式（= 报告 §2.2「7 格」的口径）──
# ⚠️ 与报告 §2.2 的表格口径**严格对齐**：只取 diag=off 且排除 recheck 复核轮
#    （报告 §2.2 的 8 行表正是这个口径）。脚本同时把该口径的 run 数列出，便于复核。
grids, caliberA_runs = {}, []
for r in rows:
    if r['tick'] <= 0 or r['cpu'] <= 0:
        continue
    mf = json.load(open(os.path.join(root, r['sub'], 'manifest.json')))
    if mf.get('diagnostics_enabled'):
        continue
    if 'recheck' in r['sub']:
        continue
    caliberA_runs.append(r['sub'])
    # ⛔ 键必须含 W(workers)：报告 §2.2 的表把 state3/n=1000 的 W=4 与 W=32 **分列两行**
    #    （两者注册项数相同但架构不同）⇒ 合并会把两行混成一格（第一版脚本踩到，已修）。
    grids.setdefault((r['state'], r['n'], r['entries'], mf.get('effective_workers')), []).append(r)
gdevs, gdetail = [], []
for key in sorted(grids):
    v = grids[key]
    entries = key[2]
    cpu_m = st.median([x['cpu'] for x in v])
    tick_m = st.median([x['tick'] for x in v])
    k = cpu_m / tick_m * 1e9 / entries
    pred = k * entries * tick_m / 1e9
    dev = abs(pred - cpu_m) / cpu_m * 100.0
    gdevs.append(dev)
    gdetail.append(dict(state=key[0], n=key[1], entries=entries, workers=key[3], runs=len(v),
                        k_median_cpu_tick=k,
                        cpu_median=cpu_m, tick_median=tick_m, pred=pred, dev_pct=dev))
print()
print(f"口径 A（diag=off 且排除 recheck，先取格中位再代式，= 报告 §2.2 的表）："
      f"run 数 = {len(caliberA_runs)}、格数 = {len(gdevs)}"
      + (f"，**最大偏差 = {max(gdevs):.6e} %**" if gdevs else ""))

if out_json:
    json.dump(dict(root=root, total_runs=len(rows), checked_runs=len(checked),
                   skipped_runs=skipped, max_dev_pct=max(devs) if devs else None,
                   median_dev_pct=st.median(devs) if devs else None,
                   zero_dev_runs=sum(1 for d in devs if d == 0.0),
                   caliber_B_per_run=checked,
                   caliber_A_grids=len(gdevs),
                   caliber_A_max_dev_pct=max(gdevs) if gdevs else None,
                   caliber_A_detail=gdetail), open(out_json, 'w'), ensure_ascii=False, indent=2)
    print(f"JSON 已写 {out_json}")
