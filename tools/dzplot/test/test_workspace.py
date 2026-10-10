#!/usr/bin/env python3
"""统一工作区的真实 HTTP / WebSocket 与浏览器验收（不需要 dzIPC 绑定）。"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
import base64

ROOT = Path(__file__).resolve().parents[3]
sys.path.append(str(ROOT))
from tools.dzplot import main as plot
from tools.dzplot.workspace import WorkspaceVizHub, load_workspace_ipc, viz


async def receive(reader, kind=None):
    while True:
        opcode, payload = await asyncio.wait_for(plot.ws_read_frame(reader), 3)
        if opcode == 2:
            if kind == "binary":
                return payload
            continue
        event = json.loads(payload)
        if kind is None or event.get("kind") == kind:
            return event


async def command(writer, payload):
    raw = json.dumps(payload).encode()
    mask = os.urandom(4)
    header = bytes([0x81, 0x80 | len(raw)]) if len(raw) < 126 else b"\x81\xfe" + struct.pack("!H", len(raw))
    writer.write(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(raw)))
    await writer.drain()


class WorkspaceIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dzplot-workspace-")
        self.plot = plot.PlotHub()
        self.viz = WorkspaceVizHub(self.plot, {"domain": 0, "transport": "shm"}, Path(self.temp.name) / "config.json")
        self.plot.sample_observer = self.viz.observe_plot_sample
        self.server = await asyncio.start_server(lambda r, w: plot.handle_client(r, w, self.plot, self.viz), "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.tasks = [asyncio.create_task(self.plot.broadcast_loop()), asyncio.create_task(self.viz.broadcast_loop())]
        self.writers = []

    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
            await writer.wait_closed()
        self.server.close()
        await self.server.wait_closed()
        await self.plot.shutdown()
        await self.viz.shutdown()
        for task in self.tasks:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        self.temp.cleanup()

    async def websocket(self, path):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        self.writers.append(writer)
        writer.write((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n").encode())
        await writer.drain()
        self.assertIn(b"101 Switching Protocols", await reader.readuntil(b"\r\n\r\n"))
        return reader, writer

    async def http(self, path):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
        await writer.drain()
        response = await reader.read()
        writer.close()
        await writer.wait_closed()
        return response.split(b"\r\n\r\n", 1)

    async def test_main_subpage_and_offline_assets_share_one_server(self):
        for path in ("/", "/app.js", "/workspace.js", "/plots.js", "/viz/", "/viz/app.js", "/viz/styles.css", "/viz/workspace.css", "/viz/vendor/three/build/three.module.js", "/viz/vendor/three/examples/jsm/controls/OrbitControls.js", "/viz/vendor/three/examples/jsm/loaders/OBJLoader.js", "/viz/vendor/three/examples/jsm/loaders/STLLoader.js"):
            head, body = await self.http(path)
            self.assertIn(b"200 OK", head, path)
            relative = {"/": "index.html", "/viz/": "viz/index.html"}.get(path, path.lstrip("/"))
            self.assertEqual(body, (ROOT / "tools/dzplot/web" / relative).read_bytes(), path)
        _, main_html = await self.http("/")
        _, viz_html = await self.http("/viz/")
        self.assertIn(b'id="plotTab"', main_html)
        self.assertIn(b'id="sceneCanvas"', viz_html)
        self.assertNotIn(b"cdn.jsdelivr", viz_html)
        head, _ = await self.http("/viz/%2e%2e/main.py")
        self.assertIn(b"404 Not Found", head)
        head, _ = await self.http("/viz")
        self.assertIn(b"Location: /viz/", head)

    async def test_bundled_robot_models_and_mesh_assets_are_served(self):
        config = json.loads((ROOT / "tools/dzplot/demo/robot_state_config.json").read_text())
        tiny = self.viz.add_robot_display(config["robot_displays"][0])
        self.assertIn("<robot", tiny.urdf_text)
        self.assertTrue((ROOT / tiny.urdf_path).is_file())
        robot = self.viz.add_robot_display({
            "id": "go2", "urdf_path": "tools/dzplot/demo/robots/go2/go2.urdf",
        })
        self.assertTrue(robot.mesh_assets)
        self.assertFalse(robot.mesh_warnings, robot.mesh_warnings)
        for asset in robot.mesh_assets.values():
            head, body = await self.http(asset["url"])
            self.assertIn(b"200 OK", head)
            self.assertEqual(len(body), asset["byte_length"])
            key = asset["url"].split("/")[2]
            self.assertEqual(body, self.viz.robot_asset_paths[key].read_bytes())

    async def test_source_samples_reach_curves_and_3d_without_duplicate_subscription(self):
        plot_reader, plot_writer = await self.websocket("/ws")
        viz_reader, viz_writer = await self.websocket("/viz/ws")
        self.assertIn("selected_fields", await receive(plot_reader, "hello"))
        self.assertIn("message_type_map", await receive(viz_reader, "hello"))
        await command(viz_writer, {"action": "add_source_display", "topic": {"topic": "/motion", "msg_type": "Pose"}})
        self.assertTrue((await receive(viz_reader, "ack"))["ok"])
        self.assertFalse(self.viz.subscribers)
        fields = {"position": {"x": 1.25, "y": 2.5, "z": 0.0}}
        self.plot.event_queue.put({"source": "bag", "topic": "/motion", "fields": fields, "timestamp_ns": 1_500_000_000})
        frame = await receive(plot_reader, "frame")
        sample = await receive(viz_reader, "sample")
        self.assertEqual(frame["batch"][0]["fields"], sample["data"])
        self.assertEqual(sample["timestamp_ms"], 1500)
        self.assertEqual(self.plot.event_queue.watermark().total_published, 1)
        await command(plot_writer, {"action": "set_fps", "fps": 30})
        self.assertEqual((await receive(plot_reader, "ack"))["fps"], 30)

    async def test_3d_binary_frame_is_retained_while_numeric_fields_reach_curves(self):
        plot_reader, _ = await self.websocket("/ws")
        viz_reader, _ = await self.websocket("/viz/ws")
        await receive(plot_reader, "hello")
        await receive(viz_reader, "hello")
        binary = b"DZPC" + bytes(range(24))
        self.viz.publish({"kind": "sample", "topic": "/cloud", "msg_type": "PointCloud", "timestamp_ms": 1234,
                          "data": {"point_count": 3, "points_b64": "large-payload"}, "binary_payload": binary})
        sample = await receive(viz_reader, "sample")
        self.assertNotIn("binary_payload", sample)
        self.assertEqual(await receive(viz_reader, "binary"), binary)
        frame = await receive(plot_reader, "frame")
        self.assertEqual(frame["batch"][0]["fields"], {"point_count": 3})
        self.assertEqual(self.plot.event_queue.watermark().total_published, 1)

    async def test_workspace_display_config_can_be_saved_and_reloaded(self):
        self.viz.add_source_display({"topic": "/motion", "msg_type": "Pose"})
        target = self.viz.save_config()
        self.viz.remove_topic("/motion")
        self.viz.apply_config(viz.load_config_file(target))
        self.assertIn("/motion", self.viz.source_displays)
        self.assertFalse(self.viz.subscribers)
        self.assertEqual(self.viz.export_config()["topics"][0]["source"], "workspace")

    async def test_workspace_image_and_cloud_keep_complete_data(self):
        reader, _ = await self.websocket("/viz/ws")
        await receive(reader, "hello")
        self.viz.add_source_display({"topic": "/image", "msg_type": "Image"})
        image = {"width": 16, "height": 8, "encoding": "rgb8", "step": 48,
                 "data": list(range(256)) + list(range(128))}
        sample = {"topic": "/image", "source": "bag", "fields": {"width": 16}}
        self.viz.decode_source_sample(sample, message=image)
        event = await receive(reader, "sample")
        self.assertEqual(len(base64.b64decode(event["binary"]["data_b64"])), 384)
        self.viz.observe_plot_sample(sample)
        self.assertNotIn("_viz_encoded", sample)
        self.viz.add_source_display({"topic": "/cloud", "msg_type": "PointCloud"})
        cloud = {"points": [{"x": i, "y": 0, "z": 0} for i in range(250)]}
        self.viz.decode_source_sample({"topic": "/cloud", "source": "bag"}, message=cloud)
        event = await receive(reader, "sample")
        self.assertEqual(event["binary"]["point_count"], 250)
        data = await receive(reader, "binary")
        self.assertEqual(len(data), 24 + 250 * 12)

    async def test_robot_state_from_workspace_does_not_create_a_second_receiver(self):
        self.viz.add_robot_display({"id": "arm", "urdf": "<robot name='arm'><link name='base'/></robot>"})
        self.viz.add_robot_state_display({"id": "state", "topic": "/joints", "target_robot_id": "arm", "source": "workspace"})
        self.assertFalse(self.viz.subscribers)
        self.assertIn("/joints", self.viz.source_displays)
        config = self.viz.export_config()
        self.assertEqual(config["robot_state_displays"][0]["source"], "workspace")
        self.assertEqual(config["displays"]["robot_state_displays"][0]["source"], "workspace")
        self.viz.apply_config(config)
        self.assertFalse(self.viz.subscribers)
        self.assertIn("/joints", self.viz.source_displays)
        self.assertEqual(self.viz.export_config()["robot_state_displays"][0]["source"], "workspace")
        self.viz.remove_robot_state_display("state")
        self.assertNotIn("/joints", self.viz.source_displays)


def make_replay_bag(path, count=5, interval_ns=50_000_000, payloads=None):
    def header(fields):
        parts = []
        for key, value in fields.items():
            if isinstance(value, int):
                value = (struct.pack("<II", value // 1_000_000_000, value % 1_000_000_000)
                         if key == "time" else struct.pack("<B" if key == "op" else "<I", value))
            elif isinstance(value, str):
                value = value.encode()
            entry = key.encode() + b"=" + value
            parts.append(struct.pack("<I", len(entry)) + entry)
        return b"".join(parts)
    def record(fields, data):
        h = header(fields)
        return struct.pack("<I", len(h)) + h + struct.pack("<I", len(data)) + data
    connection = record({"op": 7, "conn": 0, "topic": "/replay"}, header({"type": "StdRawMessage"}))
    if payloads is None:
        payloads = [f"sample-{i}".encode() for i in range(count)]
    messages = b"".join(record({"op": 2, "conn": 0, "time": 1_000_000_000 + i * interval_ns}, data)
                        for i, data in enumerate(payloads))
    chunk = record({"op": 5, "compression": "none", "size": len(messages)}, messages)
    Path(path).write_bytes(plot.BAG_MAGIC + connection + chunk)


class WorkspaceReplayTests(unittest.TestCase):
    def test_pause_resume_speed_progress_and_stop_follow_replay_timestamps(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "replay.bag"
            make_replay_bag(path)
            queue = plot.BoundedPubQueue()
            source = plot.BagReplaySource(str(path), queue)
            try:
                source.start()
                first = queue.get(timeout=1)
                self.assertIsNotNone(first)
                source.pause()
                time.sleep(.1)
                # 暂停的时间不消耗后续录包的时间间隔。
                self.assertEqual(source.progress()["progress"], 0)
                self.assertIsNone(queue.get(timeout=.01))
                source.resume()
                second = queue.get(timeout=1)
                self.assertEqual(second["timestamp_ns"] - first["timestamp_ns"], 50_000_000)
                source.set_options(speed=2, loop=False)
                for _ in range(3):
                    self.assertIsNotNone(queue.get(timeout=1))
                deadline = time.monotonic() + 1
                while source.running and time.monotonic() < deadline:
                    time.sleep(.005)
                self.assertFalse(source.running)
                self.assertEqual(source.progress()["progress"], 1)
            finally:
                source.stop()
            make_replay_bag(path, count=2, interval_ns=60_000_000_000)
            source = plot.BagReplaySource(str(path), queue)
            source.start()
            self.assertIsNotNone(queue.get(timeout=1))
            start = time.monotonic()
            source.stop()
            self.assertLess(time.monotonic() - start, .5)

    def test_complete_bag_can_restart_and_invalid_load_keeps_current_source(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "replay.bag"
            make_replay_bag(path, count=2)
            hub = plot.PlotHub()
            try:
                result = hub.load_bag(str(path))
                self.assertTrue(result["ok"])
                self.assertEqual(result["topics"][0]["msg_type"], "StdRawMessage")
                self.assertIsNotNone(hub.event_queue.get(timeout=1))
                self.assertIsNotNone(hub.event_queue.get(timeout=1))
                time.sleep(.02)
                self.assertEqual(hub.playback_status()["mode"], "finished")
                hub.resume()
                self.assertIsNotNone(hub.event_queue.get(timeout=1))
                self.assertEqual(hub.playback_status()["mode"], "playing")
                source = hub._bag_source
                self.assertFalse(hub.load_bag(str(path.parent / "missing.bag"))["ok"])
                self.assertIs(hub._bag_source, source)
                invalid = path.parent / "truncated.bag"
                invalid.write_bytes(plot.BAG_MAGIC + struct.pack("<I", 100) + b"op")
                self.assertFalse(hub.load_bag(str(invalid))["ok"])
                self.assertIs(hub._bag_source, source)
            finally:
                hub.stop_source()


class WorkspaceBindingTests(unittest.TestCase):
    """实际绑定生成的原始字节必须保留完整 3D 数据和有界曲线预览。"""

    def setUp(self):
        self.ipc = load_workspace_ipc()
        from dzipc import dzflat
        self.dzflat = dzflat

    def raw_events(self, message, display_type):
        for wire, raw in (("tlv", message.to_generic().serialize_bytes()),
                          ("dzflat", self.dzflat.pack(message))):
            with self.subTest(wire=wire), tempfile.TemporaryDirectory() as temp:
                self.assertIsNotNone(raw)
                hub = WorkspaceVizHub(plot.PlotHub(), {"domain": 0, "transport": "shm"}, Path(temp) / "config.json")
                hub.add_source_display({"topic": "/raw", "msg_type": display_type})
                hub.events.get_nowait()  # topic_added
                sample = {"topic": "/raw", "source": "bag", "msg_type": type(message).__name__,
                          "timestamp_ns": 1_234_000_000,
                          "fields": plot._decode_payload(type(message).__name__, raw)}
                hub.decode_source_sample(sample, raw=raw)
                event = hub.events.get_nowait()
                self.assertEqual(event["kind"], "sample", event)
                self.assertEqual(event["timestamp_ms"], 1234)
                self.assertFalse(hub.subscribers)
                hub.observe_plot_sample(sample)
                self.assertNotIn("_viz_encoded", sample)
                self.assertTrue(hub.events.empty(), "同一个样本不应重复进入 3D 通路")
                yield sample, event

    def test_workspace_decodes_full_image_from_tlv_and_dzflat(self):
        image = self.ipc.StdImage()
        image.width, image.height, image.step, image.encoding = 80, 60, 240, "rgb8"
        image.data = [i % 256 for i in range(14_400)]
        for sample, event in self.raw_events(image, "Image"):
            self.assertLessEqual(len(sample["fields"]["data"]), 100)
            self.assertEqual(base64.b64decode(event["binary"]["data_b64"]), bytes(image.data))
            self.assertEqual(event["binary"]["width"], 80)
            self.assertEqual(event["binary"]["height"], 60)

    def test_workspace_decodes_full_cloud_from_tlv_and_dzflat(self):
        cloud = self.ipc.StdPointCloud()
        cloud.points = []
        for i in range(1200):
            point = self.ipc.StdVector3d()
            point.data = [i, 2, 3]
            cloud.points.append(point)
        for sample, event in self.raw_events(cloud, "PointCloud"):
            self.assertLessEqual(len(sample["fields"]["points"]), 100)
            self.assertEqual(event["binary"]["point_count"], 1200)
            self.assertEqual(len(event["binary_payload"]), 24 + 1200 * 12)
            self.assertEqual(struct.unpack_from("<fff", event["binary_payload"], 24 + 1199 * 12), (1199, 2, 3))

    def test_recorded_transport_packet_reaches_curves_and_3d(self):
        image = self.ipc.StdImage()
        image.width, image.height, image.step, image.encoding = 80, 60, 240, "rgb8"
        image.data = [i % 256 for i in range(14_400)]
        raw = image.to_generic().serialize_bytes()
        def ros_string(value):
            data = value.encode()
            return struct.pack("<I", len(data)) + data
        packet = (struct.pack("<Q", 1_000_000_000) + ros_string("/recorded_image")
                  + ros_string("StdImage") + struct.pack("<IIBBBI", 0, 0, 0, 0, 0, len(raw)) + raw)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "image.bag"
            make_replay_bag(path, payloads=[packet])
            hub = plot.PlotHub()
            scene = WorkspaceVizHub(hub, {"domain": 0, "transport": "shm"}, Path(temp) / "config.json")
            scene.add_source_display({"topic": "/recorded_image", "msg_type": "Image"})
            scene.events.get_nowait()
            hub.sample_decoder = scene.decode_source_sample
            try:
                self.assertTrue(hub.load_bag(str(path))["ok"])
                sample = hub.event_queue.get(timeout=1)
                self.assertEqual(sample["topic"], "/recorded_image")
                self.assertEqual(sample["fields"]["width"], 80)
                self.assertLessEqual(len(sample["fields"]["data"]), 100)
                event = scene.events.get(timeout=1)
                self.assertEqual(event["kind"], "sample", event)
                self.assertEqual(base64.b64decode(event["binary"]["data_b64"]), bytes(image.data))
                self.assertFalse(scene.subscribers)
            finally:
                hub.stop_source()


class WorkspaceBrowserTests(unittest.TestCase):
    @contextlib.contextmanager
    def browser_workspace(self):
        from playwright.sync_api import sync_playwright
        with tempfile.TemporaryDirectory(prefix="dzplot-browser-") as temp:
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            process = subprocess.Popen([sys.executable, str(ROOT / "tools/dzplot/main.py"), "--demo", "--port", str(port), "--config", str(Path(temp) / "config.json")], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            base = f"http://127.0.0.1:{port}"
            try:
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                deadline = time.monotonic() + 8
                while True:
                    try:
                        with opener.open(base, timeout=.5):
                            break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            self.fail("工作区服务未启动")
                        time.sleep(.05)
                with sync_playwright() as p:
                    browser = p.chromium.launch(executable_path=os.environ.get("DZPLOT_BROWSER") or shutil.which("google-chrome"), headless=True, args=["--no-sandbox", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"])
                    try:
                        page = browser.new_page(viewport={"width": 1569, "height": 941})
                        page.set_default_timeout(8000)
                        errors, external, sockets = [], [], []
                        page.on("pageerror", lambda e: errors.append(str(e)))
                        page.on("websocket", lambda ws: sockets.append(ws.url))
                        def offline(route):
                            if route.request.url.startswith(base):
                                route.continue_()
                            else:
                                external.append(route.request.url)
                                route.abort()
                        page.route("**/*", offline)
                        page.goto(base, wait_until="networkidle")
                        page.wait_for_function("window.dzplotWorkspace?.state.availableFields.demo_robot_state?.includes('battery')")
                        yield page, sockets
                        self.assertFalse(external, external)
                        self.assertFalse(errors, errors)
                    finally:
                        browser.close()
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=3)
                if process.stdout:
                    process.stdout.close()

    def test_workspace_views_render_and_preserve_state_offline(self):
        with self.browser_workspace() as (page, sockets):
            self.assertEqual(page.locator("#appShell").get_attribute("data-view"), "plot")
            page.locator('.field-row[data-field="battery"] input').check()
            page.locator('.field-row[data-field="temperature"] input').check()
            page.wait_for_function("Object.values(window.dzplotWorkspace.state.chartData).length === 2 && Object.values(window.dzplotWorkspace.state.chartData).every(c => c.times.length >= 30)")
            self.assertEqual(page.locator(".plot-window").count(), 1)
            self.assertEqual(page.locator(".plot-series").count(), 2)
            page.screenshot(path=str(ARTIFACTS / "main.png"))
            page.locator("#topicFilter").fill("battery")
            self.assertEqual(page.locator(".field-row:visible").count(), 1)
            page.locator("#topicFilter").fill("")
            page.locator(".topic-viz-btn").click()
            frame = page.frame_locator("#vizFrame")
            frame.locator("#sceneCanvas").wait_for(state="visible")
            child = page.frames[1]
            child.wait_for_function("window.dzipcVisualizer?.state.topics.get('demo_robot_state')?.visualize && window.dzipcVisualizer?.sceneState.topicObjects.has('demo_robot_state')")
            self.assertEqual(len(sockets), 2)
            old = page.locator("#plotPane").bounding_box()["width"]
            divider = page.locator("#paneDivider").bounding_box()
            page.mouse.move(divider["x"] + 2, divider["y"] + 100)
            page.mouse.down(); page.mouse.move(divider["x"] + 100, divider["y"] + 100, steps=5); page.mouse.up()
            self.assertGreater(page.locator("#plotPane").bounding_box()["width"], old + 50)
            page.locator("#toggleGridBtn").click()
            child.wait_for_function("window.dzipcVisualizer.sceneState.grid.visible === false")
            page.locator("#toggleGridBtn").click()
            child.wait_for_function("window.dzipcVisualizer.sceneState.grid.visible === true")
            page.locator("#resetCameraBtn").click()
            page.screenshot(path=str(ARTIFACTS / "split.png"))
            page.locator("#vizTab").click()
            child.wait_for_function("""() => {
                const api = window.dzipcVisualizer;
                const rect = document.getElementById('sceneCanvas').getBoundingClientRect();
                return Math.abs(api.sceneState.camera.aspect - Math.floor(rect.width) / Math.floor(rect.height)) < .001;
            }""")
            page.screenshot(path=str(ARTIFACTS / "3d.png"))
            page.locator("#plotTab").click()
            self.assertEqual(page.locator(".plot-window").count(), 1)
            self.assertEqual(page.locator(".plot-series").count(), 2)
            self.assertEqual(len(sockets), 2)
            page.locator(".plot-series-remove").first.click()
            self.assertEqual(page.locator("#seriesCount").inner_text(), "1")
            page.locator('#fpsSlider').evaluate("el => { el.value = 30; el.dispatchEvent(new Event('change')); }")
            page.wait_for_function("window.dzplotWorkspace.state.fps === 30")
            page.set_viewport_size({"width": 390, "height": 844})
            page.locator("#toggleSidebarTopBtn").click()
            page.screenshot(path=str(ARTIFACTS / "mobile.png"))
            self.assertLessEqual(page.locator(".workspace").bounding_box()["width"], 390)
            self.assertGreater(page.locator(".chart-canvas").bounding_box()["width"], 250)

    def test_plot_windows_split_select_drag_and_restore(self):
        with self.browser_workspace() as (page, _):
            def plot_window(number):
                return page.locator(f'[data-plot-id="plot_{number}"]')

            def fields(number):
                return page.evaluate("n => [...window.dzplotWorkspace.state.charts['plot_' + n].series.values()].map(s => s.field).sort()", number)

            def field_input(field):
                return page.locator(f'.field-row[data-field="{field}"] input')

            def select(number):
                plot_window(number).locator(".plot-surface").click()
                self.assertEqual(page.evaluate("window.dzplotWorkspace.state.activePlotId"), f"plot_{number}")

            def snapshot():
                return page.evaluate("""() => {
                    const s = window.dzplotWorkspace.state;
                    return {layout: s.plotLayout, active: s.activePlotId,
                        plots: Object.values(s.charts).map(p => ({id:p.id, series:[...p.series.values()]}))};
                }""")

            container = page.locator("#chartContainer").bounding_box()
            initial = plot_window(1).bounding_box()
            self.assertEqual(page.locator(".plot-window").count(), 1)
            self.assertLess(container["width"] - initial["width"], 10)
            self.assertLess(container["height"] - initial["height"], 10)
            self.assertGreater(plot_window(1).locator("canvas").bounding_box()["height"], 650)
            field_input("battery").check()
            field_input("temperature").check()
            page.wait_for_function("Object.values(window.dzplotWorkspace.state.chartData).every(c => c.times.length >= 10) && !window.dzplotWorkspace.state.charts.plot_1.dirty")
            self.assertEqual(fields(1), ["battery", "temperature"])
            self.assertEqual(page.locator(".chart-canvas").count(), 1)
            # 实际像素确认两条曲线均绘制在这个 Canvas 中。
            colored_pixels = page.evaluate("""() => {
                const plot = window.dzplotWorkspace.state.charts.plot_1;
                const pixels = plot.ctx.getImageData(0, 0, plot.canvas.width, plot.canvas.height).data;
                return [...plot.series.values()].map(({color}) => {
                    const rgb = [1,3,5].map(i => parseInt(color.slice(i, i + 2), 16));
                    let count = 0;
                    for (let i = 0; i < pixels.length; i += 4) {
                        if (rgb.every((value, channel) => Math.abs(pixels[i + channel] - value) < 12)) count++;
                    }
                    return count;
                });
            }""")
            self.assertTrue(all(count > 50 for count in colored_pixels), colored_pixels)

            page.locator("#splitPlotRightBtn").click()
            left, right = plot_window(1).bounding_box(), plot_window(2).bounding_box()
            self.assertAlmostEqual(left["height"], initial["height"], delta=2)
            self.assertAlmostEqual(right["height"], initial["height"], delta=2)
            self.assertAlmostEqual(left["width"], right["width"], delta=2)
            self.assertGreater(right["x"], left["x"] + left["width"])
            self.assertEqual(fields(2), [])
            self.assertFalse(field_input("battery").is_checked())
            field_input("battery").check()
            self.assertEqual(fields(1), ["battery", "temperature"])
            self.assertEqual(fields(2), ["battery"])
            self.assertEqual(page.evaluate("Object.keys(window.dzplotWorkspace.state.chartData).length"), 2)
            self.assertTrue(page.evaluate("Object.values(window.dzplotWorkspace.state.chartData).every(c => c.times.length === new Set(c.times).size)"))
            select(1)
            self.assertTrue(field_input("temperature").is_checked())
            field_input("battery").uncheck()
            self.assertEqual(fields(2), ["battery"])
            select(2)
            self.assertTrue(field_input("battery").is_checked())
            self.assertFalse(field_input("temperature").is_checked())
            self.assertEqual(page.locator("#seriesCount").inner_text(), "1")

            page.locator("#splitPlotDownBtn").click()
            top, bottom = plot_window(2).bounding_box(), plot_window(3).bounding_box()
            self.assertAlmostEqual(top["width"], bottom["width"], delta=2)
            self.assertAlmostEqual(top["height"], bottom["height"], delta=2)
            self.assertGreater(bottom["y"], top["y"] + top["height"])
            self.assertAlmostEqual(top["height"] + bottom["height"] + 5, initial["height"], delta=2)
            select(1)
            page.locator('.field-row[data-field="temperature"]').drag_to(plot_window(3).locator(".plot-surface"))
            self.assertEqual(fields(3), ["temperature"])
            self.assertEqual(page.evaluate("window.dzplotWorkspace.state.activePlotId"), "plot_3")
            plot_window(1).locator(".plot-series").drag_to(plot_window(2).locator(".plot-surface"))
            self.assertEqual(fields(1), ["temperature"])
            self.assertEqual(fields(2), ["battery", "temperature"])
            select(3)
            page.locator(".topic-name").drag_to(plot_window(2).locator(".plot-surface"))
            all_fields = ["battery", "current_pose.x", "current_pose.y", "current_pose.z", "temperature"]
            self.assertEqual(fields(2), all_fields)
            self.assertEqual(fields(3), ["temperature"])
            self.assertEqual(page.evaluate("new Set([...window.dzplotWorkspace.state.charts.plot_2.series.values()].map(s => s.color)).size"), 5)
            select(3)
            self.assertTrue(page.locator(".topic-select").evaluate("el => el.indeterminate"))
            page.locator(".topic-select").check()
            self.assertEqual(fields(3), all_fields)
            page.locator(".topic-select").uncheck()
            self.assertEqual(fields(3), [])
            self.assertEqual(fields(2), all_fields)
            self.assertEqual(page.evaluate("Object.keys(window.dzplotWorkspace.state.chartData).length"), 5)
            page.locator('.field-row[data-field="current_pose.x"]').drag_to(plot_window(3).locator(".plot-surface"))
            self.assertEqual(fields(3), ["current_pose.x"])

            divider = page.locator('.plot-divider[aria-orientation="vertical"]').bounding_box()
            width = plot_window(1).bounding_box()["width"]
            page.mouse.move(divider["x"] + 2, divider["y"] + 100)
            page.mouse.down(); page.mouse.move(divider["x"] + 80, divider["y"] + 100, steps=5); page.mouse.up()
            self.assertGreater(plot_window(1).bounding_box()["width"], width + 50)
            height = plot_window(2).bounding_box()["height"]
            page.locator('.plot-divider[aria-orientation="horizontal"]').press("ArrowDown")
            self.assertGreater(plot_window(2).bounding_box()["height"], height + 5)
            saved = snapshot()
            page.reload(wait_until="networkidle")
            page.wait_for_function("window.dzplotWorkspace?.state.availableFields.demo_robot_state?.includes('temperature') && Object.values(window.dzplotWorkspace.state.chartData).every(c => c.times.length >= 20)")
            self.assertEqual(snapshot(), saved)
            self.assertEqual(page.locator("#seriesCount").inner_text(), "1")
            self.assertTrue(field_input("current_pose.x").is_checked())
            self.assertFalse(field_input("temperature").is_checked())
            page.screenshot(path=str(ARTIFACTS / "windows.png"))
            # 标签切换与窄屏仍保留分裂方向和窗口中的曲线。
            page.locator("#vizTab").click()
            page.frame_locator("#vizFrame").locator("#sceneCanvas").wait_for(state="visible")
            page.locator("#plotTab").click()
            self.assertEqual(snapshot(), saved)
            page.set_viewport_size({"width": 390, "height": 844})
            page.locator("#toggleSidebarTopBtn").click()
            for number in (1, 2, 3):
                rect = plot_window(number).bounding_box()
                self.assertGreater(rect["width"], 100)
                self.assertGreater(rect["height"], 200)
            self.assertEqual(snapshot(), saved)
            page.set_viewport_size({"width": 1569, "height": 941})
            page.locator("#toggleSidebarTopBtn").click()

            page.locator("#clearChartsBtn").click()
            self.assertEqual(fields(3), [])
            self.assertEqual(fields(2), all_fields)
            plot_window(3).locator(".plot-window-close").click()
            self.assertEqual(page.locator(".plot-window").count(), 2)
            self.assertAlmostEqual(plot_window(2).bounding_box()["height"], initial["height"], delta=2)
            page.locator("#mergePlotsBtn").click()
            self.assertEqual(page.locator(".plot-window").count(), 1)
            self.assertEqual(fields(1), all_fields)
            self.assertLess(container["height"] - plot_window(1).bounding_box()["height"], 10)
            self.assertTrue(plot_window(1).locator(".plot-window-close").is_disabled())

            # 离线清空后，刷新与自动重连均不能恢复后端残留的旧选择。
            page.evaluate("window.dzplotWorkspace.state.socket.close()")
            page.wait_for_function("!window.dzplotWorkspace.state.connected")
            page.locator("#clearChartsBtn").click()
            page.reload(wait_until="networkidle")
            page.wait_for_function("window.dzplotWorkspace?.state.availableFields.demo_robot_state?.includes('battery')")
            self.assertEqual(fields(1), [])
            self.assertEqual(page.evaluate("Object.keys(window.dzplotWorkspace.state.chartData).length"), 0)
            field_input("battery").check()
            page.wait_for_function("Object.values(window.dzplotWorkspace.state.chartData).some(c => c.times.length >= 4)")
            count = page.evaluate("window.dzplotWorkspace.state.topics.demo_robot_state.sample_count")
            page.evaluate("window.dzplotWorkspace.state.socket.close()")
            page.wait_for_function("!window.dzplotWorkspace.state.connected")
            page.locator("#clearChartsBtn").click()
            page.wait_for_function("count => window.dzplotWorkspace.state.connected && window.dzplotWorkspace.state.topics.demo_robot_state.sample_count > count + 4", arg=count)
            self.assertEqual(fields(1), [])
            self.assertEqual(page.evaluate("Object.keys(window.dzplotWorkspace.state.chartData).length"), 0)


ARTIFACTS = Path("/tmp/dzplot-workspace-artifacts")
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="dzplot 统一工作区验收")
    parser.add_argument("--browser", action="store_true", help="运行真实浏览器验收，需要 Playwright 和 Chrome / Chromium")
    parser.add_argument("--binding", action="store_true", help="验证实际绑定的 TLV / DZFlat 图像和点云，需要匹配 ABI 的 Python")
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS)
    args = parser.parse_args()
    ARTIFACTS = args.artifacts.resolve()
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    loader = unittest.defaultTestLoader
    suite = loader.loadTestsFromTestCase(WorkspaceIntegrationTests)
    suite.addTests(loader.loadTestsFromTestCase(WorkspaceReplayTests))
    if args.binding:
        suite.addTests(loader.loadTestsFromTestCase(WorkspaceBindingTests))
    if args.browser:
        suite.addTests(loader.loadTestsFromTestCase(WorkspaceBrowserTests))
    sys.exit(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1)
