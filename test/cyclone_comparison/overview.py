#!/usr/bin/env python3
"""汇总冻结矩阵及环境复测，展示完整性、延迟、速度和 topic 规模。"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
from run import dump, fingerprint
from report import NAMES, table

STAGES=[('path-probe','传输路径探针'),('latency','单发布阻塞延迟'),('multi-per-publisher','多发布：每发布者100Hz'),('multi-fixed-total','原固定总800Hz'),('multi-32sub','原多发布32订阅'),('speed','既有全尺寸速度'),('stress-installed','原安装 topic 压力'),('stress-scaled','扩容 topic 压力'),('verification-multi-fixed-total','环境复测：固定总800Hz'),('verification-multi-32sub','环境复测：多发布32订阅')]
LEGACY_NAMES={**NAMES,'shm':'dzIPC SHM/TLV','socket':'dzIPC 直接Socket/TLV','a':'dzIPC DZFlat A','b':'dzIPC DZFlat B','prebuilt':'dzIPC 预构造段'}
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();root=a.directory.resolve()
records={};stage_rows=[];overview={};total=Counter()
for name,label in STAGES:
    folder=root/name;m=json.loads((folder/'manifest.json').read_text());jobs={c['id']:c for c in m['jobs']};w=m['windows']
    assert set(jobs)=={x['id'] for x in w} and len(jobs)==len(w)
    audit=json.loads((folder/'audit.json').read_text()) if (folder/'audit.json').exists() else {}
    derived={x['id']:x['summary'] for x in audit.get('audits',[])}
    rr=[];counts=Counter();backend=defaultdict(Counter);noise=0
    for x in w:
        c=jobs[x['id']];path=folder/x['id']/'result.json';d=json.loads(path.read_text()) if path.exists() else {'status':'failed'}
        assert d['status']==x['status'];counts[d['status']]+=1;backend[c['backend']][d['status']]+=1
        r={'case':c,'status':d['status'],'summary':derived.get(x['id'],d.get('summary'))}
        r.update({k:d[k] for k in ('pub','sub','publisher','receivers','duration','cpu_seconds','resource_seconds') if k in d});rr.append(r)
        if 'competing_activity' in x:noise+=len(x['competing_activity'])
        else:noise+=sum(p['comm'] in ('cc1plus','cc1','make','ninja','cmake','xproc_benchmark','git-maintenance') for snap in x['activity'] for p in snap['processes'])
    records[name]=rr;total.update(counts)
    coverage='含 Git 维护监测' if name.startswith(('stress-','verification-')) else '旧监测，未覆盖 Git 维护'
    if name in ('multi-fixed-total','multi-32sub'):coverage='潜在受干扰；整批复测另列'
    overview[name]={'windows':len(w),'statuses':dict(counts),'by_backend':{k:dict(v) for k,v in backend.items()},'known_competing_observations':noise,'coverage':coverage,'manifest_sha256':fingerprint(folder/'manifest.json'),'audit_sha256':fingerprint(folder/'audit.json') if audit else None}
    stage_rows.append([f'[{label}]({name}/results.md)',len(w),counts['ok'],counts['degraded'],counts['failed'],coverage])
assert sum(x['windows'] for x in overview.values())==920
for n in ('verification-multi-fixed-total','verification-multi-32sub'):assert overview[n]['known_competing_observations']==0

def med(ds,f):return statistics.median(f(d) for d in ds)
def fmt(x):return '—' if x is None else f'{x:,.3f}'
def select(stage,**keys):return [d for d in records[stage] if all(d['case'][k]==v for k,v in keys.items())]
def good(ds):return f"{sum(d['status']=='ok' for d in ds)}/{len(ds)}"

latency=[]
for ns,b in ((1,4096),(8,4096),(32,64),(32,1048576)):
    for backend in ('shared','dds-udp','dds-iox'):
        ds=select('latency',subscribers=ns,bytes=b,backend=backend)
        latency.append([ns,b,NAMES[backend],good(ds),*[fmt(med(ds,lambda d:d['summary'][group][key])) for group,key in [('publish','mean_us'),('receive','p50_us'),('receive','p99_us')]]])

fixed=records['verification-multi-fixed-total'];integrity=[];publication=[]
for np in (1,4,8):
    for backend in ('shared','dds-udp','dds-iox'):
        ds=select('verification-multi-fixed-total',publishers=np,backend=backend)
        integrity.append([np,NAMES[backend],good(ds),sum(d['publisher']['rejected'] for d in ds),sum(x['missing_accepted'] for d in ds for x in d['summary']['delivery']),sum(x['duplicates'] for d in ds for x in d['summary']['delivery'])])
        ds=select('verification-multi-fixed-total',publishers=np,bytes=4096,backend=backend)
        values=[fmt(med(ds,lambda d:d['summary'][g][k])) if all(d['status']=='ok' for d in ds) else '—' for g,k in [('publish','mean_us'),('receive','p50_us'),('receive','p99_us')]]
        publication.append([np,NAMES[backend],good(ds),*values])

speed=[]
for b in (64,4096,1048576):
    for backend in ('shm','prebuilt','shared','dds-udp','dds-iox'):
        ds=select('speed',bytes=b,backend=backend);ok=all(d['status']=='ok' for d in ds)
        speed.append([b,LEGACY_NAMES[backend],good(ds),fmt(med(ds,lambda d:d['sub']['received_in_window']/d['duration'])) if ok else '—',fmt(med(ds,lambda d:d['pub']['prepare_mean_us'])) if ok else '—',fmt(med(ds,lambda d:d['pub']['send_mean_us'])) if ok else '—'])

stress=[]
for stage,version in [('stress-installed','原安装'),('stress-scaled','同版本扩容')]:
    for backend in ('shm','shared','dds-udp','dds-iox'):
        ds=select(stage,topics=1000,backend=backend);have=all('pub' in d and 'sub' in d for d in ds)
        stress.append([version,LEGACY_NAMES[backend],good(ds),fmt(med(ds,lambda d:d['sub']['received_in_window']/d['duration'])) if have else '未初始化',f"{med(ds,lambda d:d['pub']['sent']-d['sub']['received']):,.0f}" if have else '—',fmt(med(ds,lambda d:d['sub']['latency_p99_us'])) if have else '—'])

shared_fixed=[d for d in fixed if d['case']['backend']=='shared']
text=['# 当前 dzIPC 与 Cyclone DDS 本机 Benchmark 总报告','',
      '测试于 2026-10-07 开始、10-08 完成。生产源码固定为 131f37c5；分支 bench/cyclone-current-20261007。只修改测试工装和文档，未修改生产实现。',
      '本次共完成 **920 个窗口：原计划 848 个，加环境补正后整批复测 72 个**。原始 CSV、逐轮结果、库与二进制哈希、环境记录均保留；开发冒烟另存，不混入正式结果。','',
      '单发布、本机阻塞接收下，共享端点的延迟多数场景低于本机安装的 Cyclone DDS 两档。但同话题多发布仍存在接受后缺失、重复及 loan 池耗尽，不能用较短的发布返回时间掩盖交付失败。1000 topic 工况中，纯 SHM 和扩容 iceoryx 达成每秒百万条目标，共享端点与 DDS UDP 未达目标。','',
      '## 完成范围与证据索引','',
      table(['阶段','完成窗','无异常','降级','失败','环境边界'],stage_rows),
      '“无异常”是该窗口的完整性和输入时序检查结果；不是历史延迟门槛验收，也不表示完全无系统噪声。降级包含迟发、未达输入或交付缺失；初始化失败与发布拒绝保留为失败，并在逐轮账本中细分。','',
      '## 单发布阻塞延迟','',
      '100Hz，1/8/32 SUB，64B/4KiB/1MiB，2秒预热、10秒正式、3轮。下表是三轮指标中位数，单位 µs；异常格列明通过轮数，只描述观察值。完整表含端到端均值、发布 API 与总耗时、CPU、RSS，见[单发布报告](latency/results.md)。',
      table(['SUB','载荷 B','后端','无异常轮','发布总均值','端到端 p50','端到端 p99'],latency),
      '108 窗全部接受 108,000 条发布、完整交付 1,476,000 条订阅样本。DDS UDP 的 32 SUB/1MiB 三轮均出现发布迟发，因此 105/108 窗无异常。共享端点并非所有指标都更优：例如 1 SUB/64B 的 p99 为 158.868µs，iceoryx 为 149.578µs。',
      '![单发布延迟](latency/latency.png)','',
      '## 真正多发布、多订阅','',
      '各发布者为独立进程，使用共同单调时钟起点；消息按（发布者ID、独立序号）核对，记录实际发布区间的重叠，最大同时调用达到 4/8。固定每发布者100Hz与固定总800Hz分别报告，另测4/8 PUB×32 SUB。纯 IPC_SHM 的同话题多发布不在本次这组三后端对照内，不把共享端点结论直接推广到它。',
      '每发布者100Hz的162窗中，共享端点31/54无异常，DDS UDP48/54，iceoryx54/54。DDS UDP在高负载1MiB并发组合也出现迟发或交付不足，详见[全部逐轮记录](multi-per-publisher/results.md)；不能把下面小载荷固定总速率的通过结果推广到所有负载。',
      '以下使用补充 Git 维护监测后的固定总800Hz复测，均为8 SUB；每格合并64B/4KiB、各3轮的完整性检查。计数按消息身份集合计算，重复单列，不只用发送减接收估算缺失。',
      table(['PUB','后端','无异常窗','拒绝发布','缺失的已接受投递','重复接收'],integrity),
      f"共享端点共 {sum(d['status']=='ok' for d in shared_fixed)}/18 无异常；DDS UDP、iceoryx 各18/18无异常。共享端点异常窗包含 {sum(d['publisher']['rejected'] for d in shared_fixed):,} 次拒绝发布及 {sum(x['missing_accepted'] for d in shared_fixed for x in d['summary']['delivery']):,} 条缺失的已接受投递。日志记录 loan 池容量10及池耗尽；这些是症状证据，尚不能单独确定内部根因。",
      '![多发布完整性](concurrency-integrity.png)',
      '同一总输入、4KiB、8 SUB 的发布耗时与延迟如下；只展示三轮均无异常的性能格。失败窗口的全部尝试耗时仍保留在[详细报告](verification-multi-fixed-total/results.md)，不能把快速拒绝解释成性能收益。',
      table(['PUB','后端','无异常轮','发布总均值 µs','端到端 p50 µs','端到端 p99 µs'],publication),
      '固定总速率控制平均输入量，同步发布形成4/8条微突发，瞬时到达形状与 CPU 唤醒节奏并未固定。上述差异不能单独归因于共享网关、锁竞争或 socket 数量；本机共享端点的数据路径也不能等同远端 UDP 路径。','',
      '## 既有速度工况','',
      '保留8B～1MiB共18档、最多8条在途、32次预热、每格1秒×3轮。每条重新准备载荷，dzIPC主动取样、DDS原生回调；与上面的阻塞延迟口径不同。下表仅展示全部三轮无异常的格，速率为本工况窗内完成消息数/秒。',
      table(['载荷 B','后端','无异常轮','窗内接收/s','准备均值 µs','API均值 µs'],speed),
      '432窗中419无异常、12降级、1失败。直接Socket/TLV在128KiB～1MiB的三轮均有交付异常，包含缺失及部分载荷/序号异常；DDS UDP的2KiB第三轮因端口53151被占用而初始化失败，属于环境冲突，不记作零吞吐。此项没有三轮完整有效性能比较。',
      '![速度对照](speed.png)','',
      '## 1000 topic 压力','',
      '每话题64B、1000条/秒，1000话题总目标为每秒100万条、10秒1000万条。4个发布线程分配不同话题，一个接收进程；这与同话题多发布进程不同。下表是三轮中位数，包含未达输入的窗口；最终缺失列只核对已实际发送部分。延迟是原工装每话题每100条的确定性抽样，不是全量分位数。',
      table(['依赖','后端','无异常轮','窗内接收/s','实际发送减最终接收','抽样 p99 µs'],stress),
      '原安装iceoryx在1000端点组合无法初始化；同版本将端点/通知器上限提高到2048并重建RouDi/C binding/DDS后，三轮达成目标。两组各自完整交付所有实际发送消息；共享端点和DDS UDP的异常是输入计划未完成。依赖重建也可能引入编译配置差异，不能把所有性能变化都归因于容量上限。',
      '完整1/100/1000 topic表、逐轮输入遗漏、CPU/线程与原始加载库见[原安装](stress-installed/results.md)和[扩容](stress-scaled/results.md)。压力表的CPU列是接收进程CPU，不能当框架总CPU；服务和发布进程资源另见逐窗记录。','',
      '## 测量边界与环境补正','',
      '- 机器为 Intel Core i9-14900KF，32个逻辑CPU；未绑核、未改电源策略或 sysctl，保留宿主后台活动。使用私有 SHM/IPC/mount 命名空间，网络命名空间共享，因此仍可能有端口冲突。具体环境按各阶段 manifest，不能宣称无任何外部噪声。',
      '- dzIPC使用预构造StdImage DZFlat；DDS使用固定数组。逻辑载荷相同，编码元数据和底层缓冲不同。DDS借样/复制计入发布总耗时；API列不能代表全部发布成本。CPU按实际测量秒数和进程范围计算，RSS求和包含共享映射重复计数。',
      '- DDS使用RELIABLE/VOLATILE/KEEP_LAST(64)。应用队列深度相同不等于底层loan池容量或可靠性契约相同；这是当前配置对照，不是协议等价或等内存预算实验。',
      '- [路径探针](path-probe/results.md)确认UDP档能捕获测试载荷，iceoryx档未见该UDP载荷且发布借样位于SHM；接收应用指针不在SHM，不能称端到端零拷贝。探针不纳入性能。',
      '- 提交证据曾触发Git后台打包，旧监测未覆盖；原固定总速率/32SUB两组保守标为潜在受干扰并完整复测，原数据不删除。后续提交禁用自动维护，新监测加入Git维护进程，72个复测窗均未记录已知竞争活动。包文件时间戳不是精确CPU活动区间；此前阶段保留旧监测覆盖范围，详见[环境记录](git-maintenance-interference.json)。',
      '- 所有非零退出但完整保存CSV的多发布窗口也已复算；速度原始时间值共1293组已核对，展示层修订通过git核对输入与已提交审计不变后复用该审计。各阶段audit.json明确记录复算范围和报告哈希。',
      '- 本次不包含跨机或RPC，也不替代历史54窗口验收。C1通知候选未合入，历史三项延迟门槛仍未完成D12关闭。','',
      '后续优先诊断共享端点同话题多发布下的loan所有权/归还、接受序号与扇出记录；再区分千话题的发布计划不足与处理瓶颈。此次只测量和补齐证据，没有据此修改生产实现。','',
      '复现入口：[范围与命令](README.md)、[原始计划](execution-plan.json)、[完整复测计划](verification-plan.json)、[构建与预检](provenance/README.md)。图表有同名SVG可导出；机器可读汇总见[overview.json](overview.json)。','']
(root/'results.md').write_text('\n\n'.join(x.strip('\n') for x in text if x)+'\n')
dump(root/'overview.json',{'production_commit':'131f37c5','total_windows':920,'statuses':dict(total),'stages':overview,'generator_sha256':fingerprint(Path(__file__))})

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
font=FontProperties(fname='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
plt.rcParams['axes.unicode_minus']=False
fig,axes=plt.subplots(1,3,figsize=(13,4),sharey=True)
for ax,backend in zip(axes,('shared','dds-udp','dds-iox')):
    bottom=[0,0,0]
    for status,color,label in [('ok','#2ca02c','无异常'),('degraded','#e6a23c','缺失/降级'),('failed','#d9534f','失败')]:
        values=[sum(d['status']==status for d in select('verification-multi-fixed-total',publishers=n,backend=backend)) for n in (1,4,8)]
        ax.bar([0,1,2],values,bottom=bottom,color=color,label=label)
        bottom=[x+y for x,y in zip(bottom,values)]
    ax.set_xticks([0,1,2],['1 PUB','4 PUB','8 PUB']);ax.set_ylim(0,6.5);ax.set_yticks(range(7));ax.set_title(NAMES[backend],fontproperties=font);ax.grid(axis='y',alpha=.2)
axes[0].set_ylabel('窗口数（每格6窗）',fontproperties=font);axes[-1].legend(prop=font)
fig.suptitle('总输入800Hz、8订阅者：64B/4KiB各三轮，环境监测补正后',fontproperties=font)
fig.tight_layout();fig.savefig(root/'concurrency-integrity.png',dpi=160);fig.savefig(root/'concurrency-integrity.svg');plt.close(fig)
fig,ax=plt.subplots(figsize=(11,5))
sizes=[8<<i for i in range(18)]
for backend in ('shm','socket','a','b','prebuilt','shared','dds-udp','dds-iox'):
    ys=[]
    for b in sizes:
        ds=select('speed',bytes=b,backend=backend)
        ys.append(med(ds,lambda d:d['sub']['received_in_window']/d['duration']) if len(ds)==3 and all(d['status']=='ok' for d in ds) else math.nan)
    ax.plot(sizes,ys,marker='.',label=LEGACY_NAMES[backend])
ax.set_xscale('log',base=2);ax.set_yscale('log');ax.set_xlabel('逻辑载荷（B）',fontproperties=font);ax.set_ylabel('窗内接收消息数/s',fontproperties=font);ax.grid(alpha=.25)
ax.set_title('既有速度工况：最多8条在途，只展示三轮均无异常的格',fontproperties=font);ax.legend(prop=font,ncol=2)
fig.tight_layout();fig.savefig(root/'speed.png',dpi=160);fig.savefig(root/'speed.svg');plt.close(fig)
print(json.dumps({'windows':920,'statuses':dict(total),'report':str(root/'results.md')},ensure_ascii=False))
