"""
Rate Controller for dzviz dynamic sampling, batching, and backpressure.

Design goals:
- Receive up to 1000 Hz from IPC channel (min poll interval 1 ms)
- Render at configurable rate (default 60 fps, max 60 fps)
- Batch multiple received samples into a single WebSocket publish frame
- Apply gradual backpressure when the internal event queue exceeds the
  target occupancy, and recover when it drops below
- Hysteresis (confirm_ticks) prevents oscillation around the target

Algorithm overview
-------------------
The controller tracks four key variables:

  T_recv   – current poll interval (seconds).  Starts at 1/max_recv_hz.
  T_render – current render interval (seconds). Fixed at 1/render_hz.
  batch    – accumulated samples since the last render flush.
  q        – queue depth ratio = queue_depth / queue_capacity.

Every poll cycle:
  1. Try to receive a sample (non-blocking).
  2. If successful, push into *batch*.
  3. Compute q = queue_depth / queue_capacity.
  4. Adjust T_recv using a proportional controller (see below).

Every render cycle (T_render):
  1. If batch is non-empty, emit the entire batch as one frame.
  2. Clear batch.

Proportional control with dead-zone
------------------------------------
Instead of hard high/low watermarks with multiplicative step changes,
we use a proportional (P) controller that scales the adjustment by how
far q is from the target:

    error     = q - q_target                           # ∈ [-q_target, 1-q_target]
    T_recv   *= 1.0 + K_p * error                      # proportional correction
    T_recv    = clamp(T_recv, 1/max_recv_hz, T_render) # hard bounds

To prevent flapping from single-sample noise, a rate change is only
committed after *confirm_ticks* consecutive observations on the same
side of q_target (all above or all below).  This is a simple Schmitt-
trigger hysteresis that costs no extra state.

Why proportional control?
-------------------------
- Multiplicative step (α_up / α_down) creates fixed-size jumps that
  can overshoot the equilibrium.
- A P-controller with small K_p (0.5) applies smaller corrections as
  q approaches q_target, converging smoothly.
- The confirm_ticks dead-zone prevents reacting to transient spikes.

When q ≈ q_target, error ≈ 0, and T_recv stays put → stable equilibrium.

Equilibrium
-----------
At steady state:

    recv_hz ≈ render_hz × queue_capacity × (1 - q_target)
            ≈ 60 × 10 × 0.5 = 300 Hz   (with defaults)

This leaves headroom: the events queue stays half-full on average,
absorbing bursts without dropping samples.  The IPC ring buffer (256
slots) absorbs the rest before the C++ layer drops.

Bounds
------
  T_recv   ∈ [1 / max_recv_hz,   T_render]
           ∈ [1 ms,               16.67 ms]   at defaults
  T_render ∈ [1 / max_render_hz, +∞)          (fixed in practice)

Complexity
----------
  - Per-sample: O(1) amortised (queue push, simple arithmetic).
  - Per-render: O(batch_size) for serialisation.
  - Memory:     O(batch_size) worst-case.  At equilibrium (~5 samples
                per frame with defaults) this is negligible.

Thread safety
-------------
The controller is NOT internally synchronised.  The caller must ensure
that only one thread at a time calls `on_sample()` and that `flush()`
is called from the render loop (which may be the same or a different
thread, as long as the batch is replaced atomically).
"""

from __future__ import annotations

import dataclasses
import math
import time
from typing import Any, Dict, List, Optional


