import csv
import gzip
import json
from pathlib import Path
import tempfile
import unittest
from summarize_perf_scheduler import analyze


class SchedulerEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        result = {'seconds': 1, 'rate': 2, 'subscribers': 1, 'affinity_plan': None,
                  'before': [{'thread_status': [{'tid': 12}]}, {'thread_status': [{'tid': 77}]}]}
        (self.folder/'result.json').write_text(json.dumps(result))
        self.write_csv('pub', [dict(sequence=seq, start_ns=start, elapsed_ns=1500, success=1, bytes=64,
                 notify_begin_ns=start+1000, copy_begin_ns=start+100, copy_end_ns=start+700,
                 copy_cpu_begin_ns=100, copy_cpu_end_ns=700, copy_begin_cpu=0, copy_end_cpu=0)
                 for seq, start in ((200, 1000), (201, 10000))])
        self.write_csv('sub0', [dict(sequence=seq, reader_tid=12, read_ns=start+6000, elapsed_ns=6000,
                  assisted=1, wait_begin_ns=start-500, wait_end_ns=start+5500)
                  for seq, start in ((200, 1000), (201, 10000))])
        self.events = [
            '0 [004] 0.000000300: power:cpu_idle: state=3 cpu_id=4',
            '77 [000] 0.000002100: sched:sched_waking: comm=shared_net_benc pid=12 prio=120 target_cpu=002',
            '0 [004] 0.000004500: power:cpu_idle: state=4294967295 cpu_id=4',
            '0 [004] 0.000005000: sched:sched_wakeup: comm=shared_net_benc pid=12 prio=120 target_cpu=004',
            '0 [004] 0.000005500: sched:sched_switch: prev_comm=swapper prev_pid=0 prev_state=R ==> next_comm=shared_net_benc next_pid=12 next_prio=120',
            '-1 [004] 0.000020000: sched:sched_switch: prev_comm=shared_net_benc prev_pid=12 prev_state=X ==> next_comm=swapper next_pid=0 next_prio=120']

    def write_csv(self, name, rows):
        with gzip.open(self.folder/(name+'.csv.gz'), 'wt', newline='') as f:
            writer = csv.DictWriter(f, list(rows[0])); writer.writeheader(); writer.writerows(rows)

    def write_events(self):
        with gzip.open(self.folder/'events.txt.gz', 'wt') as f:
            f.write('\n'.join(self.events)+'\n')

    def test_migration_uses_activation_cpu_and_does_not_zero_fill_missing_chain(self):
        self.write_events()
        result = analyze(self.folder)
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['unmatched'], {'no_waking_in_interval': 1})
        self.assertEqual(result['unknown_event_count'], 0)
        self.assertEqual(result['publisher_wake_count'], 1)
        stages = result['all_matched']
        self.assertEqual(stages['notify_to_waking_ns']['mean_us'], 0.1)
        self.assertEqual(stages['waking_to_wakeup_ns']['mean_us'], 2.9)
        self.assertEqual(stages['wakeup_to_scheduled_ns']['mean_us'], 0.5)
        self.assertEqual(stages['scheduled_to_wait_end_ns']['mean_us'], 1)
        self.assertEqual(stages['notify_to_wait_end_ns']['mean_us'], 4.5)
        self.assertEqual(stages['waking_to_idle_exit_ns']['mean_us'], 2.4)
        self.assertEqual(result['by_idle_at_waking']['3']['elapsed_ns']['count'], 1)

    def test_lost_event_is_preserved_as_invalid_evidence(self):
        self.events.append('LOST 7 events')
        self.write_events()
        result = analyze(self.folder)
        self.assertEqual(result['lost_event_lines'], ['LOST 7 events'])

    def test_background_receiver_is_joined_instead_of_getter(self):
        with gzip.open(self.folder/'sub0.csv.gz', 'rt') as f:
            rows = list(csv.DictReader(f))
        for row in rows:
            row.update(receiver_tid=34, receiver_cpu=4, generation=1,
                       receiver_wait_begin_ns=int(row['wait_begin_ns']),
                       receiver_wait_end_ns=int(row['wait_end_ns']), recv_return_ns=int(row['read_ns'])-200)
        self.write_csv('sub0', rows)
        self.events = [line.replace('pid=12', 'pid=34') for line in self.events]
        self.write_events()
        result = analyze(self.folder)
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['receive_roles'], {'background': 2})
        self.assertEqual(result['receiver_tids'], [34])
        self.assertEqual(result['all_matched']['wait_end_to_recv_return_ns']['mean_us'], .3)

    def test_perf_off_is_distinguished_from_missing_wakeup(self):
        result = analyze(self.folder)
        self.assertEqual(result['matched'], 0)
        self.assertEqual(result['unmatched'], {'perf_disabled': 2})

    def test_duplicate_receive_sequence_fails_integrity_gate(self):
        with gzip.open(self.folder/'sub0.csv.gz', 'rt') as f:
            rows = list(csv.DictReader(f))
        rows[1]['sequence'] = rows[0]['sequence']
        self.write_csv('sub0', rows)
        with self.assertRaises(AssertionError): analyze(self.folder)


if __name__ == '__main__': unittest.main()
