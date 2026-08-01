#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import importlib.util
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
import urllib.request
from pathlib import Path
from typing import Any, Dict, Iterable, Optional, Tuple
from unittest import mock


TEST_DIR = Path(__file__).resolve().parent
VISUALIZER_DIR = TEST_DIR.parent
ROOT_DIR = VISUALIZER_DIR.parents[1]
BRIDGE_PATH = VISUALIZER_DIR / "dzipc_web_bridge.py"
WEB_DIR = VISUALIZER_DIR / "web"
DEMO_DIR = VISUALIZER_DIR / "demo"
DEFAULT_VISUALIZER_CONFIG = VISUALIZER_DIR / "config.json"
if str(DEMO_DIR) not in sys.path:
    sys.path.insert(0, str(DEMO_DIR))

EXPECTED_DISPLAY_TYPES = ["RawMessage", "Pose", "Path", "PointCloud", "Marker", "Image", "TF", "Robot", "RobotState"]
EXPECTED_MESSAGE_MAP = {
    "RawMessage": "StdRawMessage",
    "Pose": "StdPose",
    "Path": "StdPath",
    "PointCloud": "StdPointCloud",
    "Marker": "StdMarker",
    "Image": "StdImage",
    "TF": "StdTF",
    "RobotState": "RobotState",
}


