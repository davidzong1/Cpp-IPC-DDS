#!/bin/bash
# t58 独立复现 R-1 机制（⛔ 不转抄 t32 读数；全程自采）
# 判据 1：Σ = 1000×(1000/10) + 1000×(1000/50) = 120000 /s 应等于**回退臂**实测 ctx/s
# 判据 2：默认臂 ctx/s ≈ 池项 320 + Σ/B（B≥6）⇒ 远低于 Σ
# 判据 3：2×2 四格（调度器臂 × 池臂）联立，且线程数与 ctx 同步
set -u
O=artifacts/perf/20261001-r58-W05-S4
BIN=build/bin/w10_idle
mkdir -p $O/r1
run(){ # tag domain envs...
  local tag="$1" dom="$2"; shift 2
  env "$@" timeout 900 $BIN --n 1000 --domain $dom --state 3 --windows 3 --win-s 15 > $O/r1/$tag.log 2>&1
  echo "$tag rc=$?"
}
run A-sched-pool    9761
run B-compat-pool   9762 DZIPC_SHM_CONTROL_SCHEDULER=1
run C-sched-nopool  9763 DZIPC_SHM_RECV_COMPAT=1
run D-compat-nopool 9764 DZIPC_SHM_CONTROL_SCHEDULER=1 DZIPC_SHM_RECV_COMPAT=1
python3 - <<'PY'
import statistics as st
O='artifacts/perf/20261001-r58-W05-S4/r1'
rows={}
for tag in ['A-sched-pool','B-compat-pool','C-sched-nopool','D-compat-nopool']:
    ctx=[];thr=[];tick=[];wo=[]
    for l in open(f'{O}/{tag}.log',encoding='utf-8',errors='replace'):
        if l.startswith('state='):
            head=l.split()
        if l[:2] in ('0,','1,','2,'):
            f=l.strip().split(','); thr.append(int(f[3])); ctx.append(float(f[5])); wo.append(int(f[9]))
        if l.strip().startswith('scheduler entries'):
            tick.append(float(l.split('tick_wakes_per_s=')[1].split()[0]))
    rows[tag]=dict(ctx=st.median(ctx),thr=st.median(thr),tick=st.median(tick),wo=st.median(wo))
SIGMA=1000*(1000/10)+1000*(1000/50); POOL=32*1000/100
print(f"Σ = {SIGMA:.0f} /s  池项结构值 = {POOL:.0f} /s")
print(f"{'格':18s}{'threads':>9s}{'ctx/s':>12s}{'tick/s':>10s}{'池 wait_timeout/s':>18s}{'ctx-(tick+池)':>15s}{'ctx/Σ':>9s}")
for k,v in rows.items():
    s=v['tick']+v['wo']
    print(f"{k:18s}{v['thr']:9.0f}{v['ctx']:12.1f}{v['tick']:10.1f}{v['wo']:18.1f}{v['ctx']-s:15.1f}{v['ctx']/SIGMA:9.4f}")
b=rows['B-compat-pool']['ctx']; d=rows['D-compat-nopool']['ctx']
print(f"\n回退臂（池 ON，B 格）ctx = {b:.1f} /s  ⇒ 与 Σ 偏差 {100*(b-SIGMA)/SIGMA:+.3f}%")
print(f"回退臂（池 OFF，D 格）ctx = {d:.1f} /s ⇒ 与 Σ+池 偏差 {100*(d-(SIGMA+POOL))/(SIGMA+POOL):+.3f}%")
a=rows['A-sched-pool']['ctx']
print(f"默认臂（A 格）ctx = {a:.1f} /s ⇒ 隐含批均因子 B = Σ/(ctx-池项) = {SIGMA/(a-POOL):.2f}（须 ≥ B_min=6.0）")
print(f"R-1b 默认臂阈值 = {(POOL+20000)*1.5:.0f} /s ；回退臂阈值 = {(POOL+SIGMA)*1.5:.0f} /s")
print(f"判定：A 格 {a:.0f} ≤ 30480 ? {a<=30480}；B 格 {b:.0f} ≤ 180480 ? {b<=180480}")
PY
echo T58_R1_DONE
