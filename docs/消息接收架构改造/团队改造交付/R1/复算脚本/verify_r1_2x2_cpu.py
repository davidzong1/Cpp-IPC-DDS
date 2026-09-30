#!/usr/bin/env python3
"""t32 独立复算：4 格 2×2（调度器开关 × 接收池开关）的进程级 CPU。
⛔ 用**进程级** /proc/<pid>/stat 的 utime+stime（不逐 tid，避免逐 tid 读数器的偏差），
并额外用 /proc/<pid>/task 计数线程数；窗口 12 s，每格 3 轮取中位与极值。"""
import os, subprocess, sys, time, statistics, signal

def proc_cpu(pid):
    b = open(f"/proc/{pid}/stat").read()
    f = b[b.rfind(")")+2:].split()
    return int(f[11]) + int(f[12])          # utime + stime (jiffies, 进程级含全部线程)

def nthreads(pid):
    try: return len(os.listdir(f"/proc/{pid}/task"))
    except OSError: return 0

CELLS = [
    ("A 调度器ON  池ON ", []),
    ("B 调度器OFF 池ON ", ["DZIPC_SHM_CONTROL_SCHEDULER=1"]),
    ("C 调度器ON  池OFF", ["DZIPC_SHM_RECV_COMPAT=1"]),
    ("D 调度器OFF 池OFF", ["DZIPC_SHM_CONTROL_SCHEDULER=1", "DZIPC_SHM_RECV_COMPAT=1"]),
]
WIN = 12.0
print(f"# 进程级 CPU（utime+stime 全线程），窗口 {WIN}s，每格 3 轮（state=3, n=1000）")
print(f"{'格':18s} {'轮':>2s} {'pool/route':>14s} {'threads':>8s} {'cpu(core)':>10s} {'wakes/s':>9s} {'ctx/s(工具)':>12s}")
res = {}
for label, envs in CELLS:
    cpu, thr = [], []
    for r in range(3):
        env = dict(os.environ)
        for e in envs:
            k, v = e.split("="); env[k] = v
        env.pop("T32A", None)
        dom = 50000 + abs(hash((label, r))) % 4000
        p = subprocess.Popen(["build/t32/w10_idle", "--n", "1000", "--domain", str(dom),
                              "--state", "3", "--windows", "1", "--win-s", str(WIN)],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
        time.sleep(14)                       # 等注册+握手完成并进入稳态
        if p.poll() is not None:
            print(f"{label} 第{r+1}轮: 进程提前退出 rc={p.returncode}"); continue
        c0, t0 = proc_cpu(p.pid), nthreads(p.pid)
        time.sleep(WIN)
        c1, t1 = proc_cpu(p.pid), nthreads(p.pid)
        core = (c1 - c0) / 100.0 / WIN
        try: p.kill()
        except Exception: pass
        out = p.stdout.read()
        pool = "n/a"; wakes = "n/a"; ctx = "n/a"
        for line in out.splitlines():
            if line.startswith("state=3"):
                pool = " ".join(x for x in line.split() if x.startswith(("pool","route_count")))
            if line.startswith("   scheduler"):
                wakes = line.split("tick_wakes_per_s=")[1].split()[0]
            if line.startswith("0,3,"):
                ctx = line.split(",")[5]
        thr.append(t0); cpu.append(core)
        print(f"{label} {r+1:>2d} {pool[:14]:>14s} {t0:>8d} {core:>10.4f} {wakes:>9s} {ctx:>12s}")
    if cpu:
        res[label] = (statistics.median(cpu), min(cpu), max(cpu), statistics.median(thr))
print()
print(f"{'格':18s} {'cpu中位':>9s} {'min':>8s} {'max':>8s} {'线程中位':>9s}")
for k, (m, lo, hi, t) in res.items():
    print(f"{k:18s} {m:9.4f} {lo:8.4f} {hi:8.4f} {t:9.0f}")
