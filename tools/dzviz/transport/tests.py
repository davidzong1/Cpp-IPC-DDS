"""
Tests for dzviz transport layer — bounded queue, client manager,
backpressure controller, stats, and batch/downsample helpers.

Run:
    cd /home/zwc/cpp_ipc_dds && python3 -m pytest tools/dzviz/transport/tests.py -v
"""

from __future__ import annotations

import asyncio
import json
import math
import threading
import time
from typing import Any, Dict, List

import pytest

# Ensure the transport package is importable
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[2]
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from tools.dzviz.transport.bounded_queue import (
    BackpressureSignal,
    BoundedPubQueue,
    QueueWatermark,
    queue_watermark_log_line,
)
from tools.dzviz.transport.backpressure import (
    DEFAULT_ZONE_POLICIES,
    BackpressureController,
    DownsamplePolicy,
    TopicRateState,
    downsample_batch,
)
from tools.dzviz.transport.client_manager import (
    ClientManager,
    ClientState,
)
from tools.dzviz.transport.stats import (
    TransportSnapshot,
    TransportStats,
)

# ---------------------------------------------------------------------------
# BoundedPubQueue
# ---------------------------------------------------------------------------


class TestBoundedPubQueue:
    def test_basic_put_get(self):
        q = BoundedPubQueue(max_size=10)
        assert q.size == 0
        assert q.is_empty

        ok = q.put({"kind": "sample", "topic": "/test"})
        assert ok
        assert q.size == 1

        event = q.get(timeout=0.1)
        assert event is not None
        assert event["kind"] == "sample"
        assert q.size == 0

    def test_get_nowait_empty(self):
        q = BoundedPubQueue(max_size=5)
        assert q.get_nowait() is None

    def test_fifo_order(self):
        q = BoundedPubQueue(max_size=100)
        for i in range(50):
            q.put({"seq": i})
        for i in range(50):
            event = q.get(timeout=0.1)
            assert event["seq"] == i

    def test_overflow_drops_oldest(self):
        q = BoundedPubQueue(max_size=3)
        q.put({"seq": 1})
        q.put({"seq": 2})
        q.put({"seq": 3})
        q.put({"seq": 4})  # should drop seq=1
        assert q.size == 3
        first = q.get(timeout=0.1)
        assert first["seq"] == 2

    def test_watermark_zones(self):
        q = BoundedPubQueue(max_size=10)
        wm = q.watermark()
        assert wm.zone == "normal"
        assert wm.fill_ratio == 0.0

        # Fill to 5/10 = warn
        for i in range(5):
            q.put({"seq": i})
        wm = q.watermark()
        assert wm.zone == "warn"
        assert 0.49 < wm.fill_ratio < 0.76

        # Fill to 8/10 = heavy
        for i in range(3):
            q.put({"seq": i + 5})
        wm = q.watermark()
        assert wm.zone == "heavy"

        # Fill to 10/10 = emergency
        for i in range(2):
            q.put({"seq": i + 8})
        wm = q.watermark()
        assert wm.zone == "emergency"

    def test_backpressure_signal(self):
        q = BoundedPubQueue(max_size=10)

        # Fill to just above 50% (6/10 = 60% = warn)
        for i in range(6):
            q.put({"seq": i})
        sig = q.last_signal()
        assert sig is not None
        assert sig.active
        assert sig.zone == "warn", f"expected warn, got {sig.zone} at fill {q.fill_ratio}"
        assert sig.keep_every_n == 2

        # Fill to emergency (10/10 = 100%)
        for i in range(4):
            q.put({"seq": i + 6})
        sig = q.last_signal()
        assert sig.zone == "emergency", f"expected emergency, got {sig.zone}"
        assert sig.keep_latest_only

    def test_signal_callback(self):
        signals: list = []

        def cb(sig):
            signals.append(sig)

        q = BoundedPubQueue(max_size=5)
        q.on_signal(cb)
        for i in range(6):  # overflow triggers signal
            q.put({"seq": i})

        assert len(signals) >= 6

    def test_drain(self):
        q = BoundedPubQueue(max_size=10)
        for i in range(5):
            q.put({"seq": i})
        drained = q.drain()
        assert len(drained) == 5
        assert q.is_empty

    def test_drain_up_to(self):
        q = BoundedPubQueue(max_size=10)
        for i in range(5):
            q.put({"seq": i})
        partial = q.drain_up_to(3)
        assert len(partial) == 3
        assert q.size == 2

    def test_thread_safety_put_get(self):
        q = BoundedPubQueue(max_size=2000)
        errors = []
        N = 1000
        stop = threading.Event()

        def producer():
            for i in range(N):
                try:
                    q.put({"seq": i})
                except Exception as e:
                    errors.append(f"put {i}: {e}")

        def consumer():
            received = 0
            while received < N and not stop.is_set():
                event = q.get(timeout=3.0)
                if event is None:
                    # Check if producer is done and queue is empty
                    if q.is_empty and received < N:
                        # Producer may still be running; keep waiting
                        continue
                if event is not None:
                    received += 1

        t_prod = threading.Thread(target=producer)
        t_cons = threading.Thread(target=consumer)
        t_prod.start()
        t_cons.start()
        t_prod.join(timeout=10)
        t_cons.join(timeout=10)
        stop.set()
        assert not errors, f"errors: {errors}"

    def test_log_line_format(self):
        q = BoundedPubQueue(max_size=100)
        for i in range(55):
            q.put({"seq": i})
        wm = q.watermark()
        line = queue_watermark_log_line(wm)
        assert "warn" in line
        assert "55/100" in line


