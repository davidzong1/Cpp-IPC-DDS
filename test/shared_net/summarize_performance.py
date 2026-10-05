#!/usr/bin/env python3
"""按全部原始接收样本重算分位数；报告三轮中位数及每轮门槛。"""
import argparse
import csv
import gzip
import json
from pathlib import Path
import statistics
from benchmark import quantiles


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    cases=json.loads((args.directory/'cases.json').read_text())
    indexed={}
    for case in cases:
        if 'receivers' not in case:
            continue
        samples=[]
        for path in (args.directory/case['case']).glob('sub*.csv.gz'):
            with gzip.open(path,'rt') as file:
                samples.extend(int(row['elapsed_ns']) for row in csv.DictReader(file))
        case['aggregate_latency']=quantiles(samples)
        case['cpu_total_seconds']=sum(case['cpu_seconds'])
        case['rss_sum_kib']=sum(x['rss_kib'] for x in case['after'])
        indexed[(case['mode'],case['subscribers'],case['bytes'],case['repeat'])]=case
    rows=[]
    for subscribers in (1,8,32):
        for size in (64,4096,1048576):
            groups=[[indexed[(mode,subscribers,size,repeat)] for repeat in (1,2,3)] for mode in ('baseline','shared_v1')]
            medians=[{key:statistics.median(c['aggregate_latency'][key] for c in group) for key in ('p50_us','p95_us','p99_us')} for group in groups]
            gate=lambda baseline,current:current-baseline<=max(baseline*.1,5)
            paired=[all(gate(groups[0][i]['aggregate_latency'][key],groups[1][i]['aggregate_latency'][key]) for key in ('p50_us','p99_us')) for i in range(3)]
            median_gate=all(gate(medians[0][key],medians[1][key]) for key in ('p50_us','p99_us'))
            rows.append({'subscribers':subscribers,'payload_bytes':size,'wire_bytes':groups[0][0]['wire_bytes'],
                'baseline':medians[0],'shared_v1':medians[1],'median_gate':median_gate,'paired_round_gates':paired,
                'cpu_total_seconds':[statistics.median(c['cpu_total_seconds'] for c in group) for group in groups],
                'rss_sum_kib':[statistics.median(c['rss_sum_kib'] for c in group) for group in groups],
                'failed_calls':[sum(c['publish']['rejected'] for c in group) for group in groups],
                'lost':[sum(r['lost'] for c in group for r in c['receivers']) for group in groups]})
    result={'completed_windows':len(cases),'rows':rows,'all_median_gates':all(row['median_gate'] for row in rows),
            'all_paired_round_gates':all(all(row['paired_round_gates']) for row in rows),
            'invalid':sum(r['invalid'] for case in cases for r in case.get('receivers',[])),
            'duplicates':sum(r['duplicates'] for case in cases for r in case.get('receivers',[])),
            'received':sum(r['count'] for case in cases for r in case.get('receivers',[]))}
    (args.directory/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    lines=['# 纯本机预构造 DZFlat 比较','',
        '每格为三次独立 30 秒窗口的分位数中位数；每轮先合并全部订阅进程的原始样本。原始 CSV 无损压缩保留。', '',
        '| 订阅者 | 载荷 / wire B | 基线 p50 / p99 µs | shared_v1 p50 / p99 µs | 中位数门槛 | 逐轮门槛 |',
        '|---:|---:|---:|---:|---|---|']
    for row in rows:
        a,b=row['baseline'],row['shared_v1']
        lines.append(f"| {row['subscribers']} | {row['payload_bytes']} / {row['wire_bytes']} | {a['p50_us']:.3f} / {a['p99_us']:.3f} | {b['p50_us']:.3f} / {b['p99_us']:.3f} | {'通过' if row['median_gate'] else '不满足'} | {','.join('通过' if value else '不满足' for value in row['paired_round_gates'])} |")
    lines.extend(['',f"窗口 {len(cases)}/54；接收 {result['received']} 次，错误 {result['invalid']}，重复 {result['duplicates']}。",'',
        '门槛分别用于 p50、p99：新值 − 基线 ≤ max(基线 × 10%, 5 µs)。逐轮结果也完整报告，不删除不利轮次。',
        'CPU/RSS/FD/SHM 与每个订阅进程的分位数见 cases.json；RSS 求和会重复计入共享映射，不等于物理内存实占。',
        '这是 100 Hz 固定负载与已编码 StdImage 的测试，未替代普通对象编码、饱和吞吐、真实网卡或跨物理主机测量。'])
    (args.directory/'summary.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=False))


if __name__=='__main__':main()
