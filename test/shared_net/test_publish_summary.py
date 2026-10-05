import unittest
from summarize_publish import decode, summarize, STAMPS


def row():
    return dict(start_ns=100, elapsed_ns=90, success='1', **dict(zip(STAMPS, range(110, 190, 10))))


class PublishSummaryTest(unittest.TestCase):
    def test_missing_or_unordered_is_rejected(self):
        for key, value in [('loan_end_ns', 0), ('copy_end_ns', 200), ('notify_begin_ns', 110)]:
            bad = row()
            bad[key] = value
            with self.assertRaises(ValueError):
                decode(bad)

    def test_same_call_conservation_and_tail(self):
        self.assertEqual(sum(decode(row()).values()), 90)
        slow = row()
        slow['elapsed_ns'] = 990
        for key in STAMPS[3:]:
            slow[key] += 900
        result = summarize([row() for _ in range(99)] + [slow])
        self.assertEqual(result['slowest_one_percent']['count'], 1)
        self.assertAlmostEqual(result['slowest_one_percent']['stage_fraction']['copy'], 910/990)
        self.assertAlmostEqual(sum(result['slowest_one_percent']['stage_fraction'].values()), 1)


if __name__ == '__main__':
    unittest.main()
