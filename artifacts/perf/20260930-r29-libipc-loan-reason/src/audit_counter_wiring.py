#!/usr/bin/env python3
"""F1/t35：CounterId 接线清点 —— **机械可核对**，且刻意不算"grep 命中即接线"。

判定为什么需要分层（否则会假绿）：
  counters.h 里出现 `CounterId::X` 有四种性质完全不同的场合：
    ① `counter_table()` 的元数据行        → 只是**登记**，不是生产者；
    ② `classify_dzflat_attempt()` 的 return  → **潜在**写入点，但只有该分类器被调用才成立；
    ③ `note_dzflat_attempt/note_*` 的函数体 → 同②，取决于有无调用方；
    ④ `ScanRoundScope` 的构造/析构体        → 同②，取决于有无使用方。
  ⇒ 因此本脚本把"写入点"定义为**在 src/ 的非 counters.h 文件里的实际落计数调用**，
    并把 counters.h 内部的路径分别标注为「潜在（须验调用方）」。

判定取值：
  wired            非 counters.h 文件里出现 CounterId::X（宏/inc 实参位置）
  potential_unused X 只在 counters.h 的 ②③④ 里出现，且其载体在 src/ **无调用方** ⇒ 未接线
  potential_used   同上，但载体在 src/ 有调用方 ⇒ 视为 wired（证据记调用点）
  table_only       X 只出现在 ①（登记表）⇒ 从未接线
用法: audit_counter_wiring.py <repo_root> <out_tsv>
"""
import os, re, sys, collections

root = sys.argv[1] if len(sys.argv) > 1 else '.'
out = sys.argv[2] if len(sys.argv) > 2 else '-'
HDR = 'include/dzIPC/measure/counters.h'
hdr = open(os.path.join(root, HDR), encoding='utf-8').read()

# ---- 1) 枚举名 ----
m = re.search(r'enum class CounterId : std::size_t\s*\{(.*?)\n\};', hdr, re.S)
ids = []
for line in m.group(1).split('\n'):
    line = line.split('///')[0].split('//')[0].strip().rstrip(',')
    mm = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*(=\s*0)?$', line)
    if mm and mm.group(1) != 'count':
        ids.append(mm.group(1))
idset = set(ids)

# ---- 2) counters.h 内部按函数切片 ----
def slice_fn(src, name):
    i = src.find(name)
    if i < 0: return ''
    # 到下一个顶层注释块/函数定义结束（用大括号配平）
    j = src.find('{', i)
    if j < 0: return ''
    depth = 0
    for k in range(j, len(src)):
        if src[k] == '{': depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0: return src[i:k+1]
    return src[i:]

tbl = slice_fn(hdr, 'counter_table()')
cls = slice_fn(hdr, 'classify_dzflat_attempt')
notes = slice_fn(hdr, 'note_dzflat_attempt') + slice_fn(hdr, 'note_dzflat_borrow_failed')
scope = slice_fn(hdr, 'class ScanRoundScope')

def ids_in(txt):
    return set(re.findall(r'CounterId::(\w+)', txt)) & idset

ids_table = ids_in(tbl)
ids_cls   = ids_in(cls)
ids_notes = ids_in(notes)
ids_scope = ids_in(scope)
# 去掉落在 ①②③④ 的裸 "CounterId::X"（例如 cls 的 return 里）
carriers = {
    'classify_dzflat_attempt': ('note_dzflat_attempt', ids_cls),
    'note_dzflat_borrow_failed': ('note_dzflat_borrow_failed', ids_notes),
    'ScanRoundScope': ('ScanRoundScope', ids_scope),
}

# ---- 3) 收集非 counters.h 的出现 ----
files = []
for base in ('src', 'include'):
    for dp, _, ns in os.walk(os.path.join(root, base)):
        for n in ns:
            if n.endswith(('.cc', '.cpp', '.h', '.hpp', '.cxx')):
                p = os.path.join(dp, n)
                if os.path.relpath(p, root) == HDR: continue
                files.append(p)

def is_comment(line):
    s = line.strip()
    return s.startswith('//') or s.startswith('*') or s.startswith('/*')

hits = collections.defaultdict(list)          # cid -> [(file, line, text)]
carrier_uses = collections.defaultdict(set)    # carrier name -> {file}
for p in files:
    rel = os.path.relpath(p, root)
    txt = open(p, encoding='utf-8', errors='replace').read()
    for cname, _ in carriers.items():
        if cname in txt and rel != HDR:
            carrier_uses[cname].add(rel)
    for ln, line in enumerate(txt.split('\n'), 1):
        if is_comment(line): continue
        for cid in re.findall(r'CounterId::(\w+)', line):
            if cid in idset:
                hits[cid].append((rel, ln, line.strip()))

def carrier_used(cid):
    for cname, (_fn, ids_) in carriers.items():
        if cid in ids_:
            for u in carrier_uses.get(cname, ()):
                if not u.endswith('counters.h'):
                    return cname, sorted(carrier_uses[cname])
    return None, None

rows = []
for cid in ids:
    direct = hits.get(cid, [])
    if direct:
        verdict = 'wired'
        ev = ';'.join(f'{f}:{l}' for f, l, _ in direct[:8])
    else:
        cname, users = carrier_used(cid)
        if cname:
            verdict = 'potential_used' if users else 'potential_unused'
            ev = f'carrier={cname} users={users or "NONE"}'
        elif cid in ids_table:
            verdict = 'table_only'
            ev = 'counter_table only'
        else:
            verdict = 'table_only'
            ev = 'no occurrence outside enum/table'
    # 生产路径 vs 测量自检路径：proc_sampler.h / measurement.h 里的出现属于
    # "开销自测/示例"，**不算**产品的生产写入点（否则会把自测读数误当成运行时接线）。
    SELF_TEST = ('include/dzIPC/measure/proc_sampler.h', 'include/dzIPC/measure/measurement.h')
    prod = [h for h in direct if h[0] not in SELF_TEST]
    selftest = [h for h in direct if h[0] in SELF_TEST]
    if direct and not prod and selftest:
        verdict = 'selftest_only'
        ev = 'self-test only: ' + ';'.join(f'{f}:{l}' for f, l, _ in selftest[:4])
    rows.append((cid, len(prod), verdict,
                 ev if verdict != 'selftest_only' else ev))

order = {'wired': 0, 'potential_used': 1, 'selftest_only': 2, 'potential_unused': 3, 'table_only': 4}
rows.sort(key=lambda r: (order[r[2]], r[0]))

w = sys.stdout if out == '-' else open(out, 'w', encoding='utf-8')
w.write('id\tn_direct_writers\tverdict\tevidence\n')
for r in rows:
    w.write('\t'.join(str(x) for x in r) + '\n')
if out != '-': w.close()
c = collections.Counter(r[2] for r in rows)
sys.stderr.write('total=%d %s\n' % (len(rows), dict(c)))
