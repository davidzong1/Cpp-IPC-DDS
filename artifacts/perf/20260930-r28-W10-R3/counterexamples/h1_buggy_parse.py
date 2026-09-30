#!/usr/bin/env python3
"""H1 最小反例：逐字复现 w10_matrix.cpp 修前的解析逻辑，打印实际得到的 used 集合。

修前逻辑（队长已核实）：
    std::istringstream is(line);
    std::string addr;
    if (!(is >> addr)) continue;          # 取第一个 token
    const auto colon = addr.find(':');
    used.insert(strtol(addr.c_str() + colon + 1, nullptr, 16));
"""
import re, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/proc/net/udp"
used = set()
samples = []
with open(path) as f:
    next(f)                                   # 表头
    for line in f:
        toks = line.split()
        if not toks:
            continue
        addr = toks[0]                        # ← 修前：第一个 token（实为 sl 列，形如 "0:"）
        colon = addr.find(':')
        tail = addr[colon + 1:]               # ← 冒号在索引 1 ⇒ tail 为空串
        try:
            v = int(tail, 16) if tail else 0  # strtol("") = 0
        except ValueError:
            v = 0
        used.add(v)
        if len(samples) < 3:
            samples.append((line.split(), addr, repr(tail), v))

print("== 修前（逐字复现）==")
for t, a, tail, v in samples:
    print(f"  行前 3 列 = {t[:3]}  第一个 token = {a!r}  冒号后子串 = {tail}  → 解析值 = {v}")
print(f"  used 集合 = {sorted(used)}    |used| = {len(used)}")

# 正确解析（第二列 local_address）
used_ok = set()
bad = 0
with open(path) as f:
    next(f)
    for line in f:
        toks = line.split()
        if len(toks) < 2:
            bad += 1
            continue
        a = toks[1]
        c = a.find(':')
        if c < 0:
            bad += 1
            continue
        try:
            used_ok.add(int(a[c + 1:], 16))
        except ValueError:
            bad += 1
print("== 修后（按列解析，第 2 列 local_address）==")
print(f"  used 集合大小 = {len(used_ok)}（解析失败 {bad} 行）；样本 = {sorted(used_ok)[:8]}")
print()
print(f"判定：修前 |used|={len(used)}（{sorted(used)}）⇒ **端口预检从未生效**；"
      f"修后 |used|={len(used_ok)} ⇒ 预检生效")
