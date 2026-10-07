import unittest
from summarize_perf_stacks import family, parse_stacks


class KernelStackEvidenceTest(unittest.TestCase):
    def test_only_exact_reader_and_event_time_are_linked(self):
        key = ('sched:sched_wakeup', 12, 2000)
        data = ['0 [004] 0.000002000: sched:sched_wakeup: comm=shared_net_benc pid=12 target_cpu=004\n',
                '\tffffffffaf10 ttwu_do_activate\n', '\tffffffffaf20 sched_ttwu_pending\n',
                '0 [004] 0.000003000: sched:sched_wakeup: comm=shared_net_benc pid=13 target_cpu=004\n',
                '\tffffffffaf30 try_to_wake_up\n', 'LOST 7 events\n']
        stacks, unknown, lost = parse_stacks(data, {key})
        self.assertEqual(stacks, {key: ['ttwu_do_activate', 'sched_ttwu_pending']})
        self.assertEqual(unknown, [])
        self.assertEqual(lost, ['LOST 7 events'])
        self.assertEqual(family(stacks[key], key[0]), 'remote_pending')

    def test_absent_stack_is_not_classified_as_direct(self):
        self.assertEqual(family([], 'sched:sched_wakeup'), 'missing_stack')
        self.assertEqual(family(['try_to_wake_up'], 'sched:sched_wakeup'), 'direct_activation')
        self.assertEqual(family(['wake_up_q', 'futex_wake'], 'sched:sched_waking'), 'futex_wake')


if __name__ == '__main__': unittest.main()
