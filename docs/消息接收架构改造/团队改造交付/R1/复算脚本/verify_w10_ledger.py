#!/usr/bin/env python3
"""R1/t31 独立复算：r25-W10 逐 route 台账 / samples / counters 三方自洽（⛔ 只读）。

用法: python3 verify_w10_ledger.py [run_root]
默认 run_root = artifacts/perf/20260929-r25-W10
产出：逐条打印复算数值（非引用实现者结论）；退出码 0 = 全部自洽，1 = 发现不一致。
"""
import csv, json, os, sys, collections

ROOT = sys.argv[1] if len(sys.argv) > 1 else "artifacts/perf/20260929-r25-W10"
fails = []

def chk(name, cond, detail=""):
    print(f"  [{'OK ' if cond else 'FAIL'}] {name}{('  ' + detail) if detail else ''}")
    if not cond:
        fails.append(name)

print("=" * 78)
print(f"R1/t31 独立复算 —— run_root = {ROOT}")
print("=" * 78)

# ---------- 1) 逐 route 台账 ----------
led = os.path.join(ROOT, "shm-ind-1000", "ledger.csv")
rows = list(csv.DictReader(open(led)))
print(f"\n[1] {led}: {len(rows)} 行")
chk("行数 == 1000", len(rows) == 1000, f"实测 {len(rows)}")
names = [r["route"] for r in rows]
chk("route 唯一（无重复）", len(set(names)) == len(names), f"唯一 {len(set(names))}")
idx = sorted(int(r["route"].rsplit("_", 1)[1]) for r in rows)
chk("route 下标恰为 0..999（无遗漏）", idx == list(range(1000)), f"min={idx[0]} max={idx[-1]}")
ok = [r for r in rows if r["registered"] == "1" and r["rx"] == r["planned"]
      and r["dup"] == "0" and r["corrupt"] == "0" and r["timeout"] == "0"]
chk("全条件满足（registered=1 ∧ rx==planned ∧ dup=corrupt=timeout=0）", len(ok) == 1000,
    f"{len(ok)}/1000")
notok = [r for r in rows if r not in ok]
chk("不满足样例为空", not notok, f"不满足 {len(notok)} 条")
agg = {k: sum(int(r[k]) for r in rows) for k in ("planned", "sent", "rx", "dup", "out_of_order", "corrupt", "timeout")}
print(f"      合计: {agg}")
chk("planned == sent == rx = 3005", agg["planned"] == agg["sent"] == agg["rx"] == 3005,
    f"{agg['planned']}/{agg['sent']}/{agg['rx']}")
chk("planned 分布 = 999×3 + 1×8（首 route 含 5 条恢复消息）",
    collections.Counter(r["planned"] for r in rows) == collections.Counter({"3": 999, "8": 1}),
    str(dict(collections.Counter(r["planned"] for r in rows))))

# ---------- 2) samples.jsonl 与台账交叉 ----------
sl = os.path.join(ROOT, "shm-ind-1000", "samples.jsonl")
S = [json.loads(l) for l in open(sl)]
print(f"\n[2] {sl}: {len(S)} 行, keys={sorted(S[0].keys())}")
chk("无任何时延字段（⇒ 该 run 不含 §10.7 三组计时）",
    not any(k in S[0] for k in ("ns", "us", "latency", "t_send", "t_recv")))
dirs = collections.Counter(x["direction"] for x in S)
chk("direction = tx:rx = 3005:3005", dirs["tx"] == dirs["rx"] == 3005, str(dict(dirs)))
tx = collections.defaultdict(list); rx = collections.defaultdict(list)
for x in S:
    (tx if x["direction"] == "tx" else rx)[x["route"]].append(x["seq"])
bad = []
for r in rows:
    n = r["route"]
    if len(tx[n]) != int(r["sent"]): bad.append(("sent", n))
    if len(rx[n]) != int(r["rx"]): bad.append(("rx", n))
    if sorted(rx[n]) != list(range(int(r["rx"]))): bad.append(("seqset", n))
chk("台账 sent/rx 与 samples 逐 route 一致且序号为 0..n-1", not bad, f"不一致 {len(bad)} 项")
chk("samples 覆盖全部 1000 route", len(set(tx) | set(rx)) == 1000)

# ---------- 3) counters.json 自洽 + P0 恒等式 ----------
cj = os.path.join(ROOT, "shm-ind-1000", "counters.json")
C = json.load(open(cj))
reg = C["registry_counters"]["counters"]
print(f"\n[3] {cj}")
chk("counters: registered_count == expected_count", C["registered_count"] == C["expected_count"] == 1000)
chk("counters: valid_rx_count == expected_count", C["valid_rx_count"] == C["expected_count"] == 1000)
chk("registry: registration_attempts == registration_ok == 1000",
    reg["registration_attempts"] == reg["registration_ok"] == 1000,
    f"{reg['registration_attempts']}/{reg['registration_ok']}")
