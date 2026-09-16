# -*- coding: utf-8 -*-
"""DZFlat 段的 Python 解码器 (设计见 docs/dzflat_shm.md §3, 落地见 §9.6)

为什么需要它
------------
`GenericMessage` 是**自描述 TLV** 的通用走查器 —— 它靠 wire 里的字段名工作。DZFlat 是
定长布局, wire 里没有字段名, 所以 C++ 侧的 `GenericMessage` 解不了它: 缺 schema。而
schema 只存在于 generator 产出的物件里, 且**没有任何 TU 会编译那些生成头文件**
(Python 进程尤其如此), 所以 C++ 侧拿不到。

于是分工是: C++ 侧把段原样直通(`GenericMessage::dzflat_read`), schema 以数据形式发到
Python(`gen_msgs/_dzflat_schema.py`), 由本模块解码。

顺带的好处
----------
Python 读 DZFlat 比读 TLV **更省**: 没有页尾交错要剥, 而大数组可以用
`memoryview.cast()` 零拷贝直读 —— 而 TLV 路径的 `get_uint8_array()` 会把一张 1 MB 的
图变成一个百万元素的 Python list。

不信任段内容
------------
段是别的进程写进共享内存的。所有 VarRef 在解引用前都做边界校验, 越界返回空值而不是
抛异常或读出段外字节 —— 与 C++ 侧 `dzflat::Reader` 同一套契约。
"""

from __future__ import annotations

import struct
from typing import Any, Dict, NamedTuple, Optional, Tuple

# ---------------------------------------------------------------- 段格式常量
# 必须与 include/ipc_msg/ipc_msg_base/dzflat.h 保持一致。
MAGIC = 0x4C465A44          # 'D','Z','F','L' 小端
LAYOUT_VER = 1
SEG_HEADER_SIZE = 32
VARREF_SIZE = 8

# IDL 标量 → (struct 格式字符, 字节数)。bool 在 wire 上是 uint8。
_SCALAR = {
    "bool": ("B", 1),
    "int8": ("b", 1),
    "uint8": ("B", 1),
    "int16": ("h", 2),
    "uint16": ("H", 2),
    "int32": ("i", 4),
    "uint32": ("I", 4),
    "int64": ("q", 8),
    "uint64": ("Q", 8),
    "float32": ("f", 4),
    "float64": ("d", 8),
}


class Field(NamedTuple):
    """schema 里的一个字段。offset 是它在 Root 记录内的字节偏移。

    kind 与 generator 的七种形态一一对应:
        scalar / fixed_scalar_array / string / var_scalar_array /
        string_array / nested / nested_array
    ftype 是 dzIPC::FieldType 枚举值 —— 与 TLV 路径 field_type(i) 同一套编号, 于是通用
    工具(topic_echo)可以对两种 wire 用同一份渲染代码。
    """

    name: str
    kind: str
    base: Optional[str]
    ftype: int
    offset: int
    count: Optional[int]
    nested: Optional["Schema"]


class Schema(NamedTuple):
    name: str
    schema_hash: int
    root_size: int
    fields: Tuple[Field, ...]


SCHEMA_BY_HASH: Dict[int, "Schema"] = {}
SCHEMA_BY_NAME: Dict[str, "Schema"] = {}


def register(schema: "Schema") -> "Schema":
    """由生成的 _dzflat_schema.py 调用。"""
    SCHEMA_BY_HASH[schema.schema_hash] = schema
    SCHEMA_BY_NAME[schema.name] = schema
    return schema


class SegHeader(NamedTuple):
    magic: int
    schema_hash: int
    root_off: int
    root_size: int
    total_size: int
    layout_ver: int
    flags: int
    msg_id: int


_HDR = struct.Struct("<IIIIIHHII")


def parse_header(buf) -> Optional[SegHeader]:
    """解析段头。不是 DZFlat 段(或长度不足/自相矛盾)时返回 None。"""
    if buf is None or len(buf) < SEG_HEADER_SIZE:
        return None
    magic, sh, roff, rsize, total, ver, flags, msg_id, _rsv = _HDR.unpack_from(buf, 0)
    if magic != MAGIC or ver != LAYOUT_VER:
        return None
    if roff != SEG_HEADER_SIZE or total > len(buf):
        return None
    if total < SEG_HEADER_SIZE + rsize:
        return None
    return SegHeader(magic, sh, roff, rsize, total, ver, flags, msg_id)


def looks_like_dzflat(buf) -> bool:
    return parse_header(buf) is not None


