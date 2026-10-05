import unittest

from summarize_copy_cpu import decode_cpu, summarize
from test_publish_summary import row


def cpu_row():
    return dict(row(), sequence=1, copy_outer_begin_ns=125, copy_outer_end_ns=145,
                copy_cpu_begin_ns=1000, copy_cpu_end_ns=1012, copy_begin_cpu=0, copy_end_cpu=0)


class CopyCpuSummaryTest(unittest.TestCase):
    def test_bracket_and_signed_difference(self):
        r = decode_cpu(cpu_row())
        self.assertEqual((r['wall_ns'], r['cpu_ns'], r['gap_ns'], r['probe_bracket_ns']), (10, 12, -2, 10))

    def test_missing_or_inconsistent_clock_is_rejected(self):
        for key, value in [('copy_outer_begin_ns', 131), ('copy_outer_end_ns', 151),
                           ('copy_cpu_begin_ns', 0), ('copy_cpu_end_ns', 999), ('copy_end_cpu', -1)]:
            r = cpu_row()
            r[key] = value
            with self.assertRaises(ValueError):
                decode_cpu(r)

    def test_cpu_identity_and_same_slow_records(self):
        rows = [cpu_row() for _ in range(9)]
        slow = cpu_row()
        for key in ('copy_end_ns', 'commit_begin_ns', 'notify_begin_ns', 'notify_end_ns', 'commit_end_ns', 'copy_outer_end_ns', 'elapsed_ns'):
            slow[key] += 100
        slow['copy_cpu_end_ns'] += 90
        slow['copy_end_cpu'] = 16
        result = summarize(rows + [slow])
        self.assertEqual(result['different_endpoint_cpu'], 1)
        tail = result['slowest_copy_ten_percent']
        self.assertEqual(tail['count'], 1)
        self.assertAlmostEqual(tail['cpu_over_wall_sum'], 102 / 110)
        self.assertEqual(len(result['by_endpoint_cpus']), 2)


if __name__ == '__main__':
    unittest.main()
