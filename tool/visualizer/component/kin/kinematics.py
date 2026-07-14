from __future__ import annotations

from os import PathLike
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Union

import numpy as np

try:
    import kinpy as kp
except Exception as exc:  # pragma: no cover - exercised only when dependency is missing.
    kp = None
    _KINPY_IMPORT_ERROR = exc
else:
    _KINPY_IMPORT_ERROR = None


JointInput = Union[Mapping[str, float], Sequence[float], np.ndarray]
UrdfInput = Union[str, PathLike[str]]
RotationInput = Union[Sequence[float], np.ndarray]


class RobotKinematics:
    """Thin kinpy wrapper for robot forward/inverse kinematics.

    Use a serial chain when an end link is provided.  Serial chains support
    forward kinematics for the end-effector, inverse kinematics, and Jacobians.
    Full chains are useful when transforms for every link are needed.
    """

    def __init__(
        self,
        urdf: UrdfInput,
        end_link: Optional[str] = None,
        root_link: str = "",
        *,
        from_file: Optional[bool] = None,
    ) -> None:
        self._require_kinpy()
        self.urdf = self._read_urdf(urdf, from_file)
        self.end_link = end_link
        self.root_link = root_link
        if end_link:
            self.chain = kp.build_serial_chain_from_urdf(self.urdf, end_link, root_link)
            self.is_serial = True
        else:
            self.chain = kp.build_chain_from_urdf(self.urdf)
            self.is_serial = False
        self.joint_names = list(self.chain.get_joint_parameter_names())
        self.dof = len(self.joint_names)

    @classmethod
    def from_urdf_file(
        cls, path: UrdfInput, end_link: Optional[str] = None, root_link: str = ""
    ) -> "RobotKinematics":
        return cls(path, end_link=end_link, root_link=root_link, from_file=True)

    @classmethod
    def from_urdf_string(
        cls, urdf: str, end_link: Optional[str] = None, root_link: str = ""
    ) -> "RobotKinematics":
        return cls(urdf, end_link=end_link, root_link=root_link, from_file=False)

    @staticmethod
    def _require_kinpy() -> None:
        if kp is None:
            raise RuntimeError(
                "kinpy is required for RobotKinematics. Install it with "
                "`python3 -m pip install -r tool/requirements.txt` and activate "
                "the workspace with `source setup.sh`."
            ) from _KINPY_IMPORT_ERROR

    @staticmethod
    def _read_urdf(urdf: UrdfInput, from_file: Optional[bool]) -> str:
        if from_file is True:
            return Path(urdf).read_text(encoding="utf-8")
        if from_file is False:
            return str(urdf)
        if isinstance(urdf, PathLike):
            return Path(urdf).read_text(encoding="utf-8")

        text = str(urdf)
        if RobotKinematics._looks_like_urdf_xml(text):
            return text

        path = Path(text)
        try:
            if path.is_file():
                return path.read_text(encoding="utf-8")
        except OSError:
            pass

        raise FileNotFoundError(
            "URDF input is neither XML content containing '<robot' nor an existing file path: "
            f"{text[:160]!r}"
        )

    def zeros(self) -> Dict[str, float]:
        return {name: 0.0 for name in self.joint_names}

    def joint_vector(self, joints: JointInput) -> List[float]:
        if isinstance(joints, Mapping):
            missing = [name for name in self.joint_names if name not in joints]
            if missing:
                raise ValueError(f"missing joint values: {', '.join(missing)}")
            return [float(joints[name]) for name in self.joint_names]
        values = np.asarray(joints, dtype=float).reshape(-1).tolist()
        if len(values) != self.dof:
            raise ValueError(f"expected {self.dof} joint values, got {len(values)}")
        return [float(value) for value in values]

    def joint_map(self, joints: JointInput) -> Dict[str, float]:
        values = self.joint_vector(joints)
        return dict(zip(self.joint_names, values))

    def forward(
        self,
        joints: JointInput,
        *,
        link_name: Optional[str] = None,
        world: Any = None,
        as_matrix: bool = False,
    ) -> Union[Any, Dict[str, Any], np.ndarray, Dict[str, np.ndarray]]:
        """Calculate forward kinematics.

        For a serial chain without ``link_name`` this returns the end-effector
        transform.  For a full chain, or when ``link_name`` is provided, it
        returns transforms keyed by link name or the selected link transform.
        """

        joint_values = self.joint_vector(joints)
        world_transform = self._optional_transform(world, "world")
        if self.is_serial and link_name is None:
            result = self.chain.forward_kinematics(joint_values, world=world_transform, end_only=True)
            return self._matrix_or_transform(result, as_matrix)

        if self.is_serial:
            transforms = self.chain.forward_kinematics(joint_values, world=world_transform, end_only=False)
        else:
            transforms = self.chain.forward_kinematics(joint_values, world=world_transform)
        if link_name is not None:
            if link_name not in transforms:
                raise ValueError(f"unknown link: {link_name}")
            return self._matrix_or_transform(transforms[link_name], as_matrix)
        if as_matrix:
            return {name: transform.matrix() for name, transform in transforms.items()}
        return transforms

    def inverse(
        self,
        target: Any,
        initial_state: Optional[JointInput] = None,
    ) -> Dict[str, float]:
        """Calculate inverse kinematics for a serial chain target transform."""

        self._require_serial("inverse kinematics")
        target_transform = self.make_transform(target) if not self._is_transform(target) else target
        initial = None if initial_state is None else np.asarray(self.joint_vector(initial_state), dtype=float)
        solution = self.chain.inverse_kinematics(target_transform, initial_state=initial)
        return dict(zip(self.joint_names, [float(value) for value in solution]))

    def jacobian(self, joints: JointInput, *, end_only: bool = True) -> Union[np.ndarray, Dict[str, np.ndarray]]:
        """Calculate a serial-chain geometric Jacobian."""

        self._require_serial("jacobian")
        return self.chain.jacobian(self.joint_vector(joints), end_only=end_only)

    def link_names(self) -> List[str]:
        return [frame.link.name for frame in self.chain if frame.link is not None]

    def link_states(self, joints: JointInput, *, world: Any = None) -> List[Dict[str, Any]]:
        """Return JSON-friendly transforms for every link in the chain."""

        transforms = self.forward(joints, world=world, as_matrix=False)
        if not isinstance(transforms, dict):
            link_name = self.end_link or "end"
            transforms = {link_name: transforms}

        states: List[Dict[str, Any]] = []
        names = self.link_names()
        for name in names:
            transform = transforms.get(name)
            if transform is None:
                continue
            matrix = transform.matrix()
            states.append(
                {
                    "name": name,
                    "position": np.asarray(transform.pos, dtype=float).tolist(),
                    "rotation": np.asarray(transform.rot, dtype=float).tolist(),
                    "rpy": np.asarray(transform.rot_euler, dtype=float).tolist(),
                    "matrix": np.asarray(matrix, dtype=float).tolist(),
                }
            )
        return states

    @staticmethod
    def make_transform(
        pose: Union[Any, Mapping[str, Any], Sequence[float], np.ndarray],
        rotation: Optional[RotationInput] = None,
    ) -> Any:
        """Create a kinpy Transform.

        Accepted forms:
        - existing ``kp.Transform``
        - mapping with ``position``/``pos`` and ``rotation``/``rot``
        - 4x4 homogeneous matrix
        - sequence ``[x, y, z]`` with optional rotation argument
        - sequence ``[x, y, z, qw, qx, qy, qz]``
        - sequence ``[x, y, z, roll, pitch, yaw]``
        """

        RobotKinematics._require_kinpy()
        if RobotKinematics._is_transform(pose):
            return pose
        if isinstance(pose, Mapping):
            pos = pose.get("position", pose.get("pos", [0.0, 0.0, 0.0]))
            rot = pose.get("rotation", pose.get("rot", rotation))
            return kp.Transform(rot=RobotKinematics._normalize_rotation(rot), pos=np.asarray(pos, dtype=float))

        arr = np.asarray(pose, dtype=float)
        if arr.shape == (4, 4):
            return RobotKinematics._transform_from_matrix(arr)
        flat = arr.reshape(-1)
        if flat.size == 3:
            return kp.Transform(rot=RobotKinematics._normalize_rotation(rotation), pos=flat[:3])
        if flat.size == 6:
            return kp.Transform(rot=flat[3:6], pos=flat[:3])
        if flat.size == 7:
            return kp.Transform(rot=flat[3:7], pos=flat[:3])
        raise ValueError("pose must be a Transform, mapping, 4x4 matrix, xyz, xyz+rpy, or xyz+quaternion")

    @staticmethod
    def transform_to_dict(transform: Any) -> Dict[str, List[float]]:
        RobotKinematics._require_transform(transform, "transform")
        return {
            "position": np.asarray(transform.pos, dtype=float).tolist(),
            "rotation": np.asarray(transform.rot, dtype=float).tolist(),
            "rpy": np.asarray(transform.rot_euler, dtype=float).tolist(),
        }

    @staticmethod
    def transform_to_matrix(transform: Any) -> np.ndarray:
        RobotKinematics._require_transform(transform, "transform")
        return transform.matrix()

    @staticmethod
    def _matrix_or_transform(transform: Any, as_matrix: bool) -> Any:
        return transform.matrix() if as_matrix else transform

    @staticmethod
    def _looks_like_urdf_xml(value: str) -> bool:
        return "<robot" in value

    @staticmethod
    def _normalize_rotation(rotation: Optional[RotationInput]) -> List[float]:
        if rotation is None:
            return [1.0, 0.0, 0.0, 0.0]
        values = np.asarray(rotation, dtype=float).reshape(-1).tolist()
        if len(values) not in (3, 4):
            raise ValueError("rotation must be roll/pitch/yaw or scalar-first quaternion")
        return [float(value) for value in values]

    @staticmethod
    def _transform_from_matrix(matrix: np.ndarray) -> Any:
        RobotKinematics._require_kinpy()
        rot = kp.transform.tf.quaternion_from_matrix(matrix)
        pos = matrix[:3, 3]
        return kp.Transform(rot=rot, pos=pos)

    @staticmethod
    def _is_transform(value: Any) -> bool:
        return kp is not None and isinstance(value, kp.Transform)

    @staticmethod
    def _require_transform(value: Any, name: str) -> None:
        if not RobotKinematics._is_transform(value):
            raise TypeError(f"{name} must be a kinpy Transform")

    @staticmethod
    def _optional_transform(value: Any, name: str) -> Optional[Any]:
        if value is None:
            return None
        if RobotKinematics._is_transform(value):
            return value
        try:
            return RobotKinematics.make_transform(value)
        except Exception as exc:
            raise TypeError(f"{name} must be a kinpy Transform, 4x4 matrix, or pose sequence") from exc

    def _require_serial(self, operation: str) -> None:
        if not self.is_serial:
            raise RuntimeError(f"{operation} requires end_link/root_link serial chain construction")


class robot_kin(RobotKinematics):
    """Backward-compatible class name for older visualizer code."""


__all__ = ["RobotKinematics", "robot_kin"]
