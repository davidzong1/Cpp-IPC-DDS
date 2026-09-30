import os, sys, time
pid = int(sys.argv[1]); win = float(sys.argv[2])
def snap():
    out={}; d=f"/proc/{pid}/task"
    for tid in os.listdir(d):
        try:
            b=open(f"{d}/{tid}/stat").read(); nm=open(f"{d}/{tid}/comm").read().strip()
            st=os.stat(f"{d}/{tid}")
        except OSError: continue
        f=b[b.rfind(")")+2:].split()
        out[int(tid)]=(nm,int(f[11])+int(f[12]),int(f[19]),st.st_ino)
    return out
a=snap(); time.sleep(win); b=snap()
rows=[(b[t][1]-a[t][1], b[t][2], t, b[t][0]) for t in b if t in a]
rows.sort(key=lambda r:r[1])            # 按 starttime 排 = 创建顺序
tot=sum(r[0] for r in rows)
print(f"PID={pid}（主线程 tid 必等于 PID）；进程内线程数={len(rows)}")
print(f"窗口 {win}s：总 tick={tot} ⇒ {tot/100/win:.4f} core")
print("按【创建顺序】列出（含 0 值）：")
for v,st,t,nm in rows:
    mark = "  <== 主线程" if t==pid else ""
    print(f"   #{rows.index((v,st,t,nm))+1:<3} tid={t:<7} {nm:<22} cputick={v:5d}  {v/100/win:.4f} core{mark}")
