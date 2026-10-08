#!/usr/bin/env python3
"""多发布对照：按发布者身份复算，显示真实调用重叠和每发布者差异。"""
import argparse
from collections import defaultdict, Counter
import gzip
import hashlib
import json
from pathlib import Path
import statistics
from run import audit_raw, dump, fingerprint
from report import table, NAMES, number, median_metric

p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
root=a.directory; m=json.loads((root/'manifest.json').read_text())
planned=Counter((c['publishers'],c['subscribers'],c['bytes'],c['backend']) for c in m['jobs'])
assert len({w['id'] for w in m['windows']})==len(m['jobs'])==len(m['windows'])
assert {w['id'] for w in m['windows']}=={j['id'] for j in m['jobs']}
groups=defaultdict(list);ledger=[];totals=Counter(); audits=[]; integrity=[]
for w in m['windows']:
    folder=root/w['id']; d=json.loads((folder/'result.json').read_text());c=d['case'];totals[d['status']]+=1
    for name,h in w['files'].items():
        with gzip.open(folder/(name+'.gz'),'rb') as f:assert hashlib.sha256(f.read()).hexdigest()==h
    can_recompute=('publisher' in d and 'receivers' in d and all((folder/(name+'.csv.gz')).exists() for name in [*[f'pub{i}' for i in range(c['publishers'])],*[f'sub{i}' for i in range(c['subscribers'])]]))
    if 'summary' in d or can_recompute:
        s=audit_raw(folder,c,d)
        original_summary='summary' in d
        if original_summary: assert s==d['summary']
        assert s['accepted_total']==d['publisher']['accepted'], (c['id'],'发布接受计数')
        assert s['publish']['count']-s['accepted_total']==d['publisher']['rejected'], (c['id'],'发布拒绝计数')
        assert all(x['count']==r['received'] for x,r in zip(s['per_subscriber'],d['receivers'])), (c['id'],'接收计数')
        d['summary']=s
        audits.append({'id':c['id'],'status':d['status'],'original_summary':original_summary,'summary':s})
        totals['raw_recomputed_windows']+=1
        totals['rejected']+=d['publisher']['rejected']
        totals['missing_accepted_deliveries']+=sum(x['missing_accepted'] for x in s['delivery'])
        integrity.append([c['id'],d['status'],c['publishers']*c['rate']*c['seconds'],s['accepted_total'],d['publisher']['rejected'],s['delivered_total'],sum(x['missing_accepted'] for x in s['delivery']),sum(x['duplicates'] for x in s['delivery']),sum(x['invalid'] for x in d['receivers'])])
        totals['accepted']+=s['accepted_total'];totals['received']+=s['delivered_total']
        groups[(c['publishers'],c['subscribers'],c['bytes'],c['backend'])].append(d)
        ledger.append([c['id'],d['status'],s['accepted_total'],s['delivered_total'],number(s['receive'].get('p50_us')),number(s['receive'].get('p99_us')),
                       s['concurrent_publish_max'],s['overlapped_publications'],'；'.join(s['errors']) or '无'])
    else:ledger.append([c['id'],d['status'],*['—']*6,d.get('error','失败').splitlines()[-1]])
rows=[]; publishers=[]
for (np,ns,b,backend),ds in sorted(groups.items()):
    med=lambda f:statistics.median(f(d) for d in ds)
    rows.append([np,ns,b,NAMES[backend],ds[0]['case']['aggregate_rate'],f"{sum(d['status']=='ok' for d in ds)}/{planned[(np,ns,b,backend)]}",
                 *[number(median_metric(ds,group,key)) for group,key in [('publish','mean_us'),('receive','mean_us'),('receive','p50_us'),('receive','p99_us')]],
                 f"{med(lambda d:sum(d['cpu_seconds'].values())/d['resource_seconds']*100):.3f}",
                 number(med(lambda d:d['summary']['overlapped_publications']/d['summary']['publish']['count']*100)) if all(d['summary']['publish']['count'] for d in ds) else '—',
                 max(d['summary']['concurrent_publish_max'] for d in ds)])
    for i in range(np):
        publishers.append([np,ns,b,NAMES[backend],i,*[number(med(lambda d:d['summary']['publishers'][str(i)][key])) if all(key in d['summary']['publishers'].get(str(i),{}) for d in ds) else '—' for key in ('mean_us','p99_us')]])
text=['# 多发布、多订阅本机对照','',f"源码：`131f37c5`；{len(m['windows'])} 窗，状态和总计：`{dict(totals)}`。",'',
      '各发布者为独立进程，向同一话题发送；通过相同的 CLOCK_MONOTONIC 起点同步定时计划，不由单线程轮流调用 publish。消息身份使用（发布者 ID，独立序号），每个订阅者须收到全部发布者的全部正式消息。',
      '每个窗口的 rate_policy、每发布者 rate 和总 aggregate_rate 见 manifest。固定每发布者速率的组随发布者数增加总负载；固定总速率组才用于比较同一总输入下的争用。所有定速循环使用 sleep_until，接收使用阻塞 get/waitset。',
      '固定总速率控制的是平均输入量；相同起点使 4/8 个发布者形成同步微突发，瞬时到达形状和 CPU 唤醒节奏并未固定。因此本组不能单独把延迟或完整性差异归因于网关、锁或框架内部争用。',
      '发布重叠依据不同进程实际 [发布起点, 返回) 区间求交，包含借样/复制；并不证明框架内部提交段完全并行。并发最大值为 1 的窗口没有观察到调用重叠，应按该边界解读。',
      '原始 CSV 包含发布失败的尝试。发布耗时及重叠比例均按全部尝试计算；失败窗口的快速拒绝不能解读为有效业务性能。非零退出但完整保存 CSV 的窗口也复算并纳入计数，原始 failed 状态不改变；audit.json 标明其原始摘要是否存在。',
      '表中为各轮指标的中位数，异常轮保留；无异常轮不足 3 时不宣称稳定收益。端到端先合并该窗全部订阅者，发布指标也提供逐发布者表。DDS 的 API 边界、类型格式、资源和完整性规则与单发布阻塞对照相同。','',
      table(['PUB','SUB','载荷 B','后端','总输入/s','无异常轮','发布总均值 µs','端到端均值 µs','p50 µs','p99 µs','CPU %','重叠调用 %','最大同时发布'],rows),'',
      '## 逐轮完整性', '',table(['窗口','状态','计划发布','接受','拒绝','接收','缺失的已接受投递','重复','损坏'],integrity),'',
      '## 每个发布者','',table(['PUB','SUB','载荷 B','后端','发布者 ID','发布均值 µs','发布 p99 µs'],publishers),'',
      '## 逐轮账本','',table(['窗口','状态','接受','接收','p50','p99','最大同时发布','重叠调用数','异常'],ledger)]
if root.name in ('multi-fixed-total','multi-32sub'):
    text.insert(3,'环境补注：阶段提交曾触发 Git 后台打包，旧监测未覆盖该进程。本批保留为潜在受干扰记录；同条件完整复测见根目录 verification- 对应阶段，不把原性能数值作为环境补正后的结果。')
(root/'results.md').write_text('\n'.join(text)+'\n');dump(root/'audit.json',{'windows':len(m['windows']),'totals':totals,'raw_recomputed_windows':len(audits),'windows_without_complete_raw':len(m['windows'])-len(audits),'report_sha256':fingerprint(Path(__file__)),'audits':audits})
print(json.dumps(totals,ensure_ascii=False))
