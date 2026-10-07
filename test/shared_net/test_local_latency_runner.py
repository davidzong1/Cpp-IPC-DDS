import unittest

from run_local_latency_experiments import (activity_delta, aggregate_process_activity,
                                           activity_under_roots, observed_competitors,
                                           system_busy_delta)


class LocalLatencyRunnerTest(unittest.TestCase):
    def test_activity_ignores_new_and_reused_pids(self):
        before = {"processes": {
            "10": {"comm": "stable", "utime_ticks": 10, "stime_ticks": 10, "start_ticks": 100},
            "11": {"comm": "reused", "utime_ticks": 10, "stime_ticks": 0, "start_ticks": 100},
        }}
        after = {"processes": {
            "10": {"comm": "stable", "utime_ticks": 115, "stime_ticks": 10, "start_ticks": 100},
            "11": {"comm": "new-instance", "utime_ticks": 900, "stime_ticks": 0, "start_ticks": 200},
            "12": {"comm": "new-process", "utime_ticks": 1000, "stime_ticks": 0, "start_ticks": 100},
        }}

        self.assertEqual(activity_delta(before, after, 100), [
            {"pid": 12, "comm": "new-process", "start_ticks": 100, "cpu_seconds": 10.0},
            {"pid": 11, "comm": "new-instance", "start_ticks": 200, "cpu_seconds": 9.0},
            {"pid": 10, "comm": "stable", "start_ticks": 100, "cpu_seconds": 1.05},
        ])

    def test_activity_is_accumulated_across_samples_by_process_instance(self):
        samples = [
            {"process_cpu_delta": [
                {"pid": 10, "comm": "worker", "start_ticks": 100, "cpu_seconds": 0.6},
            ]},
            {"process_cpu_delta": [
                {"pid": 10, "comm": "worker", "start_ticks": 100, "cpu_seconds": 0.5},
                {"pid": 10, "comm": "replacement", "start_ticks": 200, "cpu_seconds": 0.7},
            ]},
        ]

        self.assertEqual(aggregate_process_activity(samples), [
            {"pid": 10, "comm": "worker", "start_ticks": 100, "cpu_seconds": 1.1,
             "cwd": None},
            {"pid": 10, "comm": "replacement", "start_ticks": 200, "cpu_seconds": 0.7},
        ])

    def test_competitor_threshold_and_runner_exclusion(self):
        activity = [
            {"pid": 10, "cpu_seconds": 1.0},
            {"pid": 11, "cpu_seconds": 0.99},
            {"pid": 12, "cpu_seconds": 2.0},
        ]

        self.assertEqual(observed_competitors(activity, current_pid=12), [activity[0]])

    def test_worktree_processes_are_not_external_competitors(self):
        activity = [
            {"pid": 10, "cpu_seconds": 1.2, "cwd": "/tmp/test-worktree/bin"},
            {"pid": 11, "cpu_seconds": 1.2, "cwd": "/tmp/external-job"},
        ]

        self.assertEqual(observed_competitors(activity, current_pid=99,
                                               worktree_root="/tmp/test-worktree"), [activity[1]])

    def test_declared_ambient_processes_are_recorded_but_not_competitors(self):
        activity = [
            {"pid": 10, "cpu_seconds": 3.0, "cwd": "/home/zwc/MPC_GPU/job"},
            {"pid": 11, "cpu_seconds": 2.0, "cwd": "/tmp/external-job"},
        ]

        self.assertEqual(activity_under_roots(activity, ["/home/zwc/MPC_GPU"]),
                         [activity[0]])
        self.assertEqual(observed_competitors(
            activity, current_pid=99, ambient_roots=["/home/zwc/MPC_GPU"]),
            [activity[1]])

    def test_system_busy_ticks_excludes_idle(self):
        before = {"cpu": {"total_ticks": 1000, "idle_ticks": 700}}
        after = {"cpu": {"total_ticks": 1900, "idle_ticks": 1300}}

        self.assertEqual(system_busy_delta(before, after), 300)

    def test_monitor_delta_uses_process_snapshot_shape(self):
        before = {"processes": {
            "10": {"comm": "worker", "utime_ticks": 10,
                   "stime_ticks": 0, "start_ticks": 100},
        }}
        after = {"processes": {
            "10": {"comm": "worker", "utime_ticks": 25,
                   "stime_ticks": 0, "start_ticks": 100},
        }}
        self.assertEqual(activity_delta(before, after, 100), [
            {"pid": 10, "comm": "worker", "start_ticks": 100,
             "cpu_seconds": 0.15},
        ])


if __name__ == "__main__":
    unittest.main()
