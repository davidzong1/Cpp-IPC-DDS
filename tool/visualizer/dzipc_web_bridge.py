#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import ctypes
import gc
import glob
import hashlib
import importlib.machinery
import json
import math
import mimetypes
import os
import queue
import random
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

ROOT_DIR = Path(__file__).resolve().parents[2]
WEB_DIR = Path(__file__).resolve().parent / "web"
DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.json"
VISUALIZER_DIR = Path(__file__).resolve().parent

if str(VISUALIZER_DIR) not in sys.path:
    sys.path.insert(0, str(VISUALIZER_DIR))

from component.robot import (  # noqa: E402
    RobotDisplay,
    RobotStateDisplay,
    normalize_robot_displays as component_normalize_robot_displays,
    normalize_robot_models as component_normalize_robot_models,
    normalize_robot_state_displays as component_normalize_robot_state_displays,
)

VISUALIZER_MESSAGE_ALIASES = {
    "RawMessage": "StdRawMessage",
    "Pose": "StdPose",
    "Path": "StdPath",
    "PointCloud": "StdPointCloud",
    "Marker": "StdMarker",
    "Image": "StdImage",
    "RobotState": "RobotState",
}

VISUALIZER_MESSAGE_TYPES = list(VISUALIZER_MESSAGE_ALIASES.keys())
VISUALIZER_DISPLAY_TYPES = VISUALIZER_MESSAGE_TYPES[:-1] + ["Robot", "RobotState"]

BINARY_MAGIC = b"DZPC"
BINARY_VERSION = 1
BINARY_POINT_CLOUD = 1
BINARY_POINT_CLOUD_HAS_COLORS = 1
BINARY_HEADER = struct.Struct("<4sBBHIIII")
MIN_POLL_INTERVAL_S = 0.005
SUBSCRIBER_FIRST_SAMPLE_TIMEOUT_S = 3.0
SUBSCRIBER_RETRY_BACKOFF_MAX_S = 5.0
SHM_IDLE_RECONNECT_MIN_S = 2.0
SHM_IDLE_RECONNECT_POLL_MULTIPLIER = 200.0


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


def normalize_poll_interval(value: Any, fallback: float = 0.03) -> float:
    # `poll` is used on no-data paths; clamp it so poll=0 cannot busy-loop.
    try:
        poll = float(value)
    except (TypeError, ValueError):
        poll = fallback
    if not math.isfinite(poll):
        poll = fallback
    return max(MIN_POLL_INTERVAL_S, poll)


def to_jsonable(value: Any, depth: int = 0) -> Any:
    if depth > 16:
        return "<max-depth>"
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


def get_field(value: Any, name: str, default: Any = None) -> Any:
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def vector_xyz(value: Any) -> Optional[Tuple[float, float, float]]:
    data = get_field(value, "data")
    if isinstance(data, (list, tuple)) and len(data) >= 3:
        return float(data[0]), float(data[1]), float(data[2])
    x = get_field(value, "x")
    y = get_field(value, "y")
    z = get_field(value, "z", 0.0)
    if x is None or y is None:
        return None
    return float(x), float(y), float(z)


def color_rgba(value: Any) -> Tuple[float, float, float, float]:
    return (
        float(get_field(value, "r", 1.0)),
        float(get_field(value, "g", 1.0)),
        float(get_field(value, "b", 1.0)),
        float(get_field(value, "a", 1.0)),
    )


