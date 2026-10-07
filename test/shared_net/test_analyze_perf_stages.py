import unittest
from analyze_perf_stages import cohorts, partition


class StagePartitionTest(unittest.TestCase):
    def test_same_message_partition_conserves_latency(self):
        pub = {'sequence': 1, 'start_ns': 100, 'notify_begin_ns': 150}
        sub = {'sequence': 1, 'elapsed_ns': 300, 'wait_end_ns': 250, 'recv_begin_ns': 270,
               'recv_return_ns': 280, 'enqueue_before_ns': 310, 'enqueue_after_ns': 320,
               'dequeue_after_ns': 370, 'read_ns': 400}
        stages, reason = partition(pub, sub)
        self.assertIsNone(reason)
        self.assertEqual(sum(stages.values()), 300)
        self.assertEqual(stages['notify_to_wait_end_ns'], 100)
        sub['wait_end_ns'] = 0
        self.assertEqual(partition(pub, sub), (None, 'missing_timestamp'))
        sub['wait_end_ns'] = 140
        self.assertEqual(partition(pub, sub), (None, 'nonmonotonic_or_no_wait'))

    def test_cohorts_share_identical_message_membership(self):
        rows = [dict(elapsed_ns=i, sequence=i, reader_tid=2) for i in range(100)]
        grouped = cohorts(rows[::-1])
        self.assertEqual([r['sequence'] for r in grouped['median_band_p45_p55']], list(range(45, 55)))
        self.assertEqual([r['sequence'] for r in grouped['slowest_one_percent']], [99])


if __name__ == '__main__': unittest.main()
