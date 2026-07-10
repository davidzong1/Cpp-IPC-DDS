#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Batch message generator
  - Generate C++ .hpp header files for IPC messages and services
  - Generate Python encapsulated class (dynamically serialized through GenericMessage, no need to recompile C++)
  - Generate Python .pyi type stubs for static type checking
"""

import os
import sys
import glob
import textwrap
import re
from typing import Optional, List, Tuple
from msg_generator import MessageGenerator, NestedTypeInfo
from srv_generator import ServiceGenerator

default_msg_path = "./msg/"
default_output_path = "./include/ipc_msg/"
default_srv_path = "./include/ipc_srv/"
default_srv_input_path = "./srv/"
default_pyi_path = "./python/dzipc.pyi"

# Python wrapper 输出目录
PYTHON_WRAPPER_DIR = "./python/dzipc/gen_msgs"
PYTHON_WRAPPER_INIT = os.path.join(PYTHON_WRAPPER_DIR, "__init__.py")

AUTO_PYI_BEGIN = "# AUTO_GENERATED_MSG_SRV_STUBS_BEGIN"
AUTO_PYI_END = "# AUTO_GENERATED_MSG_SRV_STUBS_END"

# 当 dzipc.pyi 不存在时自动创建的骨架
_PYI_SKELETON = """from __future__ import annotations

from enum import Enum
from typing import Callable, List, Tuple

class IPCType(Enum):
    Shm: int
    Socket: int

IPC_SHM: IPCType
IPC_SOCKET: IPCType

class IpcMsgBase:
    def set_msg_id(self, msg_id: int) -> None: ...
    def total_size(self) -> int: ...
    def total_page_cnt(self) -> int: ...

class GenericMessage(IpcMsgBase):
    def clear(self) -> None: ...
    def field_count(self) -> int: ...
    def field_name(self, i: int) -> str: ...
    def field_type(self, i: int) -> int: ...
    def set_bool(self, name: str, val: bool) -> None: ...
    def set_int32(self, name: str, val: int) -> None: ...
    def set_float64(self, name: str, val: float) -> None: ...
    def set_string(self, name: str, val: str) -> None: ...
    def set_nested(self, name: str, val: GenericMessage) -> None: ...
    def set_int32_array(self, name: str, arr: List[int]) -> None: ...
    def set_float64_array(self, name: str, arr: List[float]) -> None: ...
    def set_string_array(self, name: str, arr: List[str]) -> None: ...
    def set_nested_array(self, name: str, arr: List[GenericMessage]) -> None: ...
    def get_bool(self, name: str) -> bool: ...
    def get_int32(self, name: str) -> int: ...
    def get_float64(self, name: str) -> float: ...
    def get_string(self, name: str) -> str: ...
    def get_nested(self, name: str) -> GenericMessage: ...
    def get_int32_array(self, name: str) -> List[int]: ...
    def get_float64_array(self, name: str) -> List[float]: ...
    def get_string_array(self, name: str) -> List[str]: ...
    def get_nested_array(self, name: str) -> List[GenericMessage]: ...
    def serialize(self) -> bytes: ...
    def deserialize(self, data: bytes) -> None: ...
    def serialize_bytes(self) -> bytes: ...
    def deserialize_bytes(self, data: bytes) -> None: ...

class TopicData:
    def __init__(self, topic: IpcMsgBase, msg_id: int = 0) -> None: ...
    def topic(self) -> IpcMsgBase: ...
    def update(self, other: IpcMsgBase) -> None: ...
    def swap(self, other: IpcMsgBase) -> IpcMsgBase: ...

class ServiceData:
    def __init__(
        self, request: IpcMsgBase, response: IpcMsgBase, msg_id: int = 0
    ) -> None: ...
    def request(self) -> IpcMsgBase: ...
    def response(self) -> IpcMsgBase: ...

class ServerIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: ServiceData) -> None: ...
    def reset_callback(self, callback: Callable[[ServiceData], None]) -> None: ...

class ClientIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: ServiceData) -> None: ...
    def send_request(self, request: ServiceData, rev_tm: int = ...) -> bool: ...

class PublisherIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: TopicData) -> None: ...
    def publish(self, msg: IpcMsgBase) -> bool: ...
    def publish_best_effort(self, msg: IpcMsgBase) -> bool: ...
    def publish_blocking(self, msg: IpcMsgBase, tm: int) -> bool: ...
    def publish_for_sniffer(self, msg: IpcMsgBase) -> bool: ...
    def has_subscribed(self) -> bool: ...

class SubscriberIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: TopicData) -> None: ...
    def get(self, msg: TopicData) -> TopicData: ...
    def try_get(self, msg: TopicData) -> Tuple[bool, TopicData]: ...

# AUTO_GENERATED_MSG_SRV_STUBS_BEGIN
{stubs}# AUTO_GENERATED_MSG_SRV_STUBS_END

def make_topic_data(msg: IpcMsgBase, msg_id: int = 0) -> TopicData: ...
def make_service_data(
    request: IpcMsgBase,
    response: IpcMsgBase,
    msg_id: int = 0,
) -> ServiceData: ...
def ServerIPCPtrMake(
    topic_name: str,
    msg: ServiceData,
    callback: Callable[[ServiceData], None],
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> ServerIPC: ...
def ClientIPCPtrMake(
    topic_name: str,
    msg: ServiceData,
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> ClientIPC: ...
def PublisherIPCPtrMake(
    msg: TopicData,
    topic_name: str,
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> PublisherIPC: ...
def SubscriberIPCPtrMake(
    msg: TopicData,
    topic_name: str,
    domain_id: int,
    queue_size: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> SubscriberIPC: ...
def StartShutdownMonitor() -> None: ...
def RequestShutdown() -> None: ...
def IsShutdownRequested() -> bool: ...
"""

PY_TYPE_MAPPING = {
    "bool": "bool",
    "int8": "int",
    "uint8": "int",
    "int16": "int",
    "uint16": "int",
    "int32": "int",
    "uint32": "int",
    "int64": "int",
    "uint64": "int",
    "float32": "float",
    "float64": "float",
    "string": "str",
}

# .msg 类型 → GenericMessage setter/getter 方法名后缀
SCALAR_SETTER_MAP = {
    "bool":     ("set_bool",     "get_bool"),
    "int8":     ("set_int8",     "get_int8"),
    "uint8":    ("set_uint8",    "get_uint8"),
    "int16":    ("set_int16",    "get_int16"),
    "uint16":   ("set_uint16",   "get_uint16"),
    "int32":    ("set_int32",    "get_int32"),
    "uint32":   ("set_uint32",   "get_uint32"),
    "int64":    ("set_int64",    "get_int64"),
    "uint64":   ("set_uint64",   "get_uint64"),
    "float32":  ("set_float32",  "get_float32"),
    "float64":  ("set_float64",  "get_float64"),
    "string":   ("set_string",   "get_string"),
}

ARRAY_SETTER_MAP = {
    "bool":     ("set_bool_array",     "get_bool_array"),
    "int8":     ("set_int8_array",     "get_int8_array"),
    "uint8":    ("set_uint8_array",    "get_uint8_array"),
    "int16":    ("set_int16_array",    "get_int16_array"),
    "uint16":   ("set_uint16_array",   "get_uint16_array"),
    "int32":    ("set_int32_array",    "get_int32_array"),
    "uint32":   ("set_uint32_array",   "get_uint32_array"),
    "int64":    ("set_int64_array",    "get_int64_array"),
    "uint64":   ("set_uint64_array",   "get_uint64_array"),
    "float32":  ("set_float32_array",  "get_float32_array"),
    "float64":  ("set_float64_array",  "get_float64_array"),
    "string":   ("set_string_array",   "get_string_array"),
}

NESTED_TYPE_INDEX = 25
NESTED_ARRAY_TYPE_INDEX = 26


def snake_to_pascal(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_") if part)


def parse_fields(lines: List[str]) -> List[str]:
    fields: List[str] = []
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            fields.append(parts[1])
    return fields


def parse_typed_fields(lines: List[str], msg_type_registry: Optional[dict] = None) -> List[Tuple[str, str, str, bool, str]]:
    """解析字段，返回 (field_name, py_type, field_type, is_array, base_type) 列表"""
    msg_type_registry = msg_type_registry or {}
    fields: List[Tuple[str, str, str, bool, str]] = []
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            field_type = parts[0]
            field_name = parts[1]
            is_array = field_type.endswith("[]")
            base_type = field_type[:-2] if is_array else field_type
            nested_info = msg_type_registry.get(base_type)
            if nested_info:
                py_base = nested_info.class_name
                base_type = nested_info.field_type
            else:
                py_base = PY_TYPE_MAPPING.get(base_type, "object")
            py_type = f"List[{py_base}]" if is_array else py_base
            fields.append((field_name, py_type, field_type, is_array, base_type))
    return fields


def collect_msg_files(msg_input_dir: str) -> List[str]:
    return sorted(glob.glob(os.path.join(msg_input_dir, "**/*.msg"), recursive=True))


def snake_to_pascal_checked(name: str) -> str:
    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise ValueError("Msg file name must start with a lowercase letter and contain only lowercase letters, digits, and underscores")
    return snake_to_pascal(name)


def build_msg_type_registry(msg_files: List[str], msg_root: str) -> dict:
    registry = {}
    for msg_file in msg_files:
        rel = os.path.relpath(msg_file, msg_root).replace("\\", "/")
        stem = os.path.splitext(rel)[0]
        base_name = os.path.splitext(os.path.basename(msg_file))[0]
        class_name = snake_to_pascal_checked(base_name)
        info = NestedTypeInfo(
            field_type=base_name,
            class_name=class_name,
            include_path=f"ipc_msg/{stem}.hpp",
        )
        registry[base_name] = info
        registry[class_name] = info
    return registry


def _msg_file_base_name(msg_file: str) -> str:
    return os.path.splitext(os.path.basename(msg_file))[0]


def parse_msg_dependencies(msg_file: str, msg_type_registry: dict) -> List[str]:
    deps: List[str] = []
    self_type = _msg_file_base_name(msg_file)
    with open(msg_file, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            field_type = parts[0]
            base_type = field_type[:-2] if field_type.endswith("[]") else field_type
            nested_info = msg_type_registry.get(base_type)
            if not nested_info:
                continue
            dep_type = nested_info.field_type
            if dep_type == self_type:
                raise ValueError(f"Recursive nested message is not supported: {self_type}")
            if dep_type not in deps:
                deps.append(dep_type)
    return deps


def sort_msg_files_by_dependencies(msg_files: List[str], msg_type_registry: dict) -> List[str]:
    file_by_type = {_msg_file_base_name(msg_file): msg_file for msg_file in msg_files}
    deps_by_type = {
        msg_type: parse_msg_dependencies(msg_file, msg_type_registry)
        for msg_type, msg_file in file_by_type.items()
    }
    sorted_types: List[str] = []
    visiting = set()
    visited = set()

    def visit(msg_type: str) -> None:
        if msg_type in visited:
            return
        if msg_type in visiting:
            raise ValueError(f"Circular nested message dependency detected at: {msg_type}")
        visiting.add(msg_type)
        for dep in deps_by_type.get(msg_type, []):
            if dep in file_by_type:
                visit(dep)
        visiting.remove(msg_type)
        visited.add(msg_type)
        sorted_types.append(msg_type)

    for msg_type in sorted(file_by_type):
        visit(msg_type)

    return [file_by_type[msg_type] for msg_type in sorted_types]


def parse_raw_srv_fields(srv_file: str):
    with open(srv_file, "r", encoding="utf-8") as f:
        lines = f.readlines()
    req_lines: List[str] = []
    resp_lines: List[str] = []
    in_response = False
    for raw in lines:
        line = raw.strip()
        if line == "---":
            in_response = True
            continue
        if in_response:
            resp_lines.append(raw)
        else:
            req_lines.append(raw)
    return req_lines, resp_lines


def replace_block(content: str, begin: str, end: str, block_body: str) -> str:
    begin_idx = content.find(begin)
    end_idx = content.find(end)
    if begin_idx == -1 or end_idx == -1 or end_idx < begin_idx:
        raise RuntimeError(f"未找到自动生成锚点: {begin} / {end}")
    begin_line_end = content.find("\n", begin_idx)
    if begin_line_end == -1:
        begin_line_end = begin_idx + len(begin)
    return content[: begin_line_end + 1] + block_body + content[end_idx:]


# ========== Python 封装类生成 ==========

PY_WRAPPER_TEMPLATE = '''"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage
{extra_imports}


class {class_name}:
    """Python 侧 {class_name} 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",{slot_fields})

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
{default_inits}

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
{to_generic_calls}
        return g

    @classmethod
    def from_generic(cls, g) -> "{class_name}":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
{from_generic_calls}
        return obj

    def __repr__(self):
        parts = []
{repr_parts}
        return f"{class_name}(" + ", ".join(parts) + ")"
'''

# srv 响应类模板（不含文件头，用于拼接在请求类后面）
PY_WRAPPER_TEMPLATE_NO_HEADER = '''

class {class_name}:
    """Python 侧 {class_name} 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",{slot_fields})

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
{default_inits}

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
{to_generic_calls}
        return g

    @classmethod
    def from_generic(cls, g) -> "{class_name}":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
{from_generic_calls}
        return obj

    def __repr__(self):
        parts = []
{repr_parts}
        return f"{class_name}(" + ", ".join(parts) + ")"
'''


def generate_python_wrapper(class_name: str, fields: List[Tuple[str, str, str, bool, str]],
                             namespace: str = "Msg") -> str:
    """为一个消息/服务类型生成 Python 封装类"""
    return _generate_wrapper(PY_WRAPPER_TEMPLATE, class_name, fields)


def generate_python_wrapper_no_header(class_name: str, fields: List[Tuple[str, str, str, bool, str]]) -> str:
    """生成 srv 响应类的 Python 封装（不含文件头，拼在请求类之后）"""
    return _generate_wrapper(PY_WRAPPER_TEMPLATE_NO_HEADER, class_name, fields)


def _is_nested_base(base_type: str) -> bool:
    return base_type not in PY_TYPE_MAPPING


def _nested_class_name(py_type: str, is_array: bool) -> str:
    if is_array and py_type.startswith("List[") and py_type.endswith("]"):
        return py_type[5:-1]
    return py_type


def _nested_import_lines(class_name: str, fields: List[Tuple[str, str, str, bool, str]]) -> List[str]:
    nested_imports = []
    for _, py_type, _, is_array, base_type in fields:
        if not _is_nested_base(base_type):
            continue
        nested_class = _nested_class_name(py_type, is_array)
        if nested_class == "object" or nested_class == class_name:
            continue
        import_line = f"from .{base_type} import {nested_class}"
        if import_line not in nested_imports:
            nested_imports.append(import_line)
    return nested_imports


def _generate_wrapper(template: str, class_name: str, fields: List[Tuple[str, str, str, bool, str]]) -> str:
    nested_imports = _nested_import_lines(class_name, fields)
    extra_imports = "\n" + "\n".join(nested_imports) if nested_imports else ""

    slot_fields = ", ".join(f'"{f[0]}"' for f in fields)
    if slot_fields:
        slot_fields = "\n" + " " * 17 + slot_fields

    # default_inits: 初始化默认值
    default_lines = []
    for fname, py_type, ftype, is_array, base_type in fields:
        if is_array:
            default_lines.append(f"self.{fname} = []")
        elif _is_nested_base(base_type):
            default_lines.append(f"self.{fname} = {py_type}()")
        elif base_type == "string":
            default_lines.append(f'self.{fname} = ""')
        elif base_type == "bool":
            default_lines.append(f"self.{fname} = False")
        elif base_type in ("float32", "float64"):
            default_lines.append(f"self.{fname} = 0.0")
        else:
            default_lines.append(f"self.{fname} = 0")
    default_inits = "\n".join(default_lines) if default_lines else "pass"

    # to_generic_calls
    to_gen_lines = []
    for fname, _, ftype, is_array, base_type in fields:
        if _is_nested_base(base_type) and is_array:
            to_gen_lines.append(
                f'g.set_nested_array("{fname}", [item.to_generic() if hasattr(item, "to_generic") else item for item in self.{fname}])'
            )
        elif _is_nested_base(base_type):
            to_gen_lines.append(
                f'g.set_nested("{fname}", self.{fname}.to_generic() if hasattr(self.{fname}, "to_generic") else self.{fname})'
            )
        elif is_array:
            setter, _ = ARRAY_SETTER_MAP[base_type]
            to_gen_lines.append(f'g.{setter}("{fname}", self.{fname})')
        else:
            setter, _ = SCALAR_SETTER_MAP[base_type]
            to_gen_lines.append(f'g.{setter}("{fname}", self.{fname})')
    to_generic_calls = "\n".join(to_gen_lines)

    # from_generic_calls
    from_gen_lines = []
    for fname, py_type, ftype, is_array, base_type in fields:
        if _is_nested_base(base_type) and is_array:
            nested_class = _nested_class_name(py_type, is_array)
            from_gen_lines.append(f'obj.{fname} = [{nested_class}.from_generic(item) for item in g.get_nested_array("{fname}")]')
        elif _is_nested_base(base_type):
            from_gen_lines.append(f'obj.{fname} = {py_type}.from_generic(g.get_nested("{fname}"))')
        elif is_array:
            _, getter = ARRAY_SETTER_MAP[base_type]
            from_gen_lines.append(f'obj.{fname} = list(g.{getter}("{fname}"))')
        else:
            _, getter = SCALAR_SETTER_MAP[base_type]
            from_gen_lines.append(f'obj.{fname} = g.{getter}("{fname}")')
    from_generic_calls = "\n".join(from_gen_lines)

    # repr
    repr_lines = []
    for fname, _, _, _, _ in fields:
        repr_lines.append(f'parts.append(f"{fname}={{self.{fname}}}")')
    repr_parts = "\n".join(repr_lines)

    return template.format(
        class_name=class_name,
        extra_imports=extra_imports,
        slot_fields=slot_fields,
        default_inits=textwrap.indent(default_inits, "            "),
        to_generic_calls=textwrap.indent(to_generic_calls, "        "),
        from_generic_calls=textwrap.indent(from_generic_calls, "        "),
        repr_parts=textwrap.indent(repr_parts, "        "),
    )


def generate_registry_init(msg_files: List[str], srv_files: List[str],
                           msg_root: str, srv_root: str) -> str:
    """生成 gen_msgs/__init__.py — 类型注册表"""
    lines = [
        '"""自动生成的类型注册表 — 每次修改 .msg/.srv 后由 batch_msg_srv_generator.py 更新"""',
        "from __future__ import annotations",
        "from typing import List",
        "",
        "# 导入所有封装类",
    ]

    msg_classes = []
    srv_classes = []

    msg_type_registry = build_msg_type_registry(msg_files, msg_root)
    sorted_msg_files = sort_msg_files_by_dependencies(msg_files, msg_type_registry)

    for msg_file in sorted_msg_files:
        base_name = os.path.splitext(os.path.basename(msg_file))[0]
        class_name = snake_to_pascal(base_name)
        module_name = base_name
        lines.append(f"from .{module_name} import {class_name}")
        msg_classes.append((class_name, module_name))

    for srv_file in sorted(srv_files):
        base_name = os.path.splitext(os.path.basename(srv_file))[0]
        py_base = snake_to_pascal(base_name)
        req_cls = f"{py_base}Request"
        resp_cls = f"{py_base}Response"
        module_name = base_name
        lines.append(f"from .{module_name} import {req_cls}, {resp_cls}")
        srv_classes.append((py_base, req_cls, resp_cls, module_name))

    lines.append("")
    lines.append("# 暴露到 dzipc 模块命名空间")
    lines.append("__all__ = [")
    for cn, _ in msg_classes:
        lines.append(f'    "{cn}",')
    for _, req, resp, _ in srv_classes:
        lines.append(f'    "{req}",')
        lines.append(f'    "{resp}",')
    lines.append("]")
    lines.append("")

    # message_types()
    lines.append("")
    lines.append("def message_types() -> List[str]:")
    lines.append("    return [")
    for cn, _ in msg_classes:
        lines.append(f'        "{cn}",')
    lines.append("    ]")
    lines.append("")

    # create_message()
    lines.append("")
    lines.append("def create_message(type_name: str):")
    lines.append('    """创建消息实例并返回其 GenericMessage（已序列化字段）"""')
    lines.append("    from dzipc._dzipc_core import GenericMessage  # noqa: F401")
    for cn, mn in msg_classes:
        lines.append(f'    if type_name == "{cn}":')
        lines.append(f"        return {cn}().to_generic()")
    lines.append('    raise ValueError("Unknown message type: " + type_name)')
    lines.append("")

    # service_types()
    lines.append("")
    lines.append("def service_types() -> List[str]:")
    lines.append("    return [")
    for py_base, _, _, _ in srv_classes:
        lines.append(f'        "{py_base}",')
    lines.append("    ]")
    lines.append("")

    # create_service_request()
    lines.append("")
    lines.append("def create_service_request(service_name: str):")
    lines.append('    """创建服务请求实例并返回其 GenericMessage"""')
    for py_base, req_cls, _, _ in srv_classes:
        lines.append(f'    if service_name == "{py_base}":')
        lines.append(f"        return {req_cls}().to_generic()")
    lines.append('    raise ValueError("Unknown service type: " + service_name)')
    lines.append("")

    # create_service_response()
    lines.append("")
    lines.append("def create_service_response(service_name: str):")
    lines.append('    """创建服务响应实例并返回其 GenericMessage"""')
    for py_base, _, resp_cls, _ in srv_classes:
        lines.append(f'    if service_name == "{py_base}":')
        lines.append(f"        return {resp_cls}().to_generic()")
    lines.append('    raise ValueError("Unknown service type: " + service_name)')
    lines.append("")

    return "\n".join(lines)


# ========== Python stub (.pyi) 生成 ==========

def generate_pyi_blocks(msg_files: List[str], srv_files: List[str], msg_root: str) -> str:
    stub_lines: List[str] = []
    msg_type_registry = build_msg_type_registry(msg_files, msg_root)
    sorted_msg_files = sort_msg_files_by_dependencies(msg_files, msg_type_registry)

    for msg_file in sorted_msg_files:
        class_name = os.path.splitext(os.path.basename(msg_file))[0]
        py_name = snake_to_pascal(class_name)
        with open(msg_file, "r", encoding="utf-8") as f:
            typed_fields = parse_typed_fields(f.readlines(), msg_type_registry)
        stub_lines.append(f"class {py_name}(IpcMsgBase):")
        if typed_fields:
            for fname, ftype, _, _, _ in typed_fields:
                stub_lines.append(f"    {fname}: {ftype}")
        else:
            stub_lines.append("    pass")
        stub_lines.append("")

    for srv_file in sorted(srv_files):
        base_name = os.path.splitext(os.path.basename(srv_file))[0]
        py_base = snake_to_pascal(base_name)
        req_lines_raw, resp_lines_raw = parse_raw_srv_fields(srv_file)
        req_typed = parse_typed_fields(req_lines_raw, msg_type_registry)
        resp_typed = parse_typed_fields(resp_lines_raw, msg_type_registry)

        stub_lines.append(f"class {py_base}Request(IpcMsgBase):")
        if req_typed:
            for fname, ftype, _, _, _ in req_typed:
                stub_lines.append(f"    {fname}: {ftype}")
        else:
            stub_lines.append("    pass")
        stub_lines.append("")

        stub_lines.append(f"class {py_base}Response(IpcMsgBase):")
        if resp_typed:
            for fname, ftype, _, _, _ in resp_typed:
                stub_lines.append(f"    {fname}: {ftype}")
        else:
            stub_lines.append("    pass")
        stub_lines.append("")

    if msg_files:
        stub_lines.append("def message_types() -> List[str]: ...")
        stub_lines.append("def create_message(type_name: str) -> IpcMsgBase: ...")
        stub_lines.append("")

    if srv_files:
        stub_lines.append("def service_types() -> List[str]: ...")
        stub_lines.append("def create_service_request(service_name: str) -> IpcMsgBase: ...")
        stub_lines.append("def create_service_response(service_name: str) -> IpcMsgBase: ...")
        stub_lines.append("")

    stub_block = "\n".join(stub_lines)
    if stub_block:
        stub_block += "\n"
    return stub_block


# ========== 主流程 ==========

def process_msg_directory(input_dir: str, output_dir: Optional[str] = None) -> None:
    if output_dir is None:
        output_dir = default_output_path
    os.makedirs(output_dir, exist_ok=True)

    for root, dirs, files in os.walk(output_dir):
        if 'ipc_msg_base' in dirs:
            dirs.remove('ipc_msg_base')
        for file in files:
            try:
                os.remove(os.path.join(root, file))
            except OSError as e:
                print(f"警告: 无法删除文件 {os.path.join(root, file)}: {e}")

    for root, dirs, files in os.walk(output_dir, topdown=False):
        if root == output_dir or 'ipc_msg_base' in root:
            continue
        try:
            os.rmdir(root)
        except OSError:
            pass

    msg_files = collect_msg_files(input_dir)
    if not msg_files:
        print(f"在目录 {input_dir} 中未找到.msg文件")
        return

    success_count = 0
    msg_type_registry = build_msg_type_registry(msg_files, input_dir)
    sorted_msg_files = sort_msg_files_by_dependencies(msg_files, msg_type_registry)
    for msg_file in sorted_msg_files:
        try:
            generator = MessageGenerator(nested_types=msg_type_registry)
            rel_path = os.path.relpath(msg_file, input_dir)
            rel_dir = os.path.dirname(rel_path)
            target_output_dir = os.path.join(output_dir, rel_dir) if rel_dir else output_dir
            os.makedirs(target_output_dir, exist_ok=True)
            generator.process_msg_file(msg_file, target_output_dir)
            success_count += 1
        except Exception as e:
            print(f"  错误: {e}")


def process_srv_directory(
    input_dir: str,
    output_dir: Optional[str] = None,
    msg_type_registry: Optional[dict] = None,
) -> None:
    if output_dir is None:
        output_dir = default_srv_path
    os.makedirs(output_dir, exist_ok=True)

    for root, dirs, files in os.walk(output_dir):
        if 'IpcMsgBase' in dirs:
            dirs.remove('IpcMsgBase')
        for file in files:
            try:
                os.remove(os.path.join(root, file))
            except OSError as e:
                print(f"警告: 无法删除文件 {os.path.join(root, file)}: {e}")

    for root, dirs, files in os.walk(output_dir, topdown=False):
        if root == output_dir or 'IpcMsgBase' in root:
            continue
        try:
            os.rmdir(root)
        except OSError:
            pass

    msg_files = glob.glob(os.path.join(input_dir, "**/*.srv"), recursive=True)
    if not msg_files:
        print(f"在目录 {input_dir} 中未找到.srv文件")
        return

    for srv_file in msg_files:
        try:
            generator = ServiceGenerator(nested_types=msg_type_registry or {})
            rel_path = os.path.relpath(srv_file, input_dir)
            rel_dir = os.path.dirname(rel_path)
            target_output_dir = os.path.join(output_dir, rel_dir) if rel_dir else output_dir
            os.makedirs(target_output_dir, exist_ok=True)
            generator.process_srv_file(srv_file, target_output_dir)
        except Exception as e:
            print(f"  错误: {e}")


def generate_python_wrappers(msg_input_dir: str, srv_input_dir: str) -> None:
    """为所有 msg/srv 生成 Python 封装类到 python/dzipc/gen_msgs/"""
    os.makedirs(PYTHON_WRAPPER_DIR, exist_ok=True)

    msg_files = collect_msg_files(msg_input_dir)
    srv_files = sorted(glob.glob(os.path.join(srv_input_dir, "**/*.srv"), recursive=True))
    msg_type_registry = build_msg_type_registry(msg_files, msg_input_dir)
    sorted_msg_files = sort_msg_files_by_dependencies(msg_files, msg_type_registry)

    # 生成每个 msg 的 Python 封装
    for msg_file in sorted_msg_files:
        base_name = os.path.splitext(os.path.basename(msg_file))[0]
        class_name = snake_to_pascal(base_name)
        typed_fields = parse_typed_fields(
            open(msg_file, "r", encoding="utf-8").readlines(),
            msg_type_registry)

        wrapper_code = generate_python_wrapper(class_name, typed_fields)
        output_path = os.path.join(PYTHON_WRAPPER_DIR, f"{base_name}.py")
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(wrapper_code)
        # print(f"  Generate Python wrapper: {output_path}")

    # 生成每个 srv 的 Python 封装
    for srv_file in srv_files:
        base_name = os.path.splitext(os.path.basename(srv_file))[0]
        py_base = snake_to_pascal(base_name)
        req_lines, resp_lines = parse_raw_srv_fields(srv_file)
        req_typed = parse_typed_fields(req_lines, msg_type_registry)
        resp_typed = parse_typed_fields(resp_lines, msg_type_registry)

        # Request
        req_code = generate_python_wrapper(f"{py_base}Request", req_typed)
        # Response 使用无头模板（拼在 Request 后面，共用文件头和 imports）
        resp_code = generate_python_wrapper_no_header(f"{py_base}Response", resp_typed)

        full_code = (
            req_code.rstrip()
            + "\n"
            + resp_code
        )
        missing_imports = [
            line for line in _nested_import_lines(f"{py_base}Response", resp_typed)
            if line not in req_code
        ]
        if missing_imports:
            full_code = full_code.replace(
                "from dzipc._dzipc_core import GenericMessage\n",
                "from dzipc._dzipc_core import GenericMessage\n" + "\n".join(missing_imports) + "\n",
                1,
            )
        output_path = os.path.join(PYTHON_WRAPPER_DIR, f"{base_name}.py")
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(full_code)
        # print(f"  Generate Python wrapper: {output_path}")

    # 生成注册表 __init__.py
    init_code = generate_registry_init(msg_files, srv_files, msg_input_dir, srv_input_dir)
    with open(PYTHON_WRAPPER_INIT, "w", encoding="utf-8") as f:
        f.write(init_code)
    # print(f"  Generate registry: {PYTHON_WRAPPER_INIT}")


def generate_python_stub_bindings(msg_input_dir: str, srv_input_dir: str,
                                  pyi_path: str = default_pyi_path) -> None:
    msg_files = collect_msg_files(msg_input_dir)
    srv_files = glob.glob(os.path.join(srv_input_dir, "**/*.srv"), recursive=True)

    stub_block = generate_pyi_blocks(msg_files, srv_files, msg_input_dir)

    if not os.path.exists(pyi_path):
        # 文件不存在时自动创建（包含基础设施存根 + 锚点）
        skeleton = _PYI_SKELETON.format(stubs=stub_block)
        os.makedirs(os.path.dirname(pyi_path) or ".", exist_ok=True)
        with open(pyi_path, "w", encoding="utf-8") as f:
            f.write(skeleton)
        print(f"  Create Python stub: {pyi_path}")
        return

    with open(pyi_path, "r", encoding="utf-8") as f:
        content = f.read()

    content = replace_block(content, AUTO_PYI_BEGIN, AUTO_PYI_END, stub_block)

    with open(pyi_path, "w", encoding="utf-8") as f:
        f.write(content)


def get_msg_directories(input_dir: str) -> List[str]:
    msg_directories = set()
    for root, dirs, files in os.walk(input_dir):
        for file in files:
            if file.endswith('.msg'):
                msg_directories.add(root)
                break
    return sorted(list(msg_directories))


def get_srv_directories(input_dir: str) -> List[str]:
    srv_directories = set()
    for root, dirs, files in os.walk(input_dir):
        for file in files:
            if file.endswith('.srv'):
                srv_directories.add(root)
                break
    return sorted(list(srv_directories))


def main():
    msg_input_dir = default_msg_path
    srv_input_dir = default_srv_input_path

    if not os.path.exists(msg_input_dir):
        print(f"错误: 输入目录 {msg_input_dir} 不存在")
        sys.exit(1)

    # 1. 生成 C++ .hpp 头文件（供 C++ 客户端使用）
    try:
        process_msg_directory(msg_input_dir)
    except Exception as e:
        print(f"处理 .msg 失败: {e}")
        sys.exit(1)

    try:
        msg_files = collect_msg_files(msg_input_dir)
        msg_type_registry = build_msg_type_registry(msg_files, msg_input_dir)
        process_srv_directory(srv_input_dir, msg_type_registry=msg_type_registry)
    except Exception as e:
        print(f"处理 .srv 失败: {e}")
        sys.exit(1)

    # 2. 生成 Python 封装类（通过 GenericMessage，无需重编 C++）
    try:
        generate_python_wrappers(msg_input_dir, srv_input_dir)
    except Exception as e:
        print(f"生成 Python 封装失败: {e}")
        sys.exit(1)

    # 3. 生成 Python .pyi 类型存根
    try:
        generate_python_stub_bindings(msg_input_dir, srv_input_dir)
    except Exception as e:
        print(f"更新 Python stub 失败: {e}")
        sys.exit(1)

    # 4. 更新 dzipc 主 __init__.py 的自动生成段（注册表导入）
    try:
        update_main_init()
    except Exception as e:
        print(f"更新 dzipc/__init__.py 失败: {e}")


def update_main_init():
    """更新 python/dzipc/__init__.py 的自动生成段"""
    main_init_path = "./python/dzipc/__init__.py"
    if not os.path.exists(main_init_path):
        return

    # 收集 gen_msgs 所有类
    gen_dir = PYTHON_WRAPPER_DIR
    if not os.path.exists(gen_dir):
        return

    msg_files = sorted(glob.glob(os.path.join(default_msg_path, "**/*.msg"), recursive=True))
    srv_files = sorted(glob.glob(os.path.join(default_srv_input_path, "**/*.srv"), recursive=True))

    import_lines = ["from .gen_msgs import ("]
    for mf in msg_files:
        cn = snake_to_pascal(os.path.splitext(os.path.basename(mf))[0])
        import_lines.append(f"    {cn},")
    for sf in srv_files:
        py_base = snake_to_pascal(os.path.splitext(os.path.basename(sf))[0])
        import_lines.append(f"    {py_base}Request,")
        import_lines.append(f"    {py_base}Response,")
    import_lines.append(")")

    import_block = "\n".join(import_lines) + "\n"

    # function redirects
    func_lines = [
        "from .gen_msgs import (message_types, service_types,",
        "    create_message, create_service_request, create_service_response)",
    ]
    func_block = "\n".join(func_lines) + "\n"

    begin_marker = "# AUTO_GENERATED_MSG_SRV_IMPORTS_BEGIN"
    end_marker = "# AUTO_GENERATED_MSG_SRV_IMPORTS_END"

    with open(main_init_path, "r", encoding="utf-8") as f:
        content = f.read()

    if begin_marker in content and end_marker in content:
        content = replace_block(content, begin_marker, end_marker, import_block)
    else:
        # 没有锚点则追加到文件末尾
        if not content.endswith("\n"):
            content += "\n"
        content += f"{begin_marker}\n{import_block}{end_marker}\n"

    # function block
    func_begin = "# AUTO_GENERATED_MSG_SRV_FUNCS_BEGIN"
    func_end = "# AUTO_GENERATED_MSG_SRV_FUNCS_END"

    if func_begin in content and func_end in content:
        content = replace_block(content, func_begin, func_end, func_block)
    else:
        if not content.endswith("\n"):
            content += "\n"
        content += f"{func_begin}\n{func_block}{func_end}\n"

    with open(main_init_path, "w", encoding="utf-8") as f:
        f.write(content)

    # print(f"  Update host __init__.py: {main_init_path}")


if __name__ == "__main__":
    main()
