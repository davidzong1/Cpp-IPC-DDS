#!/usr/bin/env python3
"""段名核对: dzplot 推导出的段名, 必须**就是传输层真实建出的那一个**。

为什么不能只比字符串: dzplot 的段名是 C++ 规则的 Python 转写(name_operator.h 的注释
把本工具列为"已知复刻点")。两份字符串互相印证是**同义反复** —— 修复前 dzplot 的
"_topic_control" 也"自洽", 只是与传输层不符。所以这里起真实 SHM 发布端, 直接查
/dev/shm 的文件名是谁建的, 并检查修复前那两个错名字**没有**被建出来。

覆盖两个维度:
  - topic(发布订阅)腿 —— dzplot 的 sniffer 路径只走这一条(routing topology);
    dzplot **没有** ser/cli 腿(--transport socket 走的是 pub/sub 的 Subscriber API),
    所以 ser 的 "_ser_control2" 不在本脚本射程内, 属另一处。
  - domain —— 段名含 domain 才有隔离; 这里同时验 domain=0 与 domain≠0, 因为
    "默认 domain=0" 恰好是最容易蒙对的那一半。

用法: python3.10 tools/dzplot/test/verify_segment_naming.py
      (绑定只有 cpython-310 的构建; 3.12 载入失败 ⇒ 退出码 2 = SKIP, 不当成通过)
退出码: 0=全过, 1=有失败, 2=缺 dzipc 绑定(跳过)
"""

import gc
import importlib.util
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
DZPLOT_DIR = SCRIPT_DIR.parent
sys.path.insert(0, str(DZPLOT_DIR.parent / "dzviz"))
sys.path.insert(0, str(DZPLOT_DIR))

# 段名一律取自**被测工具自己**的静态方法 —— 本脚本不复刻任何名字。
_spec = importlib.util.spec_from_file_location("dzplot", str(DZPLOT_DIR / "main.py"))
dzplot = importlib.util.module_from_spec(_spec)
sys.modules["dzplot"] = dzplot
_spec.loader.exec_module(dzplot)
LSS = dzplot.LiveSniffSource
data_name = LSS._channel_name_for_topic
control_name = LSS._control_plane_name_for_topic

for _cand in (DZPLOT_DIR.parents[1] / "python",
              DZPLOT_DIR.parents[1] / "build_py/python",
              DZPLOT_DIR.parents[1] / "local/lib/python"):
    if str(_cand) not in sys.path:
        sys.path.append(str(_cand))

try:
    import dzipc as ipc
except Exception as exc:                      # noqa: BLE001 — 任何载入失败都算 SKIP
    print(f"SKIP: dzipc 绑定不可用({type(exc).__name__}: {exc})")
    print("      → 用 python3.10 跑本脚本(绑定是 cpython-310 构建)")
    sys.exit(2)

SHM = Path("/dev/shm")
PASS = 0
FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {msg}")
    else:
        FAIL += 1
        print(f"  FAIL  {msg}")


def exists(name):
    return (SHM / name).exists()


def libipc_files_containing(name):
    """盘上文件名里**包含** name 的那些(传输层数据通道的落盘形态)。

    数据通道由 libipc 建, 落盘名带前缀后缀, 不是裸的段名:
      __IPC_SHM__QU_CONN__<段名>__<queue_size>__<max_receivers>   ← 队列/route 通道
      __IPC_SHM__AC_CONN__<段名> / CC_ / RD_ / WT_ ...             ← 连接/等待者信道
    所以判据是"有文件的名字里含这个段名", 而不是"有个叫这个的裸文件"。后缀那两个
    数字随配置变, **不要**硬编码。
    对照: 控制面段是裸文件 —— TopicControlPlane 走 ipc::shm::acquire, 不经过 libipc
    的通道机制。
    """
    try:
        return sorted(p.name for p in SHM.iterdir() if name in p.name)
    except OSError:
        return []


def rm(name):
    try:
        os.remove(SHM / name)
        return True
    except FileNotFoundError:
        return False


