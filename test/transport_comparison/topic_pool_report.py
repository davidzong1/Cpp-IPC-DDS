#!/usr/bin/env python3
"""将话题十块池的逐轮结果归档为 Markdown；不覆盖历史 40 块池报告。"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics as stat

ROOT=Path(__file__).resolve().parents[2]
def table(headers,rows):
    return "\n".join(["| "+" | ".join(map(str,headers))+" |","|"+"---|"*len(headers)]+
                     ["| "+" | ".join(str(v).replace("|","/").replace("\n"," ") for v in row)+" |" for row in rows])+"\n"
def median(rows,role,key):
    return stat.median(r[role][key] for r in rows)
def f(x):
    return f"{x:.3f}"
def read(path):
    return json.loads(path.read_text())
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--tag",default="topic10-v2")
    p.add_argument("--out",type=Path,default=ROOT/"docs/transport_comparison_20261002")
    a=p.parse_args();w=a.work.resolve();out=a.out
    out.mkdir(parents=True,exist_ok=True)
    quick=read(w/f"{a.tag}-quick-results.json")
    comp=read(w/f"{a.tag}-compare-results.json")
    long=read(w/f"{a.tag}-long-results.json")
    resources=read(w/"resources/results.json")
    assert len(quick)==16 and len(comp)==60 and len(long)==6 and len(resources)==3
    assert all(r["status"]=="ok" for r in quick)
    assert all(r["exit"]==0 and len(r["checkpoints"])==4 for r in resources)
    units=read(w/"topic10-units-results.json")
    revised_path=w/"revised-tests/results.json"
    revised={r["test"]:r for r in read(revised_path)} if revised_path.exists() else {}
    assert all(revised.get(r["test"],r)["exit"]==0 for r in units)
    speed=[r for r in comp if r["mode"]=="pubsub"]
    stress=[r for r in comp if r["mode"]=="stress"]
    common="""测试日期：2026-10-02。机器为 Intel Core i9-14900KF、32 个逻辑 CPU、约 62 GiB 可见内存，Release / O3，CPU affinity 为 0–31，不固定到特定核心。宿主共享内存为 32 GiB，本次未改挂载。测试在独立的 4 GiB tmpfs 中串行执行；构建与资源探针不与正式性能轮并行。宿主仍有其他进程活动，三轮只能说明本机当前条件下的趋势。

旧基线为 Git `e3b17db3a3ac35a3a73497d16c22e02032e64456` 的 40 块共享池；新版本为本次每话题每尺寸档 10 块独立池。比较使用相同测试器、32 个接收 worker、4 个发布线程配置；不重测 CycloneDDS + iceoryx，不能据此更新与 DDS 的排名。
"""
    timing="""计时使用同机单调时钟。端到端延迟从发布者开始准备本条消息的时间戳，到订阅者应用取样时刻，包含借样、载荷准备、发布、传输、接收 worker 和应用队列等待，**不是从消息写入完成开始**。发布调用耗时单独统计；B 的借样与填充在发布调用计时之外。压力轮每 100 条采样一次延迟，普通速度轮逐条记录；p99 是本轮采样分位数，三轮汇总取各轮指标中位数，不混合所有样本求分位数。

