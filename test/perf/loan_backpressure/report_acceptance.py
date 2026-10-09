#!/usr/bin/env python3
"""复核最终原始记录和预算，生成验收报告；保留首轮与确认轮各自判定。"""
import argparse
import collections
import csv
import hashlib
import itertools
import json
import pathlib
import re

import run_acceptance as acceptance


def load(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def key(config):
    entry = config.get('dzflat_publish', 'object') if config['transport'] == 'dzflat' else 'explicit_loan'
    return tuple(config[k] for k in ('transport', 'pubs', 'subs', 'payload', 'consume_mode')) + (entry,)


def audit_directory(directory, kind, only=None):
    runs, verdicts = load(directory / 'runs.json'), load(directory / 'verdicts.json')
    budget = load(directory / 'budget_snapshot.json')
    identity = load(directory / 'run_identity.json')
    assert 'VmHWM' in identity['rss_source'], f'{directory}: 不能使用继承的 RSS 口径'
    budget_files = directory.parent.glob('*budget.json')
    assert any(digest(path) == identity['budget_sha256'] and load(path) == budget
               for path in budget_files), f'{directory}: 冻结预算身份不匹配'
    assert len({run['name'] for run in runs}) == len(runs), f'{directory}: 运行记录重名'
    if kind == 'consumer':
        expected = {(group, key(config)) for group, config in acceptance.consumer_configs(budget)}
        assert {(v['group'], key(v['config'])) for v in verdicts} == expected
    else:
        p = budget['performance']
        expected = {
            key(acceptance.configuration(transport, p['publishers'], subs, int(payload), mode,
                                         p['messages_by_payload'][payload], budget, metrics=0,
                                         dzflat_publish=p.get('dzflat_publish', 'object')))
            for transport, subs, payload, mode in itertools.product(
                p['transports'], p['subscribers'], p['messages_by_payload'], p['consume_modes'])}
        if only:
            expected = {k for k in expected if [str(v) for v in k[:5]] == only}
        assert {key(v['config']) for v in verdicts} == expected, f'{directory}: 配置矩阵不完整'
    assert len(verdicts) == len(expected)
    for run in runs:
        messages, pools, failures = acceptance.parse_measurements((directory / (run['name'] + '.txt')).read_text())
        assert (messages, pools, failures) == (run['messages'], run['pools'], run['parse_failures']), \
            f'{directory}: 原始日志与 JSON 不一致'
        current = not run['name'].startswith('baseline_')
        expected = acceptance.validate_run(run, run['config'], current)
        assert expected == run['reliability_failures'] and run['reliable'] == (not expected)
        if current and run['config']['metrics']:
            curves = list((directory / 'occupancy' / run['name']).glob('*.csv'))
            assert len(curves) == 1, f'{directory}: 池曲线缺失或重复'
            with curves[0].open() as file:
                rows = list(csv.DictReader(file))
            assert rows and all(int(row['consistent']) == 1 and
                                int(row['capacity']) == 10 and int(row['used']) <= 9 and
                                int(row['free']) + int(row['used']) == 10 for row in rows)
            assert int(rows[-1]['free']) == 10 and int(rows[-1]['used']) == 0
            assert all(int(a['monotonic_ns']) < int(b['monotonic_ns']) for a, b in zip(rows, rows[1:]))
    for verdict in verdicts:
        selected = [run for run in runs if key(run['config']) == key(verdict['config'])]
        if kind == 'consumer':
            selected = [run for run in selected if run['name'].startswith(verdict['group'] + '_')]
            assert len(selected) == budget['windows']
            checks = acceptance.consumer_checks(verdict['group'], selected, budget)
            assert checks == verdict['checks'] and verdict['passed'] == all(checks.values())
        else:
            before = [run for run in selected if run['name'].startswith('baseline_')]
            after = [run for run in selected if run['name'].startswith('current_')]
            comparison = acceptance.compare_performance(before, after, budget)
            assert comparison == {k: v for k, v in verdict.items() if k != 'config'}
    return runs, verdicts, acceptance.build_summary(kind, runs, verdicts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(pathlib.Path(__file__).resolve().parent))
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    out = root / 'acceptance_review'
    consumer_runs, consumer_verdicts, consumer = audit_directory(root / 'consumer_final', 'consumer')
    performance, effective = {}, {}
    evidence_paths = ['consumer_final', 'performance_final', 'performance_loaned_final']
    for directory in evidence_paths[1:]:
        _, verdicts, summary = audit_directory(root / directory, 'performance')
        performance[directory] = summary
        for verdict in verdicts:
            effective[key(verdict['config'])] = {
                'config': verdict['config'], 'first_status': verdict['status'],
                'status': verdict['status'], 'evidence': directory, 'verdict': verdict,
            }
    confirmation_budget = load(root / 'performance_final_confirmation_budget.json')
    required_thresholds = ('minimum_throughput_ratio', 'maximum_publish_p99_ratio',
                           'maximum_cpu_per_message_ratio', 'maximum_rss_ratio',
                           'rss_allowance_kb', 'maximum_rss_kb')
    for case in confirmation_budget['performance']['confirmation_configs']:
        name = 'performance_final_confirmation_' + '_'.join(case)
        _, verdicts, summary = audit_directory(root / name, 'performance', only=case)
        assert len(verdicts) == 1
        verdict = verdicts[0]
        result = effective[key(verdict['config'])]
        assert result['first_status'] == 'over_budget', f'{name}: 只允许确认首轮失败项'
        original = load(root / result['evidence'] / 'budget_snapshot.json')['performance']
        confirmed = load(root / name / 'budget_snapshot.json')['performance']
        assert all(original[k] == confirmed[k] for k in required_thresholds), '确认轮不能放宽阈值'
        result.update(status=verdict['status'], confirmation=name, verdict=verdict)
        performance[name] = summary
        evidence_paths.append(name)
    group_totals = {}
    for label, predicate in (
            ('raw', lambda c: c['transport'] == 'raw'),
            ('dzflat_object', lambda c: c['transport'] == 'dzflat' and c.get('dzflat_publish') == 'object'),
            ('dzflat_loaned', lambda c: c['transport'] == 'dzflat' and c.get('dzflat_publish') == 'loaned')):
        selected = [v for v in effective.values() if predicate(v['config'])]
        group_totals[label] = dict(configurations=len(selected),
                                  **collections.Counter(v['status'] for v in selected))
    groups = {}
    for group in ('matrix', 'stress32', 'hold', 'slow_start'):
        runs = [run for run in consumer_runs if run['name'].startswith(group + '_')]
        verdicts = [v for v in consumer_verdicts if v['group'] == group]
        groups[group] = acceptance.build_summary('consumer', runs, verdicts)
    ctest = (out / 'ctest_final.txt').read_text()
    match = re.search(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)', ctest)
    assert match and match[1] == '100' and match[2] == '0', '最终聚焦 CTest 未完成或未通过'
    ctest_names = re.findall(r'Test\s+#\d+:\s+(\w+)\s+\.\.\..*Passed', ctest)
    assert len(ctest_names) == int(match[3])
    regressions = dict(ctest_total=int(match[3]), ctest_passed=int(match[3]),
                       loan_pool_total=sum(name.startswith('loan_pool_') for name in ctest_names),
                       ctest_names=ctest_names)
    for name in ('test_topic_chunk_pool', 'test_loan', 'test_uf011_chunk_return',
                 'test_lap_safety', 'test_uf003_crash_reclaim'):
        log = (out / (name + '.txt')).read_text()
        passed = re.search(r'\[  PASSED  \] (\d+) tests?\.', log)
        assert passed and '[  FAILED  ]' not in log, f'{name}: 回归失败或不完整'
        regressions[name] = int(passed[1])
    evidence = {}
    for name in evidence_paths:
        for file in ('runs.json', 'verdicts.json', 'budget_snapshot.json', 'run_identity.json'):
            path = root / name / file
            evidence[str(path.relative_to(root))] = digest(path)
    baseline_identity = load(root / 'performance_final/baseline_identity.json')
    repo = root.parents[2]
    source_identity = {str(path.relative_to(repo)): digest(path) for path in (
        repo / 'test/loan_phase_latency_benchmark.cpp', repo / 'test/CMakeLists.txt',
        root / 'run_acceptance.py', root / 'build_baseline.py', root / 'report_acceptance.py',
        root / 'test_acceptance.py')}
    final_source_hash = source_identity['test/loan_phase_latency_benchmark.cpp']
    assert final_source_hash == baseline_identity['measurement_source_sha256']
    for name in evidence_paths:
        assert load(root / name / 'run_identity.json')['measurement_source_sha256'] == final_source_hash
    peak = max(int(run['messages'][0]['rss_peak_kb']) for run in consumer_runs)
    slow = [run['messages'][0] for run in consumer_runs if run['name'].startswith('slow_start_') and
            run['config']['transport'] == 'dzflat']
    copy_holds = [run['messages'][0] for run in consumer_runs if run['name'].startswith('hold_') and
                  run['config']['consume_mode'] == 'copy']
    consumer_observations = dict(peak_rss_kb=peak,
        slow_queue_high_watermarks=sorted({int(row['queue_high_watermark']) for row in slow}),
        slow_queue_residence_max_ns_range=[min(int(row['queue_residence_max_ns']) for row in slow),
                                           max(int(row['queue_residence_max_ns']) for row in slow)],
        copy_sample_hold_p99_ns_max=max(int(row['sample_hold_p99_ns']) for row in copy_holds),
        hold_us=load(root / 'consumer_final/budget_snapshot.json')['consumer']['hold_us'])
    gates = dict(consumer=all(v['passed'] for v in consumer_verdicts),
                 performance=all(v['status'] == 'passed' for v in effective.values()),
                 adaptive_growth_allowed=False)
    report = dict(consumer=consumer, consumer_groups=groups, consumer_observations=consumer_observations,
                   performance=performance, effective_performance=list(effective.values()),
                   effective_group_totals=group_totals, regressions=regressions, gates=gates,
                   evidence_sha256=evidence, current_source_sha256=source_identity,
                   baseline_identity=baseline_identity,
                   current_library_flags=(repo / 'build/src/CMakeFiles/ipc.dir/flags.make').read_text(),
                   current_measurement_flags=(repo / 'build/test/CMakeFiles/loan_phase_latency_measure.dir/flags.make').read_text())
    acceptance.save_json(out / 'report.json', report)
    lines = [
        '# 性能预算及消费路径独立验收', '',
        '消费可靠性与独立生命周期验收通过；相对旧版的性能预算未全部通过，自适应扩容不准入。', '',
        '## 预算与口径', '',
        '预算在运行前冻结：吞吐比例 >= 0.90，发布 p99 比例 <= 1.20，单位消息 CPU 比例 <= 1.25，'
        'RSS <= 基线 * 1.25 + 8 MiB，单发布者 RSS 硬上限 512 MiB。'
        '消费矩阵 RSS 硬上限 1 GiB；三窗均要求正确交付、最终回满、无所有权错误。', '',
        '同一测量源码分别链接匹配 HEAD 源码/头文件的旧库和当前库；raw 都使用显式 loan 与 '
        'try_publish_loan。旧库没有有界 loan，只有池耗尽时进行带截止时间的 yield 重试。'
        'DZFlat 对象发布与 B 级直接借样分别验收，任何 TLV 回退均使该性能配置不可判定。', '',
        '每窗独立进程，交错新旧执行顺序，性能比较三窗中位数。吞吐排除线程准备与 20 ms 静默核验；'
        'CPU 覆盖发布与实际排空。Linux RSS 使用 /proc/self/status 的 VmHWM，'
        'rusage_peak_kb 仅作继承父进程高水位的诊断。负载写入及完整字节校验保留，CPU 包含基准这些应用工作。', '',
        '## 消费与压力', '',
        '| 分组 | 配置 | 窗口 | 尝试 | 接受 | 信用拒绝 |',
        '| --- | ---: | ---: | ---: | ---: | ---: |',
    ]
    for name, summary in groups.items():
        lines.append(f'| {name} | {summary["configurations"]} | {summary["windows"]} | '
                     f'{summary["attempted"]} | {summary["published"]} | {summary["pool_exhausted"]} |')
    lines += ['', f'共 {consumer["passed_configurations"]}/{consumer["configurations"]} 配置、'
              f'{consumer["reliable_windows"]}/{consumer["windows"]} 窗口通过。每个订阅者收到全部已接受消息，'
              '缺失、重复、损坏、发布者内乱序、非信用拒绝、队列淘汰和剩余队列均为零。'
              f'264 条占用曲线均一致、最大占用不超过 9、结束回满 10/10；进程峰值 RSS 为 {peak} KiB。', '',
              '慢启动只暂停第 0 个应用消费者 100 ms，DZFlat 传输接收仍持续 drain，应用队列高水位均为 9。'
              '受控应用处理 2 ms 时，复制模式共享持样 p99 均小于 500 us，'
              f'实测最大 {consumer_observations["copy_sample_hold_p99_ns_max"] / 1000:.3f} us；'
              '零拷贝处理等待计入共享持样，复制模式在共享样本释放后才校验和处理 owning 数据。', '',
              '## 性能判定', '',
              '| 路径 | 配置 | 通过 | 超预算 | 不可判定 |',
              '| --- | ---: | ---: | ---: | ---: |']
    for group, totals in group_totals.items():
        lines.append(f'| {group} | {totals["configurations"]} | {totals.get("passed", 0)} | '
                     f'{totals.get("over_budget", 0)} | {totals.get("inconclusive", 0)} |')
    lines += ['', '上表使用一次长窗口确认后的最终判定。完整矩阵首轮仍分别保留：'
              '对象/raw 32 配置为 12 通过、5 超预算、15 不可判定；B 级 16 配置为 15 通过、1 超预算。'
              '只对这 6 个超预算项按预先冻结的相同阈值确认一次，不覆盖首轮结果。', '',
              '| 最终超预算配置 | 吞吐比例 | p99 比例 | CPU/消息比例 | 失败指标 |',
              '| --- | ---: | ---: | ---: | --- |']
    for value in effective.values():
        if value['status'] != 'over_budget':
            continue
        verdict, config = value['verdict'], value['config']
        ratio = verdict['ratios']
        failed = ', '.join(name for name, passed in verdict['checks'].items() if not passed)
        lines.append(f'| {config["transport"]} / {config["subs"]} 订阅者 / {config["payload"]} B / '
                     f'{config["consume_mode"]} | {ratio["throughput_msgs_per_s"]:.4f} | '
                     f'{ratio["publish_p99_ns"]:.4f} | {ratio["cpu_ns_per_message"]:.4f} | {failed} |')
    previous = performance['performance_final']['versions']['baseline']
    lines += ['', f'对象发布旧版共出现 {previous["tlv"]} 条 TLV 交付（按订阅者计）及 '
              f'{previous["delivery_errors"]["missing"]} 条缺失；这些 15 个配置不可用于证明无回退。'
              '当前版本的对象/raw 96 窗口、B 级 48 窗口，以及全部长窗口确认均无交付错误，当前 DZFlat 无 TLV 回退。'
              '最终可比配置的 RSS 相对预算与硬上限均通过。', '',
              '## 回归与证据', '',
              f'聚焦 CTest {regressions["ctest_passed"]}/{regressions["ctest_total"]} 通过，其中 loan_pool_* '
              f'{regressions["loan_pool_total"]}/{regressions["loan_pool_total"]}。池完整套件 35/35、loan 10/10、'
              'UF-011 严格回满 1/1、覆写安全 3/3、失联回收 2/2 通过。'
              '本轮未重跑全量 CTest；前序 74/81 与 7 项修改前已复现失败仍保留，不宣称全量通过。', '',
              '原始输出、命令、返回码、逐订阅者统计、曲线和预算分别见 '
              '`../consumer_final/`、`../performance_final/`、`../performance_loaned_final/` '
              '及 `../performance_final_confirmation_*/`。`report.json` 保存独立复核后的判定和证据哈希。', '',
              '旧 RSS 口径、被后台日志打断的轮次、旧版本长窗口确认均保留，'
              '其路径为 `consumer_rusage_previous/`、`performance_rusage_previous/`、'
              '`performance_loaned_rusage_previous/`、`performance_loaned_interrupted/`、'
              '`performance_raw4k*confirmation/` 与 `performance_loaned_long_*/`。'
              '它们不作为最终 RSS 预算证据。', '',
              '## 阶段准入', '',
              '本轮验收工作完成，但性能预算门未通过。保持固定容量 10、有界发布信用 9；'
              '自适应扩容未实现。下一阶段先处理以上三项性能超预算，再补齐对象发布入口的可比对照，'
              '通过后才能推进独立分段扩容。', '']
    (out / 'report.md').write_text('\n'.join(lines))
    print(f'验收报告已生成：{out / "report.md"}；消费通过，性能预算未通过，扩容不准入。')


if __name__ == '__main__':
    main()