def encode_point_cloud_binary(
    msg_obj: Any, sample_id: int
) -> Tuple[Dict[str, Any], bytes]:
    points = get_field(msg_obj, "points", []) or []
    colors = get_field(msg_obj, "colors", []) or []
    if not hasattr(points, "__len__"):
        points = list(points)
    if not hasattr(colors, "__len__"):
        colors = list(colors)
    point_count = len(points)
    has_colors = len(colors) == point_count and point_count > 0
    flags = BINARY_POINT_CLOUD_HAS_COLORS if has_colors else 0
    header_size = BINARY_HEADER.size
    point_bytes = point_count * 3 * 4
    color_bytes = point_count * 4 * 4 if has_colors else 0
    payload = bytearray(header_size + point_bytes + color_bytes)
    BINARY_HEADER.pack_into(
        payload,
        0,
        BINARY_MAGIC,
        BINARY_VERSION,
        BINARY_POINT_CLOUD,
        flags,
        sample_id,
        point_count,
        point_count if has_colors else 0,
        0,
    )
    offset = header_size
    for point in points:
        xyz = vector_xyz(point) or (0.0, 0.0, 0.0)
        struct.pack_into("<fff", payload, offset, *xyz)
        offset += 12
    if has_colors:
        for color in colors:
            struct.pack_into("<ffff", payload, offset, *color_rgba(color))
            offset += 16

    metadata = {
        "encoding": "dzpc.pointcloud.v1",
        "sample_id": sample_id,
        "point_count": point_count,
        "has_colors": has_colors,
        "byte_length": len(payload),
    }
    return metadata, bytes(payload)


def lightweight_point_cloud_data(msg_obj: Any) -> Dict[str, Any]:
    return {
        "header": to_jsonable(get_field(msg_obj, "header", {})),
        "channel_names": to_jsonable(get_field(msg_obj, "channel_names", [])),
        "binary_points": True,
    }


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
        self.apply_robot_display_config(defaults, replace=True, publish=False)
        # Track recently removed topics with timestamps to enforce
        # a cooldown period before re-add, ensuring C++ SHM cleanup
        # completes.  Key: topic_name, Value: removal timestamp (monotonic).
        self._removed_topics: Dict[str, float] = {}

    def publish(self, event: Dict[str, Any]) -> None:
        self.events.put(event)

    async def add_client(self, writer: asyncio.StreamWriter) -> None:
        self.clients.add(writer)
        await self.send(
            writer,
            {
                "kind": "hello",
                "topics": list(self.topics.values()),
                "config": self.export_config(),
                "message_types": self.message_types,
                "message_type_map": VISUALIZER_MESSAGE_ALIASES,
            },
        )

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
                        state_display.handle_joint_state_sample(event.get("data", {}))
                        event["robot_state_display_id"] = state_display.id
                        event["robot_id"] = state_display.target_robot_id
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
        worker = DzipcSubscriber(self, spec, cooldown_remain)
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
            if worker.is_alive():
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
                    "message_types": self.message_types,
                    "message_type_map": VISUALIZER_MESSAGE_ALIASES,
                    "timestamp_ms": now_ms(),
                }
            )

    def add_robot_display(self, raw: Dict[str, Any]) -> RobotDisplay:
        robot = RobotDisplay.from_config(raw)
        if not robot.urdf and robot.urdf_path:
            robot.set_urdf_path(robot.urdf_path, ROOT_DIR)
        self.robot_displays[robot.id] = robot
        self.publish({"kind": "robot_added", "robot": robot.to_config(), "timestamp_ms": now_ms()})
        return robot

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

    def save_config(self, path: Optional[str] = None) -> Path:
        target = Path(path).expanduser() if path else self.config_path
        if not target.is_absolute():
            target = (ROOT_DIR / target).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            json.dumps(self.export_config(), indent=2, ensure_ascii=False) + "\n",
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
            path = self.save_config(command.get("path"))
            return {"kind": "ack", "action": action, "ok": True, "path": str(path)}
        if action == "load_config":
            config = load_config_file(self.config_path)
            self.apply_config(config, replace=True)
            return {
                "kind": "ack",
                "action": action,
                "ok": True,
                "config": self.export_config(),
            }
        raise ValueError(f"unknown action: {action}")


