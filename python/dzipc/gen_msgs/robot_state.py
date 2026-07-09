"""
自动生成的消息封装 — 通过 GenericMessage 实现序列化，无需重编 C++ 动态库
"""
from __future__ import annotations

from typing import List
from dzipc._dzipc_core import GenericMessage

from .pose import Pose


class RobotState:
    """Python 侧 RobotState 封装（底层使用 GenericMessage）"""

    __slots__ = ("_g",
                 "name", "current_pose", "pose_history", "note")

    def __init__(self, generic=None):
        if generic is not None:
            self._g = generic
        else:
            self._g = GenericMessage()
            self.name = ""
            self.current_pose = Pose()
            self.pose_history = []
            self.note = ""

    def to_generic(self):
        """将 Python 字段序列化到 GenericMessage"""
        g = self._g
        g.clear()
        g.set_string("name", self.name)
        g.set_nested("current_pose", self.current_pose.to_generic() if hasattr(self.current_pose, "to_generic") else self.current_pose)
        g.set_nested_array("pose_history", [item.to_generic() if hasattr(item, "to_generic") else item for item in self.pose_history])
        g.set_string("note", self.note)
        return g

    @classmethod
    def from_generic(cls, g) -> "RobotState":
        """从 GenericMessage 反序列化"""
        obj = cls(g)
        obj.name = g.get_string("name")
        obj.current_pose = Pose.from_generic(g.get_nested("current_pose"))
        obj.pose_history = [Pose.from_generic(item) for item in g.get_nested_array("pose_history")]
        obj.note = g.get_string("note")
        return obj

    def __repr__(self):
        parts = []
        parts.append(f"name={self.name}")
        parts.append(f"current_pose={self.current_pose}")
        parts.append(f"pose_history={self.pose_history}")
        parts.append(f"note={self.note}")
        return f"RobotState(" + ", ".join(parts) + ")"
