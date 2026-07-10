#!/usr/bin/env python3
from __future__ import annotations

import argparse
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


TEST_DIR = Path(__file__).resolve().parent
VISUALIZER_DIR = TEST_DIR.parent
ROOT_DIR = VISUALIZER_DIR.parents[1]
BRIDGE_PATH = VISUALIZER_DIR / "dzipc_web_bridge.py"
WEB_DIR = VISUALIZER_DIR / "web"

EXPECTED_DISPLAY_TYPES = ["RawMessage", "Pose", "Path", "PointCloud", "Marker", "Image"]
EXPECTED_MESSAGE_MAP = {
    "RawMessage": "StdRawMessage",
    "Pose": "StdPose",
    "Path": "StdPath",
    "PointCloud": "StdPointCloud",
    "Marker": "StdMarker",
    "Image": "StdImage",
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


def ws_read_json(sock: socket.socket, timeout_s: float = 3.0) -> Dict[str, Any]:
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
        )
        for display_type, class_name in EXPECTED_MESSAGE_MAP.items():
            resolved_name, resolved_class = bridge.resolve_message_class(fake_ipc, display_type)
            self.assertEqual(resolved_name, class_name)
            self.assertIs(resolved_class, getattr(fake_ipc, class_name))
            resolved_name, resolved_class = bridge.resolve_message_class(fake_ipc, class_name)
            self.assertEqual(resolved_name, class_name)
            self.assertIs(resolved_class, getattr(fake_ipc, class_name))

    def test_resolve_message_class_rejects_non_std_display_types(self) -> None:
        fake_ipc = types.SimpleNamespace(RobotState=object)
        with self.assertRaisesRegex(ValueError, "Visualizer only supports std message display types"):
            bridge.resolve_message_class(fake_ipc, "RobotState")

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

        with self.assertRaisesRegex(ValueError, "topic and msg_type"):
            bridge.TopicSpec.from_config({"topic": "pose"}, defaults)
        with self.assertRaisesRegex(ValueError, "transport"):
            bridge.TopicSpec.from_config({"topic": "pose", "msg_type": "Pose", "transport": "udp"}, defaults)

    def test_config_normalization_and_json_conversion(self) -> None:
        normalized = bridge.normalize_defaults({"domain": "2", "queue": "8", "transport": "udp"}, {})
        self.assertEqual(normalized["domain"], 2)
        self.assertEqual(normalized["queue"], 8)
        self.assertEqual(normalized["transport"], "socket")

        class SlotMessage:
            __slots__ = ("x", "values", "_hidden")

            def __init__(self) -> None:
                self.x = 1.25
                self.values = [float("nan"), {"ok": True}]
                self._hidden = "skip"

        self.assertEqual(bridge.to_jsonable(SlotMessage()), {"x": 1.25, "values": [None, {"ok": True}]})

    def test_static_path_rejects_traversal(self) -> None:
        index_path = bridge.safe_static_path("/")
        self.assertEqual(index_path.name, "index.html")
        self.assertTrue(str(index_path).startswith(str(WEB_DIR)))
        self.assertEqual(bridge.safe_static_path("/../../etc/passwd").name, "index.html")


class FrontendStaticComponentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.html = read_text(WEB_DIR / "index.html")
        cls.js = read_text(WEB_DIR / "app.js")
        cls.css = read_text(WEB_DIR / "styles.css")

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
            "saveConfigBtn",
            "loadConfigBtn",
            "configFileInput",
        ):
            self.assertIn(f'id="{element_id}"', self.html)
            self.assertIn(element_id, self.js)
        self.assertIn('action: "add_topic"', self.js)
        self.assertIn('action: "remove_topic"', self.js)
        self.assertIn('action: "apply_config"', self.js)

    def test_3d_scene_controls_and_grid_exist(self) -> None:
        self.assertIn('id="sceneCanvas"', self.html)
        self.assertIn('getContext("2d"', self.js)
        self.assertIn('addEventListener(', self.js)
        self.assertIn('"wheel"', self.js)
        self.assertIn('addEventListener("pointerdown"', self.js)
        self.assertIn('addEventListener("contextmenu"', self.js)
        self.assertIn("Right-drag to move camera", self.html + self.js)
        self.assertRegex(self.js, r"for \(let i = -range; i <= range; i \+= 1\)")
        self.assertIn("#ef4444", self.js)
        self.assertIn("#22c55e", self.js)
        self.assertIn("#3b82f6", self.js)

    def test_disabled_message_type_has_readable_style(self) -> None:
        self.assertIn("select:disabled", self.css)
        self.assertIn("opacity: 1", self.css)


class BridgeIntegrationTests(unittest.TestCase):
    process: Optional[subprocess.Popen[str]] = None
    port: int = 0
    temp_dir: Optional[tempfile.TemporaryDirectory[str]] = None
    default_config = {"domain": 7, "transport": "socket", "queue": 5, "poll": 0.02, "topics": []}

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

    def test_websocket_config_round_trip(self) -> None:
        with contextlib.closing(websocket_connect(self.port)) as sock:
            ws_read_json(sock)
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
                    },
                },
            )
            ack = read_until_kind(sock, "ack")
            self.assertTrue(ack["ok"], ack)
            self.assertEqual(ack["action"], "apply_config")
            self.assertEqual(ack["config"]["domain"], 9)
            config = read_until_kind(sock, "config")
            self.assertEqual(config["message_types"], EXPECTED_DISPLAY_TYPES)
            self.assertEqual(config["message_type_map"], EXPECTED_MESSAGE_MAP)

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

    def test_add_display_modal_and_select_mapping(self) -> None:
        try:
            from playwright.sync_api import expect, sync_playwright
        except Exception as exc:  # pragma: no cover - depends on local environment.
            self.skipTest(f"playwright is not available: {exc}")

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"],
                timeout=10000,
            )
            try:
                page = browser.new_page(viewport={"width": 1280, "height": 800})
                page.set_default_timeout(5000)
                page.goto(f"http://127.0.0.1:{self.port}/", wait_until="domcontentloaded", timeout=5000)
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