@dataclass
class TopicSpec:
    topic: str
    msg_type: str
    domain: int
    queue: int
    transport: str
    poll: float
    extra: str = ""
    verbose: bool = False

    @classmethod
    def from_config(cls, raw: Dict[str, Any], defaults: Dict[str, Any]) -> "TopicSpec":
        topic = str(raw.get("topic") or raw.get("name") or "").strip()
        msg_type = str(raw.get("msg_type") or raw.get("type") or "").strip()
        if not topic or not msg_type:
            raise ValueError("topic and msg_type are required")
        transport = str(
            raw.get("transport", defaults.get("transport", "socket"))
        ).strip()
        if transport not in {"shm", "socket"}:
            raise ValueError("transport must be shm or socket")
        return cls(
            topic=topic,
            msg_type=msg_type,
            domain=int(raw.get("domain", defaults.get("domain", 1))),
            queue=int(raw.get("queue", defaults.get("queue", 10))),
            transport=transport,
            poll=normalize_poll_interval(raw.get("poll", defaults.get("poll", 0.03))),
            extra=str(raw.get("extra", defaults.get("extra", ""))),
            verbose=bool(raw.get("verbose", defaults.get("verbose", False))),
        )

    def to_config(self) -> Dict[str, Any]:
        return {
            "topic": self.topic,
            "msg_type": self.msg_type,
            "domain": self.domain,
            "queue": self.queue,
            "transport": self.transport,
            "poll": self.poll,
            "extra": self.extra,
            "verbose": self.verbose,
        }

    def to_meta(self, active: bool = True) -> Dict[str, Any]:
        meta = self.to_config()
        meta.update({"name": self.topic, "type": self.msg_type, "active": active})
        return meta


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


