#!/usr/bin/env python3
"""两套真实网关/应用进程，共享网络、隔离 /dev/shm；不冒充物理跨机测试。"""
import argparse
import json
import os
import pathlib
import queue
import random
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


class Process:
    def __init__(self, args, env=None, group=False):
        self.group = group
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1, env=env,
                                  start_new_session=group)
        self.lines = queue.Queue()
        self.errors = []
        def read():
            for line in self.p.stdout:
                if line.startswith('{'):
                    try:
                        self.lines.put(json.loads(line))
                    except json.JSONDecodeError:
                        self.errors.append(line)
            self.lines.put(None)
        def errors():
            for line in self.p.stderr:
                self.errors.append(line)
        threading.Thread(target=read, daemon=True).start()
        threading.Thread(target=errors, daemon=True).start()

    def receive(self, timeout=15):
        try:
            result = self.lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f'进程超时：{self.p.args}；{self.errors[-10:]}')
        if result is None:
            raise AssertionError(f'进程提前退出：{self.p.poll()}；{self.errors[-10:]}')
        if 'error' in result:
            raise AssertionError(result)
        return result

    def request(self, command):
        self.p.stdin.write(command + '\n')
        self.p.stdin.flush()
        return self.receive()

    def close(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM) if self.group else self.p.terminate()
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL) if self.group else self.p.kill()
                self.p.wait()


def udp_count(pid):
    inodes = set()
    for entry in pathlib.Path(f'/proc/{pid}/fd').iterdir():
        try:
            target = os.readlink(entry)
            if target.startswith('socket:['):
                inodes.add(target[8:-1])
        except FileNotFoundError:
            pass
    found = set()
    for name in ('udp', 'udp6'):
        for line in pathlib.Path(f'/proc/{pid}/net/{name}').read_text().splitlines()[1:]:
            fields = line.split()
            if fields[9] in inodes:
                found.add(fields[9])
    return len(found)


