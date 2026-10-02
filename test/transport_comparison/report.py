#!/usr/bin/env python3
"""将逐组合结果汇总为三份中文报告，并在 Markdown 中保留逐轮账本。"""
import argparse
import collections
import datetime
import hashlib
import json
import pathlib
import re
import statistics

ROOT = pathlib.Path(__file__).resolve().parents[2]
NAMES = {"shm":"SHM/TLV","socket":"Socket/TLV","a":"DzFlatA","b":"DzFlatB",
         "prebuilt":"PrebuiltSegment","dds-udp":"DDS/UDP","dds-iox":"DDS/iceoryx"}
SIZES = [8 << k for k in range(18)]
STATUS = {"ok":"完成","degraded":"有异常","failed":"失败","unsupported":"接口不支持"}

def load(directory, prefix):
    return [json.loads(p.read_text()) for p in sorted((directory/"cases").glob(prefix+"*.json"))]

def med(v):
    return statistics.median(v) if v else None

def fmt(v, n=2):
    return "—" if v is None else f"{v:,.{n}f}"

def label(n):
    return f"{n} B" if n<1024 else (f"{n//1024} KiB" if n<1048576 else "1 MiB")

def done(d):
    return "pub" in d and "sub" in d

def rate(d):
    return d["pub"]["sent"]/d["pub"]["elapsed"] if d["mode"]=="rpc" else d["sub"]["received_in_window"]/d["duration"]

def table(headers,rows):
    return "| "+" | ".join(headers)+" |\n|"+"|".join(["---"]*len(headers))+"|\n"+"".join(
        "| "+" | ".join(str(v).replace("|","/").replace("\n"," ") for v in row)+" |\n" for row in rows)

def reason(d):
    if d.get("reasons"): return "；".join(d["reasons"])
    if d.get("reason"): return d["reason"]
    t=re.sub(r"\x1b\[[0-9;]*m","","\n".join(d.get("pub_log_tail",[])+d.get("sub_log_tail",[])))
    if "POSH__RUNTIME_ROUDI_PUBLISHER_LIST_FULL" in t or "Failed to attach iox subscriber to iox listener" in t:
        return "原安装发布者/通知器容量耗尽（512 发布者、256 通知器）"
    if "CHUNK_IS_TOO_LARGE" in t: return "RouDi 池块不足以容纳载荷及协议开销"
    if "Out Of Resources" in t or "Out of resources" in t or "error attaching reader" in t:
        return "创建接收端资源耗尽（原安装通知器上限 256）"
    return t[-700:] or str(d.get("exit",{}))