class DzipcSubscriber(threading.Thread):
    def __init__(self, hub: WebHub, spec: TopicSpec, cooldown_remain: float = 0.0) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.spec = spec
        self.stop_event = threading.Event()
        self.sample_seq = 0
        self._cooldown_remain = cooldown_remain

    @staticmethod
    def _sanitize_for_shm(topic: str) -> str:
        """Mimic C++ sanitize_topic_name() character-level sanitisation.

        Replaces every character that is NOT alphanumeric, '_', '-', or '.'
        with '_', matching the C++ implementation in name_operator.cc.
        This does NOT add the "dz_ipc_" prefix or "_topic" suffix.
        """
        result: List[str] = []
        for ch in topic:
            if ch.isalnum() or ch in ('_', '-', '.'):
                result.append(ch)
            else:
                result.append('_')
        return ''.join(result)

    @staticmethod
    def _clean_shm_for_topic(topic: str) -> None:
        """Remove stale SHM connection files for *topic*.

        This is a last-resort offline cleanup helper.  Normal display
        removal must not call it because display removal does not own the
        publisher's live SHM segments.  We match against the
        C++-sanitised topic name to cover control-plane, queue, waiter,
        and counter SHM segments when no publisher is running.

        IMPORTANT: Do NOT call this while a publisher is still running on
        the same topic — that would delete the publisher's live SHM files,
        creating a split-brain where publisher writes to old (unlinked)
        memory and the new subscriber reads from fresh (empty) files.
        """
        sanitised = DzipcSubscriber._sanitize_for_shm(topic)
        for pat in (
            f"/dev/shm/__IPC_SHM__*__dz_ipc__{sanitised}*",
            f"/dev/shm/__IPC_SHM__*__dz_ipc_{sanitised}*",
        ):
            for path in glob.glob(pat):
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def _retry_delay(self, attempt: int) -> float:
        return min(SUBSCRIBER_RETRY_BACKOFF_MAX_S, 0.5 * (2 ** min(attempt, 4)))

    def _idle_reconnect_timeout(self) -> float:
        if self.spec.transport != "shm":
            return 0.0
        return max(
            SHM_IDLE_RECONNECT_MIN_S,
            self.spec.poll * SHM_IDLE_RECONNECT_POLL_MULTIPLIER,
        )

    def _cleanup_session(
        self,
        sub: Any,
        topic_data: Any,
        template: Any,
        msg_cls: Any,
        ipc: Any,
    ) -> Tuple[None, None, None, None, None]:
        del sub
        del topic_data
        del template
        del msg_cls
        del ipc
        for _ in range(3):
            gc.collect()
        return None, None, None, None, None

    def run(self) -> None:
        # Outer retry loop: if the SHM ring-buffer has stale reader state
        # from a previous (possibly different-type) subscriber, the new
        # subscriber may fail to receive data even though InitChannel
        # succeeds.  Keep retrying until the display is removed.

        # Honour the cooldown period (deferred here from add_topic()
        # to keep the asyncio event loop responsive).
        if self._cooldown_remain > 0:
            time.sleep(self._cooldown_remain)
            gc.collect()

        attempt = 0
        while not self.stop_event.is_set():
            if self.stop_event.is_set():
                return
            sub = None
            ipc = None
            topic_data = None
            template = None
            msg_cls = None
            resolved_msg_type = ""
            try:
                # NOTE: Do NOT call _clean_shm_for_topic() here or from
                # normal display removal. If a publisher is already
                # running, deleting its live SHM files creates
                # split-brain: the publisher keeps writing to old
                # unlinked memory while the subscriber creates fresh
                # files and never receives data.
                ipc = load_dzipc()
                resolved_msg_type, msg_cls = resolve_message_class(
                    ipc, self.spec.msg_type
                )
                template = msg_cls()
                topic_data = ipc.make_topic_data(template)
                ipc_type = (
                    ipc.IPC_SHM
                    if self.spec.transport == "shm"
                    else ipc.IPC_SOCKET
                )
                sub = ipc.SubscriberIPCPtrMake(
                    topic_data,
                    self.spec.topic,
                    self.spec.domain,
                    self.spec.queue,
                    ipc_type,
                    self.spec.verbose,
                )
                sub.InitChannel(self.spec.extra)
            except Exception as exc:
                self.hub.publish({
                    "kind": "error", "topic": self.spec.topic,
                    "message": f"init failed (attempt {attempt + 1}): {exc}",
                    "timestamp_ms": now_ms(),
                })
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(self._retry_delay(attempt)):
                    return
                attempt += 1
                continue

            # Wait for the first sample to confirm the channel is healthy.
            # If we just re-created a subscriber for a topic whose previous
            # (wrong-type) incarnation left stale reader state in the SHM
            # ring buffer, try_get may never return data.
            first_sample_deadline = (
                time.monotonic() + SUBSCRIBER_FIRST_SAMPLE_TIMEOUT_S
            )
            got_data = False
            while time.monotonic() < first_sample_deadline:
                if self.stop_event.is_set():
                    # Shutting down during health check – bail out
                    sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                        sub, topic_data, template, msg_cls, ipc
                    )
                    return
                try:
                    ok, out = sub.try_get(topic_data)
                    if ok:
                        got_data = True
                        # Process this first sample immediately so the
                        # frontend sees data without further delay.
                        self._process_sample(
                            sub, topic_data, out, msg_cls,
                            resolved_msg_type,
                        )
                        break
                    if self.stop_event.wait(self.spec.poll):
                        sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                            sub, topic_data, template, msg_cls, ipc
                        )
                        return
                except Exception as exc:
                    self.hub.publish({
                        "kind": "error", "topic": self.spec.topic,
                        "message": f"read failed during health check: {exc}",
                        "timestamp_ms": now_ms(),
                    })
                    if self.stop_event.wait(self.spec.poll):
                        sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                            sub, topic_data, template, msg_cls, ipc
                        )
                        return

            if not got_data:
                self.hub.publish({
                    "kind": "error", "topic": self.spec.topic,
                    "message": f"no data received (attempt {attempt + 1}), retrying",
                    "timestamp_ms": now_ms(),
                })
                # Destroy the C++ subscriber and give SHM a chance to
                # settle before the next attempt.
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(max(2.0, self._retry_delay(attempt))):
                    return
                attempt += 1
                continue

            # --- healthy main loop ---
            attempt = 0
            last_sample_time = time.monotonic()
            idle_reconnect_timeout = self._idle_reconnect_timeout()
            try:
                while not self.stop_event.is_set():
                    try:
                        ok, out = sub.try_get(topic_data)
                        if not ok:
                            if (
                                idle_reconnect_timeout > 0
                                and time.monotonic() - last_sample_time
                                > idle_reconnect_timeout
                            ):
                                self.hub.publish({
                                    "kind": "error",
                                    "topic": self.spec.topic,
                                    "message": (
                                        "no samples received for "
                                        f"{idle_reconnect_timeout:.1f}s; "
                                        "rebuilding subscriber"
                                    ),
                                    "timestamp_ms": now_ms(),
                                })
                                break
                            if self.stop_event.wait(self.spec.poll):
                                return
                            continue
                        last_sample_time = time.monotonic()
                        self._process_sample(
                            sub, topic_data, out, msg_cls,
                            resolved_msg_type,
                        )
                    except Exception as exc:
                        self.hub.publish({
                            "kind": "error", "topic": self.spec.topic,
                            "message": str(exc), "timestamp_ms": now_ms(),
                        })
                        if self.stop_event.wait(
                            timeout=max(self.spec.poll, 0.2)
                        ):
                            return
            finally:
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(0.3):
                    return

    def _process_sample(
        self, sub: Any, topic_data: Any, out: Any,
        msg_cls: Any, resolved_msg_type: str,
    ) -> None:
        """Decode and publish one sample from the subscriber."""
        msg_obj = out.topic() if out is not None else topic_data.topic()
        if hasattr(msg_cls, "from_generic") and hasattr(msg_obj, "field_count"):
            msg_obj = msg_cls.from_generic(msg_obj)
        self.sample_seq = (self.sample_seq + 1) & 0xFFFFFFFF
        event: Dict[str, Any] = {
            "kind": "sample",
            "topic": self.spec.topic,
            "msg_type": self.spec.msg_type,
            "resolved_msg_type": resolved_msg_type,
            "transport": self.spec.transport,
            "domain": self.spec.domain,
            "queue": self.spec.queue,
            "extra": self.spec.extra,
            "timestamp_ms": now_ms(),
        }
        if resolved_msg_type == "StdPointCloud":
            metadata, binary_payload = encode_point_cloud_binary(
                msg_obj, self.sample_seq
            )
            event.update({
                "data": lightweight_point_cloud_data(msg_obj),
                "binary": metadata,
                "binary_payload": binary_payload,
            })
        elif resolved_msg_type == "StdImage":
            metadata = encode_image_data(msg_obj)
            event.update({
                "data": {
                    "header": to_jsonable(get_field(msg_obj, "header", {})),
                    "width": metadata["width"],
                    "height": metadata["height"],
                    "encoding": metadata["image_encoding"],
                    "step": metadata["step"],
                    "data_length": metadata["data_length"],
                },
                "binary": metadata,
            })
        else:
            event["data"] = to_jsonable(msg_obj)
        self.hub.publish(event)


