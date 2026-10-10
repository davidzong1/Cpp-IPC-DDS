"""
Per-client isolated writer with slow-client detection and disconnect cleanup.

Replaces the single-threaded serial broadcast loop with per-client
asyncio tasks that never block each other.  Each client gets its own
bounded output queue; when a client cannot keep up its queue fills,
backpressure is applied only to that client's stream (not to all).

Slow-client heuristics
----------------------
A client is considered "slow" when its outbound queue exceeds
slow_threshold events.  The manager then:
  1. Emits a "client_slow" event so subscribers/downsamplers can react.
  2. After grace_period_s seconds of continuous slowness, disconnects
     the client cleanly.
"""

from __future__ import annotations

import asyncio
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

@dataclass
class ClientState:
    """Book-keeping for one connected WebSocket client."""

    writer: asyncio.StreamWriter
    client_id: str
    connected_at: float = 0.0
    queue: deque[bytes] = field(default_factory=deque)
    pending: List[Dict[str, Any]] = field(default_factory=list)
    max_queue: int = 256
    slow_threshold: int = 128
    grace_period_s: float = 5.0
    slow_since: float = 0.0  # monotonic timestamp, 0 = not slow
    is_slow: bool = False
    total_sent: int = 0
    total_dropped: int = 0
    last_send_time: float = 0.0
    disconnected: bool = False


# ---------------------------------------------------------------------------
# Client manager
# ---------------------------------------------------------------------------

