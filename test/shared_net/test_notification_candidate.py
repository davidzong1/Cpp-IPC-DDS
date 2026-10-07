import csv
import gzip
import json
from pathlib import Path
import tempfile
import unittest

from summarize_notification_candidate import ORDER, SCENARIOS, summarize


class NotificationCandidateTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.windows = []
        for subscribers, size in SCENARIOS:
            for position, label in enumerate(ORDER, 1):
                name = f'sub{subscribers}-bytes{size}-{position}-{label}'
                folder = self.root / name
                folder.mkdir()
                self.windows.append({'case': name, 'artifact': label, 'returncode': 0})
                (folder / 'result.json').write_text(json.dumps({
                    'subscribers': subscribers, 'bytes': size, 'wire_bytes': size + 80,
                    'seconds': 1, 'rate': 2}))
                pubs = [{'sequence': sequence, 'start_ns': sequence * 100000, 'elapsed_ns': 1000, 'success': 1}
                        for sequence in (200, 201)]
                self.write_csv(folder / 'pub.csv.gz', pubs)
                for subscriber in range(subscribers):
                    receives = []
                    for index, pub in enumerate(pubs):
                        elapsed = (1000 + index * 1000 if subscriber == 0 else 3000 + index * 100)
                        elapsed += 100 if label == 'C' else 0
                        receives.append({'sequence': pub['sequence'], 'elapsed_ns': elapsed,
                                         'read_ns': pub['start_ns'] + elapsed})
                    self.write_csv(folder / f'sub{subscriber}.csv.gz', receives)
        self.save_manifest()

    @staticmethod
    def write_csv(path, rows):
        with gzip.open(path, 'wt', newline='') as stream:
            writer = csv.DictWriter(stream, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def save_manifest(self):
        (self.root / 'manifest.json').write_text(json.dumps({
            'suite': 'candidate', 'instrumentation': 'l0', 'cases': list(range(24)), 'windows': self.windows}))

    def test_all_subscribers_are_pooled_and_clustered_by_publish_sequence(self):
        result = summarize(self.root)
        window = result['windows'][8]
        self.assertEqual(window['metrics']['e2e']['count'], 64)
        self.assertEqual(window['metrics']['e2e']['p50_us'], 3)
        self.assertEqual(window['metrics']['max_sequence_e2e']['count'], 2)
        self.assertEqual(window['metrics']['max_sequence_e2e']['p99_us'], 3.1)
        self.assertEqual(len(window['subscriber_stats']), 32)
        self.assertEqual(len(result['scenarios'][1]['pairs']), 4)
        self.assertFalse(result['formal_acceptance'])

    def test_missing_or_reordered_complete_windows_are_rejected(self):
        self.windows.pop()
        self.save_manifest()
        with self.assertRaises(ValueError):
            summarize(self.root)
        self.windows.append(self.windows[0])
        self.save_manifest()
        with self.assertRaises(ValueError):
            summarize(self.root)

    def test_duplicate_sequence_is_rejected(self):
        folder = self.root / self.windows[8]['case']
        path = folder / 'sub31.csv.gz'
        with gzip.open(path, 'rt') as stream:
            rows = list(csv.DictReader(stream))
        rows[1]['sequence'] = rows[0]['sequence']
        self.write_csv(path, rows)
        with self.assertRaises(ValueError):
            summarize(self.root)


if __name__ == '__main__':
    unittest.main()