class DemoPublisher(threading.Thread):
    def __init__(self, hub: WebHub, period_s: float) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.period_s = period_s
        self.stop_event = threading.Event()
        self.x = 0.0
        self.y = 0.0
        self.seq = 0

    def run(self) -> None:
        while not self.stop_event.is_set():
            self.seq += 1
            self.x += random.uniform(-0.08, 0.16)
            self.y += random.uniform(-0.10, 0.10)
            data = {
                "name": "demo_robot",
                "current_pose": {
                    "x": round(self.x, 4),
                    "y": round(self.y, 4),
                    "z": 0.0,
                    "frame_id": "map",
                },
                "battery": round(72.0 + 8.0 * math.sin(self.seq / 20.0), 3),
                "temperature": round(36.0 + 3.0 * math.sin(self.seq / 13.0), 3),
                "state": "RUNNING" if self.seq % 80 < 65 else "IDLE",
            }
            self.hub.publish(
                {
                    "kind": "sample",
                    "topic": "demo_robot_state",
                    "msg_type": "RobotState",
                    "transport": "demo",
                    "domain": 0,
                    "timestamp_ms": now_ms(),
                    "data": data,
                }
            )
            time.sleep(self.period_s)


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

    file_path = safe_static_path(path.split("?", 1)[0])
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
        worker = DemoPublisher(hub, args.demo_period)
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