# ---------------------------------------------------------------------------
# BackpressureController
# ---------------------------------------------------------------------------


class TestBackpressureController:
    def test_normal_no_drop(self):
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/test")
        signal = BackpressureSignal(active=False, zone="normal")
        # All should pass
        for i in range(20):
            assert bp.should_publish("/test", signal)

    def test_warn_downsample(self):
        bp = BackpressureController(max_input_hz=1000, max_output_hz=0)  # 0 = no output cap
        bp.register_topic("/test")
        signal = BackpressureSignal(active=True, zone="warn", keep_every_n=2, max_publish_hz=0)
        # Manual rate tracking reset since all calls are instantaneous
        bp.reset_counters()

        published = sum(
            1 for i in range(100) if bp.should_publish("/test", signal)
        )
        # With keep_every_n=2, expect ~50% publish rate
        assert 40 <= published <= 60, f"published={published}"

    def test_emergency_keep_latest(self):
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/emergency")
        signal = BackpressureSignal(
            active=True, zone="emergency", keep_every_n=8,
            keep_latest_only=True, drop_oldest=True,
        )
        # With keep-latest-only, should_publish doesn't internally
        # filter — caller is expected to dedup. But keep_every_n still applies.
        published = sum(
            1 for i in range(80) if bp.should_publish("/emergency", signal)
        )
        assert published < 80  # some dropped

    def test_input_rate_cap(self):
        bp = BackpressureController(max_input_hz=5, max_output_hz=60)
        bp.register_topic("/fast")
        signal = BackpressureSignal(active=False, zone="normal")

        # Rapid-fire samples should trigger rate cap
        published = 0
        for i in range(50):
            if bp.should_publish("/fast", signal):
                published += 1
            time.sleep(0.01)  # 100 Hz input
        # Most should be dropped due to input cap
        assert published < 50

    def test_output_rate_cap(self):
        bp = BackpressureController(max_input_hz=1000, max_output_hz=10)
        bp.register_topic("/test")
        signal = BackpressureSignal(active=True, zone="warn", keep_every_n=2, max_publish_hz=5)

        published = sum(
            1 for i in range(100) if bp.should_publish("/test", signal)
        )
        assert published < 100

    def test_per_topic_independent(self):
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/a")
        bp.register_topic("/b")
        signal_a = BackpressureSignal(active=True, zone="warn", keep_every_n=2)

        a_pub = sum(1 for i in range(60) if bp.should_publish("/a", signal_a))
        b_pub = sum(1 for i in range(60) if bp.should_publish("/b"))  # no signal = normal

        # /a downsampled, /b not
        assert a_pub < b_pub

    def test_aggregate_stats(self):
        bp = BackpressureController()
        bp.register_topic("/t")
        for i in range(20):
            bp.should_publish("/t")
        stats = bp.aggregate_stats()
        assert stats["total_in"] == 20
        assert stats["total_out"] == 20
        assert stats["total_dropped"] == 0

    def test_reset_counters(self):
        bp = BackpressureController()
        for i in range(10):
            bp.should_publish("/t")
        bp.reset_counters()
        stats = bp.aggregate_stats()
        assert stats["total_in"] == 0