def _in_bounds(total: int, off: int, cnt: int, stride: int) -> bool:
    """[off, off + cnt*stride) 是否落在段内。off == 0 表示空字段。"""
    if off == 0 or off >= total:
        return False
    if stride and cnt > (total - off) // stride:
        return False
    return True


def _read_varref(buf, at: int) -> Tuple[int, int]:
    return struct.unpack_from("<II", buf, at)


def _decode_record(buf, total: int, schema: "Schema", root: int,
                   zero_copy: bool) -> Dict[str, Any]:
    """解码一条 Root 记录(可递归)。root 是该记录在段内的起始偏移。"""
    out: Dict[str, Any] = {}
    for f in schema.fields:
        at = root + f.offset
        k = f.kind

        if k == "scalar":
            fmt, _sz = _SCALAR[f.base]
            (v,) = struct.unpack_from("<" + fmt, buf, at)
            out[f.name] = bool(v) if f.base == "bool" else v

        elif k == "fixed_scalar_array":
            fmt, sz = _SCALAR[f.base]
            vals = struct.unpack_from("<" + fmt * f.count, buf, at)
            out[f.name] = [bool(x) for x in vals] if f.base == "bool" else list(vals)

        elif k == "string":
            off, n = _read_varref(buf, at)
            if n == 0 or not _in_bounds(total, off, n, 1):
                out[f.name] = ""
            else:
                out[f.name] = bytes(buf[off:off + n]).decode("utf-8", "replace")

        elif k == "var_scalar_array":
            off, n = _read_varref(buf, at)
            fmt, sz = _SCALAR[f.base]
            if n == 0 or not _in_bounds(total, off, n, sz):
                out[f.name] = memoryview(b"").cast(fmt) if zero_copy else []
            elif zero_copy:
                # 零拷贝: 视图直接落在段上。cast 要求起点按元素对齐 —— DZFlat 的变长区
                # 每块都按 8 对齐(见 dzflat.h kAlign), 所以恒成立。
                out[f.name] = memoryview(buf)[off:off + n * sz].cast(fmt)
            else:
                vals = struct.unpack_from("<" + fmt * n, buf, off)
                out[f.name] = [bool(x) for x in vals] if f.base == "bool" else list(vals)

        elif k == "string_array":
            off, n = _read_varref(buf, at)
            items = []
            if n and _in_bounds(total, off, n, VARREF_SIZE):
                for i in range(n):
                    soff, slen = _read_varref(buf, off + i * VARREF_SIZE)
                    if slen and _in_bounds(total, soff, slen, 1):
                        items.append(bytes(buf[soff:soff + slen]).decode("utf-8", "replace"))
                    else:
                        items.append("")
            out[f.name] = items

        elif k == "nested":
            # 嵌套 Root 内联在本记录里 —— 一次偏移加法, 没有间接层。
            out[f.name] = _decode_record(buf, total, f.nested, at, zero_copy)

        elif k == "nested_array":
            off, n = _read_varref(buf, at)
            elems = []
            esz = f.nested.root_size
            if n and _in_bounds(total, off, n, esz):
                for i in range(n):
                    elems.append(_decode_record(buf, total, f.nested, off + i * esz, zero_copy))
            out[f.name] = elems

        else:   # pragma: no cover - schema 由 generator 产出, 不应出现未知形态
            raise ValueError("未知的 DZFlat 字段形态: %r" % (k,))
    return out


# ------------------------------------------------------------------ 接收计数
#
# 为什么 Python 侧要单独计: C++ 层的 GenericMessage 是**直通体**(无 schema, 见
# generic_message.hpp), 对任何格式合法的段都返回成功。所以 C++ 的 dzflat_schema_drop
# 在 Python 进程里恒为 0 —— 版本错配要到这里、按指纹查注册表时才暴露。
#
# 而这一步的失败原本是**静默**的: decode() 返回 None, 调用方通常当成"没数据"。于是
# "对端换了 msg 定义但这边没重跑 generator"的现场表现是消息凭空消失, 两侧都不报错。
#
#   accepted         解出来了。拒收数没有分母无法解读, 故必须有
#   not_dzflat       段首 magic 不符 —— **正常**。TLV 消息走到这里就会记一笔, 量可能
#                    远大于其余各项
#   header_bad       magic 符但段头自相矛盾(长度不足 / total_size 超出缓冲 / root_size
#                    比段还大) ⇒ 段被**截断或损坏**。与 not_dzflat 分开是必须的: 混在
#                    一起, TLV 流量会把这个真信号淹到看不见
#   schema_unknown   指纹不在本进程的注册表里 ⇒ **msg 定义变了但没重跑 generator**,
#                    或对端用的是别的 msg 集合
#   schema_mismatch  指纹对上但 root_size 与本地 schema 不符, 或与显式传入的 schema
#                    不符 ⇒ 两侧的 schema 不是同一份(比同名不同构更隐蔽)
#
# 与 C++ 侧的 dzipc.DzFlatRxCounters() 合起来看才是完整的接收侧图景。
_RX = {"accepted": 0, "not_dzflat": 0, "header_bad": 0,
       "schema_unknown": 0, "schema_mismatch": 0}


