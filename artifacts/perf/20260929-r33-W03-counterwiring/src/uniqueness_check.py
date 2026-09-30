#!/usr/bin/env python3
"""F1/t35 要求 4：**写出点唯一性**机械核对。

对每个 ID 统计"非注释行里出现该 ID 的 (文件, 行) 数"，并归类：
  · 单一写入点  = 恰好 1 处；
  · 多源（须说明汇总口径）= >1 处；
  · 表内登记（counter_table）与分类器 return **不计入**（不是写入点，见 audit 脚本的说明）。
输出 TSV + 汇总。
用法: uniqueness_check.py <repo_root> <out_tsv>
"""
import os, re, sys, collections

root = sys.argv[1] if len(sys.argv) > 1 else '.'
out = sys.argv[2] if len(sys.argv) > 2 else '-'
HDR = 'include/dzIPC/measure/counters.h'
hdr = open(os.path.join(root, HDR), encoding='utf-8').read()
m = re.search(r'enum class CounterId : std::size_t\s*\{(.*?)\n\};', hdr, re.S)
ids = []
for line in m.group(1).split('\n'):
    line = line.split('///')[0].split('//')[0].strip().rstrip(',')
    mm = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*(=\s*0)?$', line)
    if mm and mm.group(1) != 'count':
        ids.append(mm.group(1))

# counters.h 里排除：元数据表行、classify_dzflat_attempt 的 return、note_* 函数体、ScanRoundScope
excl = set()
for pat in (r'\{CounterId::\w+,', r'return \{[^}]*CounterId::\w+'):
    for mm in re.finditer(pat, hdr):
        for cid in re.findall(r'CounterId::(\w+)', mm.group(0)):
            excl.add(cid)

files = []
for base in ('src', 'include'):
    for dp, _, ns in os.walk(os.path.join(root, base)):
        for n in ns:
            if n.endswith(('.cc', '.cpp', '.h', '.hpp', '.cxx')):
                p = os.path.join(dp, n)
                if os.path.relpath(p, root) != HDR:
                    files.append(p)

def is_comment(line):
    s = line.strip()
    return s.startswith('//') or s.startswith('*') or s.startswith('/*')

rows = []
for cid in ids:
    pat = re.compile(r'\bCounterId::' + re.escape(cid) + r'\b|DZIPC_MEASURE_\w+\(\s*CounterId::' + re.escape(cid))
    hits = []
    for p in files:
        txt = open(p, encoding='utf-8', errors='replace').read()
        if cid not in txt:
            continue
        for ln, line in enumerate(txt.split('\n'), 1):
            if is_comment(line):
                continue
            if pat.search(line):
                hits.append((os.path.relpath(p, root), ln))
    # 同一文件内的相邻多行（如 if/else 两分支）算"同一写入点族"
    uniq_files = sorted(set(f for f, _ in hits))
    verdict = ('single' if len(hits) == 1 else
               'single_file_multi_branch' if len(uniq_files) == 1 else
               'multi_source' if hits else 'none')
    ev = ';'.join(f'{f}:{l}' for f, l in hits) or '-'
    rows.append((cid, len(hits), len(uniq_files), verdict, ev))

w = sys.stdout if out == '-' else open(out, 'w', encoding='utf-8')
w.write('id\tn_sites\tn_files\tverdict\tevidence\n')
for r in rows:
    w.write('\t'.join(str(x) for x in r) + '\n')
if out != '-':
    w.close()
c = collections.Counter(r[3] for r in rows)
sys.stderr.write('total=%d %s\n' % (len(rows), dict(c)))
