#!/usr/bin/env python3
"""单应用 1/100/1000 公共话题的真实资源采样。"""
import argparse
import json
import os
import select
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from benchmark import sample
from end_to_end import Process, reserve_ports


def gateway_status(gateway, executable, control, env):
    try:
        return json.loads(subprocess.check_output(
            [executable, 'status', '--control', control, '--json'], env=env, text=True))
    except subprocess.CalledProcessError as error:
        stderr = b''
        if gateway.stderr:
            fd = gateway.stderr.fileno()
            os.set_blocking(fd, False)
            chunks = []
            while select.select([fd], [], [], 0)[0]:
                try:
                    chunk = os.read(fd, 65536)
                except BlockingIOError:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
            stderr = b''.join(chunks)
        if gateway.poll() is None:
            try:
                gateway.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                pass
        if gateway.poll() is not None and gateway.stderr and not stderr:
            stderr = gateway.stderr.read()
        detail = stderr.decode(errors='replace') if isinstance(stderr, bytes) else str(stderr)
        raise AssertionError({'status_returncode': error.returncode,
                              'gateway_returncode': gateway.poll(),
                              'gateway_stderr': detail}) from error


def host(args):
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=4g', 'tmpfs', '/dev/shm'], check=True)
    with tempfile.TemporaryDirectory(prefix='dzipc-scale-') as directory:
        control = directory + '/control.sock'
        env = dict(os.environ, DZIPC_NET_BACKEND='shared_v1', DZIPC_SHM_MPMC='1', DZIPC_SHM_RECV_WORKERS='1', DZIPC_GATEWAY_CONTROL=control)
        bases, discovery = reserve_ports()
        gateway = subprocess.Popen([args.gateway, 'serve', '--control', control, '--listen-ip', '127.0.0.1', '--interface', 'lo',
            '--data-base-port', str(bases[0]), '--control-port', str(bases[0]+4), '--discovery-port', str(discovery)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        process = None
        try:
            end = time.monotonic() + 5
            while not os.path.exists(control):
                assert gateway.poll() is None and time.monotonic() < end
                time.sleep(.01)
            before = sample(gateway.pid)
            started = time.monotonic()
            process = Process([args.binary, 'scale', 'socket', 'scale_', str(args.topics), '/dev/null', '1'], env)
            ready = process.receive(timeout=180)
            assert ready['topics'] == args.topics
            creation = time.monotonic() - started
            stats = gateway_status(gateway, args.gateway, control, env)
            live = {'application': sample(process.p.pid), 'gateway': sample(gateway.pid)}
            memory = os.statvfs('/dev/shm')
            shm_bytes = (memory.f_blocks-memory.f_bfree)*memory.f_frsize
            assert live['application']['udp'] == 0 and live['gateway']['udp'] == 6
            assert stats['logical_publishers'] == args.topics and stats['active_routes'] == args.topics
            process.p.stdin.write('close\n'); process.p.stdin.flush()
            assert process.receive(timeout=180)['closed']
            time.sleep(.1)
            closed = {'application': sample(process.p.pid), 'gateway': sample(gateway.pid)}
            status_closed = gateway_status(gateway, args.gateway, control, env)
            assert status_closed['registered_handles'] == 0 and status_closed['active_routes'] == 0
            print(json.dumps({'topics': args.topics, 'creation_seconds': creation, 'before_gateway': before, 'live': live,
                'closed': closed, 'shm_bytes': shm_bytes, 'status': stats, 'status_closed': status_closed}, ensure_ascii=False))
        finally:
            if process:
                process.close()
            gateway.terminate()
            try:
                gateway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                gateway.kill(); gateway.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--topics', type=int, required=True)
    args = parser.parse_args()
    if args.host:
        host(args)
    else:
        sys.exit(subprocess.call(['unshare', '--user', '--map-root-user', '--mount', '--ipc', sys.executable, __file__, '--host', *sys.argv[1:]], start_new_session=True))
