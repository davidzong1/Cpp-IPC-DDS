import copy
import unittest
from summarize_performance import content_errors
from summarize_local_stages import split


class PerformanceEvidenceTest(unittest.TestCase):
    def test_publish_return_can_follow_receive(self):
        publish = {'start_ns': 100, 'elapsed_ns': 100}
        self.assertEqual(split(publish, {'read_ns': 150, 'elapsed_ns': 50}), (50, 50, 0))
        self.assertEqual(split(publish, {'read_ns': 300, 'elapsed_ns': 200}), (200, 100, 100))
        with self.assertRaises(ValueError):
            split(publish, {'read_ns': 300, 'elapsed_ns': 199})

    def test_latency_samples_do_not_hide_failed_delivery(self):
        good = {'seconds': 30, 'rate': 100, 'returncode': 0, 'subscribers': 1,
                'publish': {'accepted': 3000, 'rejected': 0},
                'receivers': [{'count': 3000, 'lost': 0, 'invalid': 0, 'duplicates': 0}]}
        samples = [10000] * 3000
        self.assertEqual(content_errors(good, samples, 1), [])
        for key in ('lost', 'invalid', 'duplicates'):
            bad = copy.deepcopy(good)
            bad['receivers'][0][key] = 1
            self.assertTrue(content_errors(bad, samples, 1))
        bad = copy.deepcopy(good)
        bad['returncode'] = 1
        self.assertTrue(content_errors(bad, samples, 1))
        self.assertTrue(content_errors(good, samples[:-1], 1))
        self.assertTrue(content_errors(good, samples, 0))


if __name__ == '__main__':
    unittest.main()
