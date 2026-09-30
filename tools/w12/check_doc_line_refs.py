#!/usr/bin/env python3
"""W12 前置门禁 · 跨文档「代码行号」一致性核查（队长裁决 D-21 / 任务 t28）

设计要点（为什么必须「符号邻接」才判定）
----------------------------------------
`file:NNN` 本身**不可机械判定** —— 同一个文件有成千行，`ipc.cpp:461` 可能指
`reclaim_dead_chunks`，也可能指旁边的 `large_msg_cache` 注释。把任意
`file:NNN` 都拿去比对一个锚点，会制造大量**假阳性**（本工具第一版即如此，
被自身阳性对照实验暴露：`ipc.cpp:461` 等 49 处被误判）。

因此判据是 **「引用 + 锚点符号在同一处出现」**：
  · 只有当文档在该行（或紧邻的续行）**同时**给出 `basename:NNN` 与锚点符号时，
    才认为「该行号声称指向该符号」⇒ 可机械判定；
  · 只给 `file:NNN` 而附近没有锚点符号的引用 ⇒ 归入 **unverifiable（不可判定）**，
    只计数、**不计失败**（这是诚实的：我们无法机械知道它指谁）。

三态（处置不同，不得混为一类）
------------------------------
  一致    consistent   ：声明值与锚点提交（或工作区）真实行号相符
  已漂移  drifted      ：声明值 = **锚点提交**真实行号，但**工作区**该锚点已不存在
                         （代码被后续工作包改了，如 W07 把 `select` 迁 `poll`）
                         ⇒ 按锚点提交复核是对的，属"预期漂移"，**不是文档错误**
  计数错误 count-error ：两棵树都对不上，且与真实行号呈固定偏移（off-by-one 等）
                         ⇒ **文档必须改**（唯一触发非零退出码的状态）

两棵树：文档合理地同时引用「基线提交」与「当前已修复工作区」两种口径
（如 `udp.h:347` 是基线口径、`udp.h:360/368` 是 W07 修复后的工作区口径），
只认一棵树会把后者误判为计数错误，故 `一致` 只要命中任一树即可。

⛔ 只报告不改写（写入权分离）。本脚本不修改任何文件。

用法
----
    python3 tools/w12/check_doc_line_refs.py                 # 锚点 e800ccc、扫 docs/
    python3 tools/w12/check_doc_line_refs.py --self-test     # 阳性/阴性/误报三向自证
    python3 tools/w12/check_doc_line_refs.py --anchor <rev> --scan docs --out /tmp/x.txt

例外表：tools/w12/line_ref_exceptions.txt（`<文档>:<行> <basename> <登记编号>`）
        —— 只放**已登记待收口**的计数错误（如 W01-F3）；命中即记「📌 已知待收口」且不计失败。
        ⛔ W12 收口后必须删除对应行，否则该例外会永久掩盖后续回归。

退出码：0 = 无「计数错误」；1 = 存在「计数错误」；2 = 前置条件缺失
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

ANCHORS = [
    # (file, 源码侧符号[可多处命中；命中任一处即算一致], 首选行号, 说明, 文档侧写法[用于把引用归因])
    # ⚠️ 源码侧符号必须用**带语境的构造**（如 `FD_SET(server_fd`），不能用裸 `FD_SET` ——
    #    后者会命中注释里的「用 poll 而不是 select/FD_SET」，把已迁 poll 的工作区误判为"未漂移"。
    ("src/libipc/platform/posix/udp.h", "FD_ZERO(&read_fds)", 346, "定时等待分支：位图清零",
     [r"FD_ZERO"]),
    ("src/libipc/platform/posix/udp.h", "FD_SET(server_fd", 347, "定时等待分支：填位图（off-by-one 高发）",
     [r"FD_SET"]),
    ("src/libipc/platform/posix/udp.h", "::select(server_fd", 353, "定时等待分支：::select（W07 已迁 poll）",
     [r"::select", r"select\(\)"]),
    ("src/dzIPC/shm_pub_sub_ipc.cc", "publish_thread_ = new std::thread", 237, "per-route 控制线程",
     [r"publish_thread_", r"pub_handshake"]),
    ("src/dzIPC/shm_pub_sub_ipc.cc", "sub_handshake_thread_ = new std::thread", 940, "per-route 握手线程",
     [r"sub_handshake_thread_", r"sub_handshake"]),
    ("src/dzIPC/shm_pub_sub_ipc.cc", "subscribe_thread_ = new std::thread", 941, "per-route 订阅线程",
     [r"subscribe_thread_"]),
    ("src/libipc/ipc.cpp", "reclaim_dead_chunks(", 683, "死条目回收（定义 :556 / 调用 :683，两处都算一致）",
     [r"reclaim_dead_chunks"]),
    ("include/dzIPC/ipc_info_pool.h", "kMaxEntries = ", 36, "注册表容量（W07 后 :52 = 4096）",
     [r"kMaxEntries"]),
]

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# 只对**声明了锚点提交**的文档做「计数错误」判定：未声明基准的文档无法机械
# 判断其行号属哪棵树（历史的自家基准），强判会把它们全部误报。这类文档计入
# 「不可判定（no-anchor-doc）」，供 W12 人工决定是否补锚点声明。
ANCHOR_DECL = re.compile(r"(锚点提交|锚定提交|基线提交|source_revision|基线)")

# 同一行内、或前一非空行内含 basename 时，本行的裸 `:NNN` 也归该 basename
# 冻结快照目录标记（t54，D-21 补充）：按体例**逐字保留修订前原文**的目录 ⇒ 不参与
# 行号一致性判定（否则任何 WP 新增历史快照都会永久污染门禁）。命中即**整棵子树剪枝**，
# 并在报告中列出被跳过的目录（⛔ 不静默跳过）。只认**目录名**，不认文件名。
FROZEN_SNAPSHOT_DIR_MARKERS = {"历史版本", "历史快照", "superseded-snapshots"}

BARE = re.compile(r"(?<![\w:])[:：](\d{1,5})(?:-(\d{1,5}))?(?![\w-])")
WITHBASE = re.compile(r"(?<![\w])(?:[\w./-]*/)?([\w.]+\.(?:h|hpp|cc|cpp|c))[:：](\d{1,5})(?:-(\d{1,5}))?(?![\w-])")


def _path_suffix_ok(cited: str, base: str) -> bool:
    """引用里带目录时，其目录链必须与锚点文件路径一致（后缀匹配）。"""
    cited = cited.strip().lstrip("./")
    if "/" not in cited:
        return True
    return any(f.endswith("/" + cited) or f == cited for f, _s, _e, _n, _p in ANCHORS)


def git(*args: str) -> str:
    r = subprocess.run(["git", "-C", REPO, *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


def read_lines(rev: str | None, path: str) -> list[str]:
    if rev is None:
        try:
            with open(os.path.join(REPO, path), errors="ignore") as fh:
                return fh.read().split("\n")
        except OSError:
            return []
    try:
        return git("show", f"{rev}:{path}").split("\n")
    except RuntimeError:
        return []


def real_lines(txt: list[str], sym: str) -> list[int]:
    return [i for i, l in enumerate(txt, 1) if sym in l]


SMALL_OFFSET_WINDOW = 3   # 判「计数错误」的偏移窗：只抓 off-by-one/小计数误差
DECL_SPAN = 1             # 声明跨行容差：**仅**紧邻的签名续行算一致
                          # （如 `bool reclaim_dead_chunks(` :556 → :557 参数续行）。
                          # ⚠️ 不可放宽到 3：实测会把 `udp.h:349-353`（真错，应为 :345-353）
                          #    误判成"一致"（349 落在 347+3 内）—— 放宽容差 = 放过真缺陷。

CTX_SWITCH = ("win 侧", "win侧", "Windows", "平台侧", "另一文件", "别的文件", "非本", "include/libipc/udp.h")

META_MARKERS = ("应为", "期望", "纠正", "勘误", "off-by-one", "已修正", "已改为",
                "❌", "误写", "错了", "不是", "而非", "~~", "锚点提交", "声明=",
                # 讨论/对照/现状描述语境（在谈"哪里写错了/现状是几"），不是位置断言
                "实际", "已迁", "旧：", "原表述", "纠正为", "主报告", "本文档", "同处扩散",
                "位置（", "旧行号", "更正")

def doc_declares_anchor(path: str) -> bool:
    try:
        with open(path, errors="ignore") as fh:
            return bool(ANCHOR_DECL.search(fh.read()))
    except OSError:
        return False


def collect_citations(scan_dirs: list[str],
                      bases: set[str]) -> tuple[list[tuple[str, int, str, int, int, int]], list[str]]:
    """返回 ((relpath, srcline, basename, start, end, col), 跳过的冻结快照目录列表)。

    `col` = 该引用在源行内的字符偏移，用于把**同一行的多个引用**各自归给最近的锚点符号。

    同行/续行的裸 `:NNN` 归属最近出现的 basename（支持
    「`udp.h:346` 的 FD_SET 与 `:352` 的 ::select」这种并列表述）。

    ⛔ **冻结快照目录（`历史版本/`）不参与行号一致性判定**（t54 新增）：这类目录按体例
    **必须逐字保留修订前的原文**（含当时正确的旧行号），因此它**必然**被判为「计数错误」——
    而那不是文档错误，是快照的用途本身。若不做豁免，任何一个 WP 新增历史快照都会**永久污染**
    门禁的「待收口」段（实测 t54 的 `_20260930_t54修订前快照.md` 正是如此）。
    豁免只作用于**目录**，不作用于文件内容；且跳过的目录会**在报告中逐条列出**（⛔ 不静默跳过）。
    """
    out = []
    skipped_dirs: list[str] = []
    for d in scan_dirs:
        for dirpath, dirs, files in os.walk(os.path.join(REPO, d)):
            # 剪枝：命中冻结快照目录标记即整棵子树跳过（⛔ 不逐文件豁免，避免漏网）
            frozen = [x for x in dirs if x in FROZEN_SNAPSHOT_DIR_MARKERS]
            for x in frozen:
                dirs.remove(x)
                skipped_dirs.append(os.path.relpath(os.path.join(dirpath, x), REPO))
            for name in files:
                p = os.path.join(dirpath, name)
                try:
                    with open(p, errors="ignore") as fh:
                        raw = fh.read().split("\n")
                except OSError:
                    continue
                for ln, line in enumerate(raw, 1):
                    if any(mk in line for mk in META_MARKERS):
                        continue  # 元讨论行（在讨论/纠正行号本身）⇒ 不是位置断言
                    rel = os.path.relpath(p, REPO)
                    carry_name = None
                    allnames = set(m.group(1) for m in WITHBASE.finditer(line))
                    cand_names = {n for n in allnames if n in bases} | {n for n in allnames}
                    hits = []
                    spans = []
                    for m in WITHBASE.finditer(line):
                        if m.group(1) not in bases:
                            continue
                        a = int(m.group(2))
                        bb = int(m.group(3)) if m.group(3) else a
                        hits.append((m.group(1), a, bb))
                        spans.append(m.span())
                    for mm, (b, a, bb) in zip([x for x in WITHBASE.finditer(line) if x.group(1) in bases], hits):
                        full = mm.group(0).split(":")[0]
                        if full != b and not _path_suffix_ok(full, b):
                            continue  # 例：`include/libipc/udp.h` ≠ `src/libipc/platform/posix/udp.h`
                        out.append((rel, ln, b, a, bb, mm.start()))
                    if hits:
                        carry_name = hits[-1][0]
                    last_base_end = spans[-1][1] if spans else 0
                    # 同一行里**未被 file:NNN 覆盖**的裸 `:NNN`：
                    #   ⛔ 只在该行**唯一**候选 basename 时才归属 —— 一行里若同时出现
                    #     多个 basename 语境（如「posix/udp.h:347 … win 侧 :327/:330」），
                    #     裸 `:NNN` 到底属谁**无法机械判定** ⇒ 归 unverifiable，不计失败。
                    #   ⛔ 不做跨行 carry：实测会把他行（如 shm_ser_cli 的 `:285`）误挂过来。
                    if any(c in line for c in CTX_SWITCH):
                        continue  # 本行切换了文件语境（如「win 侧 :327/:330」）⇒ 裸引用无法机械归属
                    if carry_name and line.strip() and len(cand_names) == 1:
                        for m in BARE.finditer(line):
                            if any(m.start() >= s0 and m.end() <= s1 for s0, s1 in spans):
                                continue
                            # ⛔ 必须紧跟 basename（120 字符窗口内）——否则「；win 侧 :327/:330」
                            #    这类别的文件语境会被误挂到本锚点文件上（实测假阳性）
                            if m.start() - last_base_end > 120:
                                continue
                            a = int(m.group(1))
                            bb = int(m.group(2)) if m.group(2) else a
                            out.append((rel, ln, carry_name, a, bb, m.start()))
    return sorted(set(out)), sorted(set(skipped_dirs))


def nearest_symbol(raw_lines: list[str], ln: int, col: int, pats: list[str]) -> str | None:
    """把一条引用归给它**最近的**锚点符号（同行为字符距离，±1 行按行距 + 大权重）。

    必要性：一行里可能并列多个引用（如「`udp.h:346` 的 FD_SET 与 `:352` 的 ::select」），
    若不做最近归因，`:352` 会被同时拿去比对 FD_SET，产出**期望值误导**的二重判定。
    """
    best, best_d = None, None
    if not (1 <= ln <= len(raw_lines)):
        return None
    line = raw_lines[ln - 1]          # ⛔ 只看同一行（邻行会让别的语境误挂，实测假阳性）
    for pat in pats:
        for m in re.compile(pat).finditer(line):
            d = abs(m.start() - col)
            if best_d is None or d < best_d:
                best, best_d = pat, d
    return best


def classify(a: int, b: int, anchor_real: list[int], wt_real: list[int], group: list[int] | None = None):
    if not anchor_real:
        return "锚点歧义", "锚点提交内无此符号"
    # ⚠️ 同一符号在文件内可有**多处**（定义 + 调用点）：命中**任一处**都算一致。
    #    只认单一「冻结值」会把 `reclaim_dead_chunks` 的**定义行**（:556）误判为偏移。
    # 函数/变量声明可跨多行：引用落在声明行起 <DECL_SPAN> 行内也算「一致」
    # （如 `bool reclaim_dead_chunks(` 在 :556，其签名续行 :557 被文档引用 ⇒ 不算错）
    in_anchor = any(a <= x <= b or x <= a <= x + DECL_SPAN for x in anchor_real)
    in_wt = any(a <= x <= b or x <= a <= x + DECL_SPAN for x in wt_real)
    areal = anchor_real[0]
    if in_anchor:
        if not wt_real:
            # ★ 本门禁的「已漂移」定义：声明值**在锚点提交上正确**，但**工作区该符号已不存在**
            #   （典型：W07 把 `select` 迁 `poll` 后，历史文档仍写 `:353 ::select`）
            #   ⇒ 引用**不是错的**，处置 = 保留并在文档内标注锚定提交；**不计失败**。
            return "已漂移", (f"锚点提交={areal}（声明正确）；**工作区已无该符号**"
                            f" ⇒ 预期漂移，按锚点提交复核；建议文档标注锚定提交")
        if not in_wt:
            return "已漂移", (f"锚点提交={areal}（声明正确）；工作区已移位到 {','.join(map(str, wt_real))}"
                            f" ⇒ 按锚点提交复核")
        return "一致", f"锚点提交={areal}；工作区={','.join(map(str, wt_real))}"
    if in_wt:
        return "一致", f"按工作区口径正确={','.join(map(str, wt_real))}（锚点提交为 {areal}）"
    nearest = min(anchor_real, key=lambda x: abs(x - a))
    off = a - nearest
    if abs(off) > SMALL_OFFSET_WINDOW:
        # 小幅偏移 = 计数错误（这是本门禁的靶心）；大幅偏移无法机械区分
        # 「计数错误」与「引用同文件的另一处/函数体内语义」⇒ 归不可判定，避免误伤。
        return "不可判定", f"偏移={off:+d}（超出 ±{SMALL_OFFSET_WINDOW} 判定窗；疑引用同文件别处或函数体内语义）"
    tag = "off-by-one" if abs(off) == 1 else f"偏移={off:+d}"
    lo, hi = min(anchor_real), max(anchor_real)
    if b > a and (a <= hi or a <= lo <= b):
        # 区间型声明：给出「应覆盖范围」比给单点更可操作（用**同文件全锚点组**的最外边界）
        g = sorted(group) if group else anchor_real
        exp = f"应覆盖 :{g[0] - 1 if g[0] > 1 else g[0]}-{g[-1]}（该文件锚点组 {g}）"
    else:
        exp = ":" + "/".join(map(str, anchor_real))
    exp += (f"；工作区 :{','.join(map(str, wt_real))}" if wt_real else "；工作区已无该符号")
    return "计数错误", f"**期望 {exp}**，声明=:{a}{'-' + str(b) if b != a else ''}（{tag}）"


def load_exceptions(path: str | None) -> list[tuple[str, int, str]]:
    """例外表：`docfile:line  basename  id`（# 开头为注释）。命中的计数错误记为
    「已知待收口（已登记 id）」并**不计失败** —— 前提是该 id 必须真实存在于团队登记
    （如 W01-F3/D-20）；本函数只做匹配，不校验 id 是否真被登记（由 W12 人工保证）。"""
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path, errors="ignore") as fh:
        for raw in fh:
            line = raw.split("#")[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            loc, base, eid = parts[0], parts[1], parts[2]
            if ":" not in loc:
                continue
            f, n = loc.rsplit(":", 1)
            out.append((f.strip(), int(n), f"{base} ({eid})"))
    return out


def run(anchor: str, scan_dirs: list[str], out_path: str,
        exceptions: list[tuple[str, int, str]] | None = None,
        self_test_mode: bool = False) -> tuple[int, str]:
    L: list[str] = []
    add = L.append
    ver = git("rev-parse", anchor).strip()
    add("=" * 78)
    add("W12 前置门禁 · 跨文档代码行号一致性核查（D-21 / t28）")
    add(f"仓库={REPO}")
    add(f"锚点提交={ver}   扫描={', '.join(scan_dirs)}")
    add("判据：仅当 `file:NNN` 与锚点**符号同处出现**（同行/±1 行）时才判定；")
    add("      只有行号无可辨符号的引用归入 unverifiable（不可判定，不计失败）")
    add("三态：一致 / 已漂移（锚点值正确、工作区已变）/ 计数错误（两棵树都对不上）")
    add("⛔ 只报告不改写")
    add("=" * 78)
    add("")

    exc = exceptions or []
    stat = {"一致": 0, "已漂移": 0, "计数错误": 0, "锚点歧义": 0, "不可判定": 0, "已知待收口": 0}
    errs: list[str] = []
    known: list[str] = []
    unver_noanchor: set[str] = set()
    bases = {os.path.basename(f) for f, *_ in ANCHORS}
    cites, frozen_dirs = collect_citations(scan_dirs, bases)
    rawcache: dict[str, list[str]] = {}

    for path, sym, exp, note, _pats in ANCHORS:
        a_txt = read_lines(anchor, path)
        if not a_txt:
            add(f"⚠️ 锚点提交不含 {path}（跳过）"); continue
        a_real = real_lines(a_txt, sym)
        w_txt = read_lines(None, path)
        w_real = real_lines(w_txt, sym)

        add(f"── 锚点 {path} :: `{sym}`")
        add(f"   锚点提交={a_real or '无'}（冻结期望={exp}） 工作区={w_real or '无'} （{note}）")
        if len(a_real) == 1 and a_real[0] != exp:
            add(f"   ⚠️ 冻结期望 {exp} ≠ 锚点提交真实 {a_real[0]} —— 锚点表需更新（本工具以真实值为准）")
        if not a_real:
            add("   ⚠️ 锚点提交内无此符号 —— 锚点表失效，请更新")

        base = os.path.basename(path)
        checked = 0
        seen_lines: set[tuple[str, int]] = set()
        for rel, srcln, b, a, bb, col in cites:
            if b != base:
                continue
            if rel not in rawcache:
                try:
                    rawcache[rel] = open(os.path.join(REPO, rel), errors="ignore").read().split("\n")
                except OSError:
                    rawcache[rel] = []
            pats_here = [pat for f, _s, _e, _n, ps in ANCHORS if os.path.basename(f) == base for pat in ps]
            # ⚠️ 必须按 **sym**（而非仅 file）定位本锚点 —— 同一文件可有多个锚点
            mine_list = next(ps for f, sy, _e, _n, ps in ANCHORS if f == path and sy == sym)
            got_pat = nearest_symbol(rawcache[rel], srcln, col, pats_here)
            if got_pat not in mine_list:
                continue  # 该引用最近的是同一文件里的**别的**锚点（或无可辨符号）⇒ 不在此判定
            if (rel, srcln) in seen_lines:
                continue  # 同行同锚点文件只报一次（避免一符多行重复计入）
            seen_lines.add((rel, srcln))
            checked += 1
            # 对照/更正行：同一行同时给出错误值与正确值（如「`:346` … 纠正为 `:347`」）
            # ⇒ 在**引述**旧值，不是位置断言 ⇒ 不判（否则更正表本身会被当成错误写入）
            row_txt = (rawcache[rel][srcln - 1] if rawcache.get(rel) and srcln - 1 < len(rawcache[rel]) else "")
            # ⛔ 必须**同时**有「更正/对照」的显式标记，才认定为引述旧值；
            #    仅"数字同时出现"会误豁免真正的错误（实测：主报告 :242 被误放行）。
            CORRECT_MARK = ("纠正为", "更正为", "应为", "期望", "→", "原表述", "勘误", "已改为")
            # 更正表：标记可能在**表头**（如「| 原表述 | 纠正为 |」），故向上回看 8 行找表头
            ctx_rows = rawcache.get(rel, [])[max(0, srcln - 9):srcln]
            in_corr_table = any(r.lstrip().startswith("|") and any(mk in r for mk in CORRECT_MARK)
                                for r in ctx_rows)
            if a_real and (in_corr_table or any(mk in row_txt for mk in CORRECT_MARK)) \
               and any(str(x) in row_txt for x in a_real) and (str(a) in row_txt):
                continue
            if not doc_declares_anchor(os.path.join(REPO, rel)):
                unver_noanchor.add(rel)
                continue  # 未声明锚点基准的文档 ⇒ 不判「计数错误」（计入不可判定）
            group = [x for f, _s, _e, _n, _ps in ANCHORS if f == path for x in []] or None
            group = sorted({x for f, s2, _e, _n, _ps in ANCHORS if f == path
                            for x in real_lines(a_txt, s2)})
            state, detail = classify(a, bb, a_real, w_real, group)
            if state == "计数错误":
                hit = next((e for e in exc if e[0] == rel and e[1] == srcln), None)
                if hit is not None:
                    state, detail = "已知待收口", f"已登记 {hit[2]}；{detail}"
            stat[state] += 1
            if state == "计数错误":
                hit = next((e for e in exc if e[0] == rel and e[1] == srcln), None)
                if hit is not None:
                    state, detail = "已知待收口", f"已登记 {hit[2]}；{detail}"
            mark = {"一致": "✅", "已漂移": "🟡", "计数错误": "⛔", "锚点歧义": "⚠️",
                    "不可判定": "➖", "已知待收口": "📌"}[state]
            add(f"   {mark} [{state}] {rel}:{srcln}  声明 {base}:{a}{'-' + str(bb) if bb != a else ''}  →  {detail}")
            if state == "计数错误":
                errs.append(f"{rel}:{srcln} {base}:{a} → {detail}")
            elif state == "已知待收口":
                known.append(f"{rel}:{srcln} {base}:{a} → {detail}")
        add(f"   （可机械判定的引用 {checked} 处）")
        add("")

    # 不可判定清单（只计数）
    add("-" * 78)
    add("unverifiable（只有行号、附近无可辨锚点符号 ⇒ 本门禁不判定，不算失败）:")
    unver_by_base: dict[str, int] = {}
    for rel, srcln, b, a, bb, col in cites:
        if b not in unver_by_base:
            unver_by_base[b] = 0
        # 该引用是否已被某锚点判定
        pats = [pat for f, _s, _e, _n, ps in ANCHORS if os.path.basename(f) == b for pat in ps]
        row = rawcache.get(rel) or open(os.path.join(REPO, rel), errors="ignore").read().split("\n")
        if nearest_symbol(row, srcln, col, pats) is None:
            unver_by_base[b] += 1
    for b, n in sorted(unver_by_base.items()):
        add(f"   {b}: {n} 处")
    add("")

    add("=" * 78)
    add(f"汇总: 一致={stat['一致']}  已漂移={stat['已漂移']}  **计数错误={stat['计数错误']}**  "
        f"锚点歧义={stat['锚点歧义']}  大偏移不可判定={stat['不可判定']}")
    add(f"      unverifiable（不判定、不计失败）= {sum(unver_by_base.values())} 处")
    add(f"      跳过 {len(frozen_dirs)} 个冻结快照目录（逐字保留的历史版本，不参与行号一致性判定）")
    for fd in sorted(frozen_dirs):
        add(f"         - {fd}/")
    add(f"      其中「未声明锚点基准的文档」命中 = {len(unver_noanchor)} 个文档（W12 可决定是否补锚点声明）:")
    for d in sorted(unver_noanchor):
        add(f"         - {d}")
    add("-" * 78)
    if known:
        add("📌 已知待收口（已登记例外，不计失败；W12 收口后请从例外表删除对应行）:")
        for k in known:
            add(f"   - {k}")
    if errs:
        add("⛔ 计数错误清单（W12 必须人工收口；期望值见上）:")
        for e in errs:
            add(f"   - {e}")
        rc = 1
    else:
        add("✅ 无「计数错误」—— 门禁通过（「已漂移」属预期，按锚点提交复核即可，不计失败）")
        rc = 0
    add(f"清单: {out_path}")

    text = "\n".join(L)
    with open(out_path, "w") as fh:
        fh.write(text + "\n")
    return rc, text


def self_test(anchor: str) -> int:
    """三向自证：阳性须抓 / 阴性不误报 / 两类误报须排除。

    ⚠️ 注意：正/负对照目录在**调用 run() 之前不得被删除** —— 首版用
    `with tempfile.TemporaryDirectory()` 包住写入、却在 with 外调用 run，
    目录已销毁 ⇒ 扫到空集、阳性对照"未抓到"（该缺陷由自证本身暴露，已修）。
    """
    print("=" * 78)
    print("自证（阳性 / 阴性 / 误报排除）")
    print("=" * 78)
    fails: list[str] = []
    td = tempfile.mkdtemp(prefix="w12_selftest_")
    pos = os.path.join(td, "positive")
    neg = os.path.join(td, "negative")
    os.makedirs(pos)
    os.makedirs(neg)
    with open(os.path.join(pos, "doc.md"), "w") as fh:
        fh.write("锚点提交：e800ccc496ac710b711c9346709e86a148c41241\n")
        fh.write("根因位置：`src/libipc/platform/posix/udp.h:346` 的 `FD_SET(server_fd, &read_fds)` "
                 "与 `:352` 的 `::select`\n")
        fh.write("无关引用：`test/test_dzflat_builder.cpp:352` 是**别的文件**，不得计入\n")
        fh.write("不可判定：`src/libipc/ipc.cpp:461` 附近没有锚点符号，不得判定\n")
        fh.write("已用删除线处理的旧值（应放行）：~~`src/libipc/platform/posix/udp.h:346`~~ → 纠正为 `:347`\n")
    with open(os.path.join(neg, "doc.md"), "w") as fh:
        fh.write("锚点提交：e800ccc496ac710b711c9346709e86a148c41241\n")
        fh.write("根因：`src/libipc/platform/posix/udp.h:347 FD_SET` / `:353 ::select`（锚点口径）\n")
        fh.write("已修：`src/libipc/platform/posix/udp.h:345-353`（含 FD_ZERO/FD_SET/::select）\n")
    out1 = os.path.join(td, "pos.txt")
    out2 = os.path.join(td, "neg.txt")

    rc, t1 = run(anchor, [pos], out1, [], self_test_mode=True)
    n_pos = t1.count("[计数错误]")
    print(f"阳性对照: 计数错误 {n_pos} 项（期望恰 2：`:346` 与 `:352`；删除线行必须被放行）")
    if n_pos < 2:
        fails.append("阳性对照未抓到 off-by-one")
    if n_pos > 2:
        fails.append(f"阳性对照多报：删除线行（~~:346~~ 纠正为 :347）未被放行（多出 {n_pos-2} 项）")
    else:
        print("元讨论放行: ✅ 删除线引述的旧值（~~:346~~）未被计入")
    if "test_dzflat_builder.cpp" in t1.split("unverifiable")[0]:
        fails.append("误报：把 test_dzflat_builder.cpp:352 计入 udp.h")
    else:
        print("误报排除 A: ✅ `test_dzflat_builder.cpp:352` 未被计入（basename 不同）")
    if re.search(r"\[计数错误\].*ipc\.cpp", t1):
        fails.append("误报：把无锚点符号的 ipc.cpp:461 判为计数错误")
    else:
        print("误报排除 B: ✅ `ipc.cpp:461`（附近无锚点符号）未被判定，归 unverifiable")
    for ln in t1.splitlines():
        if "udp.h:346" in ln or "udp.h:352" in ln:
            print("   " + ln.strip())

    rc2, t2 = run(anchor, [neg], out2, [], self_test_mode=True)
    n_neg = t2.count("[计数错误]")
    print(f"阴性对照: 计数错误 {n_neg} 项（期望 0）")
    print("   说明：阴性对照的 :347/:353 在**锚点提交上正确**、工作区已迁 poll ⇒ 判"
          "「已漂移」是正确行为（不是误报）；本条对照只要求「不被判为计数错误」。")
    for ln in t2.splitlines():
        if "[一致]" in ln or "[已漂移]" in ln:
            print("   " + ln.strip())
    if n_neg != 0:
        fails.append("阴性对照被误报")

    print("-" * 78)
    if fails:
        print("⛔ 自证失败:")
        for f in fails:
            print(f"   - {f}")
        return 1
    print("✅ 自证通过：阳性被抓（≥2）、阴性不误报（0）、两处误报排除成立")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="跨文档代码行号一致性门禁（只报告不改写）")
    ap.add_argument("--anchor", default="e800ccc496ac710b711c9346709e86a148c41241")
    ap.add_argument("--scan", default="docs")
    ap.add_argument("--out", default="/tmp/w12_doc_line_refs.txt")
    ap.add_argument("--exceptions", default=os.path.join(REPO, "tools/w12/line_ref_exceptions.txt"),
                    help="已登记例外表（文件:行 basename id）")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(REPO, ".git")):
        print(f"⛔ 不是 git 仓库: {REPO}", file=sys.stderr); return 2
    try:
        git("rev-parse", "--verify", f"{a.anchor}^{{commit}}")
    except RuntimeError as e:
        print(f"⛔ 锚点不可用: {e}", file=sys.stderr); return 2
    if a.self_test:
        return self_test(a.anchor)
    rc, txt = run(a.anchor, [d.strip() for d in a.scan.split(",") if d.strip()], a.out,
                  load_exceptions(a.exceptions))
    print(txt); print(f"\n退出码 {rc}（0=通过；1=有计数错误需收口）")
    return rc


if __name__ == "__main__":
    sys.exit(main())
