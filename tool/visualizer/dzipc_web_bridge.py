#!/usr/bin/env python3
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

VISUALIZER_MESSAGE_ALIASES = {
    "RawMessage": "StdRawMessage",
    "Pose": "StdPose",
    "Path": "StdPath",
    "PointCloud": "StdPointCloud",
    "Marker": "StdMarker",
    "Image": "StdImage",
}

VISUALIZER_MESSAGE_TYPES = list(VISUALIZER_MESSAGE_ALIASES.keys())


def load_dzipc():
    python_dir = ROOT_DIR / "python"
    if python_dir.is_dir() and str(python_dir) not in sys.path:
        sys.path.insert(0, str(python_dir))
    try:
        import dzipc as ipc  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "Cannot import dzipc. Build/install the Python package first or run with --demo."
        ) from exc
    return ipc


def now_ms() -> int:
    return int(time.time() * 1000)


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
        payload = json.dumps(event, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        writer.write(make_frame(payload))
        await writer.drain()

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
                topic.update(
                    {
                        "name": event["topic"],
                        "type": event.get("msg_type", topic.get("type", "")),
                        "msg_type": event.get("msg_type", topic.get("msg_type", "")),
                        "transport": event.get("transport", topic.get("transport", "")),
                        "domain": event.get("domain", topic.get("domain", 0)),
                        "queue": event.get("queue", topic.get("queue", self.defaults.get("queue", 10))),
                        "extra": event.get("extra", topic.get("extra", "")),
                        "updated_ms": event.get("timestamp_ms", now_ms()),
                        "active": True,
                    }
                )
            dead: List[asyncio.StreamWriter] = []
            for writer in list(self.clients):
                try:
                    await self.send(writer, event)
                except Exception:
                    dead.append(writer)
            for writer in dead:
                await self.remove_client(writer)

    async def shutdown(self) -> None:
        for worker in list(self.subscribers.values()):
            worker.stop_event.set()
        self.subscribers.clear()
        for worker in list(self.demo_workers.values()):
            worker.stop_event.set()
        self.demo_workers.clear()
        self.publish({"kind": "__shutdown__"})
        for writer in list(self.clients):
            await self.remove_client(writer)

    def add_topic(self, spec: "TopicSpec") -> None:
        if spec.topic in self.subscribers:
            self.remove_topic(spec.topic)
        self.topics[spec.topic] = spec.to_meta(active=True)
        worker = DzipcSubscriber(self, spec)
        self.subscribers[spec.topic] = worker
        worker.start()
        self.publish({"kind": "topic_added", "topic": spec.to_meta(active=True), "timestamp_ms": now_ms()})

    def remove_topic(self, topic: str) -> None:
        worker = self.subscribers.pop(topic, None)
        if worker is not None:
            worker.stop_event.set()
        meta = self.topics.get(topic)
        if meta is not None:
            meta["active"] = False
        self.publish({"kind": "topic_removed", "topic": topic, "timestamp_ms": now_ms()})

    def export_config(self) -> Dict[str, Any]:
        return {
            "domain": self.defaults.get("domain", 1),
            "transport": self.defaults.get("transport", "socket"),
            "queue": self.defaults.get("queue", 10),
            "poll": self.defaults.get("poll", 0.03),
            "extra": self.defaults.get("extra", ""),
            "verbose": self.defaults.get("verbose", False),
            "topics": [worker.spec.to_config() for worker in self.subscribers.values()],
        }

    def save_config(self, path: Optional[str] = None) -> Path:
        target = Path(path).expanduser() if path else self.config_path
        if not target.is_absolute():
            target = (ROOT_DIR / target).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(self.export_config(), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        return target

    def apply_config(self, config: Dict[str, Any], replace: bool = True) -> None:
        self.defaults.update(normalize_defaults(config, self.defaults))
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
            return {"kind": "ack", "action": action, "ok": True, "topic": spec.to_meta(active=True)}
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
            return {"kind": "ack", "action": action, "ok": True, "config": self.export_config()}
        if action == "save_config":
            path = self.save_config(command.get("path"))
            return {"kind": "ack", "action": action, "ok": True, "path": str(path)}
        if action == "load_config":
            config = load_config_file(self.config_path)
            self.apply_config(config, replace=True)
            return {"kind": "ack", "action": action, "ok": True, "config": self.export_config()}
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
        transport = str(raw.get("transport", defaults.get("transport", "socket"))).strip()
        if transport not in {"shm", "socket"}:
            raise ValueError("transport must be shm or socket")
        return cls(
            topic=topic,
            msg_type=msg_type,
            domain=int(raw.get("domain", defaults.get("domain", 1))),
            queue=int(raw.get("queue", defaults.get("queue", 10))),
            transport=transport,
            poll=float(raw.get("poll", defaults.get("poll", 0.03))),
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
    return TopicSpec.from_config({"topic": topic.strip(), "msg_type": msg_type.strip()}, defaults)


def normalize_defaults(config: Dict[str, Any], fallback: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    fallback = fallback or {}
    transport = str(config.get("transport", fallback.get("transport", "socket")))
    if transport not in {"shm", "socket"}:
        transport = "socket"
    return {
        "domain": int(config.get("domain", fallback.get("domain", 1))),
        "transport": transport,
        "queue": int(config.get("queue", fallback.get("queue", 10))),
        "poll": float(config.get("poll", fallback.get("poll", 0.03))),
        "extra": str(config.get("extra", fallback.get("extra", ""))),
        "verbose": bool(config.get("verbose", fallback.get("verbose", False))),
    }


def load_config_file(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"config must be a JSON object: {path}")
    return data


def discover_message_types() -> List[str]:
    return VISUALIZER_MESSAGE_TYPES[:]


def resolve_message_class(ipc: Any, msg_type: str) -> Tuple[str, Any]:
    class_name = VISUALIZER_MESSAGE_ALIASES.get(msg_type, msg_type)
    allowed = set(VISUALIZER_MESSAGE_ALIASES) | set(VISUALIZER_MESSAGE_ALIASES.values())
    if msg_type not in allowed:
        raise ValueError(
            "Visualizer only supports std message display types: " + ", ".join(VISUALIZER_MESSAGE_TYPES)
        )
    return class_name, getattr(ipc, class_name)


class DzipcSubscriber(threading.Thread):
    def __init__(self, hub: WebHub, spec: TopicSpec) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.spec = spec
        self.stop_event = threading.Event()

    def run(self) -> None:
        try:
            ipc = load_dzipc()
            resolved_msg_type, msg_cls = resolve_message_class(ipc, self.spec.msg_type)
            template = msg_cls()
            topic_data = ipc.make_topic_data(template)
            ipc_type = ipc.IPC_SHM if self.spec.transport == "shm" else ipc.IPC_SOCKET
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
            self.hub.publish(
                {
                    "kind": "error",
                    "topic": self.spec.topic,
                    "message": str(exc),
                    "timestamp_ms": now_ms(),
                }
            )
            return

        while not self.stop_event.is_set():
            try:
                ok, out = sub.try_get(topic_data)
                if not ok:
                    time.sleep(self.spec.poll)
                    continue
                msg_obj = out.topic() if out is not None else topic_data.topic()
                if hasattr(msg_cls, "from_generic") and hasattr(msg_obj, "field_count"):
                    msg_obj = msg_cls.from_generic(msg_obj)
                self.hub.publish(
                    {
                        "kind": "sample",
                        "topic": self.spec.topic,
                        "msg_type": self.spec.msg_type,
                        "resolved_msg_type": resolved_msg_type,
                        "transport": self.spec.transport,
                        "domain": self.spec.domain,
                        "queue": self.spec.queue,
                        "extra": self.spec.extra,
                        "timestamp_ms": now_ms(),
                        "data": to_jsonable(msg_obj),
                    }
                )
            except Exception as exc:
                self.hub.publish(
                    {
                        "kind": "error",
                        "topic": self.spec.topic,
                        "message": str(exc),
                        "timestamp_ms": now_ms(),
                    }
                )
                time.sleep(max(self.spec.poll, 0.2))


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


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, hub: WebHub) -> None:
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
        writer.write(http_response("400 Bad Request", {"Content-Type": "text/plain"}, b"bad request"))
        await writer.drain()
        writer.close()
        return

    if method == "GET" and path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
        key = headers.get("sec-websocket-key", "")
        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
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
                        response = {"kind": "ack", "ok": False, "message": str(exc), "timestamp_ms": now_ms()}
                    await hub.send(writer, response)
        except Exception:
            pass
        await hub.remove_client(writer)
        return

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
    print(f"[dzipc-web] VS Code Remote: Ctrl+Click {forwarded_url}", flush=True)
    if host not in {"127.0.0.1", "localhost"}:
        print(f"[dzipc-web] LAN:  http://{local_ip_hint()}:{port}", flush=True)


async def run_server(args: argparse.Namespace) -> None:
    config_path = Path(args.config).expanduser()
    if not config_path.is_absolute():
        config_path = (ROOT_DIR / config_path).resolve()
    file_config = load_config_file(config_path)
    defaults = normalize_defaults(
        {
            **file_config,
            "domain": args.domain if args.domain is not None else file_config.get("domain", 1),
            "transport": args.transport or file_config.get("transport", "socket"),
            "queue": args.queue if args.queue is not None else file_config.get("queue", 10),
            "poll": args.poll if args.poll is not None else file_config.get("poll", 0.03),
            "extra": args.extra if args.extra is not None else file_config.get("extra", ""),
            "verbose": args.verbose or bool(file_config.get("verbose", False)),
        }
    )

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

    # 检查端口是否被占用
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind((args.host, args.port))
    except OSError as e:
        print(f"\033[91m[dzipc-web] ERROR: Cannot bind to {args.host}:{args.port}: {e}\033[0m", flush=True)
        print(f"[dzipc-web] You can input \"lsof -i :{args.port} and kill the process\".", flush=True)
        sys.exit(1)

    server = await asyncio.start_server(lambda r, w: handle_client(r, w, hub), args.host, args.port)
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
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="JSON config path")
    parser.add_argument("--load-config", action="store_true", help="Load topics from --config on startup")
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
    parser.add_argument("--demo", action="store_true", help="Publish demo data without importing dzipc")
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
