"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage


class TrainIterInfo:
    """Python 侧 TrainIterInfo 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "iter_cnt")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.iter_cnt = 0

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_int32("iter_cnt", self.iter_cnt)
        return g

    @classmethod
    def from_generic(cls, g) -> "TrainIterInfo":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.iter_cnt = g.get_int32("iter_cnt")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"iter_cnt={self.iter_cnt}")
        return f"TrainIterInfo(" + ", ".join(parts) + ")"
