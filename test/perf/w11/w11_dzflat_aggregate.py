#!/usr/bin/env python3
"""W11 (t12) —— DZFlat TLV/A/B 串行配对聚合（只读）。

从 `w11_dzflat_pair.sh` 的 `round<K>/artifacts/perf/<内嵌 run_id>/samples.csv` 逐样本重算，
按方案 §10.7 的**三个实验组**分列：

  实验组① 传输机制   transport_ns = transport_done_ns − publish_enter_ns
  实验组② 完整读取   app_read_ns  = fully_consumed_ns − app_obtained_ns
  实验组③ 生产到消费 e2e_ns       = fully_consumed_ns − produced_ns
  （另给通知与交付 delivery_ns = app_obtained_ns − transport_done_ns，不计入三组）

⛔ 禁止把实验组①称为「完整应用端到端延迟」。
⛔ 不得宣称「N 倍提升」——B 的收益只能表述为「省掉一次 0.88 MB 拷贝」。
⛔ 轮间离散度按**逐轮中位**给出，⛔ 不挑最优轮。

用法: w11_dzflat_aggregate.py <pair_root> [out_dir]
"""
import csv, glob, json, os, statistics as st, sys

R = sys.argv[1] if len(sys.argv) > 1 else 'artifacts/perf/20260930-r49-W11-dzflat'
OUT = sys.argv[2] if len(sys.argv) > 2 else R
os.makedirs(OUT, exist_ok=True)

SIZES = ('img66k', 'img262k', 'img880k')


def rounds():
    out = []
    for rd in sorted(glob.glob(R + '/round*')):
        k = os.path.basename(rd)
        if not k.startswith('round') or not k[5:].isdigit():
            continue
        for d in sorted(glob.glob(rd + '/artifacts/perf/*')):
            if os.path.exists(d + '/samples.csv'):
                out.append((int(k[5:]), d))
    return out


def load():
    data = {}
    for k, d in rounds():
        cfg = os.path.basename(d)
        m = json.load(open(d + '/manifest.json')) if os.path.exists(d + '/manifest.json') else {}
        for row in csv.DictReader(open(d + '/samples.csv')):
            sz = row['route'].rsplit('/', 1)[-1]
            if sz not in SIZES:
                continue
            def gi(f):
                v = row.get(f)
                return int(v) if v not in (None, '') else None
            data.setdefault((cfg, sz), []).append(dict(
                round=k, path=row['path'], transport=gi('transport_ns'),
                delivery=gi('delivery_ns'), app_read=gi('app_read_ns'), e2e=gi('e2e_ns'),
                bytes=gi('payload_bytes'), ok=str(row.get('payload_checksum_ok')),
                dropped=str(row.get('dropped')), lib=m.get('lib_sha256', '')))
    return data


data = load()

# 每配置每尺寸：逐轮中位 → 再取轮间中位（并把轮值列出）
def per_config():
    res = {}
    for (cfg, sz), rows in data.items():
        byr = {}
        for r in rows:
            byr.setdefault(r['round'], []).append(r)
        perround = {}
        for k, v in byr.items():
            perround[k] = {f: st.median([x[f] for x in v if x[f] is not None])
                           for f in ('transport', 'delivery', 'app_read', 'e2e')}
        res[(cfg, sz)] = perround
    return res


pc = per_config()
cfgs = sorted(set(c for c, _ in pc))

lines = ['# W11 · DZFlat TLV/A/B 串行配对（方案 §10.7 三实验组）', '',
         '口径：① 传输机制 `transport_ns`；② 完整读取 `app_read_ns`；③ 生产到消费 `e2e_ns`。',
         '⛔ 实验组①**不是**完整应用端到端延迟；⛔ 全文不出现「N 倍提升」表述。', '',
         '## 一、逐配置 × 尺寸档（轮间中位；单位 µs）', '',
         '| 配置(run_id) | 尺寸 | 载荷 B | ① transport | ② app_read | ③ e2e | 轮数 |',
         '|---|---|---|---|---|---|---|']

alias = {}