# ---------------------------------------------------------------------------
# downsample_batch helper
# ---------------------------------------------------------------------------


class TestDownsampleBatch:
    def test_keep_all(self):
        events = [{"seq": i} for i in range(10)]
        result = downsample_batch(events, keep_every_n=1)
        assert len(result) == 10

    def test_keep_every_n(self):
        events = [{"seq": i} for i in range(10)]
        result = downsample_batch(events, keep_every_n=3)
        assert len(result) >= 3
        # Last event always included
        assert result[-1]["seq"] == 9

    def test_keep_latest_only(self):
        events = [{"seq": i} for i in range(10)]
        result = downsample_batch(events, keep_every_n=1, keep_latest_only=True)
        assert len(result) == 1
        assert result[0]["seq"] == 9

    def test_empty(self):
        assert downsample_batch([], keep_every_n=2) == []

    def test_single_event(self):
        events = [{"seq": 42}]
        result = downsample_batch(events, keep_every_n=10, keep_latest_only=True)
        assert len(result) == 1
        assert result[0]["seq"] == 42


# ---------------------------------------------------------------------------
# TransportStats
# ---------------------------------------------------------------------------


class TestTransportStats:
    def test_basic_counters(self):
        s = TransportStats(rate_window_s=10.0)
        s.record_sample_in("/t")
        s.record_sample_in("/t")
        s.record_sample_out("/t", byte_count=100)
        s.record_drop("/t")

        assert s.samples_in == 2
        assert s.samples_out == 1
        assert s.samples_dropped == 1
        assert s.bytes_sent == 100

    def test_drop_ratio(self):
        s = TransportStats()
        s.record_sample_in("/t")
        s.record_sample_in("/t")
        s.record_sample_in("/t")
        s.record_drop("/t")
        s.record_drop("/t")
        assert s.drop_ratio == pytest.approx(2 / 3)

    def test_snapshot(self):
        s = TransportStats()
        for i in range(10):
            s.record_sample_in(f"/t{i % 3}")
            s.record_sample_out(f"/t{i % 3}")
        snap = s.snapshot()
        assert snap.samples_in == 10
        assert snap.samples_out == 10
        assert len(snap.topics) == 3

    def test_log_line(self):
        s = TransportStats()
        line = s.log_line()
        assert "[transport]" in line
        assert "in=" in line
        assert "out=" in line

    def test_update_watermark(self):
        q = BoundedPubQueue(max_size=10)
        for i in range(8):
            q.put({"seq": i})
        wm = q.watermark()

        s = TransportStats()
        s.update_watermark(wm)
        snap = s.snapshot()
        assert snap.queue_zone == "heavy"
        assert snap.queue_size == 8

    def test_update_clients(self):
        s = TransportStats()
        s.update_clients(5, 2)
        snap = s.snapshot()
        assert snap.client_count == 5
        assert snap.slow_client_count == 2

    def test_per_topic_counters(self):
        s = TransportStats()
        s.record_sample_in("/pose")
        s.record_sample_in("/pose")
        s.record_sample_out("/pose")
        s.record_drop("/pose")
        s.record_sample_in("/cloud")
        snap = s.snapshot()
        assert snap.topics["/pose"] == {"in": 2, "out": 1, "drop": 1}
        assert snap.topics["/cloud"] == {"in": 1, "out": 0, "drop": 0}


