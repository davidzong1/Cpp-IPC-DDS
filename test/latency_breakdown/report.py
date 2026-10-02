#!/usr/bin/env python3
"""审计分段计时并生成报告；均值来自同组关联样本，分位数不能相加。"""
import argparse
import collections
import hashlib
import importlib.util
import json
import pathlib
import statistics

HERE=pathlib.Path(__file__).resolve().parent
ROOT=HERE.parents[1]
spec=importlib.util.spec_from_file_location("breakdown_runner",HERE/"run.py")
assert spec is not None and spec.loader is not None
runner=importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
NAMES={"a":"DzFlatA","b":"DzFlatB","dds-iox":"DDS/iceoryx"}
SEGMENTS=["prepare","before_submit","before_worker","worker_after_submit","release","validate",
          "allocate","enqueue","queue_to_app","enter_to_take","take_to_app"]
LABELS=["准备","发布入口→提交前","提交前→处理开始","处理开始→取包","lease释放","校验","构造Sample","入队","队列→应用","DDS入口→take完成","take→应用"]

def med(v): return statistics.median(v) if v else None
def f(v,n=3): return "—" if v is None else f"{v:,.{n}f}"
def table(headers,rows):
    return "| "+" | ".join(headers)+" |\n|"+"|".join(["---"]*len(headers))+"|\n"+"".join(
        "| "+" | ".join(map(str,row))+" |\n" for row in rows)
def aggregate(ds,key,stat="mean"):
    return med([d["breakdown"]["segments"][key][stat] for d in ds if key in d.get("breakdown",{}).get("segments",{})])
