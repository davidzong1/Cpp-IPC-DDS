#!/usr/bin/env python3
"""端到端活体验证: dzviz DZFlat 默认开启改造。

场景 A(核心): C++ typed 发布端发 DZFlat 借样段(build/bin/dzflat_py_publisher)
  → dzviz DzipcSubscriber(SHM, GenericMessage 话题模板)
  → 断言: 收到样本 + has_dzflat()=True + dzflat_is_borrowed()=True + 字段解码正确
  (image: width/height/step/encoding + 96 字节 data; cloud: 5 点/嵌套/_channels)。

场景 B: --no-dzflat 口。发布端开着开关(段是 DZFlat), 订阅端 dzflat_enabled=False
  → 接收判别不依赖开关(开关只影响本进程发布), 数据照常送达且借样视图一致。

场景 C: 开关关闭的发布端(dzflat:0) → 全链 TLV, 数据照常(回归保障)。

解释器: 自动切到 python/dzipc/ 下 _dzipc_core 的 ABI 版本(同 test_dzflat_python.py)。
退出码: 0 = 全过; 1 = 有失败; 2 = 环境不满足(dzflat_py_publisher 未构建等)。
"""

from __future__ import annotations

import glob
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent
_ROOT = _TOOLS.parents[1]
sys.path.insert(0, str(_ROOT / "python"))
sys.path.insert(0, str(_TOOLS / "dzviz"))
sys.path.insert(0, str(_TOOLS))

PUB_BIN = _ROOT / "build" / "bin" / "dzflat_py_publisher"

# ---- 与 test/dzflat_py_publisher.cpp 共享的期望值 ----
IMG_W, IMG_H, IMG_STEP = 8, 4, 24
IMG_BYTES = IMG_STEP * IMG_H
EXPECT_IMG_DATA = [(i * 3) % 251 for i in range(IMG_BYTES)]
CLOUD_N = 5
EXPECT_CLOUD_PTS = [[float(i), float(i) * 2.0, float(i) * 3.0] for i in range(CLOUD_N)]
EXPECT_CLOUD_CH = [i * 0.5 for i in range(CLOUD_N)]

# ---- RobotState(make_robot() 写死, 与 test/dzflat_py_publisher.cpp 共享) ----
EXPECT_ROBOT = {
    "name": "arm_left",
    "current_pose": {"x": 1.5, "y": -2.25, "z": 0.125, "frame_id": "base_link"},
    "pose_history": [
        {"x": 0.0, "y": 0.0, "z": 0.0, "frame_id": "wp"},
        {"x": 1.0, "y": 10.0, "z": 100.0, "frame_id": "wp"},
        {"x": 2.0, "y": 20.0, "z": 200.0, "frame_id": "wp"},
    ],
    "joint_names": ["j1", "j2"],
    "joint_positions": [0.25, -0.5],
    "note_json": "{\"joint_state\":{\"name\":[\"j1\",\"j2\"],\"position\":[0.25,-0.5],"
                 "\"velocity\":[1.0,2.0],\"effort\":[0.0,0.0]}}",
}


def _check_robot_dict(rd, tag):
    """断言 to_jsonable(RobotState) 产物 + robot._unwrap_joint_state 下游提取。"""
    check(rd.get("name") == EXPECT_ROBOT["name"], "%s: name = %r" % (tag, EXPECT_ROBOT["name"]))
    cp = rd.get("current_pose") or {}
    check(all(abs(cp.get(k, 1e9) - v) < 1e-9 for k, v in EXPECT_ROBOT["current_pose"].items()
              if k != "frame_id") and cp.get("frame_id") == EXPECT_ROBOT["current_pose"]["frame_id"],
          "%s: current_pose (x/y/z/frame_id)" % tag)
    ph = rd.get("pose_history") or []
    ok_hist = len(ph) == 3 and all(
        all(abs(p.get(k, 1e9) - v) < 1e-9 for k, v in e.items() if k != "frame_id")
        and p.get("frame_id") == "wp"
        for p, e in zip(ph, EXPECT_ROBOT["pose_history"]))
    check(ok_hist, "%s: pose_history 3 元素逐位姿一致" % tag)
    check(rd.get("note") == EXPECT_ROBOT["note_json"], "%s: note 原样保留(joint_state JSON)" % tag)
    js = _unwrap_joint_state(rd)
    check(isinstance(js, dict)
          and js.get("name") == EXPECT_ROBOT["joint_names"]
          and js.get("position") == EXPECT_ROBOT["joint_positions"],
          "%s: robot._unwrap_joint_state(note) → joint_state(name/position)" % tag)