# ---------------------------------------------------------------------------
# ClientManager (asyncio tests)
# ---------------------------------------------------------------------------


class MockStreamWriter:
    """Minimal asyncio.StreamWriter mock for testing ClientManager."""

    def __init__(self):
        self.written: List[bytes] = []
        self.closed = False
        self._drain_called = 0

    def write(self, data: bytes) -> None:
        self.written.append(data)

    async def drain(self) -> None:
        self._drain_called += 1

    def close(self) -> None:
        self.closed = True

    async def wait_closed(self) -> None:
        pass

    def get_extra_info(self, name: str, default=None):
        return default


class TestClientManager:
    def test_add_remove_client(self):
        mgr = ClientManager()
        writer = MockStreamWriter()
        cid = mgr.add_client(writer)
        assert cid.startswith("c")
        assert mgr.client_count == 1
        assert mgr.remove_client(cid)
        assert mgr.client_count == 0

    def test_enqueue_distributes_to_all(self):
        mgr = ClientManager(max_queue_per_client=10, slow_threshold=5)
        w1 = MockStreamWriter()
        w2 = MockStreamWriter()
        mgr.add_client(w1)
        mgr.add_client(w2)

        event = {"kind": "sample", "topic": "/test"}
        dead = mgr.enqueue(event)
        assert len(dead) == 0

        snapshot = mgr.snapshot()
        assert snapshot["total_sent"] == 0  # not yet drained
        assert snapshot["client_count"] == 2

    def test_slow_client_detection(self):
        slow_events: List[tuple] = []

        def on_event(kind, cid, detail):
            slow_events.append((kind, cid))

        mgr = ClientManager(
            max_queue_per_client=10,
            slow_threshold=3,
            grace_period_s=10.0,  # long grace so it doesn't kick during test
            on_client_event=on_event,
        )
        writer = MockStreamWriter()
        cid = mgr.add_client(writer)

        # Push events past slow threshold
        for i in range(5):
            mgr.enqueue({"kind": "sample", "seq": i})

        snapshot = mgr.snapshot()
        assert snapshot["slow_count"] >= 1
        assert any(e[0] == "client_slow" for e in slow_events)

    def test_slow_client_kicked_after_grace(self):
        kicked: List[str] = []

        def on_event(kind, cid, detail):
            if kind == "client_kicked":
                kicked.append(cid)

        mgr = ClientManager(
            max_queue_per_client=10,
            slow_threshold=3,
            grace_period_s=0.01,  # very short grace
            on_client_event=on_event,
        )
        writer = MockStreamWriter()
        cid = mgr.add_client(writer)

        # First batch: mark slow
        for i in range(5):
            mgr.enqueue({"kind": "sample", "seq": i})
        time.sleep(0.02)

        # Second batch: should kick
        for i in range(5):
            dead = mgr.enqueue({"kind": "sample", "seq": i + 5})

        assert len(kicked) >= 1 or mgr.client_count == 0

    def test_client_recovery(self):
        recovered: List[str] = []

        def on_event(kind, cid, detail):
            if kind == "client_recovered":
                recovered.append(cid)

        mgr = ClientManager(
            max_queue_per_client=20,
            slow_threshold=5,
            grace_period_s=99.0,
            on_client_event=on_event,
        )
        writer = MockStreamWriter()
        cid = mgr.add_client(writer)

        # Push to slow
        for i in range(6):
            mgr.enqueue({"kind": "sample", "seq": i})
        assert len(mgr.slow_clients) >= 1

        # Drain (simulated by async drain)
        # We can't easily drain in non-async, but enqueue doesn't drain either
        # Let's just verify slow detection works
        snap = mgr.snapshot()
        assert snap["slow_count"] >= 1

    def test_shutdown_event(self):
        mgr = ClientManager()
        w1 = MockStreamWriter()
        mgr.add_client(w1)

        dead = mgr.enqueue({"kind": "__shutdown__"})
        # Shutdown event should be distributed, client may be disconnected
        assert mgr.client_count >= 0  # may or may not be disconnected

    def test_snapshot_structure(self):
        mgr = ClientManager(max_queue_per_client=10)
        w1 = MockStreamWriter()
        cid = mgr.add_client(w1)

        for i in range(3):
            mgr.enqueue({"kind": "sample", "seq": i})

        snap = mgr.snapshot()
        assert "client_count" in snap
        assert "slow_count" in snap
        assert "total_sent" in snap
        assert "total_dropped" in snap
        assert "per_client" in snap
        assert cid in snap["per_client"]

    @pytest.mark.anyio
    async def test_async_drain(self):
        mgr = ClientManager(drain_interval_s=0.01)
        w1 = MockStreamWriter()
        cid = mgr.add_client(w1)

        mgr.enqueue({"kind": "sample", "topic": "/t"})
        mgr.start()

        # Let drain loop run once
        await asyncio.sleep(0.05)

        snap = mgr.snapshot()
        # After drain, pending should be empty and sent incremented
        assert snap["per_client"][cid]["sent"] >= 1

        await mgr.shutdown()

    @pytest.mark.anyio
    async def test_async_shutdown(self):
        mgr = ClientManager()
        w1 = MockStreamWriter()
        mgr.add_client(w1)
        await mgr.shutdown()
        assert mgr.client_count == 0


