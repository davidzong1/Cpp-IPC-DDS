#!/usr/bin/env python3
"""原速度与千话题压力：复算时间值，保留容量和交付失败。"""
import argparse
from collections import defaultdict,Counter
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
from report import table, number
from run import dump, ROOT, fingerprint

p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--reuse-audit-from');a=p.parse_args();root=a.directory
reused=None
if a.reuse_audit_from:
    subprocess.run(['git','diff','--quiet',a.reuse_audit_from,'--',str(root.resolve().relative_to(ROOT))],cwd=ROOT,check=True)
    reused={'commit':a.reuse_audit_from,'audit_sha256':fingerprint(root/'audit.json'),'audit':json.loads((root/'audit.json').read_text())}
m=json.loads((root/'manifest.json').read_text());assert {w['id'] for w in m['windows']}=={j['id'] for j in m['jobs']}
assert len(m['windows'])==len(m['jobs'])==len({w['id'] for w in m['windows']})
planned=Counter((c['topics'],c['bytes'],c['backend']) for c in m['jobs'])
groups=defaultdict(list);ledger=[];totals=Counter();raw_checks=0
for w in m['windows']:
    folder=root/w['id']; c=json.loads((folder/'case.json').read_text())
    d=json.loads((folder/'result.json').read_text()) if (folder/'result.json').exists() else {'status':'failed'}
    totals[d['status']]+=1; checked=set()
    for name,h in ([] if reused else w['raw_sha256'].items()):
        with gzip.open(folder/(name+'.gz'),'rb') as f: assert hashlib.sha256(f.read()).hexdigest()==h
        role=Path(name).name.split('.')[0]
        values=defaultdict(list)
        with gzip.open(folder/(name+'.gz'),'rt') as f:
            for r in csv.DictReader(f): values[r['kind']].append(float(r['value_us']))
        for kind,v in values.items():
            v.sort(); recorded=d[role]
            assert recorded[kind+'_n']==len(v)
            for suffix,value in [('mean_us',statistics.mean(v)),('p50_us',v[math.ceil(len(v)*.5)-1]),('p99_us',v[math.ceil(len(v)*.99)-1])]:
                assert math.isclose(recorded[kind+'_'+suffix],value,rel_tol=1e-9,abs_tol=1e-7), (c['id'],role,kind,suffix)
            raw_checks+=1;checked.add((role,kind))
    for role,kind in [('pub','send'),('pub','prepare'),('sub','latency')]:
        if not reused and d.get(role,{}).get(kind+'_n',0)>0:assert (role,kind) in checked, (c['id'],'缺少原始时间值',role,kind)
    if 'pub' in d and 'sub' in d:
        groups[(c['topics'],c['bytes'],c['backend'])].append(d)
        totals['sent']+=d['pub']['sent'];totals['received']+=d['sub']['received']
    ledger.append([c['id'],d['status'],d.get('pub',{}).get('plan','—'),d.get('pub',{}).get('sent','—'),d.get('sub',{}).get('received','—'),
                   '；'.join(d.get('reasons',[])) or str(d.get('sub_log_tail',[])[-2:]) if d['status']!='ok' else '无'])
rows=[]
for topics,b,backend in sorted(planned):
    ds=groups[(topics,b,backend)]
    if not ds:
        rows.append([topics,b,backend,f'0/{planned[(topics,b,backend)]}',*['—']*8]);continue
    med=lambda f:statistics.median(f(d) for d in ds)
    rows.append([topics,b,backend,f"{sum(d['status']=='ok' for d in ds)}/{planned[(topics,b,backend)]}",f"{med(lambda d:d['sub']['received_in_window']/d['duration']):.1f}",
                 *[number(med(lambda d:d[role][key])) if all(d[role].get(key.rsplit('_',2)[0]+'_n',0)>0 for d in ds) else '—' for role,key in [('sub','latency_p50_us'),('sub','latency_p99_us'),('pub','prepare_mean_us'),('pub','send_mean_us')]],
                 f"{med(lambda d:d['pub']['sent']-d['sub']['received']):.0f}",
                 f"{med(lambda d:d['sub']['cpu_seconds']/d['sub']['elapsed']*100):.1f}",
                 f"{med(lambda d:d['sub']['threads']):.0f}"])
if reused:
    assert dict(totals)==reused['audit']['totals'] and len(m['windows'])==reused['audit']['windows']
    raw_checks=reused['audit']['raw_groups_checked']
stress=m['jobs'][0]['kind']=='stress'
lines=['# '+('千话题压力测试' if stress else '既有速度工况重新测量'),'',
       f"源码冻结 `131f37c5`。{len(m['windows'])} 窗；状态及总计：`{dict(totals)}`。原始时间值复算 {raw_checks} 组，全部通过。",'',
       ('原始时间值审计沿用已提交且经 git diff 核对未变的完整阶段；此次仅修正展示层分母和无数据行，不重新筛选样本。' if reused else '本次从全部原始时间值重新校核。'),
       '本报告保留既有 transport_comparison 工装的计时语义，与 100Hz 阻塞延迟表分开：每条重新生成逻辑载荷，发布准备时间和 API 时间分列；端到端从准备开始到应用取得。DDS 为原生数据回调，dzIPC 工装主动 try_get 取样，CPU 包含这部分应用轮询。',
       '速度测试为 8B～1MiB 共 18 档、最多 8 条在途、每格 1 秒×3轮，32 次预热；报告该工况完成速率，不称无限队列极限吞吐。正常轮每条 memset、接收检查长度/序号/首尾；额外完整字节冒烟单列。',
       '压力测试为 64B，1/100/1000 话题×每话题 1000条/s，10秒×3轮，4 个发布线程分配不同话题、1 个接收进程，dzIPC 32 接收 worker。输入计划独立于接收完成；每话题最终数量、序号缺口、损坏、重复及遗漏发送计划均保留。延迟是原每话题每100条的确定性抽样，不称全量分位数。',
       'shared 是新增的当前共享端点预构造路径，包含网关；socket 是原直接 socket/TLV API，不能混称当前共享网关。DDS 原安装与同版本扩容依赖在不同目录、不同报告中记录，容量失败不等于零吞吐。',
       '三轮中位数表包含有完整计数的异常轮，无异常轮数以计划轮数为分母；初始化失败或容量不足显示无数据，不记为零吞吐；这些有异常的速率不能称完整正确业务吞吐。协议不同，逻辑载荷不等于线上字节。DDS 发布借样与接收指针证据、原始加载库和线程快照见逐窗 JSON。',
       '吞吐模式和阻塞延迟模式的编译配置分别为 Release 与 RelWithDebInfo，具体参数见各 manifest；只在同一工况内对比后端。宿主机后台活动保留，未绑核、未改变电源。','',
       table(['话题','载荷 B','后端','无异常轮','窗内接收/s','p50 µs','p99 µs','准备均值 µs','API均值 µs','发送减接收','接收CPU %','接收线程'],rows),'',
       '## 全部逐轮记录','',table(['窗口','状态','计划','成功发送','最终接收','异常'],ledger)]
(root/'results.md').write_text('\n'.join(lines)+'\n');dump(root/'audit.json',{'windows':len(m['windows']),'totals':totals,'raw_groups_checked':raw_checks,'raw_validation_reused_from':{k:v for k,v in reused.items() if k!='audit'} if reused else None,'presentation_sha256':fingerprint(Path(__file__))})
print(json.dumps(totals,ensure_ascii=False))
