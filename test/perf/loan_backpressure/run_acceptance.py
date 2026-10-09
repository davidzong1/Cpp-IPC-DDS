#!/usr/bin/env python3
"""独立消费验收与一致入口性能预算对照；串行运行并保留失败证据。"""
import argparse
import csv
import hashlib
import itertools
import json
import math
import os
import pathlib
import platform
import statistics
import subprocess
import time

from run_matrix import parse_rows


ERRORS = ('other_failed', 'missing', 'unexpected', 'duplicate', 'corrupt', 'out_of_order')


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def parse_measurements(stdout):
    try:
        return parse_rows(stdout, 'RESULT'), parse_rows(stdout, 'POOL'), []
    except ValueError:
        return [], [], ['测量记录被打断或格式无效，保留原始输出但不可判定']


def validate_run(run, config, current=True):
    """校验输出的完整性；性能基线只校验交付，旧库没有共享池快照。"""
    failures = list(run.get('parse_failures', []))
    messages, pools = run['messages'], run['pools']
    if run['exit_code'] != 0:
        failures.append(f'进程退出码 {run["exit_code"]}')
    if len(messages) != config['subs'] or {row.get('subscriber') for row in messages} != {
            str(s) for s in range(config['subs'])}:
        failures.append('订阅者结果不完整或重复')
    try:
        first = messages[0]
        for row in messages:
            for key in ('transport', 'pubs', 'subs', 'payload', 'consume_mode', 'hold_us', 'slow_start_us',
                        'metrics'):
                if row[key] != str(config[key]):
                    failures.append(f'{key} 与配置不符')
            if 'dzflat_publish' in config and row.get('dzflat_publish', 'object') != config['dzflat_publish']:
                failures.append('DZFlat 发布入口与配置不符')
            if row['window'] != '1' or int(row['baseline']) != int(not current):
                failures.append('窗口或基线身份不符')
            accepted, rejected = int(row['published']), int(row['pool_exhausted'])
            if not (int(row['attempted']) == config['msgs'] and 0 < accepted <= config['msgs'] and
                    accepted + rejected == config['msgs'] and int(row['publish_failed']) == rejected and
                    int(row['received']) == accepted and int(row['publish_n']) == config['msgs']):
                failures.append('尝试、接受、拒绝或交付计数不一致')
            if config['require_all'] and rejected:
                failures.append('常规窗口有信用拒绝')
            if any(int(row[key]) for key in ERRORS):
                failures.append('存在交付错误或非信用拒绝')
            if any(row[key] != first[key] for key in ('published', 'pool_exhausted', 'publish_failed',
                                                     'cpu_total_ns', 'rss_peak_kb', 'elapsed_ns')):
                failures.append('同窗发布或资源统计不一致')
            for key in ('recv_n', 'destroy_n', 'sample_hold_n', 'app_work_n'):
                if int(row[key]) != accepted:
                    failures.append(f'{key} 样本数不完整')
            expected_bytes = accepted * config['payload'] if config['consume_mode'] == 'copy' else 0
            if int(row['copied_bytes']) != expected_bytes:
                failures.append('复制字节数不符合消费模式')
            if config['transport'] == 'dzflat':
                if int(row['flat']) != accepted or int(row['tlv']) != 0:
                    failures.append('DZFlat 路径发生 TLV 回退，不能作独立消费对照')
                if current and config['metrics'] and int(row['queue_residence_n']) != accepted:
                    failures.append('DZFlat 队列驻留样本数不完整')
                if current and (int(row['queue_evicted']) or int(row['consumer_lag'])):
                    failures.append('应用队列有淘汰或未排空')
                if current and int(row['queue_high_watermark']) > 9:
                    failures.append('应用队列超过固定信用上限')
            elif int(row['transport_latency_n']) != accepted or 'queue_residence_n' in row:
                failures.append('raw 传输时延口径或样本数错误')
            for key in ('elapsed_ns', 'drain_elapsed_ns', 'cpu_total_ns', 'rss_peak_kb',
                        'throughput_msgs_per_s', 'drain_throughput_msgs_per_s', 'publish_p99_ns'):
                value = float(row[key])
                if not math.isfinite(value) or value <= 0:
                    failures.append(f'{key} 无有效测量值')
            if int(row['drain_elapsed_ns']) < int(row['elapsed_ns']):
                failures.append('排空计时短于生产计时')
        if current:
            if len(pools) != 1:
                failures.append('池快照不完整')
            else:
                pool = pools[0]
                if not (int(pool['capacity']) == 10 and int(pool['publisher_cap']) == 9 and
                        int(pool['final_free']) == 10 and int(pool['consistent']) == 1 and
                        int(pool['used_max']) <= 9 and int(pool['loan_attempt']) == config['msgs'] and
                        pool['loan_success'] == first['published'] and pool['loan_reject'] == first['pool_exhausted']):
                    failures.append('池信用不一致、超限或未回满')
                if any(int(pool[key]) for key in ('waiters', 'duplicate_return', 'invalid_storage_id',
                                                  'pool_chain_corrupt')):
                    failures.append('池所有权或空闲链诊断非零')
        elif pools:
            failures.append('修改前基线不应输出当前信用快照')
    except (KeyError, ValueError, IndexError, TypeError):
        failures.append('输出缺少必要字段或字段无效')
    return sorted(set(failures))


