import unittest
from summarize_receive import decode, summarize


def row(elapsed=100, received=1020, enqueue=1030, dequeue=1080, begin=1010):
    return dict(sequence=1, read_ns=1000 + elapsed, elapsed_ns=elapsed, bytes=144,
                recv_begin_ns=begin, recv_return_ns=received,
                enqueue_before_ns=enqueue, dequeue_after_ns=dequeue)


class ReceiveSummaryTest(unittest.TestCase):
    def test_missing_and_reordered_stages_fail(self):
        for bad in (row(received=0), row(enqueue=0), row(dequeue=1025)):
            with self.assertRaises(ValueError):
                decode(bad, complete=True)
        # 基线只拥有recv返回点，其余零字段明确属于未采样。
        self.assertEqual(len(decode(row(begin=0, enqueue=0, dequeue=0), False)['stages']), 2)

    def test_recv_can_start_before_publish(self):
        result = decode(row(begin=900), complete=True)
        self.assertTrue(result['recv_overlaps_publish'])
        self.assertEqual(sum(result['stages'].values()), 100)

    def test_tail_uses_same_slowest_messages(self):
        # 常规消息的排队占比较高，最慢消息的延迟发生在recv返回之前。
        rows = [row() for _ in range(99)] + [row(elapsed=10000, received=10900, enqueue=10910, dequeue=10980)]
        result = summarize(rows, True)
        tail = result['slowest_one_percent']
        self.assertEqual(tail['count'], 1)
        self.assertAlmostEqual(tail['stage_fraction']['publish_to_recv_return'], .99)
        self.assertAlmostEqual(sum(tail['stage_fraction'].values()), 1)
        self.assertAlmostEqual(sum(tail['stage_mean_us'].values()), tail['mean_elapsed_us'])


if __name__ == '__main__':
    unittest.main()