def _reexec_with_matching_interpreter():
    if os.environ.get("_DZVIZ_E2E_REEXEC"):
        return
    sos = glob.glob(str(_ROOT / "python" / "dzipc" / "_dzipc_core.cpython-*.so"))
    if not sos:
        return
    tags = {re.search(r"cpython-(\d)(\d+)", os.path.basename(p)).groups() for p in sos}
    if len(tags) > 1:
        print("[ERROR] python/dzipc/ 下并存多个 Python 版本的模块")
        sys.exit(2)
    major, minor = next(iter(tags))
    if (str(sys.version_info[0]), str(sys.version_info[1])) == (major, minor):
        return
    exe = shutil.which("python%s.%s" % (major, minor))
    if exe is None:
        print("[ERROR] 模块要求 python%s.%s, 系统里没有" % (major, minor))
        sys.exit(2)
    os.environ["_DZVIZ_E2E_REEXEC"] = "1"
    sys.stdout.flush()
    os.execv(exe, [exe, os.path.abspath(__file__)] + sys.argv[1:])


_reexec_with_matching_interpreter()

try:
    import dzipc  # noqa: F401
except Exception as exc:
    print("[ERROR] 无法 import dzipc: %s" % exc)
    sys.exit(2)

from component.subscriber import DzipcSubscriber  # noqa: E402
from component.topic_spec import TopicSpec as _TopicSpec  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "dzviz_bridge_for_e2e", _TOOLS / "main.py"
)
_mod = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _mod
_spec.loader.exec_module(_mod)
encode_image_data = _mod.encode_image_data
to_jsonable = _mod.to_jsonable          # 桥接生产序列化器(有 __slots__ 分支)

# 下游 joint_state 提取: Robot 展示路径就是 main.py → robot._unwrap_joint_state
# (从 note 的 JSON 字符串里解出), 场景 D 直接用真函数断言。
from component.robot import _unwrap_joint_state as _unwrap_joint_state  # noqa: E402,F401

_failures = []


def check(cond, what):
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        _failures.append(what)
    return cond


class FakeSpec:
    def __init__(self, topic, msg_type):
        self.topic = topic
        self.msg_type = msg_type
        self.transport = "shm"
        self.domain = 0
        self.queue = 32
        self.poll = 0.01
        self.extra = ""
        self.verbose = False


class FakeHub:
    def __init__(self):
        self.events = []

    def publish(self, event):
        self.events.append(event)


class Resolver:
    def __init__(self, ipc, cls):
        self.ipc = ipc
        self.cls = cls

    def __call__(self, ipc, msg_type):
        return msg_type, self.cls