@dataclasses.dataclass
class RateControllerConfig:
    """Tunable parameters for the rate controller.

    Attributes:
        max_recv_hz:       Upper bound on receive frequency (Hz).  Default 1000.
        default_render_hz: Default rendering frequency (Hz).       Default 60.
        max_render_hz:     Upper bound on rendering frequency (Hz). Default 60.
        queue_capacity:    Total capacity of the event queue (slots).
        q_target:          Target queue ratio (aim point).         Default 0.5.
        K_p:               Proportional gain for rate adjustment.  Default 0.5.
        confirm_ticks:     Consecutive observations before a rate
                           change is committed (anti-flapping).
    """

    max_recv_hz: float = 1000.0
    default_render_hz: float = 60.0
    max_render_hz: float = 60.0
    queue_capacity: int = 10
    q_target: float = 0.50
    K_p: float = 0.50
    confirm_ticks: int = 3

    def __post_init__(self) -> None:
        if not (0.0 < self.K_p <= 2.0):
            raise ValueError("K_p must be in (0, 2]")
        if not (0.0 < self.q_target < 1.0):
            raise ValueError("require 0 < q_target < 1")
        if self.max_recv_hz <= 0:
            raise ValueError("max_recv_hz must be > 0")
        if self.default_render_hz <= 0:
            raise ValueError("default_render_hz must be > 0")
        if self.max_render_hz <= 0:
            raise ValueError("max_render_hz must be > 0")
        if self.queue_capacity <= 0:
            raise ValueError("queue_capacity must be > 0")
        if self.max_render_hz > self.max_recv_hz:
            raise ValueError("max_render_hz must not exceed max_recv_hz")


@dataclasses.dataclass
class RateControllerMetrics:
    """Live metrics exported by the rate controller.

    These are updated by the controller on each call; the caller may
    read them at any time for diagnostics or logging.
    """

    recv_interval_s: float = 0.001       # current T_recv
    render_interval_s: float = 1.0 / 60.0  # current T_render
    effective_recv_hz: float = 0.0       # measured receive rate (EMA)
    effective_render_hz: float = 0.0     # measured render rate (EMA)
    queue_depth: int = 0                 # last observed queue depth
    queue_ratio: float = 0.0             # q = queue_depth / capacity
    batch_size: int = 0                  # current batch length
    backpressure_active: bool = False    # True when T_recv > min bound
    backpressure_level: float = 0.0      # 0.0 (none) … 1.0 (max)
    dropped_samples: int = 0             # samples dropped due to full batch
    total_samples: int = 0               # total samples received
    total_flushes: int = 0               # total render flushes


