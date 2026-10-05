#!/usr/bin/env python3
"""1/2/8 个远端 locality 的真实单播放大；仍属于同机模拟。"""
import argparse
import json
import os
from pathlib import Path
import random
import socket
import sys
import tempfile
import time
from end_to_end import Process


def ports(count):
    rng = random.Random(20261005 + count)
    for attempt in range(100):
        bases = [18000 + 10 * value for value in rng.sample(range(1300), count)]
        discovery = 32001
        sockets = []
        try:
            for port in [p for base in bases for p in range(base, base+5)] + [discovery]:
                descriptor = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sockets.append(descriptor)
                descriptor.bind(('127.0.0.1', port))
            return bases, discovery
        except OSError:
            pass
        finally:
            for descriptor in sockets:
                descriptor.close()
    raise RuntimeError('没有可用测试端口组')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--probe', required=True)
    parser.add_argument('--targets', type=int, choices=(1, 2, 8), required=True)
    args = parser.parse_args()
    count = args.targets + 1
    bases, discovery = ports(count)
    processes = []
    evidence = []
    with tempfile.TemporaryDirectory(prefix='dzipc-fanout-') as directory:
        try:
            for index in range(count):
                os.mkdir(f'{directory}/host{index}', 0o700)
                process = Process(['unshare', '--user', '--map-root-user', '--mount', '--ipc', sys.executable,
                    str(Path(__file__).with_name('end_to_end.py')), '--host', '--gateway', args.gateway, '--probe', args.probe,
                    '--control', f'{directory}/host{index}/control.sock', '--base', str(bases[index]),
                    '--discovery', str(discovery), '--topic', 'shared_fanout', '--local-subscribers', '1'], group=True)
                processes.append(process)
                process.receive()
            def request(index, command):
                return processes[index].request(json.dumps(command))
            end = time.monotonic() + 12
            while True:
                states = [request(i, ['probe', 0, 'state']) for i in range(count)]
                if all(state['remote'] == args.targets and state['synchronized'] for state in states):
                    break
                assert time.monotonic() < end, states
                time.sleep(.02)
            for size in (64, 4096, 1048576):
                before = request(0, ['probe', 0, 'status'])
                start = time.monotonic_ns()
                sent = request(0, ['probe', 0, f'send {size} 1 91 1'])
                elapsed = time.monotonic_ns()-start
                assert sent['success'], sent
                for target in range(count):
                    received = request(target, ['probe', 0, 'recv 1 3000'])['received']
                    assert len(received) == 1 and received[0]['valid'] and received[0]['crc'] == sent['crc'], received
                after = request(0, ['probe', 0, 'status'])
                packets = after['sent_packets']-before['sent_packets']
                retries = after['retry_packets']-before['retry_packets']
                expected = ((size+1023)//1024)*args.targets
                assert packets-retries == expected, (packets,retries,expected)
                evidence.append({'wire_bytes': size, 'targets': args.targets, 'data_packets': packets,
                    'retry_packets': retries, 'data_wire_bytes': after['data_bytes_tx']-before['data_bytes_tx'],
                    'reliable_completion_with_driver_ns': elapsed})
            for target in range(count):
                status = request(target, ['probe', 0, 'status'])
                assert status['committed_messages'] == (0 if target == 0 else 3)
                assert status['source_injections'] == 0
                assert request(target, ['resources'])['gateway_udp'] == 6
            print(json.dumps({'environment': '同机隔离 locality，非物理跨机', 'cases': evidence}, ensure_ascii=False))
        finally:
            for process in processes:
                process.close()


if __name__ == '__main__':
    main()
