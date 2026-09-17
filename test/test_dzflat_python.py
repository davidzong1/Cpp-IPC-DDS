#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DZFlat 的 Python 侧验收 (docs/dzflat_shm.md Step 4)

验收门: 开了 DZFlat 的话题, Python / topic_echo 必须能看到**完整字段**。

在 Step 4 之前这条是断的, 而且断法很难察觉 —— 不是"退化成慢路径", 而是**静默丢消息**:
订阅循环在 dzflat_read 返回 false 时 continue(见 shm_pub_sub_ipc.cc 的双 wire 分派),
而 GenericMessage 的默认实现就是返回 false。所以本文件的第一条断言是"收到了", 第二条
才是"字段对"。

读的路径见 docs/dzflat_shm.md §9.6: generator 把 schema 以数据形式发到了 Python
(python/dzipc/gen_msgs/_dzflat_schema.py), 由 python/dzipc/dzflat.py 解码。

**发**的路径也已经打通(§9.7): `dzipc.publish_dzflat()` 用同一份 schema 把段写出来
(`dzflat.pack()`), 再交给 `PublisherIPC.publish_prebuilt_segment()` 借 chunk 送出;
段不可用时回退普通 TLV。所以 [1]-[6] 需要 C++ 发布端(dzflat_py_publisher), 而 [9]-[12]
是 Python 自己发、自己收。

用法: python3 test/test_dzflat_python.py [--publisher build/bin/dzflat_py_publisher]
退出码 0 = 全部通过。

解释器: 本脚本会**自动切换**到与 python/dzipc/ 下那个 _dzipc_core 模块匹配的解释器。
pybind 模块按具体 Python 版本编译, 用错版本会静默加载到别的(或加载失败), 而症状看起来
像通道故障 —— 见 docs/dzflat_known_issues.md 第 1 条。所以这里不硬编码版本号: 硬编码
本身就是同一个坑的另一种形态(构建目标换了解释器, 脚本就跟着失灵)。
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_ROOT, "python"))


def _reexec_with_matching_interpreter():
    """按 python/dzipc/ 下模块的 ABI 标签切换解释器。

    _dzipc_core.cpython-312-x86_64-linux-gnu.so 里的 "312" 就是它要求的 Python 版本。
    当前解释器不匹配时用匹配的那个重新 exec 自己; 找不到就直接报错退出, 不做无谓尝试
    —— 静默用错版本正是本仓踩过的坑。"""
    import glob
    import re
    if os.environ.get("_DZFLAT_REEXEC"):
        return
    sos = glob.glob(os.path.join(_ROOT, "python", "dzipc", "_dzipc_core.cpython-*.so"))
    if not sos:
        return   # 未构建, 让后面的 import 报出原本的错误
    tags = {re.search(r"cpython-(\d)(\d+)", os.path.basename(p)).groups() for p in sos}
    if len(tags) > 1:
        print("[ERROR] python/dzipc/ 下并存多个 Python 版本的模块: %s" %
              sorted(os.path.basename(p) for p in sos))
        print("[HINT] 重新构建一次即可(CMake 的 POST_BUILD 会清掉其他版本)")
        sys.exit(2)
    major, minor = next(iter(tags))
    if (str(sys.version_info[0]), str(sys.version_info[1])) == (major, minor):
        return
    exe = "python%s.%s" % (major, minor)
    import shutil
    full = shutil.which(exe)
    if full is None:
        print("[ERROR] 模块要求 %s, 但系统里找不到它" % exe)
        print("[HINT] 用构建时的解释器运行, 或重新构建")
        sys.exit(2)
    os.environ["_DZFLAT_REEXEC"] = "1"
    print("[INFO] 模块由 %s 构建, 切换解释器重跑" % exe)
    sys.stdout.flush()   # execv 不会刷 Python 的缓冲, 不刷这条提示就丢了
    os.execv(full, [full, os.path.abspath(__file__)] + sys.argv[1:])


_reexec_with_matching_interpreter()

try:
    import dzipc as ipc
    from dzipc import dzflat
    from dzipc._dzipc_core import GenericMessage
except Exception as exc:   # pragma: no cover
    print("[ERROR] 无法 import dzipc:", exc)
    print("[HINT] 需要先构建 pybind 模块(make _dzipc_core)")
    sys.exit(1)


