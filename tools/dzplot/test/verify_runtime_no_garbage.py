#!/usr/bin/env python3
"""dzplot 运行时会不会在 /dev/shm 留下**垃圾段** —— 真发布端 + 真 sniff 路径。

与 verify_segment_naming.py 的分工:
  - verify_segment_naming.py: 段名**对不对**(推导名 vs 传输层建出的名字);
  - 本脚本:                  运行时**有没有多建**段(名字对不对的另一半后果)。

判据是 /dev/shm 的**集合差**, 而且合法性以**传输层自己建出的段**为准:
  ① pre → 起真发布端 → post:  legit = post - pre  ← 传输层建的, 不是我推导的
  ② 跑 LiveSniffSource(与 main() 同一条 sniff 路径) → after
  ③ garbage = (after - pre) - legit
⛔ 不能拿"Python 推导的名字"当合法集合 —— 那正是被验的东西: 推导错了, 按错名建出的段
   会被自己的判据当成合法, 恒绿。

两条必须先知道的坑(都会让判据假红):
  - /dev/shm 里有**与本仓无关**的 root 段在自行增删(空跑 25s 实测:
    `LSHM_FUNC_CODE_AGENT-t*` 新增一个、旧的一个消失) ⇒ 按前缀排除。
  - 运行中 vs 退出后是**两个不同的量**: ipc::shm::handle 析构会 shm_unlink, 于是
    某些段只在运行中存在。只测一个会漏掉一半, 所以两边都测。

用法: python3.10 tools/dzplot/test/verify_runtime_no_garbage.py
退出码: 0=全过, 1=有失败, 2=SKIP(无绑定)
"""
import contextlib
import importlib.util
import io
import os
import sys
import time
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