def execute(binary, config, label, out, budget, current=True):
    name = label + '_' + '_'.join(str(config[k]) for k in (
        'transport', 'pubs', 'subs', 'payload', 'consume_mode', 'hold_us', 'slow_start_us'))
    options = {'consume_mode': 'consume-mode', 'hold_us': 'hold-us',
               'slow_start_us': 'slow-start-us', 'require_all': 'require-all', 'dzflat_publish': 'dzflat-publish'}
    command = [str(binary)] + [f'--{options.get(k, k)}={v}' for k, v in config.items()]
    command.append('--windows=1')
    if current and config['metrics']:
        command.append(f'--csv-dir={out / "occupancy" / name}')
    started = time.monotonic()
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=budget['process_timeout_seconds'])
        code, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or '', error.stderr or ''
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors='replace')
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors='replace')
        code, stderr = 124, stderr + '\n' + str(error)
    (out / (name + '.txt')).write_text(stdout + stderr)
    messages, pools, parse_failures = parse_measurements(stdout)
    run = dict(name=name, command=command, exit_code=code, seconds=time.monotonic() - started,
               config=config, messages=messages, pools=pools, parse_failures=parse_failures)
    run['reliability_failures'] = validate_run(run, config, current)
    run['reliable'] = not run['reliability_failures']
    return run


def configuration(transport, pubs, subs, payload, mode, messages, budget, **overrides):
    config = dict(transport=transport, pubs=pubs, subs=subs, payload=payload, consume_mode=mode,
                  msgs=messages, rate=0, timeout=budget['timeout_ms'], require_all=1,
                  hold_us=0, slow_start_us=0, metrics=1)
    config.update(overrides)
    return config


def consumer_configs(budget):
    b = budget['consumer']
    for transport, pubs, subs, payload, mode in itertools.product(
            b['transports'], b['publishers'], b['subscribers'], b['payloads'], b['consume_modes']):
        yield 'matrix', configuration(transport, pubs, subs, payload, mode, b['matrix_messages'], budget)
    for transport, payload, mode in itertools.product(b['transports'], b['payloads'], b['consume_modes']):
        yield 'stress32', configuration(transport, 32, 8, payload, mode, b['stress_messages'], budget,
                                        require_all=0)
    for transport, payload, mode in itertools.product(b['transports'], b['payloads'], b['consume_modes']):
        yield 'hold', configuration(transport, 4, 8, payload, mode, b['hold_messages'], budget,
                                    hold_us=b['hold_us'])
        yield 'slow_start', configuration(transport, 32, 8, payload, mode, b['hold_messages'], budget,
                                          slow_start_us=b['slow_start_us'], timeout=b['slow_timeout_ms'],
                                          require_all=0)


def consumer_checks(group, runs, budget):
    b = budget['consumer']
    checks = {'accepted_delivery_and_pool': all(run['reliable'] for run in runs)}
    if not checks['accepted_delivery_and_pool']:
        return checks
    checks['rss_hard_limit'] = all(int(run['messages'][0]['rss_peak_kb']) <= b['maximum_rss_kb'] for run in runs)
    if group == 'hold':
        rows = [run['messages'][0] for run in runs]
        hold_ns = runs[0]['config']['hold_us'] * 1000
        checks['application_delay_observed'] = all(int(row['app_work_p50_ns']) >= hold_ns for row in rows)
        if runs[0]['config']['consume_mode'] == 'copy':
            checks['shared_sample_released_before_work'] = all(
                int(row['sample_hold_p99_ns']) <= hold_ns * b['maximum_copy_hold_fraction'] for row in rows)
        else:
            checks['zero_copy_delay_counted_as_shared_hold'] = all(
                int(row['sample_hold_p50_ns']) >= hold_ns for row in rows)
    if group == 'slow_start':
        checks['explicit_credit_backpressure'] = all(int(run['messages'][0]['pool_exhausted']) > 0 for run in runs)
        checks['credit_high_watermark_observed'] = all(int(run['pools'][0]['used_max']) == 9 for run in runs)
        if runs[0]['config']['transport'] == 'dzflat':
            checks['receive_thread_kept_draining'] = all(
                int(run['messages'][0]['queue_high_watermark']) >= b['minimum_slow_queue_high_watermark']
                for run in runs)
            checks['slow_queue_residence_observed'] = all(
                int(run['messages'][0]['queue_residence_max_ns']) >= b['minimum_slow_residence_ns']
                for run in runs)
    return checks


