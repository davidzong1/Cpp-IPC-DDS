"""
dzviz transport layer — bounded queue, batch protocol, client isolation,
backpressure control, and transport statistics.

Intended as an independent backend module that lives alongside (not inside)
the existing dzviz.py WebHub.  The WebHub or its equivalent can optionally
adopt these components without structural changes.

Quick start (standalone)
------------------------
    import asyncio
    from tools.dzviz.transport import (
        BoundedPubQueue, ClientManager, BackpressureController, TransportStats,
    )

    # 1. Replace unbounded queue.Queue with bounded variant
    events = BoundedPubQueue(max_size=4096)

    # 2. Per-client non-blocking broadcast
    mgr = ClientManager(max_queue_per_client=256, slow_threshold=128)

    # 3. Multi-zone backpressure
    bp = BackpressureController(max_input_hz=1000.0, max_output_hz=60.0)

    # 4. Stats collector
    stats = TransportStats(rate_window_s=2.0)

Integration with dzviz.py WebHub
--------------------------------
    # In WebHub.__init__:
    from tools.dzviz.transport import BoundedPubQueue, HubTransportAdapter

    self.events = BoundedPubQueue(max_size=4096)
    self.transport = HubTransportAdapter(self, max_queue=4096)

    # In broadcast_loop():
    # Replace: event = await asyncio.to_thread(self.events.get, True, 0.2)
    # With:    event = await asyncio.to_thread(self.transport.get_event, 0.2)
    #
    # Replace the for-writer broadcast block with:
    # await self.transport.broadcast(event)

Protocol
--------
Batch frame (JSON over WS text frame):
    {
        "kind": "batch",
        "count": N,
        "seq": 0,
        "timestamp_ms": 1234567890,
        "events": [ ... ]
    }

Single frame (unchanged):
    { "kind": "sample", "topic": "...", ... }
"""

from .bounded_queue import (
    BackpressureSignal,
    BoundedPubQueue,
    QueueWatermark,
    queue_watermark_log_line,
)
from .client_manager import ClientManager, ClientState
from .backpressure import (
    DEFAULT_ZONE_POLICIES,
    BackpressureController,
    DownsamplePolicy,
    TopicRateState,
    downsample_batch,
)
from .stats import TransportSnapshot, TransportStats

# ---------------------------------------------------------------------------
# HubTransportAdapter — optional bridge between WebHub and transport layer
# ---------------------------------------------------------------------------

import asyncio
import json
import queue
import time
from typing import Any, Dict, List, Optional


