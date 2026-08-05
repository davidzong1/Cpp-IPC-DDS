#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dzIPC 性能测试结果绘图 / 报告生成

用法:
    python3 scripts/plot_dzipc_perf.py <结果目录>

<结果目录> 由 dzipc_perf_benchmark 生成, 需包含:
    results.csv      每个用例一行的全部指标
    results.json     指标 + 硬件配置
    samples/*.csv    每个用例的延迟样本(已排序、下采样), 用于画 CDF

输出(写回同一目录的 charts/ 子目录):
    01_pubsub_latency_vs_payload.png   pub-sub 单向时延 vs payload
    02_pubsub_throughput.png           pub-sub 吞吐 (msg/s 与 MB/s) vs payload
    03_sercli_rtt_vs_payload.png       ser-cli 往返时延 vs payload
    04_latency_cdf.png                 延迟 CDF (SHM vs SOCKET)
    05_loss_and_cpu.png                丢包率与 CPU 占用
    06_transport_comparison.png        传输方式横向对比
    report.md                          含硬件配置与全部数据表的报告
"""

import csv
import json
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter
except ImportError:
    sys.stderr.write(
        "需要 matplotlib: pip3 install matplotlib\n")
    sys.exit(1)

# 图表标签统一用英文, 避免缺少中文字体时出现方块;
# 中文说明放在 report.md 中。
plt.rcParams["figure.dpi"] = 130
plt.rcParams["savefig.dpi"] = 130
plt.rcParams["axes.grid"] = True
plt.rcParams["grid.alpha"] = 0.3
plt.rcParams["font.size"] = 9

MARKERS = {"shm": "o", "socket": "s"}
TRANSPORT_LABEL = {"shm": "SHM", "socket": "UDP socket"}


# --------------------------------------------------------------------------
# 数据读取
# --------------------------------------------------------------------------

def load_results(result_dir):
    csv_path = os.path.join(result_dir, "results.csv")
    if not os.path.isfile(csv_path):
        sys.stderr.write("not found: %s\n" % csv_path)
        sys.exit(1)

    rows = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            rec = dict(row)
            for k, v in list(rec.items()):
                if k in ("case_id", "pattern", "transport", "test_kind", "note"):
                    continue
                try:
                    rec[k] = float(v)
                except (TypeError, ValueError):
                    rec[k] = 0.0
            rec["payload_bytes"] = int(rec["payload_bytes"])
            rec["ok"] = bool(rec.get("ok", 0))
            rows.append(rec)
    return rows


def load_hardware(result_dir):
    json_path = os.path.join(result_dir, "results.json")
    if not os.path.isfile(json_path):
        return {}, {}
    with open(json_path, encoding="utf-8") as f:
        doc = json.load(f)
    return doc.get("hardware", {}), doc.get("config", {})


def select(rows, **kw):
    out = []
    for r in rows:
        if all(r.get(k) == v for k, v in kw.items()):
            out.append(r)
    return sorted(out, key=lambda r: r["payload_bytes"])


def transports_present(rows, pattern, kind):
    seen = []
    for r in rows:
        if r["pattern"] == pattern and r["test_kind"] == kind and r["ok"]:
            if r["transport"] not in seen:
                seen.append(r["transport"])
    return seen


def fmt_bytes(n):
    n = int(n)
    if n >= 1024 * 1024:
        return "%dM" % (n // (1024 * 1024))
    if n >= 1024:
        return "%dK" % (n // 1024)
    return "%dB" % n


def apply_payload_axis(ax, payloads):
    ax.set_xscale("log", base=2)
    ax.set_xticks(payloads)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: fmt_bytes(v)))
    ax.set_xlabel("Payload size")


def si(v, _=None):
    if v >= 1e6:
        return "%.1fM" % (v / 1e6)
    if v >= 1e3:
        return "%.0fk" % (v / 1e3)
    return "%.0f" % v


# --------------------------------------------------------------------------
# 图 1: pub-sub 单向时延 vs payload
# --------------------------------------------------------------------------

def plot_pubsub_latency(rows, out_dir):
    tr = transports_present(rows, "pubsub", "lat")
    if not tr:
        return None

    fig, axes = plt.subplots(1, len(tr) + 1, figsize=(5.2 * (len(tr) + 1), 4.2))
    if len(tr) + 1 == 1:
        axes = [axes]

    all_payloads = set()

    for ax, t in zip(axes, tr):
        data = [r for r in select(rows, pattern="pubsub", transport=t, test_kind="lat") if r["ok"]]
        if not data:
            continue
        x = [r["payload_bytes"] for r in data]
        all_payloads.update(x)
        for key, label in (("lat_p50_us", "p50"), ("lat_p99_us", "p99"),
                           ("lat_p999_us", "p99.9"), ("lat_max_us", "max")):
            ax.plot(x, [r[key] for r in data], marker=MARKERS.get(t, "o"),
                    linewidth=1.4, markersize=4, label=label)
        ax.set_yscale("log")
        ax.set_ylabel("One-way latency (us)")
        ax.set_title("pub-sub one-way latency - %s\n(paced send, tail percentiles)" % TRANSPORT_LABEL.get(t, t))
        apply_payload_axis(ax, sorted(set(x)))
        ax.legend(fontsize=8)

    # 最后一栏: 两种传输的 p50 / p99 直接对比
    ax = axes[-1]
    for t in tr:
        data = [r for r in select(rows, pattern="pubsub", transport=t, test_kind="lat") if r["ok"]]
        if not data:
            continue
        x = [r["payload_bytes"] for r in data]
        all_payloads.update(x)
        ax.plot(x, [r["lat_p50_us"] for r in data], marker=MARKERS.get(t, "o"),
                linewidth=1.6, markersize=4, label="%s p50" % TRANSPORT_LABEL.get(t, t))
        ax.plot(x, [r["lat_p99_us"] for r in data], marker=MARKERS.get(t, "o"),
                linestyle="--", linewidth=1.2, markersize=4, label="%s p99" % TRANSPORT_LABEL.get(t, t))
    ax.set_yscale("log")
    ax.set_ylabel("One-way latency (us)")
    ax.set_title("pub-sub latency: SHM vs socket")
    apply_payload_axis(ax, sorted(all_payloads))
    ax.legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(out_dir, "01_pubsub_latency_vs_payload.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 2: pub-sub 吞吐
# --------------------------------------------------------------------------

def plot_pubsub_throughput(rows, out_dir):
    kind = "tput" if transports_present(rows, "pubsub", "tput") else "lat"
    tr = transports_present(rows, "pubsub", kind)
    if not tr:
        return None

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    all_payloads = set()

    for t in tr:
        data = [r for r in select(rows, pattern="pubsub", transport=t, test_kind=kind) if r["ok"]]
        if not data:
            continue
        x = [r["payload_bytes"] for r in data]
        all_payloads.update(x)
        axes[0].plot(x, [r["throughput_msg_s"] for r in data], marker=MARKERS.get(t, "o"),
                     linewidth=1.6, markersize=4, label=TRANSPORT_LABEL.get(t, t))
        axes[1].plot(x, [r["throughput_MB_s"] for r in data], marker=MARKERS.get(t, "o"),
                     linewidth=1.6, markersize=4, label=TRANSPORT_LABEL.get(t, t))

    axes[0].set_yscale("log")
    axes[0].set_ylabel("Goodput (messages/s, received)")
    axes[0].yaxis.set_major_formatter(FuncFormatter(si))
    axes[0].set_title("pub-sub message rate vs payload\n(%s)" %
                      ("unpaced, max rate" if kind == "tput" else "paced"))
    apply_payload_axis(axes[0], sorted(all_payloads))
    axes[0].legend(fontsize=8)

    axes[1].set_yscale("log")
    axes[1].set_ylabel("Goodput (MB/s, received)")
    axes[1].set_title("pub-sub bandwidth vs payload")
    apply_payload_axis(axes[1], sorted(all_payloads))
    axes[1].legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(out_dir, "02_pubsub_throughput.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 3: ser-cli RTT
# --------------------------------------------------------------------------

def plot_sercli(rows, out_dir):
    tr = transports_present(rows, "sercli", "rtt")
    if not tr:
        return None

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    all_payloads = set()

    for t in tr:
        data = [r for r in select(rows, pattern="sercli", transport=t, test_kind="rtt") if r["ok"]]
        if not data:
            continue
        x = [r["payload_bytes"] for r in data]
        all_payloads.update(x)
        m = MARKERS.get(t, "o")
        lbl = TRANSPORT_LABEL.get(t, t)
        axes[0].plot(x, [r["lat_p50_us"] for r in data], marker=m, linewidth=1.6,
                     markersize=4, label="%s p50" % lbl)
        axes[0].plot(x, [r["lat_p99_us"] for r in data], marker=m, linestyle="--",
                     linewidth=1.2, markersize=4, label="%s p99" % lbl)
        axes[1].plot(x, [r["throughput_msg_s"] for r in data], marker=m,
                     linewidth=1.6, markersize=4, label=lbl)

    axes[0].set_yscale("log")
    axes[0].set_ylabel("Round-trip latency (us)")
    axes[0].set_title("ser-cli request/response RTT vs payload")
    apply_payload_axis(axes[0], sorted(all_payloads))
    axes[0].legend(fontsize=8)

    axes[1].set_yscale("log")
    axes[1].set_ylabel("Requests/s (synchronous, back-to-back)")
    axes[1].yaxis.set_major_formatter(FuncFormatter(si))
    axes[1].set_title("ser-cli request rate vs payload")
    apply_payload_axis(axes[1], sorted(all_payloads))
    axes[1].legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(out_dir, "03_sercli_rtt_vs_payload.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 4: 延迟 CDF
# --------------------------------------------------------------------------

def load_samples(result_dir, case_id):
    path = os.path.join(result_dir, "samples", case_id + ".csv")
    if not os.path.isfile(path):
        return []
    vals = []
    with open(path, encoding="utf-8") as f:
        next(f, None)   # header
        for line in f:
            line = line.strip()
            if line:
                try:
                    vals.append(float(line))
                except ValueError:
                    pass
    vals.sort()
    return vals


def plot_cdf(rows, result_dir, out_dir):
    payloads = sorted({r["payload_bytes"] for r in rows if r["ok"]})
    if not payloads:
        return None
    # 选 3 个有代表性的 payload: 最小、中间、最大
    picks = [payloads[0], payloads[len(payloads) // 2], payloads[-1]]
    picks = sorted(set(picks))

    panels = [("pubsub", "lat", "pub-sub one-way"), ("sercli", "rtt", "ser-cli RTT")]
    panels = [p for p in panels if transports_present(rows, p[0], p[1])]
    if not panels:
        return None

    fig, axes = plt.subplots(len(panels), len(picks),
                             figsize=(4.6 * len(picks), 3.8 * len(panels)),
                             squeeze=False)

    for pi, (pattern, kind, title) in enumerate(panels):
        for xi, payload in enumerate(picks):
            ax = axes[pi][xi]
            plotted = False
            for t in transports_present(rows, pattern, kind):
                match = [r for r in rows
                         if r["pattern"] == pattern and r["transport"] == t
                         and r["test_kind"] == kind and r["payload_bytes"] == payload and r["ok"]]
                if not match:
                    continue
                vals = load_samples(result_dir, match[0]["case_id"])
                if not vals:
                    continue
                n = len(vals)
                y = [100.0 * (i + 1) / n for i in range(n)]
                ax.plot(vals, y, linewidth=1.5, label=TRANSPORT_LABEL.get(t, t))
                plotted = True
            ax.set_xscale("log")
            ax.set_ylim(0, 100)
            ax.set_xlabel("Latency (us)")
            ax.set_ylabel("Percentile (%)")
            ax.set_title("%s CDF - %s" % (title, fmt_bytes(payload)))
            if plotted:
                ax.legend(fontsize=8)
            else:
                ax.text(0.5, 0.5, "no data", ha="center", va="center", transform=ax.transAxes)

    fig.tight_layout()
    path = os.path.join(out_dir, "04_latency_cdf.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 5: 丢包率与 CPU
# --------------------------------------------------------------------------

def plot_loss_cpu(rows, out_dir):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    all_payloads = set()
    any_data = False

    for pattern, kind in (("pubsub", "tput"), ("pubsub", "lat"), ("sercli", "rtt")):
        for t in transports_present(rows, pattern, kind):
            data = [r for r in select(rows, pattern=pattern, transport=t, test_kind=kind) if r["ok"]]
            if not data:
                continue
            any_data = True
            x = [r["payload_bytes"] for r in data]
            all_payloads.update(x)
            lbl = "%s/%s %s" % (pattern, kind, TRANSPORT_LABEL.get(t, t))
            axes[0].plot(x, [r["loss_pct"] for r in data], marker=MARKERS.get(t, "o"),
                         linewidth=1.4, markersize=4, label=lbl)
            axes[1].plot(x, [r["cpu_cores"] for r in data], marker=MARKERS.get(t, "o"),
                         linewidth=1.4, markersize=4, label=lbl)

    if not any_data:
        plt.close(fig)
        return None

    axes[0].set_ylabel("Message loss (%)")
    axes[0].set_title("Loss rate vs payload\n(sent by publisher but not delivered)")
    apply_payload_axis(axes[0], sorted(all_payloads))
    axes[0].legend(fontsize=7)

    axes[1].set_ylabel("CPU (cores, tx + rx process)")
    axes[1].set_title("CPU consumption vs payload")
    apply_payload_axis(axes[1], sorted(all_payloads))
    axes[1].legend(fontsize=7)

    fig.tight_layout()
    path = os.path.join(out_dir, "05_loss_and_cpu.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 6: 传输方式横向对比
# --------------------------------------------------------------------------

def plot_comparison(rows, out_dir):
    ok_rows = [r for r in rows if r["ok"]]
    if not ok_rows:
        return None
    payloads = sorted({r["payload_bytes"] for r in ok_rows})
    # 取中间偏小的一个 payload 做对比, 优先 1KB
    target = 1024 if 1024 in payloads else payloads[len(payloads) // 2]

    combos = []
    for pattern, kind in (("pubsub", "lat"), ("sercli", "rtt")):
        for t in ("shm", "socket"):
            match = [r for r in ok_rows if r["pattern"] == pattern and r["transport"] == t
                     and r["test_kind"] == kind and r["payload_bytes"] == target]
            if match:
                combos.append(("%s\n%s" % (pattern, TRANSPORT_LABEL.get(t, t)), match[0]))
    if not combos:
        return None

    labels = [c[0] for c in combos]
    pos = range(len(combos))

    fig, axes = plt.subplots(1, 3, figsize=(14, 4.2))

    p50 = [c[1]["lat_p50_us"] for c in combos]
    p99 = [c[1]["lat_p99_us"] for c in combos]
    w = 0.38
    axes[0].bar([p - w / 2 for p in pos], p50, w, label="p50")
    axes[0].bar([p + w / 2 for p in pos], p99, w, label="p99")
    axes[0].set_yscale("log")
    axes[0].set_ylabel("Latency (us)")
    axes[0].set_title("Latency @ %s\n(pub-sub = one-way, ser-cli = RTT)" % fmt_bytes(target))
    axes[0].set_xticks(list(pos))
    axes[0].set_xticklabels(labels, fontsize=8)
    axes[0].legend(fontsize=8)

    axes[1].bar(list(pos), [c[1]["throughput_msg_s"] for c in combos])
    axes[1].set_yscale("log")
    axes[1].set_ylabel("Messages/s")
    axes[1].yaxis.set_major_formatter(FuncFormatter(si))
    axes[1].set_title("Message rate @ %s" % fmt_bytes(target))
    axes[1].set_xticks(list(pos))
    axes[1].set_xticklabels(labels, fontsize=8)

    axes[2].bar(list(pos), [c[1]["jitter_stddev_us"] for c in combos])
    axes[2].set_yscale("log")
    axes[2].set_ylabel("Jitter (stddev, us)")
    axes[2].set_title("Latency jitter @ %s" % fmt_bytes(target))
    axes[2].set_xticks(list(pos))
    axes[2].set_xticklabels(labels, fontsize=8)

    fig.tight_layout()
    path = os.path.join(out_dir, "06_transport_comparison.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 图 7: 分片丢失诊断 (阶段 0)
# --------------------------------------------------------------------------

GAP_LABELS = ["1", "2-5", "6-15", "16-31", "32-63", "64-255", "256+"]
GAP_COLS = ["frag_gap_1", "frag_gap_2_5", "frag_gap_6_15", "frag_gap_16_31",
            "frag_gap_32_63", "frag_gap_64_255", "frag_gap_256p"]


def plot_fragment_diag(rows, out_dir):
    """分片级丢失诊断。

    左: 每片丢失率 vs payload —— 若各 payload 下基本是一条水平线, 说明消息级
        丢包完全由"分片数连乘"放大而来, 而非大包本身更容易丢。
    右: 空洞长度分布 —— 区分随机丢失(集中在 gap=1)与突发溢出(长尾)。
        这是选择对策的关键: 随机丢失是链路质量问题, 突发溢出得靠扩缓冲/限速。
        实测本项目属于后者(255 片连续丢失), 正解是调大 net.core.rmem_max。
    """
    data = [r for r in rows if r.get("frag_expected", 0) > 0]
    if not data:
        return None

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))

    by_series = {}
    for r in data:
        key = "%s/%s %s" % (r["pattern"], r["test_kind"],
                            TRANSPORT_LABEL.get(r["transport"], r["transport"]))
        by_series.setdefault(key, []).append(r)

    all_payloads = set()
    for key in sorted(by_series):
        series = sorted(by_series[key], key=lambda r: r["payload_bytes"])
        x = [r["payload_bytes"] for r in series]
        all_payloads.update(x)
        axes[0].plot(x, [r.get("frag_loss_pct", 0.0) for r in series],
                     marker="o", linewidth=1.4, markersize=4, label=key)

    axes[0].set_ylabel("Per-fragment loss (%)")
    axes[0].set_title("Per-fragment loss vs payload\n(flat line => msg loss is fragment-count amplification)")
    apply_payload_axis(axes[0], sorted(all_payloads))
    axes[0].legend(fontsize=7)

    # 右图: 把所有用例的空洞直方图累加, 按 payload 分组画堆叠柱
    payloads = sorted(all_payloads)
    bottoms = [0.0] * len(payloads)
    cmap = plt.get_cmap("viridis")
    drew = False
    for bi, (col, lbl) in enumerate(zip(GAP_COLS, GAP_LABELS)):
        vals = []
        for p in payloads:
            vals.append(sum(r.get(col, 0) for r in data if r["payload_bytes"] == p))
        if any(vals):
            drew = True
        axes[1].bar(range(len(payloads)), vals, bottom=bottoms,
                    color=cmap(bi / max(1, len(GAP_COLS) - 1)),
                    label="gap %s" % lbl)
        bottoms = [b + v for b, v in zip(bottoms, vals)]

    axes[1].set_ylabel("Gap occurrences")
    axes[1].set_title("Missing-fragment run-length distribution\n(gap=1 random, long tail => burst overflow)")
    axes[1].set_xticks(range(len(payloads)))
    axes[1].set_xticklabels([fmt_bytes(p) for p in payloads], fontsize=8)
    if drew:
        axes[1].legend(fontsize=7)

    fig.tight_layout()
    path = os.path.join(out_dir, "07_fragment_diagnosis.png")
    fig.savefig(path)
    plt.close(fig)
    return path


# --------------------------------------------------------------------------
# 报告
# --------------------------------------------------------------------------

HW_LABEL = {
    "test_time": "测试时间", "hostname": "主机名", "os": "操作系统", "kernel": "内核版本",
    "arch": "架构", "distro": "发行版", "cpu_model": "CPU 型号", "cpu_vendor": "CPU 厂商",
    "cpu_logical_cores": "逻辑核心数", "cpu_physical_cores": "物理核心数",
    "cpu_mhz_now": "当前主频 (MHz)", "cpu_mhz_max": "最大主频 (MHz)",
    "cpu_governor": "调频策略", "cpu_cache": "CPU 缓存", "cpu_flags": "相关指令集",
    "mem_total": "内存总量", "mem_available": "可用内存", "hugepages_thp": "透明大页",
    "dev_shm_size": "/dev/shm 容量", "net_rmem_max": "net.core.rmem_max",
    "net_wmem_max": "net.core.wmem_max", "net_rmem_default": "net.core.rmem_default",
    "net_netdev_backlog": "netdev_max_backlog", "kernel_cmdline": "内核启动参数",
    "isolcpus": "CPU 隔离", "loadavg_1min_at_start": "起始 1 分钟负载",
    "compiler": "编译器", "build_type": "构建类型", "cxx_standard": "C++ 标准",
    "nodelet_fast_path": "Nodelet 快速路径", "process_model": "进程模型",
}


def write_report(result_dir, rows, hardware, config, charts):
    lines = []
    a = lines.append

    a("# dzIPC 通信性能测试报告\n")
    a("测试对象: dzIPC 的 **pub-sub** 与 **ser-cli** 两种通信模式, "
      "分别跑在 **SHM(共享内存)** 和 **SOCKET(UDP)** 两种传输之上, 共 4 种组合。\n")

    a("\n## 1. 测试环境\n")
    a("| 项目 | 值 |")
    a("| --- | --- |")
    for k, v in hardware.items():
        if not v:
            continue
        a("| %s | %s |" % (HW_LABEL.get(k, k), str(v).replace("|", "\\|")))

    if config:
        a("\n## 2. 测试参数\n")
        a("| 参数 | 值 |")
        a("| --- | --- |")
        cfg_label = {
            "duration_s": "每用例测量时长 (s)", "warmup_s": "每用例预热时长 (s)",
            "latency_rate_hz": "时延用例发送速率 (Hz)", "subscriber_queue_size": "订阅端队列深度",
            "domain_id": "domain id", "publish_mode": "发送语义",
            "publish_timeout_ms": "publish_blocking 超时 (ms, 仅 SHM)",
            "pin_tx_cpu": "发送端绑核", "pin_rx_cpu": "接收端绑核",
        }
        for k, v in config.items():
            a("| %s | %s |" % (cfg_label.get(k, k), v))

    a("\n## 3. 测试方法\n")
    a("- **进程模型**: 每个用例 `fork()` 出独立的对端进程, 收发分属两个进程, 测的是真实跨进程 IPC; "
      "`Nodelet` 同进程快速路径已显式关闭。")
    a("- **pub-sub 时延**: 发布端把 `steady_clock` 时间戳打进消息, 订阅端收到后与本地时钟作差, "
      "得到**单向时延**。两个进程共用同一个 `CLOCK_MONOTONIC`, 因此可直接比较。")
    a("- **ser-cli 时延**: 由客户端本地测量一次 `send_request()` 的**往返时延 (RTT)**, 不依赖时钟同步。")
    a("- **两类 pub-sub 用例**:")
    a("  - `lat`: 按固定速率发送, 队列不堆积, 反映链路本身的时延分布。")
    a("  - `tput`: 不限速全速发送, 反映吞吐上限; 此时时延包含排队, 不代表链路时延。")
    a("- **ser-cli** 是同步请求-响应, 背靠背发送, 单次运行同时给出 RTT 分布和 req/s。")
    a("- 每个用例先 warmup 再测量; 预热消息时间戳置 0, 接收端直接丢弃, 不进入统计。")
    a("- **吞吐** 按接收端实际收到的消息数计算 (goodput), 不是发送端的调用次数。")
    a("- **丢包率** = (发送数 - 接收数) / 发送数; pub-sub 默认走 best-effort 语义, 队列满时会丢。")
    a("- **CPU** 为发送端进程与接收端进程 user+sys 时间之和除以墙钟时间, 单位是「核」。")
    a("- `bytes_per_op`: pub-sub 为 payload 大小; ser-cli 为 2×payload(请求与响应各搬运一次)。")

    if charts:
        a("\n## 4. 性能曲线\n")
        titles = {
            "01_pubsub_latency_vs_payload.png": "pub-sub 单向时延 vs payload(含 p50/p99/p99.9/max 尾时延)",
            "02_pubsub_throughput.png": "pub-sub 吞吐 vs payload(消息速率与带宽)",
            "03_sercli_rtt_vs_payload.png": "ser-cli 往返时延与请求速率 vs payload",
            "04_latency_cdf.png": "时延累积分布 (CDF): SHM vs SOCKET",
            "05_loss_and_cpu.png": "丢包率与 CPU 占用 vs payload",
            "06_transport_comparison.png": "传输方式横向对比",
        }
        for c in charts:
            name = os.path.basename(c)
            a("\n### %s\n" % titles.get(name, name))
            a("![%s](charts/%s)\n" % (name, name))

    a("\n## 5. 完整测试数据\n")

    def table(subset, latency_header):
        out = []
        out.append("| payload | 发送 | 接收 | 丢包率 | 吞吐 (msg/s) | 吞吐 (MB/s) | %s min | p50 | p90 | p99 | p99.9 | max | 抖动σ | CPU(核) |"
                   % latency_header)
        out.append("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for r in subset:
            if not r["ok"]:
                out.append("| %s | - | - | - | - | - | - | - | - | - | - | - | - | - |" % fmt_bytes(r["payload_bytes"]))
                continue
            out.append("| %s | %d | %d | %.2f%% | %s | %.1f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |" % (
                fmt_bytes(r["payload_bytes"]), int(r["sent"]), int(r["recv"]), r["loss_pct"],
                si(r["throughput_msg_s"]), r["throughput_MB_s"],
                r["lat_min_us"], r["lat_p50_us"], r["lat_p90_us"], r["lat_p99_us"],
                r["lat_p999_us"], r["lat_max_us"], r["jitter_stddev_us"], r["cpu_cores"]))
        return out

    sections = [
        ("pubsub", "lat", "5.1 pub-sub / SHM - 定速时延", "shm", "单向时延(us)"),
        ("pubsub", "lat", "5.2 pub-sub / SOCKET - 定速时延", "socket", "单向时延(us)"),
        ("pubsub", "tput", "5.3 pub-sub / SHM - 全速吞吐", "shm", "带排队时延(us)"),
        ("pubsub", "tput", "5.4 pub-sub / SOCKET - 全速吞吐", "socket", "带排队时延(us)"),
        ("sercli", "rtt", "5.5 ser-cli / SHM - 请求响应", "shm", "RTT(us)"),
        ("sercli", "rtt", "5.6 ser-cli / SOCKET - 请求响应", "socket", "RTT(us)"),
    ]
    for pattern, kind, title, transport, lat_hdr in sections:
        subset = select(rows, pattern=pattern, transport=transport, test_kind=kind)
        if not subset:
            continue
        a("\n### %s\n" % title)
        lines.extend(table(subset, lat_hdr))

    failed = [r for r in rows if not r["ok"]]
    if failed:
        a("\n## 6. 失败用例\n")
        a("| 用例 | 原因 |")
        a("| --- | --- |")
        for r in failed:
            a("| %s | %s |" % (r["case_id"], r.get("note", "")))

    a("\n---\n")
    a("原始数据: `results.csv` / `results.json`; 延迟样本: `samples/`; 硬件配置: `hardware.txt`\n")

    path = os.path.join(result_dir, "report.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return path


# --------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(1)
    result_dir = sys.argv[1].rstrip("/")
    if not os.path.isdir(result_dir):
        sys.stderr.write("directory does not exist: %s\n" % result_dir)
        sys.exit(1)

    rows = load_results(result_dir)
    hardware, config = load_hardware(result_dir)

    charts_dir = os.path.join(result_dir, "charts")
    os.makedirs(charts_dir, exist_ok=True)

    charts = []
    for fn in (plot_pubsub_latency, plot_pubsub_throughput, plot_sercli):
        p = fn(rows, charts_dir)
        if p:
            charts.append(p)
    p = plot_cdf(rows, result_dir, charts_dir)
    if p:
        charts.append(p)
    for fn in (plot_loss_cpu, plot_comparison):
        p = fn(rows, charts_dir)
        if p:
            charts.append(p)
    p = plot_fragment_diag(rows, charts_dir)
    if p:
        charts.append(p)

    charts.sort(key=lambda c: os.path.basename(c))
    report = write_report(result_dir, rows, hardware, config, charts)

    print("generated %d charts:" % len(charts))
    for c in charts:
        print("  " + c)
    print("report: " + report)


if __name__ == "__main__":
    main()