class RateController:
    """Dynamic receive-rate controller with batch accumulation.

    Usage pattern (pseudocode)::

        ctrl = RateController(config)

        # --- receive thread / loop ---
        while running:
            sample = subscriber.try_get(timeout=ctrl.recv_interval_s)
            ctrl.on_sample(sample is not None, current_queue_depth)
            if sample is not None:
                ctrl.batch_append(sample)

        # --- render loop (may be same or different thread) ---
        while running:
            time.sleep(ctrl.render_interval_s)
            batch = ctrl.flush()
            if batch:
                ws.send(serialise_batch(batch))
    """

    # EMA smoothing factor for rate measurements.
    _EMA_ALPHA: float = 0.1

    # Maximum batch size to prevent unbounded memory growth when
    # receive rate far exceeds render rate and backpressure hasn't
    # kicked in yet.
    _MAX_BATCH_HARD: int = 64

    def __init__(self, config: Optional[RateControllerConfig] = None) -> None:
        self._cfg = config or RateControllerConfig()
        self._cfg.__post_init__()

        min_recv = 1.0 / self._cfg.max_recv_hz
        render = 1.0 / self._cfg.default_render_hz

        self._recv_interval: float = min_recv
        self._render_interval: float = render

        self._batch: List[Any] = []
        self._consecutive_above: int = 0
        self._consecutive_below: int = 0

        # Smoothed queue ratio (EMA) to filter render-cycle oscillation
        self._ema_q: float = 0.0

        # Rate measurement (EMA)
        self._ema_recv_hz: float = 0.0
        self._ema_render_hz: float = 0.0
        self._last_recv_ts: float = 0.0
        self._last_flush_ts: float = 0.0

        self.metrics = RateControllerMetrics(
            recv_interval_s=self._recv_interval,
            render_interval_s=self._render_interval,
        )

    # ------------------------------------------------------------------
    # Public read-only helpers
    # ------------------------------------------------------------------

    @property
    def recv_interval_s(self) -> float:
        """Current poll interval in seconds (1 / current_recv_hz)."""
        return self._recv_interval

    @property
    def render_interval_s(self) -> float:
        """Current render interval in seconds (1 / current_render_hz)."""
        return self._render_interval

    @property
    def config(self) -> RateControllerConfig:
        return self._cfg

    # ------------------------------------------------------------------
    # Rate control logic
    # ------------------------------------------------------------------

    def on_sample(self, received: bool, queue_depth: int) -> None:
        """Notify the controller of one poll cycle result.

        Must be called once per poll attempt, regardless of whether a
        sample was actually received.

        Args:
            received:     True if a sample was dequeued this cycle.
            queue_depth:  Current number of items in the event queue
                          (the queue between subscriber and broadcaster).
        """
        now = time.monotonic()

        # --- update rate EMA ---
        if self._last_recv_ts > 0.0:
            instant_hz = 1.0 / max(now - self._last_recv_ts, 1e-9)
            if self._ema_recv_hz == 0.0:
                self._ema_recv_hz = instant_hz
            else:
                self._ema_recv_hz = (
                    self._EMA_ALPHA * instant_hz
                    + (1.0 - self._EMA_ALPHA) * self._ema_recv_hz
                )
        self._last_recv_ts = now

        if received:
            self.metrics.total_samples += 1

        # --- compute q ---
        cap = self._cfg.queue_capacity
        q = queue_depth / cap if cap > 0 else 1.0
        self.metrics.queue_depth = queue_depth
        self.metrics.queue_ratio = q

        # --- proportional control with confirm_ticks dead-zone ---
        # Use EMA-smoothed q to filter out render-cycle oscillation.
        # The EMA time constant is ~1 render frame, so the controller
        # sees the average queue pressure rather than instantaneous
        # fill/drain transitions.
        if self._ema_q == 0.0:
            self._ema_q = q
        else:
            self._ema_q = self._EMA_ALPHA * q + (1.0 - self._EMA_ALPHA) * self._ema_q

        q_smooth = self._ema_q

        if q_smooth > self._cfg.q_target:
            self._consecutive_above += 1
            self._consecutive_below = 0
            if self._consecutive_above >= self._cfg.confirm_ticks:
                self._apply_pcontrol(q_smooth)
                self._consecutive_above = 0
        elif q_smooth < self._cfg.q_target:
            self._consecutive_below += 1
            self._consecutive_above = 0
            if self._consecutive_below >= self._cfg.confirm_ticks:
                self._apply_pcontrol(q_smooth)
                self._consecutive_below = 0
        else:
            # q_smooth == q_target (exact): reset both counters, no change needed
            self._consecutive_above = 0
            self._consecutive_below = 0

        # --- update metric signals ---
        min_recv = 1.0 / self._cfg.max_recv_hz
        self.metrics.recv_interval_s = self._recv_interval
        span = self._render_interval - min_recv
        if span > 0:
            self.metrics.backpressure_level = min(
                1.0, max(0.0, (self._recv_interval - min_recv) / span)
            )
        self.metrics.backpressure_active = self._recv_interval > min_recv * 1.01

    def batch_append(self, sample: Any) -> None:
        """Append a received sample to the pending batch.

        If the batch is already at the hard ceiling the oldest sample
        is dropped to make room (bounded-memory guarantee).
        """
        if len(self._batch) >= self._MAX_BATCH_HARD:
            self._batch.pop(0)
            self.metrics.dropped_samples += 1
        self._batch.append(sample)
        self.metrics.batch_size = len(self._batch)

    def flush(self) -> List[Any]:
        """Atomically extract and return the current batch.

        Returns an empty list when there is nothing to render.
        The caller owns the returned list.
        """
        now = time.monotonic()

        # --- update render rate EMA ---
        if self._last_flush_ts > 0.0:
            instant_hz = 1.0 / max(now - self._last_flush_ts, 1e-9)
            if self._ema_render_hz == 0.0:
                self._ema_render_hz = instant_hz
            else:
                self._ema_render_hz = (
                    self._EMA_ALPHA * instant_hz
                    + (1.0 - self._EMA_ALPHA) * self._ema_render_hz
                )
        self._last_flush_ts = now

        batch = self._batch
        self._batch = []
        flushed_count = len(batch)
        self.metrics.batch_size = 0
        self.metrics.total_flushes += 1
        self.metrics.effective_recv_hz = self._ema_recv_hz
        self.metrics.effective_render_hz = self._ema_render_hz
        # Store last flushed batch size for observability
        self.metrics.queue_ratio = self._ema_q
        return batch

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _apply_pcontrol(self, q: float) -> None:
        """Apply one proportional-control correction.

        T_recv ← T_recv × (1 + K_p × (q - q_target))
        Then clamp to [1/max_recv_hz, T_render].
        """
        error = q - self._cfg.q_target
        factor = 1.0 + self._cfg.K_p * error
        new_interval = self._recv_interval * factor
        min_interval = 1.0 / self._cfg.max_recv_hz
        self._recv_interval = max(min_interval, min(new_interval, self._render_interval))

    # ------------------------------------------------------------------
    # Configuration helpers
    # ------------------------------------------------------------------

    def set_render_hz(self, hz: float) -> None:
        """Reconfigure the render rate at runtime.

        Args:
            hz: New render frequency in Hz.  Clamped to (0, max_render_hz].
        """
        hz = max(1e-6, min(hz, self._cfg.max_render_hz))
        self._render_interval = 1.0 / hz
        self.metrics.render_interval_s = self._render_interval
        # If recv interval is now > render interval, clamp it.
        if self._recv_interval > self._render_interval:
            self._recv_interval = self._render_interval

    def set_queue_capacity(self, capacity: int) -> None:
        """Update queue capacity at runtime (e.g. after re-subscribe)."""
        if capacity <= 0:
            raise ValueError("queue_capacity must be > 0")
        self._cfg.queue_capacity = capacity

    def reset(self) -> None:
        """Reset all internal state to initial values."""
        self._recv_interval = 1.0 / self._cfg.max_recv_hz
        self._render_interval = 1.0 / self._cfg.default_render_hz
        self._batch.clear()
        self._consecutive_above = 0
        self._consecutive_below = 0
        self._ema_q = 0.0
        self._ema_recv_hz = 0.0
        self._ema_render_hz = 0.0
        self._last_recv_ts = 0.0
        self._last_flush_ts = 0.0
        self.metrics = RateControllerMetrics(
            recv_interval_s=self._recv_interval,
            render_interval_s=self._render_interval,
        )

    # ------------------------------------------------------------------
    # Introspection
    # ------------------------------------------------------------------

    def summary(self) -> Dict[str, Any]:
        """Return a human-readable summary of current state."""
        return {
            "recv_interval_ms": round(self._recv_interval * 1000, 3),
            "render_interval_ms": round(self._render_interval * 1000, 3),
            "effective_recv_hz": round(self.metrics.effective_recv_hz, 1),
            "effective_render_hz": round(self.metrics.effective_render_hz, 1),
            "queue_depth": self.metrics.queue_depth,
            "queue_ratio": round(self.metrics.queue_ratio, 3),
            "batch_size": self.metrics.batch_size,
            "backpressure_active": self.metrics.backpressure_active,
            "backpressure_level": round(self.metrics.backpressure_level, 3),
            "dropped_samples": self.metrics.dropped_samples,
            "total_samples": self.metrics.total_samples,
            "total_flushes": self.metrics.total_flushes,
        }


