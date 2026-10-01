#!/usr/bin/env python3
"""T05 §7.3.1/§7.3.2 机械检查（可离线重放）。

判据（逐条机械可判，不依赖人工阅读）：
  A. `orphan_snap` 的 `memcpy` 是否落在 `handles_` 的 `lock_` 临界区**内部**；
  B. `memcmp(&snap, &info->pool_, …)` 复核是否在 `info->lock_` **持锁区间**内；
  C. 全文件的锁序：是否存在「已持 `info->lock_` 再取 `handles_` 的 `lock_`」的逆序嵌套；
  D. 四个实验变体相对 HEAD 的差异是否**全部**落在 reclaim 相关行区间内
     （⇒ 段名 / 容量布局 / published() 语义 / 其余路径未被触碰）。
用法：python3 check_lock_order.py [--run-dir <run 目录>]
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = "/home/zwc/cpp_ipc_dds"
SRC = os.path.join(ROOT, "src/libipc/ipc.cpp")


def read(p):
    with open(p) as f:
        return f.read().splitlines()


def lock_site_map(lines):
    """返回 [(行号, 方向, 表达式)] —— 所有对 info->lock_ / handles_ 的 lock_ 的取放。"""
    sites = []
    for i, ln in enumerate(lines, 1):
        for m in re.finditer(r"lock_guard<std::mutex>\s+guard\{lock_\}", ln):
            sites.append((i, "acquire", "handles_.lock_ (std::mutex lock_)"))
        for m in re.finditer(r"(\w+)->lock_\.lock\(\)", ln):
            sites.append((i, "acquire", "%s->lock_" % m.group(1)))
        for m in re.finditer(r"(\w+)->lock_\.unlock\(\)", ln):
            sites.append((i, "release", "%s->lock_" % m.group(1)))
    return sites


def enclosing_fn(lines, lineno):
    for i in range(lineno - 2, -1, -1):
        m = re.match(r"\s*(?:[A-Za-z_][\w:<>,\s\*&]*?)\s+([A-Za-z_]\w*)\s*\([^;]*$", lines[i])
        m2 = re.match(r"\s*(?:static\s+)?(?:[A-Za-z_][\w:<>,\s\*&]*?)\s+([A-Za-z_]\w*)\s*\(", lines[i])
        if m2 and "=" not in lines[i] and "//" not in lines[i]:
            return m2.group(1), i + 1
    return "<unknown>", -1


def block_range(lines, start_idx):
    """从 start_idx（0 基）的花括号处返回该块的行区间（1 基，闭区间）。"""
    depth = 0
    started = False
    for i in range(start_idx, len(lines)):
        for ch in lines[i]:
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
                if started and depth == 0:
                    return start_idx + 1, i + 1
    return start_idx + 1, len(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-dir", default=None)
    args = ap.parse_args()
    out = []
    ok = True
    lines = read(SRC)

    def say(s):
        print(s)
        out.append(s)

    # 定位关键行
    def find_one(pat):
        hits = [i for i, ln in enumerate(lines, 1) if re.search(pat, ln)]
        assert len(hits) == 1, "锚点 %r 命中 %d 次" % (pat, len(hits))
        return hits[0]

    l_memcpy_snap = find_one(r"std::memcpy\(&orphan_snap, &probe->pool_")
    l_guard = find_one(r"std::lock_guard<std::mutex> guard\{lock_\}")
    l_h = find_one(r"h = &\(handles_\[pref\]\);")
    l_newly = find_one(r"newly_attached = !h->valid\(\);")
    l_make = find_one(r"if \(!make_handle\(\*h, shm_name, chunk_size\)\)")
    l_pristine = find_one(r"bool const pristine = probe->pool_\.invalid\(\);")
    l_memcmp = find_one(r"if \(std::memcmp\(&snap, &info->pool_, sizeof\(snap\)\) != 0\)")
    l_reset = find_one(r"info->pool_\.reset_free_chain\(\);")
    l_guard_end = None
    for i in range(l_guard - 1, len(lines)):
        if lines[i].strip() == "}" and i + 1 > l_memcpy_snap:
            l_guard_end = i + 1
            break
    say("── A. 取快照位置（§7.2 顺序）────────────────────────────")
    say("handles_ lock_guard 行            = %d" % l_guard)
    say("临界区结束行（该 lock_guard 闭括号）= %d" % l_guard_end)
    for label, ln in (("① handles_[prefix] 获取", l_h), ("② 首次 attach 判定", l_newly),
                      ("③ 建立 handle", l_make), ("④ pristine 判定", l_pristine),
                      ("⑤ orphan_snap 取得(memcpy)", l_memcpy_snap)):
        inside = l_guard <= ln <= l_guard_end
        say("  %-26s 行 %-5d 在 handles_ 临界区内 = %s" % (label, ln, inside))
        ok &= inside
    say("  判定：①–⑤ 全部在同一 handles_ 临界区内 ⇒ %s" % ("通过" if ok else "失败"))

    # B. 复核临界区
    say("")
    say("── B. 复核临界区（info->lock_ 下的 memcmp）───────────────")
    acquire = None
    for i in range(l_memcmp - 1, 0, -1):
        if re.search(r"info->lock_\.lock\(\)", lines[i - 1]):
            acquire = i
            break
    # 复核块的**收尾** unlock = reset_free_chain 之后最近的一次 info->lock_.unlock()
    release = None
    for i in range(l_reset + 1, len(lines) + 1):
        if re.search(r"info->lock_\.unlock\(\)", lines[i - 1]):
            release = i
            break
    # memcmp 失败分支的提前 unlock（应当是同区间内的第二个 unlock）
    early = [i for i in range(l_memcmp, l_reset)
             if re.search(r"info->lock_\.unlock\(\)", lines[i - 1])]
    say("info->lock_.lock()  行 = %d" % acquire)
    say("memcmp 复核         行 = %d" % l_memcmp)
    say("memcmp 不等 ⇒ 提前 unlock 行 = %s（失败分支不写池）" % early)
    say("reset_free_chain    行 = %d" % l_reset)
    say("收尾 info->lock_.unlock() 行 = %d" % release)
    b_ok = acquire < l_memcmp < l_reset < release and len(early) == 1 and early[0] < l_reset
    say("  判定：memcmp 复核与 reset_free_chain 同在 info->lock_ 持锁区间内 ⇒ %s"
        % ("通过" if b_ok else "失败"))
    ok &= b_ok

    # C. 锁序（无逆序嵌套）
    say("")
    say("── C. 锁序：是否存在「持 info->lock_ 再取 handles_ 锁」的逆序 ──")
    sites = lock_site_map(lines)
    handles_sites = [s for s in sites if "handles_" in s[2]]
    say("handles_ 的 lock_ 取用点数量 = %d（行 %s）"
        % (len(handles_sites), [s[0] for s in handles_sites]))
    reverse = []
    for ln, kind, expr in sites:
        if kind == "acquire" and "->lock_" in expr:
            # 该 info->lock_ 持锁区间
            end = None
            for j in range(ln, len(lines) + 1):
                if re.search(r"%s\.unlock\(\)" % re.escape(expr), lines[j - 1]):
                    end = j
                    break
            if end is None:
                continue
            for hln, hkind, hexpr in handles_sites:
                if ln < hln < end:
                    reverse.append((ln, hexpr, hln))
    say("逆序嵌套命中数 = %d %s" % (len(reverse), reverse if reverse else ""))
    c_ok = not reverse
    say("  判定：锁序始终为 handles_.lock_ -> info->lock_ ⇒ %s" % ("通过" if c_ok else "失败"))
    ok &= c_ok

    # 锁外 /proc 探活
    probe_line = find_one(r"if \(scan_segment_mappers\(name\.c_str\(\)\) != seg_scan::orphaned\) return false;")
    body_start = find_one(r"bool reclaim_orphan_segment\(chunk_info_t \*info")
    # 探活必须**不在任何锁区间内**：既不在 handles_ 的 lock_guard 区间，也不在 info->lock_ 区间
    # （按源码流序判定：探活行在 handles_ 临界区之外，且不属于任何 info->lock_ 持锁区间）
    in_handles = l_guard <= probe_line <= l_guard_end
    in_info = False
    for ln, kind, expr in lock_site_map(lines):
        if kind == "acquire" and "->lock_" in expr:
            end = None
            for j in range(ln, len(lines) + 1):
                if re.search(r"%s\.unlock\(\)" % re.escape(expr), lines[j - 1]):
                    end = j
                    break
            if end and ln <= probe_line <= end:
                in_info = True
    d_ok = (not in_handles) and (not in_info) and (probe_line < acquire)
    say("")
    say("── D. /proc 探活位置（锁外）─────────────────────────")
    say("reclaim_orphan_segment 定义行 = %d；/proc 探活行 = %d；info->lock_ 取锁行 = %d"
        % (body_start, probe_line, acquire))
    say("  探活在 handles_ 临界区内 = %s；在 info->lock_ 持锁区间内 = %s；在取锁之前 = %s"
        % (in_handles, in_info, probe_line < acquire))
    say("  判定：探活在两把锁之外（锁外执行 /proc 扫描）⇒ %s" % ("通过" if d_ok else "失败"))
    ok &= d_ok

    # E. 变体差异局部性
    say("")
    say("── E. 变体差异局部性（段名/容量布局/published 语义未被触碰）──")
    # 「reclaim 相关区间」= 两段源码的并集（其余行一律不得被实验变体触碰）：
    #   ① reclaim_orphan_segment() 函数体所在行区间（含其上方紧邻的 T73 说明注释）；
    #   ② get_info() 内从 `bool orphan_candidate = false;` 到 reclaim 调用块结束。
    # ⛔ 段名构造（chunk_segment_name / CHUNK_INFO__ / __C<cap>）、容量常量（large_msg_cache）、
    #    published() 读写点、以及其余全部产品路径**都在这两段之外**。
    fn_end = find_one(r"note_orphan_reset\(kind, chunk_size, prefix\);")
    call_line = find_one(r"\(void\)reclaim_orphan_segment\(info, pref, chunk_size, \"attach\",")
    setup_line = find_one(r"bool orphan_candidate = false;")
    trailing = find_one(r"return info;")
    region = set(range(body_start - 4, fn_end + 2)) | set(range(setup_line - 12, trailing + 2))
    say("  允许的 reclaim 区间 = [%d,%d] ∪ [%d,%d]"
        % (body_start - 4, fn_end + 1, setup_line - 12, trailing + 1))
    for v in ("ABLATE", "V2NEG", "FIXNEG"):
        p = os.path.join(args.run_dir or "", "src", "ipc.cpp." + v)
        if not os.path.exists(p):
            say("  %s: 源文件缺失 ⇒ 跳过" % v)
            continue
        dl = subprocess.run(["diff", "-u", SRC, p], capture_output=True, text=True).stdout
        hunks = [h for h in dl.split("\n") if h.startswith("@@")]
        touched = set()
        for h in hunks:
            for part in re.findall(r"@@ -(\d+)(?:,(\d+))? \+", h):
                start = int(part[0]); cnt = int(part[1]) if part[1] else 1
                touched |= set(range(start, start + cnt))
        outside = sorted(touched - region)
        say("  %s: 差异 hunk %d 个，触及行数 %d，落在 reclaim 区间之外的行 = %s"
            % (v, len(hunks), len(touched), outside if outside else "无"))
        ok &= not outside
    # 段名与容量常量
    for pat in (r"CHUNK_INFO__", r"__C", r"large_msg_cache"):
        say("  段名/容量锚点 %-16s 命中 %d 处" % (pat, len(re.findall(pat, "\n".join(lines)))))

    say("")
    say("═══ 总判定：%s ═══" % ("全部通过" if ok else "存在失败项"))
    if args.run_dir:
        os.makedirs(args.run_dir, exist_ok=True)
        with open(os.path.join(args.run_dir, "lock_order_check.txt"), "w") as f:
            f.write("\n".join(out) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
