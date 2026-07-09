"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage


class RewardCfgRequest:
    """Python 侧 RewardCfgRequest 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "reward_cfg_names", "reward_cfg_values")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.reward_cfg_names = []
            self.reward_cfg_values = []

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_string_array("reward_cfg_names", self.reward_cfg_names)
        g.set_float32_array("reward_cfg_values", self.reward_cfg_values)
        return g

    @classmethod
    def from_generic(cls, g) -> "RewardCfgRequest":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.reward_cfg_names = list(g.get_string_array("reward_cfg_names"))
        obj.reward_cfg_values = list(g.get_float32_array("reward_cfg_values"))
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"reward_cfg_names={self.reward_cfg_names}")
        parts.append(f"reward_cfg_values={self.reward_cfg_values}")
        return f"RewardCfgRequest(" + ", ".join(parts) + ")"


class RewardCfgResponse:
    """Python 侧 RewardCfgResponse 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "success", "message")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.success = False
            self.message = ""

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_bool("success", self.success)
        g.set_string("message", self.message)
        return g

    @classmethod
    def from_generic(cls, g) -> "RewardCfgResponse":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.success = g.get_bool("success")
        obj.message = g.get_string("message")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"success={self.success}")
        parts.append(f"message={self.message}")
        return f"RewardCfgResponse(" + ", ".join(parts) + ")"