def _has_magic(buf) -> bool:
    """段首 4 字节是不是 DZFlat 的 magic。
    用来区分"这压根不是 DZFlat 段"和"是 DZFlat 段但头被弄坏了" —— parse_header 对两者
    都返回 None, 而两者的处置完全不同。"""
    if buf is None or len(buf) < 4:
        return False
    return struct.unpack_from("<I", buf, 0)[0] == MAGIC


def rx_stats() -> Dict[str, int]:
    """Python 侧的 DZFlat 解码计数快照。字段含义见 _RX 上方的说明。"""
    return dict(_RX)


def reset_rx_stats() -> None:
    for k in _RX:
        _RX[k] = 0


def decode(buf, schema: Optional["Schema"] = None, zero_copy: bool = True):
    """把一个 DZFlat 段解成 dict。

    schema 为 None 时按段头的 schema_hash 查注册表。
    返回 None 表示: 不是 DZFlat 段 / 找不到 schema / 指纹与给定 schema 不符。
    每种结果都会记进 rx_stats() —— 返回 None 本身没有区分力, 而这几种成因的处置
    完全不同(见 _RX 上方)。

    zero_copy=True 时变长标量数组返回 memoryview(生命周期随 buf); False 时返回 list。
    """
    h = parse_header(buf)
    if h is None:
        _RX["header_bad" if _has_magic(buf) else "not_dzflat"] += 1
        return None
    if schema is None:
        schema = SCHEMA_BY_HASH.get(h.schema_hash)
        if schema is None:
            _RX["schema_unknown"] += 1
            return None
    elif schema.schema_hash != h.schema_hash:
        _RX["schema_mismatch"] += 1
        return None
    if h.root_size != schema.root_size:
        # 指纹对上但 Root 尺寸不对 —— schema 与对端不是同一份, 拒绝而不是错解。
        _RX["schema_mismatch"] += 1
        return None
    out = _decode_record(buf, h.total_size, schema, SEG_HEADER_SIZE, zero_copy)
    _RX["accepted"] += 1
    return out


_warned_stale_binding = False


def _check_binding(msg) -> bool:
    """区分"这条是 TLV 消息"和"pybind 模块太旧, 根本没有 DZFlat 绑定"。

    这两种情况都会让 decode_generic 返回 None, 但成因完全不同, 而后者的症状极具误导性:
    `import dzipc` 照常成功, schema 注册表(纯 Python)照常加载, 于是看起来像"通道不通"
    而不是"模块版本不对"。本仓的构建目标是 CMake 找到的那个解释器(当前是 python3.10),
    而 python/dzipc/ 下可能还留着别的版本的陈旧 .so —— 用不匹配的解释器就会静默用到它。

    判据: GenericMessage 在新旧模块里都有 field_count, 只有新模块才有 has_dzflat。
    详见 docs/dzflat_known_issues.md。
    """
    global _warned_stale_binding
    if hasattr(msg, "has_dzflat"):
        return True
    if hasattr(msg, "field_count") and not _warned_stale_binding:
        _warned_stale_binding = True
        import warnings
        warnings.warn(
            "dzipc 的 pybind 模块缺少 DZFlat 绑定(has_dzflat) —— 很可能用到了陈旧的 .so。"
            "DZFlat 消息会全部读不到, 且症状像通道不通。请用构建时的那个解释器"
            "(见 CMakeCache 里的 _Python3_EXECUTABLE), 或重建并更新 python/dzipc/ 下的 .so。"
            "详见 docs/dzflat_known_issues.md",
            RuntimeWarning, stacklevel=3)
    return False


def decode_generic(msg, zero_copy: bool = True):
    """从一个持有 DZFlat 段的 GenericMessage 解码。非 DZFlat 时返回 None。"""
    if not _check_binding(msg) or not msg.has_dzflat():
        return None
    return decode(msg.dzflat_memoryview(), zero_copy=zero_copy)


def schema_of(msg) -> Optional["Schema"]:
    """给定持有段的 GenericMessage, 返回其 Schema(未注册则 None)。"""
    if not _check_binding(msg) or not msg.has_dzflat():
        return None
    return SCHEMA_BY_HASH.get(msg.dzflat_schema_hash_rx())


