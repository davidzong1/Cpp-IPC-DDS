"""
Bounded publish queue with watermark statistics and backpressure signals.

Replaces the unbounded queue.Queue in WebHub.broadcast_loop() with a
capacity-bounded queue that tracks fill-level watermarks and emits
backpressure signals when approaching capacity.

Queue semantics:
  - Below 50% fill: normal operation, no drops.
  - 50-75% fill:  light backpressure, downsampling recommended.
  - 75-90% fill:  heavy backpressure, aggressive downsampling.
  - Above 90% fill: emergency, keep-latest-only or drop-oldest.

Thread-safe.  Designed to be a drop-in transport-level queue — the
existing dzviz.py WebHub can wrap its self.events with this class
without changing any subscriber or broadcast logic.
"""

from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Dict, Optional


# ---------------------------------------------------------------------------
# Watermark / stats types
# ---------------------------------------------------------------------------

@dataclass
class QueueWatermark:
    """Snapshot of queue fill level at a point in time."""

    current_size: int = 0
    max_size: int = 0
    fill_ratio: float = 0.0  # 0.0 – 1.0
    zone: str = "normal"  # normal | warn | heavy | emergency
    total_published: int = 0
    total_dropped: int = 0
    timestamp_ms: int = 0


@dataclass
class BackpressureSignal:
    """Advice emitted when the queue watermark crosses a threshold.

    Callers (e.g. a batch publisher or subscriber) can use these values to
    decide *how* to reduce the data rate without inspecting queue internals.
    """

    # ---- mandatory flags ----
    active: bool = False
    zone: str = "normal"  # normal | warn | heavy | emergency

    # ---- rate limiting ----
    keep_every_n: int = 1  # keep 1 out of every N samples (1 = keep all)
    max_publish_hz: float = 0.0  # 0 = unlimited; else cap publish rate

    # ---- emergency strategy ----
    drop_oldest: bool = False  # True → discard oldest; False → drop newest
    keep_latest_only: bool = False  # True → only the freshest sample survives

    # ---- metadata ----
    watermark: Optional[QueueWatermark] = None


# ---------------------------------------------------------------------------
# Bounded queue
# ---------------------------------------------------------------------------