def host(args):
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=768m', 'tmpfs', '/dev/shm'], check=True)
    env = dict(os.environ, DZIPC_SHM_MPMC='1')
    control = args.control
    gateway_args = [args.gateway, 'serve', '--listen-ip', '127.0.0.1',
        '--interface', 'lo', '--control', control]
    if args.network_version == 1:
        gateway_args += ['--data-base-port', str(args.base), '--data-shards', str(args.data_shards)]
    else:
        gateway_args += ['--network-version', '2', '--data-mode', args.data_mode,
                         '--data-port-range', args.data_port_range,
                         '--data-workers', str(args.data_workers)]
        if args.data_mode != 'per-topic':
            gateway_args += ['--data-sockets', str(args.data_sockets)]
        if args.topic_policy_file:
            gateway_args += ['--topic-policy-file', args.topic_policy_file]
    if args.socket_buffer_budget_bytes is not None:
        gateway_args += ['--socket-buffer-budget-bytes', str(args.socket_buffer_budget_bytes)]
    port_blocker = None
    if args.block_first_dedicated_port:
        first_dedicated = args.base if args.data_mode == 'per-topic' else args.base + args.data_sockets
        port_blocker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        port_blocker.bind(('127.0.0.1', first_dedicated))
    gateway = subprocess.Popen(gateway_args + [
        '--control-port', str(args.control_port), '--discovery-port', str(args.discovery)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    probes = []
    try:
        end = time.monotonic() + 5
        while not os.path.exists(control):
            if gateway.poll() is not None:
                raise AssertionError(gateway.stderr.read().decode())
            assert time.monotonic() < end, '网关启动超时'
            time.sleep(.01)
        roles = (args.lifecycle_roles.split(',') if args.lifecycle_roles else
                 ['both'] + ['sub'] * (args.local_subscribers - 1) + ['pub'] * (args.local_publishers - 1))
        if not roles or any(role not in ('pub', 'sub', 'both') for role in roles):
            raise AssertionError('生命周期角色只接受 pub、sub 或 both')
        for role in roles:
            probe = Process([args.probe, control, args.topic, role], env)
            probes.append(probe)
            if args.expect_endpoint_failure:
                try:
                    probe.receive()
                except AssertionError:
                    if probe.p.poll() is None:
                        probe.p.wait(timeout=5)
                    failure = '\n'.join(probe.errors)
                    assert probe.p.returncode != 0 and 'BufferBudget' in failure, failure
                    break
                raise AssertionError('预期 dedicated 注册因缓冲预算失败，但 probe 已 ready')
            assert probe.receive()['ready']
        print(json.dumps({'ready': True, 'gateway_pid': gateway.pid,
                          'probe_pids': [p.p.pid for p in probes],
                          'registration_failure': args.expect_endpoint_failure}), flush=True)
        for line in sys.stdin:
            command = json.loads(line)
            if command[0] == 'quit':
                break
            if command[0] == 'probe':
                result = probes[command[1]].request(command[2])
            elif command[0] == 'close_probe':
                probe = probes[command[1]]
                probe.p.stdin.write('quit\n'); probe.p.stdin.flush()
                result = {'closed': probe.p.wait(timeout=5) == 0}
            elif command[0] == 'signal':
                gateway.send_signal(command[1])
                result = {'signaled': True}
            elif command[0] == 'metrics':
                result = {category: json.loads(subprocess.check_output([args.gateway, 'status', '--control', args.control, '--metrics', category], env=env, text=True))
                          for category in ('counters', 'quota', 'latency', 'shards')}
                result['application'] = probes[0].request('diagnostics')
            elif command[0] == 'resources':
                result = {'gateway_udp': udp_count(gateway.pid),
                          'application_udp': [udp_count(p.p.pid) for p in probes if p.p.poll() is None]}
            elif command[0] == 'gateway_status':
                result = json.loads(subprocess.check_output(
                    [args.gateway, 'status', '--control', args.control, '--json'], env=env, text=True))
            else:
                raise AssertionError(command)
            print(json.dumps(result), flush=True)
    finally:
        if gateway.poll() is None:
            gateway.send_signal(signal.SIGCONT)
        for probe in probes:
            try:
                if probe.p.poll() is None:
                    probe.p.stdin.write('quit\n')
                    probe.p.stdin.flush()
                    probe.p.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
            probe.close()
        if gateway.poll() is None:
            gateway.terminate()
            try:
                gateway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait()


def reserve_ports(data_shards=4, span=None):
    span = span or data_shards
    rng = random.Random(20261005)
    for _ in range(100):
        bases = [rng.randrange(18000, 31000), rng.randrange(18000, 31000)]
        ports = [p for b in bases for p in range(b, b + span + 1)]
        discovery = rng.randrange(18000, 31000)
        ports.append(discovery)
        if len(set(ports)) != len(ports):
            continue
        sockets = []
        try:
            for port in ports:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sockets.append(s)
                s.bind(('127.0.0.1', port))
            return bases, discovery
        except OSError:
            pass
        finally:
            for s in sockets:
                s.close()
    raise AssertionError('测试无空闲端口组')


def driver(args):
    hosts = []
    evidence = {'environment': '同机独立用户/挂载/IPC 命名空间，真实 UDP 和独立业务 SHM',
                'seed': 20261005, 'deliveries': [], 'resources': []}
    with tempfile.TemporaryDirectory(prefix='dzipc-net-e2e-') as directory:
        span = args.data_shards if args.network_version == 1 else 64
        bases, discovery = reserve_ports(args.data_shards if args.network_version == 1 else args.data_sockets, span)
        policy_file = None
        if args.dedicated_policy:
            policy_file = str(pathlib.Path(directory) / 'topic-policy.json')
            default_endpoint = 'dedicated' if args.data_mode == 'per-topic' else 'pooled'
            pathlib.Path(policy_file).write_text(json.dumps({
                'default': {'endpoint': default_endpoint},
                'routes': ([] if args.data_mode == 'per-topic' else
                           [{'topic': 'shared_net_e2e', 'domain': '0', 'msg_id': 71,
                             'endpoint': 'dedicated'}])}, ensure_ascii=False))
        try:
            for i in range(2):
                os.mkdir(directory + f'/host{i}', 0o700)
                if args.network_version == 1:
                    control_port = bases[i] + args.data_shards
                    data_range = ''
                else:
                    control_port = bases[i] + span
                    data_range = f'{bases[i]}:{bases[i] + span - 1}'
                command = ['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                    sys.executable, __file__, '--host', '--gateway', args.gateway,
                    '--probe', args.probe, '--control', directory + f'/host{i}/control.sock',
                    '--base', str(bases[i]), '--control-port', str(control_port),
                    '--discovery', str(discovery), '--topic', 'shared_net_e2e',
                    '--local-subscribers', str(args.local_subscribers),
                    '--network-version', str(args.network_version), '--data-mode', args.data_mode,
                    '--data-workers', str(args.data_workers)]
                if args.lifecycle_roles:
                    command += ['--lifecycle-roles', args.lifecycle_roles]
                if args.block_first_dedicated_port:
                    command += ['--block-first-dedicated-port']
                if args.expect_endpoint_failure:
                    command += ['--expect-endpoint-failure']
                if args.socket_buffer_budget_bytes is not None:
                    command += ['--socket-buffer-budget-bytes', str(args.socket_buffer_budget_bytes)]
                if args.network_version == 1:
                    command += ['--data-shards', str(args.data_shards)]
                else:
                    command += ['--data-port-range', data_range]
                    if args.data_mode != 'per-topic':
                        command += ['--data-sockets', str(args.data_sockets)]
                if policy_file:
                    command += ['--topic-policy-file', policy_file]
                hosts.append(Process(command, group=True))
            for host_process in hosts:
                ready = host_process.receive()
            def request(index, command):
                return hosts[index].request(json.dumps(command))
            if args.expect_endpoint_failure:
                for host_index in range(2):
                    status = request(host_index, ['gateway_status'])
                    resources = request(host_index, ['resources'])
                    assert status['network_version'] == 2 and status['data_mode'] == args.data_mode, status
                    assert not status['routes'] and status['registered_handles'] == 0, status
                    assert status['data_sockets'] == 0 and status['endpoint_states']['ready'] == 0, status
                    assert status['resource_budget']['reserved_ports'] == 2, status
                    assert status['last_endpoint_failure_code'] != 0 and 'BufferBudget' in status['last_endpoint_failure'], status
                    assert resources['gateway_udp'] == 2 and resources['application_udp'] == [], resources
                    evidence.setdefault('registration_failures', []).append({
                        'code': status['last_endpoint_failure_code'],
                        'reason': status['last_endpoint_failure'],
                        'data_sockets_after_rollback': status['data_sockets']})
                assert ready['registration_failure']
                print(json.dumps(evidence, ensure_ascii=False))
                return
            if args.lifecycle_roles:
                if args.network_version != 2 or not (args.dedicated_policy or args.data_mode == 'per-topic'):
                    raise AssertionError('生命周期角色场景要求 v2 dedicated 端点')
                roles = args.lifecycle_roles.split(',')
                pubs = sum(role in ('pub', 'both') for role in roles)
                subs = sum(role in ('sub', 'both') for role in roles)
                def role_refs(role):
                    return int(role in ('pub', 'both')) + int(role in ('sub', 'both'))
                expected_data = (args.data_sockets if args.data_mode != 'per-topic' else 0) + 1
                for host_index in range(2):
                    before_resources = request(host_index, ['resources'])
                    before = request(host_index, ['gateway_status'])
                    assert len(before['routes']) == 1, before
                    route = before['routes'][0]
                    assert route['endpoint'] == 'dedicated' and route['endpoint_refs'] == sum(map(role_refs, roles)), before
                    assert route['publisher_refs'] == pubs and route['subscriber_refs'] == subs, before
                    assert route['ready_subscribers'] == subs, before
                    assert before_resources['gateway_udp'] == expected_data + 2, before_resources
                    for index, role in enumerate(roles):
                        assert request(host_index, ['close_probe', index])['closed']
                        status = request(host_index, ['gateway_status'])
                        resources = request(host_index, ['resources'])
                        remaining = roles[index + 1:]
                        if remaining:
                            current = status['routes'][0]
                            assert current['endpoint_refs'] == sum(map(role_refs, remaining)), status
                            assert current['publisher_refs'] == sum(r in ('pub', 'both') for r in remaining), status
                            assert current['subscriber_refs'] == sum(r in ('sub', 'both') for r in remaining), status
                            assert resources['gateway_udp'] == before_resources['gateway_udp'], resources
                        else:
                            assert not status['routes'], status
                            assert resources['gateway_udp'] == before_resources['gateway_udp'] - 1, resources
                            assert status['endpoint_states']['ready'] == (args.data_sockets if args.data_mode != 'per-topic' else 0), status
                evidence['dedicated_role_matrix'] = {'roles': roles, 'publisher_refs': pubs, 'subscriber_refs': subs}
                print(json.dumps(evidence, ensure_ascii=False))
                return
            deadline = time.monotonic() + 8
            while True:
                states = [request(i, ['probe', 0, 'state']) for i in range(2)]
                if all(s['remote'] == 1 and s['synchronized'] for s in states):
                    break
                assert time.monotonic() < deadline, states
                time.sleep(.05)
            for i in range(2):
                resources = request(i, ['resources'])
                dedicated = args.dedicated_policy or args.data_mode == 'per-topic'
                expected_data = (args.data_shards if args.network_version == 1 else
                                 (args.data_sockets if args.data_mode != 'per-topic' else 0))
                if dedicated:
                    expected_data += 1
                assert resources['gateway_udp'] == expected_data + 2, resources
                assert resources['application_udp'] == [0] * args.local_subscribers, resources
                evidence['resources'].append(resources)
                status = request(i, ['gateway_status'])
                assert status['network_version'] == args.network_version, status
                assert len(status['routes']) == 1 and status['routes'][0]['publisher_refs'] == 1 and status['routes'][0]['subscriber_refs'] == args.local_subscribers, status
                if args.network_version == 2:
                    assert status['routes'][0]['data_port'] > 0 and status['routes'][0]['endpoint_epoch'] != '0', status
                    if args.dedicated_policy or args.data_mode == 'per-topic':
                        assert status['routes'][0]['endpoint'] == 'dedicated', status
                        assert status['routes'][0]['endpoint_flags'] == 1, status
                        if args.block_first_dedicated_port:
                            blocked = bases[i] if args.data_mode == 'per-topic' else bases[i] + args.data_sockets
                            assert status['routes'][0]['data_port'] == blocked + 1, status
                    else:
                        assert status['routes'][0]['endpoint'] == 'pooled', status
                        assert status['routes'][0]['endpoint_flags'] == 0, status
            expected_remote = [0, 0]
            for seed, size in enumerate((64, 1023, 1024, 1025, 4096, 1048576), 1):
                for sender in range(2):
                    sent = request(sender, ['probe', 0, f'send {size} {sender + 1} {seed} {1 if args.reliable else 0}'])
                    assert sent['success'] and sent['local'] == 2 and sent['network'] == 2, sent
                    if args.reliable:
                        assert sent['result'] == 0, sent
                    for receiver in range(2):
                        for app in range(args.local_subscribers):
                            result = request(receiver, ['probe', app, 'recv 1 3000'])['received']
                            assert len(result) == 1 and result[0]['valid'], result
                            assert (result[0]['tag'], result[0]['seed'], result[0]['size'], result[0]['crc']) == (sender + 1, seed, size, sent['crc']), result
                    expected_remote[1 - sender] += 1
                    evidence['deliveries'].append({'source': sender, 'size': size, 'crc': sent['crc']})
            for host_index in range(2):
                status = request(host_index, ['probe', 0, 'status'])
                assert status['source_injections'] == 0 and status['committed_messages'] == expected_remote[host_index], status
                for app in range(args.local_subscribers):
                    assert not request(host_index, ['probe', app, 'recv 1 30'])['received'], '出现重复或回流'
            evidence['metrics'] = [request(i, ['metrics']) for i in range(2)]
            if args.reliable:
                for metrics in evidence['metrics']:
                    assert metrics['counters']['reliable_completed'] == 6 and metrics['counters']['acks_rx'] == 6, metrics
                    assert metrics['counters']['acks_tx'] >= 6 and metrics['latency']['remote_commit']['count'] >= 6, metrics
            if args.network_version == 2 and (args.dedicated_policy or args.data_mode == 'per-topic'):
                # 先撤销额外 SUB。PUB+SUB 仍存活时，专用端点必须保持并只减少一个引用。
                for host_index in range(2):
                    before = request(host_index, ['resources'])['gateway_udp']
                    before_status = request(host_index, ['gateway_status'])
                    assert before_status['routes'][0]['endpoint_refs'] == args.local_subscribers + 1, before_status
                    assert request(host_index, ['close_probe', 1])['closed']
                    middle = request(host_index, ['resources'])['gateway_udp']
                    middle_status = request(host_index, ['gateway_status'])
                    detail = {'before_udp': before, 'middle_udp': middle,
                              'before_routes': before_status['routes'],
                              'middle_routes': middle_status['routes'],
                              'middle_endpoint_states': middle_status['endpoint_states']}
                    assert middle == before, detail
                    assert len(middle_status['routes']) == 1
                    assert middle_status['routes'][0]['endpoint_refs'] == 2, middle_status
                    assert middle_status['routes'][0]['publisher_refs'] == 1
                    assert middle_status['routes'][0]['subscriber_refs'] == 1
                evidence['dedicated_subscriber_reclamation'] = True
            request(0, ['signal', signal.SIGSTOP])
            # 等待客户端判定网络离线，既有本机发布者和两个订阅进程仍应工作。
            time.sleep(3.3)
            sent = request(0, ['probe', 0, 'send 4096 9 99'])
            assert sent['success'] and sent['local'] == 2 and sent['network'] == 1, sent
            active_subscribers = args.local_subscribers - 1 if args.network_version == 2 and (args.dedicated_policy or args.data_mode == 'per-topic') else args.local_subscribers
            for app in range(active_subscribers):
                result = request(0, ['probe', app, 'recv 1 500'])['received']
                assert len(result) == 1 and result[0]['valid'] and result[0]['tag'] == 9, result
            assert not request(1, ['probe', 0, 'recv 1 50'])['received']
            request(0, ['signal', signal.SIGCONT])
            if args.network_version == 2 and (args.dedicated_policy or args.data_mode == 'per-topic'):
                reclamation = []
                for host_index in range(2):
                    before = request(host_index, ['resources'])['gateway_udp']
                    before_status = request(host_index, ['gateway_status'])
                    route_active = bool(before_status['routes'])
                    assert request(host_index, ['close_probe', 0])['closed']
                    after = request(host_index, ['resources'])['gateway_udp']
                    after_status = request(host_index, ['gateway_status'])
                    if route_active:
                        assert after == before - 1, (before, after, before_status, after_status)
                        reclamation.append('last-role-unregister')
                    else:
                        # A paused gateway can time out its control session and reclaim the route first.
                        pooled_sockets = args.data_sockets if args.data_mode != 'per-topic' else 0
                        assert before == after == pooled_sockets + 2, (before, after, before_status, after_status)
                        reclamation.append('session-timeout')
                    assert not after_status['routes']
                    assert after_status['endpoint_states']['ready'] == (args.data_sockets if args.data_mode != 'per-topic' else 0), after_status
                evidence['dedicated_role_reclamation'] = reclamation
            evidence['gateway_pause_local_delivery'] = True
            print(json.dumps(evidence, ensure_ascii=False))
        finally:
            for process in hosts:
                try:
                    if process.p.poll() is None:
                        process.p.stdin.write('["quit"]\n')
                        process.p.stdin.flush()
                        process.p.wait(timeout=12)
                except (OSError, subprocess.TimeoutExpired):
                    pass
                process.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--probe', required=True)
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--reliable', action='store_true')
    parser.add_argument('--local-subscribers', type=int, default=2)
    parser.add_argument('--local-publishers', type=int, default=1)
    parser.add_argument('--network-version', type=int, choices=(1, 2), default=1)
    parser.add_argument('--data-mode', choices=('pooled', 'hybrid', 'per-topic'), default='pooled')
    parser.add_argument('--data-sockets', type=int, default=4)
    parser.add_argument('--data-workers', type=int, default=4)
    parser.add_argument('--data-port-range', default='')
    parser.add_argument('--data-shards', type=int, default=4)
    parser.add_argument('--control-port', type=int)
    parser.add_argument('--topic-policy-file')
    parser.add_argument('--dedicated-policy', action='store_true')
    parser.add_argument('--lifecycle-roles', default='')
    parser.add_argument('--block-first-dedicated-port', action='store_true')
    parser.add_argument('--expect-endpoint-failure', action='store_true')
    parser.add_argument('--socket-buffer-budget-bytes', type=int)
    parser.add_argument('--control')
    parser.add_argument('--topic')
    parser.add_argument('--base', type=int)
    parser.add_argument('--discovery', type=int)
    options = parser.parse_args()
    try:
        host(options) if options.host else driver(options)
    except Exception as error:
        print(json.dumps({'error': str(error)}, ensure_ascii=False), flush=True)
        raise