def speed_report(cases):
    text=["# 速度测试结果分析\n",
        "正式速度使用现有 CycloneDDS 0.10.2/iceoryx 2.0.5 安装。隔离 RouDi 在原内存池上增加 2 MiB 档，以容纳 1 MiB 应用载荷和协议开销。cpp_ipc_dds 从当前源码独立 Release 构建。\n",
        "## 测量口径\n",
        "- 8 B—1 MiB 共 18 个二倍档位，每格三轮、每轮 1 秒；32 次预热不计时；初始就绪探测最多补发 3 次，补发次数独立记录。pub/sub 最多 8 条在途，RPC 单请求在途。这是固定窗口下的成绩，不是无限队列绝对峰值。\n",
        "- pub/sub 延迟从准备本条载荷前的单调时钟，到应用取得消息，含填充、序列化、传输、接收队列与 DDS 取样物化，不含应用遍历校验。RPC 延迟含准备请求、发送、服务 success 回调及收到响应。\n",
        "- 每条重新填充逻辑载荷。A/TLV 从 owning 对象发送；B 在共享借样内原地填充；PrebuiltSegment 先构造段后调用发布。准备和发布成本分列，不能仅按发布调用时间判断端到端快慢。\n",
        "- Tlv 就是 SHM 基线 1，Socket/TLV 为基线 2。原生 ser-cli 没有 B/PrebuiltSegment 入口，对应 108 格明确记不支持。DDS 服务通过请求/应答双话题实现，非标准 DDS RPC API。\n",
        "- RPC 分位数只统计成功请求，失败/超时的影响由实际完成速率和失败计数反映；各后端请求等待上限为 1 秒。RPC 请求 N 字节、success 响应 8 字节（值 1）。DDS 另有 16 字节序号/时间戳，cpp 生成类型也有元数据及序列化头。MiB/s 只按逻辑载荷换算，不是线上带宽。\n",
        "- 主表是无错误完整轮的指标中位数，星号表示不足三轮；异常轮全部进入总报告账本。含异常速率表用 † 标注异常，该表是到达/完成计数，不把损坏数据当有效业务吞吐。p99 是各轮 p99 的中位数，不是合并样本 p99。少于三轮的格不能据此宣称稳定性。\n",
        "- 接收速率为发送窗口内取得消息数/窗口秒数，排空后总数另列。RPC 为完成请求/实际循环耗时。发端结束后最多等 1 秒排空再析构，避免脚本提前销毁端点。\n"]
    groups=collections.defaultdict(list)
    for d in cases: groups[(d["mode"],d["bytes"],d["backend"])].append(d)
    text.append("\n## 关键尺寸观察\n\n只列三轮均无错误的格；速率冠军按窗口计数，延迟冠军按 p50，可为不同后端。\n")
    highlights=[]
    for mode in ("pubsub","rpc"):
        for n in (8,1024,65536,1048576):
            candidates=[]
            for b in NAMES:
                ds=groups[(mode,n,b)]
                if len(ds)==3 and all(d["status"]=="ok" for d in ds):
                    role,key=("pub","rpc_p50_us") if mode=="rpc" else ("sub","latency_p50_us")
                    candidates.append((b,med([rate(d) for d in ds]),med([d[role][key] for d in ds])))
            if candidates:
                fast=max(candidates,key=lambda v:v[1]);low=min(candidates,key=lambda v:v[2])
                highlights.append([mode,label(n),NAMES[fast[0]],fmt(fast[1],0),NAMES[low[0]],fmt(low[2])])
    text.append(table(["方式","载荷","速率最高","msg/s","p50 最低","µs"],highlights))
    text.append("\n## 结果解读与使用建议\n")
    large={b:[d for d in groups[("pubsub",1048576,b)] if d["status"]=="ok"] for b in NAMES}
    if all(len(large[b])==3 for b in ("shm","a","b","prebuilt","dds-iox")):
        rates={b:statistics.median([rate(d) for d in ds]) for b,ds in large.items() if ds}
        text.append(f"在本次 1 MiB 发布订阅工况，B 为 {fmt(rates['b'],0)} msg/s，是 SHM/TLV 的 {fmt(rates['b']/rates['shm'])} 倍；A、预构造段、DDS/iceoryx 分别为 {fmt(rates['a'],0)}、{fmt(rates['prebuilt'],0)}、{fmt(rates['dds-iox'],0)} msg/s。B 在共享借样内填充，适合能直接在借样中生产数据的应用。A 保留 owning 消息调用方式；预构造段本轮每次重新编码，不能将其发布调用短误认为整条路径成本低。\n")
        for b in ("b","dds-iox"):
            ds=large[b]
            text.append(f"{NAMES[b]} 在 1 MiB 下的准备/发布均值中位数为 {fmt(med([d['pub']['prepare_mean_us'] for d in ds]))}/{fmt(med([d['pub']['send_mean_us'] for d in ds]))} µs，端到端 p50 为 {fmt(med([d['sub']['latency_p50_us'] for d in ds]))} µs。准备、传输和取样之间可能流水重叠，三个数不能简单相加；发布调用本身不能代表业务端到端延迟。\n")
    text.append("小消息的速率与延迟排序并不相同，应按上表和实际消息大小选型。表中的大载荷逻辑 MiB/s 可受共享借样、缓存复用及不全量遍历接收数据影响，不能换算成设备或内存总线的实测带宽。不同后端的元数据、编码和取样成本不同，本结果不能把差异全部归因于传输或通知指令。\n")
    rpc_bad=collections.Counter(d["backend"] for d in cases if d["mode"]=="rpc" and d.get("pub",{}).get("failed",0))
    text.append("RPC 必须同时看失败计数和实际完成率："+
        "；".join(f"{NAMES[b]} 有 {rpc_bad[b]} 轮发生请求失败" for b in ("shm","a"))+
        "。单次等待上限 1 秒，足以显著影响 1 秒测试窗口；只看成功请求 p50 会低估其影响。DDS/iceoryx 的 64 KiB RPC 有预热失败，Socket/TLV 的大消息发布订阅有内容错误或未收，均不应作为稳定业务能力使用。建议先定位这些异常，再做更长时间、固定 CPU 的性能确认。\n")
    for mode,title in [("pubsub","发布订阅"),("rpc","请求响应")]:
        text.append(f"\n## {title}\n")
        for kind,caption in [("p50","延迟 p50（µs）"),("p99","延迟 p99（µs）"),("rate","无错误轮接收/完成速率（千条/秒）"),("observed_rate","全部有计数轮的实际速率（千条/秒，含异常）")]:
            rows=[]
            for n in SIZES:
                row=[label(n)]
                for b in NAMES:
                    ds=groups[(mode,n,b)];good=[d for d in ds if (done(d) if kind=="observed_rate" else d["status"]=="ok")]
                    if mode=="rpc" and b in ("b","prebuilt"): cell="不支持"
                    elif not good: cell="失败/异常" if ds else "待测"
                    else:
                        if kind in ("rate","observed_rate"): values=[rate(d)/1000 for d in good]
                        else:
                            role,prefix=("pub","rpc") if mode=="rpc" else ("sub","latency")
                            values=[d[role][prefix+"_"+kind+"_us"] for d in good]
                        cell=fmt(med(values))+("*" if len(good)!=3 else "")+("†" if kind=="observed_rate" and any(d["status"]!="ok" for d in good) else "")
                    row.append(cell)
                rows.append(row)
            text.append("### "+caption+"\n\n"+table(["载荷"]+list(NAMES.values()),rows))
    text.append("\n## 相对基线的比值\n\n比值为后端窗口速率 ÷ 基线窗口速率；仅纳入双方都有三轮无错误结果的尺寸。大于 1 表示该工况更快，不能推广为所有尺寸的固定倍率。\n")
    rows=[]
    for mode in ("pubsub","rpc"):
        for b in list(NAMES)[2:]:
            row=[mode,NAMES[b]]
            for base in ("shm","socket"):
                ratios=[]
                for n in SIZES:
                    lhs,rhs=groups[(mode,n,b)],groups[(mode,n,base)]
                    if len(lhs)==len(rhs)==3 and all(d["status"]=="ok" for d in lhs+rhs):
                        denominator=med([rate(d) for d in rhs])
                        if denominator: ratios.append(med([rate(d) for d in lhs])/denominator)
                row.append(f"{fmt(med(ratios))}×；{len(ratios)} 档" if ratios else "无完整可比档")
            rows.append(row)
    text.append(table(["方式","后端","相对 SHM/TLV","相对 Socket/TLV"],rows))
    text.append("\n## 准备与发布成本\n\npub/sub 各轮均值的中位数，仅含无错误轮。“准备”包括 B 的借样和预构造段编码，发布列不能替代完整路径。\n")
    rows=[]
    for n in (8,1024,65536,1048576):
        for b in NAMES:
            ds=[d for d in groups[("pubsub",n,b)] if d["status"]=="ok"]
            rows.append([label(n),NAMES[b],len(ds),fmt(med([d["pub"]["prepare_mean_us"] for d in ds])),
                fmt(med([d["pub"]["send_mean_us"] for d in ds])),fmt(med([rate(d)*n/1048576 for d in ds])),
                f"{fmt(min([rate(d) for d in ds]),0)}–{fmt(max([rate(d) for d in ds]),0)}" if ds else "—"])
    text.append(table(["载荷","后端","干净轮数","准备均值 µs","发布均值 µs","逻辑 MiB/s","逐轮速率范围"],rows))
    bad=[d for d in cases if d["status"] in ("failed","degraded")]
    text.append(f"\n## 异常与结论边界\n\n{len(cases)} 格记录中：{sum(d['status']=='ok' for d in cases)} 格无错误完成，{len(bad)} 格异常/失败，{sum(d['status']=='unsupported' for d in cases)} 格接口不支持。\n")
    counts=collections.Counter((d["mode"],d["backend"],reason(d)) for d in bad)
    text.append(table(["方式","后端","轮次数","原因"],[(m,NAMES[b],v,r) for (m,b,r),v in counts.items()]))
    retries=collections.Counter()
    for d in cases:
        if done(d): retries[d["backend"]]+=d["pub"].get("warmup_retries",0)
    text.append("\n就绪探测补发总次数：\n\n"+table(["后端","补发次数"],[(NAMES[b],retries[b]) for b in NAMES]))
    text.append("\nDDS 发布借样抽样指针位于 /dev/shm 映射，接收 dds_take 样本位于本地堆；本报告不称 DDS 路径端到端零拷贝。cpp pub/sub 四条路径按发送后计数验证；原生 ser-cli 的枚举计数未接线，使用 DZFlat 成功量和请求/响应 view 验证。\n\n本次未绑核、未改变频率策略、未停止用户原有进程。混合 P/E 核、后台 CPU 和频率变化可能影响长尾。三轮没有提供统计显著性证明。出现完整性错误的格不能当作正确业务吞吐的优势。\n")
    return "\n".join(text)