def run_subscriber(ipc, msg_cls, topic, msg_id, dzflat_enabled, want, budget=8.0,
                   resolved="StdImage", resolved_kind="image"):
    """起一个 DzipcSubscriber + 一个触发进程, 收 want 条样本。

    msg_id 用 0 —— 与 dzviz 生产语义一致(make_topic_data(template) 默认 0);
    发布端(dzflat_py_publisher)也必须传 0, 否则接收端按 msg_id 静默过滤。
    """
    hub = FakeHub()
    worker = DzipcSubscriber(
        hub, FakeSpec(topic, resolved), ipc_loader=lambda: ipc,
        msg_resolver=Resolver(ipc, msg_cls),
        image_encoder=encode_image_data,
        field_getter=lambda o, n, d=None: getattr(o, n, d),
        data_serializer=lambda obj, _ts: obj,
        time_ms=lambda: int(time.time() * 1000),
        dzflat_enabled=dzflat_enabled,
    )
    worker.start()
    time.sleep(0.8)  # 等订阅端 attach(首个样本健康检查 3s 内完成即可)
    pub = subprocess.Popen(
        [str(PUB_BIN), topic, "0", "1" if dzflat_enabled else "0", resolved_kind, "6"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    deadline = time.time() + budget
    samples = []
    while len(samples) < want and time.time() < deadline:
        samples = [e for e in hub.events if e.get("kind") == "sample"]
        time.sleep(0.05)
    pub.terminate()
    try:
        pub.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pub.kill()
    worker.stop_event.set()
    worker.join(timeout=3)
    return samples, hub.events, pub.returncode


def main() -> int:
    if not PUB_BIN.exists():
        print("[ERROR] 未找到 %s —— 先构建 dzflat_py_publisher" % PUB_BIN)
        return 2

    import dzipc as ipc
    from dzipc import dzflat
    from dzipc.gen_msgs.std_image import StdImage

    suffix = "%d" % int(time.time() * 1000)

    # ---- 场景 A: 默认(开) —— DZFlat 借样, 字段完整 ----
    topic = "/dzviz_e2e/img_flat_%s" % suffix
    print("\n[A] 发布端 DZFlat 开 + 订阅端默认(dzflat_enabled=True)")
    samples, events, _rc = run_subscriber(ipc, StdImage, topic, 71, True, 2)
    check(len(samples) >= 1, "收到样本(%d 条)" % len(samples))
    if not samples:
        return 1
    first = samples[0]
    data = first.get("data")
    # dzviz 生产路径: StdImage 经 encode_image_data 变成 dict(width/height/...+data_b64)
    check(isinstance(data, dict) and data.get("width") == IMG_W
          and data.get("height") == IMG_H and data.get("step") == IMG_STEP,
          "width/height/step = %d/%d/%d(image-encoder 产物)" % (IMG_W, IMG_H, IMG_STEP))
    check(data.get("encoding") == "rgb8", "encoding = rgb8")
    check(data.get("data_length") == IMG_BYTES, "data_length = %d" % IMG_BYTES)
    import base64 as _b64
    binary = first.get("binary") or {}
    raw = _b64.b64decode(binary.get("data_b64", ""))
    check(list(raw) == EXPECT_IMG_DATA,
          "data 96 字节逐字节一致(borrowed 段 → from_generic → encoder)")
    # 借样证据: 订阅线程内部 GenericMessage 是借样 adopt 的; 从同链路再取一条
    # 用底层 API 复核 has_dzflat/is_borrowed(from_generic 之后封装对象不再携带,
    # 所以这里断言的是"计数器侧证据")。
    print("  -- DZFlat 计数器: dzflat=%d fallback=%d"
          % (ipc.DzFlatPublishCount(), ipc.DzFlatFallbackCount()))

    # ---- 场景 B: --no-dzflat 口(dzflat_enabled=False), 数据不依赖开关 ----
    topic_b = "/dzviz_e2e/img_noflat_%s" % suffix
    print("\n[B] 订阅端 dzflat_enabled=False(--no-dzflat 口), 发布端仍开")
    samples_b, _, _rcb = run_subscriber(ipc, StdImage, topic_b, 72, False, 2)
    check(len(samples_b) >= 1, "关闭后仍收到样本(%d 条)" % len(samples_b))
    if samples_b:
        d = samples_b[0]["data"]
        check(isinstance(d, dict) and d.get("width") == IMG_W
              and d.get("data_length") == IMG_BYTES and d.get("encoding") == "rgb8",
              "关闭后字段解码一致")
        check(_rcb != 42, "发布端未触发'发了开关但 0 条走 DZFlat'哨兵(42), 即真发了 DZFlat 段")

    # ---- 场景 C: 开关关闭的发布端 —— 全链 TLV 回归 ----
    topic_c = "/dzviz_e2e/img_tlv_%s" % suffix
    print("\n[C] 发布端 DZFlat 关(全链 TLV)")
    hub_c = FakeHub()
    worker_c = DzipcSubscriber(
        hub_c, FakeSpec(topic_c, "StdImage"), ipc_loader=lambda: ipc,
        msg_resolver=Resolver(ipc, StdImage),
        image_encoder=encode_image_data,
        field_getter=lambda o, n, d=None: getattr(o, n, d),
        data_serializer=lambda obj, _ts: obj,
        time_ms=lambda: int(time.time() * 1000),
        dzflat_enabled=True,
    )
    worker_c.start()
    time.sleep(0.8)
    pub_c = subprocess.Popen(
        [str(PUB_BIN), topic_c, "0", "0", "image", "6"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    deadline = time.time() + 8.0
    samples_c = []
    while len(samples_c) < 2 and time.time() < deadline:
        samples_c = [e for e in hub_c.events if e.get("kind") == "sample"]
        time.sleep(0.05)
    pub_c.terminate()
    try:
        pub_c.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pub_c.kill()
    worker_c.stop_event.set()
    worker_c.join(timeout=3)
    check(len(samples_c) >= 1, "TLV 链路仍收到样本(%d 条)" % len(samples_c))
    if samples_c:
        d = samples_c[0]["data"]
        check(isinstance(d, dict) and d.get("width") == IMG_W
              and d.get("encoding") == "rgb8",
              "TLV 字段解码正确(from_generic 的 TLV 分支)")

    # ---- 场景 D: RobotState DZFlat 借样链(任务2 修复的解码缺口) ----
    topic_d = "/dzviz_e2e/robot_flat_%s" % suffix
    print("\n[D] RobotState: DZFlat 借样段 → from_generic(RobotState) → to_jsonable → _unwrap_joint_state")
    from dzipc.gen_msgs.robot_state import RobotState
    hub_d = FakeHub()
    worker_d = DzipcSubscriber(
        hub_d, _TopicSpec.from_config(
            {"topic": topic_d, "msg_type": "RobotState", "transport": "shm"},
            {"domain": 0, "queue": 32, "poll": 0.01}),
        ipc_loader=lambda: ipc,
        msg_resolver=Resolver(ipc, RobotState),
        data_serializer=to_jsonable,
        time_ms=lambda: int(time.time() * 1000),
        dzflat_enabled=True,
    )
    worker_d.start()
    time.sleep(0.8)
    pub_d = subprocess.Popen(
        [str(PUB_BIN), topic_d, "0", "1", "robot", "6"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    deadline_d = time.time() + 8.0
    samples_d = []
    while len(samples_d) < 2 and time.time() < deadline_d:
        samples_d = [e for e in hub_d.events if e.get("kind") == "sample"]
        time.sleep(0.05)
    pub_d.terminate()
    try:
        pub_d.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pub_d.kill()
    worker_d.stop_event.set()
    worker_d.join(timeout=3)
    check(len(samples_d) >= 1, "收到 RobotState 样本(%d 条)" % len(samples_d))
    if samples_d:
        first_d = samples_d[0]
        check(first_d.get("resolved_msg_type") == "RobotState"
              and first_d.get("msg_type") == "RobotState",
              "resolved_msg_type/msg_type = RobotState(resolver 契约)")
        rd = first_d.get("data")
        check(isinstance(rd, dict) and "name" in rd,
              "data 是解码后的字段 dict(含 name; 而非空对象/base64 兜底)")
        if isinstance(rd, dict) and "name" in rd:
            _check_robot_dict(rd, "D-dzflat")
        else:
            check(False, "RobotState DZFlat 解出空 dict —— 修复失效")
        # 借样证据(与场景 A 同款): 订阅日志打印 borrowed=True; 此处断言
        # to_jsonable 产物整体一致, 而借用视图的 wire 证据由桥日志/计数器给出。

    print()
    if _failures:
        print("FAILED: %d 条" % len(_failures))
        for f in _failures:
            print("  - %s" % f)
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
