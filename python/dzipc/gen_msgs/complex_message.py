"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage



class ComplexMessage:
    """Python 侧 ComplexMessage 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "status", "tiny_int", "tiny_uint", "small_int", "small_uint", "normal_int", "normal_uint", "big_int", "big_uint", "single_precision", "double_precision", "message", "status_array", "tiny_int_array", "tiny_uint_array", "small_int_array", "small_uint_array", "normal_int_array", "normal_uint_array", "big_int_array", "big_uint_array", "single_precision_array", "double_precision_array", "message_array")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.status = False
            self.tiny_int = 0
            self.tiny_uint = 0
            self.small_int = 0
            self.small_uint = 0
            self.normal_int = 0
            self.normal_uint = 0
            self.big_int = 0
            self.big_uint = 0
            self.single_precision = 0.0
            self.double_precision = 0.0
            self.message = ""
            self.status_array = []
            self.tiny_int_array = []
            self.tiny_uint_array = []
            self.small_int_array = []
            self.small_uint_array = []
            self.normal_int_array = []
            self.normal_uint_array = []
            self.big_int_array = []
            self.big_uint_array = []
            self.single_precision_array = []
            self.double_precision_array = []
            self.message_array = []

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_bool("status", self.status)
        g.set_int8("tiny_int", self.tiny_int)
        g.set_uint8("tiny_uint", self.tiny_uint)
        g.set_int16("small_int", self.small_int)
        g.set_uint16("small_uint", self.small_uint)
        g.set_int32("normal_int", self.normal_int)
        g.set_uint32("normal_uint", self.normal_uint)
        g.set_int64("big_int", self.big_int)
        g.set_uint64("big_uint", self.big_uint)
        g.set_float32("single_precision", self.single_precision)
        g.set_float64("double_precision", self.double_precision)
        g.set_string("message", self.message)
        g.set_bool_array("status_array", self.status_array)
        g.set_int8_array("tiny_int_array", self.tiny_int_array)
        g.set_uint8_array("tiny_uint_array", self.tiny_uint_array)
        g.set_int16_array("small_int_array", self.small_int_array)
        g.set_uint16_array("small_uint_array", self.small_uint_array)
        g.set_int32_array("normal_int_array", self.normal_int_array)
        g.set_uint32_array("normal_uint_array", self.normal_uint_array)
        g.set_int64_array("big_int_array", self.big_int_array)
        g.set_uint64_array("big_uint_array", self.big_uint_array)
        g.set_float32_array("single_precision_array", self.single_precision_array)
        g.set_float64_array("double_precision_array", self.double_precision_array)
        g.set_string_array("message_array", self.message_array)
        return g

    @classmethod
    def from_generic(cls, g) -> "ComplexMessage":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.status = g.get_bool("status")
        obj.tiny_int = g.get_int8("tiny_int")
        obj.tiny_uint = g.get_uint8("tiny_uint")
        obj.small_int = g.get_int16("small_int")
        obj.small_uint = g.get_uint16("small_uint")
        obj.normal_int = g.get_int32("normal_int")
        obj.normal_uint = g.get_uint32("normal_uint")
        obj.big_int = g.get_int64("big_int")
        obj.big_uint = g.get_uint64("big_uint")
        obj.single_precision = g.get_float32("single_precision")
        obj.double_precision = g.get_float64("double_precision")
        obj.message = g.get_string("message")
        obj.status_array = list(g.get_bool_array("status_array"))
        obj.tiny_int_array = list(g.get_int8_array("tiny_int_array"))
        obj.tiny_uint_array = list(g.get_uint8_array("tiny_uint_array"))
        obj.small_int_array = list(g.get_int16_array("small_int_array"))
        obj.small_uint_array = list(g.get_uint16_array("small_uint_array"))
        obj.normal_int_array = list(g.get_int32_array("normal_int_array"))
        obj.normal_uint_array = list(g.get_uint32_array("normal_uint_array"))
        obj.big_int_array = list(g.get_int64_array("big_int_array"))
        obj.big_uint_array = list(g.get_uint64_array("big_uint_array"))
        obj.single_precision_array = list(g.get_float32_array("single_precision_array"))
        obj.double_precision_array = list(g.get_float64_array("double_precision_array"))
        obj.message_array = list(g.get_string_array("message_array"))
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"status={self.status}")
        parts.append(f"tiny_int={self.tiny_int}")
        parts.append(f"tiny_uint={self.tiny_uint}")
        parts.append(f"small_int={self.small_int}")
        parts.append(f"small_uint={self.small_uint}")
        parts.append(f"normal_int={self.normal_int}")
        parts.append(f"normal_uint={self.normal_uint}")
        parts.append(f"big_int={self.big_int}")
        parts.append(f"big_uint={self.big_uint}")
        parts.append(f"single_precision={self.single_precision}")
        parts.append(f"double_precision={self.double_precision}")
        parts.append(f"message={self.message}")
        parts.append(f"status_array={self.status_array}")
        parts.append(f"tiny_int_array={self.tiny_int_array}")
        parts.append(f"tiny_uint_array={self.tiny_uint_array}")
        parts.append(f"small_int_array={self.small_int_array}")
        parts.append(f"small_uint_array={self.small_uint_array}")
        parts.append(f"normal_int_array={self.normal_int_array}")
        parts.append(f"normal_uint_array={self.normal_uint_array}")
        parts.append(f"big_int_array={self.big_int_array}")
        parts.append(f"big_uint_array={self.big_uint_array}")
        parts.append(f"single_precision_array={self.single_precision_array}")
        parts.append(f"double_precision_array={self.double_precision_array}")
        parts.append(f"message_array={self.message_array}")
        return f"ComplexMessage(" + ", ".join(parts) + ")"
