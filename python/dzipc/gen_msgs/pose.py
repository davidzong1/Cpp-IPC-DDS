"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage



class Pose:
    """Python 侧 Pose 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "x", "y", "z", "frame_id")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.x = 0.0
            self.y = 0.0
            self.z = 0.0
            self.frame_id = ""

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_float64("x", self.x)
        g.set_float64("y", self.y)
        g.set_float64("z", self.z)
        g.set_string("frame_id", self.frame_id)
        return g

    @classmethod
    def from_generic(cls, g) -> "Pose":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.x = g.get_float64("x")
        obj.y = g.get_float64("y")
        obj.z = g.get_float64("z")
        obj.frame_id = g.get_string("frame_id")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"x={self.x}")
        parts.append(f"y={self.y}")
        parts.append(f"z={self.z}")
        parts.append(f"frame_id={self.frame_id}")
        return f"Pose(" + ", ".join(parts) + ")"