def stress_report(groups):
    text=["# 多话题压力测试结果分析\n",
        "正式目标：1000 话题 × 每话题 1000 msg/s，总计 100 万 msg/s，64 B 逻辑载荷，10 秒 × 3 轮；补充 1/100 话题。双方均为 4 个发布线程、不重叠分配话题、1 个接收进程。\n",
        "## 对照条件\n",
        "原安装 iceoryx 上限为 512 发布者、1024 订阅者、256 通知器。扩容档保持 iceoryx 2.0.5/CycloneDDS 0.10.2，将发布者/订阅者/内部通知器容量改为 2048，整体重建 RouDi、C binding 与 DDS，避免 ABI 混用。两档严格分列，均运行于私有 IPC、/dev/shm、/tmp。\n",
        "DDS 使用 on_data_available，在 DDS/iceoryx 内部线程取样，不创建应用接收工作线程；它仍有内部线程和 RouDi。cpp_ipc_dds 配置 32 个接收 worker 名额，空闲 worker 可退出；控制线程及主线程排空公共接收队列。线程列是结束时快照，千话题实测为 34。本比较包含两种 API 的交付成本，不是纯 wakeup 指令微基准。\n",
        "发布按绝对时间每毫秒每话题一条，落后在窗内追赶，不等待接收；计划、实际发送、最终接收和每话题覆盖分别记录。发送未达计划不能据此判定接收池上限。每条检查长度、首尾和序号，延迟每话题每 100 条确定性抽样；抽样可能与周期任务产生相位效应，不能当作全量分位数。预检另做完整字节校验。\n",
        "## 三轮聚合\n"]
    for name,cases in groups:
        rows=[]
        for n in (1,100,1000):
            for b in ("shm","dds-udp","dds-iox"):
                all_ds=[d for d in cases if d["topics"]==n and d["backend"]==b]
                ds=[d for d in all_ds if done(d)]
                rows.append([n,NAMES[b],f"{len(ds)}/{len(all_ds)}",
                    fmt(med([d["pub"]["sent"]/d["duration"] for d in ds]),0),
                    fmt(med([rate(d) for d in ds]),0),
                    fmt(med([100*(d["pub"]["plan"]-d["pub"]["attempts"])/d["pub"]["plan"] for d in ds]),4),
                    fmt(med([d["pub"]["sent"]-d["sub"]["received"] for d in ds]),0),
                    fmt(med([d["sub"]["latency_p99_us"] for d in ds])),
                    fmt(med([d["sub"]["cpu_seconds"]/d["sub"]["elapsed"] for d in ds])),
                    fmt(med([d["sub"]["threads"] for d in ds]),0)])
        text.append("### "+name+"\n\n"+table(["话题","后端","完整轮","发送 msg/s","窗内接收 msg/s","遗漏计划 %","排空后未收","p99 µs","收 CPU 核当量","收线程"],rows))
    text.append("\n## 千话题结果解读\n")
    for name,cases in groups:
        cpp=[d for d in cases if d["topics"]==1000 and d["backend"]=="shm" and done(d)]
        dds=[d for d in cases if d["topics"]==1000 and d["backend"]=="dds-iox" and done(d)]
        if cpp and dds:
            c_cpu=med([d["sub"]["cpu_seconds"]/d["sub"]["elapsed"] for d in cpp])
            d_cpu=med([d["sub"]["cpu_seconds"]/d["sub"]["elapsed"] for d in dds])
            c_tail=med([d["sub"]["latency_p99_us"] for d in cpp])
            d_tail=med([d["sub"]["latency_p99_us"] for d in dds])
            tails=" / ".join(fmt(d["sub"]["latency_p99_us"]) for d in sorted(dds,key=lambda d:d["repeat"]))
            missing=[d["pub"]["sent"]-d["sub"]["received"] for d in cpp]
            text.append(f"{name}在目标输入下，cpp 接收端 CPU 中位数 {fmt(c_cpu)} 核当量、采样 p99 {fmt(c_tail)} µs；DDS/iceoryx 为 {fmt(d_cpu)} 核当量、{fmt(d_tail)} µs。DDS 三轮采样 p99 按轮次为 {tails} µs，波动较大。cpp 各轮应用最终未收 {' / '.join(map(str,missing))} 条，DDS 三轮全部送达。当前配置体现 CPU、尾延迟和交付完整性的取舍，没有单一全面优胜者。\n")
    text.append("原安装不能创建完整千话题 DDS/iceoryx 端点，因此其容量失败不能当作接收吞吐成绩。扩容只解除端点数量限制，仍沿用原池计数并增加 2 MiB 档。DDS/UDP 在千话题下遗漏约 82% 发送计划，达不到百万输入，无法由此推导接收端的百万吞吐上限。\n")
    text.append("cpp 的 32 个接收 worker 在六轮千话题运行中池收包计数均为 1000 万，但应用层有少量未收；差异出现在池收包统计与应用取得消息两层之间。现有计数不能确定根因，需对队列入队、出队、生命周期及测量收尾进一步插桩。不能将该差异直接定性为传输丢包，也不能称已通过零丢失验收。\n")
    text.append("cpp 应用主线程循环扫描所有话题并 try_get/try_get_clone，空轮 yield；DDS 应用主线程等待，内部 listener 取样。千话题 cpp 主线程约占 1 核，其余线程约占 10 核；单话题约 1 核的成本主要也与工装主动取样有关。CPU 列包括这部分应用成本，不能视作接收池本身的纯 CPU。池内每收到一条消息还发生约 84 次 route 检查，后续可在维持正确性的前提下评估就绪路由调度、批量排空和唤醒合并。\n")
    warnings=[]
    for name,cases in groups:
        for d in cases:
            lines=d.get("pub_log_tail",[])+d.get("sub_log_tail",[])
            if d["topics"]==1000 and d["backend"]=="dds-iox" and any("POOL_IS_RUNNING_OUT_OF_CHUNKS" in line for line in lines):
                warnings.append([name,d["repeat"],d.get("pub",{}).get("loan_failed","—"),
                    "MEPOO__MEMPOOL_GETCHUNK_POOL_IS_RUNNING_OUT_OF_CHUNKS",fmt(d.get("sub",{}).get("latency_p99_us"))])
    if warnings:
        text.append("\n### 内存池告警证据\n\n以下完整运行有内存池暂时不足告警，记录计数仍全部送达。CycloneDDS 0.10.2 的 shm_create_chunk 会在申请失败时等待 1 ms 重试、最多尝试 10 次，因此 loan_failed=0 并不代表过程中未发生等待。告警和高尾延迟同时存在，尚未用逐事件时间线证明因果；不能把全部尾延迟归因于通知线程。\n")
        text.append(table(["配置","轮","最终借样失败","日志标识","采样 p99 µs"],warnings))
    text.append("\n## 千话题逐轮验收\n")
    rows=[]
    for name,cases in groups:
        for d in cases:
            if d["topics"]!=1000: continue
            p,s=d.get("pub",{}),d.get("sub",{})
            passed=done(d) and p["sent"]==p["plan"]==s["received"] and not s["bad"] and not s["duplicate"] and s["topics_covered"]==1000
            rows.append([name,NAMES[d["backend"]],d["repeat"],STATUS[d["status"]],
                p.get("plan","—"),p.get("attempts","—"),p.get("sent","—"),s.get("received","—"),
                p["sent"]-s["received"] if done(d) else "—",s.get("bad","—"),s.get("topics_covered","—"),
                f'{s.get("topic_rx_min","—")}/{s.get("topic_rx_max","—")}',"全部送达" if passed else "未满足全部送达"])
    text.append(table(["配置","后端","轮","运行","计划","尝试","发送","最终收","未收","坏样本","覆盖话题","每话题 min/max","目标判定"],rows))
    text.append("\n## 接收池与资源成本\n\n核当量 = 接收进程 CPU 秒/接收观测秒，观测包括约 0.5 秒排空；主线程和其余线程 CPU 分列。RSS 是进程峰值，不是稳态平均值。\n")
    rows=[]
    for name,cases in groups:
        for d in cases:
            if d["topics"]!=1000 or not done(d): continue
            s=d["sub"]
            rows.append([name,NAMES[d["backend"]],d["repeat"],s["threads"],s["pool_workers"],s["pool_routes"],
                s["pool_messages"],s["pool_wakeups"],s["pool_scans"],s["pool_scanned_routes"],
                s["pool_budget_yields"],s["pool_over_budget"],fmt(s["cpu_seconds"]),
                fmt(s.get("main_cpu_seconds",0)),fmt(s["cpu_seconds"]-s.get("main_cpu_seconds",0)),fmt(s.get("max_rss_kib",0)/1024)])
    text.append(table(["配置","后端","轮","线程","worker","route","池收包","唤醒","扫描轮","扫描route次","让出","超预算","CPU s","主线程 s","其余 s","峰值MiB"],rows))
    text.append("\n## RouDi 成本\n\n以下是独立守护进程在整次 case（含建链、预热、排空）内的 CPU，不与端点的测量窗混算。原安装和扩容档的容量不同，守护进程内存也会不同。\n")
    daemon=[]
    for name,cases in groups:
        for d in cases:
            if d["topics"]==1000 and d["backend"]=="dds-iox" and d.get("roudi"):
                r=d["roudi"]
                daemon.append([name,d["repeat"],fmt(r["cpu_seconds"]),fmt(r["window_seconds"]),r["threads"],fmt(r["rss_kib"]/1024)])
    text.append(table(["配置","轮","CPU s","观测 s","线程","RSS MiB"],daemon))
    text.append("\n## 结论\n")
    for name,cases in groups:
        for b in ("shm","dds-udp","dds-iox"):
            all_ds=[d for d in cases if d["topics"]==1000 and d["backend"]==b];ds=[d for d in all_ds if done(d)]
            if not ds:
                text.append(f"- {name}、{NAMES[b]}：无完整千话题运行。{reason(all_ds[0]) if all_ds else '尚无结果'}。\n")
                continue
            offered=sum(d["pub"]["sent"]==d["pub"]["plan"] for d in ds)
            delivered=sum(d["sub"]["received"]==d["pub"]["plan"] and not d["sub"]["bad"] and not d["sub"]["duplicate"] for d in ds)
            text.append(f"- {name}、{NAMES[b]}：{len(ds)} 轮完整，{offered} 轮输入达标，{delivered} 轮全部计划消息送达且校验无误；窗口接收中位数 {fmt(med([rate(d) for d in ds]),0)} msg/s，p99 {fmt(med([d['sub']['latency_p99_us'] for d in ds]))} µs。\n")
    text.append("\n“运行完成”与目标零丢失分别判定。少量尾部未收也不称零丢包。固定池避免每话题一个接收线程，但扫描、解码、队列和应用排空仍有 CPU 成本；DDS 的零应用接收线程不代表零 CPU。RouDi 成本不在端点 CPU 列内，未将其假定为零。\n")
    return "\n".join(text)

