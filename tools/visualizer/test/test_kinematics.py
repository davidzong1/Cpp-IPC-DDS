#!/usr/bin/env python3
from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT_DIR = Path(__file__).resolve().parents[3]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tool.visualizer.component.kin import kinematics
from tool.visualizer.component.kin.kinematics import RobotKinematics, robot_kin


TINY_URDF = """<?xml version="1.0"?>
<robot name="tiny">
  <link name="base_link"/>
  <link name="link1"/>
  <link name="tool0"/>
  <joint name="joint1" type="revolute">
    <parent link="base_link"/>
    <child link="link1"/>
    <origin xyz="0 0 0" rpy="0 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-3.141592653589793" upper="3.141592653589793" effort="1" velocity="1"/>
  </joint>
  <joint name="tool_fixed" type="fixed">
    <parent link="link1"/>
    <child link="tool0"/>
    <origin xyz="1 0 0" rpy="0 0 0"/>
  </joint>
</robot>
"""


@unittest.skipIf(kinematics.kp is None, "kinpy is not available")
class RobotKinematicsTests(unittest.TestCase):
    def test_serial_robot_kin_alias_forward_and_jacobian(self) -> None:
        kin = robot_kin.from_urdf_string(TINY_URDF, end_link="tool0", root_link="base_link")

        self.assertIsInstance(kin, robot_kin)
        self.assertEqual(["joint1"], kin.joint_names)
        self.assertEqual(1, kin.dof)
        self.assertEqual({"joint1": 0.0}, kin.zeros())

        zero_pose = kin.forward({"joint1": 0.0})
        np.testing.assert_allclose(zero_pose.pos, [1.0, 0.0, 0.0], atol=1e-9)

        turned_pose = kin.forward([math.pi / 2.0])
        np.testing.assert_allclose(turned_pose.pos, [0.0, 1.0, 0.0], atol=1e-9)

        matrix = kin.forward([math.pi / 2.0], as_matrix=True)
        self.assertEqual((4, 4), matrix.shape)
        np.testing.assert_allclose(matrix[:3, 3], [0.0, 1.0, 0.0], atol=1e-9)

        jacobian = kin.jacobian([0.0])
        self.assertEqual((6, 1), jacobian.shape)

    def test_full_chain_returns_link_transforms_and_rejects_jacobian(self) -> None:
        kin = RobotKinematics.from_urdf_string(TINY_URDF)

        self.assertFalse(kin.is_serial)
        self.assertEqual(["joint1"], kin.joint_names)

        transforms = kin.forward(kin.zeros(), as_matrix=True)
        self.assertEqual({"base_link", "link1", "tool0"}, set(transforms))
        self.assertEqual((4, 4), transforms["tool0"].shape)
        np.testing.assert_allclose(transforms["tool0"][:3, 3], [1.0, 0.0, 0.0], atol=1e-9)

        with self.assertRaisesRegex(RuntimeError, "requires end_link/root_link serial chain"):
            kin.jacobian(kin.zeros())


if __name__ == "__main__":
    unittest.main()
