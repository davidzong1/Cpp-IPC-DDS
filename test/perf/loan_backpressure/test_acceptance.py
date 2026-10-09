"""防止验收程序将计数错误、路径回退或超预算误判为通过。"""
import copy
import json
import pathlib
import unittest

import run_acceptance as acceptance


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.budget = json.loads(pathlib.Path(__file__).with_name('acceptance_budget.json').read_text())
        self.config = acceptance.configuration('raw', 1, 1, 11000, 'copy', 100, self.budget)
        row = {key: str(self.config[key]) for key in ('transport', 'pubs', 'subs', 'payload',
                'consume_mode', 'hold_us', 'slow_start_us', 'metrics')}
        row.update(window='1', baseline='0', subscriber='0', attempted='100', published='100',
                   pool_exhausted='0', publish_failed='0', received='100', publish_n='100',
                   recv_n='100', destroy_n='100', sample_hold_n='100', app_work_n='100',
                   copied_bytes='1100000', transport_latency_n='100', elapsed_ns='1000000',
                   drain_elapsed_ns='1100000', cpu_total_ns='2000000', rss_peak_kb='10000',
                   throughput_msgs_per_s='100000', drain_throughput_msgs_per_s='90909', publish_p99_ns='1000')
        row.update({key: '0' for key in acceptance.ERRORS})
        pool = dict(capacity='10', publisher_cap='9', final_free='10', consistent='1', used_max='9',
                    loan_attempt='100', loan_success='100', loan_reject='0', waiters='0',
                    duplicate_return='0', invalid_storage_id='0', pool_chain_corrupt='0')
        self.run = dict(exit_code=0, messages=[row], pools=[pool])

    def test_complete_window(self):
        self.assertEqual(acceptance.validate_run(self.run, self.config), [])

    def test_accepted_delivery_and_credit_rejection_are_separate(self):
        self.config['require_all'] = 0
        row = self.run['messages'][0]
        row.update(published='90', pool_exhausted='10', publish_failed='10', received='90',
                   recv_n='90', destroy_n='90', sample_hold_n='90', app_work_n='90',
                   copied_bytes='990000', transport_latency_n='90')
        self.run['pools'][0].update(loan_success='90', loan_reject='10')
        self.assertEqual(acceptance.validate_run(self.run, self.config), [])
        row['received'] = '89'
        self.assertTrue(acceptance.validate_run(self.run, self.config))

    def test_wrong_copy_size_and_fake_queue_residence_fail(self):
        self.run['messages'][0]['copied_bytes'] = '0'
        self.assertTrue(acceptance.validate_run(self.run, self.config))
        self.run['messages'][0]['copied_bytes'] = '1100000'
        self.run['messages'][0]['queue_residence_n'] = '100'
        self.assertTrue(acceptance.validate_run(self.run, self.config))

    def test_pool_corruption_and_missing_subscriber_fail(self):
        for key in ('pool_chain_corrupt', 'duplicate_return', 'invalid_storage_id'):
            run = copy.deepcopy(self.run)
            run['pools'][0][key] = '1'
            self.assertTrue(acceptance.validate_run(run, self.config))
        self.config['subs'] = 8
        self.assertTrue(acceptance.validate_run(self.run, self.config))

    def test_tlv_fallback_makes_performance_inconclusive(self):
        self.config['transport'] = 'dzflat'
        row = self.run['messages'][0]
        row.update(transport='dzflat', baseline='1', flat='99', tlv='1')
        self.run['pools'] = []
        self.run['reliable'] = not acceptance.validate_run(self.run, self.config, current=False)
        verdict = acceptance.compare_performance([self.run] * 3, [self.run] * 3, self.budget)
        self.assertEqual(verdict['status'], 'inconclusive')

    def test_thresholds_fail_without_relaxing_budget(self):
        self.run['reliable'] = True
        after = copy.deepcopy(self.run)
        after['messages'][0].update(throughput_msgs_per_s='89999', publish_p99_ns='1201',
                                    cpu_total_ns='2500001', rss_peak_kb='524289')
        verdict = acceptance.compare_performance([self.run] * 3, [after] * 3, self.budget)
        self.assertEqual(verdict['status'], 'over_budget')
        for key in ('throughput', 'publish_p99', 'cpu_per_message', 'rss_hard_limit'):
            self.assertFalse(verdict['checks'][key])

    def test_interleaved_output_is_retained_as_inconclusive(self):
        messages, pools, failures = acceptance.parse_measurements(
            'RESULT transport=dzflat RecvWorker[1]: idle exit\npublish_n=100\n')
        self.assertEqual(messages, [])
        self.assertEqual(pools, [])
        self.assertTrue(failures)

    def test_missing_windows_and_invalid_latency_fail(self):
        self.run['reliable'] = True
        verdict = acceptance.compare_performance([self.run], [self.run], self.budget)
        self.assertEqual(verdict['status'], 'inconclusive')
        for value in ('0', 'nan', 'inf'):
            self.run['messages'][0]['publish_p99_ns'] = value
            self.assertTrue(acceptance.validate_run(self.run, self.config))

    def test_invalid_publish_entry_and_output_are_rejected(self):
        self.config['dzflat_publish'] = 'loaned'
        self.assertTrue(acceptance.validate_run(self.run, self.config))
        self.run['messages'][0]['dzflat_publish'] = 'loaned'
        self.assertEqual(acceptance.validate_run(self.run, self.config), [])
        self.run['parse_failures'] = ['测量记录无效']
        self.assertTrue(acceptance.validate_run(self.run, self.config))

    def test_summary_separates_versions_and_does_not_multiply_attempts(self):
        baseline = copy.deepcopy(self.run)
        baseline.update(name='baseline_w1', reliable=False)
        baseline['messages'][0]['missing'] = '1'
        current = copy.deepcopy(self.run)
        current.update(name='current_w1', reliable=True)
        current['messages'].append(copy.deepcopy(current['messages'][0]))
        summary = acceptance.build_summary('performance', [baseline, current], [{'status': 'inconclusive'}])
        self.assertEqual(summary['published'], 200)
        self.assertEqual(summary['versions']['baseline']['delivery_errors']['missing'], 1)
        self.assertEqual(summary['versions']['current']['delivery_errors']['missing'], 0)


if __name__ == '__main__':
    unittest.main()