# ---- 与 test/dzflat_py_publisher.cpp 共享的期望值(那边是写死的) ----
IMG_W, IMG_H, IMG_STEP = 8, 4, 24
IMG_BYTES = IMG_STEP * IMG_H            # 96
EXPECT_IMG_DATA = [(i * 3) % 251 for i in range(IMG_BYTES)]
EXPECT_IMG = {
    "width": IMG_W,
    "height": IMG_H,
    "step": IMG_STEP,
    "encoding": "rgb8",
    "frame_id": "camera_optical_frame",
    "stamp": 1234.5,
}
CLOUD_N = 5
EXPECT_CLOUD_PTS = [[float(i), i * 2.0, i * 3.0] for i in range(CLOUD_N)]
EXPECT_CLOUD_CH = [i * 0.5 for i in range(CLOUD_N)]

_failures = []


def check(cond, what):
    if cond:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        _failures.append(what)
    return cond


def _subscribe(topic, msg_id, queue=32, transport=None):
    td = ipc.make_topic_data(GenericMessage(), msg_id) if hasattr(ipc, "make_topic_data") \
        else ipc.TopicDataPtrMake(GenericMessage(), msg_id)
    sub = ipc.SubscriberIPCPtrMake(td, topic, 0, queue,
                                   ipc.IPC_SHM if transport is None else transport, False)
    sub.InitChannel()
    return sub, td


