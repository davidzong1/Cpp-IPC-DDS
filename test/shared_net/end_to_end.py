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
    gateway = subprocess.Popen([args.gateway, 'serve', '--listen-ip', '127.0.0.1',
        '--interface', 'lo', '--control', control, '--data-base-port', str(args.base),
        '--control-port', str(args.base + 4), '--discovery-port', str(args.discovery)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    probes = []
    try:
        end = time.monotonic() + 5
        while not os.path.exists(control):
            if gateway.poll() is not None:
                raise AssertionError(gateway.stderr.read().decode())
            assert time.monotonic() < end, '网关启动超时'
            time.sleep(.01)
        for role in ['both'] + ['sub'] * (args.local_subscribers - 1) + ['pub'] * (args.local_publishers - 1):
            probe = Process([args.probe, control, args.topic, role], env)
            probes.append(probe)
            assert probe.receive()['ready']
        print(json.dumps({'ready': True, 'gateway_pid': gateway.pid,
                          'probe_pids': [p.p.pid for p in probes]}), flush=True)
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
            elif command[0] == 'resources':
                result = {'gateway_udp': udp_count(gateway.pid),
                          'application_udp': [udp_count(p.p.pid) for p in probes if p.p.poll() is None]}
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


def reserve_ports():
    rng = random.Random(20261005)
    for _ in range(100):
        bases = [rng.randrange(18000, 31000), rng.randrange(18000, 31000)]
        ports = [p for b in bases for p in range(b, b + 5)]
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
        bases, discovery = reserve_ports()
        try:
            for i in range(2):
                os.mkdir(directory + f'/host{i}', 0o700)
                command = ['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                    sys.executable, __file__, '--host', '--gateway', args.gateway,
                    '--probe', args.probe, '--control', directory + f'/host{i}/control.sock',
                    '--base', str(bases[i]), '--discovery', str(discovery), '--topic', 'shared_net_e2e', '--local-subscribers', str(args.local_subscribers)]
                hosts.append(Process(command, group=True))
            for host_process in hosts:
                host_process.receive()
            def request(index, command):
                return hosts[index].request(json.dumps(command))
            deadline = time.monotonic() + 8
            while True:
                states = [request(i, ['probe', 0, 'state']) for i in range(2)]
                if all(s['remote'] == 1 and s['synchronized'] for s in states):
                    break
                assert time.monotonic() < deadline, states
                time.sleep(.05)
            for i in range(2):
                resources = request(i, ['resources'])
                assert resources == {'gateway_udp': 6, 'application_udp': [0] * args.local_subscribers}, resources
                evidence['resources'].append(resources)
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
            request(0, ['signal', signal.SIGSTOP])
            # 等待客户端判定网络离线，既有本机发布者和两个订阅进程仍应工作。
            time.sleep(3.3)
            sent = request(0, ['probe', 0, 'send 4096 9 99'])
            assert sent['success'] and sent['local'] == 2 and sent['network'] == 1, sent
            for app in range(args.local_subscribers):
                result = request(0, ['probe', app, 'recv 1 500'])['received']
                assert len(result) == 1 and result[0]['valid'] and result[0]['tag'] == 9, result
            assert not request(1, ['probe', 0, 'recv 1 50'])['received']
            request(0, ['signal', signal.SIGCONT])
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
