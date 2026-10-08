#!/usr/bin/env python3
"""复算全部逐轮原始样本并生成中文对比报告与静态图。"""
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path
import statistics
from run import audit_raw, dump, BACKENDS

NAMES={'shm':'dzIPC SHM','shared':'dzIPC 共享端点（本机）','dds-udp':'Cyclone DDS UDP','dds-iox':'Cyclone DDS iceoryx'}
def number(value):
    return '—' if value is None else f'{value:.3f}'

def median_metric(ds, group, key):
    values=[d['summary'].get(group,{}).get(key) for d in ds]
    return statistics.median(values) if values and all(v is not None for v in values) else None

def table(headers, rows):
    return '| '+' | '.join(headers)+' |\n|'+'|'.join(['---']*len(headers))+'|\n'+''.join('| '+' | '.join(map(str,row))+' |\n' for row in rows)

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('directory',type=Path); a=parser.parse_args()
    root=a.directory.resolve(); manifest=json.loads((root/'manifest.json').read_text())
    jobs=manifest['jobs']; windows=manifest['windows']
    assert all(c.get('publishers',1)==1 for c in jobs), '多发布请使用 multi_report.py'
    assert len({j['id'] for j in jobs})==len(jobs)
    assert len({w['id'] for w in windows})==len(windows)
    assert {j['id'] for j in jobs}=={w['id'] for w in windows}, '计划窗口缺失'
    groups=defaultdict(list); records=[]; totals=Counter(); audits=[]
    for window in windows:
        folder=root/window['id']; d=json.loads((folder/'result.json').read_text()); c=d['case']
        for name,digest in window['files'].items():
            with gzip.open(folder/(name+'.gz'),'rb') as f: actual=hashlib.sha256(f.read()).hexdigest()
            assert actual==digest, (folder,name)
        if 'summary' in d:
            recomputed=audit_raw(folder,c,d)
            assert recomputed==d['summary'], folder
            assert len(d['receivers'])==c['subscribers']
            totals['accepted']+=recomputed['accepted_total']; totals['delivered']+=recomputed['delivered_total']
            groups[(c['subscribers'],c['bytes'],c['backend'])].append(d)
        totals[d['status']]+=1; records.append(d)
        audits.append({'id':c['id'],'status':d['status'],'raw_recomputed':'summary' in d,'errors':d.get('summary',{}).get('errors',[]),
                       'external_compile_or_benchmark':window['competing_activity']})
    dump(root/'audit.json',{'planned':len(jobs),'completed':len(windows),'totals':totals,'windows':audits})
    def median(ds, function): return statistics.median([function(d) for d in ds])
    lines=['# 当前 dzIPC 与 Cyclone DDS 本机 benchmark','',
           f"生产源码冻结：`{manifest['production_commit']}`；工装提交：`{manifest['source_commit']}`。本轮 {len(windows)} 窗，状态为 {dict(Counter(d['status'] for d in records))}。",
           f"实际接受 {totals['accepted']:,} 次发布，接收 {totals['delivered']:,} 条订阅样本；逐窗原始数据与复算结果见 [审计](audit.json)、[运行清单](manifest.json)。",'',
           '## 测试口径','',
           '- 本机独立发布/订阅进程；单发布者，1/8/32 个订阅者，64B/4KiB/1MiB 逻辑载荷，100Hz，预热 2 秒、正式 10 秒，各组合 3 轮。四后端逐轮按预先冻结顺序交错，全部 CPU0～31 允许，不绑核、不忙轮询、不改电源。',
           '- dzIPC 使用预构造 StdImage DZFlat：SHM 是公开 IPC_SHM 路径，共享端点是 IPC_SOCKET+shared_v1 的本机直达路径。两者使用本机 MPMC 和 64 深度应用队列；共享端点设置 DZIPC_SHARED_RECV_ASSIST=1 并额外计入网关。两条路径各使用现有公开 API，不假设内部接收实现完全相同。',
           '- Cyclone DDS 使用本机已安装的 0.10.2 与 iceoryx 2.0.5，固定尺寸 octet 数组；QoS 为 RELIABLE、VOLATILE、KEEP_LAST(64)，写入最长阻塞 100ms。UDP 关闭 SHM；iceoryx 开启 SHM、调用借样并复制预构造载荷后提交。RouDi 使用私有配置，增加 2MiB 池档容纳大消息，不改宿主配置。',
           '- 底层缓冲容量和可靠性契约不等价，结果描述当前配置；发布接受不等于所有订阅者确认。探针中 iceoryx 接收应用取得的指针不在共享内存映射，不能称端到端零拷贝，见 [路径证据](../path-probe/results.md)。',
           '- 每个订阅进程使用一个应用取样线程：dzIPC get(20ms)，DDS readcondition/waitset(20ms)+dds_take。DDS 内部线程和 RouDi、dzIPC 内部线程和网关均按实际进程统计。',
           '- 端到端从更新本条消息的时间戳/序号之前，到应用取得样本之后，包含队列、等待、通知与取样；载荷主体已预构造，不含完整业务对象生产。随后全量逐字节校验并写 CSV，两侧均保留此工作。',
           '- 发布总耗时使用与端到端相同起点，止于发布返回，包含 DDS iceoryx 借样/复制。API 列仅计 publish_prebuilt_segment 或 dds_write，不能将 DDS 已放在 API 外的复制当作免费。发布返回不代表所有订阅者已完成处理。',
           '- 逻辑载荷一致，序列化格式不同：dzIPC 段含 StdImage 元数据；DDS 为固定数组，另有 RTPS/iceoryx 开销。原始接收 bytes 对 dzIPC 是 Sample 容量，不能当作序列化长度。payload_bytes 专门核对业务长度；本报告不把容量或逻辑大小称为线上字节。',
           '- 每窗合并全部订阅者原始样本计算 nearest-rank p50/p99，再展示三轮指标的中位数；不是订阅者分位数的平均，也不是所有轮次混合分位数。CPU 包含约500ms同步启动等待、2秒预热和10秒正式，迟发窗口按实际发布完成时间计算资源秒数，表中为单核百分比；RSS 求和重复计算共享映射，不是物理内存实占。',
           '- 仅测试 100Hz 下的延迟与广播放大成本，不测试饱和吞吐、RPC、多发布并发或跨机。每个订阅者目标 100 条/s，聚合接收目标为 100×订阅者数；不能据此宣称最大吞吐。',
           '- 保留桌面/编辑器等后台活动；每秒记录已知活动进程。未观察到已知编译竞争不等于完全无噪声。三轮不能证明统计显著性或框架在全部工况更优。',
           '- 最新主线的通知候选 C1 未合入生产；旧三项延迟门槛仍未完成正式关闭。本次比较不是旧 A/C 54 窗口验收。','',
           '## 三轮对照','',
           '各格使用所有有完整原始计数的轮次，包括有异常的窗口；完整无异常轮数单列。少于三轮或含异常时，只描述观察值。单位：延迟 µs，CPU 单核%，RSS MiB。','']
    summary=[]
    for n in (1,8,32):
        for b in (64,4096,1048576):
            for backend in BACKENDS:
                ds=groups[(n,b,backend)]
                if not ds: summary.append([n,b,NAMES[backend],'0/3',*['—']*8]); continue
                good=sum(d['status']=='ok' for d in ds)
                val=lambda field: median_metric(ds,'receive',field)
                cpu=lambda d:sum(d['cpu_seconds'].values())/d['resource_seconds']*100
                rss=lambda d:sum(p['rss_kib'] for p in d['after'].values())/1024
                summary.append([n,b,NAMES[backend],f'{good}/3',number(val('mean_us')),number(val('p50_us')),number(val('p99_us')),
                                number(median_metric(ds,'publish','mean_us')),number(median_metric(ds,'api','mean_us')),
                                number(median(ds,cpu)),number(median(ds,rss)),number(median(ds,lambda d:d['summary']['aggregate_receive_rate']))])
    lines.append(table(['SUB','载荷 B','后端','无异常轮','端到端均值','p50','p99','发布总均值','API均值','CPU %','RSS MiB','窗内聚合接收/s'],summary))
    lines += ['', '## 相同场景的观察差异','', '以下仅比较双方三轮均无异常的格。比值为 dzIPC 共享端点 ÷ Cyclone DDS，小于 1 表示本次 dzIPC 较低；不作为通用倍率。','']
    comparison=[]
    for n in (1,8,32):
        for b in (64,4096,1048576):
            cpp=groups[(n,b,'shared')]
            for backend in ('dds-udp','dds-iox'):
                dd=groups[(n,b,backend)]
                if len(cpp)==len(dd)==3 and all(d['status']=='ok' for d in cpp+dd):
                    ratios=[median(cpp,lambda d:d['summary'][group][key])/median(dd,lambda d:d['summary'][group][key]) for group,key in [('receive','p50_us'),('receive','p99_us'),('publish','mean_us')]]
                    comparison.append([n,b,NAMES[backend],*[number(x) for x in ratios]])
    lines.append(table(['SUB','载荷 B','对比对象','p50 比值','p99 比值','发布总耗时比值'],comparison))
    lines += ['', '## 全部逐轮账本','']
    ledger=[]
    for d in records:
        s=d.get('summary',{}); receive=s.get('receive',{})
        ledger.append([d['case']['id'],d['status'],s.get('accepted_total','—'),s.get('delivered_total','—'),
                       number(receive.get('p50_us')),number(receive.get('p99_us')),
                       '；'.join(s.get('errors',[])) or d.get('error','').splitlines()[-1:] or '无'])
    lines.append(table(['窗口','状态','接受','接收','p50 µs','p99 µs','异常'],ledger))
    lines += ['', '## 图表','', '![端到端延迟对照](latency.png)','',
              '图中点为三轮分位数的中位数，误差线为三轮最小值到最大值；纵轴为对数。只绘制三轮完整且无异常的格。','']
    (root/'results.md').write_text('\n'.join(lines))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.font_manager import FontProperties
    font=FontProperties(fname='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
    plt.rcParams['axes.unicode_minus']=False
    fig,axes=plt.subplots(2,3,figsize=(15,8),sharex=True)
    for col,n in enumerate((1,8,32)):
        for row,metric in enumerate(('p50_us','p99_us')):
            ax=axes[row,col]
            for backend in BACKENDS:
                xs=[]; ys=[]; lower=[]; upper=[]
                for x,b in enumerate((64,4096,1048576)):
                    ds=groups[(n,b,backend)]
                    if len(ds)!=3 or any(d['status']!='ok' for d in ds): continue
                    values=[d['summary']['receive'][metric] for d in ds]; m=statistics.median(values)
                    xs.append(x); ys.append(m); lower.append(m-min(values)); upper.append(max(values)-m)
                ax.errorbar(xs,ys,yerr=[lower,upper],marker='o',capsize=3,label=NAMES[backend])
            ax.set_yscale('log'); ax.set_title(f'{n} 个订阅者 · {metric[:-3]}',fontproperties=font)
            ax.set_ylabel('端到端延迟（µs）',fontproperties=font); ax.set_xticks([0,1,2],['64 B','4 KiB','1 MiB']); ax.grid(True,alpha=.3)
    axes[0,0].legend(prop=font,fontsize=8)
    fig.suptitle('当前 dzIPC 与 Cyclone DDS：100Hz 本机阻塞接收',fontproperties=font)
    fig.tight_layout(); fig.savefig(root/'latency.png',dpi=160); fig.savefig(root/'latency.svg')
    print(json.dumps({'windows':len(records),'totals':totals,'report':str(root/'results.md')},ensure_ascii=False))

if __name__=='__main__': main()
