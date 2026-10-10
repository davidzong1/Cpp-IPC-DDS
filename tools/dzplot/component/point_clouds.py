from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import Any, Dict, Optional, Tuple


def _get_field(value: Any, name: str, default: Any = None) -> Any:
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def _to_jsonable(value: Any, depth: int = 0) -> Any:
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


class PointCloudBinaryCodec:
    BINARY_MAGIC = b"DZPC"
    BINARY_VERSION = 1
    BINARY_POINT_CLOUD = 1
    BINARY_POINT_CLOUD_HAS_COLORS = 1
    BINARY_HEADER = struct.Struct("<4sBBHIIII")
    ENCODING = "dzpc.pointcloud.v1"

    @staticmethod
    def vector_xyz(value: Any) -> Optional[Tuple[float, float, float]]:
        data = _get_field(value, "data")
        if isinstance(data, (list, tuple)) and len(data) >= 3:
            return float(data[0]), float(data[1]), float(data[2])
        x = _get_field(value, "x")
        y = _get_field(value, "y")
        z = _get_field(value, "z", 0.0)
        if x is None or y is None:
            return None
        return float(x), float(y), float(z)

    @staticmethod
    def color_rgba(value: Any) -> Tuple[float, float, float, float]:
        return (
            float(_get_field(value, "r", 1.0)),
            float(_get_field(value, "g", 1.0)),
            float(_get_field(value, "b", 1.0)),
            float(_get_field(value, "a", 1.0)),
        )

    def encode(self, msg_obj: Any, sample_id: int) -> Tuple[Dict[str, Any], bytes]:
        points = _get_field(msg_obj, "points", []) or []
        colors = _get_field(msg_obj, "colors", []) or []
        if not hasattr(points, "__len__"):
            points = list(points)
        if not hasattr(colors, "__len__"):
            colors = list(colors)
        point_count = len(points)
        has_colors = len(colors) == point_count and point_count > 0
        flags = self.BINARY_POINT_CLOUD_HAS_COLORS if has_colors else 0
        header_size = self.BINARY_HEADER.size
        point_bytes = point_count * 3 * 4
        color_bytes = point_count * 4 * 4 if has_colors else 0
        payload = bytearray(header_size + point_bytes + color_bytes)
        self.BINARY_HEADER.pack_into(
            payload,
            0,
            self.BINARY_MAGIC,
            self.BINARY_VERSION,
            self.BINARY_POINT_CLOUD,
            flags,
            sample_id,
            point_count,
            point_count if has_colors else 0,
            0,
        )
        offset = header_size
        for point in points:
            xyz = self.vector_xyz(point) or (0.0, 0.0, 0.0)
            struct.pack_into("<fff", payload, offset, *xyz)
            offset += 12
        if has_colors:
            for color in colors:
                struct.pack_into("<ffff", payload, offset, *self.color_rgba(color))
                offset += 16

        metadata = {
            "encoding": self.ENCODING,
            "sample_id": sample_id,
            "point_count": point_count,
            "has_colors": has_colors,
            "byte_length": len(payload),
        }
        return metadata, bytes(payload)

    def lightweight_data(self, msg_obj: Any) -> Dict[str, Any]:
        return {
            "header": _to_jsonable(_get_field(msg_obj, "header", {})),
            "channel_names": _to_jsonable(_get_field(msg_obj, "channel_names", [])),
            "binary_points": True,
        }


@dataclass
class PointCloudSampleEncoder:
    codec: PointCloudBinaryCodec = field(default_factory=PointCloudBinaryCodec)

    def encode_event_fields(self, msg_obj: Any, sample_id: int) -> Dict[str, Any]:
        metadata, binary_payload = self.codec.encode(msg_obj, sample_id)
        return {
            "data": self.codec.lightweight_data(msg_obj),
            "binary": metadata,
            "binary_payload": binary_payload,
        }


DEFAULT_POINT_CLOUD_CODEC = PointCloudBinaryCodec()
DEFAULT_POINT_CLOUD_SAMPLE_ENCODER = PointCloudSampleEncoder(DEFAULT_POINT_CLOUD_CODEC)
