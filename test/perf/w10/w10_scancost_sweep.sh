#!/bin/bash
# W06/t30 —— §10.2 扫描计数随 route 数 × N_worker 的批量采集（证据目录只追加）。
#
# 用法: bash test/perf/w10/w10_scancost_sweep.sh <out_root>
#   <out_root> 形如 artifacts/perf/20260929-r26-W06-scan
# ⛔ 已存在且非空即拒绝（方案 §12）：重跑请换 run_id。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
OUT=${1:?usage: w10_scancost_sweep.sh <out_root>}
if [ -d "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
  echo "REFUSE: $OUT 非空（只追加）"; exit 2
fi
mkdir -p "$OUT"
BIN=build/bin/w10_scancost

# 指纹（方案 §10.6：防陈旧 build 假绿）
{
  echo "# t30 §10.2 扫描计数采集前置（$(date '+%Y-%m-%d %H:%M:%S %z')）"
  echo "## git"; git rev-parse HEAD; git status --short -- src include test
  echo "## 库与工装指纹"; sha256sum build/lib/libipc.so.1.3.0 build/bin/w10_scancost
  echo "## 实际加载库"; ldd build/bin/w10_scancost | grep ipc
  echo "## 继承环境变量清理声明"
  echo "DZIPC_SHM_RECV_COMPAT=${DZIPC_SHM_RECV_COMPAT:-<unset>} DZIPC_SHM_RECV_WORKERS=${DZIPC_SHM_RECV_WORKERS:-<unset>}"
} > "$OUT/fingerprint.txt"
unset DZIPC_SHM_RECV_COMPAT DZIPC_SHM_RECV_WORKERS DZIPC_SHM_RECV_BUDGET_MSGS

# 采集器自身开销（W03 口径：常驻 vs 诊断每次操作 ns）
timeout 300 build/w03/w03_collector overhead --iterations 200000 --out "$OUT/observation_overhead.json" \
  > "$OUT/observation_overhead.log" 2>&1
echo "collector_overhead rc=$?" | tee -a "$OUT/observation_overhead.log"

DOM=7100
for workers in 4 8 16 32; do
  for n in 100 500 1000; do
    tag="n${n}-w${workers}"
    for diag in off on; do
      d="$OUT/$tag-$diag"
      echo "=== $tag diag=$diag ==="
      timeout 900 "$BIN" --n "$n" --workers "$workers" --diag "$diag" --window-ms 6000 \
        --domain "$((DOM++))" --msgs 3 --out "$d" --run-id "$tag-$diag" \
        > "$OUT/$tag-$diag.log" 2>&1
      rc=$?
      grep -E "^scancost |^threads_breakdown|^resident |^gated |^derived |^FAILURE|DONE verdict" "$OUT/$tag-$diag.log" | sed 's/^/   /'
      echo "   rc=$rc"
    done
  done
done

python3 - "$OUT" <<'PY'
import json, os, sys, glob
root = sys.argv[1]
rows = []
for f in sorted(glob.glob(os.path.join(root, "n*-w*-*/scancost.json"))):
    try:
        d = json.load(open(f))
    except Exception as e:
        print("bad", f, e); continue
    rows.append(d)

with open(os.path.join(root, "summary.md"), "w") as o:
    o.write("# t30 §10.2 扫描计数 sweep（route 数 × N_worker × 诊断开关）\n\n")
    o.write("> run_id 见目录名；工装 `test/perf/w10/w10_scancost.cpp`（W10 工装目录内增量）。\n")
    o.write("> `mean_routes/round` = scanned_routes_total/scan_rounds；`scan/s` = 每秒**遍历 route 次数**"
            "（扫描总工作量）；`scan/s/route` = 每 route 的扫描速率（线性性判据）。\n\n")
    o.write("| N | W | 池容量 | 在册 route | 容量受限 | diag | scan_rounds | scanned_routes_total | "
            "mean_routes/round | route/W | 就绪比例 | scan/s | scan/s/route | ns/round | wait_timeouts | "
            "deferred_max | cpu_cores | 判定 |\n")
    o.write("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    for d in sorted(rows, key=lambda x: (x["workers"], x["n"], x["diag"])):
        r, g, dv = d["resident"], d["gated"], d["derived"]
        o.write("| {n} | {w} | {cap} | {rc} | {binds} | {diag} | {rounds} | {scanned} | {mpr:.2f} | "
                "{rw:.2f} | {rr:.4f} | {sps:.0f} | {spr:.2f} | {nsr:.1f} | {wt} | {dm} | {v} |\n".format(
                    n=d["n"], w=d["workers"], cap=d["pool_capacity"], rc=d["route_count"],
                    binds="是" if d["capacity_binds"] else "否", diag="on" if d["diag"] else "off",
                    rounds=r["scan_rounds"], scanned=r["scanned_routes_total"],
                    mpr=dv["mean_routes_per_round"], rw=dv["expected_routes_over_w"],
                    rr=dv["ready_ratio"], sps=dv["scanned_routes_per_s"],
                    spr=dv["scanned_routes_per_s_per_route"], nsr=dv["mean_scan_ns_per_round"],
                    wt=r["wait_timeouts"], dm=r["deferred_depth_max"], cpu=d.get("cpu_cores", -1.0),
                    v=d["verdict"]))
    o.write("\n## 线性性：scan/s ÷ 在册 route（同一 W 下应近似常数；diag=on 档）\n\n")
    o.write("| W | N=100 | N=500 | N=1000 |\n|---|---|---|---|\n")
    for w in (4, 8, 16, 32):
        cells = []
        for n in (100, 500, 1000):
            hit = [d for d in rows if d["n"] == n and d["workers"] == w and d["diag"]]
            cells.append(f"{hit[0]['derived']['scanned_routes_per_s_per_route']:.2f}" if hit else "n/a")
        o.write(f"| {w} | " + " | ".join(cells) + " |\n")
    o.write("\n## 扫描轮数（同一 W 下应近似常数 ⇒ 轮数由等待节奏决定，不随 route 数增长）\n\n")
    o.write("| W | diag | N=100 | N=500 | N=1000 |\n|---|---|---|---|---|\n")
    for w in (4, 8, 16, 32):
        for dg in (False, True):
            cells = []
            for n in (100, 500, 1000):
                hit = [d for d in rows if d["n"] == n and d["workers"] == w and d["diag"] == dg]
                cells.append(str(hit[0]["resident"]["scan_rounds"]) if hit else "n/a")
            o.write(f"| {w} | {'on' if dg else 'off'} | " + " | ".join(cells) + " |\n")
    o.write("\n## 线程账（同一次运行；`residual` = 总数 − 池 worker − 控制面调度器 − 编排线程）\n\n")
    o.write("| N | W | threads | pool_workers | 控制面 | 编排 | residual | overflow routes | compat(kWaitSetFull) | 判定 |\n")
    o.write("|---|---|---|---|---|---|---|---|---|---|\n")
    for d in sorted(rows, key=lambda x: (x["workers"], x["n"])):
        o.write("| {n} | {w} | {t} | {pw} | {cs} | {or_} | {res} | {ov} | {cw} | {v} |\n".format(
            n=d["n"], w=d["workers"], t=d["threads"], pw=d["workers"], cs=d["control_scheduler_threads"],
            or_=d["orchestration_threads"], res=d["residual_threads"], ov=d["overflow_routes"],
            cw=d["seam"]["compat_kWaitSetFull"], v=d["verdict"]))
    o.write("\n## 诊断开关代价（同一档、同一窗口，只差 `diagnostics_enabled()`）\n\n")
    o.write("| N | W | ns/round(off) | ns/round(on) | cpu(off) | cpu(on) | 判定差 |\n|---|---|---|---|---|---|---|\n")
    for w in (4, 8, 16, 32):
        for n in (100, 500, 1000):
            off = [d for d in rows if d["n"] == n and d["workers"] == w and not d["diag"]]
            on = [d for d in rows if d["n"] == n and d["workers"] == w and d["diag"]]
            if off and on:
                o.write("| {n} | {w} | {a:.1f} | {b:.1f} | {ca:.5f} | {cb:.5f} | — |\n".format(
                    n=n, w=w, a=off[0]["derived"]["mean_scan_ns_per_round"],
                    b=on[0]["derived"]["mean_scan_ns_per_round"],
                    ca=off[0].get("cpu_cores", -1.0), cb=on[0].get("cpu_cores", -1.0)))
    o.write("\n")
    o.write("`diag=off` 时 `scan_time_ns_total` 恒为 0 —— 这是**设计**而不是未接线：扫描耗时是唯一"
            "需要两次 `clock_gettime` 的量，只由门控的 `ScanRoundScope` 提供（W03 实测 0.265 ns/轮 vs "
            "29.78 ns/轮）。接线存在性由**常驻** `scanned_routes_total` 非零证明（off 档同样非零）。\n")
print("summary written:", os.path.join(root, "summary.md"))
for d in sorted(rows, key=lambda x: (x["workers"], x["n"], x["diag"])):
    r, dv = d["resident"], d["derived"]
    print("  n={:4d} w={:2d} diag={:3s} rounds={:7d} scan/s={:9.0f} scan/s/route={:6.2f} "
          "ns/round={:8.1f} ready={:.3f} capacity_binds={} {}".format(
              d["n"], d["workers"], "on" if d["diag"] else "off", r["scan_rounds"],
              dv["scanned_routes_per_s"], dv["scanned_routes_per_s_per_route"],
              dv["mean_scan_ns_per_round"], dv["ready_ratio"], d["capacity_binds"], d["verdict"]))
PY
echo "SCANCOST_SWEEP_DONE"
