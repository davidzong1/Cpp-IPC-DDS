#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
from topic_udp_matrix import nearest_rank, overlap, summary, validate_plan


class TopicUdpMatrixTest(unittest.TestCase):
    def test_nearest_rank_and_mean_are_distinct(self):
        self.assertEqual(nearest_rank([30, 10, 20], 50), 20)
        self.assertEqual(summary([30, 10, 20])["mean_ns"], 20.0)
        self.assertEqual(summary([30, 10, 20])["p99_ns"], 30)

    def test_overlap_requires_real_interval_intersection(self):
        self.assertEqual(overlap((10, 20), (15, 30)), 5)
        self.assertEqual(overlap((10, 20), (20, 30)), 0)

    def test_plan_rejects_v1_dedicated(self):
        path = Path(__file__).parent.parent.parent / "docs/shared_network_endpoint_evidence/20261006-topic-udp/plans/local-network.json"
        plan = json.loads(path.read_text())
        plan["routes"][0]["endpoint"] = "dedicated"
        self.assertTrue(any("pooled 模式" in error for error in validate_plan(plan)))

    def test_plan_is_valid(self):
        path = Path(__file__).parent.parent.parent / "docs/shared_network_endpoint_evidence/20261006-topic-udp/plans/local-network.json"
        self.assertEqual(validate_plan(json.loads(path.read_text())), [])


if __name__ == "__main__":
    unittest.main()
