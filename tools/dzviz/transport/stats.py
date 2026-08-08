"""
Transport-layer statistics collector.

Tracks bandwidth, queue depth, client counts, and event rates over
time windows.  Designed to be cheap enough to call on every publish
event — all counters are atomic/simple integers, snapshots are
constructed on demand.

Reports can be logged periodically or exposed via a management
endpoint.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from .bounded_queue import QueueWatermark


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

@dataclass
class RateSample:
    """One data-point for rate calculation."""

    timestamp_s: float
    value: float = 0.0


@dataclass
class TransportSnapshot:
    """Point-in-time transport statistics."""

    timestamp_ms: int = 0

    # Events
    samples_in: int = 0
    samples_out: int = 0
    samples_dropped: int = 0

    # Bytes
    bytes_sent: int = 0
    bytes_buffered: int = 0

    # Queue
    queue_size: int = 0
    queue_max: int = 0
    queue_fill_ratio: float = 0.0
    queue_zone: str = "normal"

    # Clients
    client_count: int = 0
    slow_client_count: int = 0

    # Rates
    input_hz: float = 0.0
    output_hz: float = 0.0

    # Per-topic
    topics: Dict[str, Dict[str, Any]] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Stats collector
# ---------------------------------------------------------------------------

class TransportStats:
    """Collect and aggregate transport-level statistics.

    Counters are incremented inline on the hot path; rate calculations
    and snapshots are computed on demand.

    Parameters
    ----------
    rate_window_s : float
        Sliding window (seconds) for Hz calculations.
    max_rate_samples : int
        Maximum number of rate samples retained.
    """

    def __init__(
        self,
        rate_window_s: float = 2.0,
        max_rate_samples: int = 256,
    ) -> None:
        self._rate_window_s = rate_window_s
        self._max_rate_samples = max_rate_samples

        # Counters (monotonic)
        self.samples_in: int = 0
        self.samples_out: int = 0
        self.samples_dropped: int = 0
        self.bytes_sent: int = 0
        self.bytes_buffered: int = 0

        # Rate samples (for Hz calculation)
        self._in_rates: deque[RateSample] = deque()
        self._out_rates: deque[RateSample] = deque()

        # Last watermark (set externally)
        self._last_watermark: Optional[QueueWatermark] = None

        # Client counts (set externally)
        self._client_count: int = 0
        self._slow_client_count: int = 0

        # Per-topic
        self._topic_counters: Dict[str, Dict[str, int]] = {}

        # Timing
        self._start_time: float = time.monotonic()

    # ------------------------------------------------------------------
    # Hot-path incrementors
    # ------------------------------------------------------------------

    def record_sample_in(self, topic: str = "") -> None:
        self.samples_in += 1
        self._in_rates.append(RateSample(time.monotonic(), 1.0))
        self._trim_rates()
        if topic:
            tc = self._topic_counters.setdefault(topic, {"in": 0, "out": 0, "drop": 0})
            tc["in"] += 1

    def record_sample_out(self, topic: str = "", byte_count: int = 0) -> None:
        self.samples_out += 1
        self.bytes_sent += byte_count
        self._out_rates.append(RateSample(time.monotonic(), 1.0))
        self._trim_rates()
        if topic:
            tc = self._topic_counters.setdefault(topic, {"in": 0, "out": 0, "drop": 0})
            tc["out"] += 1

    def record_drop(self, topic: str = "", count: int = 1) -> None:
        self.samples_dropped += count
        if topic:
            tc = self._topic_counters.setdefault(topic, {"in": 0, "out": 0, "drop": 0})
            tc["drop"] += count

    def record_bytes(self, count: int) -> None:
        self.bytes_buffered += count

    def update_watermark(self, wm: QueueWatermark) -> None:
        self._last_watermark = wm

    def update_clients(self, count: int, slow_count: int) -> None:
        self._client_count = count
        self._slow_client_count = slow_count

    # ------------------------------------------------------------------
    # Computed
    # ------------------------------------------------------------------

    @property
    def input_hz(self) -> float:
        return self._compute_hz(self._in_rates)

    @property
    def output_hz(self) -> float:
        return self._compute_hz(self._out_rates)

    @property
    def drop_ratio(self) -> float:
        return self.samples_dropped / max(self.samples_in, 1)

    @property
    def uptime_s(self) -> float:
        return time.monotonic() - self._start_time

    # ------------------------------------------------------------------
    # Snapshot
    # ------------------------------------------------------------------

    def snapshot(self) -> TransportSnapshot:
        """Return a point-in-time statistics snapshot."""
        wm = self._last_watermark
        return TransportSnapshot(
            timestamp_ms=int(time.time() * 1000),
            samples_in=self.samples_in,
            samples_out=self.samples_out,
            samples_dropped=self.samples_dropped,
            bytes_sent=self.bytes_sent,
            bytes_buffered=self.bytes_buffered,
            queue_size=wm.current_size if wm else 0,
            queue_max=wm.max_size if wm else 0,
            queue_fill_ratio=wm.fill_ratio if wm else 0.0,
            queue_zone=wm.zone if wm else "normal",
            client_count=self._client_count,
            slow_client_count=self._slow_client_count,
            input_hz=self.input_hz,
            output_hz=self.output_hz,
            topics={
                t: dict(c)
                for t, c in self._topic_counters.items()
            },
        )

    def log_line(self) -> str:
        """Single-line summary suitable for periodic logging."""
        s = self.snapshot()
        return (
            f"[transport] "
            f"in={s.samples_in}({s.input_hz:.0f}hz) "
            f"out={s.samples_out}({s.output_hz:.0f}hz) "
            f"drop={s.samples_dropped}({s.samples_dropped/max(s.samples_in,1):.1%}) "
            f"q={s.queue_size}/{s.queue_max}({s.queue_fill_ratio:.0%}:{s.queue_zone}) "
            f"clients={s.client_count}(slow={s.slow_client_count}) "
            f"bytes={s.bytes_sent}"
        )

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _trim_rates(self) -> None:
        while len(self._in_rates) > self._max_rate_samples:
            self._in_rates.popleft()
        while len(self._out_rates) > self._max_rate_samples:
            self._out_rates.popleft()

    def _compute_hz(self, samples: deque[RateSample]) -> float:
        """Compute rate (Hz) from samples in the sliding window."""
        if not samples:
            return 0.0
        cutoff = time.monotonic() - self._rate_window_s
        # Count samples within window
        count = 0
        for s in reversed(samples):
            if s.timestamp_s < cutoff:
                break
            count += 1
        return count / self._rate_window_s if self._rate_window_s > 0 else 0.0


__all__ = [
    "TransportStats",
    "TransportSnapshot",
    "RateSample",
]