def collect_metrics(runs):
    rows = [run['messages'][0] for run in runs]
    return {
        'throughput_msgs_per_s': statistics.median(float(row['throughput_msgs_per_s']) for row in rows),
        'drain_throughput_msgs_per_s': statistics.median(float(row['drain_throughput_msgs_per_s']) for row in rows),
        'publish_p99_ns': statistics.median(int(row['publish_p99_ns']) for row in rows),
        'cpu_ns_per_message': statistics.median(int(row['cpu_total_ns']) / int(row['published']) for row in rows),
        'rss_peak_kb': statistics.median(int(row['rss_peak_kb']) for row in rows),
    }


def compare_performance(before, after, budget):
    if len(before) != budget['windows'] or len(after) != budget['windows'] or \
            not all(run['reliable'] for run in before + after):
        return {'status': 'inconclusive', 'reason': '基线或当前版本可靠性/路径校验未通过'}
    baseline, current = collect_metrics(before), collect_metrics(after)
    ratios = {key: current[key] / baseline[key] for key in baseline}
    b = budget['performance']
    checks = {
        'throughput': ratios['throughput_msgs_per_s'] >= b['minimum_throughput_ratio'],
        'drain_throughput': ratios['drain_throughput_msgs_per_s'] >= b['minimum_throughput_ratio'],
        'publish_p99': ratios['publish_p99_ns'] <= b['maximum_publish_p99_ratio'],
        'cpu_per_message': ratios['cpu_ns_per_message'] <= b['maximum_cpu_per_message_ratio'],
        'rss_relative': current['rss_peak_kb'] <= baseline['rss_peak_kb'] * b['maximum_rss_ratio'] + b['rss_allowance_kb'],
        'rss_hard_limit': all(int(run['messages'][0]['rss_peak_kb']) <= b['maximum_rss_kb'] for run in after),
    }
    return {'status': 'passed' if all(checks.values()) else 'over_budget',
            'baseline': baseline, 'current': current, 'ratios': ratios, 'checks': checks}