class BoundedPubQueue:
    """Thread-safe bounded queue with watermark tracking.

    Parameters
    ----------
    max_size : int
        Maximum number of events the queue can hold before applying
        backpressure / drop policies.

    Attributes
    ----------
    max_size : int
    stats : QueueWatermark  (updated on every get/put)
    """

    __slots__ = (
        "_deque",
        "_lock",
        "_cond",
        "max_size",
        "_total_published",
        "_total_dropped",
        "_last_signal",
        "_on_signal",
    )

    def __init__(self, max_size: int = 4096) -> None:
        if max_size < 1:
            raise ValueError("max_size must be >= 1")
        self._deque: deque[Dict[str, Any]] = deque()
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self.max_size = max_size
        self._total_published: int = 0
        self._total_dropped: int = 0
        self._last_signal: Optional[BackpressureSignal] = None
        self._on_signal: Optional[callable] = None

    # ------------------------------------------------------------------
    # Properties
    # ------------------------------------------------------------------

    @property
    def size(self) -> int:
        with self._lock:
            return len(self._deque)

    @property
    def is_empty(self) -> bool:
        with self._lock:
            return len(self._deque) == 0

    @property
    def fill_ratio(self) -> float:
        with self._lock:
            if self.max_size == 0:
                return 1.0
            return len(self._deque) / self.max_size

    # ------------------------------------------------------------------
    # Put (publisher side)
    # ------------------------------------------------------------------

    def put(self, event: Dict[str, Any], *, timeout: Optional[float] = None) -> bool:
        """Push an event onto the queue.

        Returns True if the event was enqueued, False if it was dropped
        (queue full and drop policy refused insertion).
        """
        with self._lock:
            current = len(self._deque)

            if current >= self.max_size:
                # Emergency: queue is full → drop oldest to make room.
                self._deque.popleft()
                self._total_dropped += 1
                self._deque.append(event)
                self._total_published += 1
                signal = self._snapshot_and_signal()
                self._cond.notify_all()
                if self._on_signal:
                    self._on_signal(signal)
                return True  # accepted after drop

            self._deque.append(event)
            self._total_published += 1
            signal = self._snapshot_and_signal()
            self._cond.notify_all()
            if self._on_signal:
                self._on_signal(signal)
            return True

    # ------------------------------------------------------------------
    # Get (consumer / broadcast side)
    # ------------------------------------------------------------------

    def get(self, timeout: Optional[float] = None) -> Optional[Dict[str, Any]]:
        """Pop the oldest event, blocking up to *timeout* seconds.

        Returns None on timeout.
        """
        with self._cond:
            if not self._deque:
                if timeout is None or timeout <= 0:
                    return None
                if not self._cond.wait(timeout=timeout):
                    return None
                if not self._deque:
                    return None
            event = self._deque.popleft()
            return event

    def get_nowait(self) -> Optional[Dict[str, Any]]:
        """Non-blocking variant of get()."""
        return self.get(timeout=0)

    def drain(self) -> list[Dict[str, Any]]:
        """Atomically drain all pending events (for batch flush)."""
        with self._lock:
            events = list(self._deque)
            self._deque.clear()
            return events

    def drain_up_to(self, max_count: int) -> list[Dict[str, Any]]:
        """Atomically drain up to *max_count* events."""
        with self._lock:
            count = min(max_count, len(self._deque))
            events = [self._deque.popleft() for _ in range(count)]
            return events

    # ------------------------------------------------------------------
    # Watermark & signal
    # ------------------------------------------------------------------

    def watermark(self) -> QueueWatermark:
        """Return the current fill-level snapshot."""
        with self._lock:
            return self._build_watermark()

    def last_signal(self) -> Optional[BackpressureSignal]:
        with self._lock:
            return self._last_signal

    def on_signal(self, callback: Optional[callable]) -> None:
        """Register a callback invoked on every put with the new signal.

        Signature: callback(signal: BackpressureSignal) -> None
        """
        with self._lock:
            self._on_signal = callback

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _zone_for(self, ratio: float) -> str:
        if ratio < 0.5:
            return "normal"
        if ratio < 0.75:
            return "warn"
        if ratio < 0.90:
            return "heavy"
        return "emergency"

    def _build_watermark(self) -> QueueWatermark:
        current = len(self._deque)
        ratio = current / max(self.max_size, 1)
        return QueueWatermark(
            current_size=current,
            max_size=self.max_size,
            fill_ratio=ratio,
            zone=self._zone_for(ratio),
            total_published=self._total_published,
            total_dropped=self._total_dropped,
            timestamp_ms=int(time.time() * 1000),
        )

    def _build_signal(self, wm: QueueWatermark) -> BackpressureSignal:
        """Translate watermark into actionable backpressure advice."""
        zone = wm.zone
        signal = BackpressureSignal(
            active=(zone != "normal"),
            zone=zone,
            watermark=wm,
        )

        if zone == "normal":
            signal.keep_every_n = 1
            signal.max_publish_hz = 0.0
        elif zone == "warn":
            # Light backpressure: keep every 2nd sample, cap at ~30 Hz effective
            signal.keep_every_n = 2
            signal.max_publish_hz = 30.0
        elif zone == "heavy":
            # Aggressive: keep every 4th sample, cap at ~15 Hz
            signal.keep_every_n = 4
            signal.max_publish_hz = 15.0
        else:  # emergency
            signal.keep_every_n = 8
            signal.max_publish_hz = 10.0
            signal.drop_oldest = True
            signal.keep_latest_only = True

        return signal

    def _snapshot_and_signal(self) -> BackpressureSignal:
        wm = self._build_watermark()
        signal = self._build_signal(wm)
        self._last_signal = signal
        return signal

    def __len__(self) -> int:
        return self.size


# ---------------------------------------------------------------------------
# Convenience helpers
# ---------------------------------------------------------------------------

def queue_watermark_log_line(wm: QueueWatermark) -> str:
    """One-line human-readable summary of a watermark snapshot."""
    return (
        f"[queue] {wm.zone:>9s}  size={wm.current_size}/{wm.max_size} "
        f"({wm.fill_ratio:.0%})  pub={wm.total_published}  drop={wm.total_dropped}"
    )


__all__ = [
    "BoundedPubQueue",
    "QueueWatermark",
    "BackpressureSignal",
    "queue_watermark_log_line",
]