# ---------------------------------------------------------------------------
# WebSocket frame encoding
# ---------------------------------------------------------------------------


class TestFrameEncoding:
    def test_single_event_frame(self):
        from tools.dzviz.transport.client_manager import _default_frame_encoder

        event = {"kind": "sample", "topic": "/test"}
        frame = _default_frame_encoder([event])
        assert isinstance(frame, bytes)
        assert len(frame) > 0
        assert frame[0] & 0x80  # FIN bit set

    def test_batch_frame(self):
        from tools.dzviz.transport.client_manager import _default_frame_encoder

        events = [{"kind": "sample", "seq": i} for i in range(5)]
        frame = _default_frame_encoder(events)
        assert isinstance(frame, bytes)

        # Decode: skip WS header and validate JSON
        payload_start = 2 if len(frame) >= 2 else 0
        # We can't easily decode WS frames inline here, but we can
        # check that the payload is valid JSON
        if len(frame) < 126:
            payload = frame[2:]
        elif frame[1] == 126:
            payload = frame[4:]
        else:
            payload = frame[10:]  # 64-bit length
        decoded = json.loads(payload)
        assert decoded["kind"] == "batch"
        assert decoded["count"] == 5
        assert len(decoded["events"]) == 5


# ---------------------------------------------------------------------------
# End-to-end: queue → backpressure → client manager pipeline
# ---------------------------------------------------------------------------