# ------------------------------------------------------------------
# Standalone simulation runner (for design validation)
# ------------------------------------------------------------------

def _simulate() -> None:
    """Deterministic two-stage pipeline simulation.

    Stage 1: Producer → IPC ring buffer (256 slots) → Sniffer (at T_recv)
    Stage 2: Sniffer → events queue (capacity=N) → Render (at T_render)

    This models the real data path: the C++ IPC channel has a 256-slot
    ring; the Python events queue is between the subscriber thread and
    the broadcast_loop.
    """
    cfg = RateControllerConfig(
        max_recv_hz=1000.0,
        default_render_hz=60.0,
        max_render_hz=60.0,
        queue_capacity=10,
        q_target=0.5,
        K_p=0.5,
        confirm_ticks=3,
    )
    ctrl = RateController(cfg)

    # Two-stage queues
    IPC_CAP = 256
    ipc_ring: int = 0          # IPC channel fill level
    events_queue: int = 0      # hub.events fill level
    events_cap: int = cfg.queue_capacity

    recv_period = ctrl.recv_interval_s
    render_period = ctrl.render_interval_s

    total_time = 6.0
    next_recv = 0.0
    next_render = 0.0
    t = 0.0

    # Producer rate schedule
    def producer_hz(now: float) -> float:
        if 1.5 <= now < 2.5:
            return 1200.0   # burst overload
        elif 3.5 <= now < 4.5:
            return 200.0    # low load
        return 800.0        # baseline

    rows: List[str] = []
    header = (f"{'time':>6s} {'IPC':>6s} {'evQ':>6s} "
              f"{'T_recv':>8s} {'batch':>5s} {'bp':>5s} {'note'}")
    rows.append(header)

    last_log = -1.0
    producer_next = 0.0
    ipc_dropped = 0

    while t < total_time:
        # --- produce into IPC ring ---
        phz = producer_hz(t)
        p_interval = 1.0 / phz if phz > 0 else 999.0
        while producer_next <= t + recv_period * 0.5:
            if ipc_ring < IPC_CAP:
                ipc_ring += 1
            else:
                ipc_dropped += 1
            producer_next += p_interval

        # --- receive cycle: sniffer reads from IPC, pushes to events ---
        if t >= next_recv:
            got = False
            if ipc_ring > 0:
                ipc_ring -= 1
                # Push to events queue (with backpressure limit)
                if events_queue < events_cap:
                    events_queue += 1
                    got = True
                # else: events queue full, sample dropped
            ctrl.on_sample(got, events_queue)
            if got:
                ctrl.batch_append(f"s{ctrl.metrics.total_samples}")
            next_recv += ctrl.recv_interval_s

        # --- render cycle: drain events queue ---
        if t >= next_render:
            batch = ctrl.flush()
            # Each batch consumes batch_size items from events queue
            drained = min(len(batch), events_queue)
            events_queue -= drained
            next_render += ctrl.render_interval_s

        # --- log ---
        if t - last_log >= 0.2:
            note = ""
            if ctrl.metrics.backpressure_active:
                note = "◀ BP"
            if ipc_dropped > 0:
                note += f" [IPC drop:{ipc_dropped}]"
            rows.append(
                f"{t:5.2f}s IPC:{ipc_ring:>3d} evQ:{events_queue:>2d}/{events_cap} "
                f"{ctrl.recv_interval_s*1000:>6.2f}ms "
                f"{ctrl.metrics.batch_size:>4d} "
                f"{'ON' if ctrl.metrics.backpressure_active else 'off':>5s} "
                f"{note}"
            )
            last_log = t

        t += 0.00005  # fine-grained step

    print("=== Rate Controller Two-Stage Simulation ===\n")
    for row in rows:
        print(row)
    print(f"\n--- Final metrics ---")
    for k, v in ctrl.summary().items():
        print(f"  {k}: {v}")
    print(f"  ipc_dropped_total: {ipc_dropped}")


if __name__ == "__main__":
    _simulate()
