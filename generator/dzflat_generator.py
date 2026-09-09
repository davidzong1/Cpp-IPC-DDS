#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DZFlat 代码发射器 (设计见 docs/dzflat_shm.md)

对每个消息类型追加发射三样东西, **完全不触碰既有的 TLV serialize/deserialize**:

    XxxRoot   —— POD 记录, 其内存布局就是 wire; 带布局自证 static_assert
    XxxFlat   —— 静态工具: kSchemaHash / kRootTight / varlen_size / emit / size / write
    XxxView   —— 只读访问器 + copy_to(Xxx&) 兼容桥

字段的七种形态与 Root 成员的对应:

    标量                → 原地内联(bool 在 wire 上是 uint8)
    定长数组 T[N]       → 原地内联 T[N]
    string              → VarRef{off, 字节数}
    T[] (T 标量)        → VarRef{off, 元素数}
    string[]            → VarRef{off, 元素数} → 元素数个 VarRef 表 → 各自字节
    嵌套消息            → **原地内联其 Root**(任意深度都内联)
    嵌套消息[]          → VarRef{off, 元素数} → 元素数个连续 ElemRoot

嵌套类型的内部结构本模块并不需要知道: 尺寸用 sizeof 符号表达, hash 混入其
kSchemaHash 常量, 变长负载委托其 XxxFlat::varlen_size / emit。因此本模块只依赖
**当前消息的本地字段表**。
"""

from typing import List, Optional

# IDL 标量 → wire 类型。bool 在 wire 上固定为 uint8, 不依赖 sizeof(bool)。
WIRE_TYPE = {
    "bool": "std::uint8_t",
    "int8": "std::int8_t",
    "uint8": "std::uint8_t",
    "int16": "std::int16_t",
    "uint16": "std::uint16_t",
    "int32": "std::int32_t",
    "uint32": "std::uint32_t",
    "int64": "std::int64_t",
    "uint64": "std::uint64_t",
    "float32": "float",
    "float64": "double",
}

IND = "    "

# 标量 wire 类型的 (size, align)。与 WIRE_TYPE 一一对应。
_SCALAR_LAYOUT = {
    "bool": (1, 1),
    "int8": (1, 1),
    "uint8": (1, 1),
    "int16": (2, 2),
    "uint16": (2, 2),
    "int32": (4, 4),
    "uint32": (4, 4),
    "int64": (8, 8),
    "uint64": (8, 8),
    "float32": (4, 4),
    "float64": (8, 8),
}

_VARREF_LAYOUT = (8, 4)   # struct VarRef { u32 off; u32 cnt; }

# Root 一律 alignas(8), 所以嵌套 Root 的对齐恒为 8。
_ROOT_ALIGN = 8

# 跨类型的 Root 布局缓存: class_name -> {"size":…, "align":…, "fields":[…]}。
# 生成顺序已由 batch 生成器按依赖拓扑排序(sort_msg_files_by_dependencies), 所以生成某个
# 类型时它的全部嵌套类型必定已在缓存里。这个缓存是 Python 侧 schema 的唯一来源, 而
# 它算出的偏移会以**字面量**形式写进 C++ 的 static_assert —— 于是编译期就成了
# "Python 布局模型 vs 真实 ABI" 的交叉校验(见 docs/dzflat_shm.md §9.6)。
LAYOUT_CACHE = {}

# class_name -> kSchemaHash。与 LAYOUT_CACHE 同理由(依赖先行)可靠。
HASH_CACHE = {}

# 按生成顺序(= 依赖拓扑序)累积的 Python schema 条目, 供 batch 生成器写出
# python/dzipc/gen_msgs/_dzflat_schema.py。顺序很重要: 嵌套类型必须先定义。
PY_SCHEMA_ENTRIES = []

_FNV_OFFSET = 2166136261
_FNV_PRIME = 16777619
_U32 = 0xFFFFFFFF


def _fnv1a(data: bytes, h: int = _FNV_OFFSET) -> int:
    for b in data:
        h = ((h ^ b) * _FNV_PRIME) & _U32
    return h


def _fnv1a_u32(v: int, h: int) -> int:
    for shift in (0, 8, 16, 24):
        h = ((h ^ ((v >> shift) & 0xFF)) * _FNV_PRIME) & _U32
    return h


def compute_schema_hash(canon: str, nested_hashes) -> int:
    """复刻 dzflat.h 的 constexpr schema_hash()。

    Python 必须能独立算出与 C++ **逐位相同**的指纹, 否则 Python 侧按 schema_hash 查表
    会全查不到, DZFlat 消息在 Python 里就等于消失。这个等价性由 test_dzflat_python.py
    对着 C++ 导出的常量逐类型比对来守住。"""
    h = _fnv1a(canon.encode("utf-8"))
    for nh in nested_hashes:
        h = _fnv1a_u32(nh, h)
    return h


def _align_up(n: int, a: int) -> int:
    return ((n + a - 1) // a) * a


def _member_layout(f) -> tuple:
    """返回该字段在 Root 里占的 (size, align)。"""
    k = f.kind
    if k == "scalar":
        return _SCALAR_LAYOUT[f.base]
    if k == "fixed_scalar_array":
        sz, al = _SCALAR_LAYOUT[f.base]
        return (sz * f.count, al)
    if k == "nested":
        nested = LAYOUT_CACHE.get(f.nested_bare)
        if nested is None:
            raise ValueError(
                f"嵌套类型 {f.nested_bare} 的 Root 布局未知 —— "
                "生成顺序应保证依赖先行, 请检查 sort_msg_files_by_dependencies"
            )
        return (nested["size"], _ROOT_ALIGN)
    # string / T[] / string[] / Msg[] 都是 VarRef
    return _VARREF_LAYOUT


def compute_layout(cls: str, fs) -> dict:
    """按 Itanium ABI(GCC/Clang 在 Linux/ARM 上的规则)推导 Root 布局并入缓存。"""
    cur = 0
    align = _ROOT_ALIGN   # alignas(8) 是下限
    out_fields = []
    for f in fs:
        sz, al = _member_layout(f)
        off = _align_up(cur, al)
        cur = off + sz
        align = max(align, al)
        out_fields.append({"name": f.name, "offset": off, "size": sz, "align": al})
    info = {"size": _align_up(cur, align) if fs else _ROOT_ALIGN,
            "align": align, "fields": out_fields}
    LAYOUT_CACHE[cls] = info
    return info


class _Field:
    """把 msg_generator.FieldInfo 归一成本模块要用的七种形态之一。"""

    def __init__(self, f):
        self.name = f.field_name
        self.base = f.base_type
        self.is_array = f.is_array
        self.is_fixed = f.is_fixed_array
        self.count = f.array_size
        self.is_string = f.is_string
        self.is_nested = f.is_nested
        self.nested_class = (
            f"dzIPC::Msg::{f.nested_info.class_name}" if (f.is_nested and f.nested_info) else None
        )
        # 不带命名空间的类名, 用于查 LAYOUT_CACHE
        self.nested_bare = (
            f.nested_info.class_name if (f.is_nested and f.nested_info) else None
        )
        self.wire = WIRE_TYPE.get(f.base_type)

    # ---- 形态判定 ----
    @property
    def kind(self) -> str:
        if self.is_nested:
            return "nested_array" if self.is_array else "nested"
        if self.is_string:
            return "string_array" if self.is_array else "string"
        if self.is_array:
            return "fixed_scalar_array" if self.is_fixed else "var_scalar_array"
        return "scalar"

    @property
    def root_type(self) -> str:
        k = self.kind
        if k == "scalar":
            return self.wire
        if k == "fixed_scalar_array":
            return self.wire
        if k == "nested":
            return f"{self.nested_class}Root"
        return "dzflat::VarRef"

    @property
    def root_decl(self) -> str:
        k = self.kind
        if k == "fixed_scalar_array":
            return f"{self.root_type} {self.name}[{self.count}];"
        return f"{self.root_type} {self.name};"

    @property
    def elem_root(self) -> Optional[str]:
        return f"{self.nested_class}Root" if self.kind == "nested_array" else None

    @property
    def elem_flat(self) -> Optional[str]:
        return f"{self.nested_class}Flat" if self.is_nested else None

    @property
    def elem_view(self) -> Optional[str]:
        return f"{self.nested_class}View" if self.is_nested else None

    def count_expr(self, obj: str) -> str:
        """元素个数表达式(定长数组是常量, 变长取 .size())。"""
        if self.is_fixed:
            return str(self.count)
        return f"static_cast<std::uint32_t>({obj}.{self.name}.size())"

    # ---- schema hash 的规范化 token ----
    @property
    def token(self) -> str:
        """schema 指纹用的规范化 token。定长必须带上长度 —— 否则 string[3] 与
        string[] 同指纹, 而两者的 owning 容器类型不同(std::array vs std::vector)。"""
        suffix = ""
        if self.is_array:
            suffix = f"[{self.count}]" if self.is_fixed else "[]"
        if self.is_nested:
            return "{}" + suffix
        base = "string" if self.is_string else self.base
        return base + suffix


# dzIPC::FieldType 编号。必须与 msg_generator.TYPE_INDEX 及 generic_message.hpp 的
# enum FieldType 三方一致 —— Python 侧的通用渲染代码靠它对两种 wire 复用同一套逻辑。
_FT_SCALAR = {
    "bool": 1, "int8": 2, "uint8": 3, "int16": 4, "uint16": 5, "int32": 6,
    "uint32": 7, "int64": 8, "uint64": 9, "float32": 10, "float64": 11,
}
_FT_STRING = 12
_FT_SCALAR_ARRAY = {
    "bool": 13, "int8": 14, "uint8": 15, "int16": 16, "uint16": 17, "int32": 18,
    "uint32": 19, "int64": 20, "uint64": 21, "float32": 22, "float64": 23,
}
_FT_STRING_ARRAY = 24
_FT_NESTED = 25
_FT_NESTED_ARRAY = 26


def _field_type_index(f) -> int:
    k = f.kind
    if k == "nested":
        return _FT_NESTED
    if k == "nested_array":
        return _FT_NESTED_ARRAY
    if k == "string":
        return _FT_STRING
    if k == "string_array":
        return _FT_STRING_ARRAY
    if k == "scalar":
        return _FT_SCALAR[f.base]
    return _FT_SCALAR_ARRAY[f.base]   # fixed / var 标量数组


def python_schema_entry(cls: str, fs: List[_Field], schema_hash: int) -> str:
    """发射该类型在 python/dzipc/gen_msgs/_dzflat_schema.py 里的一段。

    Python 侧要靠这份 schema 才能解 DZFlat(定长布局的 wire 里没有字段名), 见
    python/dzipc/dzflat.py 的模块注释。偏移取自 LAYOUT_CACHE —— 与写进 C++
    static_assert 的字面量是**同一组数**, 所以 C++ 编译期就替 Python 校验了布局模型。
    """
    lay = LAYOUT_CACHE[cls]
    lines = [f"{cls} = _reg(Schema("]
    lines.append(f'{IND}name="{cls}",')
    lines.append(f"{IND}schema_hash=0x{schema_hash:08X},")
    lines.append(f"{IND}root_size={lay['size']},")
    lines.append(f"{IND}fields=(")
    for f, fl in zip(fs, lay["fields"]):
        base = "None" if f.is_nested or f.is_string else f'"{f.base}"'
        count = str(f.count) if f.is_fixed else "None"
        nested = f.nested_bare if f.is_nested else "None"
        lines.append(
            f'{IND}{IND}Field("{f.name}", "{f.kind}", {base}, '
            f"{_field_type_index(f)}, {fl['offset']}, {count}, {nested}),"
        )
    lines.append(f"{IND}),")
    lines.append("))")
    return "\n".join(lines)


def _fields(field_infos) -> List[_Field]:
    return [_Field(f) for f in field_infos]


def _gen_root(cls: str, fs: List[_Field]) -> List[str]:
    lay = compute_layout(cls, fs)
    out = [
        f"/* ---- DZFlat: {cls} 的 wire 记录。布局即 wire, 详见 docs/dzflat_shm.md §3.1 ---- */",
        f"struct alignas(8) {cls}Root",
        "{",
    ]
    for f in fs:
        out.append(f"{IND}{f.root_decl}")
    out.append("};")
    out.append("")

    root = f"{cls}Root"
    if not fs:
        out.append(f"static_assert(sizeof({root}) == 8, \"空消息的 Root 仅含对齐填充\");")
        out.append("")
        return out

    out.append("/* 布局自证。期望值是 generator 用 Itanium ABI 规则**独立算出的字面量**,")
    out.append(" * 不是从 offsetof 推回来的 —— 所以这几行同时校验两件事:")
    out.append(" *   ① 编译器没有插入意外填充(跨编译器/跨架构的 wire 一致性);")
    out.append(" *   ② generator 的布局模型与真实 ABI 一致 —— Python 侧的 DZFlat 解码器")
    out.append(" *      (python/dzipc/dzflat.py)用的就是这套偏移, 模型一错就会静默错解。")
    out.append(" * 详见 docs/dzflat_shm.md §9.6。 */")
    for fld in lay["fields"]:
        out.append(
            f"static_assert(offsetof({root}, {fld['name']}) == {fld['offset']}, "
            f"\"DZFlat 布局漂移: {root}::{fld['name']} 应在偏移 {fld['offset']}\");"
        )
    out.append(
        f"static_assert(sizeof({root}) == {lay['size']}, "
        f"\"DZFlat 布局漂移: sizeof({root}) 应为 {lay['size']}\");"
    )
    out.append(
        f"static_assert(alignof({root}) == {lay['align']}, "
        f"\"DZFlat 布局漂移: alignof({root}) 应为 {lay['align']}\");"
    )
    out.append("")
    return out


def _gen_flat(cls: str, fs: List[_Field]) -> List[str]:
    root = f"{cls}Root"
    canon = ";".join(f"{f.name}:{f.token}" for f in fs)
    nested_hashes = [f"{f.elem_flat}::kSchemaHash" for f in fs if f.is_nested]
    hash_args = ", ".join([f'"{canon}"'] + nested_hashes)
    # 同时用 Python 独立算一遍并缓存, 供 python schema 发射使用(见 compute_schema_hash)
    HASH_CACHE[cls] = compute_schema_hash(
        canon, [HASH_CACHE[f.nested_bare] for f in fs if f.is_nested]
    )
    # kRootTight: Root 内无任何填充字节(递归)。为真时元素块无需预先清零 —— 每个字节
    # 都会被 emit 写到, 既不泄漏共享内存里的陈旧内容, 也省掉一遍大块 memset。
    size_terms = []
    for f in fs:
        if f.kind == "fixed_scalar_array":
            size_terms.append(f"sizeof({root}::{f.name})")
        else:
            size_terms.append(f"sizeof({root}::{f.name})")
    tight_expr = f"sizeof({root}) == " + (" + ".join(size_terms) if size_terms else "8")
    for f in fs:
        if f.is_nested:
            tight_expr += f" && {f.elem_flat}::kRootTight"

    out = [
        f"/* View / Builder 定义在下方, Flat 里的 using 需要先声明。 */",
        f"class {cls}View;",
        f"class {cls}Builder;",
        "",
        f"struct {cls}Flat",
        "{",
        f"{IND}using root_t = {root};",
        f"{IND}using view_t = {cls}View;",
        f"{IND}using builder_t = {cls}Builder;",
        f"{IND}using owning_t = {cls};",
        "",
        f"{IND}/* schema 指纹: 本地字段规范化串 + 各嵌套类型的 hash, 全部编译期算完。",
        f"{IND} * 定长布局没有字段名可自描述, 版本全靠这个值兜住(docs/dzflat_shm.md §3.5)。 */",
        f"{IND}static constexpr std::uint32_t kSchemaHash = dzflat::schema_hash({hash_args});",
        f"{IND}static constexpr bool kRootTight = ({tight_expr});",
        "",
        f"{IND}/* 本消息在变长区占的字节上界(不含自己的 Root, Root 由父级内联)。 */",
        f"{IND}static std::uint32_t varlen_size(const {cls}& m)",
        f"{IND}{{",
        f"{IND}{IND}std::uint32_t n = 0;",
        f"{IND}{IND}(void)m; (void)n;",
    ]

    for f in fs:
        k = f.kind
        nm = f.name
        if k in ("scalar", "fixed_scalar_array"):
            continue
        if k == "string":
            out.append(
                f"{IND}{IND}n += dzflat::align_up(static_cast<std::uint32_t>(m.{nm}.size()));"
            )
        elif k == "var_scalar_array":
            out.append(
                f"{IND}{IND}n += dzflat::align_up(static_cast<std::uint32_t>("
                f"m.{nm}.size() * sizeof({f.wire})));"
            )
        elif k == "string_array":
            out.append(
                f"{IND}{IND}n += dzflat::align_up(static_cast<std::uint32_t>("
                f"m.{nm}.size() * sizeof(dzflat::VarRef)));"
            )
            out.append(f"{IND}{IND}for (const auto& s : m.{nm})")
            out.append(
                f"{IND}{IND}{IND}n += dzflat::align_up(static_cast<std::uint32_t>(s.size()));"
            )
        elif k == "nested":
            out.append(f"{IND}{IND}n += {f.elem_flat}::varlen_size(m.{nm});")
        elif k == "nested_array":
            out.append(
                f"{IND}{IND}n += dzflat::align_up(static_cast<std::uint32_t>("
                f"m.{nm}.size() * sizeof({f.elem_root})));"
            )
            out.append(f"{IND}{IND}for (const auto& e : m.{nm})")
            out.append(f"{IND}{IND}{IND}n += {f.elem_flat}::varlen_size(e);")

    out += [
        f"{IND}{IND}return n;",
        f"{IND}}}",
        "",
        f"{IND}/* 把 m 写进调用方已分配的 root, 变长负载追加到 w。 */",
        f"{IND}static bool emit(const {cls}& m, {root}* root, dzflat::Writer& w)",
        f"{IND}{{",
        f"{IND}{IND}(void)m; (void)root; (void)w;",
    ]

    for f in fs:
        k = f.kind
        nm = f.name
        out.append("")
        if k == "scalar":
            if f.base == "bool":
                out.append(f"{IND}{IND}root->{nm} = m.{nm} ? 1u : 0u;")
            else:
                out.append(f"{IND}{IND}root->{nm} = static_cast<{f.wire}>(m.{nm});")
        elif k == "fixed_scalar_array":
            out.append(f"{IND}{IND}for (std::uint32_t i = 0; i < {f.count}; ++i)")
            if f.base == "bool":
                out.append(f"{IND}{IND}{IND}root->{nm}[i] = m.{nm}[i] ? 1u : 0u;")
            else:
                out.append(
                    f"{IND}{IND}{IND}root->{nm}[i] = static_cast<{f.wire}>(m.{nm}[i]);"
                )
        elif k == "string":
            out.append(f"{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t n = "
                f"static_cast<std::uint32_t>(m.{nm}.size());"
            )
            out.append(f"{IND}{IND}{IND}const std::uint32_t off = n ? w.reserve(n) : 0;")
            out.append(f"{IND}{IND}{IND}if (n && off == 0) return false;")
            out.append(f"{IND}{IND}{IND}if (n) std::memcpy(w.at(off), m.{nm}.data(), n);")
            out.append(f"{IND}{IND}{IND}root->{nm} = dzflat::VarRef{{off, n}};")
            out.append(f"{IND}{IND}}}")
        elif k == "var_scalar_array":
            out.append(f"{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t cnt = "
                f"static_cast<std::uint32_t>(m.{nm}.size());"
            )
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t bytes = "
                f"static_cast<std::uint32_t>(cnt * sizeof({f.wire}));"
            )
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t off = bytes ? w.reserve(bytes) : 0;"
            )
            out.append(f"{IND}{IND}{IND}if (bytes && off == 0) return false;")
            if f.base == "bool":
                # std::vector<bool> 是位集特化, 没有可 memcpy 的连续 bool 存储。
                out.append(f"{IND}{IND}{IND}if (cnt) {{")
                out.append(
                    f"{IND}{IND}{IND}{IND}auto* dst = reinterpret_cast<{f.wire}*>(w.at(off));"
                )
                out.append(f"{IND}{IND}{IND}{IND}for (std::uint32_t i = 0; i < cnt; ++i)")
                out.append(f"{IND}{IND}{IND}{IND}{IND}dst[i] = m.{nm}[i] ? 1u : 0u;")
                out.append(f"{IND}{IND}{IND}}}")
            else:
                out.append(
                    f"{IND}{IND}{IND}if (bytes) std::memcpy(w.at(off), m.{nm}.data(), bytes);"
                )
            out.append(f"{IND}{IND}{IND}root->{nm} = dzflat::VarRef{{off, cnt}};")
            out.append(f"{IND}{IND}}}")
        elif k == "string_array":
            out.append(f"{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t cnt = "
                f"static_cast<std::uint32_t>(m.{nm}.size());"
            )
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t tbl = cnt ? w.reserve("
                f"static_cast<std::uint32_t>(cnt * sizeof(dzflat::VarRef))) : 0;"
            )
            out.append(f"{IND}{IND}{IND}if (cnt && tbl == 0) return false;")
            out.append(f"{IND}{IND}{IND}for (std::uint32_t i = 0; i < cnt; ++i)")
            out.append(f"{IND}{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}{IND}const std::uint32_t n = "
                f"static_cast<std::uint32_t>(m.{nm}[i].size());"
            )
            out.append(f"{IND}{IND}{IND}{IND}const std::uint32_t off = n ? w.reserve(n) : 0;")
            out.append(f"{IND}{IND}{IND}{IND}if (n && off == 0) return false;")
            out.append(
                f"{IND}{IND}{IND}{IND}if (n) std::memcpy(w.at(off), m.{nm}[i].data(), n);"
            )
            out.append(
                f"{IND}{IND}{IND}{IND}/* 每次 reserve 后重新取表地址: 段基址不动, 但这样"
                f"写不依赖那个前提。 */"
            )
            out.append(
                f"{IND}{IND}{IND}{IND}reinterpret_cast<dzflat::VarRef*>(w.at(tbl))[i] = "
                f"dzflat::VarRef{{off, n}};"
            )
            out.append(f"{IND}{IND}{IND}}}")
            out.append(f"{IND}{IND}{IND}root->{nm} = dzflat::VarRef{{tbl, cnt}};")
            out.append(f"{IND}{IND}}}")
        elif k == "nested":
            out.append(f"{IND}{IND}if (!{f.elem_flat}::emit(m.{nm}, &root->{nm}, w)) return false;")
        elif k == "nested_array":
            out.append(f"{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t cnt = "
                f"static_cast<std::uint32_t>(m.{nm}.size());"
            )
            out.append(
                f"{IND}{IND}{IND}const std::uint32_t blk = cnt ? w.reserve("
                f"static_cast<std::uint32_t>(cnt * sizeof({f.elem_root}))) : 0;"
            )
            out.append(f"{IND}{IND}{IND}if (cnt && blk == 0) return false;")
            out.append(
                f"{IND}{IND}{IND}/* 元素 Root 里若存在填充字节, 必须先清零: chunk 是复用的"
            )
            out.append(
                f"{IND}{IND}{IND} * 共享内存, 未写到的字节会把上一条消息的内容泄漏给订阅方。"
            )
            out.append(
                f"{IND}{IND}{IND} * 无填充(kRootTight)时每个字节都会被 emit 写到, 省掉这遍"
            )
            out.append(f"{IND}{IND}{IND} * 大块 memset。 */")
            out.append(f"{IND}{IND}{IND}if constexpr (!{f.elem_flat}::kRootTight)")
            out.append(f"{IND}{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}{IND}if (cnt) std::memset(w.at(blk), 0, "
                f"cnt * sizeof({f.elem_root}));"
            )
            out.append(f"{IND}{IND}{IND}}}")
            out.append(f"{IND}{IND}{IND}for (std::uint32_t i = 0; i < cnt; ++i)")
            out.append(f"{IND}{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}{IND}auto* er = reinterpret_cast<{f.elem_root}*>(w.at(blk)) + i;"
            )
            out.append(
                f"{IND}{IND}{IND}{IND}if (!{f.elem_flat}::emit(m.{nm}[i], er, w)) return false;"
            )
            out.append(f"{IND}{IND}{IND}}}")
            out.append(f"{IND}{IND}{IND}root->{nm} = dzflat::VarRef{{blk, cnt}};")
            out.append(f"{IND}{IND}}}")

    out += [
        "",
        f"{IND}{IND}return w.ok();",
        f"{IND}}}",
        "",
        f"{IND}/* 整段字节数上界(含 SegHeader 与 Root)。 */",
        f"{IND}static std::uint32_t size(const {cls}& m)",
        f"{IND}{{",
        f"{IND}{IND}return dzflat::align_up(",
        f"{IND}{IND}{IND}{IND}static_cast<std::uint32_t>(sizeof(dzflat::SegHeader) + sizeof({root})))",
        f"{IND}{IND}{IND}+ varlen_size(m);",
        f"{IND}}}",
        "",
        f"{IND}/* B 级: 借样时的请求字节数 = SegHeader + Root + 调用方声明的变长预算。",
        f"{IND} * 变长长度在借样时还不知道(负载要就地写), 所以必须由调用方给上界。 */",
        f"{IND}static std::uint32_t loan_size(std::uint32_t varlen_budget)",
        f"{IND}{{",
        f"{IND}{IND}return dzflat::varlen_start(static_cast<std::uint32_t>(sizeof({root})))",
        f"{IND}{IND}{IND}+ varlen_budget;",
        f"{IND}}}",
        "",
        f"{IND}/* B 级: 构造完成后封口 —— 按 Writer 的实际水位写段头。 */",
        f"{IND}static bool finalize(void* seg, dzflat::Writer& w, std::uint32_t msg_id)",
        f"{IND}{{",
        f"{IND}{IND}if (seg == nullptr || !w.ok()) return false;",
        f"{IND}{IND}dzflat::write_header(static_cast<std::uint8_t*>(seg), kSchemaHash,",
        f"{IND}{IND}{IND}{IND}{IND}{IND}{IND}  static_cast<std::uint32_t>(sizeof({root})), w.size(), msg_id);",
        f"{IND}{IND}return true;",
        f"{IND}}}",
        "",
        f"{IND}/* 把 m 整段写进 seg。cap 须 >= size(m)。 */",
        f"{IND}static bool write(const {cls}& m, void* seg, std::uint32_t cap,",
        f"{IND}{IND}{IND}{IND}{IND}  std::uint32_t msg_id = 0)",
        f"{IND}{{",
        f"{IND}{IND}auto* p = static_cast<std::uint8_t*>(seg);",
        f"{IND}{IND}const std::uint32_t root_off = sizeof(dzflat::SegHeader);",
        f"{IND}{IND}const std::uint32_t varlen_off = dzflat::align_up(",
        f"{IND}{IND}{IND}{IND}static_cast<std::uint32_t>(root_off + sizeof({root})));",
        f"{IND}{IND}if (p == nullptr || cap < varlen_off) return false;",
        f"{IND}{IND}/* Root 区一次清零: 覆盖成员间与尾部的填充字节, 不泄漏 chunk 里的旧内容。 */",
        f"{IND}{IND}std::memset(p + root_off, 0, varlen_off - root_off);",
        f"{IND}{IND}dzflat::Writer w{{p, cap, varlen_off}};",
        f"{IND}{IND}auto* root = reinterpret_cast<{root}*>(p + root_off);",
        f"{IND}{IND}if (!emit(m, root, w)) return false;",
        f"{IND}{IND}dzflat::write_header(p, kSchemaHash, "
        f"static_cast<std::uint32_t>(sizeof({root})), w.size(), msg_id);",
        f"{IND}{IND}return true;",
        f"{IND}}}",
        "};",
        "",
    ]
    return out


def _gen_view(cls: str, fs: List[_Field]) -> List[str]:
    root = f"{cls}Root"
    out = [
        f"/* {cls} 的只读视图: 直接在段上寻址, 没有反序列化步骤。",
        f" * 段由别的进程写入共享内存, 所有 VarRef 在解引用前都做边界校验, 越界返回空。 */",
        f"class {cls}View",
        "{",
        "public:",
        f"{IND}{cls}View() = default;",
        f"{IND}{cls}View(const dzflat::Reader& rd, const {root}* r) : rd_(rd), r_(r) {{}}",
        "",
        f"{IND}/* 顶层入口: 校验 magic / 版本 / schema_hash / Root 是否落在段内。 */",
        f"{IND}static {cls}View bind(const void* seg, std::size_t size)",
        f"{IND}{{",
        f"{IND}{IND}dzflat::Reader rd = dzflat::bind_segment(",
        f"{IND}{IND}{IND}{IND}seg, size, {cls}Flat::kSchemaHash, "
        f"static_cast<std::uint32_t>(sizeof({root})));",
        f"{IND}{IND}if (!rd.valid()) return {{}};",
        f"{IND}{IND}return {cls}View(rd, reinterpret_cast<const {root}*>(",
        f"{IND}{IND}{IND}{IND}rd.base() + sizeof(dzflat::SegHeader)));",
        f"{IND}}}",
        "",
        f"{IND}bool valid() const noexcept {{ return r_ != nullptr && rd_.valid(); }}",
        f"{IND}const {root}* root() const noexcept {{ return r_; }}",
        "",
    ]

    for f in fs:
        k = f.kind
        nm = f.name
        if k == "scalar":
            if f.base == "bool":
                out.append(f"{IND}bool {nm}() const noexcept {{ return r_->{nm} != 0; }}")
            else:
                out.append(f"{IND}{f.wire} {nm}() const noexcept {{ return r_->{nm}; }}")
        elif k == "fixed_scalar_array":
            out.append(
                f"{IND}dzflat::span<const {f.wire}> {nm}() const noexcept "
                f"{{ return {{r_->{nm}, {f.count}}}; }}"
            )
        elif k == "string":
            out.append(
                f"{IND}std::string_view {nm}() const noexcept "
                f"{{ return rd_.as_string(r_->{nm}); }}"
            )
        elif k == "var_scalar_array":
            out.append(
                f"{IND}dzflat::span<const {f.wire}> {nm}() const noexcept "
                f"{{ return rd_.as_span<{f.wire}>(r_->{nm}); }}"
            )
        elif k == "string_array":
            out.append(
                f"{IND}/* 计数经过边界校验后才返回: cnt 来自段内(别的进程写的), 不能直接信。"
            )
            out.append(
                f"{IND} * 未校验就拿去 resize 会让被篡改的 cnt 变成无界分配。 */"
            )
            out.append(
                f"{IND}std::uint32_t {nm}_count() const noexcept "
                f"{{ return rd_.as_span<dzflat::VarRef>(r_->{nm}).size(); }}"
            )
            out.append(f"{IND}std::string_view {nm}(std::uint32_t i) const noexcept")
            out.append(f"{IND}{{")
            out.append(
                f"{IND}{IND}const auto* e = rd_.elem_at<dzflat::VarRef>(r_->{nm}, i);"
            )
            out.append(f"{IND}{IND}return e ? rd_.as_string(*e) : std::string_view{{}};")
            out.append(f"{IND}}}")
        elif k == "nested":
            out.append(
                f"{IND}{f.elem_view} {nm}() const noexcept "
                f"{{ return {f.elem_view}(rd_, &r_->{nm}); }}"
            )
        elif k == "nested_array":
            out.append(
                f"{IND}/* 同 string[]: 计数必须先过边界校验, 否则篡改的 cnt 会驱动无界分配。 */"
            )
            out.append(
                f"{IND}std::uint32_t {nm}_count() const noexcept "
                f"{{ return rd_.as_span<{f.elem_root}>(r_->{nm}).size(); }}"
            )
            out.append(f"{IND}{f.elem_view} {nm}(std::uint32_t i) const noexcept")
            out.append(f"{IND}{{")
            out.append(f"{IND}{IND}const auto* e = rd_.elem_at<{f.elem_root}>(r_->{nm}, i);")
            out.append(f"{IND}{IND}return e ? {f.elem_view}(rd_, e) : {f.elem_view}{{}};")
            out.append(f"{IND}}}")
            out.append(
                f"{IND}/* Tier-0 元素(无变长字段)时这就是一段连续的 C 数组, 零拷贝整体可用。 */"
            )
            out.append(
                f"{IND}dzflat::span<const {f.elem_root}> {nm}_raw() const noexcept "
                f"{{ return rd_.as_span<{f.elem_root}>(r_->{nm}); }}"
            )

    # ---- copy_to: 兼容桥, 让既有的 owning struct 代码一行接上 ----
    out += [
        "",
        f"{IND}/* 兼容桥: 拷回既有的 owning struct。零拷贝读用不到它, 老代码用它接。 */",
        f"{IND}void copy_to({cls}& out) const",
        f"{IND}{{",
        f"{IND}{IND}(void)out;",
        f"{IND}{IND}if (!valid()) return;",
    ]
    for f in fs:
        k = f.kind
        nm = f.name
        if k == "scalar":
            if f.base == "bool":
                out.append(f"{IND}{IND}out.{nm} = ({nm}() != 0);")
            else:
                out.append(f"{IND}{IND}out.{nm} = {nm}();")
        elif k == "fixed_scalar_array":
            out.append(f"{IND}{IND}for (std::uint32_t i = 0; i < {f.count}; ++i)")
            out.append(f"{IND}{IND}{IND}out.{nm}[i] = r_->{nm}[i];")
        elif k == "string":
            out.append(f"{IND}{IND}{{ auto v = {nm}(); out.{nm}.assign(v.data(), v.size()); }}")
        elif k == "var_scalar_array":
            out.append(f"{IND}{IND}{{")
            out.append(f"{IND}{IND}{IND}auto v = {nm}();")
            out.append(f"{IND}{IND}{IND}out.{nm}.assign(v.begin(), v.end());")
            out.append(f"{IND}{IND}}}")
        elif k == "string_array":
            out.append(f"{IND}{IND}{{")
            out.append(f"{IND}{IND}{IND}std::uint32_t n = {nm}_count();")
            if f.is_fixed:
                out.append(
                    f"{IND}{IND}{IND}/* 定长容器是 std::array<>, 没有 resize; 段里元素多了就截断。 */"
                )
                out.append(f"{IND}{IND}{IND}if (n > {f.count}) n = {f.count};")
            else:
                out.append(f"{IND}{IND}{IND}out.{nm}.resize(n);")
            out.append(f"{IND}{IND}{IND}for (std::uint32_t i = 0; i < n; ++i)")
            out.append(f"{IND}{IND}{IND}{{")
            out.append(f"{IND}{IND}{IND}{IND}auto v = {nm}(i);")
            out.append(f"{IND}{IND}{IND}{IND}out.{nm}[i].assign(v.data(), v.size());")
            out.append(f"{IND}{IND}{IND}}}")
            out.append(f"{IND}{IND}}}")
        elif k == "nested":
            out.append(f"{IND}{IND}{nm}().copy_to(out.{nm});")
        elif k == "nested_array":
            out.append(f"{IND}{IND}{{")
            out.append(f"{IND}{IND}{IND}std::uint32_t n = {nm}_count();")
            if f.is_fixed:
                out.append(
                    f"{IND}{IND}{IND}/* 定长容器是 std::array<>, 没有 resize; 段里元素多了就截断。 */"
                )
                out.append(f"{IND}{IND}{IND}if (n > {f.count}) n = {f.count};")
            else:
                out.append(f"{IND}{IND}{IND}out.{nm}.resize(n);")
            out.append(f"{IND}{IND}{IND}for (std::uint32_t i = 0; i < n; ++i)")
            out.append(f"{IND}{IND}{IND}{IND}{nm}(i).copy_to(out.{nm}[i]);")
            out.append(f"{IND}{IND}}}")
    out += [
        f"{IND}}}",
        "",
        "private:",
        f"{IND}dzflat::Reader rd_;",
        f"{IND}const {root}* r_ = nullptr;",
        "};",
        "",
    ]
    return out


def _gen_builder(cls: str, fs: List[_Field]) -> List[str]:
    """B 级写入器: 直接往 chunk 里构造消息, 发布端零拷贝。

    与 A 级(XxxFlat::write, 从 owning struct 拷进 chunk)的区别在于**大负载不再经过
    调用方的堆**: alloc_<field>(n) 返回的 span 指向共享内存本身, 相机/雷达可以直接
    往里写(DMA / read_into), 一次拷贝都没有。

    小字段(标量/定长数组)本来就落在 Root 里, 而 Root 已经在 chunk 中, 所以 set_ 就是
    原地写。string / string[] 仍要一次 memcpy —— 它们短, 且源在调用方的 std::string
    里, 这一跳无法消除(与 §6.4 对严格零拷贝的界定一致)。

    整棵 builder 树共用一个 dzflat::Writer(变长区是追加式的), 所以子 builder 只持
    指针, 不持有状态。
    """
    root = f"{cls}Root"
    out = [
        f"/* {cls} 的就地构造器(B 级): 直接在共享 chunk 上写, 大负载零拷贝。",
        f" * 由 dzIPC::LoanedMessage<{cls}Flat> 创建, 不要手工构造。 */",
        f"class {cls}Builder",
        "{",
        "public:",
        f"{IND}{cls}Builder() = default;",
        f"{IND}{cls}Builder({root}* r, dzflat::Writer* w) : r_(r), w_(w) {{}}",
        "",
        f"{IND}bool valid() const noexcept {{ return r_ != nullptr && w_ != nullptr; }}",
        f"{IND}/* 容量耗尽后为假: alloc_ 会返回空 span, 且 finalize 会失败。 */",
        f"{IND}bool ok() const noexcept {{ return valid() && w_->ok(); }}",
        f"{IND}/* 逃生口: 直接拿 wire 记录写标量, 需自行处理 bool→uint8 之类的映射。 */",
        f"{IND}{root}* root() const noexcept {{ return r_; }}",
        "",
    ]

    for f in fs:
        k = f.kind
        nm = f.name
        if k == "scalar":
            if f.base == "bool":
                out.append(f"{IND}void set_{nm}(bool v) noexcept {{ r_->{nm} = v ? 1u : 0u; }}")
            else:
                out.append(
                    f"{IND}void set_{nm}({f.wire} v) noexcept {{ r_->{nm} = v; }}"
                )
        elif k == "fixed_scalar_array":
            out.append(
                f"{IND}/* 定长数组在 Root 内, 返回可写 span 供原地填充。 */"
            )
            out.append(
                f"{IND}dzflat::span<{f.wire}> {nm}() const noexcept "
                f"{{ return {{r_->{nm}, {f.count}}}; }}"
            )
        elif k == "string":
            out.append(f"{IND}bool set_{nm}(std::string_view v)")
            out.append(f"{IND}{{")
            out.append(f"{IND}{IND}return dzflat::put_string(*w_, v, r_->{nm});")
            out.append(f"{IND}}}")
        elif k == "var_scalar_array":
            out.append(
                f"{IND}/* 在 chunk 里占 n 个元素并返回**可写** span —— 调用方直接往共享"
            )
            out.append(f"{IND} * 内存写, 这是 B 级零拷贝的落点。容量不足时返回空 span。 */")
            out.append(f"{IND}dzflat::span<{f.wire}> alloc_{nm}(std::uint32_t n)")
            out.append(f"{IND}{{")
            out.append(
                f"{IND}{IND}return dzflat::alloc_array<{f.wire}>(*w_, n, r_->{nm});"
            )
            out.append(f"{IND}}}")
        elif k == "string_array":
            out.append(f"{IND}/* 先定元素个数, 再逐个 set_{nm}_at(i, sv)。 */")
            out.append(f"{IND}bool alloc_{nm}(std::uint32_t n)")
            out.append(f"{IND}{{")
            out.append(f"{IND}{IND}return dzflat::alloc_string_table(*w_, n, r_->{nm});")
            out.append(f"{IND}}}")
            out.append(f"{IND}bool set_{nm}_at(std::uint32_t i, std::string_view v)")
            out.append(f"{IND}{{")
            out.append(f"{IND}{IND}return dzflat::put_string_at(*w_, r_->{nm}, i, v);")
            out.append(f"{IND}}}")
        elif k == "nested":
            out.append(
                f"{IND}/* 嵌套 Root 内联在本 Root 里, 子 builder 共用同一个 Writer。 */"
            )
            out.append(
                f"{IND}{f.nested_class}Builder {nm}() const noexcept "
                f"{{ return {f.nested_class}Builder(&r_->{nm}, w_); }}"
            )
        elif k == "nested_array":
            elem = f.elem_root
            out.append(
                f"{IND}/* 元素 Root 连续排布。Tier-0 元素(kRootTight 且无变长字段)可以"
            )
            out.append(f"{IND} * 直接用返回的 span 原地写完; 否则再用 {nm}_at(i) 拿子 builder。 */")
            out.append(f"{IND}dzflat::span<{elem}> alloc_{nm}(std::uint32_t n)")
            out.append(f"{IND}{{")
            out.append(
                f"{IND}{IND}auto s = dzflat::alloc_array<{elem}>(*w_, n, r_->{nm});"
            )
            out.append(
                f"{IND}{IND}/* 元素 Root 里若有填充字节, 必须清零: chunk 是复用的共享内存,"
            )
            out.append(f"{IND}{IND} * 未写到的字节会把上一条消息泄漏给订阅方。 */")
            out.append(f"{IND}{IND}if constexpr (!{f.elem_flat}::kRootTight)")
            out.append(f"{IND}{IND}{{")
            out.append(
                f"{IND}{IND}{IND}if (!s.empty()) std::memset(s.data(), 0, s.size() * sizeof({elem}));"
            )
            out.append(f"{IND}{IND}}}")
            out.append(f"{IND}{IND}return s;")
            out.append(f"{IND}}}")
            out.append(f"{IND}{f.nested_class}Builder {nm}_at(std::uint32_t i) const noexcept")
            out.append(f"{IND}{{")
            out.append(f"{IND}{IND}auto* e = dzflat::elem_ptr<{elem}>(*w_, r_->{nm}, i);")
            out.append(
                f"{IND}{IND}return e ? {f.nested_class}Builder(e, w_) : {f.nested_class}Builder{{}};"
            )
            out.append(f"{IND}}}")

    out += [
        "",
        "private:",
        f"{IND}{root}* r_ = nullptr;",
        f"{IND}dzflat::Writer* w_ = nullptr;",
        "};",
        "",
    ]
    return out


def _gen_msgbase_impl(cls: str) -> List[str]:
    """IpcMsgBase 的 DZFlat 虚接口实现(类外定义, 因为要用到 XxxFlat / XxxView)。

    这是 dzIPC 发布/订阅路径的多态挂接点: 发布端用 dzflat_size + dzflat_write 把消息
    直接写进借来的 chunk; 订阅端用 dzflat_read 从段里拷回 owning struct。不覆写这四个
    虚函数的类型(手写消息、RTPS 控制帧)自动留在 TLV 路径上。
    """
    return [
        f"inline std::uint32_t {cls}::dzflat_schema_hash() const noexcept",
        "{",
        f"{IND}return {cls}Flat::kSchemaHash;",
        "}",
        "",
        f"inline std::uint32_t {cls}::dzflat_size() const",
        "{",
        f"{IND}return {cls}Flat::size(*this);",
        "}",
        "",
        f"inline bool {cls}::dzflat_write(void* seg, std::uint32_t cap) const",
        "{",
        f"{IND}return {cls}Flat::write(*this, seg, cap, this->dz_ipc_msg_id);",
        "}",
        "",
        f"inline bool {cls}::dzflat_read(const void* seg, std::size_t size)",
        "{",
        f"{IND}/* bind 校验 magic / 版本 / schema_hash / Root 落段内; 段不可信。 */",
        f"{IND}auto v = {cls}View::bind(seg, size);",
        f"{IND}if (!v.valid()) return false;",
        f"{IND}v.copy_to(*this);",
        f"{IND}return true;",
        "}",
        "",
    ]


def generate_dzflat_block(class_name: str, field_infos) -> str:
    """发射 XxxRoot / XxxFlat / XxxView / XxxBuilder + IpcMsgBase 虚接口实现。

    放在消息类定义之后、命名空间关闭之前。
    """
    fs = _fields(field_infos)
    lines: List[str] = [
        "",
        "/* ================================================================== */",
        "/* DZFlat: SHM 专属平坦布局 —— 定长 Root + 段内相对偏移, 读端无反序列化。 */",
        "/* 自动生成, 勿手改。设计与取舍见 docs/dzflat_shm.md                      */",
        "/* ================================================================== */",
        "",
    ]
    lines += _gen_root(class_name, fs)
    lines += _gen_flat(class_name, fs)
    # C++ 发射完毕, LAYOUT_CACHE / HASH_CACHE 已就绪 → 顺带产出 Python schema
    PY_SCHEMA_ENTRIES.append(python_schema_entry(class_name, fs, HASH_CACHE[class_name]))
    lines += _gen_view(class_name, fs)
    lines += _gen_builder(class_name, fs)
    lines += _gen_msgbase_impl(class_name)
    return "\n".join(lines)
