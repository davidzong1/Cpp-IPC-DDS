#!/usr/bin/env python3
"""审计采样完整性，并按真实样本区间标记其他编译/压测重叠。"""
import argparse
from collections import Counter
import csv
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
from summarize_perf_scheduler import stats

ROOT = Path(__file__).resolve().parents[2]


def original_bytes(path):
    # 归档时仅无损压缩文本日志，采样 manifest 中的原始字节哈希保持不变。
    return path.read_bytes() if path.exists() else gzip.decompress(Path(str(path)+'.gz').read_bytes())


def check_settings(before, after):
    assert before['cpus'] == after['cpus'] == list(range(32))
    for key in ('kernel', 'cpuidle_driver', 'core_types'):
        if key in before:
            assert before[key] == after[key]
    assert set(before['cpu']) == set(after['cpu'])
    for cpu, values in before['cpu'].items():
        final = after['cpu'][cpu]
        for key in ('scaling_governor', 'scaling_driver', 'cpuinfo_max_freq'):
            assert values['frequency'][key] == final['frequency'][key]
        assert set(values['idle']) == set(final['idle'])
        for state, idle in values['idle'].items():
            for key in ('name', 'latency', 'disable'):
                assert idle[key] == final['idle'][state][key]


def read(path):
    with gzip.open(path, 'rt') as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path); args = parser.parse_args()
    windows = []
    manifests = [args.directory/'manifest.json'] if (args.directory/'manifest.json').exists() else sorted(args.directory.glob('*/manifest.json'))
    if not manifests:
        raise ValueError('找不到采样 manifest')
    legacy_decode = False
    for manifest_path in manifests:
        manifest = json.loads(manifest_path.read_text())
        base = manifest_path.parent
        assert manifest['workload_uid'] == 1000
        assert len(manifest['windows']) == len(manifest['cases'])
        check_settings(manifest['settings_before'], manifest['settings_after'])
        for path, sha in manifest['hashes'].items():
            p = Path(path)
            if path in manifest.get('source_snapshots', {}):
                assert hashlib.sha256((base/manifest['source_snapshots'][path]).read_bytes()).hexdigest() == sha, path
            elif '/build-' in path:
                assert hashlib.sha256(p.read_bytes()).hexdigest() == sha
            elif p.is_relative_to(ROOT):
                source = subprocess.check_output(['git', 'show', f"{manifest['head']}:{p.relative_to(ROOT)}"], cwd=ROOT)
                assert hashlib.sha256(source).hexdigest() == sha, path
                if p.name == 'perf_scheduler_matrix.py':
                    legacy_decode |= b'--show-lost-events' not in source
            elif path in manifest.get('hashes', {}):
                # 旧版本诊断工件来自独立冻结工作区，必须由批次内内容寻址快照校验。
                raise AssertionError(f'缺少来源快照: {path}')
        for w in manifest['windows']:
            folder = base/w['case']
            assert w['returncode'] == w.get('decode_returncode', 0) == 0
            assert w.get('stacks_decode_returncode', 0) == 0
            check_settings(manifest['settings_before'], w['before'])
            check_settings(w['before'], w['after'])
            for path, sha in w['files'].items():
                assert hashlib.sha256(original_bytes(base/path)).hexdigest() == sha, path
            result = json.loads((folder/'result.json').read_text())
            summary = json.loads((folder/'scheduler-summary.json').read_text())
            receiver_cpu_requests = result.get('environment', {}).get('receiver_affinity_cpus')
            receiver_affinity = result.get('receiver_affinity') or []
            if receiver_cpu_requests is None:
                assert not any(receiver_affinity)
            else:
                assert len(receiver_cpu_requests) == result['subscribers']
                assert len(receiver_affinity) == result['subscribers']
                for index, cpu in enumerate(receiver_cpu_requests):
                    placement = receiver_affinity[index]
                    assert placement is not None
                    assert placement['requested_cpu'] == cpu
                    assert placement['set_result'] == placement['get_result'] == 0
                    assert placement['allowed_cpus'] == [cpu]
                    rows = read(folder/f'sub{index}.csv.gz')
                    assert {row['receiver_tid'] for row in rows} == {placement['tid']}
            expected = result['rate']*result['seconds']
            pubs = read(folder/'pub.csv.gz')
            assert len(pubs) == expected and len({p['sequence'] for p in pubs}) == expected
            assert result['publish']['accepted'] == expected and result['publish']['rejected'] == 0
            assert all(p['success'] == 1 for p in pubs)
            samples = [r for path in folder.glob('sub*.csv.gz') for r in read(path)]
            assert len(samples) == expected*result['subscribers'] == summary['samples']
            assert len({r['reader_tid'] for r in samples}) == result['subscribers']
            pub_by_seq = {p['sequence']: p for p in pubs}
            assert all(r['read_ns']-pub_by_seq[r['sequence']]['start_ns'] == r['elapsed_ns'] >= 0 for r in samples)
            assert summary['latency'] == stats([r['elapsed_ns'] for r in samples])
            assert summary['publish'] == stats([p['elapsed_ns'] for p in pubs])
            assert summary['copy_wall'] == stats([p['copy_end_ns']-p['copy_begin_ns'] for p in pubs])
            assert summary['copy_cpu'] == stats([p['copy_cpu_end_ns']-p['copy_cpu_begin_ns'] for p in pubs])
            perf_enabled = (folder/'events.txt.gz').exists()
            if perf_enabled:
                assert summary['matched']+sum(summary['unmatched'].values()) == len(samples)
                by_key = {(r['reader_tid'], r['sequence']): r for r in samples}
                seen = set()
                matched, publisher_wakes, misses = 0, 0, Counter()
                with gzip.open(folder/'joined.csv.gz', 'rt') as f:
                    for joined in csv.DictReader(f):
                        key = (int(joined['reader_tid']), int(joined['sequence']))
                        assert key not in seen
                        seen.add(key)
                        original = by_key[key]
                        assert int(joined['elapsed_ns']) == original['elapsed_ns']
                        if not int(joined['matched']):
                            misses[joined['unmatched_reason']] += 1
                            assert not joined.get('waking_to_wakeup_ns')
                            continue
                        matched += 1
                        publisher_wakes += int(joined['publisher_wake'])
                        assert int(joined['publisher_wake']) == int(int(joined['waker_tid']) == summary['publisher_tid'])
                        end_field = 'scheduler_wait_end_ns' if 'scheduler_wait_end_ns' in joined else 'wait_end_ns'
                        points = [int(joined[k]) for k in ('notify_ns', 'waking_ns', 'wakeup_ns', 'scheduled_ns', end_field)]
                        assert points == sorted(points)
                        assert points[0] == pub_by_seq[key[1]]['notify_begin_ns']
                        assert points[-1] == original.get('receiver_wait_end_ns', original['wait_end_ns'])
                        if 'receiver_tid' in original:
                            assert int(joined['scheduler_tid']) == original['receiver_tid']
                            assert points[-1] <= original['recv_return_ns'] <= original['read_ns']
                        for index, name in enumerate(('notify_to_waking_ns', 'waking_to_wakeup_ns', 'wakeup_to_scheduled_ns', 'scheduled_to_wait_end_ns')):
                            assert int(joined[name]) == points[index+1]-points[index]
                        assert int(joined['notify_to_wait_end_ns']) == points[-1]-points[0]
                assert seen == set(by_key)
                assert matched == summary['matched'] and dict(misses) == summary['unmatched']
                assert publisher_wakes == summary['publisher_wake_count']
            else:
                assert summary['matched'] == 0 and summary['unmatched'] == {'perf_disabled': len(samples)}
            for tid in {r['reader_tid'] for r in samples}:
                selected = [r for r in samples if r['reader_tid'] == tid]
                assert len(selected) == expected and {r['sequence'] for r in selected} == {p['sequence'] for p in pubs}
            assert all(r['count'] == expected and not any(r[key] for key in ('lost', 'invalid', 'duplicates', 'trace_overflow', 'trace_missing_or_unordered')) for r in result['receivers'])
            start = min(p['start_ns'] for p in pubs)
            end = max(r['read_ns'] for r in samples)
            foreign = [{'time_ns': sample['time_ns'], 'process': p} for sample in w['activity'] for p in sample['processes'] if not Path(p['cwd']).is_relative_to(ROOT)]
            overlap = [item for item in foreign if start <= item['time_ns'] <= end]
            uid_rows = [p for sample in w['activity'] for p in sample['processes'] if 'uid' in p]
            assert all(p['uid'].split() == ['1000']*4 for p in uid_rows)
            assert all(p['gid'].split() == ['1000']*4 for p in uid_rows)
            if base.name == 'copy-affinity' or 'kernel_stacks' in manifest:
                assert uid_rows
            for side in ('before', 'after'):
                for i, process in enumerate(result[side]):
                    artifacts = manifest.get('artifacts')
                    library = artifacts[w['artifact']]['library'] if artifacts else str((ROOT/'build-local-latency/lib/libipc.so').resolve())
                    assert process['loaded_libraries'] == [library]
                    affinity = result['affinity_plan']
                    allowed = '0-31' if not affinity else str(affinity['subscribers'][i] if i < result['subscribers'] else affinity['publisher'] if i == result['subscribers'] else affinity['gateway'])
                    target = receiver_affinity[i] if receiver_cpu_requests is not None and i < result['subscribers'] else None
                    for thread in process['thread_status']:
                        expected_allowed = str(target['requested_cpu']) if target and thread['tid'] == target['tid'] else allowed
                        assert thread['Cpus_allowed_list'] == expected_allowed
            assert not summary['lost_event_lines'] and not summary['unknown_event_count']
            windows.append({'case': str(folder.relative_to(args.directory)), 'accepted': expected, 'received': len(samples),
                            'perf': perf_enabled, 'matched': summary['matched'], 'unmatched': summary['unmatched'],
                            'receiver_affinity': receiver_cpu_requests,
                            'foreign_during_capture': foreign, 'foreign_during_samples': overlap, 'uncontended_samples': not overlap,
                            'uid_observations': len(uid_rows), 'latency': summary['latency']})
    if legacy_decode:
        smoke = json.loads((args.directory/'smoke-lost-check.json').read_text())
        assert smoke['returncode'] == 0 and not smoke['lost_lines'] and smoke['same_as_original_decode']
    archive = args.directory/'archive-manifest.json'
    if archive.exists():
        for entry in json.loads(archive.read_text())['files']:
            path = args.directory/entry['archive']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['archive_sha256']
            assert hashlib.sha256(gzip.decompress(path.read_bytes())).hexdigest() == entry['original_sha256']
    out = {'windows': windows, 'window_count': len(windows), 'accepted': sum(w['accepted'] for w in windows), 'received': sum(w['received'] for w in windows),
           'perf_window_count': sum(w['perf'] for w in windows), 'matched': sum(w['matched'] for w in windows), 'overlapped_windows': [w['case'] for w in windows if not w['uncontended_samples']],
           'lost_messages': 0, 'duplicates': 0, 'invalid': 0, 'unknown_events': 0, 'reported_lost_event_records': 0,
           'settings_unchanged': True, 'settings_scope': '批次及每窗前后 CPU 集合、调频驱动/governor/最大频率、空闲态名称/公布延迟/禁用标志；新格式另核对内核版本、cpuidle驱动和核型集合；不代表实际频率不变。',
           'diagnostic_only': True, 'limitations': '按秒采集已知进程名的活动，可能漏过不足一秒的任务和其他负载；perf OFF仍开启应用分段诊断；旧版未加show-lost-events时必须提供离线补查。缺失链按原因保留，不能填零。'}
    (args.directory/'audit.json').write_text(json.dumps(out, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({k: v for k, v in out.items() if k != 'windows'}, ensure_ascii=False))


if __name__ == '__main__': main()
