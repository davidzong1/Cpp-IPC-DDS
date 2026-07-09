"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage



class TestMsg:
    """Python 侧 TestMsg 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "data1", "data2", "data3", "data4")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.data1 = []
            self.data2 = []
            self.data3 = []
            self.data4 = False

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_float64_array("data1", self.data1)
        g.set_int32_array("data2", self.data2)
        g.set_string_array("data3", self.data3)
        g.set_bool("data4", self.data4)
        return g

    @classmethod
    def from_generic(cls, g) -> "TestMsg":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.data1 = list(g.get_float64_array("data1"))
        obj.data2 = list(g.get_int32_array("data2"))
        obj.data3 = list(g.get_string_array("data3"))
        obj.data4 = g.get_bool("data4")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"data1={self.data1}")
        parts.append(f"data2={self.data2}")
        parts.append(f"data3={self.data3}")
        parts.append(f"data4={self.data4}")
        return f"TestMsg(" + ", ".join(parts) + ")"