def stale_forms(topic, domain):
    """修复前后出现过的两个错名字(都是"从来不存在的段")。

    ① dzplot 原样:   "dz_ipc_" + sanitize(topic) + "_topic_control"
    ② 早期测试写法:  数据段名 + "_control"  (少了那个 "2")
    """
    san = LSS._sanitize_topic_name(topic)
    return {
        "旧复刻(无 domain, _topic_control)": f"dz_ipc_{san}_topic_control",
        "缺 '2'(数据段名 + _control)": data_name(topic, domain) + "_control",
    }


def stale_data_name(topic):
    """修复前的**数据段**名(不含 domain): "dz_ipc_" + sanitize(topic) + "_topic"。

    domain 丢了在盘上不留裸文件, 只体现为 libipc 那些带前缀的文件名不同 —— 所以它得
    单独查(见 case() 里的 libipc 反查), 只对裸文件断言会漏掉这一半。
    """
    return "dz_ipc_" + LSS._sanitize_topic_name(topic) + "_topic"


def case(topic, domain):
    dom = f"domain={domain}"
    print(f"\n--- topic={topic!r} {dom} ---")
    dn = data_name(topic, domain)
    cn = control_name(topic, domain)
    print(f"  dzplot 推导: data={dn!r}  control={cn!r}")

    for label, name in stale_forms(topic, domain).items():
        if rm(name):
            print(f"  note: 预清理了遗留的错段 {name!r}({label})")

    gc.collect()
    for name in (dn, cn, dn + "_control2"):
        rm(name)
    # libipc 的带前缀文件也要清 —— 否则上一轮遗留的 QU_CONN 会让"文件在"这条判据
    # 恒真(判据 1 的前提就是"盘上这些文件只可能是本次传输层建的")。
    for f in libipc_files_containing(dn) + libipc_files_containing(stale_data_name(topic)):
        rm(f)

    msg = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg)
    pub = ipc.PublisherIPCPtrMake(
        topic_data, topic, domain_id=domain, ipc_type=ipc.IPC_SHM, verbose=False)
    pub.InitChannel()

    # 判据 1: 传输层**自己**建出的数据通道, 段名就是 dzplot 推导的那个。
    # 先查文件再 open —— TopicControlPlane.open() 是 create|open, 名字错了它会静默建
    # 空壳, 于是"open 成功"什么都证明不了; 而"文件在"只可能是传输层建的(上面刚清空)。
    built = libipc_files_containing(dn)
    queue_files = [f for f in built if "QU_CONN" in f]
    print(f"  info: 含段名的盘上文件 {len(built)} 个, 其中 QU_CONN {len(queue_files)} 个")
    for f in queue_files[:2]:
        print(f"        {f}")
    check(bool(queue_files), f"传输层按此段名建了数据通道 ({dn})")

    # 判据 2: 控制面段(裸文件)的名字同样是 dzplot 推导的那个。
    check(exists(cn), f"传输层建出的控制面段 == dzplot 推导名 ({cn})")

    # 判据 3: 错名字一个都不许在盘上 —— 出现就说明有人又照旧写法建了段。
    for label, name in stale_forms(topic, domain).items():
        check(not exists(name), f"错名字未被建出: {label} ({name})")
    check(not libipc_files_containing(stale_data_name(topic)),
          f"没有按'丢 domain 的数据段名'建通道 ({stale_data_name(topic)})")

    # 判据 4: 名字错了还有一道**静默**后果 —— 控制面 open 会得到空壳, generation 恒 0,
    # 发布端重启检测永久失效。所以 generation>0 是"段名真的对上了"的行为级证据。
    sniffer = ipc.Sniffer()
    check(sniffer.open(dn, ipc.SnifferTopology.route), f"Sniffer 挂上了 {dn}")
    sniffer.close()

    # 走**被测工具自己的**挂载路径(不是裸 open): 发布端在跑 ⇒ 段在 ⇒ 应当挂上。
    cp, gen, status = LSS._open_control_plane_readonly(ipc, cn)
    check(status == "attached", f"只读路径挂上控制面 {cn} (status={status})")
    if cp is not None:
        gen, state = cp.generation(), cp.state()
        print(f"  info: generation={gen} state={state}")
        check(gen > 0, f"generation > 0(= 真的挂到发布端那段, 不是空壳), got {gen}")
        check(state == ipc.TopicState.Ready, f"state == Ready, got {state}")

    del cp, pub
    gc.collect()

    for name in (dn, cn, dn + "_control2"):
        rm(name)
    for f in libipc_files_containing(dn):
        rm(f)