def short(cfg):
    if 'dzflat-b' in cfg:
        return 'B ' + ('prebuilt' if 'prebuilt' in cfg else 'permsg')
    if 'dzflat-a' in cfg:
        return 'A ' + ('prebuilt' if 'prebuilt' in cfg else 'permsg')
    return 'TLV ' + ('prebuilt' if 'prebuilt' in cfg else 'permsg')


for cfg in cfgs:
    alias[cfg] = short(cfg)
for cfg in cfgs:
    for sz in SIZES:
        pr = pc.get((cfg, sz))
        if not pr:
            continue
        rows = [list(pr[k].values()) for k in sorted(pr)]
        med = lambda i: st.median([r[i] for r in rows])
        pl = data[(cfg, sz)][0]['bytes']
        lines.append(f"| `{cfg}` ({alias[cfg]}) | {sz} | {pl} | {med(0)/1000:.2f} | {med(2)/1000:.1f} | {med(3)/1000:.1f} | {len(pr)} |")

lines += ['', '## 二、轮间离散度（逐轮中位，⛔ 不做最优轮挑选）', '',
          '| 配置 | 尺寸 | 指标 | 逐轮中位（µs） | 中位 | 极差比 |', '|---|---|---|---|---|---|']
for cfg in cfgs:
    for sz in SIZES:
        pr = pc.get((cfg, sz))
        if not pr:
            continue
        for fi, fname in ((0, 'transport①'), (3, 'e2e③')):
            vals = [pr[k][list(pr[k])[fi]] for k in sorted(pr)]
            lines.append(f"| `{alias[cfg]}` | {sz} | {fname} | {[round(v/1000,1) for v in vals]} | "
                         f"{st.median(vals)/1000:.1f} | {max(vals)/min(vals):.2f}× |")

lines += ['', '## 三、可比配对（**同一 variant** 才可比；⛔ 跨 variant 直接比会混入应用侧构造差异）', '',
          '| 尺寸 | 组 | TLV | A | B | B−A (③ e2e) | 说明 |', '|---|---|---|---|---|---|---|']
for sz in SIZES:
    for var in ('prebuilt', 'permsg'):
        g = {}
        for cfg in cfgs:
            if var in cfg:
                tag = 'TLV' if 'tlv' in cfg else ('A' if 'dzflat-a' in cfg else 'B')
                pr = pc.get((cfg, sz))
                if pr:
                    rows = [list(pr[k].values()) for k in sorted(pr)]
                    g[tag] = (st.median([r[0] for r in rows]), st.median([r[3] for r in rows]))
        if 'A' not in g or 'B' not in g:
            continue
        d = g['B'][1] - g['A'][1]
        pct = d / g['A'][1] * 100 if g['A'][1] else float('nan')
        lines.append(f"| {sz} | {var} | {g.get('TLV',(float('nan'),))[0]:.1f} µs(①) | "
                     f"{g['A'][1]/1000:.1f} µs(③) | {g['B'][1]/1000:.1f} µs(③) | "
                     f"{d/1000:+.1f} µs ({pct:+.1f}%) | ①: A={g['A'][0]/1000:.2f} B={g['B'][0]/1000:.2f} µs |")

lines += ['', '## 四、路径可分性（发布侧计数；跨进程自报）', '',
          '| 配置 | 尺寸 | 样本 | checksum_ok | dropped |', '|---|---|---|---|---|']
for cfg in cfgs:
    for sz in SIZES:
        rows = data.get((cfg, sz))
        if not rows:
            continue
        oks = sum(1 for r in rows if r['ok'] == 'True')
        dr = sum(1 for r in rows if r['dropped'] == 'True')
        lines.append(f"| `{alias[cfg]}` | {sz} | {len(rows)} | {oks}/{len(rows)} | {dr} |")

fp = open(os.path.join(OUT, 'C_dzflat_ab.md'), 'w', encoding='utf-8')
fp.write('\n'.join(lines) + '\n')
fp.close()
print(f"rounds={len(set(k for k,_ in rounds()))} configs={len(cfgs)} → {OUT}/C_dzflat_ab.md")
