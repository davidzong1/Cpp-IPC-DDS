#!/usr/bin/env python3
"""
dzplot — dzIPC time-series visualization dashboard.

Serves a web UI for plotting dzIPC topic data from two sources:
  --bag     Offline: replay .bag files recorded by dzipc_log
  --sniff   Online:  subscribe to live dzIPC topics via SHM/socket

Architecture:
  Browser <--WebSocket--> dzplot.py (asyncio)
                            ├── BagReplaySource  (offline, file → timed replay)
                            └── LiveSniffSource  (online, dzIPC subscriber)

Queue & rate control reuses the team's shared modules:
  - tools/dzviz/transport/bounded_queue.py  → BoundedPubQueue (thread-safe)
  - tools/dzviz/component/rate_controller.py → RateController (backpressure)
  - tools/dzviz/transport/backpressure.py   → BackpressureController (zones)

Design constraints (per spec):
  - Render at configurable fps (default 60, max 120, left-side UI control)
  - Receive up to 1000 Hz from IPC channel
  - Batch multiple received samples into a single WebSocket frame
  - >1/2 watermark triggers progressive adaptive backpressure (<=1000Hz)
  - Recovery when queue drops below low watermark
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import hashlib
import json
import math
import mimetypes
import os
import struct
import sys
import threading
import time
import traceback
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple, Union

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
ROOT_DIR = Path(__file__).resolve().parents[2]
WEB_DIR = Path(__file__).resolve().parent / "web"
DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.json"
TOOLS_DIR = Path(__file__).resolve().parents[1]  # tools/

# Ensure tools/dzviz is importable for shared transport/rate modules.
# This is a repo-local path with no external alternative — safe to prepend.
_DZVIZ_DIR = TOOLS_DIR / "dzviz"
if str(_DZVIZ_DIR) not in sys.path:
    sys.path.insert(0, str(_DZVIZ_DIR))

# dzipc / _dzipc_core fallback paths: only append as a last resort so
# an explicit PYTHONPATH (e.g. fresh build_py/python/) or an installed
# package takes priority.  NEVER prepend here — that would override
# whatever the user or the leader's test harness explicitly configured.
for _dzipc_candidate in (
    str(ROOT_DIR / "python"),
    str(ROOT_DIR / "local" / "lib" / "python"),
):
    if _dzipc_candidate not in sys.path:
        sys.path.append(_dzipc_candidate)

# Shared team modules
from transport.bounded_queue import BoundedPubQueue, QueueWatermark, BackpressureSignal  # noqa: E402
from component.rate_controller import RateController, RateControllerConfig  # noqa: E402
from transport.backpressure import BackpressureController, DEFAULT_ZONE_POLICIES  # noqa: E402


# ---------------------------------------------------------------------------
# ROS Bag v2.0 constants (per docs/dzipc_log.md)
# ---------------------------------------------------------------------------
BAG_MAGIC = b"#ROSBAG V2.0\n"
OP_MESSAGE_DATA = 0x02
OP_BAG_HEADER = 0x03
OP_INDEX_DATA = 0x04
OP_CHUNK = 0x05
OP_CHUNK_INFO = 0x06
OP_CONNECTION = 0x07


def now_ms() -> int:
    return int(time.time() * 1000)


def now_ns() -> int:
    return int(time.time() * 1e9)


# ---------------------------------------------------------------------------
# ROS Bag v2.0 Reader (offline replay source)
# ---------------------------------------------------------------------------

@dataclass
class BagConnection:
    conn_id: int
    topic: str
    type_name: str
    md5sum: str = ""
    message_definition: str = ""


@dataclass
class ChunkInfo:
    conn_id: int
    count: int
    start_time_ns: int
    end_time_ns: int


class BagReader:
    """Parse ROS bag v2.0 files and yield (topic, timestamp_ns, raw_bytes).

    Format per docs/dzipc_log.md:
      #ROSBAG V2.0\\n header, no compression chunks.
      Record types: Header(0x03), Connection(0x07), Chunk(0x05),
      MessageData(0x02), IndexData(0x04), ChunkInfo(0x06).

    NOTE: Currently does a full linear scan on each iter_messages() call.
    For large bags (>100MB), prefer using the file-tail index (index_pos
    in the bag header) to seek directly to chunks — this is a planned
    performance optimization (see M1 in dzplot review findings).
    """

    _compressed_warned: bool = False

    def __init__(self, path: str) -> None:
        self.path = Path(path)
        self.connections: Dict[int, BagConnection] = {}
        self.chunk_infos: List[ChunkInfo] = []
        self._fh: Any = None

    def open(self) -> None:
        self._fh = open(self.path, "rb")
        header = self._fh.read(13)
        if header != BAG_MAGIC:
            raise ValueError(f"Not a ROS bag v2.0 file: {self.path}")
        # Scan file to build connection index
        offset = 13
        file_size = os.fstat(self._fh.fileno()).st_size
        while offset + 8 <= file_size:
            self._fh.seek(offset)
            header_len_raw = self._fh.read(4)
            if len(header_len_raw) < 4:
                break
            header_len = struct.unpack("<I", header_len_raw)[0]
            data_len_raw = self._fh.read(4)
            if len(data_len_raw) < 4:
                break
            data_len = struct.unpack("<I", data_len_raw)[0]
            if offset + 4 + 4 + header_len + data_len > file_size:
                break
            header_fields = self._fh.read(header_len)
            data = self._fh.read(data_len)
            fields = self._parse_header_fields(header_fields)
            op = int(fields.get("op", 0))
            if op == OP_CONNECTION:
                conn_id = int(fields.get("conn", 0))
                topic = str(fields.get("topic", ""))
                type_name = str(fields.get("type", ""))
                md5 = ""
                msg_def = ""
                if "md5sum" in fields:
                    md5 = str(fields["md5sum"])
                if "message_definition" in fields:
                    msg_def = str(fields["message_definition"])
                self.connections[conn_id] = BagConnection(
                    conn_id=conn_id, topic=topic, type_name=type_name,
                    md5sum=md5, message_definition=msg_def,
                )
            elif op == OP_CHUNK_INFO:
                self.chunk_infos.append(ChunkInfo(
                    conn_id=int(fields.get("conn", 0)),
                    count=int(fields.get("count", 0)),
                    start_time_ns=int(fields.get("start_time", 0)),
                    end_time_ns=int(fields.get("end_time", 0)),
                ))
            offset += 4 + 4 + header_len + data_len
        self._fh.seek(13)

    @staticmethod
    def _parse_header_fields(data: bytes) -> Dict[str, Any]:
        """Parse rosbag v2.0 variable-length key=value header fields."""
        result: Dict[str, Any] = {}
        pos = 0
        while pos + 4 <= len(data):
            field_len = struct.unpack("<I", data[pos: pos + 4])[0]
            pos += 4
            if field_len == 0 or pos + field_len > len(data):
                break
            eq_idx = data.find(b"=", pos, pos + field_len)
            if eq_idx < 0:
                break
            key = data[pos:eq_idx].decode("utf-8", errors="replace")
            val_start = eq_idx + 1
            val_bytes = data[val_start: pos + field_len]
            if key in ("op", "conn", "ver", "count", "conn_count", "chunk_count"):
                try:
                    result[key] = struct.unpack("<I", val_bytes)[0]
                except struct.error:
                    result[key] = val_bytes
            elif key in ("index_pos", "start_time", "end_time", "chunk_pos", "time"):
                try:
                    result[key] = struct.unpack("<Q", val_bytes)[0]
                except struct.error:
                    result[key] = val_bytes
            elif key in ("compression",):
                result[key] = val_bytes.decode("utf-8", errors="replace")
            else:
                try:
                    result[key] = val_bytes.decode("utf-8", errors="replace")
                except UnicodeDecodeError:
                    result[key] = val_bytes
            pos += field_len
        return result

    def iter_messages(self) -> Any:
        """Generator yielding (topic, timestamp_ns, data_bytes) tuples."""
        self._fh.seek(13)
        offset = 13
        file_size = os.fstat(self._fh.fileno()).st_size
        while offset + 8 <= file_size:
            self._fh.seek(offset)
            header_len_raw = self._fh.read(4)
            if len(header_len_raw) < 4:
                break
            header_len = struct.unpack("<I", header_len_raw)[0]
            data_len_raw = self._fh.read(4)
            if len(data_len_raw) < 4:
                break
            data_len = struct.unpack("<I", data_len_raw)[0]
            if offset + 4 + 4 + header_len + data_len > file_size:
                break
            header_fields = self._fh.read(header_len)
            data = self._fh.read(data_len)
            fields = self._parse_header_fields(header_fields)
            op = int(fields.get("op", 0))

            if op == OP_CHUNK:
                compression = str(fields.get("compression", "none"))
                if compression != "none":
                    if not BagReader._compressed_warned:
                        BagReader._compressed_warned = True
                        print(f"[dzplot] WARNING: compressed chunk (compression={compression}) "
                              f"in {self.path}; dzipc_log always uses 'none'. Skipping chunk.",
                              flush=True)
                    offset += 4 + 4 + header_len + data_len
                    continue
                inner_pos = 0
                while inner_pos + 8 <= len(data):
                    ih_len = struct.unpack("<I", data[inner_pos: inner_pos + 4])[0]
                    id_len = struct.unpack("<I", data[inner_pos + 4: inner_pos + 8])[0]
                    inner_pos += 8
                    if inner_pos + ih_len + id_len > len(data):
                        break
                    ih = data[inner_pos: inner_pos + ih_len]
                    ib = data[inner_pos + ih_len: inner_pos + ih_len + id_len]
                    ifields = self._parse_header_fields(ih)
                    iop = int(ifields.get("op", 0))
                    if iop == OP_MESSAGE_DATA:
                        conn_id = int(ifields.get("conn", 0))
                        ts_ns = int(ifields.get("time", 0))
                        conn = self.connections.get(conn_id)
                        if conn:
                            yield (conn.topic, ts_ns, ib)
                    inner_pos += ih_len + id_len
            elif op == OP_MESSAGE_DATA:
                conn_id = int(fields.get("conn", 0))
                ts_ns = int(fields.get("time", 0))
                conn = self.connections.get(conn_id)
                if conn:
                    yield (conn.topic, ts_ns, data)

            offset += 4 + 4 + header_len + data_len

    def close(self) -> None:
        if self._fh:
            self._fh.close()
            self._fh = None

    def topics(self) -> List[Dict[str, str]]:
        seen: Set[str] = set()
        result: List[Dict[str, str]] = []
        for conn in self.connections.values():
            if conn.topic not in seen:
                seen.add(conn.topic)
                result.append({"topic": conn.topic, "type_name": conn.type_name})
        return result

    @property
    def start_time_ns(self) -> int:
        if self.chunk_infos:
            return min(ci.start_time_ns for ci in self.chunk_infos)
        return 0

    @property
    def end_time_ns(self) -> int:
        if self.chunk_infos:
            return max(ci.end_time_ns for ci in self.chunk_infos)
        return 0


# ---------------------------------------------------------------------------
# Message deserialisation — unified helper for bag & live paths
# ---------------------------------------------------------------------------

_TP_HEADER_FMT = "<Q"    # timestamp (uint64 LE)


def _parse_transport_packet(data: bytes) -> Dict[str, Any]:
    """Parse a dzipc_log TransportPacket envelope (per docs/dzipc_log.md).

    Binary layout (little-endian):
      timestamp  uint64
      topic      string (4-byte LE length prefix, UTF-8)
      type       string (4-byte LE length prefix, UTF-8)
      domain_id  uint32
      msg_id     uint32
      transport  uint8  (0=Shm, 1=Socket)
      role       uint8  (0=Publisher, 1=Subscriber, 2=Client, 3=Server)
      event      uint8
      payload    bytes  (4-byte LE length prefix)

    Returns dict with keys: timestamp, topic, type, domain_id, msg_id,
    transport, role, event, payload.
    Returns None on parse failure.
    """
    try:
        pos = 0
        if len(data) < 8:
            return None
        ts_ns = struct.unpack("<Q", data[pos:pos + 8])[0]
        pos += 8
        if pos + 4 > len(data):
            return None
        topic_len = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 4
        if pos + topic_len > len(data):
            return None
        topic = data[pos:pos + topic_len].decode("utf-8", errors="replace")
        pos += topic_len
        if pos + 4 > len(data):
            return None
        type_len = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 4
        if pos + type_len > len(data):
            return None
        type_name = data[pos:pos + type_len].decode("utf-8", errors="replace")
        pos += type_len
        if pos + 8 > len(data):
            return None
        domain_id = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 4
        msg_id = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 4
        transport = data[pos] if pos < len(data) else 0
        pos += 1
        role = data[pos] if pos < len(data) else 0
        pos += 1
        event = data[pos] if pos < len(data) else 0
        pos += 1
        if pos + 4 > len(data):
            return None
        payload_len = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 4
        payload = data[pos:pos + payload_len] if pos + payload_len <= len(data) else b""
        return {
            "timestamp": ts_ns,
            "topic": topic,
            "type": type_name,
            "domain_id": domain_id,
            "msg_id": msg_id,
            "transport": transport,
            "role": role,
            "event": event,
            "payload": payload,
        }
    except Exception:
        return None


def _to_jsonable(value: Any, depth: int = 0) -> Any:
    """Recursively convert a dzIPC field value to a JSON-safe type."""
    if depth > 8:
        return "<max-depth>"
    if value is None or isinstance(value, (bool, int, float, str)):
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return value
    if isinstance(value, (list, tuple)):
        return [_to_jsonable(v, depth + 1) for v in value[:100]]
    if isinstance(value, bytes):
        return base64.b64encode(value).decode("ascii")
    if hasattr(value, "field_count"):
        result: Dict[str, Any] = {}
        scalar_getters = {
            1: "get_bool", 2: "get_int8", 3: "get_uint8",
            4: "get_int16", 5: "get_uint16", 6: "get_int32",
            7: "get_uint32", 8: "get_int64", 9: "get_uint64",
            10: "get_float32", 11: "get_float64", 12: "get_string",
        }
        array_getters = {
            13: "get_bool_array", 14: "get_int8_array",
            15: "get_uint8_array", 16: "get_int16_array",
            17: "get_uint16_array", 18: "get_int32_array",
            19: "get_uint32_array", 20: "get_int64_array",
            21: "get_uint64_array", 22: "get_float32_array",
            23: "get_float64_array", 24: "get_string_array",
        }
        for i in range(min(value.field_count(), 50)):
            fname = str(value.field_name(i))
            ftype = value.field_type(i)
            try:
                if ftype in scalar_getters:
                    val = getattr(value, scalar_getters[ftype])(fname)
                    result[fname] = _to_jsonable(val, depth + 1)
                elif ftype in array_getters:
                    val = list(getattr(value, array_getters[ftype])(fname))
                    result[fname] = _to_jsonable(val, depth + 1)
                elif ftype == 25:  # nested message
                    result[fname] = _to_jsonable(
                        value.get_nested(fname), depth + 1)
                elif ftype == 26:  # nested array
                    result[fname] = [
                        _to_jsonable(item, depth + 1)
                        for item in value.get_nested_array(fname)
                    ][:50]
                else:
                    result[fname] = None
            except Exception:
                result[fname] = None
        return result
    return str(value)[:256]


def _serialize_message_fields(msg_obj: Any) -> Dict[str, Any]:
    """Extract all fields from a dzIPC GenericMessage into a typed dict.

    Returns a flat dict of {field_name: JSON-safe value}.  Nested messages
    and arrays are expanded recursively (max depth 8).
    """
    fields: Dict[str, Any] = {}
    if not hasattr(msg_obj, "field_count"):
        # Fallback: __slots__-based objects
        for name in getattr(msg_obj, "__slots__", ()):
            if not name.startswith("_"):
                try:
                    fields[name] = _to_jsonable(getattr(msg_obj, name))
                except Exception:
                    fields[name] = None
        return fields

    for idx in range(msg_obj.field_count()):
        name = str(msg_obj.field_name(idx))
        try:
            ftype = msg_obj.field_type(idx)
            scalar_getters = {
                1: "get_bool", 2: "get_int8", 3: "get_uint8",
                4: "get_int16", 5: "get_uint16", 6: "get_int32",
                7: "get_uint32", 8: "get_int64", 9: "get_uint64",
                10: "get_float32", 11: "get_float64", 12: "get_string",
            }
            array_getters = {
                13: "get_bool_array", 14: "get_int8_array",
                15: "get_uint8_array", 16: "get_int16_array",
                17: "get_uint16_array", 18: "get_int32_array",
                19: "get_uint32_array", 20: "get_int64_array",
                21: "get_uint64_array", 22: "get_float32_array",
                23: "get_float64_array", 24: "get_string_array",
            }
            if ftype in scalar_getters:
                val = getattr(msg_obj, scalar_getters[ftype])(name)
                fields[name] = _to_jsonable(val)
            elif ftype in array_getters:
                val = list(getattr(msg_obj, array_getters[ftype])(name))
                fields[name] = _to_jsonable(val)
            elif ftype == 25:  # nested
                fields[name] = _to_jsonable(msg_obj.get_nested(name))
            elif ftype == 26:  # nested array
                fields[name] = [
                    _to_jsonable(item)
                    for item in msg_obj.get_nested_array(name)
                ][:50]
            else:
                fields[name] = None
        except Exception:
            fields[name] = None
    return fields


def _decode_payload(type_name: str, raw: bytes,
                    _decode_warned: Optional[Set[str]] = None) -> Dict[str, Any]:
    """Try to deserialise raw bytes into structured fields for *type_name*.

    Uses dzIPC ``create_message(type_name)`` → ``deserialize(raw)`` →
    ``_serialize_message_fields`` to produce a nested dict with typed scalar
    and array fields.  If the type is unknown or deserialisation fails, falls
    back to ``{"data": base64(raw)}`` so the frontend always gets something.

    The optional ``_decode_warned`` set tracks per-type warnings to avoid
    log spam.
    """
    if _decode_warned is None:
        _decode_warned = set()
    try:
        import dzipc as ipc_mod
        msg = ipc_mod.create_message(type_name)
        raw_buf = bytes(raw)
        # Some GenericMessage subclasses expect buffer, others bytes
        if hasattr(msg, "deserialize_bytes"):
            msg.deserialize_bytes(raw_buf)
        elif hasattr(msg, "deserialize"):
            msg.deserialize(raw_buf)
        else:
            raise RuntimeError("no deserialize method on message")
        return _serialize_message_fields(msg)
    except Exception as e:
        if type_name not in _decode_warned:
            _decode_warned.add(type_name)
            print(f"[dzplot] NOTE: cannot deserialise type={type_name}, "
                  f"falling back to base64. Reason: {e}", flush=True)
        return {"data": base64.b64encode(bytes(raw)).decode("ascii")}


# ---------------------------------------------------------------------------
# Data sources: offline bag replay and online sniffer
# ---------------------------------------------------------------------------

class BagReplaySource:
    """Replay .bag files with simulated timing.

    Pushes samples into a shared BoundedPubQueue.  Timing is simulated:
    each message is delayed so the wall-clock interval between publishes
    matches the bag's recorded interval divided by the speed multiplier.
    """

    def __init__(
        self,
        path: str,
        queue: BoundedPubQueue,
        speed: float = 1.0,
        loop: bool = False,
        topic_filter: Optional[Set[str]] = None,
    ) -> None:
        self.path = path
        self.queue = queue
        self.speed = max(0.01, min(speed, 100.0))
        self.loop = loop
        self.topic_filter = topic_filter
        self._reader: Optional[BagReader] = None
        self._running = False
        self._paused = False
        self._thread: Optional[threading.Thread] = None
        self._start_real_ns: int = 0
        self._start_bag_ns: int = 0

    @property
    def running(self) -> bool:
        return self._running

    @property
    def paused(self) -> bool:
        return self._paused

    def start(self) -> None:
        if self._running:
            return
        self._reader = BagReader(self.path)
        self._reader.open()
        self._running = True
        self._paused = False
        self._thread = threading.Thread(target=self._replay_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=3.0)
            self._thread = None
        if self._reader:
            self._reader.close()
            self._reader = None

    def pause(self) -> None:
        self._paused = True

    def resume(self) -> None:
        if self._paused:
            self._paused = False
            self._start_real_ns = now_ns()

    def _replay_loop(self) -> None:
        assert self._reader is not None
        while self._running:
            bag_msgs: List[Tuple[str, int, bytes]] = []
            try:
                for topic, ts_ns, data in self._reader.iter_messages():
                    if not self._running:
                        break
                    if self.topic_filter and topic not in self.topic_filter:
                        continue
                    bag_msgs.append((topic, ts_ns, data))
            except Exception:
                pass

            if not bag_msgs:
                if self.loop and self._running:
                    self._reader.close()
                    self._reader = BagReader(self.path)
                    self._reader.open()
                    continue
                break

            self._start_real_ns = now_ns()
            self._start_bag_ns = bag_msgs[0][1]

            for topic, ts_ns, data in bag_msgs:
                if not self._running:
                    break
                while self._paused and self._running:
                    time.sleep(0.05)
                if not self._running:
                    break

                elapsed_bag_ns = ts_ns - self._start_bag_ns
                target_real_ns = self._start_real_ns + int(elapsed_bag_ns / self.speed)
                wait_ns = target_real_ns - now_ns()
                if wait_ns > 100_000:
                    time.sleep(wait_ns / 1e9)

                # Parse TransportPacket envelope to extract real topic, type,
                # timestamp and the inner message payload.
                tp = _parse_transport_packet(data)
                if tp is not None:
                    real_topic = tp["topic"]
                    real_type = tp["type"]
                    real_ts = tp["timestamp"]
                    inner_payload = tp["payload"]
                    sample = {
                        "source": "bag",
                        "topic": real_topic,
                        "msg_type": real_type,
                        "timestamp_ns": real_ts,
                        "data_length": len(inner_payload),
                        "fields": _decode_payload(real_type, inner_payload),
                        "bag_meta": {
                            "domain_id": tp["domain_id"],
                            "msg_id": tp["msg_id"],
                            "transport": tp["transport"],
                            "role": tp["role"],
                            "event": tp["event"],
                        },
                    }
                else:
                    # Non-TransportPacket data (e.g. raw payload from other
                    # bag sources).  Fall back to base64 + length.
                    sample = {
                        "source": "bag",
                        "topic": topic,
                        "msg_type": "",
                        "timestamp_ns": ts_ns,
                        "data_length": len(data),
                        "fields": {"data": base64.b64encode(data).decode("ascii")},
                    }

                # Flow control: bag data prioritises completeness —
                # pause the source when the queue is ≥ 50 % full so
                # we never drop historical samples.  stop/pause
                # immediately break out of the wait loop.
                while self._running and not self._paused:
                    wm = self.queue.watermark()
                    if wm.fill_ratio < 0.50:
                        break
                    time.sleep(0.050)

                if not self._running:
                    break

                self.queue.put(sample)

            if not self.loop:
                break
            if self._running:
                self._reader.close()
                self._reader = BagReader(self.path)
                self._reader.open()

        self._running = False


class LiveSniffSource:
    """Subscribe to live dzIPC topics and push samples to the queue.

    Uses the passive ipc::sniffer (pybind11 binding in interface.cc) —
    a read-only hook into SHM channels that does NOT register as a
    receiver and does not affect publisher behaviour.  Falls back to a
    regular dzIPC Subscriber only if the sniffer binding is unavailable
    (e.g. _dzipc_core.so not yet rebuilt after interface.cc change).

    Channel naming follows the same convention as dzipc_topic_cat and the
    publisher internals (dzIPC/common/name_operator.h):
      "dz_ipc_" + sanitize(topic) + "_topic"  → ipc::route topology

    Uses per-topic RateController instances (team shared module) for
    thread-safe adaptive poll interval and backpressure-aware sampling.
    """

    # ------------------------------------------------------------------
    # Sniffer availability check (cached)
    # ------------------------------------------------------------------
    _sniffer_available: Optional[bool] = None

    # Per-topic error log cooldown tracker (seconds since epoch per topic).
    # Limits error log spam to at most one traceback per topic every 30 s.
    _sniff_error_cooldown: Dict[str, float] = {}

    @classmethod
    def _check_sniffer(cls) -> bool:
        """Return True if ipc::sniffer pybind11 binding is available."""
        if cls._sniffer_available is not None:
            return cls._sniffer_available
        try:
            import dzipc as ipc_mod
            # New binding: ipc::sniffer is exposed as dzipc.Sniffer
            cls._sniffer_available = hasattr(ipc_mod, "Sniffer")
        except Exception:
            cls._sniffer_available = False
        if not cls._sniffer_available:
            print("[dzplot] NOTE: ipc::sniffer pybind11 binding (dzipc.Sniffer) "
                  "not available. Rebuild _dzipc_core.so after interface.cc changes. "
                  "Falling back to Subscriber (registers as receiver, may affect publisher).",
                  flush=True)
        return cls._sniffer_available

    @staticmethod
    def _sanitize_topic_name(topic: str) -> str:
        """Replicate dzIPC/common/name_operator.h sanitize_topic_name.

        Keeps alphanumeric, '_', '-', '.'; replaces all other chars with '_'.
        """
        result: List[str] = []
        for ch in topic:
            if ch.isalnum() or ch in ("_", "-", "."):
                result.append(ch)
            else:
                result.append("_")
        return "".join(result)

    @staticmethod
    def _channel_name_for_topic(topic: str) -> str:
        """Construct the SHM channel name for a pub/sub topic.

        Matches the pattern used by shm_pub_sub_ipc and dzipc_topic_cat:
          "dz_ipc_" + sanitize_topic_name(topic) + "_topic"
        """
        return "dz_ipc_" + LiveSniffSource._sanitize_topic_name(topic) + "_topic"

    @staticmethod
    def _control_plane_name_for_topic(topic: str) -> str:
        """Construct the control plane SHM name for a pub/sub topic.

        Matches the pattern used by shm_sniffer in dzipc_topic_cat:
          "dz_ipc_" + sanitize_topic_name(topic) + "_topic_control"
        """
        return "dz_ipc_" + LiveSniffSource._sanitize_topic_name(topic) + "_topic_control"

    def __init__(
        self,
        topics: List[Dict[str, Any]],
        queue: BoundedPubQueue,
        backpressure_ctrl: BackpressureController,
        transport: str = "shm",
        domain: int = 0,
    ) -> None:
        self.topic_configs = topics
        self.queue = queue
        self._backpressure_ctrl = backpressure_ctrl
        self.transport = transport
        self.domain = domain
        self._running = False
        self._threads: List[threading.Thread] = []

        # Per-topic RateController instances (one per thread for thread safety)
        self._rate_ctrls: Dict[str, RateController] = {}
        rc_cfg = RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=60.0,
            max_render_hz=60.0,
            queue_capacity=max(10, queue.max_size // max(len(topics), 1)),
            q_target=0.50,       # target queue ratio (midpoint of 25%-75% zone)
            K_p=0.50,            # proportional gain for rate adjustment
            confirm_ticks=3,
        )
        for cfg in topics:
            self._rate_ctrls[cfg["topic"]] = RateController(
                RateControllerConfig(**rc_cfg.__dict__)
            )

    @property
    def running(self) -> bool:
        return self._running

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        for cfg in self.topic_configs:
            t = threading.Thread(target=self._sniff_loop, args=(cfg,), daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self) -> None:
        self._running = False
        for t in self._threads:
            t.join(timeout=3.0)
        self._threads.clear()

    def set_render_hz(self, hz: float) -> None:
        """Forward FPS changes to all per-topic rate controllers."""
        for rc in self._rate_ctrls.values():
            rc.set_render_hz(hz)

    def _sniff_loop(self, cfg: Dict[str, Any]) -> None:
        topic = cfg["topic"]
        msg_type = cfg.get("msg_type", "StdRawMessage")
        extra = cfg.get("extra", "")
        dom = cfg.get("domain", self.domain)
        trans = cfg.get("transport", self.transport)
        qsize = int(cfg.get("queue", 10))

        # Per-topic rate controller (thread-safe: one per thread)
        rc = self._rate_ctrls[topic]

        ipc = None
        sub = None
        topic_data = None
        control_plane = None
        generation = 0
        use_sniffer = self._check_sniffer() and trans == "shm"

        try:
            import dzipc as ipc_mod
            ipc = ipc_mod

            if use_sniffer:
                # Passive sniffer via ipc::sniffer pybind11 binding.
                # Channel name matches what the publisher creates internally:
                #   "dz_ipc_" + sanitize(topic) + "_topic"
                ch_name = self._channel_name_for_topic(topic)
                sniffer = ipc.Sniffer()
                if not sniffer.open(ch_name, ipc.SnifferTopology.route):
                    print(f"[dzplot] Sniffer.open() failed for {topic} "
                          f"(channel={ch_name}), falling back to subscriber",
                          flush=True)
                    use_sniffer = False
                else:
                    print(f"[dzplot] Sniffer attached to {topic} "
                          f"(channel={ch_name}, dropped={sniffer.dropped()})",
                          flush=True)
                    sub = sniffer
                    topic_data = None   # not needed for sniffer path

                    # Open control plane for publisher-restart detection.
                    # Mirrors dzipc_topic_cat shm_sniffer.cc:140-158.
                    try:
                        cp_name = self._control_plane_name_for_topic(topic)
                        control_plane = ipc.TopicControlPlane()
                        if control_plane.open(cp_name):
                            generation = control_plane.generation()
                            print(f"[dzplot] Control plane attached to {topic} "
                                  f"({cp_name}, generation={generation})", flush=True)
                        else:
                            print(f"[dzplot] Control plane open failed for {topic} "
                                  f"({cp_name}), restart detection disabled", flush=True)
                            control_plane = None
                    except Exception as e:
                        print(f"[dzplot] Control plane init failed for {topic}: {e}", flush=True)
                        control_plane = None

            if not use_sniffer:
                msg_cls = getattr(ipc, msg_type, ipc.StdRawMessage)
                template = msg_cls()
                topic_data = ipc.make_topic_data(template)
                ipc_type = ipc.IPC_SHM if trans == "shm" else ipc.IPC_SOCKET
                sub = ipc.SubscriberIPCPtrMake(topic_data, topic, dom, qsize, ipc_type)
                sub.InitChannel(extra)
        except Exception as e:
            print(f"[dzplot] sniffer init failed for {topic}: {e}", flush=True)
            return

        # Monotonic-deadline poll scheduling (prevents drift, respects
        # RateController interval changes immediately, ≥1ms floor).
        # We sleep on EVERY iteration — success or failure — so the
        # poll rate = RateController.recv_interval_s, and backpressure
        # actually slows down the source rather than just dropping.
        next_poll_s = time.monotonic() + rc.recv_interval_s

        while self._running:
            now_s = time.monotonic()
            remain_s = next_poll_s - now_s
            if remain_s > 0:
                time.sleep(min(remain_s, 0.100))  # cap at 100ms to stay responsive

            try:
                # Reattach sniffer if publisher restarted (generation changed).
                # Mirrors dzipc_topic_cat shm_sniffer.cc:140-158.
                if control_plane is not None and control_plane.valid():
                    state = control_plane.state()
                    if state == ipc.TopicState.Ready:
                        current_gen = control_plane.generation()
                        if current_gen != 0 and current_gen != generation:
                            print(f"[dzplot] Publisher restart detected for {topic} "
                                  f"(generation {generation} → {current_gen}), "
                                  f"re-attaching sniffer...", flush=True)
                            sub.close()
                            ch_name = self._channel_name_for_topic(topic)
                            new_sniffer = ipc.Sniffer()
                            if new_sniffer.open(ch_name, ipc.SnifferTopology.route):
                                new_sniffer.skip_to_latest()
                                sub = new_sniffer
                                generation = current_gen
                                print(f"[dzplot] Sniffer re-attached to {topic} "
                                      f"(channel={ch_name}, generation={generation})", flush=True)
                            else:
                                print(f"[dzplot] Sniffer re-open failed for {topic} "
                                      f"after restart", flush=True)

                if use_sniffer:
                    result = sub.try_recv()
                    ok = result is not None
                else:
                    ok, out = sub.try_get(topic_data)

                # Notify per-topic rate controller
                queue_depth = self.queue.size
                rc.on_sample(ok, queue_depth)

                # Schedule next poll at monotonic deadline (≥1ms floor)
                interval_s = max(rc.recv_interval_s, 0.001)
                # Guard: if we fell behind, resync to now (don't burst-catch-up)
                if next_poll_s < time.monotonic() - interval_s:
                    next_poll_s = time.monotonic() + interval_s
                else:
                    next_poll_s += interval_s

                if not ok:
                    continue

                # Apply backpressure using BackpressureController (team module)
                signal = self.queue.last_signal()
                if signal and signal.active:
                    if not self._backpressure_ctrl.should_publish(topic, signal):
                        continue

                if use_sniffer:
                    raw_data = result["data"]
                    dropped_total = result.get("dropped", 0)
                    sample = {
                        "source": "live",
                        "topic": topic,
                        "msg_type": msg_type,
                        "timestamp_ns": now_ns(),
                        "fields": _decode_payload(msg_type, raw_data),
                        "sniffer_dropped": dropped_total,
                    }
                else:
                    msg_obj = out.topic() if out else topic_data.topic()
                    sample = self._serialize_message(msg_obj, topic, msg_type)

                rc.batch_append(sample)

                # Flush batch into shared queue
                batch = rc.flush()
                for item in batch:
                    self.queue.put(item)

            except Exception:
                now_err = time.monotonic()
                last_logged = self._sniff_error_cooldown.get(topic, 0.0)
                if now_err - last_logged > 30.0:
                    self._sniff_error_cooldown[topic] = now_err
                    print(f"[dzplot] _sniff_loop error for topic={topic}:",
                          flush=True)
                    traceback.print_exc()
                next_poll_s = time.monotonic() + 0.050

    @staticmethod
    def _serialize_message(msg_obj: Any, topic: str, msg_type: str) -> Dict[str, Any]:
        """Extract fields from a dzIPC message object into a sample dict."""
        fields = _serialize_message_fields(msg_obj)
        return {
            "source": "live",
            "topic": topic,
            "msg_type": msg_type,
            "timestamp_ns": now_ns(),
            "fields": fields,
        }


# ---------------------------------------------------------------------------
# WebSocket frame helpers (matching dzviz.py wire protocol)
# ---------------------------------------------------------------------------

def ws_make_frame(payload: bytes, opcode: int = 1) -> bytes:
    first = 0x80 | (opcode & 0x0F)
    size = len(payload)
    if size < 126:
        return bytes([first, size]) + payload
    if size <= 0xFFFF:
        return bytes([first, 126]) + struct.pack("!H", size) + payload
    return bytes([first, 127]) + struct.pack("!Q", size) + payload


async def ws_read_frame(reader: asyncio.StreamReader) -> Optional[Tuple[int, bytes]]:
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


# ---------------------------------------------------------------------------
# PlotHub: WebSocket hub managing clients, sources, and broadcast
# ---------------------------------------------------------------------------

@dataclass
class PlotTopicMeta:
    topic: str
    msg_type: str = ""
    source: str = ""
    active: bool = True
    sample_count: int = 0
    last_update_ms: int = 0


# Per-client state for true sender isolation.
# Each client has its own bounded asyncio queue and independent sender task,
# so a slow TCP drain never stalls the global broadcast loop or other clients.
PER_CLIENT_QUEUE_SIZE = 8
PER_CLIENT_DRAIN_TIMEOUT = 1.0        # seconds
PER_CLIENT_CONSECUTIVE_FULL_LIMIT = 3  # disconnect after N consecutive full drops


@dataclass
class _ClientSlot:
    writer: asyncio.StreamWriter
    queue: "asyncio.Queue[bytes]"
    task: "asyncio.Task[None]"
    dead: bool = False          # sender task exited (timeout / write error)
    dropped_frames: int = 0
    total_frames: int = 0
    consecutive_full: int = 0


class PlotHub:
    """Central hub managing WebSocket clients, data sources, and broadcast.

    Uses BoundedPubQueue (team shared module) for the event pipeline and
    RateController-compatible backpressure signals.
    """

    # Queue sizing: large enough for 1000Hz × 2s buffer at 60fps broadcast
    DEFAULT_QUEUE_SIZE = 4096

    def __init__(self, max_fps: int = 60) -> None:
        self._max_fps = max(1, min(max_fps, 120))
        self._min_frame_interval = 1.0 / self._max_fps

        # Shared bounded queue (thread-safe, team module)
        self.event_queue = BoundedPubQueue(max_size=self.DEFAULT_QUEUE_SIZE)
        self._backpressure_ctrl = BackpressureController(
            max_input_hz=1000.0,
            max_output_hz=float(self._max_fps),
        )

        # Per-client sender-task slots: id(writer) -> _ClientSlot
        self._clients: Dict[int, _ClientSlot] = {}
        self.topics: Dict[str, PlotTopicMeta] = {}
        self.selected_fields: Dict[str, List[str]] = {}

        self._bag_source: Optional[BagReplaySource] = None
        self._live_source: Optional[LiveSniffSource] = None
        self._playback_state: Dict[str, Any] = {
            "mode": "idle",
            "source": "",
            "speed": 1.0,
            "loop": False,
        }

        # Broadcast rate tracking
        self._last_broadcast_time: float = 0.0
        self._total_samples_sent: int = 0

    # ------------------------------------------------------------------
    # Properties
    # ------------------------------------------------------------------

    @property
    def max_fps(self) -> int:
        return self._max_fps

    @max_fps.setter
    def max_fps(self, value: int) -> None:
        self._max_fps = max(1, min(value, 120))
        self._min_frame_interval = 1.0 / self._max_fps
        self._backpressure_ctrl.max_output_hz = float(self._max_fps)
        if self._live_source:
            self._live_source.set_render_hz(float(self._max_fps))

    # ------------------------------------------------------------------
    # Source management
    # ------------------------------------------------------------------

    def load_bag(self, path: str, speed: float = 1.0, loop: bool = False) -> Dict[str, Any]:
        if self._bag_source and self._bag_source.running:
            self._bag_source.stop()

        reader = BagReader(path)
        try:
            reader.open()
            bag_topics = reader.topics()
        except Exception as e:
            return {"ok": False, "error": str(e)}
        finally:
            reader.close()

        for t in bag_topics:
            self.topics[t["topic"]] = PlotTopicMeta(
                topic=t["topic"],
                msg_type=t.get("type_name", ""),
                source="bag",
            )

        self._bag_source = BagReplaySource(
            path, self.event_queue, speed=speed, loop=loop,
        )
        self._bag_source.start()
        self._playback_state = {
            "mode": "playing",
            "source": "bag",
            "path": path,
            "speed": speed,
            "loop": loop,
        }
        return {"ok": True, "topics": [
            {
                "topic": self.topics[t["topic"]].topic,
                "msg_type": self.topics[t["topic"]].msg_type,
                "source": self.topics[t["topic"]].source,
                "active": self.topics[t["topic"]].active,
                "sample_count": self.topics[t["topic"]].sample_count,
            }
            for t in bag_topics
        ]}

    def start_sniff(
        self, topics: List[Dict[str, Any]],
        transport: str = "shm", domain: int = 0,
    ) -> Dict[str, Any]:
        if self._live_source and self._live_source.running:
            self._live_source.stop()

        for t in topics:
            self.topics[t["topic"]] = PlotTopicMeta(
                topic=t["topic"],
                msg_type=t.get("msg_type", ""),
                source="live",
            )

        self._live_source = LiveSniffSource(
            topics, self.event_queue, self._backpressure_ctrl,
            transport, domain,
        )
        self._live_source.start()
        self._playback_state = {
            "mode": "playing",
            "source": "live",
            "transport": transport,
            "domain": domain,
        }
        return {"ok": True, "topics": [
            {
                "topic": self.topics[t["topic"]].topic,
                "msg_type": self.topics[t["topic"]].msg_type,
                "source": self.topics[t["topic"]].source,
                "active": self.topics[t["topic"]].active,
                "sample_count": self.topics[t["topic"]].sample_count,
            }
            for t in topics
        ]}

    def pause(self) -> None:
        if self._bag_source:
            self._bag_source.pause()
        self._playback_state["mode"] = "paused"

    def resume(self) -> None:
        if self._bag_source:
            self._bag_source.resume()
        self._playback_state["mode"] = "playing"

    def stop_source(self) -> None:
        if self._bag_source:
            self._bag_source.stop()
            self._bag_source = None
        if self._live_source:
            self._live_source.stop()
            self._live_source = None
        self._playback_state["mode"] = "idle"

    def select_fields(self, topic: str, fields: List[str]) -> None:
        self.selected_fields[topic] = fields

    # ------------------------------------------------------------------
    # WebSocket client management — per-client sender-task isolation
    # ------------------------------------------------------------------
    #
    # Every client gets a small bounded asyncio.Queue + an independent
    # sender task.  The broadcast loop (and _send_json for command
    # responses) only ever calls slot.queue.put_nowait().  A sender task
    # that blocks on drain() for too long is cancelled and its client
    # disconnected.  This means a single slow TCP peer can never stall
    # the global broadcast loop or other clients.

    async def add_client(self, writer: asyncio.StreamWriter) -> None:
        """Create per-client queue + sender task, send initial hello."""
        slot = _ClientSlot(
            writer=writer,
            queue=asyncio.Queue(maxsize=PER_CLIENT_QUEUE_SIZE),
            task=None,  # set below
        )
        slot.task = asyncio.create_task(self._sender_task(slot))
        self._clients[id(writer)] = slot

        # Send initial hello through the per-client queue
        hello = self._encode_frame({
            "kind": "hello",
            "topics": [
                {
                    "topic": m.topic,
                    "msg_type": m.msg_type,
                    "source": m.source,
                    "active": m.active,
                    "sample_count": m.sample_count,
                }
                for m in self.topics.values()
            ],
            "playback": self._playback_state,
            "fps": self._max_fps,
            "selected_fields": self.selected_fields,
        })
        try:
            slot.queue.put_nowait(hello)
        except asyncio.QueueFull:
            await self.remove_client(writer)

    async def remove_client(self, writer: asyncio.StreamWriter) -> None:
        """Cancel sender task, close writer, remove slot."""
        wid = id(writer)
        slot = self._clients.pop(wid, None)
        if slot is not None and slot.task is not None:
            slot.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await slot.task
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    @staticmethod
    def _encode_frame(data: Dict[str, Any]) -> bytes:
        payload = json.dumps(data, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        return ws_make_frame(payload)

    async def _send_json(
        self, writer: asyncio.StreamWriter, data: Dict[str, Any],
    ) -> None:
        """Push a JSON frame into the per-client queue (non-blocking).

        Falls back to direct write+drain if the slot was somehow removed
        (e.g. just-disconnected client).
        """
        slot = self._clients.get(id(writer))
        if slot is not None:
            try:
                slot.queue.put_nowait(self._encode_frame(data))
                return
            except asyncio.QueueFull:
                pass  # queue full → fall through to direct write
        # Fallback: direct write for edge cases (slot already removed)
        try:
            writer.write(self._encode_frame(data))
            await writer.drain()
        except Exception:
            pass

    async def _sender_task(self, slot: _ClientSlot) -> None:
        """Independent per-client sender loop.

        Reads bytes from the client's queue, writes them to the socket
        with a timeout on drain().  On timeout or write error the slot
        is marked dead and the writer is closed — the broadcast loop
        will remove the slot on the next tick.
        """
        try:
            while True:
                try:
                    frame_bytes = await asyncio.wait_for(
                        slot.queue.get(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue

                slot.writer.write(frame_bytes)
                await asyncio.wait_for(
                    slot.writer.drain(),
                    timeout=PER_CLIENT_DRAIN_TIMEOUT,
                )
                slot.total_frames += 1
        except (asyncio.TimeoutError, ConnectionError, OSError,
                asyncio.CancelledError):
            pass
        finally:
            slot.dead = True
            # Close the writer so the socket doesn't leak; the reaper
            # will call wait_closed() and pop the slot.
            with contextlib.suppress(Exception):
                slot.writer.close()

    # ------------------------------------------------------------------
    # Broadcast loop — drains BoundedPubQueue at configured fps
    # ------------------------------------------------------------------

    async def broadcast_loop(self) -> None:
        """Drain the bounded queue at configurable fps.

        Each frame is enqueued into every active client's bounded
        asyncio.Queue via put_nowait().  If a client queue is full the
        oldest queued frame is dropped to make room for the latest (the
        frontend only needs the most recent data).  After
        PER_CLIENT_CONSECUTIVE_FULL_LIMIT consecutive full queues the
        client is disconnected.

        Dead clients (those whose sender task exited) are cleaned up at
        the end of every tick.
        """
        while True:
            # -------- rate limit --------
            now_m = time.monotonic()
            elapsed = now_m - self._last_broadcast_time
            if elapsed < self._min_frame_interval:
                await asyncio.sleep(self._min_frame_interval - elapsed)
            self._last_broadcast_time = time.monotonic()

            # -------- drain event queue --------
            batch = self.event_queue.drain()
            if not batch:
                await asyncio.sleep(0.005)
                # Still need to clean up dead clients even when idle
                await self._reap_dead_clients()
                continue

            # -------- build frame once --------
            wm = self.event_queue.watermark()
            signal = self.event_queue.last_signal()
            frame_bytes = self._encode_frame({
                "kind": "frame",
                "batch": batch,
                "stats": {
                    "queue_size": wm.current_size,
                    "queue_capacity": wm.max_size,
                    "fill_ratio": round(wm.fill_ratio, 3),
                    "zone": wm.zone,
                    "total_published": wm.total_published,
                    "total_dropped": wm.total_dropped,
                    "backpressure_active": signal.active if signal else False,
                    "backpressure_zone": signal.zone if signal else "normal",
                    "keep_every_n": signal.keep_every_n if signal else 1,
                },
                "timestamp_ms": now_ms(),
            })

            # -------- enqueue to all live clients (non-blocking) --------
            dead_wids: List[int] = []
            for wid, slot in list(self._clients.items()):
                if slot.dead:
                    dead_wids.append(wid)
                    continue
                try:
                    slot.queue.put_nowait(frame_bytes)
                    slot.consecutive_full = 0
                except asyncio.QueueFull:
                    # Drop oldest, keep latest
                    try:
                        slot.queue.get_nowait()
                        slot.queue.put_nowait(frame_bytes)
                    except (asyncio.QueueFull, asyncio.QueueEmpty):
                        pass
                    slot.dropped_frames += 1
                    slot.consecutive_full += 1
                    if slot.consecutive_full >= PER_CLIENT_CONSECUTIVE_FULL_LIMIT:
                        dead_wids.append(wid)

            # -------- clean up dead clients --------
            for wid in dead_wids:
                slot = self._clients.pop(wid, None)
                if slot is not None:
                    if slot.task is not None:
                        slot.task.cancel()
                        with contextlib.suppress(asyncio.CancelledError):
                            await slot.task
                    with contextlib.suppress(Exception):
                        slot.writer.close()
                        await slot.writer.wait_closed()

            # -------- update metadata --------
            for item in batch:
                t = item.get("topic", "")
                if t in self.topics:
                    self.topics[t].sample_count += 1
                    self.topics[t].last_update_ms = now_ms()
            self._total_samples_sent += len(batch)

    async def _reap_dead_clients(self) -> None:
        """Remove clients whose sender task exited (slot.dead == True)."""
        dead = [
            wid for wid, slot in self._clients.items()
            if slot.dead
        ]
        for wid in dead:
            slot = self._clients.pop(wid, None)
            if slot is not None:
                if slot.task is not None:
                    slot.task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await slot.task
                with contextlib.suppress(Exception):
                    slot.writer.close()
                    await slot.writer.wait_closed()

    async def shutdown(self) -> None:
        self.stop_source()
        for wid, slot in list(self._clients.items()):
            if slot.task is not None:
                slot.task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await slot.task
            with contextlib.suppress(Exception):
                slot.writer.close()
                await slot.writer.wait_closed()
        self._clients.clear()

    # ------------------------------------------------------------------
    # Command dispatch
    # ------------------------------------------------------------------

    def handle_command(self, cmd: Dict[str, Any]) -> Dict[str, Any]:
        """Dispatch a command. All responses include 'kind': 'ack' for the
        frontend handleMessage() dispatch switch."""
        action = cmd.get("action", "")
        try:
            if action == "load_bag":
                return {"kind": "ack", **self.load_bag(
                    cmd.get("path", ""),
                    float(cmd.get("speed", 1.0)),
                    bool(cmd.get("loop", False)),
                )}
            if action == "start_sniff":
                return {"kind": "ack", **self.start_sniff(
                    cmd.get("topics", []),
                    cmd.get("transport", "shm"),
                    int(cmd.get("domain", 0)),
                )}
            if action == "pause":
                self.pause()
                return {"kind": "ack", "ok": True, "mode": "paused"}
            if action == "resume":
                self.resume()
                return {"kind": "ack", "ok": True, "mode": "playing"}
            if action == "stop":
                self.stop_source()
                return {"kind": "ack", "ok": True, "mode": "idle"}
            if action == "set_fps":
                self.max_fps = int(cmd.get("fps", 60))
                return {"kind": "ack", "ok": True, "fps": self._max_fps}
            if action == "select_fields":
                self.select_fields(
                    cmd.get("topic", ""),
                    cmd.get("fields", []),
                )
                return {"kind": "ack", "ok": True}
            if action == "get_topics":
                return {"kind": "ack", "ok": True,
                    "topics": [
                        {
                            "topic": m.topic,
                            "msg_type": m.msg_type,
                            "source": m.source,
                            "active": m.active,
                            "sample_count": m.sample_count,
                        }
                        for m in self.topics.values()
                    ],
                }
            if action == "get_status":
                wm = self.event_queue.watermark()
                signal = self.event_queue.last_signal()
                return {"kind": "ack", "ok": True,
                    "playback": self._playback_state,
                    "fps": self._max_fps,
                    "queue_stats": {
                        "size": wm.current_size,
                        "capacity": wm.max_size,
                        "fill_ratio": round(wm.fill_ratio, 3),
                        "zone": wm.zone,
                        "total_dropped": wm.total_dropped,
                        "backpressure_active": signal.active if signal else False,
                        "total_sent": self._total_samples_sent,
                    },
                }
            return {"kind": "ack", "ok": False, "error": f"unknown action: {action}"}
        except Exception as e:
            return {"kind": "ack", "ok": False, "error": str(e)}


# ---------------------------------------------------------------------------
# HTTP / WebSocket server
# ---------------------------------------------------------------------------

def http_response(status: str, headers: Dict[str, str], body: bytes = b"") -> bytes:
    headers = {"Content-Length": str(len(body)), "Connection": "close", **headers}
    lines = [f"HTTP/1.1 {status}"]
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
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    hub: PlotHub,
) -> None:
    try:
        request = await reader.readuntil(b"\r\n\r\n")
    except Exception:
        writer.close()
        return

    try:
        head = request.decode("utf-8", errors="replace")
        first_line = head.split("\r\n", 1)[0]
        parts = first_line.split(" ", 2)
        if len(parts) < 2:
            raise ValueError("bad request line")
        method, path = parts[0], parts[1]
        headers: Dict[str, str] = {}
        for line in head.split("\r\n")[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
    except Exception:
        writer.write(http_response("400 Bad Request", {"Content-Type": "text/plain"}, b"bad request"))
        await writer.drain()
        writer.close()
        return

    # WebSocket upgrade (matching dzviz.py protocol)
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
                frame = await ws_read_frame(reader)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 9:
                    writer.write(ws_make_frame(payload, opcode=10))
                    await writer.drain()
                elif opcode == 1:
                    try:
                        command = json.loads(payload.decode("utf-8"))
                        response = hub.handle_command(command)
                    except Exception as exc:
                        response = {"kind": "ack", "ok": False, "error": str(exc)}
                    await hub._send_json(writer, response)
        except Exception:
            pass
        await hub.remove_client(writer)
        return

    # Static file serving
    if method != "GET":
        writer.write(http_response("405 Method Not Allowed", {"Content-Type": "text/plain"}, b"method not allowed"))
        await writer.drain()
        writer.close()
        return

    file_path = safe_static_path(path.split("?", 1)[0])
    if not file_path.exists() or not file_path.is_file():
        writer.write(http_response("404 Not Found", {"Content-Type": "text/plain"}, b"not found"))
        await writer.drain()
        writer.close()
        return

    body = file_path.read_bytes()
    content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
    writer.write(http_response("200 OK", {"Content-Type": content_type}, body))
    await writer.drain()
    writer.close()


# ---------------------------------------------------------------------------
# CLI and main
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="dzplot — dzIPC time-series visualization dashboard"
    )
    p.add_argument("--host", default="127.0.0.1", help="Listen address (default: 127.0.0.1)")
    p.add_argument("--port", type=int, default=8766, help="Listen port (default: 8766)")
    p.add_argument("--bag", default=None, help="Path to .bag file for offline replay")
    p.add_argument("--bag-speed", type=float, default=1.0, help="Replay speed multiplier (default: 1.0)")
    p.add_argument("--bag-loop", action="store_true", help="Loop bag replay")
    p.add_argument("--sniff", action="store_true", help="Enable online sniffer mode")
    p.add_argument("--topic", action="append", default=[], help="Topic:MsgType, e.g. /test:StdRawMessage")
    p.add_argument("--transport", choices=["shm", "socket"], default="shm", help="Sniffer transport (default: shm)")
    p.add_argument("--domain", type=int, default=0, help="Domain ID for sniffer (default: 0)")
    p.add_argument("--fps", type=int, default=60, help="Max WebSocket broadcast FPS (default: 60, max: 120)")
    return p


async def run_server(args: argparse.Namespace) -> None:
    hub = PlotHub(max_fps=args.fps)

    # Auto-load bag if specified
    if args.bag:
        result = hub.load_bag(args.bag, speed=args.bag_speed, loop=args.bag_loop)
        if not result.get("ok"):
            print(f"[dzplot] ERROR loading bag: {result.get('error')}", flush=True)
            sys.exit(1)
        print(f"[dzplot] Loaded bag: {args.bag} ({len(result.get('topics', []))} topics)", flush=True)

    # Auto-start sniffer if --sniff
    if args.sniff:
        topics = []
        for raw in args.topic:
            if ":" in raw:
                topic, msg_type = raw.split(":", 1)
                topics.append({"topic": topic.strip(), "msg_type": msg_type.strip()})
            else:
                topics.append({"topic": raw.strip(), "msg_type": "StdRawMessage"})
        if not topics:
            print("[dzplot] WARNING: --sniff specified but no --topic given.", flush=True)
        else:
            result = hub.start_sniff(topics, transport=args.transport, domain=args.domain)
            if not result.get("ok"):
                print(f"[dzplot] ERROR starting sniffer: {result.get('error')}", flush=True)
            else:
                print(f"[dzplot] Sniffing {len(topics)} topic(s) on {args.transport}", flush=True)

    broadcast_task = asyncio.create_task(hub.broadcast_loop())

    try:
        server = await asyncio.start_server(
            lambda r, w: handle_client(r, w, hub),
            args.host,
            args.port,
            reuse_address=True,
        )
    except OSError as e:
        print(f"[dzplot] ERROR: Cannot bind to {args.host}:{args.port}: {e}", flush=True)
        sys.exit(1)

    print(f"[dzplot] serving {WEB_DIR} on {args.host}:{args.port}", flush=True)
    print(f"[dzplot] open: http://127.0.0.1:{args.port}", flush=True)
    print(f"\033[32m[dzplot] Ctrl+Click http://127.0.0.1:{args.port}\033[0m", flush=True)

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


def main() -> None:
    args = build_parser().parse_args()
    try:
        asyncio.run(run_server(args))
    except KeyboardInterrupt:
        print("\n[dzplot] stopped", flush=True)


if __name__ == "__main__":
    main()
