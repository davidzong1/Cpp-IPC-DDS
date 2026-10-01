#!/usr/bin/env python3
"""W11 (t68/F9) —— **倍数声明检测器**（排除禁令语境）+ 阳性探针自证。

背景（t66 的 F9）：t12 报告 §3.4 用的是
    grep -cE '提升\\s*[0-9]+(\\.[0-9]+)?\\s*倍'
它对本报告 :239 的**禁令句**（⛔ 不得宣称 N 倍提升：…不得写成"B 比 A 快 21 倍"）
会**假阳性**。该句是**纪律句**，不是结论句。

本检测器的做法（⛔ 不得为了消假阳性而放宽到漏真阳性）：
  1. 先把文件按行切开；
  2. **剔除禁令语境行**：该行含任一关键词（⛔ / 不得 / 禁止 / 勿 / 误读 / 写成 / 不应 / 严禁）；
  3. 在**剩余行**上匹配倍数模式（`提升 N 倍` / `快 N 倍` / `N 倍提升` / `N× 提升` 等）；
  4. 把被剔除的行**单列打印**（供人工判真伪），并在结尾断言：
     · 剩余行命中数 == 0（否则**真阳性** ⇒ 报告不合格）；
     · **阳性探针**（人为插入一句结论式倍数声明）必须被抓到 ⇒ 证明检测器没有放宽成漏报。

用法:
  python3 w11_rate_claim_check.py <markdown> [--self-test]
退出码: 0 = 无真阳性且探针被抓到；1 = 有真阳性 或 探针未被抓到。
"""
import re, sys, os, tempfile

# 结论式倍数声明的模式（真阳性目标）
PATTERNS = [
    r'提升\s*\d+(?:\.\d+)?\s*倍',
    r'快\s*\d+(?:\.\d+)?\s*倍',
    r'\d+(?:\.\d+)?\s*倍\s*提升',
    r'加速\s*\d+(?:\.\d+)?\s*倍',
    r'\d+(?:\.\d+)?\s*倍\s*(?:的)?(?:收益|提升)',
]
# 禁令语境关键词（命中任一 ⇒ 该行不作为判据）
NEGATION = ['⛔', '不得', '禁止', '勿', '误读', '写成', '不应', '严禁', '不允许']

PROBE = ('本批实测：B 路径比 A 路径**提升 21 倍**，是本次优化的核心收益。\n'
         '另一处：发布路径**快 14.2 倍**。\n')


def scan(text):
    """返回 (真阳性行, 被剔除的禁令行)。"""
    hits, neg = [], []
    for i, line in enumerate(text.splitlines(), 1):
        if any(k in line for k in NEGATION):
            if any(re.search(p, line) for p in PATTERNS):
                neg.append((i, line.strip()))
            continue
        for p in PATTERNS:
            if re.search(p, line):
                hits.append((i, p, line.strip()))
                break
    return hits, neg


def report(path):
    text = open(path, encoding='utf-8').read()
    hits, neg = scan(text)
    print(f"文件: {path}")
    print(f"  被剔除的**禁令语境**行（不计判据，列出供人工判真伪）: {len(neg)}")
    for i, l in neg:
        print(f"    :{i}  {l[:110]}")
    print(f"  **真阳性**（非禁令语境下的倍数声明）: {len(hits)}")
    for i, p, l in hits:
        print(f"    :{i}  [{p}]  {l[:110]}")
    return hits, neg


def self_test():
    """阳性探针：把 PROBE 插进真文件，检测器**必须**抓到。"""
    print("\n=== 阳性探针自证（t68/F9）: 检测器对阳性是否有效 ===")
    rc = 0
    with tempfile.NamedTemporaryFile('w', suffix='.md', delete=False, encoding='utf-8') as f:
        f.write(PROBE)
        probe_path = f.name
    # ① 纯探针文件：两条都应被抓到
    hits, neg = scan(open(probe_path, encoding='utf-8').read())
    print(f"  纯探针：真阳性 = {len(hits)}（期望 2）、禁令剔除 = {len(neg)}（期望 0）")
    if len(hits) != 2:
        print("  ✗ 探针未被全部抓到 ⇒ 检测器**过宽**（漏真阳性）")
        rc = 1
    else:
        print("  ✅ 探针 2/2 被抓到 ⇒ 检测器未放宽")
    # ② 探针 + 禁令句混合：只应剩纯探针的两条
    mixed = PROBE + '⛔ 不得宣称「提升 21 倍」，⛔ 也不得写成「快 14.2 倍」。\n'
    with open(probe_path, 'w', encoding='utf-8') as f:
        f.write(mixed)
    hits2, neg2 = scan(open(probe_path, encoding='utf-8').read())
    print(f"  探针+禁令混合：真阳性 = {len(hits2)}（期望 2）、禁令剔除 = {len(neg2)}（期望 1）")
    if len(hits2) != 2 or len(neg2) != 1:
        print("  ✗ 禁令剔除逻辑不正确")
        rc = 1
    else:
        print("  ✅ 禁令句被正确剔除，探针仍 2/2 被抓 ⇒ **既不假阳也不漏报**")
    os.unlink(probe_path)
    return rc


if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else None
    rc = 0
    if target:
        hits, neg = report(target)
        if hits:
            print("\n⇒ **存在真阳性倍数声明 ⇒ 报告不合格**")
            rc = 1
        else:
            print("\n⇒ 无非禁令语境下的倍数声明 ✅")
    if '--self-test' in sys.argv:
        rc |= self_test()
    sys.exit(rc)
