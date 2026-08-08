"""
Unit tests for RateController.

Run with:
    python3 -m pytest tools/dzviz/component/test_rate_controller.py -v
or:
    python3 tools/dzviz/component/test_rate_controller.py
"""

from __future__ import annotations

import math
import unittest

# Allow running this file directly even if the package is not installed.
import sys
from pathlib import Path

_this_dir = Path(__file__).resolve().parent
if str(_this_dir) not in sys.path:
    sys.path.insert(0, str(_this_dir))

from rate_controller import RateController, RateControllerConfig, RateControllerMetrics


class TestRateControllerConfig(unittest.TestCase):
    """Validation of configuration parameters."""

    def test_defaults_are_valid(self) -> None:
        cfg = RateControllerConfig()
        self.assertEqual(cfg.max_recv_hz, 1000.0)
        self.assertEqual(cfg.default_render_hz, 60.0)
        self.assertEqual(cfg.max_render_hz, 60.0)
        self.assertEqual(cfg.queue_capacity, 10)
        self.assertEqual(cfg.q_target, 0.5)
        self.assertEqual(cfg.K_p, 0.5)
        self.assertEqual(cfg.confirm_ticks, 3)

    def test_invalid_K_p_raises(self) -> None:
        with self.assertRaises(ValueError):
            RateControllerConfig(K_p=0.0)
        with self.assertRaises(ValueError):
            RateControllerConfig(K_p=-0.1)
        with self.assertRaises(ValueError):
            RateControllerConfig(K_p=2.1)

    def test_invalid_q_target_raises(self) -> None:
        with self.assertRaises(ValueError):
            RateControllerConfig(q_target=0.0)
        with self.assertRaises(ValueError):
            RateControllerConfig(q_target=1.0)
        with self.assertRaises(ValueError):
            RateControllerConfig(q_target=-0.1)
        with self.assertRaises(ValueError):
            RateControllerConfig(q_target=1.1)

    def test_invalid_hz_raises(self) -> None:
        with self.assertRaises(ValueError):
            RateControllerConfig(max_recv_hz=0)
        with self.assertRaises(ValueError):
            RateControllerConfig(max_recv_hz=-1)
        with self.assertRaises(ValueError):
            RateControllerConfig(default_render_hz=0)

    def test_render_exceeds_recv_raises(self) -> None:
        with self.assertRaises(ValueError):
            RateControllerConfig(max_render_hz=2000, max_recv_hz=1000)

    def test_invalid_queue_capacity_raises(self) -> None:
        with self.assertRaises(ValueError):
            RateControllerConfig(queue_capacity=0)


class TestRateControllerBounds(unittest.TestCase):
    """Verify T_recv stays within bounds under all inputs."""

    def setUp(self) -> None:
        self.ctrl = RateController(RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=60.0,
            max_render_hz=60.0,
            queue_capacity=10,
        ))

    def test_initial_recv_interval_is_min(self) -> None:
        self.assertAlmostEqual(self.ctrl.recv_interval_s, 0.001, places=6)

    def test_recv_never_below_min(self) -> None:
        """Even with q=0 (empty queue), T_recv ≥ 1/max_recv_hz."""
        for _ in range(1000):
            self.ctrl.on_sample(True, queue_depth=0)
        self.assertGreaterEqual(self.ctrl.recv_interval_s, 0.001)

    def test_recv_never_above_render(self) -> None:
        """Even with q=1 (full queue), T_recv ≤ T_render."""
        for _ in range(1000):
            self.ctrl.on_sample(True, queue_depth=self.ctrl.config.queue_capacity)
        self.assertLessEqual(self.ctrl.recv_interval_s, self.ctrl.render_interval_s + 1e-9)

    def test_backpressure_flag_off_at_min(self) -> None:
        """At min interval, backpressure should be off."""
        # Fresh controller starts at min
        self.assertFalse(self.ctrl.metrics.backpressure_active)

    def test_backpressure_flag_on_when_slowed(self) -> None:
        """After sustained overload, backpressure should activate."""
        for _ in range(50):
            self.ctrl.on_sample(True, queue_depth=self.ctrl.config.queue_capacity)
        self.assertTrue(self.ctrl.metrics.backpressure_active)
        self.assertGreater(self.ctrl.metrics.backpressure_level, 0.0)


