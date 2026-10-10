"""
Backpressure controller with multi-zone downsampling.

Reads queue watermark snapshots from BoundedPubQueue and emits
downsampling directives that the publisher / subscriber layer can
apply to match outbound bandwidth to inbound data rate.

Strategy per zone
-----------------
normal  (< 50%)   no downsampling, publish all samples
warn    (50-75%)  keep every Nth sample (2, 3, …)
heavy   (75-90%)  keep every Nth sample (4-8), cap rate
emergency (>90%)  keep latest only, drop oldest aggressively

The controller also tracks per-topic statistics so that topics with
different rates can be downsampled independently.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set

from .bounded_queue import BackpressureSignal, QueueWatermark


# ---------------------------------------------------------------------------
# Per-topic tracking
# ---------------------------------------------------------------------------

@dataclass
class TopicRateState:
    """Rate tracking for a single topic."""

    topic: str
    sample_count: int = 0
    last_sample_time: float = 0.0
    input_hz: float = 0.0
    output_hz: float = 0.0
    dropped: int = 0
    downsample_factor: int = 1  # current keep-every-N
    sample_times: List[float] = field(default_factory=list)
    # Large enough to hold 1s of data at max_input_hz (1000) + margin.
    # At 1000 Hz, 40 samples covers only 40 ms — the rate window
    # calculation can never detect rates above ~40 Hz, so the input cap
    # never fires.  2000 covers 2 s at 1000 Hz with room to spare.
    _max_window: int = 2000


# ---------------------------------------------------------------------------
# Downsample policy
# ---------------------------------------------------------------------------

@dataclass
class DownsamplePolicy:
    """Configuration for a downsampling zone."""

    zone: str
    keep_every_n: int = 1
    max_publish_hz: float = 0.0
    drop_oldest: bool = False
    keep_latest_only: bool = False


# Default zone policies
DEFAULT_ZONE_POLICIES: Dict[str, DownsamplePolicy] = {
    "normal": DownsamplePolicy(zone="normal", keep_every_n=1, max_publish_hz=0.0),
    "warn": DownsamplePolicy(zone="warn", keep_every_n=2, max_publish_hz=30.0),
    "heavy": DownsamplePolicy(zone="heavy", keep_every_n=4, max_publish_hz=15.0),
    "emergency": DownsamplePolicy(
        zone="emergency",
        keep_every_n=8,
        max_publish_hz=10.0,
        drop_oldest=True,
        keep_latest_only=True,
    ),
}


# ---------------------------------------------------------------------------
# Controller
# ---------------------------------------------------------------------------

class BackpressureController:
    """Decides when and how to downsample based on queue watermarks.

    Parameters
    ----------
    policies : dict[str, DownsamplePolicy]
        Per-zone downsampling rules.
    max_input_hz : float
        Absolute upper bound on input sample rate (default 1000 Hz).
        Samples arriving faster than this are always dropped.
    max_output_hz : float
        Absolute upper bound on output publish rate (default 60 Hz).
        This is the rendering limit — we never publish faster than this.
    rate_window_s : float
        Sliding window for input/output rate calculation.
    """

    def __init__(
        self,
        policies: Optional[Dict[str, DownsamplePolicy]] = None,
        max_input_hz: float = 1000.0,
        max_output_hz: float = 60.0,
        rate_window_s: float = 1.0,
    ) -> None:
        self._policies = policies or dict(DEFAULT_ZONE_POLICIES)
        self._max_input_hz = max_input_hz
        self._max_output_hz = max_output_hz
        self._rate_window_s = rate_window_s

        # Per-topic state
        self._topics: Dict[str, TopicRateState] = {}

        # Counters
        self._total_in: int = 0
        self._total_out: int = 0
        self._total_dropped: int = 0

        # Callbacks
        self._on_drop: Optional[Callable[[str, str, str], None]] = None  # (topic, reason, detail)

    # ------------------------------------------------------------------
    # Configuration
    # ------------------------------------------------------------------

    @property
    def max_input_hz(self) -> float:
        return self._max_input_hz

    @max_input_hz.setter
    def max_input_hz(self, value: float) -> None:
        self._max_input_hz = max(1.0, value)

    @property
    def max_output_hz(self) -> float:
        return self._max_output_hz

    @max_output_hz.setter
    def max_output_hz(self, value: float) -> None:
        self._max_output_hz = max(1.0, value)

    def set_zone_policy(self, zone: str, policy: DownsamplePolicy) -> None:
        self._policies[zone] = policy

    def on_drop(self, callback: Optional[Callable[[str, str, str], None]]) -> None:
        """Register a callback for drop events: callback(topic, reason, detail)."""
        self._on_drop = callback

    # ------------------------------------------------------------------
    # Topic registration
    # ------------------------------------------------------------------

    def register_topic(self, topic: str) -> None:
        if topic not in self._topics:
            self._topics[topic] = TopicRateState(topic=topic)

    def unregister_topic(self, topic: str) -> None:
        self._topics.pop(topic, None)

    # ------------------------------------------------------------------
    # Should-publish decision (hot path)
    # ------------------------------------------------------------------

    def should_publish(
        self,
        topic: str,
        signal: Optional[BackpressureSignal] = None,
    ) -> bool:
        """Return True if this sample should be published.

        This is the primary backpressure decision point.  It consumes
        ``signal`` from BoundedPubQueue to determine the current zone
        and applies per-topic counters to implement keep-every-N.

        Also enforces max_input_hz and max_output_hz independently.
        """
        self._total_in += 1
        now = time.monotonic()

        # Ensure per-topic state exists
        state = self._topics.get(topic)
        if state is None:
            state = TopicRateState(topic=topic)
            self._topics[topic] = state

        state.sample_count += 1
        state.sample_times.append(now)
        if len(state.sample_times) > state._max_window:
            state.sample_times = state.sample_times[-state._max_window:]

        # Compute input rate
        self._update_topic_rate(state, now)

        # --- Absolute input rate cap (window-based EMA) ---
        # NOTE: hard inter-sample interval enforcement belongs in the caller's
        # poll loop (e.g. LiveSniffSource monotonic-deadline scheduling),
        # NOT here.  The BackpressureController layer is responsible for
        # publish/downsample decisions, not raw input-rate gating.
        if self._max_input_hz > 0 and state.input_hz > self._max_input_hz:
            # Input rate exceeds max → drop sample
            self._total_dropped += 1
            state.dropped += 1
            self._emit_drop(topic, "input_rate_cap", f"{state.input_hz:.0f} > {self._max_input_hz:.0f}")
            return False

        # --- Zone-based downsampling ---
        if signal is not None and signal.active:
            keep_n = max(signal.keep_every_n, 1)
            if keep_n > 1:
                if (state.sample_count % keep_n) != 0:
                    self._total_dropped += 1
                    state.dropped += 1
                    return False

            # --- Output rate cap ---
            effective_cap = signal.max_publish_hz or self._max_output_hz
            if effective_cap > 0 and state.output_hz > effective_cap:
                self._total_dropped += 1
                state.dropped += 1
                return False

            # Emergency: keep_latest_only — let through; caller deduplicates
            if signal.keep_latest_only:
                pass  # through, caller is responsible for dedup

        self._total_out += 1
        state.last_sample_time = now
        return True

    def should_publish_batch(
        self,
        topic: str,
        signal: Optional[BackpressureSignal] = None,
    ) -> int:
        """Return the maximum number of samples to keep from the pending batch.

        Returns:
            0 = drop everything
            N = keep N samples (evenly sampled)
            -1 = keep all
        """
        if signal is None or not signal.active:
            return -1  # keep all

        policy = self._policies.get(signal.zone)
        if policy is None:
            return -1

        if policy.keep_latest_only:
            return 1  # keep only the latest
        if policy.keep_every_n > 1:
            # Return factor for batch sampling
            return -1  # caller will use keep_every_n to filter
        return -1

    # ------------------------------------------------------------------
    # Stats
    # ------------------------------------------------------------------

    def topic_stats(self, topic: str) -> Optional[TopicRateState]:
        return self._topics.get(topic)

    def aggregate_stats(self) -> Dict[str, Any]:
        """Return overall statistics."""
        topics_out = {
            t: {
                "input_hz": s.input_hz,
                "output_hz": s.output_hz,
                "dropped": s.dropped,
                "downsample_factor": s.downsample_factor,
            }
            for t, s in self._topics.items()
        }
        return {
            "total_in": self._total_in,
            "total_out": self._total_out,
            "total_dropped": self._total_dropped,
            "drop_ratio": (
                self._total_dropped / max(self._total_in, 1)
            ),
            "topics": topics_out,
        }

    # ------------------------------------------------------------------
    # Reset
    # ------------------------------------------------------------------

    def reset_counters(self) -> None:
        self._total_in = 0
        self._total_out = 0
        self._total_dropped = 0
        for state in self._topics.values():
            state.sample_count = 0
            state.dropped = 0

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _update_topic_rate(self, state: TopicRateState, now: float) -> None:
        times = state.sample_times
        if len(times) < 2:
            return
        window_start = now - self._rate_window_s
        # Count samples within the window
        recent = sum(1 for t in times if t >= window_start)
        state.input_hz = recent / self._rate_window_s if self._rate_window_s > 0 else 0.0
        # Output rate is a trailing estimate
        if state.last_sample_time > 0:
            state.output_hz = 1.0 / max(now - state.last_sample_time, 0.001)

    def _emit_drop(self, topic: str, reason: str, detail: str) -> None:
        if self._on_drop:
            try:
                self._on_drop(topic, reason, detail)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def downsample_batch(
    events: List[Dict[str, Any]],
    keep_every_n: int,
    keep_latest_only: bool = False,
) -> List[Dict[str, Any]]:
    """Apply downsampling to a list of events.

    Parameters
    ----------
    events : list
        Input events (assumed chronologically ordered).
    keep_every_n : int
        Keep 1 out of every N events (1 = keep all).
    keep_latest_only : bool
        If True, return only the last event.

    Returns
    -------
    list
        Downsampled events.
    """
    if not events:
        return []

    if keep_latest_only:
        return [events[-1]]

    if keep_every_n <= 1:
        return list(events)

    result = events[::keep_every_n]
    # Always include the last event to avoid stale displays
    if len(result) > 1 and result[-1] is not events[-1]:
        result.append(events[-1])
    return result


__all__ = [
    "BackpressureController",
    "DownsamplePolicy",
    "TopicRateState",
    "DEFAULT_ZONE_POLICIES",
    "downsample_batch",
]
