#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import ctypes
import gc
import hashlib
import importlib.machinery
import json
import math
import mimetypes
import queue
import socket
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple
from urllib.parse import quote, unquote

ROOT_DIR = Path(__file__).resolve().parents[2]
WEB_DIR = Path(__file__).resolve().parent / "web"
DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.json"
VISUALIZER_DIR = Path(__file__).resolve().parent

if str(VISUALIZER_DIR) not in sys.path:
    sys.path.insert(0, str(VISUALIZER_DIR))

from component.robot import (  # noqa: E402
    RobotDisplay,
    RobotStateDisplay,
    collect_urdf_mesh_assets,
    normalize_robot_displays as component_normalize_robot_displays,
    normalize_robot_models as component_normalize_robot_models,
    normalize_robot_state_displays as component_normalize_robot_state_displays,
)
from component.point_clouds import (  # noqa: E402
    DEFAULT_POINT_CLOUD_SAMPLE_ENCODER,
    PointCloudBinaryCodec,
)
from component.subscriber import DzipcSubscriber  # noqa: E402
from component.demo_publisher import DemoPublisher  # noqa: E402
from component.topic_spec import (  # noqa: E402
    MIN_POLL_INTERVAL_S,
    TopicSpec,
    normalize_poll_interval,
)
from component.tf import DEFAULT_TF_SAMPLE_ENCODER  # noqa: E402

VISUALIZER_MESSAGE_ALIASES = {
    "RawMessage": "StdRawMessage",
    "Pose": "StdPose",
    "Path": "StdPath",
    "PointCloud": "StdPointCloud",
    "Marker": "StdMarker",
    "Image": "StdImage",
    "TF": "StdTF",
    "RobotState": "RobotState",
}

VISUALIZER_MESSAGE_TYPES = list(VISUALIZER_MESSAGE_ALIASES.keys())
VISUALIZER_DISPLAY_TYPES = VISUALIZER_MESSAGE_TYPES[:-1] + ["Robot", "RobotState"]

BINARY_MAGIC = PointCloudBinaryCodec.BINARY_MAGIC
BINARY_VERSION = PointCloudBinaryCodec.BINARY_VERSION
BINARY_POINT_CLOUD = PointCloudBinaryCodec.BINARY_POINT_CLOUD
BINARY_POINT_CLOUD_HAS_COLORS = PointCloudBinaryCodec.BINARY_POINT_CLOUD_HAS_COLORS
BINARY_HEADER = PointCloudBinaryCodec.BINARY_HEADER
POINT_CLOUD_ENCODER = DEFAULT_POINT_CLOUD_SAMPLE_ENCODER


def compatible_dzipc_roots() -> List[Path]:
    candidates = [
        ROOT_DIR / "local" / "lib" / "python",
        ROOT_DIR / "python",
    ]
    roots: List[Path] = []
    for root in candidates:
        package_dir = root / "dzipc"
        if not package_dir.is_dir():
            continue
        if any((package_dir / f"_dzipc_core{suffix}").exists() for suffix in importlib.machinery.EXTENSION_SUFFIXES):
            roots.append(root)
    return roots


def load_dzipc():
    local_libipc = ROOT_DIR / "local" / "lib" / "libipc.so"
    if local_libipc.exists():
        ctypes.CDLL(str(local_libipc), mode=ctypes.RTLD_GLOBAL)
    candidate_roots = [
        ROOT_DIR / "local" / "lib" / "python",
        ROOT_DIR / "python",
    ]
    candidate_strings = {str(path) for path in candidate_roots}
    sys.path[:] = [path for path in sys.path if path not in candidate_strings]
    roots = compatible_dzipc_roots()
    if not roots:
        roots = [root for root in candidate_roots if (root / "dzipc").is_dir()]
    for root in reversed(roots):
        sys.path.insert(0, str(root))
    try:
        import dzipc as ipc  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            f"Cannot import dzipc for Python {sys.version.split()[0]} ({sys.executable}). "
            "Build/install the Python package for this Python environment first or run with --demo."
        ) from exc
    return ipc


def now_ms() -> int:
    return int(time.time() * 1000)


def to_jsonable(value: Any, depth: int = 0) -> Any:
    if depth > 16:
        return "<max-depth>"
    if hasattr(value, "field_count") and hasattr(value, "field_name") and hasattr(value, "field_type"):
        return generic_message_to_jsonable(value, depth + 1)
    if value is None or isinstance(value, (bool, int, float, str)):
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return value
    if isinstance(value, (list, tuple)):
        return [to_jsonable(item, depth + 1) for item in value]
    if isinstance(value, dict):
        return {str(key): to_jsonable(val, depth + 1) for key, val in value.items()}
    if hasattr(value, "__slots__"):
        out: Dict[str, Any] = {}
        for name in getattr(value, "__slots__", ()):
            if name.startswith("_"):
                continue
            if hasattr(value, name):
                out[name] = to_jsonable(getattr(value, name), depth + 1)
        return out
    if hasattr(value, "__dict__"):
        return {
            key: to_jsonable(val, depth + 1)
            for key, val in vars(value).items()
            if not key.startswith("_")
        }
    return str(value)