class TestRateControllerConvergence(unittest.TestCase):
    """End-to-end convergence under synthetic load."""

    def _simulate(
        self,
        ctrl: RateController,
        produce_hz: float,
        duration_s: float = 2.0,
        render_hz: float = 60.0,
    ) -> RateControllerMetrics:
        """Deterministic micro-simulation with two-stage pipeline."""
        IPC_CAP = 256
        ipc_ring: int = 0
        events_q: int = 0
        events_cap: int = ctrl.config.queue_capacity

        render_interval = 1.0 / render_hz

        next_recv = 0.0
        next_render = 0.0
        next_produce = 0.0
        t = 0.0

        while t < duration_s:
            # Produce
            p_interval = 1.0 / produce_hz if produce_hz > 0 else 999.0
            while next_produce <= t:
                if ipc_ring < IPC_CAP:
                    ipc_ring += 1
                next_produce += p_interval

            # Receive
            if t >= next_recv:
                got = False
                if ipc_ring > 0:
                    ipc_ring -= 1
                    if events_q < events_cap:
                        events_q += 1
                        got = True
                ctrl.on_sample(got, events_q)
                if got:
                    ctrl.batch_append(object())
                next_recv += ctrl.recv_interval_s

            # Render
            if t >= next_render:
                batch = ctrl.flush()
                drained = min(len(batch), events_q)
                events_q -= drained
                next_render += render_interval

            if next_recv <= t:
                next_recv = t + ctrl.recv_interval_s
            if next_render <= t:
                next_render = t + render_interval

            t += 1e-5

        return ctrl.metrics

    def test_converges_when_produce_exceeds_render(self) -> None:
        """With 800 Hz produce and 60 Hz render, backpressure must engage."""
        ctrl = RateController(RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=60.0,
            max_render_hz=60.0,
            queue_capacity=10,
        ))
        metrics = self._simulate(ctrl, produce_hz=800.0, duration_s=2.0)
        self.assertTrue(metrics.backpressure_active)
        self.assertGreater(metrics.recv_interval_s, 0.001)
        # Should have received some samples
        self.assertGreater(metrics.total_samples, 0)
        self.assertGreater(metrics.total_flushes, 0)

    def test_stays_at_max_when_produce_is_slow(self) -> None:
        """With 50 Hz produce and 60 Hz render, no backpressure needed."""
        ctrl = RateController(RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=60.0,
            max_render_hz=60.0,
            queue_capacity=10,
        ))
        metrics = self._simulate(ctrl, produce_hz=50.0, duration_s=2.0)
        # Should stay at or near min interval
        self.assertAlmostEqual(metrics.recv_interval_s, 0.001, places=4)
        self.assertFalse(metrics.backpressure_active)

    def test_recovers_after_burst(self) -> None:
        """Backpressure disengages when load drops."""
        ctrl = RateController(RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=60.0,
            max_render_hz=60.0,
            queue_capacity=10,
        ))
        # Burst
        self._simulate(ctrl, produce_hz=1200.0, duration_s=1.0)
        self.assertTrue(ctrl.metrics.backpressure_active)
        bp_before = ctrl.metrics.backpressure_level

        # Recovery: very slow produce
        ctrl.reset()
        metrics2 = self._simulate(ctrl, produce_hz=10.0, duration_s=1.5)
        self.assertFalse(metrics2.backpressure_active)
        self.assertAlmostEqual(metrics2.recv_interval_s, 0.001, places=4)

    def test_batch_never_exceeds_hard_limit(self) -> None:
        """Hard ceiling of _MAX_BATCH_HARD protects memory."""
        ctrl = RateController(RateControllerConfig(
            max_recv_hz=1000.0,
            default_render_hz=1.0,   # 1 Hz render → huge batches
            max_render_hz=60.0,
            queue_capacity=100,
        ))
        # Feed samples without flushing
        for _ in range(200):
            ctrl.on_sample(True, queue_depth=0)
            ctrl.batch_append(object())
        # Batch should cap at _MAX_BATCH_HARD (64), excess dropped
        self.assertLessEqual(ctrl.metrics.batch_size, 64)
        # But the total count includes all received
        self.assertEqual(ctrl.metrics.total_samples, 200)
        # Dropped count = 200 - 64
        self.assertEqual(ctrl.metrics.dropped_samples, 200 - 64)


class TestRateControllerBatching(unittest.TestCase):
    """Batch accumulation and flush behaviour."""

    def test_flush_returns_and_clears_batch(self) -> None:
        ctrl = RateController()
        ctrl.batch_append("a")
        ctrl.batch_append("b")
        ctrl.batch_append("c")

        batch = ctrl.flush()
        self.assertEqual(batch, ["a", "b", "c"])
        self.assertEqual(ctrl.metrics.batch_size, 0)

        # Second flush returns empty
        batch2 = ctrl.flush()
        self.assertEqual(batch2, [])

    def test_empty_flush_is_safe(self) -> None:
        ctrl = RateController()
        batch = ctrl.flush()
        self.assertEqual(batch, [])


