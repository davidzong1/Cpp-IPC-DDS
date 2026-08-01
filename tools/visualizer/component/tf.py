"""
TF (Transform) component for dzIPC visualizer.

Handles StdTF messages defined in msg/std_msgs/std_tf.msg::

    StdHeader header    # header.frame_id: string, header.stamp: float64
    StdMatrix3d rot     # 3×3 rotation matrix (StdVector3d[3], each data[3])
    StdVector3d trans   # 3-D translation (data[3]: x, y, z)

Converts rotation matrices to quaternions for compact web transmission.
Follows the same encoder pattern as point_clouds.py.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Helpers (match the convention in point_clouds.py / robot.py)
# ---------------------------------------------------------------------------

def _get_field(value: Any, name: str, default: Any = None) -> Any:
    """Extract *name* from a dict or object attribute."""
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def _to_jsonable(value: Any, depth: int = 0) -> Any:
    """Mirrors ``to_jsonable`` in point_clouds.py."""
    if depth > 16:
        return "<max-depth>"
    if value is None or isinstance(value, (bool, int, float, str)):
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return value
    if isinstance(value, (list, tuple)):
        return [_to_jsonable(item, depth + 1) for item in value]
    if isinstance(value, dict):
        return {str(key): _to_jsonable(val, depth + 1) for key, val in value.items()}
    if hasattr(value, "__slots__"):
        out: Dict[str, Any] = {}
        for name in getattr(value, "__slots__", ()):
            if name.startswith("_"):
                continue
            if hasattr(value, name):
                out[name] = _to_jsonable(getattr(value, name), depth + 1)
        return out
    if hasattr(value, "__dict__"):
        return {
            key: _to_jsonable(val, depth + 1)
            for key, val in vars(value).items()
            if not key.startswith("_")
        }
    return str(value)


# ---------------------------------------------------------------------------
# StdVector3d / StdMatrix3d extraction
# ---------------------------------------------------------------------------

def _parse_vector3d(value: Any) -> Tuple[float, float, float]:
    """Parse a ``StdVector3d`` into ``(x, y, z)``.

    ``StdVector3d`` is ``float64[3] data`` (``msg/std_msgs/std_vector_3d.msg``).
    Handles both the generated Python class (``.data`` attribute) and plain
    dicts / lists.
    """
    if value is None:
        return (0.0, 0.0, 0.0)

    data = _get_field(value, "data")
    if isinstance(data, (list, tuple)) and len(data) >= 3:
        return (float(data[0]), float(data[1]), float(data[2]))
    if isinstance(data, (list, tuple)):
        return tuple(float(v) for v in data[:3])  # type: ignore[return-value]

    # Fallback: try x/y/z directly (covers dicts and geometry-like objects)
    x = _get_field(value, "x")
    y = _get_field(value, "y")
    z = _get_field(value, "z")
    if x is not None and y is not None:
        return (float(x), float(y), float(z if z is not None else 0.0))

    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return (float(value[0]), float(value[1]), float(value[2]))

    return (0.0, 0.0, 0.0)


def _parse_matrix3d(value: Any) -> List[List[float]]:
    """Parse a ``StdMatrix3d`` into a 3×3 list-of-lists.

    ``StdMatrix3d`` wraps ``StdVector3d[3] rot``
    (``msg/std_msgs/std_matrix_3d.msg``).  Each row is a ``StdVector3d``
    whose ``.data`` holds the three column entries for that row.
    """
    if value is None:
        return [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]

    # Unwrap StdMatrix3d → StdVector3d[3] via its ``rot`` field
    rows = _get_field(value, "rot")
    if rows is None:
        rows = value  # *value* might already be the raw row array

    if isinstance(rows, (list, tuple)):
        flat = [float(v) for v in rows]  # ← trigger TypeError if not numeric
        if len(flat) >= 9:
            return [flat[0:3], flat[3:6], flat[6:9]]
        if len(flat) >= 3:
            return [
                list(_parse_vector3d(rows[0])),
                list(_parse_vector3d(rows[1])),
                list(_parse_vector3d(rows[2])),
            ]

    return [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]


# ---------------------------------------------------------------------------
# Rotation matrix → quaternion (Shepperd's method)
# ---------------------------------------------------------------------------

def matrix_to_quaternion(m: List[List[float]]) -> Tuple[float, float, float, float]:
    """Convert a 3×3 rotation matrix to a unit quaternion ``(x, y, z, w)``."""
    if len(m) < 3 or any(len(row) < 3 for row in m[:3]):
        return (0.0, 0.0, 0.0, 1.0)

    r00, r01, r02 = m[0][0], m[0][1], m[0][2]
    r10, r11, r12 = m[1][0], m[1][1], m[1][2]
    r20, r21, r22 = m[2][0], m[2][1], m[2][2]

    trace = r00 + r11 + r22

    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (r21 - r12) / s
        y = (r02 - r20) / s
        z = (r10 - r01) / s
    elif r00 > r11 and r00 > r22:
        s = math.sqrt(1.0 + r00 - r11 - r22) * 2.0
        w = (r21 - r12) / s
        x = 0.25 * s
        y = (r01 + r10) / s
        z = (r02 + r20) / s
    elif r11 > r22:
        s = math.sqrt(1.0 + r11 - r00 - r22) * 2.0
        w = (r02 - r20) / s
        x = (r01 + r10) / s
        y = 0.25 * s
        z = (r12 + r21) / s
    else:
        s = math.sqrt(1.0 + r22 - r00 - r11) * 2.0
        w = (r10 - r01) / s
        x = (r02 + r20) / s
        y = (r12 + r21) / s
        z = 0.25 * s

    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return (x / norm, y / norm, z / norm, w / norm)


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class TFTransform:
    """A single parsed transform from a ``StdTF`` message.

    ``rotation`` is stored as a quaternion ``(qx, qy, qz, qw)``
    (converted from the 3×3 rotation matrix on ingestion).
    """

    frame_id: str
    translation: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    stamp: float = 0.0

    # Convenience accessors -------------------------------------------------
    @property
    def x(self) -> float:
        return self.translation[0]

    @property
    def y(self) -> float:
        return self.translation[1]

    @property
    def z(self) -> float:
        return self.translation[2]

    @property
    def qx(self) -> float:
        return self.rotation[0]

    @property
    def qy(self) -> float:
        return self.rotation[1]

    @property
    def qz(self) -> float:
        return self.rotation[2]

    @property
    def qw(self) -> float:
        return self.rotation[3]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "frame_id": self.frame_id,
            "translation": list(self.translation),
            "rotation": list(self.rotation),
            "stamp": self.stamp,
        }


# ---------------------------------------------------------------------------
# Encoder  (mirrors PointCloudSampleEncoder)
# ---------------------------------------------------------------------------

@dataclass
class TFSampleEncoder:
    """Encodes ``StdTF`` messages for the web bridge.

    Parses the fields defined in ``msg/std_msgs/std_tf.msg``:

    * ``header.frame_id`` — frame identifier
    * ``header.stamp``   — timestamp
    * ``rot``            — 3×3 rotation matrix → quaternion
    * ``trans``          — 3-D translation vector

    Usage (same shape as ``PointCloudSampleEncoder``)::

        encoder = TFSampleEncoder()
        event_fields = encoder.encode_event_fields(msg_obj, sample_id)
    """

    # -- public API ---------------------------------------------------------

    def encode_event_fields(self, msg_obj: Any, _sample_id: int) -> Dict[str, Any]:
        """Return the ``sample`` event data dict for *msg_obj*.
        ``_sample_id`` is accepted for interface compatibility with
        ``PointCloudSampleEncoder`` but is not used by the TF encoder.
        """
        tf = self.parse(msg_obj)
        return {
            "data": self.lightweight_data(msg_obj, tf),
        }

    def lightweight_data(self, msg_obj: Any, tf: Optional[TFTransform] = None) -> Dict[str, Any]:
        """Lightweight JSON-serialisable summary (cf. ``PointCloudBinaryCodec.lightweight_data``)."""
        if tf is None:
            tf = self.parse(msg_obj)
        return {
            "header": _to_jsonable(_get_field(msg_obj, "header", {})),
            "frame_id": tf.frame_id,
            "translation": list(tf.translation),
            "rotation": list(tf.rotation),
            "stamp": tf.stamp,
        }

    # -- parsing ------------------------------------------------------------

    def parse(self, msg_obj: Any) -> TFTransform:
        """Extract one ``TFTransform`` from a ``StdTF`` message object.

        Covers both the generated Python binding (attribute access) and
        plain dicts (JSON-compatible representations).
        """
        header = _get_field(msg_obj, "header", {})

        frame_id = str(_get_field(header, "frame_id", ""))
        stamp = float(_get_field(header, "stamp", 0.0))

        # Rotation: StdMatrix3d { rot: StdVector3d[3] }
        rot = _get_field(msg_obj, "rot")
        matrix = _parse_matrix3d(rot)
        quaternion = matrix_to_quaternion(matrix)

        # Translation: StdVector3d { data: float64[3] }
        trans = _get_field(msg_obj, "trans")
        tx, ty, tz = _parse_vector3d(trans)

        return TFTransform(
            frame_id=frame_id,
            translation=(tx, ty, tz),
            rotation=quaternion,
            stamp=stamp,
        )


# -- Default instance (follows point_clouds.py convention) -----------------

DEFAULT_TF_SAMPLE_ENCODER = TFSampleEncoder()


__all__ = [
    "TFTransform",
    "TFSampleEncoder",
    "DEFAULT_TF_SAMPLE_ENCODER",
    "matrix_to_quaternion",
]