class ClientManager:
    """Manage per-client queues and non-blocking broadcast.

    Intended as a companion to WebHub — not a replacement.  The hub
    calls ``enqueue(event)`` and receives back a set of disconnected
    clients to clean up.

    Parameters
    ----------
    max_queue_per_client : int
        Hard limit on pending byte-frames per client.
    slow_threshold : int
        Queue depth at which a client is marked "slow".
    grace_period_s : float
        Seconds of continuous slowness before forced disconnect.
    on_client_event : callable | None
        Called as on_client_event(kind: str, client_id: str, detail: dict).
    frame_encoder : callable
        Encodes a list of event dicts to a bytes frame for WS write.
    """

    def __init__(
        self,
        max_queue_per_client: int = 256,
        slow_threshold: int = 128,
        grace_period_s: float = 5.0,
        on_client_event: Optional[Callable[[str, str, Dict[str, Any]], None]] = None,
        frame_encoder: Optional[Callable[[List[Dict[str, Any]]], bytes]] = None,
        drain_interval_s: float = 0.016,  # ~60 Hz drain
    ) -> None:
        self.max_queue_per_client = max_queue_per_client
        self.slow_threshold = slow_threshold
        self.grace_period_s = grace_period_s
        self.on_client_event = on_client_event
        self.frame_encoder = frame_encoder or _default_frame_encoder
        self.drain_interval_s = drain_interval_s

        self._clients: Dict[str, ClientState] = {}
        self._next_id: int = 0
        self._running: bool = False
        self._drain_task: Optional[asyncio.Task] = None
        self._batch_buffer: Dict[str, List[Dict[str, Any]]] = {}
        self._batch_task: Optional[asyncio.Task] = None
        self._batch_max_latency_s: float = 0.016  # 60 Hz batch flush
        self._batch_max_count: int = 32

    # ------------------------------------------------------------------
    # Client lifecycle
    # ------------------------------------------------------------------

    def add_client(self, writer: asyncio.StreamWriter) -> str:
        """Register a new writer. Returns a stable client_id."""
        self._next_id += 1
        client_id = f"c{self._next_id}"
        self._clients[client_id] = ClientState(
            writer=writer,
            client_id=client_id,
            connected_at=time.monotonic(),
            max_queue=self.max_queue_per_client,
            slow_threshold=self.slow_threshold,
            grace_period_s=self.grace_period_s,
        )
        if self._running and self._drain_task is None:
            self._start_drain()
        self._emit("client_connected", client_id, {})
        return client_id

    def remove_client(self, client_id: str) -> bool:
        """Remove a client by id. Returns True if it existed."""
        state = self._clients.pop(client_id, None)
        if state is None:
            return False
        state.disconnected = True
        self._batch_buffer.pop(client_id, None)
        # Best-effort close
        try:
            state.writer.close()
        except Exception:
            pass
        self._emit(
            "client_disconnected",
            client_id,
            {"sent": state.total_sent, "dropped": state.total_dropped},
        )
        return True

    async def remove_client_async(self, client_id: str) -> bool:
        """Async variant that awaits writer close."""
        state = self._clients.pop(client_id, None)
        if state is None:
            return False
        state.disconnected = True
        self._batch_buffer.pop(client_id, None)
        try:
            state.writer.close()
            await state.writer.wait_closed()
        except Exception:
            pass
        self._emit(
            "client_disconnected",
            client_id,
            {"sent": state.total_sent, "dropped": state.total_dropped},
        )
        return True

    @property
    def client_count(self) -> int:
        return len(self._clients)

    @property
    def client_ids(self) -> Set[str]:
        return set(self._clients)

    @property
    def slow_clients(self) -> List[str]:
        return [cid for cid, s in self._clients.items() if s.is_slow]

    # ------------------------------------------------------------------
    # Enqueue (hot path — called from broadcast loop)
    # ------------------------------------------------------------------

    def enqueue(self, event: Dict[str, Any]) -> List[str]:
        """Stage an event for all connected clients.

        Returns a list of client IDs that need cleanup (disconnected
        or forced-off) so the caller can call remove_client for them.
        """
        dead: List[str] = []
        shutdown = event.get("kind") == "__shutdown__"

        for client_id, state in list(self._clients.items()):
            if state.disconnected:
                dead.append(client_id)
                continue

            # Bounded queue: if full, drop oldest
            while len(state.pending) >= state.max_queue:
                state.pending.pop(0)
                state.total_dropped += 1

            state.pending.append(event)

            # Slow-client detection
            queue_depth = len(state.pending)
            if queue_depth >= state.slow_threshold:
                if not state.is_slow:
                    state.is_slow = True
                    state.slow_since = time.monotonic()
                    self._emit("client_slow", client_id, {"depth": queue_depth})
                elif (
                    time.monotonic() - state.slow_since >= state.grace_period_s
                ):
                    self._emit(
                        "client_kicked",
                        client_id,
                        {"depth": queue_depth, "grace_s": self.grace_period_s},
                    )
                    dead.append(client_id)
            else:
                if state.is_slow:
                    state.is_slow = False
                    state.slow_since = 0.0
                    self._emit("client_recovered", client_id, {"depth": queue_depth})

        # Clean up dead clients synchronously
        for cid in dead:
            self.remove_client(cid)

        if shutdown:
            self._broadcast_shutdown()

        return dead

    # ------------------------------------------------------------------
    # Batch protocol
    # ------------------------------------------------------------------

    def configure_batch(
        self,
        max_latency_s: float = 0.016,
        max_count: int = 32,
    ) -> None:
        """Configure batch accumulation parameters."""
        self._batch_max_latency_s = max_latency_s
        self._batch_max_count = max_count

    async def _batch_flush_loop(self) -> None:
        """Periodic task that flushes accumulated batches to all clients."""
        while self._running:
            try:
                await asyncio.sleep(self._batch_max_latency_s)
            except asyncio.CancelledError:
                break
            await self._flush_batches()

    async def _flush_batches(self) -> None:
        """Write pending batches to each client."""
        dead: List[str] = []
        for client_id, state in list(self._clients.items()):
            if state.disconnected or not state.pending:
                continue
            try:
                # Atomically drain pending events for this client
                batch = state.pending[: self._batch_max_count]
                state.pending = state.pending[len(batch):]
                if not batch:
                    continue

                frame = self.frame_encoder(batch)
                state.writer.write(frame)
                await state.writer.drain()
                state.total_sent += len(batch)
                state.last_send_time = time.monotonic()
            except Exception:
                dead.append(client_id)

        for cid in dead:
            await self.remove_client_async(cid)

    # ------------------------------------------------------------------
    # Drain loop
    # ------------------------------------------------------------------

    async def _drain_loop(self) -> None:
        """Continuous drain loop that writes pending events per-client."""
        while self._running:
            try:
                await asyncio.sleep(self.drain_interval_s)
            except asyncio.CancelledError:
                break
            await self._drain_all()

    async def _drain_all(self) -> None:
        dead: List[str] = []
        for client_id, state in list(self._clients.items()):
            if state.disconnected or not state.pending:
                continue
            try:
                batch = state.pending[:]
                state.pending.clear()
                for event in batch:
                    frame = _default_frame_encoder([event])
                    state.writer.write(frame)
                await state.writer.drain()
                state.total_sent += len(batch)
                state.last_send_time = time.monotonic()
            except Exception:
                dead.append(client_id)
        for cid in dead:
            await self.remove_client_async(cid)

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Begin background drain task (must be called from running event loop)."""
        if self._running:
            return
        self._running = True
        self._start_drain()

    def _start_drain(self) -> None:
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return  # not in async context yet
        if self._drain_task is None or self._drain_task.done():
            self._drain_task = asyncio.create_task(self._drain_loop())
        if self._batch_task is None or self._batch_task.done():
            self._batch_task = asyncio.create_task(self._batch_flush_loop())

    async def shutdown(self) -> None:
        """Gracefully flush and disconnect all clients."""
        self._running = False
        # Final flush
        await self._drain_all()
        # Close all
        for client_id in list(self._clients):
            await self.remove_client_async(client_id)
        # Cancel tasks
        for task in (self._drain_task, self._batch_task):
            if task and not task.done():
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

    # ------------------------------------------------------------------
    # Stats
    # ------------------------------------------------------------------

    def snapshot(self) -> Dict[str, Any]:
        """Return aggregate statistics across all clients."""
        total_sent = sum(s.total_sent for s in self._clients.values())
        total_dropped = sum(s.total_dropped for s in self._clients.values())
        slow = self.slow_clients
        return {
            "client_count": len(self._clients),
            "slow_count": len(slow),
            "slow_ids": slow,
            "total_sent": total_sent,
            "total_dropped": total_dropped,
            "per_client": {
                cid: {
                    "queue_depth": len(s.pending),
                    "sent": s.total_sent,
                    "dropped": s.total_dropped,
                    "is_slow": s.is_slow,
                    "connected_s": time.monotonic() - s.connected_at,
                }
                for cid, s in self._clients.items()
            },
        }

    # ------------------------------------------------------------------
    # Broadcast one-shot (non-streaming) methods
    # ------------------------------------------------------------------

    async def broadcast_immediate(self, event: Dict[str, Any]) -> List[str]:
        """Send one event to all clients immediately (bypasses queue)."""
        dead: List[str] = []
        frame = _default_frame_encoder([event])
        for client_id, state in list(self._clients.items()):
            if state.disconnected:
                continue
            try:
                state.writer.write(frame)
                await state.writer.drain()
                state.total_sent += 1
            except Exception:
                dead.append(client_id)
        for cid in dead:
            await self.remove_client_async(cid)
        return dead

    def _broadcast_shutdown(self) -> None:
        """Best-effort shutdown frame to all clients (fire-and-forget)."""
        import json

        frame = _default_frame_encoder([{"kind": "shutdown"}])
        for state in self._clients.values():
            if state.disconnected:
                continue
            try:
                state.writer.write(frame)
            except Exception:
                pass

    def _emit(self, kind: str, client_id: str, detail: Dict[str, Any]) -> None:
        if self.on_client_event:
            try:
                self.on_client_event(kind, client_id, detail)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Default frame encoder
# ---------------------------------------------------------------------------

def _default_frame_encoder(events: List[Dict[str, Any]]) -> bytes:
    """Encode a list of events into a WebSocket text frame.

    Single event → single JSON frame.
    Multiple events → JSON array wrapped in a batch envelope.
    """
    import json

    if len(events) == 1:
        payload = json.dumps(events[0], separators=(",", ":"), ensure_ascii=False)
    else:
        payload = json.dumps(
            {"kind": "batch", "count": len(events), "events": events},
            separators=(",", ":"),
            ensure_ascii=False,
        )
    body = payload.encode("utf-8")
    return _make_ws_frame(body, opcode=1)


def _make_ws_frame(payload: bytes, opcode: int = 1) -> bytes:
    """Construct a WebSocket frame (no mask — server→client)."""
    import struct

    first = 0x80 | (opcode & 0x0F)
    size = len(payload)
    if size < 126:
        return bytes([first, size]) + payload
    if size <= 0xFFFF:
        return bytes([first, 126]) + struct.pack("!H", size) + payload
    return bytes([first, 127]) + struct.pack("!Q", size) + payload


__all__ = [
    "ClientManager",
    "ClientState",
]