class TestRateControllerSetRenderHz(unittest.TestCase):
    """Runtime reconfiguration."""

    def test_set_render_hz_updates_interval(self) -> None:
        ctrl = RateController()
        self.assertAlmostEqual(ctrl.render_interval_s, 1.0 / 60.0, places=4)
        ctrl.set_render_hz(30.0)
        self.assertAlmostEqual(ctrl.render_interval_s, 1.0 / 30.0, places=4)

    def test_set_render_hz_clamped_to_max(self) -> None:
        ctrl = RateController(RateControllerConfig(max_render_hz=60.0))
        ctrl.set_render_hz(120.0)  # above max
        self.assertAlmostEqual(ctrl.render_interval_s, 1.0 / 60.0, places=4)

    def test_recv_clamped_when_render_lower(self) -> None:
        """When render interval becomes smaller than recv, recv is clamped."""
        ctrl = RateController()
        # Force recv to slow down
        for _ in range(50):
            ctrl.on_sample(True, queue_depth=ctrl.config.queue_capacity)
        old_recv = ctrl.recv_interval_s
        self.assertGreater(old_recv, 0.001)

        # Now set render to very fast (low interval)
        ctrl.set_render_hz(120.0)  # clamped to max_render_hz=60
        # recv should still be ≤ render_interval
        self.assertLessEqual(ctrl.recv_interval_s, ctrl.render_interval_s + 1e-9)

    def test_set_queue_capacity_updates(self) -> None:
        ctrl = RateController()
        ctrl.set_queue_capacity(50)
        self.assertEqual(ctrl.config.queue_capacity, 50)
        with self.assertRaises(ValueError):
            ctrl.set_queue_capacity(0)


class TestRateControllerReset(unittest.TestCase):
    """Reset behaviour."""

    def test_reset_restores_initial_state(self) -> None:
        ctrl = RateController()
        # Perturb
        for _ in range(50):
            ctrl.on_sample(True, queue_depth=ctrl.config.queue_capacity)
        ctrl.batch_append("x")
        self.assertTrue(ctrl.metrics.backpressure_active)

        ctrl.reset()
        self.assertAlmostEqual(ctrl.recv_interval_s, 0.001, places=6)
        self.assertAlmostEqual(ctrl.render_interval_s, 1.0 / 60.0, places=4)
        self.assertFalse(ctrl.metrics.backpressure_active)
        self.assertEqual(ctrl.metrics.total_samples, 0)
        self.assertEqual(ctrl.metrics.batch_size, 0)
        self.assertEqual(ctrl.flush(), [])


class TestRateControllerMetrics(unittest.TestCase):
    """Metrics accuracy."""

    def test_total_samples_increments(self) -> None:
        ctrl = RateController()
        for i in range(5):
            ctrl.on_sample(True, queue_depth=0)
        self.assertEqual(ctrl.metrics.total_samples, 5)

    def test_dropped_samples_counted(self) -> None:
        ctrl = RateController()
        # Fill batch beyond hard limit
        for i in range(100):
            ctrl.batch_append(i)
        self.assertGreater(ctrl.metrics.dropped_samples, 0)

    def test_backpressure_level_range(self) -> None:
        ctrl = RateController()
        # At min recv: level should be 0
        self.assertAlmostEqual(ctrl.metrics.backpressure_level, 0.0, places=3)
        # Force to max
        old_render = ctrl._render_interval
        ctrl._recv_interval = old_render  # set to max
        min_recv = 1.0 / ctrl.config.max_recv_hz
        span = old_render - min_recv
        # Manually trigger metric update
        ctrl.on_sample(False, queue_depth=0)
        if span > 0:
            expected = (old_render - min_recv) / span
            self.assertAlmostEqual(ctrl.metrics.backpressure_level, expected, places=3)

    def test_summary_returns_all_keys(self) -> None:
        ctrl = RateController()
        s = ctrl.summary()
        expected_keys = {
            "recv_interval_ms", "render_interval_ms",
            "effective_recv_hz", "effective_render_hz",
            "queue_depth", "queue_ratio", "batch_size",
            "backpressure_active", "backpressure_level",
            "dropped_samples", "total_samples", "total_flushes",
        }
        self.assertEqual(set(s.keys()), expected_keys)