chk("registry: tlv_messages == 3005（3×1000 + 5 恢复）", reg["tlv_messages"] == 3005, str(reg["tlv_messages"]))
chk("registry: tlv_wire_bytes == 88 B/条 × 3005（64 载荷 + 24 头）",
    reg["tlv_wire_bytes"] == 88 * 3005, f"{reg['tlv_wire_bytes']} vs {88*3005}")
chk("registry: fallback_total == 0 且 wait_set_full == wait_token_invalid == 0",
    reg["fallback_total"] == 0 and reg["wait_set_full"] == 0 and reg["wait_token_invalid"] == 0)
chk("failure_classes 十类全 0 且 first_failed_resource == none",
    all(v == 0 for v in C["failure_classes"].values()) and C["first_failed_resource"] == "none")
chk("registry: diagnostics_enabled == False（⇒ scan_* 族受门控，读数不能当开诊断值）",
    C["registry_counters"]["diagnostics_enabled"] is False)

# ---------- 4) 无写入点的计数（§13.2#3 的承重点）----------
NO_WRITER = ["chunk_exhausted", "chunk_alloc_failed", "queue_evicted", "queue_backpressure",
             "generation_mismatch", "publish_blocked", "rx_timeout", "fd_limit",
             "fallback_pool_exhausted", "fallback_type_incompatible", "fallback_oversized",
             "payload_checksum_ok", "payload_checksum_bad", "seq_lost"]
print("\n[4] 本 run 读数为 0 但**产品代码无写入点**的计数（0 是「未接线」，不是「实测为 0」）:")
for k in NO_WRITER:
    print(f"      {k:32s} = {reg.get(k)}")
print("      ⇒ 判据见 verify_w10_chunk_counter.cpp（池真实耗尽而 chunk_exhausted 仍为 0）")

# ---------- 5) 其余拓扑 ----------
for arm, exp in (("shm-ind-1", 1), ("shm-ind-100", 100), ("shm-bcast-32", 32),
                 ("shm-hotcold-1000", 1000)):
    p = os.path.join(ROOT, arm, "ledger.csv")
    if not os.path.exists(p):
        chk(f"{arm}: 台账存在", False); continue
    rr = list(csv.DictReader(open(p)))
    m = json.load(open(os.path.join(ROOT, arm, "manifest.json")))
    if arm == "shm-hotcold-1000":
        cold_ok = sum(1 for r in rr[1:] if int(r["rx"]) >= 1)
        chk(f"{arm}: 行数=1000 且冷路 999/999 各收≥1 条", len(rr) == 1000 and cold_ok == 999,
            f"行数 {len(rr)} 冷路 {cold_ok}/999")
        chk(f"{arm}: 热路 route0 planned={rr[0]['planned']} sent={rr[0]['sent']} rx={rr[0]['rx']}"
            "（sent 列仅在定速臂有意义 ⇒ 与 rx 不可比）", True)
    else:
        allok = sum(1 for r in rr if r["registered"] == "1" and r["rx"] == r["planned"])
        chk(f"{arm}: 行数={exp} 且全条件满足 {exp}/{exp}", len(rr) == exp and allok == exp,
            f"行数 {len(rr)} 满足 {allok}")

# ---------- 6) verdict.md 条件 6 的实测单元格 ----------
v = open(os.path.join(ROOT, "shm-ind-1000", "verdict.md")).read()
cell6 = [l for l in v.splitlines() if l.startswith("| 6 |")]
print("\n[5] verdict.md §13.2 第 6 条实测单元格（原文）:")
print("      " + (cell6[0] if cell6 else "(缺)"))
c6_measured = bool(cell6) and "token" not in cell6[0].split("|")[2]
print(f"      ⇒ 单元格仅含 route/fd ⇒ §13.2 第 6 条的 token/queue/chunk 三项**未被测量**"
      f"（{'确认' if c6_measured else '未确认'}；作为 finding 记录，不计入本脚本的自洽判定）")

print("\n" + "=" * 78)
if fails:
    print(f"⛔ 不一致 {len(fails)} 项: {fails}")
    sys.exit(1)
print("✅ 台账 / samples / counters / 其余拓扑 全部自洽（复算数值见上）")
print("⚠️ 但 §13.2 第 3 条（独立计数）与第 6 条（token/queue/chunk 回基线）不满足——见 verdict 文档")