# ------------------------------------------------------------------ 通用渲染
#
# 供 topic_echo 之类的通用工具使用: 同一份代码同时覆盖 TLV 与 DZFlat 两种 wire。
# 之所以能复用, 是因为 schema 里的 ftype 与 TLV 的 field_type(i) 是同一套编号
# (dzIPC::FieldType), 见 Field 的注释。

_MAX_INLINE = 8          # 数组超过这么多元素就摘要显示
_MAX_STR = 96            # 字符串超过这么长就截断


def _fmt(v, indent: int = 0) -> str:
    pad = "  " * indent
    if isinstance(v, dict):
        if not v:
            return "{}"
        inner = "\n".join("%s  %s: %s" % (pad, k, _fmt(x, indent + 1)) for k, x in v.items())
        return "\n" + inner
    if isinstance(v, memoryview):
        n = len(v)
        head = list(v[:_MAX_INLINE])
        return "<%d 个元素> %s%s" % (n, head, " …" if n > _MAX_INLINE else "")
    if isinstance(v, (list, tuple)):
        n = len(v)
        if n and isinstance(v[0], dict):
            shown = "\n".join("%s  [%d]: %s" % (pad, i, _fmt(v[i], indent + 1))
                              for i in range(min(n, _MAX_INLINE)))
            tail = "\n%s  … 共 %d 个" % (pad, n) if n > _MAX_INLINE else ""
            return "<%d 个元素>\n%s%s" % (n, shown, tail)
        head = list(v[:_MAX_INLINE])
        return "<%d 个元素> %s%s" % (n, head, " …" if n > _MAX_INLINE else "")
    if isinstance(v, str):
        return repr(v if len(v) <= _MAX_STR else v[:_MAX_STR] + "…")
    return repr(v)


def dump(msg) -> str:
    """把一条消息渲染成可读文本, 自动识别 TLV 与 DZFlat。

    DZFlat 走 schema 解码; TLV 走 GenericMessage 自带的字段走查。找不到 schema 时不装作
    成功 —— 明确说明是哪个指纹没注册, 否则排查时会误以为是通道不通。
    """
    if _check_binding(msg) and msg.has_dzflat():
        h = msg.dzflat_schema_hash_rx()
        # 先 decode 再查名字: decode 是计数的唯一入口(见 rx_stats), 提前 return 会绕过它。
        d = decode(msg.dzflat_memoryview())
        schema = SCHEMA_BY_HASH.get(h)
        if d is None:
            if schema is None:
                return ("<DZFlat 段: schema_hash=0x%08X 未注册>\n"
                        "  这条消息的类型不在本进程的 dzipc.gen_msgs 里 —— 通常是 msg 定义\n"
                        "  变了但没重跑 generator, 或对端用的是别的 msg 集合。" % h)
            return "<DZFlat 段: 解码失败(段头自相矛盾或被截断)>"
        body = "\n".join("  %s: %s" % (k, _fmt(v, 1)) for k, v in d.items())
        return "[wire=DZFlat schema=%s hash=0x%08X]\n%s" % (schema.name, h, body)

    # TLV: 用 GenericMessage 自己的走查器
    n = msg.field_count() if hasattr(msg, "field_count") else 0
    if n == 0:
        return "<空消息(既无 DZFlat 段, 也无 TLV 字段)>"
    getters = {
        1: "get_bool", 2: "get_int8", 3: "get_uint8", 4: "get_int16", 5: "get_uint16",
        6: "get_int32", 7: "get_uint32", 8: "get_int64", 9: "get_uint64",
        10: "get_float32", 11: "get_float64", 12: "get_string",
        13: "get_bool_array", 14: "get_int8_array", 15: "get_uint8_array",
        16: "get_int16_array", 17: "get_uint16_array", 18: "get_int32_array",
        19: "get_uint32_array", 20: "get_int64_array", 21: "get_uint64_array",
        22: "get_float32_array", 23: "get_float64_array", 24: "get_string_array",
    }
    lines = []
    for i in range(n):
        name = msg.field_name(i)
        ft = msg.field_type(i)
        g = getters.get(ft)
        if g is None:                      # 25/26: 嵌套 / 嵌套数组
            lines.append("  %s: <嵌套, ftype=%d>" % (name, ft))
            continue
        try:
            lines.append("  %s: %s" % (name, _fmt(getattr(msg, g)(name), 1)))
        except Exception as exc:           # 类型不匹配等, 不让一个字段毁掉整条
            lines.append("  %s: <读取失败: %s>" % (name, exc))
    return "[wire=TLV 字段数=%d]\n%s" % (n, "\n".join(lines))