class HubTransportAdapter:
    """Adapter that wraps WebHub with bounded queue + client manager.

    This is a **non-invasive** adapter — it does not modify WebHub.
    Instead, it provides replacement methods for broadcast_loop and
    send logic that callers can opt into.

    Usage inside dzviz.py run_server()::

        from tools.dzviz.transport import HubTransportAdapter

        hub = WebHub(defaults, config_path)
        transport = HubTransportAdapter(hub, max_queue=4096)

        # Replace hub.broadcast_loop() with:
        broadcast_task = asyncio.create_task(transport.broadcast_loop())

    The adapter maintains its own per-client state, so WebHub.clients
    is still used for connection tracking but send/broadcast is
    delegated to ClientManager.
    """

    def __init__(
        self,
        hub: Any,
        max_queue: int = 4096,
        max_queue_per_client: int = 256,
        slow_threshold: int = 128,
        slow_grace_s: float = 5.0,
        max_input_hz: float = 1000.0,
        max_output_hz: float = 60.0,
        drain_interval_s: float = 0.016,  # ~60 Hz
        batch_max_latency_s: float = 0.016,
        batch_max_count: int = 32,
        stats_interval_s: float = 2.0,
        verbose: bool = False,
    ) -> None:
        self.hub = hub
        self.verbose = verbose

        # Replace hub's event queue with bounded variant
        self._bounded_queue = BoundedPubQueue(max_size=max_queue)
        hub.events = self._bounded_queue  # type: ignore[assignment]

        # Client manager
        self._clients = ClientManager(
            max_queue_per_client=max_queue_per_client,
            slow_threshold=slow_threshold,
            grace_period_s=slow_grace_s,
            on_client_event=self._on_client_event,
            drain_interval_s=drain_interval_s,
        )
        self._clients.configure_batch(
            max_latency_s=batch_max_latency_s,
            max_count=batch_max_count,
        )

        # Backpressure
        self._backpressure = BackpressureController(
            max_input_hz=max_input_hz,
            max_output_hz=max_output_hz,
        )
        self._backpressure.on_drop(self._on_bp_drop)

        # Stats
        self._stats = TransportStats(rate_window_s=2.0)

        # Wiring: queue → backpressure signal
        self._bounded_queue.on_signal(self._on_queue_signal)

        # Periodic stats task
        self._stats_task: Optional[asyncio.Task] = None
        self._stats_interval_s = stats_interval_s

    # ------------------------------------------------------------------
    # Event sources
    # ------------------------------------------------------------------

    def get_event(self, timeout: float = 0.2) -> Optional[Dict[str, Any]]:
        """Blocking get (for use with asyncio.to_thread)."""
        try:
            event = self._bounded_queue.get(timeout=timeout)
        except Exception:
            return None
        if event is None:
            return None
        self._stats.record_sample_in(topic=event.get("topic", ""))
        return event

    # ------------------------------------------------------------------
    # Broadcasting
    # ------------------------------------------------------------------

    async def broadcast(self, event: Dict[str, Any]) -> None:
        """Send one event to all clients, with backpressure and client isolation."""
        topic = event.get("topic", "")
        signal = self._bounded_queue.last_signal()

        # Backpressure decision
        if not self._backpressure.should_publish(topic, signal):
            self._stats.record_drop(topic)
            return

        # Enqueue for all clients (non-blocking per-client queues)
        dead = self._clients.enqueue(event)
        self._stats.record_sample_out(topic)

        # Clean up dead clients from WebHub
        for cid in dead:
            # We need the writer reference; ClientManager already removed it.
            # WebHub cleanup may need the writer — we just notify.
            if self.verbose:
                print(f"[transport] client {cid} disconnected", flush=True)

        # Update stats
        wm = self._bounded_queue.watermark()
        self._stats.update_watermark(wm)
        snap = self._clients.snapshot()
        self._stats.update_clients(
            count=snap["client_count"],
            slow_count=snap["slow_count"],
        )

    async def broadcast_immediate(self, event: Dict[str, Any]) -> None:
        """Bypass queue and send immediately to all (for config/ack events)."""
        dead = await self._clients.broadcast_immediate(event)
        for cid in dead:
            pass  # WebHub cleanup

    async def broadcast_loop(self) -> None:
        """Replacement for WebHub.broadcast_loop().

        Uses bounded queue + client manager instead of raw queue.Queue +
        serial client iteration.
        """
        import threading

        self._clients.start()
        self._start_stats()

        loop = asyncio.get_running_loop()
        shutdown = False

        while not shutdown:
            try:
                event = await loop.run_in_executor(None, self.get_event, 0.2)
            except asyncio.CancelledError:
                break

            if event is None:
                continue

            if event.get("kind") == "__shutdown__":
                shutdown = True

            # --- Topic / sample bookkeeping (mirrors WebHub logic) ---
            if event.get("kind") == "sample":
                topic_meta = self.hub.topics.setdefault(event["topic"], {})
                topic_meta.update({
                    "name": event["topic"],
                    "type": event.get("msg_type", topic_meta.get("type", "")),
                    "msg_type": event.get("msg_type", topic_meta.get("msg_type", "")),
                    "transport": event.get("transport", topic_meta.get("transport", "")),
                    "domain": event.get("domain", topic_meta.get("domain", 0)),
                    "queue": event.get("queue", topic_meta.get("queue", 10)),
                    "extra": event.get("extra", topic_meta.get("extra", "")),
                    "updated_ms": event.get("timestamp_ms", int(time.time() * 1000)),
                    "active": True,
                })

            # --- Broadcast ---
            is_meta_event = event.get("kind") in (
                "hello", "config", "ack", "topic_added", "topic_removed",
                "robot_added", "robot_updated", "robot_removed",
                "robot_state_added", "robot_state_removed",
                "error",
            )
            if is_meta_event:
                # Meta events bypass backpressure
                await self.broadcast_immediate(event)
            else:
                await self.broadcast(event)

            # Handle robot state processing (delegated to hub)
            if event.get("kind") == "sample":
                for state_display in getattr(self.hub, "robot_state_displays", {}).values():
                    if state_display.topic == event.get("topic"):
                        robot_state_result = state_display.handle_joint_state_sample(
                            event.get("data", {})
                        )
                        event["robot_state_display_id"] = state_display.id
                        event["robot_id"] = state_display.target_robot_id
                        if robot_state_result.get("ok"):
                            event["robot_state"] = robot_state_result
                            event["robot_joint_state"] = robot_state_result.get("joint_state", {})
                            event["robot_link_states"] = robot_state_result.get("link_states", [])
                        else:
                            event["robot_state_error"] = robot_state_result

    # ------------------------------------------------------------------
    # Client lifecycle (called from handle_client)
    # ------------------------------------------------------------------

    async def add_client(self, writer: asyncio.StreamWriter) -> None:
        """Register a new WebSocket client.

        Sends the hello message via the adapter (bypasses queue).
        """
        client_id = self._clients.add_client(writer)
        # Send hello via the hub's existing mechanism or directly
        try:
            hello_event = {
                "kind": "hello",
                "topics": list(self.hub.topics.values()),
                "config": self.hub.export_config(),
                "config_path": self.hub.config_path_str(),
                "message_types": getattr(self.hub, "message_types", []),
                "message_type_map": getattr(
                    self.hub, "message_type_map",
                    {
                        "RawMessage": "StdRawMessage",
                        "Pose": "StdPose",
                        "Path": "StdPath",
                        "PointCloud": "StdPointCloud",
                        "Marker": "StdMarker",
                        "Image": "StdImage",
                        "TF": "StdTF",
                        "RobotState": "RobotState",
                    },
                ),
            }
            frame = _encode_frame(hello_event)
            writer.write(frame)
            await writer.drain()
        except Exception:
            self._clients.remove_client(client_id)

    async def remove_client(self, writer: asyncio.StreamWriter) -> None:
        """Remove a WebSocket client."""
        # Find client_id by writer
        for cid, state in list(self._clients._clients.items()):
            if state.writer is writer:
                await self._clients.remove_client_async(cid)
                return

    # ------------------------------------------------------------------
    # Stats
    # ------------------------------------------------------------------

    async def _stats_loop(self) -> None:
        while True:
            try:
                await asyncio.sleep(self._stats_interval_s)
            except asyncio.CancelledError:
                break
            if self.verbose:
                print(self._stats.log_line(), flush=True)

    def _start_stats(self) -> None:
        if self._stats_task is None or self._stats_task.done():
            try:
                self._stats_task = asyncio.create_task(self._stats_loop())
            except RuntimeError:
                pass

    def stats_snapshot(self) -> TransportSnapshot:
        return self._stats.snapshot()

    # ------------------------------------------------------------------
    # Shutdown
    # ------------------------------------------------------------------

    async def shutdown(self) -> None:
        """Graceful shutdown of transport layer."""
        self._bounded_queue.put({"kind": "__shutdown__"})
        await self._clients.shutdown()
        if self._stats_task and not self._stats_task.done():
            self._stats_task.cancel()
            try:
                await self._stats_task
            except asyncio.CancelledError:
                pass

    # ------------------------------------------------------------------
    # Internal callbacks
    # ------------------------------------------------------------------

    def _on_queue_signal(self, signal: BackpressureSignal) -> None:
        wm = signal.watermark
        if wm and self.verbose:
            print(queue_watermark_log_line(wm), flush=True)

    def _on_client_event(self, kind: str, client_id: str, detail: Dict[str, Any]) -> None:
        if self.verbose:
            if kind == "client_slow":
                print(
                    f"[transport] WARNING slow client {client_id} "
                    f"depth={detail.get('depth', 0)}",
                    flush=True,
                )
            elif kind == "client_kicked":
                print(
                    f"[transport] KICKED slow client {client_id} "
                    f"after {detail.get('grace_s', 0)}s "
                    f"depth={detail.get('depth', 0)}",
                    flush=True,
                )
            elif kind == "client_recovered":
                print(f"[transport] client {client_id} recovered", flush=True)

    def _on_bp_drop(self, topic: str, reason: str, detail: str) -> None:
        if self.verbose:
            print(f"[transport] DROP {topic}: {reason} ({detail})", flush=True)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _encode_frame(event: Dict[str, Any]) -> bytes:
    """Encode a single event into a WebSocket text frame."""
    from .client_manager import _make_ws_frame

    payload = json.dumps(
        event, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return _make_ws_frame(payload)


__all__ = [
    # Queue
    "BoundedPubQueue",
    "QueueWatermark",
    "BackpressureSignal",
    "queue_watermark_log_line",
    # Client management
    "ClientManager",
    "ClientState",
    # Backpressure
    "BackpressureController",
    "DownsamplePolicy",
    "TopicRateState",
    "DEFAULT_ZONE_POLICIES",
    "downsample_batch",
    # Stats
    "TransportStats",
    "TransportSnapshot",
    # Adapter
    "HubTransportAdapter",
]