for _cand in (DZPLOT_DIR.parents[1] / "python",
              DZPLOT_DIR.parents[1] / "build/python",
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
# 与本仓无关、自行增删的 root 段(空跑实测), 一律不参与判定。
# ⚠️ 前缀要按**族**排除而不是逐个列: 实测除 LSHM_FUNC_CODE_AGENT-t* 外, LSHM_SYNCTIME-t*
# 也会在运行中凭空出现/消失(同一个 root 侧 agent)。只列一个名会让偶发的另一个把
# "① 零垃圾"打成假红 —— 判据本身没问题, 是分母混进了别人的段。
UNRELATED = ("LSHM_", "NC_TYPE_SET_PARAM_")
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


def snap():
    try:
        return {p.name for p in SHM.iterdir() if not p.name.startswith(UNRELATED)}
    except OSError:
        return set()


def is_our_empty_control_shell(name, topic, domain):
    """这条垃圾段是不是**本次 topic** 的控制面空壳(裸文件, 不是数据通道)。

    ⛔ 必须按 topic 收窄, 不能只写 `name.endswith("_control2")`: 本仓的 C++ gtest 会并发
    跑 ser/cli 控制面(`*_ser_control2`), 那与本次判定无关却同样以 `_control2` 结尾 ——
    实测并发时它把"③ 控制面无空壳"打成假红。判据要的是"**这个** topic 多了一个裸段",
    不是"盘上有没有别人的控制面段"。
    """
    return (name == LSS._control_plane_name_for_topic(topic, domain)
            and "QU_CONN" not in name)


def rm(name):
    try:
        os.remove(SHM / name)
        return True
    except FileNotFoundError:
        return False


def clear_for(topic, domain):
    """清掉该 topic 可能遗留的一切(含错名字形态)。

    必须清干净: 判据 ③ 的前提就是"盘上这些文件只可能是本次跑出来的"。
    """
    kept = set()
    for name in (LSS._channel_name_for_topic(topic, domain),
                 LSS._control_plane_name_for_topic(topic, domain)):
        kept.add(name)
    gone = []
    for p in list(SHM.iterdir()):
        if any(k in p.name for k in kept):
            if rm(p.name):
                gone.append(p.name)
    return gone


def with_control_plane_name(ctrl_name, topic):
    """把控制面名字换成给定形态(用于 teeth 对照) —— 只改运行时, 不碰产品文件。"""
    LSS._control_plane_name_for_topic = staticmethod(
        lambda t, d=0: ctrl_name if t == topic
        else LSS._channel_name_for_topic(t, d) + "_control2")


def with_blind_open():
    """把只读探测退回**修复前**的裸 open(create|open) —— 只改运行时, 不碰产品文件。

    这是 ②/③ 两组 teeth 需要的反向对照: 修复后"零垃圾"是因为**先探测、不存在就不 open**;
    要证明这一条确实在承重, 就得能在同一个测量装置里看到它被拿掉之后垃圾真的出现。
    """
    LSS._open_control_plane_readonly = classmethod(
        lambda cls, ipc_mod, cp_name, shm_dir=None: (
            lambda p: (p, p.generation(), "attached") if p.open(cp_name)
            else (None, 0, "open_failed"))(ipc_mod.TopicControlPlane()))


def measure(tag, topic, domain, publisher, stale_ctrl=False, blind_open=False):
    """跑一次真实 sniff 路径, 返回 (运行中垃圾, 退出后垃圾, 输出)。

    stale_ctrl=True 时把控制面名字退回修复前的错形态(那个名字**从来不存在**);
    blind_open=True 时把只读探测退回修复前的裸 create|open。
    两者都是 runtime monkeypatch, 不落盘、不改产品文件。
    """
    good_ctrl = LSS._channel_name_for_topic(topic, domain) + "_control2"
    orig_name = LSS._control_plane_name_for_topic
    orig_open = LSS._open_control_plane_readonly
    if blind_open:
        with_blind_open()
    if stale_ctrl:
        with_control_plane_name(LSS._channel_name_for_topic(topic, domain) + "_control", topic)
    else:
        with_control_plane_name(good_ctrl, topic)

    gone = clear_for(topic, domain)
    if gone:
        print(f"  note: 预清理 {gone}")

    pre = snap()
    pub = None
    if publisher:
        m = ipc.TestMsg()
        td = ipc.make_topic_data(m)
        pub = ipc.PublisherIPCPtrMake(td, topic, domain_id=domain, ipc_type=ipc.IPC_SHM,
                                      verbose=False)
        pub.InitChannel()
    legit = snap() - pre
    print(f"  info: 传输层建出的合法段 {len(legit)} 个")

    q = dzplot.BoundedPubQueue(max_size=4096)
    bp = dzplot.BackpressureController()
    src = LSS([{"topic": topic, "msg_type": "StdRawMessage"}], q, bp, transport="shm",
              domain=domain)
    buf = io.StringIO()
    mid: set = set()
    with contextlib.redirect_stdout(buf):
        src.start()
        time.sleep(1.2)
        mid = snap()                       # ← 运行中
        time.sleep(0.8)
    src.stop()
    after = snap()                         # ← 退出后
    out = buf.getvalue()

    g_mid = sorted((mid - pre) - legit)
    g_aft = sorted((after - pre) - legit)
    print(f"  --- {tag} ---")
    for line in out.splitlines():
        print(f"      | {line}")
    print(f"  运行中垃圾段 {len(g_mid)}: {g_mid}")
    print(f"  退出后垃圾段 {len(g_aft)}: {g_aft}")

    del src, pub
    clear_for(topic, domain)
    LSS._control_plane_name_for_topic = orig_name
    LSS._open_control_plane_readonly = orig_open
    return g_mid, g_aft, out


print("=" * 72)
print("dzplot 运行时垃圾段核对: 运行中 vs 退出后, 合法性以传输层为准")
print("=" * 72)

TOPIC = "/dzplot_garbage_probe"

# ---------------------------------------------------------------- ① 主路径
g_mid, g_aft, out = measure("① 修复后的名字 + 有发布端  domain=0", TOPIC, 0, publisher=True)
check(not g_mid and not g_aft, f"① 零垃圾段 (运行中={g_mid}, 退出后={g_aft})")
check("Control plane attached" in out, "① 运行时挂上了控制面(不是 open 失败/静默回退)")
check("generation=1" in out, "① generation=1(段名真的对上了; 恒 0 就是空壳)")
check("Sniffer attached" in out, "① Sniffer 挂上了数据通道(sniff 路径真在跑)")

# ---------------------------------------------------------------- ①b domain≠0
g_mid, g_aft, out = measure("①b 修复后的名字 + 有发布端  domain=3", TOPIC, 3, publisher=True)
check(not g_mid and not g_aft, f"①b domain=3 零垃圾段 (运行中={g_mid}, 退出后={g_aft})")
check("generation=1" in out, "①b generation=1(domain≠0 也真的对上了)")

# ---------------------------------------------------------------- ② teeth
# 修复后"零垃圾"有两个可能的空洞: (a) 测量装置根本看不见垃圾; (b) 判据恒真。两组对照
# 把只读探测/名字**退回修复前形态**(只在运行时 monkeypatch, 不碰产品文件), 垃圾必须出现。
g_mid, g_aft, out = measure("② teeth: 名字退回修复前形态(仍只读)", TOPIC, 0, publisher=True,
                            stale_ctrl=True)
check("Control plane absent" in out and "not creating it" in out,
      "② teeth 组: 错名字下只读探测判 absent 并记了 'not creating it'")
check(not g_mid and not g_aft,
      f"② 错名字 + 只读探测 ⇒ **仍零垃圾**(名字错不再造段; 修复前这里会多一个空壳段)")

g_mid, g_aft, out = measure("②teeth 名字退回修复前形态 + 裸 create|open",
                            TOPIC, 0, publisher=True, stale_ctrl=True, blind_open=True)
check(bool(g_mid) or bool(g_aft),
      f"②teeth 错名字 + **裸 open** ⇒ 垃圾段出现 ⇒ ① 的判据不是恒真 (运行中={g_mid}, 退出后={g_aft})")

g_mid, g_aft, out = measure("②teeth 正确名字 + 裸 create|open + 无发布端",
                            TOPIC, 0, publisher=False, blind_open=True)
check(any(is_our_empty_control_shell(n, TOPIC, 0) for n in g_mid),
      f"②teeth 裸 open 在无发布端时造空壳段 ⇒ 只读探测确实在承重 (运行中={g_mid})")

# ---------------------------------------------------------------- ③ 无发布端
# ⛔ ③ 还剩**一条产品侧遗留**: libipc 的 `sniffer::open` 走的是默认 acquire 模式
#    (include/libipc/shm.h:46, 默认 create|open), 无发布端时它反而成功并把数据通道
#    **建出来**, close 只 release 不 unlink ⇒ 段留在 /dev/shm。这是 sniffer 的通路,
#    不是控制面名字问题, 也不在 dzplot 的只读改动射程内 —— 这里钉住现状, 上游修好时本组
#    立刻红, 须有人来更新。
#    控制面那半**已修**: 只读探测判 absent, 不再造空壳(下面两条判据)。
print("\n⛔ ③ 数据通道残留是**产品侧遗留**(libipc sniffer::open 的默认 acquire 模式),")
print("   与控制面无关; 这里**钉住现状**: 上游修好时本组立刻红, 须有人来更新。")
g_mid, g_aft, out = measure("③ 修复后的名字 + **无发布端**", TOPIC, 0, publisher=False)
check(any("QU_CONN" in n for n in g_aft),
      f"③ 无发布端时 sniffer.open 会**造出**数据通道且 close 后仍在(残留={g_aft})")
check("Control plane absent" in out and "not creating it" in out,
      "③ 无发布端时控制面判 absent 并记 'not creating it'(不再静默建空壳)")
check(not any(is_our_empty_control_shell(n, TOPIC, 0) for n in g_mid + g_aft),
      f"③ 本 topic 的控制面空壳段**不存在**(运行中与退出后都没有, 修复前运行中会有 1 个)")

print()
print("=" * 72)
print(f"Results: {PASS} passed, {FAIL} failed")
print("=" * 72)
sys.exit(0 if FAIL == 0 else 1)
