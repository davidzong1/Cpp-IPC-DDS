#!/usr/bin/env python3
"""将修复实验逐轮证据写为 Markdown；追加本轮章节，保留已有历史报告。"""
import argparse
from collections import Counter,defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics as stats

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
DOC=ROOT/"docs/transport_comparison_20261002"
MARK="<!-- REPAIR-20261002 -->"
def median(rows,role,key):
    vals=[r[role][key] for r in rows if role in r and key in r[role]]
    return stats.median(vals) if vals else 0
def cpu(r,role="sub"):
    x=r.get(role,{})
    return x.get("cpu_seconds",0)/max(x.get("elapsed",0),1e-9)
def fmt(x):
    return f"{x:,.3f}" if isinstance(x,float) else str(x)
def table(head,rows):
    def cell(x):return fmt(x).replace("|","/").replace("\n"," ")
    return "\n".join(["| "+" | ".join(head)+" |","|"+"---|"*len(head),
                      *["| "+" | ".join(map(cell,r))+" |" for r in rows]])+"\n"
def append(name,text):
    p=DOC/name
    old=p.read_text().split(MARK)[0].rstrip()
    p.write_text(old+"\n\n"+MARK+"\n\n"+text.strip()+"\n")
def valid(r):
    return r.get("status")=="ok"
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    a=p.parse_args();w=a.work.resolve()
    allrows=[]
    for q in sorted(w.glob("*/cases/*.json")):
        r=json.loads(q.read_text());r["_build"]=q.parent.parent.name;allrows.append(r)
    unitpath=w/"delivered-units/results.json"
    units=json.loads(unitpath.read_text()) if unitpath.exists() else []
    paired_path=w/"paired-final/o1-c2-results.json"
    paired=json.loads(paired_path.read_text()) if paired_path.exists() else []
    final=[r for r in allrows if r["_build"] in ("final","scaled-final","trace-final")]
    def select(prefix,rows=final):return [r for r in rows if r["id"].startswith(prefix)]
    overview=[]
    for build in sorted(set(r["_build"] for r in allrows)):
        rows=[r for r in allrows if r["_build"]==build];c=Counter(r["status"] for r in rows)
        overview.append([build,len(rows),c["ok"],c["degraded"],c["failed"],c["unsupported"]])
    ledger=["# 修复实验逐轮账本","日期：2026-10-02。所有异常轮均保留；不同构建、校验工作量和测量时长不能混算。",
            "状态 ok 表示本轮工装校验通过；degraded 表示运行完成但未通过交付/输入/路径检查；failed 表示进程或建链失败；unsupported 是没有对应接口。",
            "CPU 核当量 = 接收进程 CPU 秒 / 接收观测秒。正常轮已无固定 0.5 秒收尾；异常轮可含最长 5 秒排空，不能用于性能收益比较。延迟只覆盖已收到的样本。",
            table(["构建组","格数","通过","不达标","失败","不支持"],overview)]
    for build in sorted(set(r["_build"] for r in allrows)):
        rows=[r for r in allrows if r["_build"]==build]
        ledger+=["## "+build,
                 table(["case_id","状态","秒","全字节","计划","成功发","收","池收","错/重/缺口","均值 µs","p99 µs","收 CPU 核","收主线程 s","扫描/收","原因"],
                 [[r["id"],r["status"],r["duration"],r.get("full",False),r.get("pub",{}).get("plan","—"),
                   r.get("pub",{}).get("sent","—"),r.get("sub",{}).get("received","—"),
                   r.get("sub",{}).get("pool_messages","—"),
                   "/".join(str(r.get("sub",{}).get(k,"—")) for k in ("bad","duplicate","gaps")),
                   r.get("pub" if r["mode"]=="rpc" else "sub",{}).get("rpc_mean_us" if r["mode"]=="rpc" else "latency_mean_us","—"),
                   r.get("pub" if r["mode"]=="rpc" else "sub",{}).get("rpc_p99_us" if r["mode"]=="rpc" else "latency_p99_us","—"),
                   cpu(r) if "sub" in r else "—",r.get("sub",{}).get("main_cpu_seconds","—"),
                   r.get("sub",{}).get("pool_scanned_routes",0)/max(r.get("sub",{}).get("received",0),1),
                   "；".join(r.get("reasons",[r.get("reason","")]))] for r in rows])]
    anomalies=[]
    for r in allrows:
        if r["status"] in ("ok","unsupported"):continue
        s=r.get("sub",{});pub=r.get("pub",{})
        badtopics=[i for i,t in enumerate(s.get("topic_delivery",[])) if t["expected"]!=t["delivered"]]
        anomalies.append([r["_build"],r["id"],pub.get("failed","—"),pub.get("loan_failed","—"),
                          pub.get("fallback","—"),s.get("queue_evicted","—"),s.get("wire_tlv_corrupt_total","—"),
                          len(badtopics),",".join(map(str,badtopics[:20]))])
    ledger+=["## 异常定位附表","话题列表最多显示前 20 项；数量是完整统计。序号缺口可能来自发送失败，须结合成功发送与唯一交付核对。",
             table(["组","case_id","发送失败","借样失败","回退","队列淘汰","TLV 损坏总数","不守恒话题数","话题下标"],anomalies)]
    warning_rows=[]
    for r in allrows:
        for role in ("pub","sub"):
            log=w/r["_build"]/"runs"/r["id"]/(role+".log")
            if not log.exists():continue
            lines=log.read_text(errors="replace").splitlines()
            important=[re.sub(r"\x1b\[[0-9;]*m","",line) for line in lines
                       if re.search(r"Warning|WARN|ERROR|Error|Fatal|FATAL|POOL_IS_RUNNING_OUT|OUT_OF_RESOURCES",line)]
            if important:
                unique=list(dict.fromkeys(important))
                warning_rows.append([r["_build"],r["id"],role,len(important),"；".join(unique[:3])[:800]])
    ledger+=["## 运行期告警与失败上下文",
             "计数是匹配告警标识的日志行数，不是丢包数量。每格最多保留 3 种代表行；完整交付不代表从未等待内存池。",
             table(["构建","case_id","角色","告警行数","代表日志"],warning_rows)]
    rpc_rows=[r for r in final if r["mode"]=="rpc" and "pub" in r and "sub" in r]
    ledger+=["## 最终版本 RPC 计数核对",
             "服务回调总数包含 32 次预热；表中扣除预热后再与成功请求比较。",
             table(["case_id","成功请求","请求失败","服务成功回调减预热","服务内容错误","状态"],
                   [[r["id"],r["pub"]["sent"],r["pub"]["failed"],r["sub"]["reply_ok"]-32,r["sub"]["reply_bad"],r["status"]] for r in rpc_rows])]
    DOC.joinpath("repair_ledger.md").write_text("\n\n".join(ledger)+"\n")
    tests=[]
    for u in units:
        text=(w/"delivered-units"/(u["test"]+".log")).read_text(errors="replace")
        n=re.findall(r"\[  PASSED  \] (\d+) tests?",text)
        tests.append([u["test"],u["exit"],int(n[-1]) if n else "见日志"])
    # 不依赖正则计数宣称通过；退出码才是该组是否通过的判据。
    cmp=[]
    for b,profile in [("a","r500k"),("b","r500k"),("a","w8"),("b","w8"),("shm","million")]:
        groups={v:[r for r in paired if r["backend"]==b and r["profile"]==profile and r["variant"]==v and valid(r)] for v in ("baseline","candidate")}
        for v,rows in groups.items():
            cmp.append([b,profile,v,len(rows),median(rows,"sub","latency_mean_us"),median(rows,"sub","latency_p99_us"),
                        stats.median([cpu(r) for r in rows]) if rows else 0,
                        stats.median([r["pub"]["sent"]/r["pub"]["elapsed"] for r in rows]) if rows else 0])
    stress=[r for r in final if r["mode"]=="stress"]
    stress_table=table(["case_id","话题","计划","成功发","收","最少/最多每话题","p99 µs","收 CPU 核","主线程 CPU s","队列淘汰","状态"],
                      [[r["id"],r["topics"],r.get("pub",{}).get("plan","—"),r.get("pub",{}).get("sent","—"),
                        r.get("sub",{}).get("received","—"),
                        str(r.get("sub",{}).get("topic_rx_min","—"))+"/"+str(r.get("sub",{}).get("topic_rx_max","—")),
                        r.get("sub",{}).get("latency_p99_us","—"),cpu(r) if "sub" in r else "—",
                        r.get("sub",{}).get("main_cpu_seconds","—"),r.get("sub",{}).get("queue_evicted","—"),r["status"]] for r in stress])
    summary=["# 修复执行结果与验收状态","日期：2026-10-02。对应 [执行方案](execution_plan.md)，逐轮结果见 [修复账本](repair_ledger.md)。",
             "**已修复两个独立的 SHM 接收正确性缺陷；Socket 结束边界仅局部修复，旧分片协议的大包完整性仍未闭环。O1 未达到性能门槛，已撤回，最终产品保留正确性修复。整体方案尚未通过最终验收。**",
             "## 产品修改与因果证据",
             table(["项","根因与修改","确定性验证","边界"],
             [["C1","空读后将最新 sequence 当作已消费，吞掉并发发布；稳定空读仅确认接收前快照，变化则继续处理","旧版反例 10/10 失败；空读后发布、稳定检查后发布、回零三类各 1000/1000 通过","预算、停止、注销、异常不冒充空读；保留无进展退避"],
              ["C2","thread_local 分片缓存只按消息 ID 索引；改为连接独占，并互斥访问/断开清理","交错话题同 ID、断开邻路两类旧版各 10/10 失败；修复后各 1000/1000 通过","进程本地对象改变，共享 wire/公开 API 未改；锁不跨队列等待，不进入共享 chunk 快路径"],
              ["C4","final HB 后继续等页会混入下一条；立即丢弃当前不完整组装","旧版 10/10 混页；修复后该反例 1000/1000 通过","其他首片/通知丢失仍能混页，不能称彻底修复"]]),
             "修改文件：src/dzIPC/threepools/recv_worker.cc、src/libipc/ipc.cpp、src/dzIPC/common/data_rev.cc。新增失败用例及工装只位于 test 下。",
             "C1 的 A/B、8 B/1 KiB、w1/w8、10 秒 × 10 轮共 80 格全部通过。该早期进展结果用于验证 C1；最终构建/后续长测按下表和账本单独记录。",
             "## 最终版本单元回归",
             table(["测试程序","退出码","通过测试数"],tests),
             "旧 C2 回归中的 W08 曾因拒绝覆盖已有证据退出 1；独立目录重跑全 3 测试通过。交付 repair_units.py 已为每次 W08 创建新目录，最终上表保留真正执行的退出码。",
             "## O1 五轮单变量筛选",
             "基线与候选均包含 C1、C2、C4。唯一产品差异是是否删除 wait_once 末尾重复 collect_pending；相同 CPU 集合，每格 10 秒，固定种子交错串行。下表以独立轮中位数呈现，所有 50 格均完整交付。",
             table(["后端","输入档","版本","完整轮","均值 µs","p99 µs","收 CPU 核","成功发送 msg/s"],cmp),
             "千话题扫描约 84 → 56 次/消息，但 CPU 中位数 11.58 → 11.85 核，未达下降 30% 目标；8 B 的相同 50 万输入均值也未下降 15%。饱和档有改善，但未达到方案的吞吐 +15% 或均值 -20% 门槛。因此撤回 O1，不能将扫描次数减少直接宣传为 CPU 优化。O2–O5 尚无足够归因证据，不默认加入产品。",
             "## 真实验证当前覆盖",
             table(["前缀","总格","通过","不达标/失败","不支持"],[[prefix,len(rs:=select(prefix)),sum(valid(r) for r in rs),
                 sum(r["status"] in ("failed","degraded") for r in rs),sum(r["status"]=="unsupported" for r in rs)]
                 for prefix in ["final-quick","final-idle","final-short","final-long","final-capacity","dds-original-capacity","dds-scaled-short","dds-scaled-long","final-input","final-control","probe-","final-full","final-bytes"]]),
             "0 格表示尚未执行，不是通过。RPC 的 B/Prebuilt 原生接口仍为 N/A。全量速度与全部尺寸正确性矩阵保留在 repair_campaign.py 的 full 阶段；C4 未闭环时，不将代表格冒充全量验收。",
             "## 千话题交付、CPU 与容量",stress_table,
             "A 回退到 TLV 与 B 借样失败必须单列。各档共享块池数量不变；成功发送全收不代表达到计划输入。DDS 扩容只修改端点/通知器容量，必须使用同版本整套重建依赖；原安装的创建失败不能作为百万吞吐成绩。",
             "## 计时与工装变化",
             "延迟仍从准备本条载荷之前到应用取得，RPC 从准备请求到 success 响应。B 的载荷写入计入准备阶段；publish 主要提交描述符，所以大包发布耗时变化较小。未改变正常速度档 memset 工作量，全字节正确性档使用固定种子、随序号及位置变化的非均匀内容，二者分别报告。",
             "发送停止后先公布逐话题成功数和最终序号，接收确认排空；无进展有界退出。逐话题位图核对重复/缺口，实际计划与成功数、队列淘汰和 wire 拒绝原因分列。限速多发 1 条的旧工装已修正，旧初筛不可用于性能结论。",
             "静默档要求前一条确认送达后再等待 100 ms，单条在途，目标 1000 条，硬时长保留 20% 余量。没有启动下一条业务消息帮助唤醒前一条。",
             "## 未闭环项与下一步",
             "Socket 旧数据片没有逐消息 sequence；同类型同长度的 A 首片与 B 后续页无法区分。彻底修复需逐数据报携带版本、发布实例、消息序号及长度/偏移/校验，并明确两端共同升级范围。此前已询问该范围，未收到答复前保持现有协议，不擅自实施 wire 升级。",
             "C3 的代表 RPC 已验证；不得据此归因全部历史超时或声称完整尺寸回归通过。O1 已按门槛撤回，小消息与千话题性能目标尚未达成。内核 perf_event_paranoid=4 拒绝采样，未修改系统设置，也不把队列驻留合并区间伪称为纯内核唤醒时间。",
             "AGENTS.md 指定的团队 MCP 工具未在本会话开放，未执行 member_report_result 或 /compact，不声称已经团队回报。",
             "## 复现方式",
             "所有构建和中间结果放在仓库外；下例 task_work 取新的绝对路径。正常产品由 transport_comparison/CMakeLists.txt 构建，避免顶层关闭 Python 时引用缺失安装目标的问题。",
             "```bash\npython3 -B test/transport_comparison/repair_units.py --work \"$task_work/units\" --run\ncmake -S test/transport_comparison -B \"$task_work/final/build\"\ncmake --build \"$task_work/final/build\" -j6\npython3 -B test/latency_breakdown/instrument.py \"$task_work/trace-final\"\ncmake -S test/latency_breakdown -B \"$task_work/trace-final/build\"\ncmake --build \"$task_work/trace-final/build\" -j6\nbash test/transport_comparison/prepare_scaled_deps.sh \"$task_work/scaled-deps\"\ncmake -S test/transport_comparison -B \"$task_work/scaled-final/build\" -DDDS_ROOT=\"$task_work/scaled-deps/prefix\"\ncmake --build \"$task_work/scaled-final/build\" -j6\npython3 -B test/transport_comparison/repair_campaign.py --work \"$task_work\" --phase correctness\npython3 -B test/transport_comparison/repair_campaign.py --work \"$task_work\" --phase comparison\n# 全量阶段必须单独核对 Socket 未闭环状态，失败记录不可删除。\npython3 -B test/transport_comparison/repair_campaign.py --work \"$task_work\" --phase full\npython3 -B test/transport_comparison/repair_report.py --work \"$task_work\"\n```",
             "每阶段非零退出码表示存在未通过的格，不能用最后一条命令覆盖前面的失败。复现默认保留中间证据以便续跑；最终验收完成后先汇总 Markdown，再清理本任务临时产物，禁止停止宿主 RouDi。"]
    env={}
    env_path=w/"final-environment.json"
    if env_path.exists():
        env=json.loads(env_path.read_text())
        summary+=["## 环境与构建指纹",f"Git 基点：{env['git_head']}。{env['uname']}。CPU 集合：{env['affinity']}。",
                  table(["构建","程序 SHA-256","库 SHA-256"],[[k,v["build/comparison"],v["build/lib/libipc.so"]]
                        for k,v in env.items() if isinstance(v,dict) and "build/comparison" in v])]
    manifests=[]
    for path in sorted(w.glob("*/*manifest.json")):
        data=json.loads(path.read_text())
        manifests.append([str(path.relative_to(w)),hashlib.sha256(path.read_bytes()).hexdigest(),
                          data.get("runner","—"),data.get("script",data.get("matrix","—"))])
    summary+=["## 运行配置记录",table(["manifest","SHA-256","当时运行器 SHA-256","当时矩阵脚本 SHA-256"],manifests)]
    sources=[ROOT/"src/libipc/ipc.cpp",ROOT/"src/dzIPC/threepools/recv_worker.cc",ROOT/"src/dzIPC/common/data_rev.cc",
             ROOT/"test/test_recv_fragment_isolation.cpp",ROOT/"test/test_recv_worker.cpp",ROOT/"test/test_socket_reliable_crc.cpp",
             *HERE.glob("*.py"),HERE/"comparison.cpp"]
    summary+=[table(["最终源码/脚本","SHA-256"],[[str(q.relative_to(ROOT)),hashlib.sha256(q.read_bytes()).hexdigest()] for q in sorted(sources)])]
    idle=select("final-idle")
    summary+=["## 静默后单条",
              table(["case_id","计划","成功发","收","p50 µs","p99 µs","最大 µs","停止推进","状态"],
              [[r["id"],r.get("pub",{}).get("plan","—"),r.get("pub",{}).get("sent","—"),
                r.get("sub",{}).get("received","—"),r.get("sub",{}).get("latency_p50_us","—"),
                r.get("sub",{}).get("latency_p99_us","—"),r.get("sub",{}).get("latency_max_us","—"),
                r.get("pub",{}).get("stalled","—"),r["status"]] for r in idle])]
    if env_path.exists():
        summary+=["现有频率策略："+", ".join(sorted(set(env.get("governor",{}).values())))+
                  "。未修改调度策略，CPU 0–31 包含 P/E 核与 SMT，线程可以迁移；跨轮波动如实保留。",
                  "RouDi 测试池配置（容量字节 / 块数）：128/10000、1024/5000、16384/1000、131072/200、1048576/50、2097152/50。最后一档覆盖 1 MiB 应用数据加协议开销。"]
    DOC.joinpath("repair_results.md").write_text("\n\n".join(summary)+"\n")
    append("results.md","## 2026-10-02 修复执行追加\n\nC1（空读确认竞态）与 C2（跨话题分片缓存串用）已修复并通过确定性反例验证。Socket 仅修复已知结束边界，旧协议大包问题仍未闭环；O1 未达收益门槛，已撤回。整体方案尚未通过最终验收。\n\n最新测试覆盖、逐轮状态、修复边界、构建指纹及复现步骤见 [修复结果](repair_results.md) 与 [修复账本](repair_ledger.md)。以下历史账本保持原版本口径。")
    append("speed_analysis.md","## 2026-10-02 修复后速度与计时追加\n\n延迟起点仍是准备载荷前，并非发布写入完成。B 的大块写入位于准备阶段，不能只看 publish 耗时。全字节校验档额外填充非均匀数据并完整检查，单独记录，不与普通速度档混算。\n\nC1/C2 正确性修复已保留，O1 配对虽有部分饱和档收益，但相同 50 万输入均值未达下降 15% 目标，千话题 CPU 也未降低，故撤回。相关表和每轮记录见 [修复结果](repair_results.md) 与 [修复账本](repair_ledger.md)。Socket 未闭环及尚未执行的完整矩阵不能当作通过。")
    append("stress_analysis.md","## 2026-10-02 修复后压力追加\n\n旧报告中“池收到、应用少收”的现象已确定性定位为连接之间共用 thread_local 分片缓存；不同话题独立消息 ID 碰撞会串片，断开邻路还会清除半包。连接独占缓存修复后，SHM/TLV 的三轮 10 秒诊断均为 1000 万条完整送达。最终版更长测量与 DDS 对照如下。\n\n"+stress_table+"\nA/B 的原池容量限制与接收丢失分别统计；成功发送全收而计划不足仍是不达标。O1 扫描约 84 → 56 次/消息，却未降低进程 CPU，已经撤回。完整上下文见 [修复结果](repair_results.md)。")
    inputs=[r for r in final if r["id"].startswith("final-input") and valid(r)]
    input_rows=[]
    for b in ("a","b","dds-iox"):
        for size in (8,1024):
            for rate in (100000,500000,0):
                rows=[r for r in inputs if r["backend"]==b and r["bytes"]==size and r["pub"].get("offered_rate")==rate]
                if rows:input_rows.append([b,size,rate or "饱和 w8",len(rows),median(rows,"sub","latency_mean_us"),
                    median(rows,"sub","latency_p99_us"),stats.median(cpu(r) for r in rows),
                    stats.median(r["pub"]["sent"]/r["pub"]["elapsed"] for r in rows)])
    speed_file=DOC/"speed_analysis.md"
    with speed_file.open("a") as f:
        f.write("\n\n本轮同输入结果（仅通过的完整轮取中位数，失败比例另见账本）：\n\n"+
            table(["后端","字节","输入 msg/s","完整轮","均值 µs","p99 µs","收 CPU 核","实际 msg/s"],input_rows))
    controls=[r for r in final if r["id"].startswith("final-control")]
    control_rows=[]
    for b in ("a","b","dds-iox"):
        for size in (65536,1048576):
            for window in (1,8):
                for full in (False,True):
                    rows=[r for r in controls if r["backend"]==b and r["bytes"]==size
                          and r["full"]==full and r.get("pub",{}).get("window")==window]
                    passed=[r for r in rows if valid(r)]
                    if rows:control_rows.append([b,size,window,"非均匀全校验" if full else "取得/首尾检查",
                        f"{len(passed)}/{len(rows)}",median(passed,"pub","prepare_mean_us"),
                        median(passed,"pub","send_mean_us"),median(passed,"sub","latency_mean_us"),
                        median(passed,"sub","latency_p99_us"),
                        stats.median(r["pub"]["sent"]/r["pub"]["elapsed"] for r in passed) if passed else "—"])
    with speed_file.open("a") as f:
        f.write("\n\n大包控制对照：非均匀全校验档同时增加发送填充和接收校验工作；延迟计时仍止于应用取得，未单独测量本条完整处理结束时间，不能把两档延迟之差当作纯遍历耗时。吞吐包含这些工作。\n\n"+
            table(["后端","字节","窗口","模式","通过/总轮","准备均值 µs","发布均值 µs","取得均值 µs","取得 p99 µs","实际 msg/s"],control_rows))
    probes=[r for r in final if r.get("breakdown")]
    bdtable=table(["case_id","发送抽样","关联","未关联","时序异常","总均值 µs","投递前→worker µs","worker 后取包 µs","取包→应用 µs"],
                  [[r["id"],(b:=r["breakdown"])["sampled_sent"],b["matched"],len(b["incomplete"]),len(b["nonmonotonic"]),
                    b["segments"].get("total",{}).get("mean","—"),b["segments"].get("before_worker",{}).get("mean","—"),
                    b["segments"].get("worker_after_submit",{}).get("mean","—"),b["segments"].get("recv_to_app",{}).get("mean","—")] for r in probes])
    append("latency_breakdown.md","## 2026-10-02 修复后分段复核\n\n采用同一产品版本 U（无探针）、O（插桩关闭）、T（抽样开启）交错运行；阶段定义延续前文。零行表示尚未执行，不表示无开销。内核调度采样权限不足，投递前到 worker 仍为驻留与调度的合并区间。\n\n"+bdtable+"\n完整状态、未插桩结果与指纹见 [修复结果](repair_results.md)、[修复账本](repair_ledger.md)。")
    probe_rows=[r for r in final if r["id"].startswith("probe-")]
    probe_summary=[]
    for b in ("a","b","dds-iox"):
        for size,window,rate in [(8,8,0),(8,8,500000),(1024,8,0),(1024,8,500000),(65536,1,0),(1048576,1,0)]:
            for v in ("U","O","T"):
                rows=[r for r in probe_rows if r.get("variant")==v and r["backend"]==b and r["bytes"]==size
                      and r.get("window")==window and r.get("offered_rate")==rate]
                passed=[r for r in rows if valid(r)]
                if rows:probe_summary.append([b,size,window,rate,v,f"{len(passed)}/{len(rows)}",
                    median(passed,"sub","latency_mean_us"),median(passed,"sub","latency_p99_us"),
                    stats.median(r["pub"]["sent"]/r["pub"]["elapsed"] for r in passed) if passed else "—"])
    with (DOC/"latency_breakdown.md").open("a") as f:
        f.write("\n\nU/O/T 各独立轮中位数（异常轮不进入性能汇总；比例保留）：\n\n"+
            table(["后端","字节","窗口","输入","档","通过/总轮","总均值 µs","总 p99 µs","实际 msg/s"],probe_summary))
    profiles=[r for r in final if r.get("cpu_profile")]
    profile_rows=[]
    thread_rows=[]
    for r in profiles:
        p=r["cpu_profile"];sub=p["roles"]["sub"];threads=[t for t in p["threads"] if t["role"]=="sub" and t["seconds"]>0]
        voluntary=sum(t["voluntary"]/t["seconds"] for t in threads)
        involuntary=sum(t["involuntary"]/t["seconds"] for t in threads)
        total=sub["user_cores"]+sub["system_cores"]
        profile_rows.append([r["_build"],r["id"],r["status"],sub["threads_sampled"],
            sub["user_cores"],sub["system_cores"],100*sub["system_cores"]/total if total else 0,
            voluntary,involuntary,r.get("sub",{}).get("latency_p99_us","—"),
            p["sampler_cpu_seconds"],len(p["errors"])])
        for group in ("main","others"):
            ts=[t for t in threads if t["main"]==(group=="main")]
            if ts:thread_rows.append([r["id"],group,len(ts),min(t["seconds"] for t in ts),max(t["seconds"] for t in ts),
                sum(t["user_seconds"]/t["seconds"] for t in ts),sum(t["system_seconds"]/t["seconds"] for t in ts),
                sum(t["voluntary"] for t in ts),sum(t["involuntary"] for t in ts),
                sorted(set(c for t in ts for c in t["observed_cpus"]))])
    cpu_table=table(["构建","case_id","状态","采到接收线程","用户态核","系统态核","系统态 %","自愿切换/s","非自愿切换/s","p99 µs","采样器 CPU s","采样异常"],profile_rows)
    cpu_text="## 测量窗口内 CPU 归因\n\n独立诊断档每 100 ms 读取本轮子进程 /proc；采样轮开始时检查共享控制文件的 start/end 窗口。每线程用首末样本的差值除以自身观察秒数，再汇总核当量。首尾约 100 ms 未覆盖；线程依次读取，末轮个别读数可能超出终点一次遍历的耗时，并非严格同步截点。计数精度为系统时钟节拍，采样器会产生额外 CPU 与调度干扰。主线程与其余线程单列，其余线程不能全部当作 worker。观察到的 CPU 集合不是迁移次数。\n\n"+cpu_table
    if profiles:
        with (DOC/"repair_results.md").open("a") as f:f.write("\n\n"+cpu_text)
        with (DOC/"stress_analysis.md").open("a") as f:f.write("\n\n"+cpu_text)
        with (DOC/"repair_ledger.md").open("a") as f:
            f.write("\n\n"+cpu_text+"\n"+
                table(["case_id","线程组","线程数","最短窗口 s","最长窗口 s","用户态核","系统态核","自愿切换差值","非自愿切换差值","观察 CPU"],thread_rows))
    workers=[r for r in final if r["id"].startswith("workers-w")]
    worker_table=[]
    for count in sorted(set(r["configured_workers"] for r in workers)):
        rs=[r for r in workers if r["configured_workers"]==count];passed=[r for r in rs if valid(r)]
        if passed:
            values=[cpu(r) for r in passed]
            worker_table.append([count,f"{len(passed)}/{len(rs)}",stats.median(values),min(values),max(values),
                median(passed,"sub","latency_mean_us"),median(passed,"sub","latency_p99_us"),
                stats.median(r["sub"]["pool_scanned_routes"]/r["sub"]["received"] for r in passed)])
    worker_text="## 无采样 worker 配置对照\n\n同一最终产品、1000 话题 × 1000 msg/s、64 B、4 个发布线程、每格 10 秒；32/64 worker 按固定种子交错，CPU 集合不变。全部结果（包括失败）见账本，表内性能只汇总通过轮。\n\n"+table(
        ["worker","通过/总轮","CPU 核中位数","最小核","最大核","均值 µs","p99 µs","扫描/消息"],worker_table)
    pair_rows=[]
    for repeat in sorted(set(r["repeat"] for r in workers)):
        pair={r["configured_workers"]:r for r in workers if r["repeat"]==repeat and valid(r)}
        if 32 in pair and 64 in pair:
            baseline,candidate=pair[32],pair[64]
            pair_rows.append([repeat,100*(cpu(candidate)/cpu(baseline)-1),
                100*(candidate["sub"]["latency_p99_us"]/baseline["sub"]["latency_p99_us"]-1)])
    worker_text+="\n"+table(["配对轮","64 相对 32 CPU 变化 %","p99 变化 %"],pair_rows)
    if worker_table:
        for name in ("repair_results.md","stress_analysis.md"):
            with (DOC/name).open("a") as f:f.write("\n\n"+worker_text)
    if profiles:
        interpretation="""
## CPU 证据的含义与下一步

测量窗口内 CPP 32-worker 的三轮中位数为用户态约 5.48 核、系统态约 6.23 核；DDS 扩容档约 0.89 核用户态、0.002 核系统态。CPP 接收主线程约占 1 核，DDS 主线程约 0.004 核。DDS 的“0 线程”是应用不额外创建接收 worker，采样仍看到 10 个接收进程线程，不能称整个进程零线程。

CPP 自愿上下文切换在 16/32/64 worker 下分别约 27.4/69.3/87.8 万次/s；DDS 约 1895 次/s。64-worker 的切换次数更多，系统态 CPU 却较低，因此不能把每次上下文切换视为固定成本。worker 数同时改变每个等待集合的大小、调度并行度和消息聚合机会，本实验只能确认配置效果，不能单独证明其中哪个因素致因。

据此，优先级应是分别测量 waitv 调用频率、EAGAIN/真正等待比例与等待集合长度，再检验就绪消息的聚合处理。用户态分配/共享统计仍可能显著，但不能把接近一半的系统态时间归因于 Sample 构造。O2 缓冲复用也不能提前承诺降低系统态开销；需单变量测量，保留 token 生命周期、并发注销和空闲阻塞。

这轮不新增持续自旋、不削减应用校验、不删除同步来换取数字。64-worker 对照未达到 CPU 下降 30% 的门槛，保留默认 32-worker；将其视为诊断结果。CPP 更低的千话题 p99 与 DDS 更低的 CPU 同时报告，不能只选一个指标宣称全面领先。
"""
        for name in ("repair_results.md","stress_analysis.md"):
            with (DOC/name).open("a") as f:f.write("\n\n"+interpretation)
    if probes:
        segmented=[]
        for backend in ("a","b"):
            for rate in (0,500000):
                rs=[r for r in probes if r["backend"]==backend and r["bytes"]==8 and r.get("offered_rate")==rate]
                if not rs:continue
                parts=[r["breakdown"]["segments"] for r in rs]
                segmented.append([backend,rate or "各自饱和 w8",len(rs),
                    *[stats.median(b[k]["mean"] for b in parts) for k in ("total","before_worker","worker_after_submit","recv_to_app","queue_to_app")],
                    stats.median(100*b["before_worker"]["mean"]/b["total"]["mean"] for b in parts)])
        conclusion="## 本轮分段结论\n\n"+f"已完成 {len(probe_rows)} 个 U/O/T 格，其中 {sum(valid(r) for r in probe_rows)} 格通过；开启抽样的 {len(probes)} 格共关联 {sum(r['breakdown']['matched'] for r in probes):,} 个样本，未关联 {sum(len(r['breakdown']['incomplete']) for r in probes)}，时序异常 {sum(len(r['breakdown']['nonmonotonic']) for r in probes)}。"+"\n\n"+table(
            ["路径","输入 msg/s","轮","抽样总均值 µs","投递前→worker µs","worker 后取包 µs","取包→应用 µs","其中队列交接 µs","处理前占比 %"],segmented)+"""
各列是独立轮均值的中位数，不能逐列相加；占比先在同轮计算再取中位数。A/B 饱和档的主要延迟仍在 worker 开始处理前，但该段包括队列驻留、前序服务和通知调度，不能全部称为 futex 唤醒。相同 50 万输入时该段明显缩短，说明输入负载必须受控。应用队列交接约 0.19 µs，不是饱和延迟的最大分段。

U/O/T 的速度差包含探针扰动和跨轮波动，正式速度使用 U；抽样阶段成本使用 T。DDS 的发布入口→take 包含其自己的发布与接收阶段，不能当作 CPP 的同名 worker 段。相同输入差距较小，也不能据此认为高话题数 CPU 已解决。
"""
        for name in ("latency_breakdown.md","repair_results.md"):
            with (DOC/name).open("a") as f:f.write("\n\n"+conclusion)
    provenance=[]
    for build in sorted(set(r["_build"] for r in allrows)):
        grouped=Counter(r.get("binary_sha256","未记录") for r in allrows if r["_build"]==build)
        provenance.extend([build,signature,count] for signature,count in grouped.items())
    metadata_paths=[w/"manifest.json",w/"final-environment.json",w/"correctness-campaign.json",w/"comparison-campaign.json",
                    w/"trace-final/instrumentation.json",w/"final/repair-final-source-proof.json",
                    *sorted(w.glob("*/*manifest.json"))]
    with (DOC/"repair_ledger.md").open("a") as f:
        f.write("\n\n## 配置与来源存档\n\n本节保存清理前的配置元数据；其中节点状态及临时路径是当时运行记录，最终状态以执行方案为准。构建不一致的行不得混算。\n\n"+
            table(["构建组","当时程序 SHA-256","格数"],provenance))
        for path in metadata_paths:
            if path.exists():
                f.write("\n### "+str(path.relative_to(w))+"\n\n```json\n"+
                        json.dumps(json.loads(path.read_text()),ensure_ascii=False,indent=2)+"\n```\n")
    print(f"已汇总 {len(allrows)} 格，最终构建相关 {len(final)} 格；异常未删除")

if __name__=="__main__":
    main()
