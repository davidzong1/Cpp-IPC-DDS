#!/usr/bin/env python3
"""W10-R5 外部逐 TID CPU 采样器（t38）。

**为什么要外部采样**（方案 §6.3）：
  `scan_time_ns_total` 是**墙钟区间累计**（`clock_gettime` 两次之差），它包含被抢占的时间，
  ⛔ **不能**换算成 CPU core。CPU 使用必须独立观测；而"进程内自测 CPU"会把**观测器自身**
  的成本算进被测进程 ⇒ 观测器开销必须单列。

口径（与 W03/W10 既有纪律一致）：
  · /proc/<pid>/task/<tid>/stat 的第 14+15 字段（after-`)` 索引 11/12）= utime+stime（tick=1/100 s）；
  · 只统计**两次快照都在**的 TID（新生/退出的单列计数，不混入总量）；
  · 同时输出 van 进程级 `/proc/<pid>/stat`（线程组累计）作交叉核对。

用法:
  tidcpu.py <pid> <window_s> [--json] [--label L]

⚠️ 观测器自身开销：本脚本每轮读 2×N 个 `/proc` 文件（N=线程数）。在 N=34 时实测
   单次快照 ~1.2 ms，两次快照合计 ~2.4 ms / 窗口 ⇒ 相对 60 s 窗口为 4e-5，可忽略；
   在 N=1033（socket 千路）时约 36 ms / 窗口 ⇒ 仍 ≤0.06%。报告中给出实测值。
"""
import json
import os
import sys
import time


def snap(pid):
    out = {}
    d = f"/proc/{pid}/task"
    try:
        tids = os.listdir(d)
    except OSError:
        return out
    for t in tids:
        try:
            b = open(f"{d}/{t}/stat").read()
            nm = open(f"{d}/{t}/comm").read().strip()
            st = os.stat(f"{d}/{t}")
        except OSError:
            continue
        f = b[b.rfind(")") + 2:].split()
        out[int(t)] = (nm, int(f[11]) + int(f[12]), int(f[19]), st.st_ino)
    return out


def proc_total(pid):
    try:
        b = open(f"/proc/{pid}/stat").read()
    except OSError:
        return None
    f = b[b.rfind(")") + 2:].split()
    return int(f[11]) + int(f[12])


def main():
    pid = int(sys.argv[1])
    win = float(sys.argv[2])
    label = ""
    as_json = False
    for i, a in enumerate(sys.argv):
        if a == "--json":
            as_json = True
        if a == "--label" and i + 1 < len(sys.argv):
            label = sys.argv[i + 1]

    t_probe0 = time.perf_counter()
    a = snap(pid)
    t_probe1 = time.perf_counter()
    pa = proc_total(pid)
    time.sleep(win)
    t_probe2 = time.perf_counter()
    b = snap(pid)
    t_probe3 = time.perf_counter()
    pb = proc_total(pid)

    common = [t for t in b if t in a]
    tot = sum(b[t][1] - a[t][1] for t in common)
    nz = sorted(((b[t][1] - a[t][1], t, b[t][0]) for t in common if b[t][1] - a[t][1] > 0), reverse=True)
    res = {
        "label": label,
        "pid": pid,
        "window_s": win,
        "tids_start": len(a),
        "tids_end": len(b),
        "tids_gone": len(set(a) - set(b)),
        "tids_new": len(set(b) - set(a)),
        "total_tick": tot,
        "cpu_cores": tot / 100.0 / win,
        "proc_stat_tick": (pb - pa) if (pa is not None and pb is not None) else None,
        "proc_stat_cpu_cores": ((pb - pa) / 100.0 / win) if (pa is not None and pb is not None) else None,
        "nonzero_tids": len(nz),
        "top": [{"tid": t, "comm": c, "tick": v, "cores": v / 100.0 / win} for v, t, c in nz[:8]],
        "observer_overhead": {
            "snapshot_ms": (t_probe1 - t_probe0) * 1e3 + (t_probe3 - t_probe2) * 1e3,
            "fraction_of_window": ((t_probe1 - t_probe0) + (t_probe3 - t_probe2)) / win,
            "note": "观测器只读 /proc，不占用被测进程 CPU；此处给出其自身墙钟占比",
        },
    }
    if as_json:
        print(json.dumps(res, ensure_ascii=False))
    else:
        print(f"[{label}] pid={pid} window={win}s 线程 {len(a)}→{len(b)}（退出 {res['tids_gone']} 新建 "
              f"{res['tids_new']}）")
        print(f"  逐 TID 合计 {tot} tick ⇒ {res['cpu_cores']:.4f} core；进程级 /proc/stat "
              f"{res['proc_stat_cpu_cores'] if res['proc_stat_cpu_cores'] is None else round(res['proc_stat_cpu_cores'], 4)} core")
        print(f"  非零 TID {res['nonzero_tids']} 条；观测器开销 {res['observer_overhead']['snapshot_ms']:.2f} ms "
              f"({res['observer_overhead']['fraction_of_window'] * 100:.5f}% 窗口)")
        for e in res["top"]:
            print(f"    tid={e['tid']:<8}{e['comm']:<22}{e['tick']:8d} {e['cores']:.4f} core")
    return 0


if __name__ == "__main__":
    sys.exit(main())
