#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DZFlat 的 Python 侧验收 (docs/dzflat_shm.md Step 4)

验收门: 开了 DZFlat 的话题, Python / topic_echo 必须能看到**完整字段**。

在 Step 4 之前这条是断的, 而且断法很难察觉 —— 不是"退化成慢路径", 而是**静默丢消息**:
订阅循环在 dzflat_read 返回 false 时 continue(见 shm_pub_sub_ipc.cc 的双 wire 分派),
而 GenericMessage 的默认实现就是返回 false。所以本文件的第一条断言是"收到了", 第二条
才是"字段对"。

发布端必须是 C++(bin/dzflat_py_publisher): Python 发不出 DZFlat —— GenericMessage 没有
schema, 写不出定长布局。读则可以, 因为 generator 另外把 schema 以数据形式发到了 Python
(python/dzipc/gen_msgs/_dzflat_schema.py)。

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


def _subscribe(topic, msg_id, queue=32):
    td = ipc.make_topic_data(GenericMessage(), msg_id) if hasattr(ipc, "make_topic_data") \
        else ipc.TopicDataPtrMake(GenericMessage(), msg_id)
    sub = ipc.SubscriberIPCPtrMake(td, topic, 0, queue, ipc.IPC_SHM, False)
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
