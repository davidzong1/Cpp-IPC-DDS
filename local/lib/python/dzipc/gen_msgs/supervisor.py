"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage


class Supervisor:
    """Python 侧 Supervisor 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "update_time", "additional_info")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.update_time = ""
            self.additional_info = ""

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_string("update_time", self.update_time)
        g.set_string("additional_info", self.additional_info)
        return g

    @classmethod
    def from_generic(cls, g) -> "Supervisor":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.update_time = g.get_string("update_time")
        obj.additional_info = g.get_string("additional_info")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"update_time={self.update_time}")
        parts.append(f"additional_info={self.additional_info}")
        return f"Supervisor(" + ", ".join(parts) + ")"
