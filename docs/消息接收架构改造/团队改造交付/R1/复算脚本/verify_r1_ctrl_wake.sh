set -u
echo "# R-1 控制面项 独立复算原始记录（架构负责人 t32 / D-25）"
echo "# 生成时刻: $(date '+%Y-%m-%d %H:%M:%S %z')  指纹: $(sha256sum build/lib/libipc.so.1.3.0 | cut -c1-16)"
echo
echo "## A. 从 r25-W10 **既有证据**读出的原始读数（⛔ 纯读，不重跑）"
echo "### A1 state3（1000 sub + 1000 pub，门槛3 定义配置）"
grep -H "" artifacts/perf/20260929-r25-W10/idle-state3-1000.log | grep -E "^artifacts.*(state=|^.*0,3,|scheduler entries)"
echo "### A2 state2（1000 已注册未连接）"
grep -H "" artifacts/perf/20260929-r25-W10/idle-state2-1000.log | grep -E "(state=|0,2,|scheduler entries)"
echo "### A3 state1（无 route）"
grep -H "" artifacts/perf/20260929-r25-W10/idle-state1-1000.log | grep -E "(state=|0,1,|scheduler entries)"
echo "### A4 gate3 标定（同一运行的 60s/30s 两组）"
sed -n '4,31p' artifacts/perf/20260929-r25-W10/gate3/gate3_idle.txt
echo
echo "## B. 我本次独立复跑（同源重编，⛔ 不写 r25 目录）"
echo "### B1 state3 60s 窗口 ×3（门槛3 的窗口要求 ≥60 s）"
for i in 1 2 3; do timeout 180 ./build/t32/w10_idle --n 1000 --domain $((42000+i*7)) --state 3 --windows 1 --win-s 60 2>&1 | grep -E "^0,3|scheduler entries"; done
echo "### B2 state3 20s 窗口 ×5"
for i in 1 2 3 4 5; do timeout 120 ./build/t32/w10_idle --n 1000 --domain $((41000+i*13)) --state 3 --windows 1 --win-s 20 2>&1 | grep -E "^0,3|scheduler entries"; done
echo "### B3 公式项标定：wakescan（heartbeat=10ms，20s）"
for n in 1 10 100 1000 2000; do timeout 60 ./build/t32/w10_wakescan $n 20 10 2>&1 | tail -1; done
echo "### B4 公式项标定：mixwake（产品同形 1000sub@10ms + 1000pub@50ms，20s）×5"
for i in 1 2 3 4 5; do timeout 90 ./build/t32/w10_mixwake 1000 20 2>&1 | tail -1; done
echo "### B5 唤醒率的**决定因素**：产品同形 × 注册散布（18s，各项 1000 pairs）"
for s in 0 5 10 20 50 100 200 400 700; do timeout 90 ./build/t32/mixspread 1000 $s 18 2>&1 | tail -1; done
echo "### B6 批量因子（callbacks/tick）与注册散布"
for s in 0 2 5 10 20 50; do timeout 60 ./build/t32/batch 1000 $s 12 2>&1 | tail -1; done
echo
echo "## C. 恒等式核验：ctx ?= 调度器唤醒率 + 池 wait_timeout 率（同 ctx 口径）"
python3 - <<'PY'
rows=[("r25 idle-state3-1000 (60s)",9040.0,8704.6,19199,60.0),
      ("gate3 A state3 (60s)",9101.1,8762.9,19196,60.0),
      ("gate3 B state3 cpu-only(30s)",None,8791.5,9600,30.0),
      ("r25 idle-state2-1000 (60s)",14159.2,14141.1,0,60.0),
      ("gate3 A state2 (60s)",5032.6,5025.8,0,60.0),
      ("gate3 B state2 (30s)",None,14447.4,0,30.0)]
print(f"{'读数来源':32s} {'ctx/s(测)':>10s} {'tick/s':>9s} {'pool wait/s':>12s} {'和':>9s} {'偏差':>8s}")
for name,ctx,tick,wt,win in rows:
    pool=wt/win; s=tick+pool
    d=f"{(s-ctx)/ctx*100:7.2f}%" if ctx else "    n/a"
    print(f"{name:32s} {('%.1f'%ctx) if ctx else '  n/a':>10s} {tick:9.1f} {pool:12.1f} {s:9.1f} {d:>8s}")
PY
echo
echo "## D. 结论（复算要点）"
echo "D1 Σ 理论上界 = 1000×(1000/10) + 1000×(1000/50) = 120000/s（1000 sub + 1000 pub）"
echo "D2 Σ **不是**期望值：实测唤醒率随**注册散布**在 0.47k–14.7k/s 之间变化（B5），"
echo "   ⇒ 报告引用的『1.5–4.6 k/s』只是某一种注册散布下的点值，**不足以作上界**。"
echo "D3 由 B6：批量因子（callbacks/tick）= 6.3–51.7 ⇒ 唤醒率 = Σ / 批量因子。"
echo "D4 由 C：ctx 与『调度器唤醒率 + 池 wait_timeout』恒等（偏差 ≤0.20%），"
echo "   ⇒ R-1 的控制面项应换成**实测调度器唤醒率**，池项单列（实测 320/s，与结构下限 320/s 吻合）。"