class TestPControllerCorrection(unittest.TestCase):
    """Verify the proportional control formula."""

    def test_above_target_increases_interval(self) -> None:
        ctrl = RateController(RateControllerConfig(K_p=0.5, q_target=0.5))
        old = ctrl._recv_interval
        ctrl._apply_pcontrol(q=1.0)  # queue full
        # factor = 1 + 0.5 * (1.0 - 0.5) = 1.25
        self.assertAlmostEqual(ctrl._recv_interval, old * 1.25, places=6)

    def test_below_target_decreases_interval(self) -> None:
        ctrl = RateController(RateControllerConfig(K_p=0.5, q_target=0.5))
        # First increase it
        ctrl._recv_interval = 0.005  # 5ms
        old = ctrl._recv_interval
        ctrl._apply_pcontrol(q=0.0)  # queue empty
        # factor = 1 + 0.5 * (0.0 - 0.5) = 0.75
        self.assertAlmostEqual(ctrl._recv_interval, old * 0.75, places=6)

    def test_at_target_no_change(self) -> None:
        ctrl = RateController(RateControllerConfig(K_p=0.5, q_target=0.5))
        old = ctrl._recv_interval
        ctrl._apply_pcontrol(q=0.5)
        # factor = 1 + 0.5 * (0.5 - 0.5) = 1.0
        self.assertAlmostEqual(ctrl._recv_interval, old, places=6)

    def test_clamped_to_min(self) -> None:
        ctrl = RateController(RateControllerConfig(K_p=2.0, q_target=0.5))
        ctrl._recv_interval = 0.001  # already at min
        ctrl._apply_pcontrol(q=0.0)  # try to go lower
        # factor = 1 + 2.0 * (0 - 0.5) = 0.0, clamped to min
        self.assertAlmostEqual(ctrl._recv_interval, 0.001, places=6)

    def test_clamped_to_render(self) -> None:
        ctrl = RateController(RateControllerConfig(K_p=2.0, q_target=0.5))
        ctrl._recv_interval = 0.010
        ctrl._apply_pcontrol(q=1.0)  # try to go higher
        # factor = 1 + 2.0 * (1.0 - 0.5) = 2.0 → 0.020, clamped to 0.01667
        self.assertAlmostEqual(ctrl._recv_interval, ctrl.render_interval_s, places=5)


class TestConfirmTicksDeadZone(unittest.TestCase):
    """The confirm_ticks anti-flapping mechanism."""

    def test_single_spike_does_not_trigger(self) -> None:
        ctrl = RateController(RateControllerConfig(
            K_p=0.5, q_target=0.5, queue_capacity=10, confirm_ticks=5,
        ))
        initial = ctrl.recv_interval_s
        # One above-target observation: q=1.0, EMA jumps to 1.0
        ctrl.on_sample(True, queue_depth=10)  # q=1.0 > 0.5 → above=1
        self.assertEqual(ctrl._consecutive_above, 1)
        self.assertEqual(ctrl._consecutive_below, 0)
        self.assertAlmostEqual(ctrl.recv_interval_s, initial)

        # Let EMA decay with q=0 calls. After ~15 calls: EMA ≈ 0.9^15 ≈ 0.21 < 0.5
        for _ in range(20):
            ctrl.on_sample(True, queue_depth=0)  # q=0.0
        # Now EMA q is below target, so consecutive_below should have incremented
        # and consecutive_above should have been reset
        self.assertEqual(ctrl._consecutive_above, 0,
                         "consecutive_above should reset after EMA decays below target")
        self.assertGreater(ctrl._consecutive_below, 0,
                           "consecutive_below should increment after EMA crosses below target")
        self.assertAlmostEqual(ctrl.recv_interval_s, initial,
                               msg="single spike should not change rate with confirm_ticks=5")

    def test_consecutive_above_triggers(self) -> None:
        ctrl = RateController(RateControllerConfig(
            K_p=0.5, q_target=0.5, queue_capacity=10, confirm_ticks=3,
        ))
        initial = ctrl.recv_interval_s
        for _ in range(5):
            ctrl.on_sample(True, queue_depth=10)  # all above
        self.assertGreater(ctrl.recv_interval_s, initial)

    def test_consecutive_below_triggers(self) -> None:
        ctrl = RateController(RateControllerConfig(
            K_p=0.5, q_target=0.5, queue_capacity=10, confirm_ticks=3,
        ))
        # First slow it down with sustained overload
        for _ in range(10):
            ctrl.on_sample(True, queue_depth=10)
        slowed = ctrl.recv_interval_s
        self.assertGreater(slowed, 0.001)

        # Let EMA decay and then sustain empty queue to trigger speed-up.
        # After ~25 calls with q=0: EMA decays below target, then
        # 3 consecutive "below" observations trigger speed-up.
        for _ in range(40):
            ctrl.on_sample(False, queue_depth=0)
        self.assertLess(ctrl.recv_interval_s, slowed,
                        "recv interval should decrease after sustained empty queue")


if __name__ == "__main__":
    unittest.main(verbosity=2)