def generic_message_to_jsonable(value: Any, depth: int = 0) -> Dict[str, Any]:
    scalar_getters = {
        1: "get_bool",
        2: "get_int8",
        3: "get_uint8",
        4: "get_int16",
        5: "get_uint16",
        6: "get_int32",
        7: "get_uint32",
        8: "get_int64",
        9: "get_uint64",
        10: "get_float32",
        11: "get_float64",
        12: "get_string",
    }
    array_getters = {
        13: "get_bool_array",
        14: "get_int8_array",
        15: "get_uint8_array",
        16: "get_int16_array",
        17: "get_uint16_array",
        18: "get_int32_array",
        19: "get_uint32_array",
        20: "get_int64_array",
        21: "get_uint64_array",
        22: "get_float32_array",
        23: "get_float64_array",
        24: "get_string_array",
    }
    out: Dict[str, Any] = {}
    for index in range(int(value.field_count())):
        name = str(value.field_name(index))
        field_type = int(value.field_type(index))
        try:
            if field_type in scalar_getters:
                out[name] = to_jsonable(getattr(value, scalar_getters[field_type])(name), depth + 1)
            elif field_type in array_getters:
                out[name] = to_jsonable(list(getattr(value, array_getters[field_type])(name)), depth + 1)
            elif field_type == 25:
                out[name] = generic_message_to_jsonable(value.get_nested(name), depth + 1)
            elif field_type == 26:
                out[name] = [generic_message_to_jsonable(item, depth + 1) for item in value.get_nested_array(name)]
            else:
                out[name] = f"<unsupported-field-type:{field_type}>"
        except Exception as exc:
            out[name] = f"<decode-error:{exc}>"
    return out


def get_field(value: Any, name: str, default: Any = None) -> Any:
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def vector_xyz(value: Any) -> Optional[Tuple[float, float, float]]:
    return PointCloudBinaryCodec.vector_xyz(value)


def color_rgba(value: Any) -> Tuple[float, float, float, float]:
    return PointCloudBinaryCodec.color_rgba(value)


def encode_point_cloud_binary(
    msg_obj: Any, sample_id: int
) -> Tuple[Dict[str, Any], bytes]:
    return POINT_CLOUD_ENCODER.codec.encode(msg_obj, sample_id)


def lightweight_point_cloud_data(msg_obj: Any) -> Dict[str, Any]:
    return POINT_CLOUD_ENCODER.codec.lightweight_data(msg_obj)


def encode_image_data(msg_obj: Any) -> Dict[str, Any]:
    width = int(get_field(msg_obj, "width", 0))
    height = int(get_field(msg_obj, "height", 0))
    encoding = str(get_field(msg_obj, "encoding", ""))
    step = int(get_field(msg_obj, "step", 0))
    raw_data = get_field(msg_obj, "data", []) or []
    if isinstance(raw_data, list):
        raw_bytes = bytes(raw_data)
    else:
        raw_bytes = bytes(raw_data)
    data_b64 = base64.b64encode(raw_bytes).decode("ascii")
    return {
        "encoding_type": "dzpc.image.v1",
        "width": width,
        "height": height,
        "image_encoding": encoding,
        "step": step,
        "data_length": len(raw_bytes),
        "data_b64": data_b64,
    }


def make_frame(payload: bytes, opcode: int = 1) -> bytes:
    first = 0x80 | (opcode & 0x0F)
    size = len(payload)
    if size < 126:
        return bytes([first, size]) + payload
    if size <= 0xFFFF:
        return bytes([first, 126]) + struct.pack("!H", size) + payload
    return bytes([first, 127]) + struct.pack("!Q", size) + payload