def write_results(out, runs):
    save_json(out / 'runs.json', runs)
    fields = sorted({key for run in runs for row in run['messages'] for key in row})
    with (out / 'results.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=['configuration'] + fields)
        writer.writeheader()
        for run in runs:
            for row in run['messages']:
                writer.writerow(dict(configuration=run['name'], **row))


def build_summary(kind, runs, verdicts):
    summary = dict(kind=kind, configurations=len(verdicts), windows=len(runs),
                   reliable_windows=sum(run['reliable'] for run in runs),
                   passed_configurations=sum(v.get('passed', v.get('status') == 'passed') for v in verdicts),
                   over_budget_configurations=sum(v.get('status') == 'over_budget' for v in verdicts),
                   inconclusive_configurations=sum(v.get('status') == 'inconclusive' for v in verdicts))
    def totals(selected):
        rows = [row for run in selected for row in run['messages']]
        result = dict(windows=len(selected), reliable_windows=sum(run['reliable'] for run in selected),
                      delivery_errors={key: sum(int(row.get(key, 0)) for row in rows) for key in ERRORS},
                      flat=sum(int(row.get('flat', 0)) for row in rows),
                      tlv=sum(int(row.get('tlv', 0)) for row in rows))
        for key in ('attempted', 'published', 'pool_exhausted'):
            result[key] = sum(int(run['messages'][0][key]) for run in selected if run['messages'])
        return result
    all_totals = totals(runs)
    summary.update({key: value for key, value in all_totals.items() if key not in ('windows', 'reliable_windows')})
    versions = ('baseline', 'current') if kind == 'performance' else ('current',)
    summary['versions'] = {
        version: totals([run for run in runs if
                         (run['name'].startswith('baseline_') if version == 'baseline' else
                          not run['name'].startswith('baseline_'))]) for version in versions}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=('consumer', 'performance'), required=True)
    parser.add_argument('--binary', default='build/bin/loan_phase_latency_measure')
    parser.add_argument('--baseline')
    parser.add_argument('--budget', default=str(pathlib.Path(__file__).with_name('acceptance_budget.json')))
    parser.add_argument('--output', required=True)
    parser.add_argument('--only', nargs=5, metavar=('TRANSPORT', 'PUBS', 'SUBS', 'PAYLOAD', 'MODE'),
                        help='按相同预算独立复测一个配置，保留原有验收结果')
    args = parser.parse_args()
    budget = json.loads(pathlib.Path(args.budget).read_text())
    if budget['windows'] < 3:
        parser.error('独立验收至少需要 3 个窗口')
    if args.kind == 'performance' and not args.baseline:
        parser.error('性能对照必须提供匹配源码与头文件的修改前基线')
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'runs.json').exists():
        parser.error('输出目录已有运行证据，请使用新目录')
    binary = pathlib.Path(args.binary).resolve()
    baseline = pathlib.Path(args.baseline).resolve() if args.baseline else None
    identity = dict(started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                    budget_sha256=digest(args.budget), binary=str(binary), binary_sha256=digest(binary),
                    baseline=str(baseline) if baseline else None,
                    baseline_sha256=digest(baseline) if baseline else None,
                    platform=platform.platform(), cpu_affinity=sorted(os.sched_getaffinity(0)),
                    rss_source='Linux /proc/self/status VmHWM（当前 exec 后地址空间）；rusage_peak_kb 保留父进程继承高水位作诊断',
                    cpu_info=subprocess.check_output(['lscpu'], text=True),
                    current_library_sha256=digest(binary.parent.parent / 'lib/libipc.so.1.6.0'),
                    runner_sha256=digest(__file__),
                    measurement_source_sha256=digest(pathlib.Path(__file__).resolve().parents[3] /
                                                     'test/loan_phase_latency_benchmark.cpp'))
    save_json(out / 'run_identity.json', identity)
    save_json(out / 'budget_snapshot.json', budget)
    runs, verdicts = [], []
    if args.kind == 'consumer':
        configs = list(consumer_configs(budget))
        if args.only:
            configs = [(group, config) for group, config in configs if
                       [str(config[k]) for k in ('transport', 'pubs', 'subs', 'payload', 'consume_mode')]
                       == args.only]
        if not configs:
            parser.error('所选配置不在既定消费矩阵中')
        for index, (group, config) in enumerate(configs, 1):
            windows = []
            for window in range(budget['windows']):
                run = execute(binary, config, f'{group}_w{window + 1}', out, budget)
                windows.append(run)
                runs.append(run)
                save_json(out / 'runs.json', runs)
            checks = consumer_checks(group, windows, budget)
            verdicts.append(dict(group=group, config=config, checks=checks, passed=all(checks.values())))
            save_json(out / 'verdicts.json', verdicts)
            print(f'{index}/{len(configs)} {group} {config["transport"]} '
                  f'{config["pubs"]}发布者/{config["subs"]}订阅者 {config["payload"]}字节 '
                  f'{config["consume_mode"]} {"通过" if all(checks.values()) else "失败"}', flush=True)
    else:
        b = budget['performance']
        configs = list(itertools.product(b['transports'], b['subscribers'],
                                        b['messages_by_payload'], b['consume_modes']))
        if args.only:
            configs = [(transport, subs, size, mode) for transport, subs, size, mode in configs if
                       [transport, str(b['publishers']), str(subs), size, mode] == args.only]
        if not configs:
            parser.error('所选配置不在既定性能矩阵中')
        for index, (transport, subs, size, mode) in enumerate(configs, 1):
            config = configuration(transport, b['publishers'], subs, int(size), mode,
                                   b['messages_by_payload'][size], budget, metrics=0,
                                   dzflat_publish=b.get('dzflat_publish', 'object'))
            before, after = [], []
            for window in range(budget['windows']):
                # 每个窗口独立进程；交错顺序限制温度、频率和背景负载的方向性偏差。
                versions = [('baseline', baseline, before, False), ('current', binary, after, True)]
                if (index + window) % 2 == 0:
                    versions.reverse()
                for label, executable, target, current in versions:
                    run = execute(executable, config, f'{label}_w{window + 1}', out, budget, current)
                    target.append(run)
                    runs.append(run)
                    save_json(out / 'runs.json', runs)
            comparison = compare_performance(before, after, budget)
            verdicts.append(dict(config=config, **comparison))
            save_json(out / 'verdicts.json', verdicts)
            label = {'passed': '通过', 'over_budget': '超预算', 'inconclusive': '不可判定'}[comparison['status']]
            print(f'{index}/{len(configs)} {transport} 1发布者/{subs}订阅者 {size}字节 {mode} {label}', flush=True)
    write_results(out, runs)
    summary = build_summary(args.kind, runs, verdicts)
    save_json(out / 'summary.json', summary)
    return 0 if summary['passed_configurations'] == len(verdicts) else 1


if __name__ == '__main__':
    raise SystemExit(main())