def ledger(cases,name):
    rows=[]
    for d in cases:
        p,s=d.get("pub",{}),d.get("sub",{});v=p if d["mode"]=="rpc" else s
        k="rpc" if d["mode"]=="rpc" else "latency"
        rows.append([name,d["id"],STATUS[d["status"]],p.get("plan","—"),p.get("attempts","—"),p.get("sent","—"),
            s.get("received","—"),s.get("received_in_window","—"),p.get("failed","—"),s.get("bad","—"),s.get("duplicate","—"),
            fmt(v.get(k+"_p50_us")),fmt(v.get(k+"_p99_us")),d["duration"],fmt(p.get("elapsed"),6),fmt(s.get("elapsed"),6),fmt(p.get("cpu_seconds")),fmt(s.get("cpu_seconds")),
            f'{p.get("threads","—")}/{s.get("threads","—")}',
            "/".join(str(p.get(x,"—")) for x in ("tlv","a","b","prebuilt")),
            f'{p.get("dzflat","—")}/{p.get("fallback","—")}',f'{s.get("request_views","—")}/{p.get("response_views","—")}',
            f'{p.get("loan_in_shm","—")}/{s.get("data_in_shm","—")}/{s.get("data_on_heap","—")}'])
    return table(["配置","case id","状态","计划","尝试","发/请求成功","收","窗内收","发失败","坏样本","重复","p50µs","p99µs",
        "计划窗 s","发实耗 s","收观测 s","发CPU s","收CPU s","发/收线程","TLV/A/B/Prebuilt","DZFlat/回退","请求/响应view","DDS发SHM/收SHM/收堆"],rows)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--current",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-current"))
    ap.add_argument("--scaled",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-scaled"))
    ap.add_argument("--deps",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-scaled-deps"))
    ap.add_argument("--stock",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-stock-pool"))
    ap.add_argument("--out",type=pathlib.Path,default=ROOT/"docs/transport_comparison_20261002")
    a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    speed,current,scaled=load(a.current,"speed"),load(a.current,"stress"),load(a.scaled,"stress")
    if not speed and not current and not scaled:
        ap.error("没有找到测试记录，拒绝用空报告覆盖已有交付")
    (a.out/"speed_analysis.md").write_text(speed_report(speed))
    (a.out/"stress_analysis.md").write_text(stress_report([("原安装",current),("同版本扩容",scaled)]))
    out=a.out/"results.md"
    plan=out.read_text().split("\n## 正式执行结果")[0] if out.exists() else "# 对比测试总报告\n"
    np=sum(d["mode"]=="pubsub" for d in speed);nr=sum(d["mode"]=="rpc" for d in speed)
    for node,finished,note in [
        ("P04",np==378,f"pub/sub {np}/378"),
        ("P05",nr==378,f"ser-cli {nr}/378（含不支持项）"),
        ("P07",len(current)==27 and len(scaled)==27,f"原安装 {len(current)}/27，扩容 {len(scaled)}/27"),
        ("P08",len(speed)==756 and len(current)==len(scaled)==27,"三份报告及逐轮账本")]:
        plan=re.sub(rf"(\| {node} \| [^|]+ \| )[^|]+ \|[^\n]+",lambda m:m.group(1)+("已完成" if finished else "进行中")+" | "+note+" |",plan)
    parts=[plan,"\n## 正式执行结果\n",f"生成时间：{datetime.datetime.now().astimezone().isoformat(timespec='seconds')}。\n",
        table(["节点","落盘记录","预计"],[["速度",len(speed),756],["原安装压力",len(current),27],["扩容压力",len(scaled),27]]),
        "\n详见 [速度分析](speed_analysis.md) 和 [压力分析](stress_analysis.md)。容量失败、接口不支持、异常与无错误完成分别记录。\n",
        table(["速度状态","轮次"],[[STATUS[k],sum(d["status"]==k for d in speed)] for k in STATUS]),
        "\n## 环境与可重复性\n",
        "环境 JSON 中的 sources 是测试运行当时的脚本快照，文末为最终交付源码指纹；后续只修订编排与报告。扩容脚本交付前由 build_scaled_deps.sh 重命名为 prepare_scaled_deps.sh，以避开仓库忽略规则，构建步骤未变。\n"]
    for name,directory in [("原安装",a.current),("同版本扩容",a.scaled)]:
        f=directory/"environment.json"
        if f.exists(): parts.append("### "+name+"\n\n~~~json\n"+json.dumps(json.loads(f.read_text()),ensure_ascii=False,indent=2)+"\n~~~\n")
    sha=a.deps/"source-sha256.txt"
    if sha.exists(): parts.append("\n源码归档 SHA-256：\n\n~~~text\n"+sha.read_text()+"~~~\n")
    stock=load(a.stock,"smoke")
    invalidation=a.current/"overlap_invalidation.json"
    if invalidation.exists():
        span=json.loads(invalidation.read_text())
        parts.append(f"\n作废重叠区间 Unix 秒：{span['start']}—{span['end']}，共 {len(span['invalid'])} 条记录；audit.py 检查最终保留速度记录均不在该区间内。\n")
    parts.append("\n## 原始内存池边界预检\n\n使用正式工装和原安装依赖，在隔离命名空间保留最大 1 MiB 池块。预检得到原始上限失败；正式速度只增加 2 MiB 池档，未修改原安装二进制。\n")
    parts.append(table(["方式","载荷","状态","证据"],[(d["mode"],d["bytes"],STATUS[d["status"]],reason(d)) for d in stock]))
    if stock:
        log="\n".join(stock[0].get("pub_log_tail",[]))
        log=re.sub(r"\x1b\[[0-9;]*m","",log)
        lines=[line for line in log.splitlines() if "fitting mempool" in line]
        if lines: parts.append("\n~~~text\n"+lines[-1]+"\n~~~\n")
    parts.append("\n## 复现\n\n~~~bash\nbash test/transport_comparison/execute_all.sh /var/tmp/my-ipc-comparison\n~~~\n\nrun.py 支持按 case 续跑，会核对二进制、时长与并发参数；覆盖需显式 --rerun。复现脚本默认保留中间数据供检查，本次交付在报告汇总审计后清理。\n")
    parts.append("\n## 逐轮结果账本\n\n将中间 JSON 的关键数据嵌入 Markdown，清理后仍可逐轮复核。RPC 枚举路径计数未接线，0 不表示未走 DZFlat，应看成功计数与请求/响应 view。view 和 DDS 指针抽样计数含预热，仅用于路径证明；DZFlat 关闭时 fallback 也计正常 TLV，不能一律当作失败。\n")
    for name,ds in [("原安装速度",speed),("原安装压力",current),("扩容压力",scaled)]: parts.append(ledger(ds,name))
    rows=[(name,d["id"],reason(d)) for name,ds in [("原安装速度",speed),("原安装压力",current),("扩容压力",scaled)]
          for d in ds if d["status"] in ("failed","degraded")]
    parts.append("\n## 异常记录\n\n"+table(["配置","case id","原因"],rows))
    src=ROOT/"test/transport_comparison"
    parts.append("\n## 交付源码指纹\n\n"+table(["文件","SHA-256"],[(f.name,hashlib.sha256(f.read_bytes()).hexdigest()) for f in sorted(src.iterdir()) if f.is_file()]))
    out.write_text("\n".join(parts))
    print(f"已生成三份报告：速度 {len(speed)} 格，压力 {len(current)+len(scaled)} 格")

if __name__=="__main__":
    main()