def case_readonly(topic, domain=0):
    """控制面**只读纪律**的段名核对: 段不存在时绝不建段。

    与本文件其余用例的分工: 那些查"名字算得对不对"(纯字符串 vs 真实段名), 这一条查
    "名字对不上/段不存在时的**行为**" —— 旧写法 `TopicControlPlane().open(name)` 是
    create|open, 会按**任何**名字静默建一个空壳段, 于是"挂不上真实的段"这件事没有症状,
    只在 /dev/shm 留下垃圾 + 重启检测永久失效。

    判据全在**盘**上: 不看 open() 的返回值(create|open 恒真, 那正是旧写法恒绿的原因)。
    """
    print(f"\n--- 只读纪律: topic={topic!r} domain={domain} ---")
    cn = control_name(topic, domain)
    dn = data_name(topic, domain)

    # 预清理: 存在的话这条就测不出"建没建"了
    for name in (dn, cn, dn + "_control2"):
        if rm(name):
            print(f"  note: 预清理了遗留段 {name!r}")
    for f in libipc_files_containing(dn):
        rm(f)

    check(not LSS._control_plane_segment_exists(cn),
          f"前置: 无发布端时控制面段不存在 ({cn})")

    plane, gen, status = LSS._open_control_plane_readonly(ipc, cn)
    check((plane, gen, status) == (None, 0, "absent"),
          f"段不存在 ⇒ (None, 0, 'absent'), 得到 ({plane}, {gen}, {status!r})")
    check(not exists(cn), f"只读路径**没有**建出空壳段 ({cn})")

    # 反面对照: 旧的 create|open 确实会建段 —— 证明上面那条不是恒真。
    # ⚠️ 这一步**故意**制造一个垃圾段, 随即清掉; 不在 /dev/shm 留痕。
    probe_name = cn + "_oldstyle_probe"
    rm(probe_name)
    old = ipc.TopicControlPlane()
    old.open(probe_name)
    check(exists(probe_name),
          "反面对照: 裸 create|open 会建出段(说明'只读'确实是修复出来的差别)")
    del old
    gc.collect()
    rm(probe_name)

    # 发布端在跑时, 同一条只读路径必须能挂上 —— 否则"不建段"可能只是"永远不工作"。
    msg = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg)
    pub = ipc.PublisherIPCPtrMake(topic_data, topic, domain_id=domain,
                                 ipc_type=ipc.IPC_SHM, verbose=False)
    pub.InitChannel()
    check(LSS._control_plane_segment_exists(cn),
          f"发布端起来后段就存在了 ({cn})")
    plane2, gen2, status2 = LSS._open_control_plane_readonly(ipc, cn)
    check(status2 == "attached" and plane2 is not None,
          f"同一条只读路径挂上了真实段 (status={status2})")
    check(gen2 > 0, f"generation > 0, got {gen2}")

    del plane2, pub
    gc.collect()
    for name in (dn, cn, dn + "_control2"):
        rm(name)
    for f in libipc_files_containing(dn):
        rm(f)


print("=" * 72)
print("dzplot 段名核对: 推导名 vs 传输层真实建出的段")
print("=" * 72)

case("/dzplot_naming/e2e", 0)
case("/dzplot_naming/e2e", 3)
# 非 ASCII topic: 这一条是 sanitize **逐字节**规则的端到端判据 —— 段名由 C++ 建出,
# dzplot 的 Python 转写必须算出同一个名字。按字符判断(旧写法)在这里必红:
# '/dzplot_naming/中文' 的 6 个 UTF-8 字节各要换一个 '_', 按字符只会换 2 个。
case("/dzplot_naming/中文", 0)

case_readonly("/dzplot_naming/readonly", 0)

print()
print("=" * 72)
print(f"Results: {PASS} passed, {FAIL} failed")
print("=" * 72)
sys.exit(0 if FAIL == 0 else 1)
