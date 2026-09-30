#!/usr/bin/env python3
"""W11 (t12) —— 串行配对实验聚合（只读）。

从 `w11_cpu_scan_sweep.sh` 的 run 目录逐 run 重算，输出：

  A. 控制面 CPU —— **tick 率归一化形态 + 冻结三元组**（W00 §11.3 R-3）
       每 tick 成本(µs/tick) = cpu_cores ÷ tick_per_s × 1e6
       每注册项(ns/项·tick) = 每 tick 成本 × 1000 ÷ 注册项数
       注册项数（冻结定义）：state2 → n；state3 → **2n**
       ⇒ ⛔ 两口径**不得互换**，输出必须同时给口径名。
  B. 扫描成本（§10.2 五项派生值 + 常驻孪生 + t44 结束值四量 —— **与 A 分列**）
  C. 离散度（中位 / min / max / 极差比；逐 run 列出，⛔ 不挑最优轮）

用法: w11_aggregate.py <run_root> [out_dir]
"""
import csv, glob, json, os, statistics as st, sys

R = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260930-r49-W11'
OUT = sys.argv[2] if len(sys.argv) > 2 else R
os.makedirs(OUT, exist_ok=True)


def load_runs():
    out = []
    for d in sorted(glob.glob(R + '/*/*/')):
        wc = os.path.join(d, 'windows.csv')
        mf = os.path.join(d, 'manifest.json')
        if not os.path.exists(wc) or not os.path.exists(mf):
            continue
        m = json.load(open(mf))
        r = list(csv.DictReader(open(wc)))[0]
        sub = os.path.relpath(d.rstrip('/'), R)
        # 复核补充轮单独归类（同配置，但用途是"异常轮复跑"⇒ ⛔ 不并入每配置 k 轮统计）
        if 'recheck' in sub:
            out.append(dict(sub=sub, recheck=True, state=int(m['state']), n=int(m['topic_count']),
                            workers=int(m['effective_workers']), diag=m['diagnostics_enabled'],
                            cpu=float(r['cpu_cores']), tick=float(r['tick_per_s'])))
            continue
        n = int(m['topic_count'])
        st_ = int(m['state'])
        entries = 2 * n if st_ == 3 else n
        out.append(dict(sub=sub, state=st_, n=n, entries=entries,
                        workers=int(m['effective_workers']), diag=m['diagnostics_enabled'],
                        cpu=float(r['cpu_cores']), tick=float(r['tick_per_s']),
                        tmax=float(r['sched_tick_max_us']) if r.get('sched_tick_max_us') else 0.0,
                        threads=int(r['threads']), wall=float(r['wall_s']),
                        lib_sha=m.get('lib_sha256'), tool_sha=m.get('tool_sha256'),
                        d_rounds=r.get('d_scan_rounds'), d_scan=r.get('d_scanned_routes_total'),
                        d_ready=r.get('d_ready_rounds'), d_wait=r.get('d_wait_timeouts'),
                        avg_entry=r.get('avg_scanned_per_round'), ns_round=r.get('avg_ns_per_round'),
                        ns_entry=r.get('avg_ns_per_entry'), ready_ratio=r.get('ready_round_ratio'),
                        wto_per_s=r.get('wait_timeouts_per_s'),
                        res_wto_per_s=r.get('resident_wait_timeouts_per_s'),
                        consistent=r.get('resident_gated_consistent'),
                        gates=r.get('gates_passed'),
                        d_g_rounds=r.get('d_g_scan_rounds'), d_g_scan=r.get('d_g_scanned_routes_total'),
                        d_g_time=r.get('d_g_scan_time_ns')))
    return out


rows = load_runs()


def num(x):
    if x in (None, '', 'null', 'None'):
        return None
    try:
        return float(x)
    except ValueError:
        return None


def per_tick_us(cpu, tick):
    return cpu / tick * 1e6 if tick and tick > 0 else None


def per_entry_ns(cpu, tick, entries):
    return (cpu / tick * 1e9 / entries) if tick and tick > 0 and entries else None