async def read_ws_frame(reader: asyncio.StreamReader) -> Optional[Tuple[int, bytes]]:
    head = await reader.readexactly(2)
    opcode = head[0] & 0x0F
    masked = bool(head[1] & 0x80)
    size = head[1] & 0x7F
    if size == 126:
        size = struct.unpack("!H", await reader.readexactly(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if masked else b""
    payload = await reader.readexactly(size) if size else b""
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    if opcode == 8:
        return None
    return opcode, payload


class WebHub:
    def __init__(self, defaults: Dict[str, Any], config_path: Path) -> None:
        self.clients: set[asyncio.StreamWriter] = set()
        self.events: "queue.Queue[Dict[str, Any]]" = queue.Queue()
        self.topics: Dict[str, Dict[str, Any]] = {}
        self.defaults = defaults
        self.config_path = config_path
        self.subscribers: Dict[str, DzipcSubscriber] = {}
        self.demo_workers: Dict[str, DemoPublisher] = {}
        self.message_types = discover_message_types()
        self.robot_displays: Dict[str, RobotDisplay] = {}
        self.robot_state_displays: Dict[str, RobotStateDisplay] = {}
        self.robot_asset_paths: Dict[str, Path] = {}
        self.apply_robot_display_config(defaults, replace=True, publish=False)
        # Track recently removed topics with timestamps to enforce
        # a cooldown period before re-add, ensuring C++ SHM cleanup
        # completes.  Key: topic_name, Value: removal timestamp (monotonic).
        self._removed_topics: Dict[str, float] = {}

    def config_path_str(self) -> str:
        return str(self.config_path)

    def resolve_config_path(self, path: Any = None) -> Path:
        if path is None or str(path).strip() == "":
            target = self.config_path
        else:
            target = Path(str(path).strip()).expanduser()
            if not target.is_absolute():
                target = (ROOT_DIR / target).resolve()
            else:
                target = target.resolve()
        if target.exists() and target.is_dir():
            target = target / "config.json"
        return target

    def publish(self, event: Dict[str, Any]) -> None:
        self.events.put(event)

    async def add_client(self, writer: asyncio.StreamWriter) -> None:
        try:
            await self.send(
                writer,
                {
                    "kind": "hello",
                    "topics": list(self.topics.values()),
                    "config": self.export_config(),
                    "config_path": self.config_path_str(),
                    "message_types": self.message_types,
                    "message_type_map": VISUALIZER_MESSAGE_ALIASES,
                },
            )
        except Exception:
            await self.remove_client(writer)
            return
        self.clients.add(writer)

    async def remove_client(self, writer: asyncio.StreamWriter) -> None:
        self.clients.discard(writer)
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    async def send(self, writer: asyncio.StreamWriter, event: Dict[str, Any]) -> None:
        json_event = (
            {key: val for key, val in event.items() if key != "binary_payload"}
            if "binary_payload" in event
            else event
        )
        payload = json.dumps(
            json_event, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        writer.write(make_frame(payload))
        await writer.drain()

    async def send_binary(self, writer: asyncio.StreamWriter, payload: bytes) -> None:
        writer.write(make_frame(payload, opcode=2))
        await writer.drain()

    async def send_event(
        self, writer: asyncio.StreamWriter, event: Dict[str, Any]
    ) -> None:
        await self.send(writer, event)
        binary_payload = event.get("binary_payload")
        if isinstance(binary_payload, (bytes, bytearray, memoryview)):
            await self.send_binary(writer, bytes(binary_payload))

    async def broadcast_loop(self) -> None:
        while True:
            try:
                event = await asyncio.to_thread(self.events.get, True, 0.2)
            except queue.Empty:
                continue
            except asyncio.CancelledError:
                break
            if event.get("kind") == "__shutdown__":
                break
            if event.get("kind") == "sample":
                topic = self.topics.setdefault(event["topic"], {})
                for state_display in self.robot_state_displays.values():
                    if state_display.topic == event["topic"]:
                        robot_state_result = state_display.handle_joint_state_sample(event.get("data", {}))
                        event["robot_state_display_id"] = state_display.id
                        event["robot_id"] = state_display.target_robot_id
                        if robot_state_result.get("ok"):
                            event["robot_state"] = robot_state_result
                            event["robot_joint_state"] = robot_state_result.get("joint_state", {})
                            event["robot_link_states"] = robot_state_result.get("link_states", [])
                        else:
                            event["robot_state_error"] = robot_state_result
                topic.update(
                    {
                        "name": event["topic"],
                        "type": event.get("msg_type", topic.get("type", "")),
                        "msg_type": event.get("msg_type", topic.get("msg_type", "")),
                        "transport": event.get("transport", topic.get("transport", "")),
                        "domain": event.get("domain", topic.get("domain", 0)),
                        "queue": event.get(
                            "queue", topic.get("queue", self.defaults.get("queue", 10))
                        ),
                        "extra": event.get("extra", topic.get("extra", "")),
                        "updated_ms": event.get("timestamp_ms", now_ms()),
                        "active": True,
                    }
                )
            dead: List[asyncio.StreamWriter] = []
            for writer in list(self.clients):
                try:
                    await self.send_event(writer, event)
                except Exception:
                    dead.append(writer)
            for writer in dead:
                await self.remove_client(writer)

    async def shutdown(self) -> None:
        for worker in list(self.subscribers.values()):
            worker.stop_event.set()
        for worker in list(self.subscribers.values()):
            worker.join(timeout=1.0)
        self.subscribers.clear()
        for worker in list(self.demo_workers.values()):
            worker.stop_event.set()
        self.demo_workers.clear()
        self.publish({"kind": "__shutdown__"})
        for writer in list(self.clients):
            await self.remove_client(writer)

    def add_topic(self, spec: "TopicSpec") -> None:
        existing = self.subscribers.get(spec.topic)
        if existing is not None and existing.is_alive():
            self.remove_topic(spec.topic)
        elif existing is not None:
            self.subscribers.pop(spec.topic, None)
        # Enforce cooldown if the same topic was recently removed.
        # Without this the new subscriber's shm_sub_ipc::InitChannel
        # can conflict with stale shared-memory segments from the
        # previous (possibly different-type) subscriber.
        # The sleep is deferred to DzipcSubscriber.run() to avoid
        # blocking the asyncio event loop.
        cooldown_remain = 0.0
        removed_at = self._removed_topics.get(spec.topic)
        if removed_at is not None:
            elapsed = time.monotonic() - removed_at
            cooldown = 1.0  # seconds
            if elapsed < cooldown:
                cooldown_remain = cooldown - elapsed
            del self._removed_topics[spec.topic]
        self.topics[spec.topic] = spec.to_meta(active=True)
        worker = DzipcSubscriber(
            self, spec, cooldown_remain,
            ipc_loader=load_dzipc,
            msg_resolver=resolve_message_class,
            image_encoder=encode_image_data,
            tf_encoder=DEFAULT_TF_SAMPLE_ENCODER,
            data_serializer=to_jsonable,
            field_getter=get_field,
            time_ms=now_ms,
        )
        self.subscribers[spec.topic] = worker
        worker.start()
        self.publish(
            {
                "kind": "topic_added",
                "topic": spec.to_meta(active=True),
                "timestamp_ms": now_ms(),
            }
        )

    def remove_topic(self, topic: str) -> None:
        worker = self.subscribers.pop(topic, None)
        if worker is not None:
            worker.stop_event.set()
            worker.join(timeout=3.0)
            is_alive = getattr(worker, "is_alive", None)
            if callable(is_alive) and is_alive():
                self.publish(
                    {
                        "kind": "error",
                        "topic": topic,
                        "message": "subscriber worker did not stop within timeout",
                        "timestamp_ms": now_ms(),
                    }
                )
            # Record removal for cooldown enforcement on re-add.
            # The C++ shm_sub_ipc destructor needs time to flush
            # shared-memory state before a new subscriber of a
            # different type can safely attach.
            self._removed_topics[topic] = time.monotonic()
            gc.collect()
        meta = self.topics.get(topic)
        if meta is not None:
            meta["active"] = False
        self.publish(
            {"kind": "topic_removed", "topic": topic, "timestamp_ms": now_ms()}
        )

    def robot_models_compat(self) -> List[Dict[str, Any]]:
        return [robot.to_legacy_robot_model() for robot in self.robot_displays.values()]

    def robot_display_configs(self) -> List[Dict[str, Any]]:
        for robot in self.robot_displays.values():
            self.ensure_robot_mesh_assets(robot)
        return [robot.to_config() for robot in self.robot_displays.values()]

    def robot_state_display_configs(self) -> List[Dict[str, Any]]:
        return [state.to_config() for state in self.robot_state_displays.values()]

    def apply_robot_display_config(self, config: Dict[str, Any], replace: bool = True, publish: bool = True) -> None:
        if replace:
            self.robot_displays.clear()
            self.robot_state_displays.clear()
        display_config = config.get("displays", {})
        if not isinstance(display_config, dict):
            display_config = {}
        robot_display_configs = config.get(
            "robot_displays",
            display_config.get("robot_displays", config.get("robot_models", [])),
        )
        robot_state_display_configs = config.get(
            "robot_state_displays",
            display_config.get("robot_state_displays", []),
        )
        robot_displays = [RobotDisplay.from_config(raw) for raw in component_normalize_robot_displays(robot_display_configs)]
        robot_state_displays = [
            RobotStateDisplay.from_config(raw, self.defaults) for raw in component_normalize_robot_state_displays(robot_state_display_configs)
        ]
        for robot in robot_displays:
            if not robot.urdf and robot.urdf_path:
                robot.set_urdf_path(robot.urdf_path, ROOT_DIR)
            self.ensure_robot_mesh_assets(robot)
            self.robot_displays[robot.id] = robot
        for state_display in robot_state_displays:
            self.robot_state_displays[state_display.id] = state_display
            robot = self.robot_displays.get(state_display.target_robot_id)
            if robot:
                robot.attach_robot_state_display(state_display)
        if publish:
            self.publish(
                {
                    "kind": "config",
                    "config": self.export_config(),
                    "config_path": self.config_path_str(),
                    "message_types": self.message_types,
                    "message_type_map": VISUALIZER_MESSAGE_ALIASES,
                    "timestamp_ms": now_ms(),
                }
            )

    def add_robot_display(self, raw: Dict[str, Any]) -> RobotDisplay:
        robot = RobotDisplay.from_config(raw)
        if not robot.urdf and robot.urdf_path:
            robot.set_urdf_path(robot.urdf_path, ROOT_DIR)
        self.ensure_robot_mesh_assets(robot)
        existing_states = [
            state_display
            for state_display in self.robot_state_displays.values()
            if state_display.target_robot_id == robot.id
        ]
        self.robot_displays[robot.id] = robot
        for state_display in existing_states:
            robot.attach_robot_state_display(state_display)
        self.publish({"kind": "robot_added", "robot": robot.to_config(), "timestamp_ms": now_ms()})
        return robot

    def ensure_robot_mesh_assets(self, robot: RobotDisplay) -> None:
        if not robot.urdf_text and robot.urdf_path:
            robot.set_urdf_path(robot.urdf_path, ROOT_DIR)
        if not robot.urdf_text:
            return
        raw_assets = robot.mesh_assets
        warnings = list(getattr(robot, "mesh_warnings", []) or [])
        if not raw_assets or any("path" not in asset for asset in raw_assets.values()):
            raw_assets, warnings = collect_urdf_mesh_assets(robot.urdf_text, robot.urdf_path, ROOT_DIR)

        manifest: Dict[str, Dict[str, Any]] = {}
        root = ROOT_DIR.resolve()
        for filename, asset in raw_assets.items():
            try:
                asset_path = Path(str(asset.get("path") or "")).expanduser().resolve()
                asset_path.relative_to(root)
                stat = asset_path.stat()
            except (OSError, ValueError):
                warnings.append(f"mesh not accessible: {filename}")
                continue
            suffix = asset_path.suffix.lower()
            if suffix not in {".obj", ".stl"}:
                warnings.append(f"unsupported mesh format for {filename}: {suffix}")
                continue
            digest = str(asset.get("hash") or hashlib.sha256(f"{asset_path}:{stat.st_size}:{stat.st_mtime_ns}".encode("utf-8")).hexdigest())
            key = digest[:24] + suffix
            self.robot_asset_paths[key] = asset_path
            manifest[str(filename)] = {
                "filename": str(filename),
                "url": f"/robot_assets/{key}/{quote(asset_path.name)}",
                "type": suffix.lstrip("."),
                "format": suffix.lstrip("."),
                "hash": digest,
                "size": stat.st_size,
                "byte_length": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
        robot.mesh_assets = manifest
        robot.mesh_warnings = warnings

    def add_robot_state_display(self, raw: Dict[str, Any]) -> RobotStateDisplay:
        state_display = RobotStateDisplay.from_config(raw, self.defaults)
        if not state_display.topic:
            raise ValueError("robot_state topic is required")
        if not state_display.target_robot_id or state_display.target_robot_id not in self.robot_displays:
            raise ValueError("robot_state requires an existing robot_id")
        self.robot_state_displays[state_display.id] = state_display
        self.robot_displays[state_display.target_robot_id].attach_robot_state_display(state_display)
        spec = TopicSpec.from_config(state_display.to_topic_config(), self.defaults)
        self.add_topic(spec)
        self.topics[state_display.topic]["display_type"] = "RobotState"
        self.topics[state_display.topic]["robot_id"] = state_display.target_robot_id
        self.topics[state_display.topic]["display_id"] = state_display.id
        self.publish({"kind": "robot_state_added", "robot_state": state_display.to_config(), "timestamp_ms": now_ms()})
        return state_display

    def remove_robot_display(self, robot_id: str) -> None:
        robot = self.robot_displays.pop(robot_id, None)
        if robot is None:
            return
        for state_id in list(self.robot_state_displays):
            if self.robot_state_displays[state_id].target_robot_id == robot_id:
                self.remove_robot_state_display(state_id)
        self.publish({"kind": "robot_removed", "robot_id": robot_id, "timestamp_ms": now_ms()})

    def remove_robot_state_display(self, state_id: str) -> None:
        state_display = self.robot_state_displays.pop(state_id, None)
        if state_display is None:
            return
        robot = self.robot_displays.get(state_display.target_robot_id)
        if robot:
            robot.robot_state_displays = [item for item in robot.robot_state_displays if item.id != state_id]
        if state_display.topic in self.subscribers:
            self.remove_topic(state_display.topic)
        self.publish({"kind": "robot_state_removed", "robot_state_id": state_id, "timestamp_ms": now_ms()})

    def export_config(self) -> Dict[str, Any]:
        return {
            "domain": self.defaults.get("domain", 1),
            "transport": self.defaults.get("transport", "socket"),
            "queue": self.defaults.get("queue", 10),
            "poll": self.defaults.get("poll", 0.03),
            "extra": self.defaults.get("extra", ""),
            "verbose": self.defaults.get("verbose", False),
            "topics": [worker.spec.to_config() for worker in self.subscribers.values()],
            "displays": {
                "robot_displays": self.robot_display_configs(),
                "robot_state_displays": self.robot_state_display_configs(),
            },
            "robot_displays": self.robot_display_configs(),
            "robot_state_displays": self.robot_state_display_configs(),
            "robot_models": self.robot_models_compat(),
        }

    def save_config(self, path: Optional[str] = None, config: Optional[Dict[str, Any]] = None) -> Path:
        target = self.resolve_config_path(path)
        if config is not None and not isinstance(config, dict):
            raise ValueError("config must be an object")
        payload = config if config is not None else self.export_config()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        return target

    def apply_config(self, config: Dict[str, Any], replace: bool = True) -> None:
        self.defaults.update(normalize_defaults(config, self.defaults))
        if replace or any(key in config for key in ("displays", "robot_displays", "robot_state_displays", "robot_models")):
            self.apply_robot_display_config(config, replace=replace, publish=False)
        if replace:
            for topic in list(self.subscribers):
                self.remove_topic(topic)
        for item in config.get("topics", []):
            self.add_topic(TopicSpec.from_config(item, self.defaults))
        self.publish(
            {
                "kind": "config",
                "config": self.export_config(),
                "config_path": self.config_path_str(),
                "message_types": self.message_types,
                "message_type_map": VISUALIZER_MESSAGE_ALIASES,
                "timestamp_ms": now_ms(),
            }
        )

    def handle_command(self, command: Dict[str, Any]) -> Dict[str, Any]:
        action = command.get("action")
        if action == "add_topic":
            spec = TopicSpec.from_config(command.get("topic", {}), self.defaults)
            self.add_topic(spec)
            return {
                "kind": "ack",
                "action": action,
                "ok": True,
                "topic": spec.to_meta(active=True),
            }
        if action == "remove_topic":
            topic = str(command.get("topic", "")).strip()
            if not topic:
                raise ValueError("topic is required")
            self.remove_topic(topic)
            return {"kind": "ack", "action": action, "ok": True, "topic": topic}
        if action == "apply_config":
            config = command.get("config", {})
            if not isinstance(config, dict):
                raise ValueError("config must be an object")
            self.apply_config(config, bool(command.get("replace", True)))
            return {
                "kind": "ack",
                "action": action,
                "ok": True,
                "config": self.export_config(),
            }
        if action == "set_robot_models":
            self.apply_robot_display_config({"robot_models": command.get("robot_models", [])}, replace=True, publish=False)
            self.publish(
                {
                    "kind": "config",
                    "config": self.export_config(),
                    "config_path": self.config_path_str(),
                    "message_types": self.message_types,
                    "message_type_map": VISUALIZER_MESSAGE_ALIASES,
                    "timestamp_ms": now_ms(),
                }
            )
            return {
                "kind": "ack",
                "action": action,
                "ok": True,
                "config": self.export_config(),
            }
        if action == "add_robot":
            robot = self.add_robot_display(command.get("robot", {}))
            return {"kind": "ack", "action": action, "ok": True, "robot": robot.to_config(), "config": self.export_config()}
        if action == "set_robot_urdf_path":
            robot_id = str(command.get("robot_id", "")).strip()
            robot = self.robot_displays.get(robot_id)
            if robot is None:
                raise ValueError("robot_id is required")
            result = robot.handle_urdf_path_request(str(command.get("path", "")), ROOT_DIR)
            self.ensure_robot_mesh_assets(robot)
            self.publish({"kind": "robot_updated", "robot": robot.to_config(), "timestamp_ms": now_ms()})
            return {"kind": "ack", "action": action, "ok": bool(result.get("ok")), **result, "config": self.export_config()}
        if action == "add_robot_state":
            state_display = self.add_robot_state_display(command.get("robot_state", {}))
            return {"kind": "ack", "action": action, "ok": True, "robot_state": state_display.to_config(), "config": self.export_config()}
        if action == "remove_robot":
            self.remove_robot_display(str(command.get("robot_id", "")))
            return {"kind": "ack", "action": action, "ok": True, "config": self.export_config()}
        if action == "remove_robot_state":
            self.remove_robot_state_display(str(command.get("robot_state_id", "")))
            return {"kind": "ack", "action": action, "ok": True, "config": self.export_config()}
        if action == "save_config":
            path = self.save_config(command.get("path"), command.get("config"))
            return {"kind": "ack", "action": action, "ok": True, "path": str(path)}
        if action == "load_config":
            config_path = self.resolve_config_path(command.get("path"))
            config = load_config_file(config_path)
            self.apply_config(config, replace=True)
            return {
                "kind": "ack",
                "action": action,
                "ok": True,
                "config": self.export_config(),
                "path": str(config_path),
            }
        raise ValueError(f"unknown action: {action}")


def parse_topic_spec(raw: str, defaults: Dict[str, Any]) -> TopicSpec:
    if ":" not in raw:
        raise ValueError(f"topic spec must be TOPIC:MessageType, got: {raw}")
    topic, msg_type = raw.split(":", 1)
    return TopicSpec.from_config(
        {"topic": topic.strip(), "msg_type": msg_type.strip()}, defaults
    )


def normalize_defaults(
    config: Dict[str, Any], fallback: Optional[Dict[str, Any]] = None
) -> Dict[str, Any]:
    fallback = fallback or {}
    transport = str(config.get("transport", fallback.get("transport", "socket")))
    if transport not in {"shm", "socket"}:
        transport = "socket"
    return {
        "domain": int(config.get("domain", fallback.get("domain", 1))),
        "transport": transport,
        "queue": int(config.get("queue", fallback.get("queue", 10))),
        "poll": normalize_poll_interval(
            config.get("poll", fallback.get("poll", 0.03))
        ),
        "extra": str(config.get("extra", fallback.get("extra", ""))),
        "verbose": bool(config.get("verbose", fallback.get("verbose", False))),
    }


def normalize_robot_models(raw: Any) -> List[Dict[str, Any]]:
    return component_normalize_robot_models(raw)


def normalize_robot_displays(raw: Any) -> List[Dict[str, Any]]:
    return component_normalize_robot_displays(raw)


def normalize_robot_state_displays(raw: Any) -> List[Dict[str, Any]]:
    return component_normalize_robot_state_displays(raw)


def load_config_file(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"config must be a JSON object: {path}")
    return data


def discover_message_types() -> List[str]:
    return VISUALIZER_DISPLAY_TYPES[:]


def resolve_message_class(ipc: Any, msg_type: str) -> Tuple[str, Any]:
    class_name = VISUALIZER_MESSAGE_ALIASES.get(msg_type, msg_type)
    allowed = set(VISUALIZER_MESSAGE_ALIASES) | set(VISUALIZER_MESSAGE_ALIASES.values())
    if msg_type not in allowed:
        raise ValueError(
            "Visualizer only supports std message display types: "
            + ", ".join(VISUALIZER_MESSAGE_TYPES)
        )
    return class_name, getattr(ipc, class_name)


def http_response(status: str, headers: Dict[str, str], body: bytes = b"") -> bytes:
    lines = [f"HTTP/1.1 {status}"]
    headers = {"Content-Length": str(len(body)), "Connection": "close", **headers}
    lines.extend(f"{key}: {val}" for key, val in headers.items())
    return ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body


def safe_static_path(path: str) -> Path:
    if path in ("", "/"):
        path = "/index.html"
    rel = Path(path.lstrip("/"))
    candidate = (WEB_DIR / rel).resolve()
    if not str(candidate).startswith(str(WEB_DIR.resolve())):
        return WEB_DIR / "index.html"
    return candidate


async def handle_client(
    reader: asyncio.StreamReader, writer: asyncio.StreamWriter, hub: WebHub
) -> None:
    try:
        request = await reader.readuntil(b"\r\n\r\n")
    except Exception:
        writer.close()
        return

    try:
        head = request.decode("utf-8", errors="replace")
        first_line = head.split("\r\n", 1)[0]
        method, path, _ = first_line.split(" ", 2)
        headers: Dict[str, str] = {}
        for line in head.split("\r\n")[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
    except Exception:
        writer.write(
            http_response(
                "400 Bad Request", {"Content-Type": "text/plain"}, b"bad request"
            )
        )
        await writer.drain()
        writer.close()
        return

    if (
        method == "GET"
        and path == "/ws"
        and headers.get("upgrade", "").lower() == "websocket"
    ):
        key = headers.get("sec-websocket-key", "")
        accept = base64.b64encode(
            hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest()
        ).decode("ascii")
        writer.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode("ascii")
        )
        await writer.drain()
        await hub.add_client(writer)
        try:
            while True:
                frame = await read_ws_frame(reader)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 9:
                    writer.write(make_frame(payload, opcode=10))
                    await writer.drain()
                elif opcode == 1:
                    try:
                        command = json.loads(payload.decode("utf-8"))
                        response = hub.handle_command(command)
                    except Exception as exc:
                        response = {
                            "kind": "ack",
                            "ok": False,
                            "message": str(exc),
                            "timestamp_ms": now_ms(),
                        }
                    await hub.send(writer, response)
        except Exception:
            pass
        await hub.remove_client(writer)
        return

    if method != "GET":
        writer.write(
            http_response(
                "405 Method Not Allowed",
                {"Content-Type": "text/plain"},
                b"method not allowed",
            )
        )
        await writer.drain()
        writer.close()
        return

    request_path = path.split("?", 1)[0]
    if request_path.startswith("/robot_assets/"):
        parts = request_path.split("/", 3)
        asset_key = unquote(parts[2]) if len(parts) >= 3 else ""
        asset_path = hub.robot_asset_paths.get(asset_key)
        if asset_path is None or not asset_path.is_file():
            writer.write(
                http_response("404 Not Found", {"Content-Type": "text/plain"}, b"not found")
            )
            await writer.drain()
            writer.close()
            return
        body = asset_path.read_bytes()
        content_type = mimetypes.guess_type(asset_path.name)[0] or "application/octet-stream"
        writer.write(
            http_response(
                "200 OK",
                {
                    "Content-Type": content_type,
                    "ETag": f'"{asset_key}"',
                    "Cache-Control": "public, max-age=31536000, immutable",
                },
                body,
            )
        )
        await writer.drain()
        writer.close()
        return

    file_path = safe_static_path(request_path)
    if not file_path.exists() or not file_path.is_file():
        writer.write(
            http_response("404 Not Found", {"Content-Type": "text/plain"}, b"not found")
        )
        await writer.drain()
        writer.close()
        return

    body = file_path.read_bytes()
    content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
    writer.write(http_response("200 OK", {"Content-Type": content_type}, body))
    await writer.drain()
    writer.close()


def local_ip_hint() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except Exception:
        return "127.0.0.1"


def print_startup_links(host: str, port: int, web_dir: Path, listen_addr: str) -> None:
    forwarded_url = f"http://127.0.0.1:{port}"
    print(f"[dzipc-web] serving {web_dir} on {listen_addr}", flush=True)
    print(f"[dzipc-web] open: {forwarded_url}", flush=True)
    print(
        f"\033[32m[dzipc-web] VS Code Remote: Ctrl+Click {forwarded_url}\033[0m",
        flush=True,
    )
    if host not in {"127.0.0.1", "localhost"}:
        print(
            f"\033[32m[dzipc-web] LAN:  http://{local_ip_hint()}:{port}\033[0m",
            flush=True,
        )


def port_in_use_hint(port: int) -> str:
    return (
        f"[dzipc-web] Port {port} is already in use. "
        f"Run `lsof -i :{port}` to find the process, then `kill <PID>`; "
        f"or start this bridge with `--port {port + 1}`."
    )


async def run_server(args: argparse.Namespace) -> None:
    config_path = Path(args.config).expanduser()
    if not config_path.is_absolute():
        config_path = (ROOT_DIR / config_path).resolve()
    file_config = load_config_file(config_path)
    defaults = normalize_defaults(
        {
            **file_config,
            "domain": (
                args.domain if args.domain is not None else file_config.get("domain", 1)
            ),
            "transport": args.transport or file_config.get("transport", "socket"),
            "queue": (
                args.queue if args.queue is not None else file_config.get("queue", 10)
            ),
            "poll": (
                args.poll if args.poll is not None else file_config.get("poll", 0.03)
            ),
            "extra": (
                args.extra if args.extra is not None else file_config.get("extra", "")
            ),
            "verbose": args.verbose or bool(file_config.get("verbose", False)),
        }
    )
    robot_configs = normalize_robot_displays(file_config.get("robot_displays", file_config.get("robot_models", [])))
    robot_state_configs = normalize_robot_state_displays(file_config.get("robot_state_displays", []))
    defaults["robot_displays"] = robot_configs
    defaults["robot_state_displays"] = robot_state_configs
    defaults["robot_models"] = normalize_robot_models(robot_configs)

    hub = WebHub(defaults, config_path)
    broadcast_task = asyncio.create_task(hub.broadcast_loop())

    workers: List[threading.Thread] = []
    if args.demo:
        worker = DemoPublisher(hub, args.demo_period, time_ms=now_ms)
        worker.start()
        workers.append(worker)
        hub.demo_workers["demo"] = worker

    if args.load_config:
        hub.apply_config(file_config, replace=False)

    for raw in args.topic:
        hub.add_topic(parse_topic_spec(raw, defaults))

    try:
        server = await asyncio.start_server(
            lambda r, w: handle_client(r, w, hub),
            args.host,
            args.port,
            reuse_address=True,
        )
    except OSError as e:
        print(
            f"\033[91m[dzipc-web] ERROR: Cannot bind to {args.host}:{args.port}: {e}\033[0m",
            flush=True,
        )
        print(port_in_use_hint(args.port), flush=True)
        sys.exit(1)
    addr = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print_startup_links(args.host, args.port, WEB_DIR, addr)

    try:
        async with server:
            await server.serve_forever()
    finally:
        server.close()
        await server.wait_closed()
        await hub.shutdown()
        broadcast_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await broadcast_task


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Web dashboard for dzIPC topics")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument(
        "--config", default=str(DEFAULT_CONFIG), help="JSON config path"
    )
    parser.add_argument(
        "--load-config",
        action="store_true",
        help="Load topics from --config on startup",
    )
    parser.add_argument(
        "--topic",
        action="append",
        default=[],
        help="Subscribe to a dzIPC topic as TOPIC:MessageType, e.g. robot_state:RobotState",
    )
    parser.add_argument("--domain", type=int, default=None)
    parser.add_argument("--queue", type=int, default=None)
    parser.add_argument("--transport", choices=["shm", "socket"], default=None)
    parser.add_argument("--extra", default=None)
    parser.add_argument("--poll", type=float, default=None)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--demo", action="store_true", help="Publish demo data without importing dzipc"
    )
    parser.add_argument("--demo-period", type=float, default=0.1)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        asyncio.run(run_server(args))
    except KeyboardInterrupt:
        print("\n[dzipc-web] stopped")


if __name__ == "__main__":
    main()