def _collect(pub_bin, topic, msg_id, dzflat_on, kind, want, budget=10.0):
    """起发布端, 订阅直到收到 want 条(或超时)。返回 (消息列表, 发布端退出码)。"""
    sub, td = _subscribe(topic, msg_id)
    time.sleep(0.4)
    proc = subprocess.Popen(
        [pub_bin, topic, str(msg_id), "1" if dzflat_on else "0", kind, "8"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    got = []
    deadline = time.time() + budget
    while len(got) < want and time.time() < deadline:
        ok, out = sub.try_get_clone(td)
        if not ok:
            time.sleep(0.01)
            continue
        msg = out.topic() if out is not None else td.topic()
        got.append(msg)
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:   # pragma: no cover
        proc.kill()
    return got, proc.returncode


def test_image_dzflat(pub_bin):
    print("\n[1] DZFlat 图像 → Python 完整字段")
    got, _ = _collect(pub_bin, "/dzflat_py/img", 61, True, "image", 1)
    if not check(len(got) > 0, "收到了 DZFlat 消息(不是被静默丢弃)"):
        return
    msg = got[-1]
    if not check(msg.has_dzflat(), "GenericMessage 持有 DZFlat 段"):
        return
    check(msg.dzflat_is_borrowed(), "段是借样(未拷贝) —— Python 零拷贝已生效")

    schema = dzflat.schema_of(msg)
    if not check(schema is not None and schema.name == "StdImage",
                 "按 schema_hash 反查到 StdImage 的 schema"):
        return

    d = dzflat.decode_generic(msg)
    if not check(d is not None, "解码成功"):
        return
    check(sorted(d.keys()) == sorted(["header", "height", "width", "encoding", "step", "data"]),
          "字段集完整(6 个, 一个不少)")
    check(d["width"] == EXPECT_IMG["width"], "width = %d" % EXPECT_IMG["width"])
    check(d["height"] == EXPECT_IMG["height"], "height = %d" % EXPECT_IMG["height"])
    check(d["step"] == EXPECT_IMG["step"], "step = %d" % EXPECT_IMG["step"])
    check(d["encoding"] == EXPECT_IMG["encoding"], "encoding = %r" % EXPECT_IMG["encoding"])
    check(d["header"]["frame_id"] == EXPECT_IMG["frame_id"],
          "嵌套 header.frame_id = %r" % EXPECT_IMG["frame_id"])
    check(abs(d["header"]["stamp"] - EXPECT_IMG["stamp"]) < 1e-9,
          "嵌套 header.stamp = %r" % EXPECT_IMG["stamp"])

    px = d["data"]
    check(len(px) == IMG_BYTES, "data 长度 = %d" % IMG_BYTES)
    check(list(px) == EXPECT_IMG_DATA, "data 逐字节一致")
    # 零拷贝: 变长标量数组默认返回 memoryview 而不是 list
    check(isinstance(px, memoryview), "大数组以 memoryview 返回(零拷贝, 非 list)")


def test_cloud_dzflat(pub_bin):
    print("\n[2] DZFlat 点云 → 嵌套数组 / string[] 完整")
    got, _ = _collect(pub_bin, "/dzflat_py/cloud", 62, True, "cloud", 1)
    if not check(len(got) > 0, "收到了 DZFlat 点云"):
        return
    d = dzflat.decode_generic(got[-1])
    if not check(d is not None, "解码成功"):
        return
    check(d["header"]["frame_id"] == "lidar_top", "嵌套 header.frame_id")
    check(len(d["points"]) == CLOUD_N, "嵌套数组 points 元素数 = %d" % CLOUD_N)
    check([list(p["data"]) for p in d["points"]] == EXPECT_CLOUD_PTS,
          "嵌套数组逐元素一致(Tier-0 元素)")
    check(list(d["channels"]) == EXPECT_CLOUD_CH, "float64[] channels 一致")
    check(d["channel_names"] == ["intensity", "ring"], "string[] channel_names 一致")


def test_tlv_still_works(pub_bin):
    print("\n[3] 开关关闭 → 仍走 TLV, GenericMessage 原有路径不受影响")
    got, _ = _collect(pub_bin, "/dzflat_py/tlv", 63, False, "image", 1)
    if not check(len(got) > 0, "收到了 TLV 消息"):
        return
    msg = got[-1]
    check(not msg.has_dzflat(), "TLV 消息不应被当成 DZFlat 段")
    check(msg.field_count() == 6, "TLV 走查出 6 个字段")
    check(msg.get_uint32("width") == IMG_W, "TLV get_uint32('width')")
    check(msg.get_string("encoding") == "rgb8", "TLV get_string('encoding')")
    check(dzflat.decode_generic(msg) is None, "对 TLV 消息 decode_generic 返回 None")


def test_typed_wrapper_both_wires(pub_bin):
    """生成的封装类 Xxx.from_generic() 必须两种 wire 都能用。

    这是 Python 侧最常用的读法(比 dzflat.dump 更常用), 而 DZFlat 段里没有字段名 ——
    不给 from_generic 加 DZFlat 分支的话, 它会走到 g.get_uint32() 并抛
    "Field not found", 使用者只会看到一个莫名其妙的异常。"""
    from dzipc.gen_msgs.std_image import StdImage

    print("\n[4] 生成的封装类 StdImage.from_generic() 覆盖两种 wire")
    for label, dzflat_on, topic, mid in (("DZFlat", True, "/dzflat_py/w_flat", 64),
                                         ("TLV", False, "/dzflat_py/w_tlv", 65)):
        got, _ = _collect(pub_bin, topic, mid, dzflat_on, "image", 1)
        if not check(len(got) > 0, "%s: 收到消息" % label):
            continue
        try:
            img = StdImage.from_generic(got[-1])
        except Exception as exc:
            check(False, "%s: from_generic 抛异常 %r" % (label, exc))
            continue
        check(img.width == EXPECT_IMG["width"], "%s: width" % label)
        check(img.height == EXPECT_IMG["height"], "%s: height" % label)
        check(img.encoding == EXPECT_IMG["encoding"], "%s: encoding" % label)
        check(img.header.frame_id == EXPECT_IMG["frame_id"],
              "%s: 嵌套 header.frame_id(且是 StdHeader 对象而非 dict)" % label)
        check(list(img.data) == EXPECT_IMG_DATA, "%s: data 逐字节一致" % label)


def test_tamper_rejected():
    print("\n[5] 不信任段内容: 篡改/垃圾必须安全拒绝而不是错解")
    schema = dzflat.SCHEMA_BY_NAME["StdImage"]
    check(dzflat.decode(b"", schema) is None, "空缓冲 → None")
    check(dzflat.decode(b"\x00" * 64, schema) is None, "全零(magic 不符) → None")
    check(dzflat.decode(bytes(range(64)), schema) is None, "垃圾字节 → None")
    check(dzflat.parse_header(b"\x44\x5aFL" + b"\x00" * 8) is None, "长度不足 → None")

    # 构造一个合法段头但 schema_hash 不符
    import struct
    seg = bytearray(64)
    struct.pack_into("<IIIIIHHII", seg, 0, dzflat.MAGIC, 0xDEADBEEF, 32, 48, 64, 1, 0, 0, 0)
    check(dzflat.decode(bytes(seg), schema) is None, "schema_hash 不符 → None(不错解)")
    check(dzflat.decode(bytes(seg)) is None, "未注册的 schema_hash → None")


def test_rx_counters(pub_bin):
    """接收侧拒收计数: 两侧各管一半, 合起来才完整。

    为什么非要有这一组: 拒收原本是**完全静默**的 —— C++ 侧订阅循环只是 continue,
    Python 侧 decode() 只是返回 None。于是"版本错配"这种一定会发生的部署事故, 现场
    表现是"消息量对不上, 两端都不报错"。计数是这个失败形态唯一的抓手。

    分工(这不是重复, 是必须的两半):
      C++  dzipc.DzFlatRxCounters()   判别 / msg_id / 结构门 / TLV 越界
      Py   dzflat.rx_stats()          按指纹查不到 schema —— C++ 侧看不见这一步, 因为
                                      Python 的载体 GenericMessage 是无 schema 的直通体,
                                      对任何格式合法的段都收下
    """
    print("\n[6] 接收侧计数: 静默拒收必须变成可观测的数")

    # ---- 6.1 活链路上 C++ 侧计数确实在动 ----
    ipc.ResetDzFlatRxCounters()
    dzflat.reset_rx_stats()
    got, _ = _collect(pub_bin, "/dzflat_py/rxcnt", 66, True, "image", 2)
    if not check(len(got) > 0, "收到了消息(前提)"):
        return
    st = ipc.DzFlatRxCounters()
    check(st.dzflat_accepted >= 1,
          "C++ 侧 dzflat_accepted 随收下的段增长(实测 %d)" % st.dzflat_accepted)
    check(st.defects == 0, "正常链路 defects 为 0(实测 %d)" % st.defects)

    # 解一遍, Python 侧的 accepted 应当跟上
    dzflat.reset_rx_stats()
    d = dzflat.decode_generic(got[-1])
    ps = dzflat.rx_stats()
    check(d is not None and ps["accepted"] == 1,
          "Python 侧 accepted 记到解码成功(实测 %r)" % (ps,))

    # ---- 6.2 真实版本错配: 只改结构指纹, msg_id 保持正确 ----
    #
    # 这正是"两端 msg 定义不是同一份"在 wire 上的样子: 前面每一道门都过, 只有指纹对不上。
    # 拿一个真收到的段来改, 而不是凭空拼一个 —— 凭空拼的段可能在更早的门就被挡下,
    # 那样测到的就不是这条路径。
    seg = bytearray(got[-1].dzflat_bytes())
    import struct
    real_hash = struct.unpack_from("<I", seg, 4)[0]
    struct.pack_into("<I", seg, 4, 0xDEADBEEF)

    dzflat.reset_rx_stats()
    check(dzflat.decode(bytes(seg)) is None, "改了指纹的段必须拒收, 而不是按错误布局错解")
    ps = dzflat.rx_stats()
    check(ps["schema_unknown"] == 1 and ps["accepted"] == 0,
          "Python 侧记为 schema_unknown —— 这一项非 0 就等于'该重跑 generator 了'(实测 %r)"
          % (ps,))

    # 指纹对得上但 root_size 与本地 schema 不符 → 另一种成因, 必须分开记。
    #
    # 这里的错值必须**比真值小**: 段头自洽性检查(total >= 32 + root_size)会先把过大的
    # root_size 挡在 parse_header, 那样测到的是 header_bad 而不是 schema_mismatch。
    seg2 = bytearray(got[-1].dzflat_bytes())
    real_root = struct.unpack_from("<I", seg2, 12)[0]
    assert real_root > 8, real_root
    struct.pack_into("<I", seg2, 12, real_root - 8)
    dzflat.reset_rx_stats()
    check(dzflat.decode(bytes(seg2)) is None, "root_size 与本地 schema 不符的段必须拒收")
    ps = dzflat.rx_stats()
    check(ps["schema_mismatch"] == 1,
          "记为 schema_mismatch(与 schema_unknown 分开; 实测 %r)" % (ps,))

    # 段头自相矛盾(root_size 比整段还大) → header_bad, 是"段被截断/损坏", 不是"不是 DZFlat"
    seg3 = bytearray(got[-1].dzflat_bytes())
    struct.pack_into("<I", seg3, 12, 0xFFFF)
    dzflat.reset_rx_stats()
    check(dzflat.decode(bytes(seg3)) is None, "段头自相矛盾的段必须拒收")
    ps = dzflat.rx_stats()
    check(ps["header_bad"] == 1 and ps["not_dzflat"] == 0,
          "记为 header_bad 而非 not_dzflat —— 混在一起, TLV 流量会把这个真信号淹掉"
          "(实测 %r)" % (ps,))

    # ---- 6.3 TLV 消息走到 Python 解码器: 记 not_dzflat, 不算缺陷 ----
    dzflat.reset_rx_stats()
    check(dzflat.decode(b"\x00" * 64) is None, "非 DZFlat 缓冲 → None")
    ps = dzflat.rx_stats()
    check(ps["not_dzflat"] == 1 and ps["header_bad"] == 0,
          "记为 not_dzflat —— 与 header_bad 分开(前者正常, 后者是缺陷; 实测 %r)" % (ps,))

    # ---- 6.4 计数器有牙: 未篡改的同一个段必须仍然解得开 ----
    #
    # 6.2 是在真段上改字节, 万一改错了位置(比如改到了负载里), 上面几条也会"通过"但
    # 测的是别的东西。这里用原始指纹回填, 确认段本身没被弄坏。
    struct.pack_into("<I", seg, 4, real_hash)
    dzflat.reset_rx_stats()
    check(dzflat.decode(bytes(seg)) is not None,
          "把指纹改回去就该解开 —— 证明 6.2 改的确实是指纹那 4 个字节")


def test_borrow_pins_the_chunk(pub_bin):
    """借样必须**钉住**共享 chunk: 持有一条消息期间, 后续流量不得改写它的字节。

    这条是"零拷贝"的另一半。只断言 dzflat_is_borrowed() 说明的是实现选了借样这条路;
    这里断言借样**真的是共享内存** —— 段若指向一个发布端可复用的 chunk, 后面几条消息
    发完它就已经被写花了。SHM 的 chunk 池每尺寸档只有 32 块, 所以收满 8 条就足够把
    没被钉住的块抢回去(实现若退回整段拷贝, 这条仍会过 —— 它守的是生命周期, 不是拷贝;
    两者相加才是 C++ 侧 Sample 的三条契约在 Python 上的对应物)。
    """
    print("\n[8] 借样钉住 chunk: 持有期间后续流量不得改写它")
    got, _ = _collect(pub_bin, "/dzflat_py/pin", 67, True, "image", 8)
    if not check(len(got) > 0, "收到了借样消息"):
        return
    held = got[0]
    if not check(held.dzflat_is_borrowed(), "第 1 条是借样(未拷贝)"):
        return
    held_view = held.dzflat_memoryview()      # 传阅视图: 不拷贝, 指向共享 chunk
    before = bytes(held_view)                 # 取一份副本当对照
    check(len(got) >= 8, "8 条都收齐了 —— 后续流量确实发生过(实测 %d 条)" % len(got))
    check(bytes(held_view) == before,
          "持有中的段字节没被后续流量改写(chunk 被借样钉住了)")

    d = dzflat.decode_generic(held)
    check(d is not None and list(d["data"]) == EXPECT_IMG_DATA,
          "持有期间借样段仍解码出正确字段")


# --------------------------------------------------------------- Python 侧发布
#
# 历史: Python 只能**收** DZFlat(GenericMessage 没有 schema, 写不出定长布局)。现在
# dzipc.dzflat.pack() 按生成 schema 把段写出来, 由 PublisherIPC.publish_prebuilt_segment()
# 借一块 chunk 送出去(见 docs/dzflat_shm.md §9.6)。发布端仍有**一次 memcpy**(段在 Python
# 地址空间生成), 但省掉了 TLV 组装与页尾分段, 而接收侧照旧借样零拷贝。


def _make_publisher(topic, msg_id, transport):
    td = ipc.make_topic_data(GenericMessage(), msg_id)
    pub = ipc.PublisherIPCPtrMake(td, topic, 0, transport, False)
    pub.InitChannel()
    return pub, td


def _make_std_image():
    """与 test/dzflat_py_publisher.cpp 共享的期望值造一条等价的 StdImage。"""
    img = ipc.StdImage()
    img.header.frame_id = EXPECT_IMG["frame_id"]
    img.header.stamp = EXPECT_IMG["stamp"]
    img.width = IMG_W
    img.height = IMG_H
    img.step = IMG_STEP
    img.encoding = EXPECT_IMG["encoding"]
    img.data = list(EXPECT_IMG_DATA)
    return img


def _drain_one(sub, td, budget=3.0):
    deadline = time.time() + budget
    while time.time() < deadline:
        ok, out = sub.try_get_clone(td)
        if ok:
            return out.topic() if out is not None else td.topic()
        time.sleep(0.005)
    return None


def _first_diff(a: bytes, b: bytes) -> str:
    """逐字节比对失败时给出**第一个**不符的偏移与上下文 —— 只说"不一致"对布局问题是废话。"""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            lo = max(0, i - 8)
            return ("首个不符字节 @%d: 参考 %s | 我方 %s"
                    % (i, a[lo:i + 8].hex(), b[lo:i + 8].hex()))
    return "前 %d 字节相同, 但长度不同(参考 %d, 我方 %d)" % (n, len(a), len(b))


def test_encoder_matches_cpp_writer(pub_bin):
    """Python 编码器与 C++ 写端**逐字节一致**。

    这是发布路径上最关键的一条门: 布局差一个字节, 对端要么按 kDzFlatSchemaDrop 拒收,
    要么指纹对上却读出错位的数据。判据不是"解出来差不多", 而是把同一份内容再写一遍, 与
    C++ 写端产出的段做逐字节比对(含段头 total_size / msg_id / 对齐填充)。
    两种入参都测: decode() 出的 dict, 与生成的封装对象(发布路径用的就是后者)。
    msg_id 故意取**非零**(7): 全零的段头会让"编码器根本没写 msg_id"也通过比对。
    """
    print("\n[9] 编码器与 C++ 写端逐字节一致")
    for label, kind, topic in (("StdImage", "image", "/dzflat_py/enc_img"),
                               ("StdPointCloud", "cloud", "/dzflat_py/enc_cloud")):
        got, _ = _collect(pub_bin, topic, 7, True, kind, 1)
        if not check(len(got) > 0, "%s: 收到 C++ 写端产出的段" % label):
            continue
        msg = got[-1]
        ref = bytes(msg.dzflat_bytes())            # C++ 写端的段(AcceptWire → dzflat_read 拷贝, 内容等价)
        d = dzflat.decode_generic(msg, zero_copy=True)
        schema = dzflat.schema_of(msg)
        if not check(d is not None and schema is not None, "%s: 解码并找到 schema" % label):
            continue

        mine = dzflat.pack(d, schema, msg_id=7)    # (a) dict 入参
        if check(mine is not None and bytes(mine) == ref,
                 "%s: dict 入参重新编码 = 原段(%d 字节, msg_id=7)" % (label, len(ref))):
            pass
        else:
            print("       " + _first_diff(ref, bytes(mine) if mine is not None else b""))

        wrapper = getattr(ipc, label).from_generic(msg)
        mine2 = dzflat.pack(wrapper, msg_id=7)     # (b) 封装对象入参
        if check(mine2 is not None and bytes(mine2) == ref,
                 "%s: 封装对象入参重新编码 = 原段" % label):
            pass
        else:
            print("       " + _first_diff(ref, bytes(mine2) if mine2 is not None else b""))
        # 比对本身得**有牙**: 段头里的 msg_id 必须真的参与编码, 否则上面两条只是在
        # 证明"布局一致"(全零也能对上)。
        other = dzflat.pack(d, schema, msg_id=8)
        check(other is not None and bytes(other) != ref,
              "%s: 改 msg_id 会改变段头(比对不是空转)" % label)


def test_python_publishes_dzflat():
    """Python 发布平坦段 → Python 订阅借样: 两条传输各跑一遍。

    接收侧走的是同一套借样路径(schema-less 话题 → 物化队列 + dzflat_adopt), 所以判据与
    C++ 发布端那几条完全一致。SHM 借的是发布方写好的 chunk; UDP 借的是接收层去帧出来的
    独立块 —— 后者正是 T1 那条"能力就绪但无生产者"的腿, 本轮给了它第一个生产者。
    """
    print("\n[10] Python 发布平坦段 → Python 订阅借样(SHM 与 UDP)")
    prior = ipc.IsDzFlatEnabled()
    ipc.EnableDzFlat(True)
    try:
        for label, transport in (("SHM", ipc.IPC_SHM), ("UDP", ipc.IPC_SOCKET)):
            topic = "/dzflat_py/pub_%s" % label.lower()
            sub, td = _subscribe(topic, 0, transport=transport)
            # msg_id 用 0: 与今日 Python 用法一致(模板与消息两侧都默认 0), 也是平坦段
            # 段头自证必须与话题模板相符的那一项场景。
            pub, _ptd = _make_publisher(topic, 0, transport)
            time.sleep(0.5)                                    # 会合: SHM 握手 / UDP 组播入组
            ipc.ResetDzFlatCounters()

            img = _make_std_image()
            msg = None
            went_flat = False
            deadline = time.time() + 5.0
            while msg is None and time.time() < deadline:
                went_flat = ipc.publish_dzflat(pub, img) or went_flat
                msg = _drain_one(sub, td, 0.2)
            if not check(msg is not None, "%s: 收到了 Python 自己发布的段" % label):
                continue
            check(went_flat, "%s: publish_dzflat 报告走了平坦段" % label)
            check(msg.has_dzflat(), "%s: 收到的是 DZFlat 段(不是回退的 TLV)" % label)
            check(msg.dzflat_is_borrowed(), "%s: 段是借样(未拷贝) —— 零拷贝已生效" % label)
            check(ipc.DzFlatPublishCount() > 0,
                  "%s: DZFlat 发布计数已增长(实测 %d)" % (label, ipc.DzFlatPublishCount()))

            out = ipc.StdImage.from_generic(msg)
            check(out.width == IMG_W and out.height == IMG_H and out.step == IMG_STEP,
                  "%s: 标量字段往返一致" % label)
            check(out.encoding == EXPECT_IMG["encoding"], "%s: string 字段往返一致" % label)
            check(out.header.frame_id == EXPECT_IMG["frame_id"],
                  "%s: 嵌套 string 往返一致" % label)
            check(abs(out.header.stamp - EXPECT_IMG["stamp"]) < 1e-9,
                  "%s: 嵌套 float64 往返一致" % label)
            check(list(out.data) == EXPECT_IMG_DATA, "%s: 大数组逐字节往返一致" % label)
    finally:
        ipc.EnableDzFlat(prior)


def test_publish_fallback():
    """回退矩阵: 不可用时必须退化成 TLV **送达**, 而不是丢消息或抛异常。

    三种成因各起一个话题, 每一条都同时断言两件事: `publish_dzflat` 返回 False, 且消息
    仍然以 TLV 到达(字段可读)。只断言返回值会漏掉"返回了 False 但其实没发"这种形态。
    """
    print("\n[11] 回退矩阵(开关关 / 无 schema / 段头 msg_id 不符)")
    prior = ipc.IsDzFlatEnabled()
    try:
        # (a) 开关关: 库级默认态, 也是灰度期最短的那一档
        ipc.EnableDzFlat(False)
        sub, td = _subscribe("/dzflat_py/fb_off", 0)
        pub, _ = _make_publisher("/dzflat_py/fb_off", 0, ipc.IPC_SHM)
        time.sleep(0.5)
        check(ipc.publish_dzflat(pub, _make_std_image()) is False, "开关关: 返回 False")
        msg = _drain_one(sub, td)
        check(msg is not None and not msg.has_dzflat() and msg.field_count() > 0,
              "开关关: 仍以 TLV 送达且字段可读")
        ipc.EnableDzFlat(prior)

        # (b) 类型没有 schema: 裸 GenericMessage —— pack() 返回 None, 直接回退
        sub, td = _subscribe("/dzflat_py/fb_noschema", 0)
        pub, _ = _make_publisher("/dzflat_py/fb_noschema", 0, ipc.IPC_SHM)
        time.sleep(0.5)
        g = GenericMessage()
        g.set_uint32("width", IMG_W)
        g.set_string("encoding", EXPECT_IMG["encoding"])
        check(dzflat.pack(g) is None, "无 schema: pack() 返回 None(回退信号)")
        check(ipc.publish_dzflat(pub, g) is False, "无 schema: 返回 False")
        msg = _drain_one(sub, td)
        check(msg is not None and not msg.has_dzflat(), "无 schema: 仍以 TLV 送达")
        check(msg is not None and msg.get_uint32("width") == IMG_W,
              "无 schema: TLV 字段可读")

        # (c) 段头 msg_id 与本话题模板不符 ⇒ C++ 侧复核后拒绝发出(对端只会静默丢), 回退 TLV
        sub, td = _subscribe("/dzflat_py/fb_badid", 0)
        pub, _ = _make_publisher("/dzflat_py/fb_badid", 0, ipc.IPC_SHM)
        time.sleep(0.5)
        check(dzflat.pack(_make_std_image(), msg_id=1000) is not None,
              "msg_id 不符: 编码本身成功(段头写的就是 1000)")
        check(ipc.publish_dzflat(pub, _make_std_image(), msg_id=1000) is False,
              "msg_id 不符: 发布端复核后返回 False")
        msg = _drain_one(sub, td)
        check(msg is not None and not msg.has_dzflat(), "msg_id 不符: 仍以 TLV 送达")
    finally:
        ipc.EnableDzFlat(prior)


def test_mixed_wire_on_one_topic():
    """同一话题上平坦段与 TLV 混跑: Python 侧两种 wire 都从**物化队列**出。

    与 C++ typed 话题不同, Python 的话题模板恒为 schema-less(GenericMessage), 所以两条
    wire 走同一条队列, 靠 has_dzflat() 判别 —— 不存在"漏 drain"问题。这条钉住这个差异,
    免得把 C++ 的双 drain 要求照搬到 Python 侧(那是无用的复杂度)。
    """
    print("\n[12] 混跑: 同一话题上平坦段 + TLV")
    prior = ipc.IsDzFlatEnabled()
    ipc.EnableDzFlat(True)
    try:
        sub, td = _subscribe("/dzflat_py/mixed", 0)
        pub, _ = _make_publisher("/dzflat_py/mixed", 0, ipc.IPC_SHM)
        time.sleep(0.5)
        img = _make_std_image()
        flat_ok = False
        for _ in range(20):
            flat_ok = ipc.publish_dzflat(pub, img)
            pub.publish(img.to_generic())          # 同一条内容的 TLV 形态
            time.sleep(0.02)
        check(flat_ok, "混合对流中至少一次走了平坦段")

        seen = {}
        deadline = time.time() + 4.0
        while len(seen) < 2 and time.time() < deadline:
            m = _drain_one(sub, td, 0.3)
            if m is None:
                continue
            seen[bool(m.has_dzflat())] = m
        check(set(seen.keys()) == {True, False},
              "两种 wire 都收到了(平坦段与 TLV; 实测 %r)" % sorted(seen.keys()))
        for is_flat, m in seen.items():
            w = ipc.StdImage.from_generic(m)
            check(w.width == IMG_W and list(w.data) == EXPECT_IMG_DATA,
                  "%s 形态读出的字段一致" % ("平坦段" if is_flat else "TLV"))
    finally:
        ipc.EnableDzFlat(prior)


def test_switch_not_inverted():
    """EnableDzFlat(False) 必须真的关掉。

    这条看着像废话, 但它守的是一个已经发生过的缺陷: dzipc.h 里曾有个与函数**同名**的
    对象式宏 EnableDzFlat, 展开后把实参静默反转成 true。而 EnableDzFlat(False) 恰好是
    灰度升级的第一步, 反转的后果是把 DZFlat 段推给未升级的订阅方 —— 静默丢消息。
    C++ 侧只有包含了 dzipc.h 的 TU 才会中招, Python 侧走的是 pybind, 这里一起钉住。
    """
    print("\n[7] 开关语义不得被反转")
    prev = ipc.IsDzFlatEnabled()
    try:
        ipc.EnableDzFlat(True)
        check(ipc.IsDzFlatEnabled() is True, "EnableDzFlat(True) → 开")
        ipc.EnableDzFlat(False)
        check(ipc.IsDzFlatEnabled() is False, "EnableDzFlat(False) → 关(不得反转)")
    finally:
        ipc.EnableDzFlat(prev)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--publisher",
                    default=os.path.join(_ROOT, "build", "bin", "dzflat_py_publisher"))
    args = ap.parse_args()
    if not os.path.exists(args.publisher):
        print("[ERROR] 找不到发布端: %s" % args.publisher)
        print("[HINT] make dzflat_py_publisher")
        return 2

    print("DZFlat Python 验收 (schema 注册数=%d)" % len(dzflat.SCHEMA_BY_HASH))
    test_image_dzflat(args.publisher)
    test_cloud_dzflat(args.publisher)
    test_tlv_still_works(args.publisher)
    test_typed_wrapper_both_wires(args.publisher)
    test_tamper_rejected()
    test_rx_counters(args.publisher)
    test_borrow_pins_the_chunk(args.publisher)
    test_encoder_matches_cpp_writer(args.publisher)
    test_python_publishes_dzflat()
    test_publish_fallback()
    test_mixed_wire_on_one_topic()
    test_switch_not_inverted()

    print("\n" + "=" * 60)
    if _failures:
        print("失败 %d 项:" % len(_failures))
        for f in _failures:
            print("  -", f)
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