def load_bridge_module():
    spec = importlib.util.spec_from_file_location("dzipc_web_bridge_under_test", BRIDGE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {BRIDGE_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


bridge = load_bridge_module()
from component import point_clouds as point_cloud_component  # noqa: E402
from component import robot as robot_component  # noqa: E402

robot_state_demo_spec = importlib.util.spec_from_file_location(
    "robot_state_demo_under_test", VISUALIZER_DIR / "demo" / "send_robot_state.py"
)
if robot_state_demo_spec is None or robot_state_demo_spec.loader is None:
    raise RuntimeError("cannot load robot state demo")
robot_state_demo = importlib.util.module_from_spec(robot_state_demo_spec)
sys.modules[robot_state_demo_spec.name] = robot_state_demo
robot_state_demo_spec.loader.exec_module(robot_state_demo)


KINPY_AVAILABLE = True
try:
    robot_component.RobotKinematics._require_kinpy()
except RuntimeError:
    KINPY_AVAILABLE = False


ROBOT_KINEMATICS_URDF = """<?xml version="1.0"?>
<robot name="arm">
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


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def http_get_text(port: int, path: str) -> Tuple[int, str, str]:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(f"http://127.0.0.1:{port}{path}", timeout=3) as response:
        body = response.read().decode("utf-8", errors="replace")
        return response.status, response.headers.get("Content-Type", ""), body


def start_bridge_server(port: int, config_path: Path) -> subprocess.Popen[str]:
    env = os.environ.copy()
    env.update(
        {
            "PYTHONUNBUFFERED": "1",
            "no_proxy": "127.0.0.1,localhost",
            "NO_PROXY": "127.0.0.1,localhost",
        }
    )
    return subprocess.Popen(
        [
            sys.executable,
            str(BRIDGE_PATH),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--config",
            str(config_path),
            "--demo",
            "--demo-period",
            "0.02",
        ],
        cwd=str(ROOT_DIR),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def wait_for_http(port: int, process: subprocess.Popen[str], timeout_s: float = 5.0) -> None:
    deadline = time.time() + timeout_s
    last_error: Optional[BaseException] = None
    while time.time() < deadline:
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout else ""
            raise RuntimeError(f"bridge exited early with code {process.returncode}\n{output}")
        try:
            status, _, _ = http_get_text(port, "/")
            if status == 200:
                return
        except BaseException as exc:
            last_error = exc
        time.sleep(0.05)
    raise TimeoutError(f"bridge did not answer on port {port}: {last_error}")


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def load_playwright():
    try:
        from playwright.sync_api import expect, sync_playwright
    except Exception as exc:  # pragma: no cover - depends on local environment.
        raise unittest.SkipTest(f"playwright is not available: {exc}") from exc
    return expect, sync_playwright


def websocket_connect(port: int) -> socket.socket:
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    request = (
        "GET /ws HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("websocket handshake closed")
        response += chunk
    if not response.startswith(b"HTTP/1.1 101"):
        raise ConnectionError(response.decode("utf-8", errors="replace"))
    return sock


def ws_read_frame(sock: socket.socket, timeout_s: float = 3.0) -> Tuple[int, bytes]:
    sock.settimeout(timeout_s)
    header = sock.recv(2)
    if len(header) != 2:
        raise ConnectionError("short websocket frame header")
    opcode = header[0] & 0x0F
    size = header[1] & 0x7F
    if size == 126:
        size = struct.unpack("!H", sock.recv(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", sock.recv(8))[0]
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            raise ConnectionError("websocket frame closed")
        payload += chunk
    return opcode, payload


def ws_read_json(sock: socket.socket, timeout_s: float = 3.0) -> Dict[str, Any]:
    opcode, payload = ws_read_frame(sock, timeout_s)
    if opcode != 1:
        raise ValueError(f"expected text frame, got opcode {opcode}")
    return json.loads(payload.decode("utf-8"))


def ws_send_json(sock: socket.socket, payload: Dict[str, Any]) -> None:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    mask = os.urandom(4)
    if len(raw) < 126:
        frame = bytearray([0x81, 0x80 | len(raw)])
    elif len(raw) <= 0xFFFF:
        frame = bytearray([0x81, 0x80 | 126]) + struct.pack("!H", len(raw))
    else:
        frame = bytearray([0x81, 0x80 | 127]) + struct.pack("!Q", len(raw))
    frame += mask
    frame += bytes(byte ^ mask[index % 4] for index, byte in enumerate(raw))
    sock.sendall(frame)


def read_until_kind(sock: socket.socket, kind: str, timeout_s: float = 3.0) -> Dict[str, Any]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        event = ws_read_json(sock, max(0.05, deadline - time.time()))
        if event.get("kind") == kind:
            return event
    raise TimeoutError(f"did not receive websocket event kind={kind!r}")


class BackendComponentTests(unittest.TestCase):
    def test_message_types_use_std_message_contract(self) -> None:
        self.assertEqual(bridge.discover_message_types(), EXPECTED_DISPLAY_TYPES)
        self.assertEqual(bridge.VISUALIZER_MESSAGE_ALIASES, EXPECTED_MESSAGE_MAP)

    def test_resolve_message_class_accepts_alias_and_resolved_class(self) -> None:
        fake_ipc = types.SimpleNamespace(
            StdRawMessage=type("StdRawMessage", (), {}),
            StdPose=type("StdPose", (), {}),
            StdPath=type("StdPath", (), {}),
            StdPointCloud=type("StdPointCloud", (), {}),
            StdMarker=type("StdMarker", (), {}),
            StdImage=type("StdImage", (), {}),
            StdTF=type("StdTF", (), {}),
            RobotState=type("RobotState", (), {}),
        )
        for display_type, class_name in EXPECTED_MESSAGE_MAP.items():
            resolved_name, resolved_class = bridge.resolve_message_class(fake_ipc, display_type)
            self.assertEqual(resolved_name, class_name)
            self.assertIs(resolved_class, getattr(fake_ipc, class_name))
            resolved_name, resolved_class = bridge.resolve_message_class(fake_ipc, class_name)
            self.assertEqual(resolved_name, class_name)
            self.assertIs(resolved_class, getattr(fake_ipc, class_name))
        with self.assertRaisesRegex(ValueError, "std message display types"):
            bridge.resolve_message_class(fake_ipc, "Robot")

    def test_resolve_message_class_rejects_unknown_display_types(self) -> None:
        fake_ipc = types.SimpleNamespace(CustomState=object)
        with self.assertRaisesRegex(ValueError, "Visualizer only supports std message display types"):
            bridge.resolve_message_class(fake_ipc, "CustomState")

    def test_topic_spec_defaults_and_validation(self) -> None:
        defaults = {"domain": 3, "queue": 4, "transport": "socket", "poll": 0.05, "extra": "x"}
        spec = bridge.TopicSpec.from_config({"topic": "pose", "msg_type": "Pose"}, defaults)
        self.assertEqual(spec.topic, "pose")
        self.assertEqual(spec.msg_type, "Pose")
        self.assertEqual(spec.domain, 3)
        self.assertEqual(spec.queue, 4)
        self.assertEqual(spec.transport, "socket")
        self.assertEqual(spec.poll, 0.05)
        self.assertEqual(spec.to_meta()["type"], "Pose")

        zero_poll_spec = bridge.TopicSpec.from_config(
            {"topic": "pose", "msg_type": "Pose", "poll": 0},
            defaults,
        )
        self.assertEqual(zero_poll_spec.poll, bridge.MIN_POLL_INTERVAL_S)

        with self.assertRaisesRegex(ValueError, "topic and msg_type"):
            bridge.TopicSpec.from_config({"topic": "pose"}, defaults)
        with self.assertRaisesRegex(ValueError, "transport"):
            bridge.TopicSpec.from_config({"topic": "pose", "msg_type": "Pose", "transport": "udp"}, defaults)

    def test_config_normalization_and_json_conversion(self) -> None:
        normalized = bridge.normalize_defaults({"domain": "2", "queue": "8", "transport": "udp"}, {})
        self.assertEqual(normalized["domain"], 2)
        self.assertEqual(normalized["queue"], 8)
        self.assertEqual(normalized["transport"], "socket")
        self.assertEqual(
            bridge.normalize_defaults({"poll": 0}, {})["poll"],
            bridge.MIN_POLL_INTERVAL_S,
        )

        robot_models = bridge.normalize_robot_models(
            [
                {
                    "id": "arm",
                    "name": "Arm",
                    "filename": "arm.urdf",
                    "urdfText": "<robot name='arm'/>",
                    "visible": False,
                    "fixed_frame": "world",
                }
            ]
        )
        self.assertEqual(robot_models[0]["id"], "arm")
        self.assertEqual(robot_models[0]["file_name"], "arm.urdf")
        self.assertEqual(robot_models[0]["urdf"], "<robot name='arm'/>")
        self.assertFalse(robot_models[0]["visible"])
        self.assertEqual(robot_models[0]["fixed_frame"], "world")
        with self.assertRaisesRegex(ValueError, "robot_models"):
            bridge.normalize_robot_models({"name": "arm"})

    def test_robot_display_collects_embedded_mesh_assets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="robot_mesh_assets_") as temp_dir:
            temp_path = Path(temp_dir)
            package_dir = temp_path / "go2"
            assets_dir = package_dir / "assets"
            assets_dir.mkdir(parents=True)
            obj_path = assets_dir / "tri.obj"
            obj_text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
            obj_path.write_text(obj_text, encoding="utf-8")
            stl_path = assets_dir / "tri.stl"
            stl_text = (
                "solid tri\n"
                "facet normal 0 0 1\nouter loop\n"
                "vertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                "endloop\nendfacet\nendsolid tri\n"
            )
            stl_path.write_text(stl_text, encoding="utf-8")
            urdf_path = package_dir / "go2.urdf"
            urdf = """<robot name="go2">
  <link name="base">
    <visual><geometry><mesh filename="assets/tri.obj"/></geometry></visual>
    <collision><geometry><mesh filename="package://go2/assets/tri.stl"/></geometry></collision>
  </link>
</robot>"""
            urdf_path.write_text(urdf, encoding="utf-8")

            robot = robot_component.RobotDisplay(id="go2", name="Go2")
            result = robot.set_urdf_path(urdf_path, root_dir=temp_path)

            self.assertTrue(result["ok"], result)
            self.assertIn("assets/tri.obj", robot.mesh_assets)
            self.assertIn("package://go2/assets/tri.stl", robot.mesh_assets)
            self.assertEqual(robot.mesh_assets["assets/tri.obj"]["format"], "obj")
            self.assertEqual(robot.mesh_assets["package://go2/assets/tri.stl"]["format"], "stl")
            self.assertEqual(Path(robot.mesh_assets["assets/tri.obj"]["path"]).read_text(encoding="utf-8"), obj_text)
            self.assertEqual(robot.to_config()["mesh_assets"]["assets/tri.obj"]["byte_length"], len(obj_text.encode("utf-8")))
            self.assertNotIn("data", robot.to_config()["mesh_assets"]["assets/tri.obj"])

    def test_webhub_hydrates_robot_urdf_path_with_mesh_assets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="robot_mesh_assets_", dir=str(VISUALIZER_DIR)) as temp_dir:
            temp_path = Path(temp_dir)
            assets_dir = temp_path / "assets"
            assets_dir.mkdir()
            (assets_dir / "tri.obj").write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
            urdf_path = temp_path / "mesh_bot.urdf"
            urdf_path.write_text(
                "<robot name='mesh_bot'><link name='base'><visual><geometry>"
                "<mesh filename='assets/tri.obj'/></geometry></visual></link></robot>",
                encoding="utf-8",
            )
            relative_urdf = str(urdf_path.relative_to(ROOT_DIR))
            hub = bridge.WebHub(
                {
                    "domain": 1,
                    "transport": "socket",
                    "queue": 10,
                    "poll": 0.03,
                    "extra": "",
                    "verbose": False,
                    "robot_displays": [{"id": "mesh_bot", "name": "Mesh Bot", "urdf_path": relative_urdf}],
                },
                temp_path / "config.json",
            )

            exported = hub.export_config()["robot_displays"][0]
            self.assertIn("<robot name='mesh_bot'>", exported["urdf"])
            self.assertIn("assets/tri.obj", exported["mesh_assets"])
            self.assertTrue(exported["mesh_assets"]["assets/tri.obj"]["url"].startswith("/robot_assets/"))
            self.assertEqual(exported["mesh_assets"]["assets/tri.obj"]["type"], "obj")
            self.assertNotIn("data", exported["mesh_assets"]["assets/tri.obj"])

    def test_webhub_config_paths_are_resolved_on_backend(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dzipc_config_path_test_") as temp_dir:
            temp_path = Path(temp_dir)
            default_path = temp_path / "default.json"
            default_path.write_text("{}", encoding="utf-8")
            import_path = temp_path / "import.json"
            import_path.write_text(
                json.dumps({"domain": 12, "transport": "socket", "queue": 6, "poll": 0.05, "topics": []}),
                encoding="utf-8",
            )
            export_path = temp_path / "export.json"
            hub = bridge.WebHub(
                {"domain": 1, "transport": "socket", "queue": 10, "poll": 0.03, "extra": "", "verbose": False},
                default_path,
            )

            save_ack = hub.handle_command(
                {
                    "action": "save_config",
                    "path": str(export_path),
                    "config": {"domain": 4, "transport": "shm", "queue": 2, "poll": 0.04, "topics": []},
                }
            )
            self.assertTrue(save_ack["ok"], save_ack)
            self.assertEqual(save_ack["path"], str(export_path.resolve()))
            self.assertEqual(json.loads(export_path.read_text(encoding="utf-8"))["domain"], 4)

            load_ack = hub.handle_command({"action": "load_config", "path": str(import_path)})
            self.assertTrue(load_ack["ok"], load_ack)
            self.assertEqual(load_ack["path"], str(import_path.resolve()))
            self.assertEqual(load_ack["config"]["domain"], 12)

            directory_path = temp_path / "nested"
            directory_path.mkdir()
            self.assertEqual(hub.resolve_config_path(str(directory_path)), directory_path / "config.json")

    def test_go2_robot_mesh_assets_are_exposed_as_backend_urls(self) -> None:
        go2_urdf = VISUALIZER_DIR / "demo" / "robots" / "go2" / "go2.urdf"
        if not go2_urdf.exists():
            self.skipTest("go2 demo robot is not available")
        hub = bridge.WebHub(
            {"domain": 1, "transport": "socket", "queue": 10, "poll": 0.03, "extra": "", "verbose": False},
            VISUALIZER_DIR / "config.json",
        )
        robot = hub.add_robot_display({"id": "go2", "name": "go2", "urdf_path": str(go2_urdf)})
        self.assertGreater(len(robot.mesh_assets), 0)
        base_asset = robot.mesh_assets.get("assets/base_0.obj")
        self.assertIsNotNone(base_asset)
        self.assertEqual(base_asset["format"], "obj")
        self.assertTrue(base_asset["url"].startswith("/robot_assets/"))
        self.assertIn("hash", base_asset)
        self.assertGreater(base_asset["byte_length"], 0)
        self.assertNotIn("data", base_asset)

    @unittest.skipUnless(KINPY_AVAILABLE, "kinpy is not available")
    def test_robot_display_bundle_normalization(self) -> None:
        robots = robot_component.normalize_robot_displays(
            [
                {
                    "id": "arm",
                    "name": "Arm",
                    "urdf_path": "arm.urdf",
                    "urdf": "<robot name='arm'/>",
                    "visible": False,
                }
            ]
        )
        states = robot_component.normalize_robot_state_displays(
            [
                {
                    "id": "arm_state",
                    "topic": "joint_states",
                    "robot_id": "arm",
                    "msg_type": "RobotState",
                }
            ]
        )
        self.assertEqual(robots[0]["id"], "arm")
        self.assertEqual(robots[0]["urdf_path"], "arm.urdf")
        self.assertEqual(states[0]["robot_id"], "arm")
        bundle_displays, bundle_states = robot_component.parse_robot_display_bundle(
            {"robot_displays": robots, "robot_state_displays": states}
        )
        self.assertEqual(bundle_displays[0]["id"], "arm")
        self.assertEqual(bundle_states[0]["id"], "arm_state")

        with tempfile.TemporaryDirectory() as tmp:
            urdf_path = Path(tmp) / "arm.urdf"
            urdf_path.write_text(ROBOT_KINEMATICS_URDF, encoding="utf-8")
            robot = robot_component.RobotDisplay.from_config({"id": "arm", "name": "Arm"})
            result = robot.handle_urdf_path_request(str(urdf_path), Path(tmp))
            self.assertTrue(result["ok"], result)
            self.assertIn("base_link", robot.urdf_text)
            state_display = robot_component.RobotStateDisplay.from_config(
                {"id": "arm_state", "topic": "/robot_state", "robot_id": "arm", "msg_type": "RobotState"},
                {"domain": 1, "transport": "socket", "queue": 3, "poll": 0.05},
            )
            robot.bind_robot_state(state_display)
            state_display.handle_joint_state({"joint_state": {"name": ["j1"], "position": [1.0]}})
            robot.handle_joint_state({"joint_state": {"name": ["joint1"], "position": [2.0]}})
            self.assertEqual(robot.robot_state, state_display)
            self.assertEqual(len(robot.joint_state_records), 1)
            self.assertEqual(len(state_display.joint_state_records), 2)
            self.assertEqual(robot.to_config()["urdf_path"], str(Path(tmp).resolve() / "arm.urdf"))
            self.assertEqual(state_display.to_config()["robot_id"], "arm")

    @unittest.skipUnless(KINPY_AVAILABLE, "kinpy is not available")
    def test_robot_display_reads_urdf_path_and_binds_state(self) -> None:
        with tempfile.TemporaryDirectory(prefix="robot_display_test_") as temp_dir:
            temp_path = Path(temp_dir)
            urdf_path = temp_path / "fixture.urdf"
            urdf_path.write_text(ROBOT_KINEMATICS_URDF, encoding="utf-8")

            robot = robot_component.RobotDisplay(id="fixture", name="fixture")
            result = robot.set_urdf_path(urdf_path, base_dir=temp_path)
            self.assertTrue(result["ok"], result)
            self.assertEqual(robot.urdf_path, str(urdf_path.resolve()))
            self.assertIn("<robot name=\"arm\">", robot.urdf_text)

            robot_state = robot_component.RobotStateDisplay(id="state_1", topic="joint_states")
            robot.attach_robot_state_display(robot_state)
            callback = robot_state.handle_joint_state_sample({"name": ["joint1"], "position": [1.0]})
            self.assertEqual(robot_state.target_robot_id, robot.id)
            self.assertEqual(robot_state.latest_joint_state, {"name": ["joint1"], "position": [1.0]})
            self.assertTrue(callback["ok"])
            self.assertEqual(callback["target_robot_id"], robot.id)
            self.assertEqual(callback["joint_names"], ["joint1"])
            self.assertEqual({"base_link", "link1", "tool0"}, {item["name"] for item in callback["link_states"]})

            robot_result = robot.handle_joint_state_sample({"name": ["joint1"], "position": [2.0]})
            self.assertEqual(robot_result["robot_state_ids"], [robot_state.id])
            self.assertEqual(robot_state.latest_joint_state, {"name": ["joint1"], "position": [2.0]})

        class SlotMessage:
            __slots__ = ("x", "values", "_hidden")

            def __init__(self) -> None:
                self.x = 1.25
                self.values = [float("nan"), {"ok": True}]
                self._hidden = "skip"

        self.assertEqual(bridge.to_jsonable(SlotMessage()), {"x": 1.25, "values": [None, {"ok": True}]})

    @unittest.skipUnless(KINPY_AVAILABLE, "kinpy is not available")
    def test_robot_state_rejects_joint_length_mismatch_without_overwriting_latest_links(self) -> None:
        robot = robot_component.RobotDisplay(id="arm", name="Arm", urdf_text=ROBOT_KINEMATICS_URDF)
        robot_state = robot_component.RobotStateDisplay(id="arm_state", topic="/joint_states", robot_id="arm")
        robot.attach_robot_state_display(robot_state)

        ok = robot_state.handle_joint_state_sample({"name": ["joint1"], "position": [0.5]})
        self.assertTrue(ok["ok"], ok)
        latest_links = list(robot.latest_link_states)
        self.assertTrue(latest_links)

        bad = robot_state.handle_joint_state_sample({"name": ["joint1", "extra"], "position": [0.5, 1.0]})
        self.assertFalse(bad["ok"])
        self.assertIn("length mismatch", bad["message"])
        self.assertEqual(robot.latest_link_states, latest_links)
        self.assertEqual(robot_state.latest_link_states, latest_links)

    @unittest.skipUnless(KINPY_AVAILABLE, "kinpy is not available")
    def test_robot_state_extracts_joint_state_from_note_json(self) -> None:
        robot = robot_component.RobotDisplay(id="arm", name="Arm", urdf_text=ROBOT_KINEMATICS_URDF)
        robot_state = robot_component.RobotStateDisplay(id="arm_state", topic="/joint_states", robot_id="arm")
        robot.attach_robot_state_display(robot_state)
        sample = {
            "name": "arm",
            "note": json.dumps(
                {
                    "joint_state": {
                        "name": ["joint1"],
                        "position": [0.25],
                    }
                }
            ),
        }

        result = robot_state.handle_joint_state_sample(sample)

        self.assertTrue(result["ok"], result)
        self.assertEqual(result["joint_names"], ["joint1"])
        self.assertEqual(result["joint_positions"], [0.25])
        self.assertTrue(result["link_states"])

    def test_robot_state_demo_prepare_offline_config_and_urdf_topic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="robot_state_demo_test_") as temp_dir:
            temp_path = Path(temp_dir)
            args = robot_state_demo.build_parser().parse_args(
                [
                    "--prepare-only",
                    "--no-download",
                    "--force-download",
                    "--resource-dir",
                    str(temp_path / "robots"),
                    "--config-out",
                    str(temp_path / "config.json"),
                    "--topic",
                    "/demo/test_robot_state",
                    "--urdf-topic",
                    "/demo/test_robot_urdf",
                ]
            )

            robot_state_demo.publish_robot_states(args)

            config = json.loads((temp_path / "config.json").read_text(encoding="utf-8"))
            topics = {item["topic"]: item for item in config["topics"]}
            self.assertEqual(topics["/demo/test_robot_state"]["msg_type"], "RobotState")
            self.assertEqual(topics["/demo/test_robot_urdf"]["msg_type"], "RawMessage")
            self.assertTrue((temp_path / "robots" / "tiny_2dof_arm.urdf").exists())
            self.assertIn("<robot", config["robot_displays"][0]["urdf"])
            self.assertEqual(config["robot_state_displays"][0]["topic"], "/demo/test_robot_state")

    def test_point_cloud_binary_encoding_uses_dzpc_frame_layout(self) -> None:
        msg = types.SimpleNamespace(
            header=types.SimpleNamespace(frame_id="map"),
            points=[
                types.SimpleNamespace(data=[1.0, 2.0, 3.0]),
                types.SimpleNamespace(x=4.0, y=5.0, z=6.0),
            ],
            colors=[
                types.SimpleNamespace(r=1.0, g=0.5, b=0.25, a=1.0),
                types.SimpleNamespace(r=0.0, g=0.25, b=0.75, a=0.5),
            ],
            channel_names=["intensity"],
            channels=[0.1, 0.2],
        )

        metadata, payload = bridge.encode_point_cloud_binary(msg, sample_id=42)
        header = bridge.BINARY_HEADER.unpack_from(payload, 0)
        self.assertEqual(header[:3], (bridge.BINARY_MAGIC, bridge.BINARY_VERSION, bridge.BINARY_POINT_CLOUD))
        self.assertEqual(header[3], bridge.BINARY_POINT_CLOUD_HAS_COLORS)
        self.assertEqual(header[4:7], (42, 2, 2))
        self.assertEqual(metadata["encoding"], "dzpc.pointcloud.v1")
        self.assertEqual(metadata["point_count"], 2)
        self.assertTrue(metadata["has_colors"])
        self.assertEqual(metadata["byte_length"], len(payload))

        points = struct.unpack_from("<ffffff", payload, bridge.BINARY_HEADER.size)
        self.assertEqual(points, (1.0, 2.0, 3.0, 4.0, 5.0, 6.0))
        colors = struct.unpack_from("<ffffffff", payload, bridge.BINARY_HEADER.size + 24)
        self.assertEqual(colors, (1.0, 0.5, 0.25, 1.0, 0.0, 0.25, 0.75, 0.5))
        self.assertEqual(
            bridge.lightweight_point_cloud_data(msg),
            {"header": {"frame_id": "map"}, "channel_names": ["intensity"], "binary_points": True},
        )
        event_fields = point_cloud_component.PointCloudSampleEncoder().encode_event_fields(msg, sample_id=7)
        self.assertEqual(event_fields["binary"]["sample_id"], 7)
        self.assertEqual(event_fields["data"]["binary_points"], True)
        self.assertIsInstance(event_fields["binary_payload"], bytes)

    def test_webhub_send_event_sends_json_then_binary_frame(self) -> None:
        class DummyWriter:
            def __init__(self) -> None:
                self.data = bytearray()

            def write(self, data: bytes) -> None:
                self.data.extend(data)

            async def drain(self) -> None:
                return None

        writer = DummyWriter()
        event = {
            "kind": "sample",
            "topic": "cloud",
            "data": {"binary_points": True},
            "binary_payload": b"abc",
        }

        hub = bridge.WebHub({}, BRIDGE_PATH)
        asyncio.run(hub.send_event(writer, event))
        raw = bytes(writer.data)
        first_size = raw[1] & 0x7F
        text_payload = raw[2 : 2 + first_size]
        binary_offset = 2 + first_size
        self.assertEqual(raw[0] & 0x0F, 1)
        self.assertNotIn("binary_payload", json.loads(text_payload.decode("utf-8")))
        self.assertEqual(raw[binary_offset] & 0x0F, 2)
        self.assertEqual(raw[binary_offset + 2 :], b"abc")

    def test_remove_topic_does_not_unlink_live_publisher_shm(self) -> None:
        class FakeWorker:
            def __init__(self) -> None:
                self.stop_event = bridge.threading.Event()
                self.joined = False

            def join(self, timeout: float) -> None:
                self.joined = True

        hub = bridge.WebHub({}, BRIDGE_PATH)
        worker = FakeWorker()
        hub.subscribers["/demo/depth_image"] = worker
        hub.topics["/demo/depth_image"] = {"name": "/demo/depth_image", "active": True}

        with mock.patch.object(bridge.DzipcSubscriber, "_clean_shm_for_topic") as clean_shm:
            hub.remove_topic("/demo/depth_image")

        self.assertTrue(worker.stop_event.is_set())
        self.assertTrue(worker.joined)
        clean_shm.assert_not_called()
        self.assertFalse(hub.topics["/demo/depth_image"]["active"])

    def test_readd_correct_image_after_wrong_type_recovers_without_shm_cleanup(self) -> None:
        created_workers = []
        clean_shm = mock.Mock()

        class FakeSubscriber:
            _clean_shm_for_topic = clean_shm

            def __init__(self, hub: Any, spec: Any, cooldown_remain: float = 0.0,
                         ipc_loader: Any = None, msg_resolver: Any = None,
                         image_encoder: Any = None, data_serializer: Any = None,
                         field_getter: Any = None, time_ms: Any = None,
                         **_: Any) -> None:
                self.hub = hub
                self.spec = spec
                self.cooldown_remain = cooldown_remain
                self.stop_event = bridge.threading.Event()
                self.started = False
                self.joined = False
                created_workers.append(self)

            def start(self) -> None:
                self.started = True
                if self.spec.msg_type == "Image":
                    self.hub.publish(
                        {
                            "kind": "sample",
                            "topic": self.spec.topic,
                            "msg_type": "Image",
                            "resolved_msg_type": "StdImage",
                            "data": {"width": 2, "height": 1},
                            "timestamp_ms": bridge.now_ms(),
                        }
                    )

            def join(self, timeout: float) -> None:
                self.joined = True

        hub = bridge.WebHub({}, BRIDGE_PATH)
        published_events = []

        def capture_event(event: Dict[str, Any]) -> None:
            published_events.append(event)
            hub.events.put(event)

        hub.publish = capture_event
        with mock.patch.object(bridge, "DzipcSubscriber", FakeSubscriber):
            wrong_ack = hub.handle_command(
                {
                    "action": "add_topic",
                    "topic": {
                        "topic": "/demo/depth_image",
                        "msg_type": "Pose",
                        "transport": "shm",
                    },
                }
            )
            remove_ack = hub.handle_command(
                {"action": "remove_topic", "topic": "/demo/depth_image"}
            )
            correct_ack = hub.handle_command(
                {
                    "action": "add_topic",
                    "topic": {
                        "topic": "/demo/depth_image",
                        "msg_type": "Image",
                        "transport": "shm",
                    },
                }
            )

        self.assertTrue(wrong_ack["ok"])
        self.assertTrue(remove_ack["ok"])
        self.assertTrue(correct_ack["ok"])
        self.assertEqual([worker.spec.msg_type for worker in created_workers], ["Pose", "Image"])
        self.assertTrue(created_workers[0].stop_event.is_set())
        self.assertTrue(created_workers[0].joined)
        self.assertTrue(created_workers[1].started)
        self.assertGreater(created_workers[1].cooldown_remain, 0.0)
        clean_shm.assert_not_called()
        self.assertEqual(hub.topics["/demo/depth_image"]["type"], "Image")
        self.assertTrue(hub.topics["/demo/depth_image"]["active"])
        image_samples = [
            event
            for event in published_events
            if event.get("kind") == "sample" and event.get("topic") == "/demo/depth_image"
        ]
        self.assertEqual(len(image_samples), 1)
        self.assertEqual(image_samples[0]["resolved_msg_type"], "StdImage")

    def test_static_path_rejects_traversal(self) -> None:
        index_path = bridge.safe_static_path("/")
        self.assertEqual(index_path.name, "index.html")
        self.assertTrue(str(index_path).startswith(str(WEB_DIR)))
        self.assertEqual(bridge.safe_static_path("/../../etc/passwd").name, "index.html")

    def test_port_in_use_hint_is_actionable(self) -> None:
        hint = bridge.port_in_use_hint(8765)
        self.assertIn("Port 8765 is already in use", hint)
        self.assertIn("`lsof -i :8765`", hint)
        self.assertIn("`kill <PID>`", hint)
        self.assertIn("`--port 8766`", hint)
        self.assertNotIn('input "lsof', hint)

    def test_run_server_uses_reusable_asyncio_bind_without_prebinding(self) -> None:
        class FakeServer:
            class Socket:
                def getsockname(self) -> Tuple[str, int]:
                    return ("127.0.0.1", 8765)

            sockets = [Socket()]

            async def __aenter__(self) -> "FakeServer":
                return self

            async def __aexit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
                return None

            async def serve_forever(self) -> None:
                raise asyncio.CancelledError()

            def close(self) -> None:
                return None

            async def wait_closed(self) -> None:
                return None

        async def fake_start_server(*args: Any, **kwargs: Any) -> FakeServer:
            return FakeServer()

        args = types.SimpleNamespace(
            config=str(bridge.DEFAULT_CONFIG),
            domain=None,
            transport=None,
            queue=None,
            poll=None,
            extra=None,
            verbose=False,
            demo=False,
            demo_period=0.1,
            load_config=False,
            topic=[],
            host="127.0.0.1",
            port=8765,
        )

        with mock.patch.object(bridge.asyncio, "start_server", side_effect=fake_start_server) as start_server:
            with mock.patch.object(bridge.socket, "socket") as socket_factory:
                with mock.patch.object(bridge, "print_startup_links"):
                    with self.assertRaises(asyncio.CancelledError):
                        asyncio.run(bridge.run_server(args))

        socket_factory.return_value.bind.assert_not_called()
        self.assertTrue(start_server.called)
        self.assertTrue(start_server.call_args.kwargs["reuse_address"])


class FrontendStaticComponentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.html = read_text(WEB_DIR / "index.html")
        cls.js = read_text(WEB_DIR / "app.js")
        cls.css = read_text(WEB_DIR / "styles.css")
        cls.config = json.loads(read_text(DEFAULT_VISUALIZER_CONFIG))
        cls.point_cloud_demo = read_text(VISUALIZER_DIR / "demo" / "send_point_cloud_demo.py")
        cls.robot_state_demo = read_text(VISUALIZER_DIR / "demo" / "send_robot_state.py")

    def test_add_display_modal_uses_select_components(self) -> None:
        self.assertIn('<select id="typeInput"></select>', self.html)
        self.assertIn('<select id="messageTypeInput" disabled></select>', self.html)
        self.assertNotIn("<datalist", self.html)
        self.assertNotIn("messageTypeList", self.html + self.js)

    def test_display_type_and_message_type_mapping(self) -> None:
        for display_type, message_type in EXPECTED_MESSAGE_MAP.items():
            self.assertIn(display_type, self.js)
            self.assertIn(message_type, self.js)
        self.assertIn("event.message_type_map", self.js)
        self.assertIn("el.typeInput.onchange = updateResolvedMessageType", self.js)
        self.assertRegex(self.js, r"messageTypes:\s*\[\s*\"RawMessage\"")

    def test_config_buttons_and_topic_commands_exist(self) -> None:
        for element_id in (
            "exportConfigBtn",
            "importConfigBtn",
            "configPathInput",
        ):
            self.assertIn(f'id="{element_id}"', self.html)
            self.assertIn(element_id, self.js)
        self.assertNotIn("reloadConfigBtn", self.html + self.js)
        self.assertNotIn("configFileInput", self.html + self.js)
        self.assertNotIn('type="file"', self.html)
        self.assertNotIn('id="saveConfigBtn"', self.html)
        self.assertNotIn('id="loadConfigBtn"', self.html)
        self.assertNotIn("saveConfigBtn", self.js)
        self.assertNotIn("loadConfigBtn", self.js)
        self.assertIn('action: "add_topic"', self.js)
        self.assertIn('action: "remove_topic"', self.js)
        self.assertIn('action: "save_config"', self.js)
        self.assertIn('action: "load_config"', self.js)
        self.assertIn("backendConfigPath()", self.js)
        self.assertNotIn("await file.text()", self.js)
        self.assertNotIn("downloadJson", self.js)

    def test_robot_display_components_are_added_through_add_display(self) -> None:
        for removed_id in ("importUrdfBtn", "urdfFileInput", "robotVisibleInput", "clearRobotBtn", "robotInfo"):
            self.assertNotIn(removed_id, self.html + self.js)
        for element_id in ("urdfPathInput", "urdfPathField", "targetRobotInput", "targetRobotField"):
            self.assertIn(f'id="{element_id}"', self.html)
            self.assertIn(element_id, self.js)
        self.assertIn("RobotState", self.js)
        self.assertIn("Robot", self.js)
        self.assertIn('action: "add_robot"', self.js)
        self.assertIn('action: "add_robot_state"', self.js)
        self.assertIn("robot_displays", self.js)
        self.assertIn("robot_state_displays", self.js)
        self.assertIn("window.dzipcVisualizer", self.js)
        self.assertIn("robot_models", self.config)
        self.assertIsInstance(self.config["robot_models"], list)

    def test_robot_link_states_update_3d_robot_group(self) -> None:
        self.assertIn("event.robot_link_states", self.js)
        self.assertIn("applyRobotLinkStates", self.js)
        self.assertIn("matrixFromRobotLinkState", self.js)
        self.assertIn("updateRobotGroupFromLinkStates", self.js)
        self.assertIn('group.getObjectByName("link_frames")', self.js)
        self.assertIn('group.getObjectByName("link_segments")', self.js)
        self.assertIn('group.getObjectByName("link_groups")', self.js)
        self.assertIn("robot.latestLinkStates", self.js)
        self.assertIn("latestJointState", self.js)
        self.assertIn("latestError", self.js)

    def test_robot_mesh_assets_are_loaded_through_backend_urls(self) -> None:
        self.assertIn('import { OBJLoader }', self.js)
        self.assertIn('import { STLLoader }', self.js)
        self.assertIn("parseLinkVisuals", self.js)
        self.assertIn("parseLinkCollisions", self.js)
        self.assertIn("mesh_assets", self.js)
        self.assertIn("normalizeRobotMeshAssets", self.js)
        self.assertIn("objLoader.loadAsync", self.js)
        self.assertIn("stlLoader.loadAsync", self.js)
        self.assertIn("robotAssetForVisual", self.js)
        self.assertIn("addRobotMeshToLinkGroup", self.js)
        self.assertIn("state.assetCache", self.js)
        self.assertIn('linkGroup.name = `link_group:${link.name}`', self.js)
        self.assertNotIn("decodeBase64Bytes", self.js)

    def test_display_visualization_toggle_controls_scene_output(self) -> None:
        self.assertIn("visualize: meta.visualize === true", self.js)
        self.assertIn("displayRenderIntervalMs", self.js)
        self.assertIn("requestDisplayRender()", self.js)
        self.assertIn("displayInteractionUntil", self.js)
        self.assertIn("holdDisplayInteraction", self.js)
        self.assertIn("setTopicVisualization", self.js)
        self.assertIn('data-visualize="${escapeHtml(topic.name)}"', self.js)
        self.assertIn('class="visualize-toggle"', self.js)
        self.assertIn("visualizeToggle.onpointerdown", self.js)
        self.assertIn("setTopicVisualization(topic, !topic.visualize)", self.js)
        self.assertIn("topic.visualize = visualize", self.js)
        self.assertIn("resetImageOverlayAutoSize", self.js)
        self.assertIn("findVisualizedImageTopic", self.js)
        self.assertIn("active?.visualize", self.js)
        self.assertIn("topic.visualize &&", self.js)
        self.assertIn("!topic.visualize || (!hasPoseTrail && !hasPointCloud)", self.js)
        self.assertIn("if (topic) topic.visualize = false", self.js)
        self.assertIn(".visualize-toggle", self.css)

    def test_image_overlay_sizes_to_image_and_drags_smoothly(self) -> None:
        self.assertIn("imageRenderIntervalMs", self.js)
        self.assertIn("fitImageOverlayToImage", self.js)
        self.assertIn("overlayChromeHeight", self.js)
        self.assertIn("--image-content-width", self.js)
        self.assertIn("--image-content-height", self.js)
        self.assertIn("borderWidth", self.js)
        self.assertIn("borderHeight", self.js)
        self.assertIn("resizeOrigContentWidth", self.js)
        self.assertIn("resizeOrigContentHeight", self.js)
        self.assertIn("widthScale", self.js)
        self.assertIn("heightScale", self.js)
        self.assertIn("scheduleImageOverlayRender", self.js)
        self.assertIn("imageState.dragActive || imageState.resizeActive", self.js)
        self.assertIn("translate3d", self.js)
        self.assertIn("el.imageOverlayHeader.setPointerCapture", self.js)
        self.assertIn("finishImageOverlayDrag", self.js)
        self.assertIn("imageState.manualSize = true", self.js)
        self.assertIn("will-change: transform, left, top, width, height", self.css)
        self.assertIn(".image-overlay.dragging", self.css)
        self.assertIn("width: var(--image-content-width, 100%)", self.css)
        self.assertIn("height: var(--image-content-height, auto)", self.css)
        self.assertNotIn("width: min(90vw, 400px) !important", self.css)
        self.assertNotIn("resize: both", self.css)
        self.assertIn("width: 100%", self.css)
        self.assertIn("height: 100%", self.css)
        self.assertIn("object-fit: contain", self.css)

    def test_3d_scene_controls_and_grid_exist(self) -> None:
        self.assertIn('id="sceneCanvas"', self.html)
        self.assertIn('id="sceneFpsMetric"', self.html)
        self.assertIn('class="scene-fps"', self.html)
        self.assertIn('id="imageOverlay"', self.html)
        self.assertIn('type="importmap"', self.html)
        self.assertIn('type="module" src="/app.js?v=7"', self.html)
        self.assertIn('import * as THREE from "three"', self.js)
        self.assertIn("new THREE.WebGLRenderer", self.js)
        self.assertIn("new OrbitControls", self.js)
        self.assertIn('addEventListener(', self.js)
        self.assertIn("new THREE.GridHelper", self.js)
        self.assertIn("grid.rotation.x = gridPlaneRotation", self.js)
        self.assertIn("const gridPlaneRotation = Math.PI / 2", self.js)
        self.assertIn("const sceneBackgroundColor = 0xffffff", self.js)
        self.assertIn("renderer.setClearColor(sceneBackgroundColor, 1)", self.js)
        self.assertIn("THREE.Object3D.DEFAULT_UP.set(0, 0, 1)", self.js)
        self.assertIn("const referenceAxisConfig = {", self.js)
        self.assertIn("axisRadius:", self.js)
        self.assertIn("backgroundOpacity:", self.js)
        self.assertIn("renderer.render(axisScene, axisCamera)", self.js)
        self.assertIn("makeAxisGizmo", self.js)
        self.assertIn("renderAxisGizmo", self.js)
        self.assertIn("sceneRenderState", self.js)
        self.assertIn("requestSceneRender", self.js)
        self.assertIn("sceneInteractionRenderMs", self.js)
        self.assertIn("const maxSceneFps = 30", self.js)
        self.assertIn("const maxPointCloudFps = 10", self.js)
        self.assertIn("pointCloudMapConfig", self.js)
        self.assertIn("pointCloudAdaptiveRenderConfig", self.js)
        self.assertIn("targetFps: 10", self.js)
        self.assertIn("recoverFps: 13", self.js)
        self.assertIn("maxRenderStep: 16", self.js)
        self.assertIn("voxelSize: 0.1", self.js)
        self.assertIn("chunkSize: 10", self.js)
        self.assertIn("maxPoints: 2000000", self.js)
        self.assertIn("accumulatePointCloudMap", self.js)
        self.assertIn("syncPointCloudMapObjects", self.js)
        self.assertIn("requestPointCloudMapRender", self.js)
        self.assertIn("tunePointCloudRenderBudget", self.js)
        self.assertIn("sampledFloat32Array", self.js)
        self.assertIn("markPointCloudMapDirty", self.js)
        self.assertIn("latestStep", self.js)
        self.assertIn("mapStep", self.js)
        self.assertIn("objectSet.lastPointCloudRenderStep !== renderBudget.latestStep", self.js)
        self.assertIn("map_points", self.js)
        self.assertIn("sceneFpsWindowMs", self.js)
        self.assertIn("sceneFpsUpdateIntervalMs", self.js)
        self.assertIn("sceneFpsMetric", self.js)
        self.assertIn("updateSceneFpsMetric", self.js)
        self.assertIn('el.sceneFpsMetric.textContent = "0.0"', self.js)
        self.assertIn(".scene-fps", self.css)
        self.assertIn("font-variant-numeric: tabular-nums", self.css)
        self.assertIn("minSceneFrameMs", self.js)
        self.assertIn("minPointCloudFrameMs", self.js)
        self.assertIn("requestPointCloudRender", self.js)
        self.assertIn("lastPointCloudRenderTime", self.js)
        self.assertIn("sceneRenderState.lastRenderTime + minFrameMs", self.js)
        self.assertIn("if (sceneRenderState.pending) return", self.js)
        self.assertIn("performance.now() < sceneRenderState.continuousUntil", self.js)
        self.assertIn('controls.addEventListener("change"', self.js)
        self.assertIn("requestSceneRender();", self.js)
        self.assertNotIn("connect();\nrequestAnimationFrame(renderScene);", self.js)
        self.assertIn('makeLabel("X"', self.js)
        self.assertIn('makeLabel("Y"', self.js)
        self.assertIn('makeLabel("Z"', self.js)
        self.assertNotIn("scene.add(axes)", self.js)
        self.assertIn('addEventListener("contextmenu"', self.js)
        self.assertIn("Right-drag to move camera", self.html + self.js)
        self.assertIn("syncTopicObjects", self.js)
        self.assertIn("new THREE.BufferGeometry().setFromPoints", self.js)
        self.assertIn('ws.binaryType = "arraybuffer"', self.js)
        self.assertIn("handleBinaryFrame", self.js)
        self.assertIn("binaryPointCloudHeaderBytes = 24", self.js)
        self.assertIn("new THREE.Points", self.js)
        self.assertIn("sampledFloat32Array(topic.pointCloud.positions, 3, renderBudget.latestStep)", self.js)
        self.assertIn("new THREE.PointsMaterial", self.js)

    def test_random_point_cloud_demo_exists_for_stress_tests(self) -> None:
        self.assertIn('default="/demo/random_point_cloud"', self.point_cloud_demo)
        self.assertIn("ipc.StdPointCloud()", self.point_cloud_demo)
        self.assertIn("ipc.StdVector3d()", self.point_cloud_demo)
        self.assertIn("ipc.StdColor()", self.point_cloud_demo)
        self.assertIn('choices=["none", "height", "random"]', self.point_cloud_demo)
        self.assertIn('choices=["normal", "best-effort"]', self.point_cloud_demo)
        self.assertIn("publish_best_effort", self.point_cloud_demo)
        self.assertIn("--report-every", self.point_cloud_demo)

    def test_robot_state_demo_exists_for_urdf_motion_tests(self) -> None:
        self.assertIn('default="/demo/robot_state"', self.robot_state_demo)
        self.assertIn('DEFAULT_URDF_TOPIC = "/demo/robot_urdf"', self.robot_state_demo)
        self.assertIn("DOWNLOAD_CANDIDATES", self.robot_state_demo)
        self.assertIn("FALLBACK_URDF", self.robot_state_demo)
        self.assertIn("ipc.RobotState()", self.robot_state_demo)
        self.assertIn("ipc.StdRawMessage()", self.robot_state_demo)
        self.assertIn('"joint_state"', self.robot_state_demo)
        self.assertIn("--prepare-only", self.robot_state_demo)
        self.assertIn("--no-download", self.robot_state_demo)

    def test_disabled_message_type_has_readable_style(self) -> None:
        self.assertIn("select:disabled", self.css)
        self.assertIn("opacity: 1", self.css)


class BridgeIntegrationTests(unittest.TestCase):
    process: Optional[subprocess.Popen[str]] = None
    port: int = 0
    temp_dir: Optional[tempfile.TemporaryDirectory[str]] = None
    default_robot_displays = [
        {
            "id": "fixture_bot",
            "display_type": "Robot",
            "name": "Fixture Bot",
            "urdf_path": "fixture.urdf",
            "urdf": "<robot name='fixture_bot'/>",
            "visible": False,
        }
    ]
    default_robot_state_displays = [
        {
            "id": "fixture_state",
            "display_type": "RobotState",
            "name": "/fixture_state",
            "topic": "/fixture_state",
            "robot_id": "fixture_bot",
            "msg_type": "RobotState",
            "transport": "socket",
            "queue": 5,
            "poll": 0.02,
        }
    ]
    default_config = {
        "domain": 7,
        "transport": "socket",
        "queue": 5,
        "poll": 0.02,
        "topics": [],
        "robot_displays": default_robot_displays,
        "robot_state_displays": default_robot_state_displays,
    }

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp_dir = tempfile.TemporaryDirectory(prefix="dzipc_visualizer_test_")
        config_path = Path(cls.temp_dir.name) / "config.json"
        config_path.write_text(json.dumps(cls.default_config), encoding="utf-8")
        cls.port = unused_tcp_port()
        cls.process = start_bridge_server(cls.port, config_path)
        wait_for_http(cls.port, cls.process)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.process is not None:
            stop_process(cls.process)
        if cls.temp_dir is not None:
            cls.temp_dir.cleanup()

    def tearDown(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            ws_read_json(sock)
            ws_send_json(sock, {"action": "apply_config", "replace": True, "config": self.default_config})
            ack = read_until_kind(sock, "ack")
            if not ack.get("ok"):
                raise AssertionError(ack)

    def test_static_resources_are_served(self) -> None:
        checks = {
            "/": "text/html",
            "/app.js": "text/javascript",
            "/styles.css": "text/css",
        }
        for path, content_type in checks.items():
            status, actual_type, body = http_get_text(self.port, path)
            self.assertEqual(status, 200)
            self.assertIn(content_type, actual_type)
            self.assertTrue(body.strip())

    def test_websocket_hello_contains_display_contract(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            hello = ws_read_json(sock)
            self.assertEqual(hello["kind"], "hello")
            self.assertEqual(hello["message_types"], EXPECTED_DISPLAY_TYPES)
            self.assertEqual(hello["message_type_map"], EXPECTED_MESSAGE_MAP)
            self.assertEqual(hello["config"]["domain"], 7)
            self.assertEqual(hello["config"]["robot_displays"][0]["id"], "fixture_bot")
            self.assertEqual(hello["config"]["robot_displays"][0]["urdf"], "<robot name='fixture_bot'/>")
            self.assertEqual(hello["config"]["robot_state_displays"][0]["id"], "fixture_state")
            self.assertEqual(hello["config"]["robot_state_displays"][0]["robot_id"], "fixture_bot")
            self.assertEqual(hello["config"]["robot_models"][0]["id"], "fixture_bot")

    def test_websocket_config_round_trip(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            ws_read_json(sock)
            robot_displays = [
                {
                    "id": "round_trip_bot",
                    "display_type": "Robot",
                    "name": "Round Trip Bot",
                    "urdf_path": "round_trip.urdf",
                    "urdf": "<robot name='round_trip_bot'/>",
                    "visible": False,
                }
            ]
            robot_state_displays = [
                {
                    "id": "round_trip_state",
                    "display_type": "RobotState",
                    "name": "/round_trip_state",
                    "topic": "/round_trip_state",
                    "robot_id": "round_trip_bot",
                    "msg_type": "RobotState",
                    "domain": 9,
                    "queue": 2,
                    "transport": "shm",
                    "poll": 0.04,
                    "extra": "unit-test",
                    "active": True,
                }
            ]
            ws_send_json(
                sock,
                {
                    "action": "apply_config",
                    "replace": True,
                    "config": {
                        "domain": 9,
                        "transport": "shm",
                        "queue": 2,
                        "poll": 0.04,
                        "extra": "unit-test",
                        "topics": [],
                        "robot_displays": robot_displays,
                        "robot_state_displays": robot_state_displays,
                    },
                },
            )
            ack = read_until_kind(sock, "ack")
            self.assertTrue(ack["ok"], ack)
            self.assertEqual(ack["action"], "apply_config")
            self.assertEqual(ack["config"]["domain"], 9)
            self.assertEqual(ack["config"]["robot_displays"][0]["id"], "round_trip_bot")
            self.assertEqual(ack["config"]["robot_displays"][0]["urdf_path"], "round_trip.urdf")
            self.assertFalse(ack["config"]["robot_displays"][0]["visible"])
            self.assertEqual(ack["config"]["robot_state_displays"][0]["id"], "round_trip_state")
            self.assertEqual(ack["config"]["robot_state_displays"][0]["robot_id"], "round_trip_bot")
            config = read_until_kind(sock, "config")
            self.assertEqual(config["message_types"], EXPECTED_DISPLAY_TYPES)
            self.assertEqual(config["message_type_map"], EXPECTED_MESSAGE_MAP)
            self.assertEqual(config["config"]["robot_displays"][0]["id"], "round_trip_bot")
            self.assertEqual(config["config"]["robot_state_displays"][0]["id"], "round_trip_state")
            self.assertEqual(config["config"]["robot_state_displays"][0]["robot_id"], "round_trip_bot")

    def test_demo_publisher_emits_samples(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            ws_read_json(sock)
            sample = read_until_kind(sock, "sample", timeout_s=4.0)
            self.assertEqual(sample["topic"], "demo_robot_state")
            self.assertIn("current_pose", sample["data"])


class BrowserComponentTests(unittest.TestCase):
    port: int = 0
    process: Optional[subprocess.Popen[str]] = None
    temp_dir: Optional[tempfile.TemporaryDirectory[str]] = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp_dir = tempfile.TemporaryDirectory(prefix="dzipc_visualizer_browser_test_")
        config_path = Path(cls.temp_dir.name) / "config.json"
        config_path.write_text("{}", encoding="utf-8")
        cls.port = unused_tcp_port()
        cls.process = start_bridge_server(cls.port, config_path)
        wait_for_http(cls.port, cls.process)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.process is not None:
            stop_process(cls.process)
        if cls.temp_dir is not None:
            cls.temp_dir.cleanup()

    def tearDown(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            ws_read_json(sock)
            ws_send_json(sock, {"action": "apply_config", "replace": True, "config": {}})
            ack = read_until_kind(sock, "ack")
            if not ack.get("ok"):
                raise AssertionError(ack)

    def test_add_display_modal_and_select_mapping(self) -> None:
        expect, sync_playwright = load_playwright()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"],
                timeout=10000,
            )
            try:
                page = browser.new_page(viewport={"width": 1280, "height": 800})
                page.set_default_timeout(8000)
                page.goto(f"http://127.0.0.1:{self.port}/", wait_until="domcontentloaded", timeout=8000)
                expect(page.locator("#displayModal")).to_be_hidden()
                page.locator("#openAddDisplayBtn").click()
                expect(page.locator("#displayModal")).to_be_visible()
                self.assertEqual(page.locator("#typeInput").evaluate("node => node.tagName"), "SELECT")
                self.assertEqual(
                    page.eval_on_selector_all("#typeInput option", "nodes => nodes.map(n => n.value)"),
                    EXPECTED_DISPLAY_TYPES,
                )
                self.assertEqual(page.locator("#messageTypeInput").evaluate("node => node.tagName"), "SELECT")
                self.assertTrue(page.locator("#messageTypeInput").is_disabled())
                page.select_option("#typeInput", "Pose")
                self.assertEqual(page.locator("#messageTypeInput").input_value(), "StdPose")
                page.select_option("#typeInput", "PointCloud")
                self.assertEqual(page.locator("#messageTypeInput").input_value(), "StdPointCloud")
                page.locator("#cancelDisplayBtn").click()
                expect(page.locator("#displayModal")).to_be_hidden()
            finally:
                browser.close()

    def test_display_visualization_toggle_is_user_controlled(self) -> None:
        expect, sync_playwright = load_playwright()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"],
                timeout=10000,
            )
            try:
                page = browser.new_page(viewport={"width": 1280, "height": 800})
                page.set_default_timeout(8000)
                page.goto(f"http://127.0.0.1:{self.port}/", wait_until="domcontentloaded", timeout=8000)
                checkbox = page.locator(".display-item input[data-visualize]").first
                expect(checkbox).to_be_visible()
                expect(checkbox).not_to_be_checked()
                checkbox.check()
                expect(page.locator(".display-item input[data-visualize]").first).to_be_checked()
                page.locator(".display-item input[data-visualize]").first.uncheck()
                expect(page.locator(".display-item input[data-visualize]").first).not_to_be_checked()
            finally:
                browser.close()

    def test_webgl_scene_renders_nonblank_canvas(self) -> None:
        _, sync_playwright = load_playwright()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"],
                timeout=10000,
            )
            try:
                for viewport in ({"width": 1280, "height": 800}, {"width": 390, "height": 844}):
                    page = browser.new_page(viewport=viewport)
                    page.set_default_timeout(8000)
                    page.goto(f"http://127.0.0.1:{self.port}/", wait_until="networkidle", timeout=8000)
                    page.wait_for_function(
                        """
                        async () => {
                          const canvas = document.querySelector("#sceneCanvas");
                          if (!canvas || canvas.width < 20 || canvas.height < 20) return false;
                          if (!window.dzipcVisualizer?.requestSceneRender) return false;
                          window.dzipcVisualizer.requestSceneRender();
                          await new Promise((resolve) => requestAnimationFrame(resolve));
                          const gl = canvas.getContext("webgl2") || canvas.getContext("webgl");
                          if (!gl) return false;
                          const pixels = new Uint8Array(4 * 64);
                          gl.readPixels(
                            Math.max(0, Math.floor(canvas.width / 2) - 4),
                            Math.max(0, Math.floor(canvas.height / 2) - 4),
                            8,
                            8,
                            gl.RGBA,
                            gl.UNSIGNED_BYTE,
                            pixels,
                          );
                          for (let i = 0; i < pixels.length; i += 4) {
                            if (pixels[i] > 18 || pixels[i + 1] > 24 || pixels[i + 2] > 32) return true;
                          }
                          return false;
                        }
                        """,
                        timeout=8000,
                    )
                    self.assertIn("WebGL", page.locator("#sceneCanvas").evaluate("node => node.getContext('webgl2') ? 'WebGL2' : 'WebGL'"))
                    page.close()
            finally:
                browser.close()

    def test_add_robot_then_robot_state_display_updates_displays_list(self) -> None:
        expect, sync_playwright = load_playwright()
        assert self.temp_dir is not None
        urdf_path = Path(self.temp_dir.name) / "tiny.urdf"
        urdf_path.write_text(
            """<?xml version="1.0"?>
<robot name="tiny_bot">
  <link name="base_link"/>
</robot>
""",
            encoding="utf-8",
        )

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"],
                timeout=10000,
            )
            try:
                page = browser.new_page(viewport={"width": 1280, "height": 800})
                page.set_default_timeout(8000)
                page.goto(f"http://127.0.0.1:{self.port}/", wait_until="networkidle", timeout=8000)
                page.wait_for_function("() => window.dzipcVisualizer?.sceneState?.scene")
                page.locator("#openAddDisplayBtn").click()
                page.select_option("#typeInput", "Robot")
                page.fill("#topicInput", "tiny_bot")
                page.fill("#urdfPathInput", str(urdf_path))
                page.locator("#addTopicBtn").click()
                expect(page.locator("#displayModal")).to_be_hidden()
                expect(page.locator(".display-item").filter(has_text="tiny_bot")).to_be_visible()

                page.locator("#openAddDisplayBtn").click()
                page.select_option("#typeInput", "RobotState")
                page.fill("#topicInput", "tiny_bot_state")
                page.select_option("#targetRobotInput", "tiny_bot")
                page.locator("#addTopicBtn").click()
                expect(page.locator(".display-item").filter(has_text="tiny_bot_state")).to_be_visible()

                page.wait_for_function(
                    """
                    () => {
                      const api = window.dzipcVisualizer;
                      return Boolean(
                        api?.state?.robotDisplays?.get("tiny_bot") &&
                        api?.state?.robotStateDisplays?.get("tiny_bot_state")
                      );
                    }
                    """,
                    timeout=8000,
                )
                self.assertEqual(page.evaluate("() => window.dzipcVisualizer.state.robotDisplays.size"), 1)
                self.assertEqual(page.evaluate("() => window.dzipcVisualizer.state.robotStateDisplays.size"), 1)
            finally:
                browser.close()


def build_suite(include_integration: bool, include_browser: bool) -> unittest.TestSuite:
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(BackendComponentTests))
    suite.addTests(loader.loadTestsFromTestCase(FrontendStaticComponentTests))
    if include_integration:
        suite.addTests(loader.loadTestsFromTestCase(BridgeIntegrationTests))
    if include_browser:
        suite.addTests(loader.loadTestsFromTestCase(BrowserComponentTests))
    return suite


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Test dzIPC web visualizer components")
    parser.add_argument(
        "--integration",
        action="store_true",
        help="Start dzipc_web_bridge.py in --demo mode and test HTTP/WebSocket behavior.",
    )
    parser.add_argument(
        "--browser",
        action="store_true",
        help="Run Playwright UI tests. This also starts the bridge and may need a working Chromium runtime.",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Use verbose unittest output.")
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] = sys.argv[1:]) -> int:
    args = parse_args(argv)
    suite = build_suite(include_integration=args.integration, include_browser=args.browser)
    runner = unittest.TextTestRunner(verbosity=2 if args.verbose else 1)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