def dump_cpu():
    lines = ['# A 组：控制面 CPU（tick 率归一化 + 冻结三元组；⛔ 两口径不得互换）', '']
    lines.append('| 配置 | k | cpu 中位 [min,max] | tick/s 中位 | **每 tick 成本 µs/tick** | **每注册项 ns/项·tick** | threads |')
    lines.append('|---|---|---|---|---|---|---|')
    groups = {}
    for r in rows:
        if r['diag'] or r.get('recheck'):
            continue
        key = (r['state'], r['n'], r['workers'])
        groups.setdefault(key, []).append(r)
    for key in sorted(groups, key=lambda k: (k[0], k[1], k[2])):
        v = groups[key]
        cpu = [x['cpu'] for x in v]
        tk = [x['tick'] for x in v]
        pt = [x for x in (per_tick_us(x['cpu'], x['tick']) for x in v) if x]
        pe = [x for x in (per_entry_ns(x['cpu'], x['tick'], x['entries']) for x in v) if x]
        st_, n, w = key
        ent = 2 * n if st_ == 3 else n
        pt_s = f"**{st.median(pt):.2f}** [{min(pt):.2f},{max(pt):.2f}]" if pt else "n/a（tick=0，无控制项）"
        pe_s = f"**{st.median(pe):.2f}** [{min(pe):.2f},{max(pe):.2f}]" if pe else "n/a"
        lines.append(f"| state{st_} n={n} W={w} (项数={ent}) | {len(v)} | "
                     f"{st.median(cpu):.5f} [{min(cpu):.5f},{max(cpu):.5f}] | {st.median(tk):.0f} | "
                     f"{pt_s} | {pe_s} | {v[0]['threads']} |")
    lines += ['', '逐 run 明细：', '',
              '| sub | cpu | tick/s | 每 tick 成本 µs | 每注册项 ns | threads | lib_sha | tool_sha |',
              '|---|---|---|---|---|---|---|---|']
    for r in sorted(rows, key=lambda x: x['sub']):
        if r['diag'] or r.get('recheck'):
            continue
        pt = per_tick_us(r['cpu'], r['tick'])
        pe = per_entry_ns(r['cpu'], r['tick'], r['entries'])
        p1 = f"{pt:.2f}" if pt else "n/a"
        p2 = f"{pe:.2f}" if pe else "n/a"
        lines.append(f"| `{r['sub']}` | {r['cpu']:.5f} | {r['tick']:.0f} | "
                     f"{p1} | {p2} | {r['threads']} | `{(r['lib_sha'] or '')[:12]}` | `{(r['tool_sha'] or '')[:12]}` |")
    return '\n'.join(lines) + '\n'


def dump_scan():
    lines = ['# B 组：扫描成本（§10.2；**与控制面 CPU 分列**）', '',
             '⛔ 不得把扫描当作 CPU 达标/不达标的唯一归因（t31-F2/t32/t45：真因是 ShmControlScheduler，量级差 3112×）。', '']
    lines.append('| 配置 | k | 均扫条目/轮 | ns/轮 | ns/条目 | 就绪轮占比 | 等待超时/s |')
    lines.append('|---|---|---|---|---|---|---|')
    groups = {}
    for r in rows:
        if not r['diag'] or r.get('recheck'):
            continue
        key = (r['state'], r['n'], r['workers'])
        groups.setdefault(key, []).append(r)
    for key in sorted(groups, key=lambda k: (k[0], k[1], k[2])):
        v = groups[key]

        def med(field):
            xs = [num(x[field]) for x in v]
            xs = [x for x in xs if x is not None]
            return st.median(xs) if xs else None
        st_, n, w = key
        f = lambda x: ('null' if x is None else (f'{x:.4f}' if abs(x) < 10 else f'{x:.3f}'))
        lines.append(f"| state{st_} n={n} W={w} | {len(v)} | {f(med('avg_entry'))} | {f(med('ns_round'))} | "
                     f"{f(med('ns_entry'))} | {f(med('ready_ratio'))} | {f(med('wto_per_s'))} |")
    lines += ['', '逐 run 明细：', '',
              '| sub | d_scan_rounds | d_scanned | d_ready | d_wait_timeouts | 均扫条目/轮 | ns/轮 | ns/条目 | 就绪轮占比 | 等待超时/s |',
              '|---|---|---|---|---|---|---|---|---|---|']
    for r in sorted(rows, key=lambda x: x['sub']):
        if not r['diag'] or r.get('recheck'):
            continue
        lines.append(f"| `{r['sub']}` | {r['d_rounds']} | {r['d_scan']} | {r['d_ready']} | {r['d_wait']} | "
                     f"{r['avg_entry']} | {r['ns_round']} | {r['ns_entry']} | {r['ready_ratio']} | {r['wto_per_s']} |")
        lines.append(f"| `{r['sub']}` (常驻孪生/gated) | g={r['d_g_rounds']} | g={r['d_g_scan']} | — | — | "
                     f"— | g_ns={r['d_g_time']} | — | — | 常驻={r['res_wto_per_s']} |")
    return '\n'.join(lines) + '\n'


fp = open(os.path.join(OUT, 'A_cpu_tickrate_normalized.md'), 'w', encoding='utf-8')
fp.write(dump_cpu()); fp.close()
fp = open(os.path.join(OUT, 'B_scan_cost.md'), 'w', encoding='utf-8')
fp.write(dump_scan()); fp.close()

rc = [r for r in rows if r.get('recheck')]
if rc:
    fp2 = open(os.path.join(OUT, 'A_cpu_tickrate_normalized.md'), 'a', encoding='utf-8')
    fp2.write('\n## 复核补充轮（异常轮复跑；⛔ 未并入上方每配置 k 轮统计）\n\n')
    fp2.write('| sub | cpu | tick/s | 每注册项 ns |\n|---|---|---|---|\n')
    for r in sorted(rc, key=lambda x: x['sub']):
        pe = per_entry_ns(r['cpu'], r['tick'], 2000)
        fp2.write(f"| `{r['sub']}` | {r['cpu']:.5f} | {r['tick']:.0f} | {pe:.2f} |\n")
    fp2.close()

print(f"runs={len(rows)}  (recheck={len(rc)})  → {OUT}/A_cpu_tickrate_normalized.md, {OUT}/B_scan_cost.md")