def rate(d): return d["sub"]["received_in_window"]/d["duration"]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--work",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-breakdown-20261002"))
    ap.add_argument("--out",type=pathlib.Path,default=ROOT/"docs/transport_comparison_20261002/latency_breakdown.md")
    a=ap.parse_args()
    cases=[json.loads(p.read_text()) for p in sorted((a.work/"cases").glob("stage*.json"))]
    expected={(r,n,p,on,b) for r in (1,2,3) for n in (8,1024,65536)
              for p in (("w1","w8") if n==65536 else runner.PROFILES)
              for on in ((True,False) if n==8 else (True,)) for b in NAMES}
    actual={(d["repeat"],d["bytes"],d["profile"],d["trace"],d["backend"]) for d in cases}
    if actual!=expected or len(cases)!=len(expected):
        raise RuntimeError(f"矩阵不完整：已有 {len(cases)}，预计 {len(expected)}")
    binary=hashlib.sha256((a.work/"build/comparison").read_bytes()).hexdigest()
    for d in cases:
        assert d["binary_sha256"]==binary,d["id"]
        assert d["pub"]["attempts"]==d["pub"]["sent"]+d["pub"]["failed"],d["id"]
        assert d["sub"]["received_in_window"]<=d["sub"]["received"],d["id"]
        if d["trace"]:
            assert runner.summarize(a.work,d)==d["breakdown"],d["id"]
            assert not d["breakdown"]["nonmonotonic"],d["id"]
        if d["status"]=="ok":
            assert d["pub"]["sent"]==d["sub"]["received"] and d["sub"]["bad"]==d["sub"]["duplicate"]==0,d["id"]
    groups=collections.defaultdict(list)
    for d in cases: groups[(d["bytes"],d["profile"],d["trace"],d["backend"])].append(d)
    parts=["# 小消息分段计时分析\n",
           "本轮只进行计时与归因；产品代码和上一轮测试源码未修改，性能优化尚未实施。\n",
           "## 执行节点\n",
           table(["节点","内容","状态"],[
               ["L01","边界与采样方案","已完成"],["L02","独立计时构建及正确性预检","已完成"],
               ["L03",f"{len(cases)} 个正式组合","已完成"],["L04","逐样本关联、时序与计数审计","已完成"],
               ["L05","临时产物清理并等待后续指示","待清理"]]),
           "\n## 核心观察\n"]
    for b in ("a","b"):
        all_w8=groups[(8,"w8",True,b)];w1=groups[(8,"w1",True,b)]
        w8=[d for d in all_w8 if d["status"]=="ok"]
        total=aggregate(w8,"total")
        waiting=aggregate(w8,"before_worker")
        queue=aggregate(w8,"queue_to_app")
        share=med([100*d["breakdown"]["segments"]["before_worker"]["mean"]/d["breakdown"]["segments"]["total"]["mean"] for d in w8])
        parts.append(f"{NAMES[b]}，8 B、8 条在途，完整送达 {len(w8)}/3 轮：这些完整轮的抽样端到端均值中位数 {f(total)} µs，投递前到 worker 开始处理 {f(waiting)} µs，占各轮总均值的中位比例 {f(share,1)}%，worker 取包后到应用取得 {f(aggregate(w8,'recv_to_app'))} µs，其中应用队列交接 {f(queue)} µs。单条在途完整送达 {sum(d['status']=='ok' for d in w1)}/3 轮，异常轮只代表停止推进前的成功样本，不能当作正常稳态延迟。\n")
    parts.append("本轮修正此前优先怀疑应用队列交接的判断：当前小消息饱和工况的主要耗时在 worker 开始处理之前，应用队列交接只占较小部分。下一步定位优先看共享队列驻留与 worker 就绪/调度，不应直接把逐条 Sample 分配或应用队列作为主要瓶颈。等待占比不等于 CPU 热点占比：worker 的完整消费周期决定排队速度，Sample 构造等小段也可能通过降低服务能力放大排队；尚不能仅按分段延迟给出某个函数的 CPU 瓶颈排名。本轮没有实施优化。\n")
    for b in NAMES:
        ds=[d for d in groups[(8,"r500k",True,b)] if d["status"]=="ok"]
        parts.append(f"8 B、相同 50 万条/s 输入，{NAMES[b]} 完整轮 {len(ds)}/3，抽样总均值中位数 {f(aggregate(ds,'total'))} µs。\n")
    parts.append("归因应以以下分段表和相同输入速率对照为准。投递前→worker 开始包括共享队列驻留、worker 的前序工作、调度与通知等待；本轮没有直接采集内核唤醒时间，不能把整段称为 futex 唤醒耗时。单条在途与限速改变排队状态，不能将其低延迟宣称为生产路径优化收益。\n")
    bad=[d for d in cases if d["status"]!="ok"]
    parts.append(f"本轮共 {len(bad)} 个异常组合，探针关闭档也有 {sum(not d['trace'] for d in bad)} 个。对这些异常应先检查发/池收/应用收的关系；缺失消息没有到达时间，因而不会出现在已收到样本的延迟分位数中。较低的 p99 不能掩盖停止推进。\n")
    parts.append("\n## 方法与边界\n\n"
        "- 单机跨进程 CLOCK_MONOTONIC；8 B、1 KiB 覆盖 w1/w8/10万每秒/50万每秒，64 KiB 补充 w1/w8；每格 3 秒 × 3 轮。w1/w8 为无指定速率；限速档仍最多 8 条在途，并记录实际发送量。\n"
        "- 每 64 条按序号抽样，三轮采样相位不同；时间戳保存于各进程本地预分配数组，结束后按序号关联，不通过被测传输或共享控制区传递。预热 32 条排除。\n"
        "- CPP 的提交点是共享描述符 que->push 调用之前，载荷已写完；不是发布 API 返回，也不是精准的共享队列 release 指令。应用队列点紧邻槽位 sequence.store(release) 之前。\n"
        "- worker 起点复用接收池已有的每次 recv_once 起始时钟。若处理开始早于本条投递前时刻，将共同区间分解为提交前→max(提交前,处理开始)→取包，避免把正常并发记为负延迟；原始 worker_service 另列。\n"
        "- recv 点在 recv 返回后、释放 lease 前；为了按序号采样，探针先只读绑定视图，故该段含少量探针识别开销。校验段包括现有类型锁、消息/schema检查和接收计数；构造段包括 make_shared 和 Sample 构造。\n"
        "- 应用到达沿用原工装 consume 入口时钟；不包含完整载荷遍历。速度均值和分位数含原有逐条统计与应用处理对后续消息的影响。\n"
        "- DDS 保留原安装，只在 dds_take 批次调用前后打点。入口→take完成合并了发布、传输、内部排队及取样物化，不能与 CPP 的单独内核通知或出队成本直接等同。take批次耗时是诊断值，同一批次会关联到多个样本。\n"
        "- 各段在同一轮同一组样本上严格相加等于端到端均值；表格再分别取三轮中位数，因此表内中位数之和可能不等于总中位数。各段 p50/p99 不可相加。\n"
        "- 全程未绑核或修改频率策略，后台负载与 P/E 核调度会影响尾延迟。启闭探针对照能估计记录扰动，但关闭档仍包含空探针调用，不等于完全未插桩二进制。\n")
    parts.append("\n## 完成率与全量延迟\n")
    rows=[]
    for (n,p,on,b),ds in sorted(groups.items()):
        rows.append([n,p,"开" if on else "关",NAMES[b],"/".join(d["status"] for d in ds),
                     f(med([rate(d) for d in ds]),0),f(med([d["sub"]["latency_p50_us"] for d in ds])),
                     f(med([d["sub"]["latency_p99_us"] for d in ds])),
                     f(med([d["sub"]["latency_mean_us"] for d in ds])),f(max(d["sub"]["latency_max_us"] for d in ds)),
                     sum(d["pub"]["sent"]-d["sub"]["received"] for d in ds),
                     sum(d["sub"]["bad"]+d["sub"]["duplicate"] for d in ds)])
    parts.append(table(["字节","负载","探针","后端","三轮状态","窗内条/s","全量p50 µs","全量p99 µs","全量均值 µs","三轮最大 µs","总未收","坏样本+重复"],rows))
    parts.append("\n## CPP 分段均值（仅完整送达轮的中位数，µs）\n")
    rows=[]
    for (n,p,on,b),ds in sorted(groups.items()):
        if not on or b=="dds-iox": continue
        ds=[d for d in ds if d["status"]=="ok"]
        rows.append([n,p,NAMES[b],f"{len(ds)}/3",sum(d["breakdown"]["matched"] for d in ds)] +
                    [f(aggregate(ds,k)) for k in SEGMENTS[:9]]+[f(aggregate(ds,"total"))])
    parts.append(table(["字节","负载","后端","完整轮","关联样本"]+LABELS[:9]+["总计"],rows))
    parts.append("\n## DDS 分段均值（仅完整送达轮的中位数，µs）\n")
    rows=[]
    for (n,p,on,b),ds in sorted(groups.items()):
        if not on or b!="dds-iox": continue
        ds=[d for d in ds if d["status"]=="ok"]
        rows.append([n,p,f"{len(ds)}/3",sum(d["breakdown"]["matched"] for d in ds)]+
                    [f(aggregate(ds,k)) for k in ("prepare","enter_to_take","take_to_app","total","take_batch")])
    parts.append(table(["字节","负载","完整轮","关联样本","准备","入口→take完成","take→应用","总计","take批次诊断"],rows))
    parts.append("\n## 停止推进与未收证据\n\n该表保留所有非完整运行。池统计与应用接收相同而小于发送时，缺口在池取包统计之前，不能归因于 Sample 入队后的应用交接。探针关闭仍出现异常可排除“必须开启计时才发生”，但不等于已锁定根因。\n")
    parts.append(table(["case","探针","发","池收","应用收","未收","完整性错误","池等待超时"],[
        [d["id"],"开" if d["trace"] else "关",d["pub"]["sent"],d["sub"]["pool_messages"],d["sub"]["received"],
         d["pub"]["sent"]-d["sub"]["received"],d["sub"]["bad"]+d["sub"]["duplicate"],d["sub"]["pool_wait_timeouts"]]
        for d in bad]))
    parts.append("候选代码位置：src/dzIPC/threepools/recv_worker.cc 中空读后将 last_seq 推进到当下 sequence，同时 SHM 适配器的 has_pending() 恒为 false。若新的投递发生在空读与推进序号之间，存在需要验证的就绪变化被吸收窗口；单条在途随后没有新消息触发再次处理，与本次停止推进的表现相符。这里只提出代码级假设，尚未用确定性交错或反事实修复证明，未修改该实现。\n")
    parts.append("\n## 启闭探针对照（8 B）\n\n同一配置分别取三轮中位数；数值含运行波动，不能直接当作精确的探针指令成本。\n")
    rows=[]
    for p in runner.PROFILES:
        for b in NAMES:
            on=[d for d in groups[(8,p,True,b)] if d["status"]=="ok"];off=[d for d in groups[(8,p,False,b)] if d["status"]=="ok"]
            if not on or not off: continue
            r_on=statistics.median([rate(d) for d in on]);r_off=statistics.median([rate(d) for d in off])
            rows.append([p,NAMES[b],f"{len(on)}/{len(off)}",f(r_on,0),f(r_off,0),f(100*(r_on/r_off-1),2),
                f(med([d["sub"]["latency_p50_us"] for d in on])),
                f(med([d["sub"]["latency_p50_us"] for d in off]))])
    parts.append(table(["负载","后端","开/关完整轮","开 条/s","关 条/s","速率变化%","开p50 µs","关p50 µs"],rows))
    parts.append("8 B 饱和档的探针开启会扰动测量：应结合上表的吞吐变化和全量 p50 解读各段绝对值，不能将计时数值视为完全无扰动的服务耗时。固定输入速率下也必须同时核对送达完整性。\n")
    parts.append("\n## 逐轮账本\n\n以下保留每轮分段均值与总分位数，供清理中间 CSV 后核对。分段汇总只含关联成功的样本；未关联和不单调计数单列，不能悄悄丢弃。\n")
    rows=[]
    for d in cases:
        bd=d.get("breakdown",{});sg=bd.get("segments",{})
        rows.append([d["id"],d["status"],d["pub"]["sent"],d["sub"]["received"],d["sub"]["bad"],d["sub"]["duplicate"],
                     bd.get("sampled_sent","—"),bd.get("matched","—"),len(bd.get("incomplete",[])),len(bd.get("nonmonotonic",[])),
                     f(d["sub"]["latency_p50_us"]),f(d["sub"]["latency_p99_us"])] +
                     [f(sg.get(k,{}).get("mean")) for k in SEGMENTS+["total","worker_service","publish_call"]] +
                     [f(d["pub"]["cpu_seconds"]),f(d["sub"]["cpu_seconds"])])
    parts.append(table(["case","状态","发","收","坏","重复","抽样发","关联","缺段","时序异常","全量p50","全量p99"]+
                       LABELS+["抽样总均值","worker服务","发布调用","发CPU秒","收CPU秒"],rows))
    parts.append("\n## 复现与指纹\n\n~~~bash\nbash test/latency_breakdown/execute.sh /var/tmp/my-breakdown\n~~~\n")
    env=json.loads((a.work/"environment.json").read_text())
    parts.append("~~~json\n"+json.dumps(env,ensure_ascii=False,indent=2)+"\n~~~\n")
    parts.append("插桩清单：\n\n~~~json\n"+(a.work/"instrumentation.json").read_text()+"\n~~~\n")
    parts.append(table(["交付文件","SHA-256"],[(p.name,hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(HERE.iterdir()) if p.is_file()]))
    a.out.write_text("\n".join(parts))
    print(f"审计及报告完成：{len(cases)} 条，{sum(d.get('breakdown',{}).get('matched',0) for d in cases)} 个关联样本。")

if __name__=="__main__":
    main()
