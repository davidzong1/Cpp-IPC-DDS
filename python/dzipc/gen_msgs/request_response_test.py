"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage



class RequestResponseTestRequest:
    """Python 侧 RequestResponseTestRequest 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "request")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.request = []

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_float64_array("request", self.request)
        return g

    @classmethod
    def from_generic(cls, g) -> "RequestResponseTestRequest":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.request = list(g.get_float64_array("request"))
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"request={self.request}")
        return f"RequestResponseTestRequest(" + ", ".join(parts) + ")"


class RequestResponseTestResponse:
    """Python 侧 RequestResponseTestResponse 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "response")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.response = []

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_float64_array("response", self.response)
        return g

    @classmethod
    def from_generic(cls, g) -> "RequestResponseTestResponse":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.response = list(g.get_float64_array("response"))
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"response={self.response}")
        return f"RequestResponseTestResponse(" + ", ".join(parts) + ")"
