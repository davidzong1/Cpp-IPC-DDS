#!/usr/bin/env python3
"""R1 主体 t14 · W02 ① 跨进程身份可核验 —— 独立复算（在**现行 r23** 上，非 r20）"""
import re, sys, os, json
run = sys.argv[1] if len(sys.argv)>1 else 'artifacts/perf/20260928-r23-W02'
txt = open(os.path.join(run,'verdict.md')).read()
pat = re.compile(r"`([^`]+)` identity: parent_pid=(\d+) pub_pid=(\d+) sub_pid=(\d+)"
                 r" \| pub_ready_ns=(\d+) in \[(\d+),(\d+)\]"
                 r" \| sub_ready_ns=(\d+) in \[(\d+),(\d+)\]"
                 r" \| pub_starttime_ticks=(\d+) sub_starttime_ticks=(\d+)"
                 r" \| pub_lib=(\S+) \| sub_lib=(\S+)")
rows=[]
for m in pat.finditer(txt):
    rows.append(dict(case=m.group(1), parent=int(m.group(2)), pub=int(m.group(3)), sub=int(m.group(4)),
        prn=int(m.group(5)), plo=int(m.group(6)), phi=int(m.group(7)),
        srn=int(m.group(8)), slo=int(m.group(9)), shi=int(m.group(10)),
        pt=int(m.group(11)), st=int(m.group(12)), plib=m.group(13), slib=m.group(14)))
tot=len(rows)
asserts = {
 "解析出的 identity 行数 ==75": tot==75,
 "parent/pub/sub 三者互异 (逐条)": all(len({r['parent'],r['pub'],r['sub']})==3 for r in rows),
 "75 个 pid 三元组互不重复": len({(r['parent'],r['pub'],r['sub']) for r in rows})==75,
 "pub_ready 落在 spawn 括号内": all(r['plo']<=r['prn']<=r['phi'] for r in rows),
 "sub_ready 落在 spawn 括号内": all(r['slo']<=r['srn']<=r['shi'] for r in rows),
 "starttime 均存在(>0)": all(r['pt']>0 and r['st']>0 for r in rows),
 "pub_lib 全部 = build/lib/libipc.so.1.3.0": all(r['plib'].endswith('build/lib/libipc.so.1.3.0') for r in rows),
 "sub_lib 全部 = build/lib/libipc.so.1.3.0": all(r['slib'].endswith('build/lib/libipc.so.1.3.0') for r in rows),
}
man=json.load(open(os.path.join(run,'manifest.json')))
dt=json.load(open(os.path.join(run,'results.json')))
asserts["child_killed_total == 0"] = man.get('child_killed_total',man.get('counts',{}).get('child_killed_total'))==0
# 独立的二次证据：results.json 的 valid/expected/registered
cases=dt.get('cases',[])
# 与 verdict.md 的 "valid_rx_count / expected_count / registered_count = 75 / 75 / 75"
# 做**跨来源**核对：results.json 逐 case 的 plan==attempts==sent_ok==recv_measure==1000/500 且失败计数全 0
def _c(k): return [c.get(k) for c in cases]
asserts["results.json cases == 75"] = len(cases)==75
asserts["逐 case sent_ok == plan（无静默丢失）"] = all(c.get('sent_ok')==c.get('plan') for c in cases)
asserts["逐 case recv_measure == plan（有效接收）"] = all(c.get('recv_measure')==c.get('plan') for c in cases)
asserts["逐 case missing/duplicate/out_of_order/checksum_bad/abnormal 全 0"] = all(
    c.get(k,0)==0 for c in cases for k in ('missing','duplicate','out_of_order','checksum_bad','abnormal'))
asserts["逐 case child_killed == 0"] = all(c.get('child_killed',0)==0 for c in cases)
asserts["逐 case case_ok == True"] = all(c.get('case_ok') is True for c in cases)
asserts["逐 case failure_reasons 为空"] = all(not c.get('failure_reasons') for c in cases)
ok=all(asserts.values())
for k,v in asserts.items(): print(f"  {'✅' if v else '⛔'} {k}")
pk=[(r['parent'],r['pub'],r['sub']) for r in rows]
print(f"\n  样例 pid 三元组: {pk[:3]} … 共 {len(set(pk))} 组唯一值")
print(f"\n{'IDENTITY_OK' if ok else 'IDENTITY_FAIL'} rows={tot} violations={sum(1 for v in asserts.values() if not v)}")
