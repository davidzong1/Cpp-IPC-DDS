# -*- coding: utf-8 -*-
"""t58: “回退只换驱动源、不换语义”的语句级机械核对。
把 HEAD 的循环体语句集与 W05 的回调体语句集做**归一化后的集合差**，并把差异逐条
归入“文档已声明的偏差”或“未声明差异”。归一化：去注释/空白、h-> → 、
had_subscriber_ → had_subscriber、kControlTiming.X → X。
"""
import re, difflib

def strip(t):
    t=re.sub(r'/\*.*?\*/','',t,flags=re.S)
    t=re.sub(r'//[^\n]*','',t)
    return t
def norm(t):
    t=strip(t)
    t=t.replace('h->','').replace('had_subscriber_','had_subscriber')
    t=re.sub(r'kControlTiming\.','',t)
    t=re.sub(r'\s+','',t)
    return t
def stmts(t):
    t=norm(t).replace('}','}@').replace(';',';@').replace('{','{@')
    return [x for x in t.split('@') if x.strip() not in ('','{','}')]
def slice_lines(path, a_pat, b_pat):
    L=open(path,encoding='utf-8',errors='replace').read().splitlines()
    a=next(i for i,l in enumerate(L) if a_pat in l)
    b=next(i for i,l in enumerate(L) if i>a and b_pat in l)
    return "\n".join(L[a:b]), a+1, b

HP='build/t58/head_shm_pub_sub_ipc.cc'
WP='artifacts/perf/20260928-r23-W05/shm_pub_sub_ipc.cc.W05'

# 发布端
H,ha,hb = slice_lines(HP,'void shm_pub_ipc::pub_handshake','bool shm_pub_ipc::publish')
Wh,wa,wb = slice_lines(WP,'void on_pub_heartbeat','void on_pub_stale_scan')
Ws,sa,sb = slice_lines(WP,'void on_pub_stale_scan','shm_pub_ipc* host_{nullptr};')
hs=set(stmts(H)); ws=set(stmts(Wh))|set(stmts(Ws))
print("### 发布端：HEAD .cc:%d-%d 循环体  vs  W05 .cc:%d-%d(on_pub_heartbeat)+%d-%d(on_pub_stale_scan)"%(ha,hb,wa,wb,sa,sb))
print("HEAD 语句 %d，W05 语句 %d"%(len(hs),len(ws)))
print("仅在 HEAD（语义缺口）:")
for x in sorted(hs-ws): print("   -",x[:140])
print("仅在 W05（新增）:")
for x in sorted(ws-hs): print("   +",x[:140])

# 订阅端
H2,h2a,h2b = slice_lines(HP,'void shm_sub_ipc::sub_handshake','void shm_sub_ipc::reset_message')
W2,w2a,w2b = slice_lines(WP,'void on_sub_heartbeat','shm_sub_ipc* host_{nullptr};')
hs2=set(stmts(H2)); ws2=set(stmts(W2))
print()
print("### 订阅端：HEAD .cc:%d-%d  vs  W05 .cc:%d-%d"%(h2a,h2b,w2a,w2b))
print("HEAD 语句 %d，W05 语句 %d"%(len(hs2),len(ws2)))
print("仅在 HEAD（语义缺口）:")
for x in sorted(hs2-ws2): print("   -",x[:150])
print("仅在 W05（新增，前 25）:")
for x in sorted(ws2-hs2)[:25]: print("   +",x[:150])