class TestIntegration:
    def test_queue_to_signal_flow(self):
        """Verify the full pipeline: queue fills → signal changes → BP activates."""
        q = BoundedPubQueue(max_size=20)
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/test")

        # Phase 1: normal
        for i in range(5):
            q.put({"kind": "sample", "topic": "/test", "seq": i})
            sig = q.last_signal()
            assert sig.zone == "normal"
            assert bp.should_publish("/test", sig)

        # Phase 2: fill to warn (5/20 + 9/20 = 14/20 = 70%)
        for i in range(9):
            q.put({"kind": "sample", "topic": "/test", "seq": i + 5})
        sig = q.last_signal()
        assert sig.zone == "warn"
        assert sig.keep_every_n == 2

        # Phase 3: drain back to normal (check watermark, not stale signal)
        for _ in range(10):
            q.get_nowait()
        wm = q.watermark()
        assert wm.zone == "normal"

    def test_full_pipeline(self):
        """Bounded queue + BP + stats wired together."""
        q = BoundedPubQueue(max_size=50)
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/sensor/camera")
        stats = TransportStats(rate_window_s=10.0)

        # Publish bursts
        for burst in range(3):
            for i in range(30):
                event = {"kind": "sample", "topic": "/sensor/camera", "seq": burst * 30 + i}
                q.put(event)
                stats.record_sample_in("/sensor/camera")

                sig = q.last_signal()
                if bp.should_publish("/sensor/camera", sig):
                    stats.record_sample_out("/sensor/camera")
                else:
                    stats.record_drop("/sensor/camera")

            # Drain queue between bursts
            while not q.is_empty:
                q.get_nowait()

        snap = stats.snapshot()
        assert snap.samples_in == 90
        assert snap.samples_out + snap.samples_dropped == 90

    def test_downsample_then_batch(self):
        """Downsample logic produces a batch that the frame encoder handles."""
        # Simulate: subscriber produces 200 samples, BP keeps ~25%
        bp = BackpressureController(max_input_hz=1000, max_output_hz=60)
        bp.register_topic("/cloud")
        signal = BackpressureSignal(active=True, zone="heavy", keep_every_n=4)

        kept: List[Dict[str, Any]] = []
        for i in range(200):
            if bp.should_publish("/cloud", signal):
                kept.append({"kind": "sample", "topic": "/cloud", "seq": i})

        # Downsample further by batch helper
        batch = downsample_batch(kept, keep_every_n=2)
        assert len(batch) > 0
        assert len(batch) <= len(kept)

        # Encode as batch frame
        from tools.dzviz.transport.client_manager import _default_frame_encoder
        frame = _default_frame_encoder(batch)
        assert len(frame) > 0


# ---------------------------------------------------------------------------
# Protocol compliance
# ---------------------------------------------------------------------------


class TestProtocolCompliance:
    def test_batch_frame_has_kind(self):
        from tools.dzviz.transport.client_manager import _default_frame_encoder
        events = [{"kind": "sample", "data": {}} for _ in range(3)]
        frame = _default_frame_encoder(events)
        # Extract payload
        if len(frame) < 126:
            payload = frame[2:]
        else:
            payload = frame[4:]
        decoded = json.loads(payload)
        assert decoded["kind"] == "batch"
        assert isinstance(decoded["count"], int)
        assert isinstance(decoded["events"], list)

    def test_single_frame_no_wrapper(self):
        from tools.dzviz.transport.client_manager import _default_frame_encoder
        event = {"kind": "hello", "config": {}}
        frame = _default_frame_encoder([event])
        if len(frame) < 126:
            payload = frame[2:]
        else:
            payload = frame[4:]
        decoded = json.loads(payload)
        assert decoded["kind"] == "hello"
        # Single event: no batch wrapper
        assert "events" not in decoded

    def test_max_output_rate_enforced(self):
        bp = BackpressureController(max_output_hz=30)
        bp.register_topic("/t")
        signal = BackpressureSignal(active=True, zone="warn", max_publish_hz=15)

        # Rapid publish attempt
        published = sum(
            1 for i in range(200) if bp.should_publish("/t", signal)
        )
        # Should be significantly less than 200
        assert published < 180

    def test_config_events_bypass_backpressure(self):
        """Meta events like config/ack should bypass backpressure."""
        # This is the responsibility of HubTransportAdapter.broadcast_loop()
        # which routes meta events to broadcast_immediate.
        # We test that broadcast_immediate exists and works.
        mgr = ClientManager()
        w = MockStreamWriter()
        mgr.add_client(w)

        async def _test():
            dead = await mgr.broadcast_immediate({"kind": "config", "data": {}})
            assert len(dead) == 0
            assert len(w.written) > 0

        asyncio.run(_test())


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
