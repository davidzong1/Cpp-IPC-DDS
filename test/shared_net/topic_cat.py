#!/usr/bin/env python3
"""安装产物也可复用：真实公共订阅，分别读取本机与隔离远端消息。"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from end_to_end import Process, reserve_ports, udp_count


def host(args):
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=768m', 'tmpfs', '/dev/shm'], check=True)
    env = dict(os.environ, DZIPC_NET_BACKEND='shared_v1', DZIPC_SHM_MPMC='1', DZIPC_GATEWAY_CONTROL=args.control)
    gateway = subprocess.Popen([args.gateway, 'serve', '--listen-ip', '127.0.0.1', '--interface', 'lo',
        '--control', args.control, '--data-base-port', str(args.base), '--control-port', str(args.base + 4),
        '--discovery-port', str(args.discovery)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    publisher = cat = None
    try:
        end = time.monotonic() + 5
        while not os.path.exists(args.control):
            assert gateway.poll() is None and time.monotonic() < end
            time.sleep(.01)
        publisher = Process([args.probe, args.control, 'cat_test', 'pub'], env)
        publisher.receive()
        print(json.dumps({'ready': True}), flush=True)
        for line in sys.stdin:
            command = json.loads(line)
            if command[0] == 'quit':
                break
            if command[0] == 'cat':
                cat = Process([args.cat, '-t', 'cat_test', '-s', 'false', '-m', '71', '--once', '--timeout-ms', '10000'], env)
                end = time.monotonic() + 5
                while not any('订阅已 Ready' in line for line in cat.errors):
                    assert cat.p.poll() is None and time.monotonic() < end, cat.errors
                    time.sleep(.005)
                listing = subprocess.check_output([args.list, '-t', 'cat_test'], text=True, env=env)
                assert listing.count('mode=shared_v1') == 2, listing
                result = {'cat_udp': udp_count(cat.p.pid), 'listing': listing}
            elif command[0] == 'read':
                result = cat.receive()
                assert cat.p.wait(timeout=5) == 0
                cat.close()
                cat = None
            elif command[0] == 'probe':
                result = publisher.request(command[1])
            else:
                raise AssertionError(command)
            print(json.dumps(result), flush=True)
    finally:
        if cat:
            cat.close()
        if publisher:
            publisher.close()
        gateway.terminate()
        try:
            gateway.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gateway.kill()
            gateway.wait()


def driver(args):
    evidence = []
    with tempfile.TemporaryDirectory(prefix='dzipc-cat-') as directory:
        bases, discovery = reserve_ports()
        hosts = []
        try:
            for index in range(2):
                os.mkdir(f'{directory}/host{index}', 0o700)
                hosts.append(Process(['unshare', '--user', '--map-root-user', '--mount', '--ipc',
                    sys.executable, __file__, '--host', '--gateway', args.gateway, '--probe', args.probe,
                    '--cat', args.cat, '--list', args.list, '--control', f'{directory}/host{index}/control.sock',
                    '--base', str(bases[index]), '--discovery', str(discovery)], group=True))
                hosts[-1].receive()
            def request(index, command):
                return hosts[index].request(json.dumps(command))
            for target in (0, 1):
                resources = request(target, ['cat'])
                assert resources['cat_udp'] == 0
                if target == 1:
                    end = time.monotonic() + 8
                    while request(0, ['probe', 'state'])['remote'] != 1:
                        assert time.monotonic() < end
                        time.sleep(.02)
                sent = request(0, ['probe', f'send 4096 {target + 1} 73'])
                assert sent['success'], sent
                received = request(target, ['read'])
                assert received['crc32c'] == sent['crc'] and received['bytes'] == 4096
                assert len(bytes.fromhex(received['segment_hex'])) == 4096
                evidence.append({'target': '本机' if target == 0 else '隔离远端', 'bytes': 4096, 'cat_udp': 0})
        finally:
            for item in hosts:
                item.close()
    print(json.dumps({'passed': 2, 'deliveries': evidence}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for option in ('gateway', 'probe', 'cat', 'list'):
        parser.add_argument('--' + option, required=True)
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--control')
    parser.add_argument('--base', type=int)
    parser.add_argument('--discovery', type=int)
    args = parser.parse_args()
    host(args) if args.host else driver(args)