性能轮测试器对大载荷仅更新/校验标记位置，衡量传输与调度，不代表每条完整填充 1 MiB 的生产成本。另有 16 格全字节正确性轮覆盖 8 B、1 KiB、1 MiB；其速度数值不作为正式对照。压力测试的应用接收环节由单线程轮询 1000 个订阅队列，也会影响应用可见延迟；不能把端到端全部归因于 worker 唤醒。
"""
    s="# 话题独立十块池：速度结果分析\n\n"+common+"\n## 口径\n\n"+timing
    s+="\n每格持续 5 秒，三轮按固定种子交错新旧版本；最多 8 条在途。限速档目标为 50 万条/秒；饱和档不指定目标速率。以下吞吐为成功发送数/发布测量时长，单位百万条/秒。\n\n"
    rows=[]
    for b in ["a","b"]:
        for size,rate in [(8,500000),(8,0),(1024,500000),(1048576,0)]:
            for v in ["baseline","final"]:
                rr=[r for r in speed if r["backend"]==b and r["bytes"]==size and
                    r["variant"]==v and r["pub"]["offered_rate"]==rate]
                rows.append([b.upper(),size,"50万/s" if rate else "饱和","旧共享40" if v=="baseline" else "新话题10",
                             f(stat.median(r["pub"]["sent"]/r["pub"]["elapsed"]/1e6 for r in rr)),
                             f(median(rr,"sub","latency_mean_us")),f(median(rr,"sub","latency_p50_us")),
                             f(median(rr,"sub","latency_p99_us")),f(median(rr,"pub","send_mean_us")),
                             "/".join(r["status"] for r in rr)])
    s+=table(["路径","载荷 B","输入","版本","吞吐 M/s","平均 μs","p50 μs","p99 μs","发布调用平均 μs","三轮状态"],rows)
    s+="\n## 结论\n\n单话题小包的延迟以表中三轮中位数为准，不能仅凭池隔离假设延迟下降。本次改动解决容量隔离问题，不能宣称降低小包延迟。千话题下的明显延迟上升见[压力分析](topic_pool_stress_analysis.md)。\n\n"
    s+="## 逐轮账本\n\n所有速度轮均保留，包括未达标项；成功发送与最终唯一接收应一致。\n\n"
    s+=table(["版本/路径/字节/限速/轮","状态","计划","尝试","发送","最终接收","失败","TLV回退","平均 μs","p50 μs","p99 μs"],
             [[f'{r["variant"]}/{r["backend"]}/{r["bytes"]}/{r["pub"]["offered_rate"]}/{r["repeat"]}',r["status"],
               r["pub"]["plan"],r["pub"]["attempts"],r["pub"]["sent"],r["sub"]["received"],r["pub"]["failed"],
               r["pub"]["fallback"],f(r["sub"]["latency_mean_us"]),f(r["sub"]["latency_p50_us"]),
               f(r["sub"]["latency_p99_us"])] for r in speed])
    (out/"topic_pool_speed_analysis.md").write_text(s)

    s="# 话题独立十块池：压力结果分析\n\n"+common+"\n## 负载与计时\n\n"
    s+="1000 话题，每话题目标 1000 条/秒，载荷 64 B。每轮 4 个线程分别负责互不重叠的话题，每毫秒向各话题发送一条；不对失败重试，不用低成功输入伪装达标。三轮新旧对照各 10 秒；另做新版本 A/B 各三轮 60 秒。订阅构造参数为 64，DzFlat view 上限为 10，新旧相同；队列容量与池块数相等不代表不会背压。\n\n"+timing
    s+="\n## 十秒对照\n\n"
    s+=table(["路径","版本","交付率/计划 %","借样失败/尝试 %","TLV回退/发送 %","p50 μs","p99 μs","发布CPU核当量","订阅CPU核当量","发布/订阅峰值RSS MiB"],
        [[b.upper(),"旧共享40" if v=="baseline" else "新话题10",
          f(stat.median(r["sub"]["received"]/r["pub"]["plan"]*100 for r in rr)),
          f(stat.median(r["pub"]["loan_failed"]/max(1,r["pub"]["attempts"])*100 for r in rr)),
          f(stat.median(r["pub"]["fallback"]/max(1,r["pub"]["sent"])*100 for r in rr)),
          f(median(rr,"sub","latency_p50_us")),f(median(rr,"sub","latency_p99_us")),
          f(stat.median(r["pub"]["cpu_seconds"]/r["pub"]["elapsed"] for r in rr)),
          f(stat.median(r["sub"]["cpu_seconds"]/r["sub"]["elapsed"] for r in rr)),
          f(median(rr,"pub","max_rss_kib")/1024)+"/"+f(median(rr,"sub","max_rss_kib")/1024)]
         for b in ["a","b"] for v in ["baseline","final"]
         for rr in [[r for r in stress if r["backend"]==b and r["variant"]==v]]])
    s+="\nCPU 核当量为进程 CPU 秒/各自测量墙钟秒；1 相当于占满一个逻辑 CPU。峰值 RSS 是进程生命周期峰值，两进程共享页可能重复计数，不能相加当物理共享内存使用量。\n\n"
    s+="独立池将同一档总容量从全机共 40 块变为 1000 话题合计 10000 块；容量增长和隔离同时发生，不能把改善全部归因于减少锁竞争。失败和回退率见汇总及逐轮账本；各轮成功发送均全部收齐。A 保留 TLV 回退，B 的借样失败仍需单独统计。\n\n"
    s+="代价是应用可见尾延迟明显上升：十秒轮新版本的 p99 达到毫秒级，旧版本约百微秒级，详见逐轮账本。新版本接纳更多在途消息、改变了队列与背压分布，并增大映射/缓存工作集；这些都是可能原因，本轮未再次做分段插桩，不能断言某一原因已被证实。**交付率改善成立，低延迟改善不成立。**\n\n"
    s+="## 全部压力轮账本\n\n短轮每轮计划 10000000 条，长轮每轮计划 60000000 条。\n\n"
    s+=table(["版本/路径/秒/轮","状态","尝试","发送","最终接收","借样失败","TLV回退","p50 μs","p99 μs","逐话题不守恒数","错误/重复/驱逐"],
        [[f'{r["variant"]}/{r["backend"]}/{r["duration"]}/{r["repeat"]}',r["status"],
          r["pub"]["attempts"],r["pub"]["sent"],r["sub"]["received"],r["pub"]["loan_failed"],r["pub"]["fallback"],
          f(r["sub"]["latency_p50_us"]),f(r["sub"]["latency_p99_us"]),
          sum(t["expected"]!=t["delivered"] for t in r["sub"]["topic_delivery"]),
          f'{r["sub"]["bad"]}/{r["sub"]["duplicate"]}/{r["sub"]["queue_evicted"]}'] for r in stress+long])
    s+="\n### 状态解释及长期轮结论\n\n"
    for b in ["a","b"]:
        rr=[r for r in long if r["backend"]==b]
        s+=f'- {b.upper()} 的 60 秒三轮：计划 {sum(r["pub"]["plan"] for r in rr)} 条，尝试 {sum(r["pub"]["attempts"] for r in rr)} 条，成功发送 {sum(r["pub"]["sent"] for r in rr)} 条，最终接收 {sum(r["sub"]["received"] for r in rr)} 条；借样失败 {sum(r["pub"]["loan_failed"] for r in rr)} 次，TLV 回退 {sum(r["pub"]["fallback"] for r in rr)} 次。\n'
    s+="\n"
    s+=table(["轮次","降级原因"],[[f'{r["variant"]}/{r["backend"]}/{r["duration"]}/{r["repeat"]}',
             "；".join(r.get("reasons",[])) or "无"] for r in stress+long])
    s+="\nA 的池空通过既有 TLV 回退交付，不计为 B 式 loan_failed；B 的序号缺口需结合发送失败解释。本报告分别检查成功发送、最终接收和逐话题守恒，不把未成功发出的序号计为接收端丢包。池满时不能保证满速零回退；多订阅者、慢消费者、应用长期持样会进一步改变结果。\n"
    (out/"topic_pool_stress_analysis.md").write_text(s)

    s="# DzFlat 每话题每尺寸档十块池：执行结果\n\n"
    s+="已实施每个底层话题通道、每个尺寸档 10 块的独立 DzFlat 载荷池。A、B、预构造段以及 SHM RPC 请求/响应均接入；RPC 两个方向按各自通道隔离。同话题的多个发布句柄共享这 10 块，多订阅者共享消息并由最后持有者归还。普通 libipc loan、SHM/TLV 继续用原 40 块共享池，Socket 不变。\n\n"
    s+="方案与中断续跑进度见[执行记录](topic_pool_execution.md)，性能详见[速度分析](topic_pool_speed_analysis.md)和[压力分析](topic_pool_stress_analysis.md)。池隔离降低了千话题容量争抢，但压力尾延迟上升，不能作为低延迟优化发布。\n\n"+common
    s+="\n## 正确性与回归\n\n17 组回归最终全部通过，其中新增话题池回归包含 12 个用例；16 格全字节 pub/sub 与 RPC 通信全部通过。首次失败和修正均保留在执行记录，未扩大池容量或放宽交付正确性断言。\n\n"
    s+=table(["回归程序","17 组回归首次退出码","修正后退出码"],[[r["test"],r["exit"],revised[r["test"]]["exit"] if r["test"] in revised else "无需重跑"] for r in units])
    s+="\n新回归覆盖：精确十块、归还循环、话题/prefix/尺寸隔离、同话题多句柄共享、旧池仍 40、实际段容量、旧新池交替收取、广播最后持有者、消息晚于路由释放、sniffer、独立子进程映射、并发首次借样、非法 ID 与超大请求。\n\n"
    s+=table(["全字节路径","载荷 B","状态","成功发送","接收或成功响应"],[[r["mode"]+"/"+r["backend"],r["bytes"],r["status"],r["pub"]["sent"],
          r["sub"]["received"] if r["mode"]!="rpc" else r["pub"]["sent"]] for r in quick])
    s+="\n## 1000 话题资源实测\n\n每话题同时持有 10 块，第 11 次必须返回池满；随后全部归还并验证重新借出。只触及请求载荷首尾页，没有全量填充大档。逻辑容量来自共享文件 st_size，实际占用来自 st_blocks × 512；不把 VmSize 当物理内存。下表均为全部 1000 话题总和。\n\n"
    s+=table(["每次请求 B","池数量","池逻辑 MiB","池实际 MiB","含路由全部段数","全部段实际 MiB","进程RSS MiB","进程VmSize MiB"],
             [[r["requested_bytes"],h["pools"],f(h["pool_logical_bytes"]/2**20),f(h["pool_allocated_bytes"]/2**20),
               h["segments"],f(h["allocated_bytes"]/2**20),f(h["rss_kib"]/1024),f(h["vms_kib"]/1024)]
              for r in resources for h in [r["checkpoints"][1]]])
    s+="\n2 MiB 档共 19.5408 GiB 逻辑池容量；首尾触页时池实际为 82.031 MiB。该实验验证一万个借样可同时成立，不是 19.54 GiB 全量写入或 32 GiB 满载稳定性测试。64 B 和 1024 B 的原始 loan 请求都落到 1 KiB 档；真实 DzFlat 还需加消息头再选档。\n\n"
    s+="本平台单池文件大小为 `52 + 10 × align_up(尺寸档容量 + 16, 1024)` 字节，52 包括池头和段尾引用计数。池按需创建；若 1000 话题每个都曾用到 1–64 KiB 的全部 64 档及 128 KiB–2 MiB 的 5 个大档，合计约 **58.34 GiB 逻辑容量**，超过 32 GiB。因此本方案不能理解为“所有话题所有尺寸同时预分配也必定够用”。\n\n"
    s+="归还借样仅归还块，不清除已触页内容。实测销毁全部路由后仍保留 1000 个池：池句柄由进程级缓存持有，进程退出时释放；动态创建大量不同话题/尺寸档会累计映射与物理页。当前没有新增全局 32 GiB 配额或页预留器。\n\n"
    s+="## 使用边界\n\n- 默认 view 上限与池容量同为 10；取出后持有的 Sample、未发布借样也占池。慢消费者可能把十块全部占满，A 回退、B 借样失败是保留的行为。\n- `loan_t` 布局保留，新本地池标识映射为线上负 ID -2…-11。新收端可读旧共享池与新话题池，旧收端会拒绝新池 ID；新池通信要求双方升级。未增加能力协商，不能承诺混合版本交付。公开 chan_impl 旧符号未删除，新增三个模板实例的 loan_topic；内部 STL 模板符号有变化，这不是完整 ABI 兼容性认证。\n- 池名包含完整底层通道名并受操作系统名字长度限制。新增命名开销会缩短可用话题名上限；本次未引入散列缩名。以 Linux 的 255 字节段名限制为准，部署长话题名需要连同 prefix、版本、尺寸和容量后缀检查。\n- 后续若要降低压力延迟，应另测相同成功输入下的分段耗时、应用取样等待和 worker 排队；本次不调整默认 32-worker 或借样池容量。\n\n"
    s+="## 构建与运行复现\n\n所有工作目录放仓库外。先将上述旧 Git 的 include、src、test/transport_comparison 快照放入工作目录的 baseline-source，再分别从 baseline-source/test/transport_comparison 与当前 test/transport_comparison 配置 CMake 至 baseline/build、final/build。二者均执行 Release 构建。\n\n"
    s+="```bash\n# 工作目录、RouDi 路径与配置需替换为本机值；配置可使用既有测试配置。\npython3 -B test/transport_comparison/repair_units.py --work <工作目录>/units --targets <报告所列17个目标>\n\n# 每阶段在 isolated.sh 内执行；脚本使用同一 flock 保证串行。\nbash test/transport_comparison/isolated.sh <RouDi> <配置> <RouDi日志> \\\n  python3 -B test/transport_comparison/topic_pool_runs.py --work <工作目录> --stage units\nbash test/transport_comparison/isolated.sh <RouDi> <配置> <RouDi日志> \\\n  python3 -B test/transport_comparison/topic_pool_runs.py --work <工作目录> --stage quick --tag topic10-v2\n# 同样运行 --stage compare、--stage long。配置或二进制变化时使用新 tag。\npython3 -B test/transport_comparison/topic_pool_resources.py --work <工作目录> --roudi <RouDi> --config <配置>\npython3 -B test/transport_comparison/topic_pool_report.py --work <工作目录> --tag topic10-v2\n```\n\n报告脚本针对本次证据布局；新执行若无修正重跑，可省略 revised-tests/results.json。\n\n"
    s+="## 证据指纹\n\n本次性能二进制、库及运行器 SHA-256 如下；报告生成时核对当前二进制与运行归档一致。源码交付指纹见下表。\n\n"
    manifest=read(w/f"{a.tag}-compare-manifest.json")
    for v in ["baseline","final"]:
        for name,digest in manifest[v].items():
            assert hashlib.sha256((w/v/"build"/name).read_bytes()).hexdigest()==digest
    if (w/"final-verification.json").exists():
        verification=read(w/"final-verification.json")
        assert verification["exit"]==0 and verification["identical"]
        s+="最后整理注释与行尾后已重新构建，comparison 与 libipc.so 均与正式测试版本逐字节一致。\\n\\n"
    s+=table(["对象","SHA-256"],[[v+"/"+k,h] for v in ["baseline","final"] for k,h in manifest[v].items()]+
             [[k,manifest[k]] for k in ["script","runner"]])
    paths=[ROOT/p for p in ["include/libipc/def.h","include/libipc/ipc.h","src/libipc/ipc.cpp","src/libipc/memory/resource.h",
        "src/libipc/utility/id_pool.h","src/libipc/sniffer.cpp","include/dzIPC/shm_pub_sub_ipc.h",
        "src/dzIPC/shm_pub_sub_ipc.cc","src/dzIPC/shm_ser_cli_ipc.cc","src/dzIPC/common/nodelet_config.cc",
        "include/dzIPC/common/nodelet_config.h","include/dzIPC/common/loaned_message.h",
        "test/test_topic_chunk_pool.cpp","test/test_adopt_loan_quota.cpp","test/test_dzflat_transport.cpp"]]
    s+="\n"+table(["交付源码","SHA-256"],[[str(x.relative_to(ROOT)),hashlib.sha256(x.read_bytes()).hexdigest()] for x in paths])
    s+="\n全部正式性能轮错误消息计数合计："+str(sum(r["sub"]["bad"] for r in comp+long))+"；重复数合计："+str(sum(r["sub"]["duplicate"] for r in comp+long))+"。状态、失败、回退及延迟已逐轮归档到两份分析。构建存在原有 /proc 路径 snprintf 截断警告；编辑器部分 C++ 诊断仍引用旧安装头，真实 CMake 构建及回归结果作为验证依据。\n"
    out.mkdir(parents=True,exist_ok=True)
    (out/"topic_pool_results.md").write_text(s)
    print("三份报告已生成")
if __name__=="__main__":
    main()
