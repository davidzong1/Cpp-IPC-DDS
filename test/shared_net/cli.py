#!/usr/bin/env python3
"""只创建自有控制目录/端口，验证 CLI 退出码与 JSON。"""
import argparse
import json
import os
import subprocess
import tempfile
import time
from end_to_end import reserve_ports


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--gateway', required=True)
    args = parser.parse_args()
    cases = []
    def run(arguments, expected):
        result = subprocess.run([args.gateway, *arguments], capture_output=True, text=True, timeout=5)
        assert result.returncode == expected, (arguments, result.returncode, result.stderr)
        cases.append({'arguments': arguments, 'exit': result.returncode})
        return result
    with tempfile.TemporaryDirectory(prefix='dzipc-cli-') as directory:
        control = directory + '/control.sock'
        bases, discovery = reserve_ports()
        options = ['--control', control, '--listen-ip', '127.0.0.1', '--interface', 'lo',
                   '--data-base-port', str(bases[0]), '--control-port', str(bases[0] + 4), '--discovery-port', str(discovery)]
        run(['--help'], 0)
        run(['unknown'], 2)
        run(['check-config', '--bad', 'x'], 2)
        run(['status', '--control', control, '--domain', '-1'], 2)
        run(['status', '--control', control], 3)
        run(['check-config', *options], 0)
        gateway = subprocess.Popen([args.gateway, 'serve', *options], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            end = time.monotonic() + 5
            while not os.path.exists(control):
                assert gateway.poll() is None and time.monotonic() < end
                time.sleep(.005)
            status = json.loads(run(['status', '--control', control, '--json'], 0).stdout)
            assert status['udp_sockets'] == 6 and len(status['socket_buffers']) == 6
            assert all(b['receive_bytes'] > 0 and b['send_bytes'] > 0 for b in status['socket_buffers'])
            query = json.loads(run(['status', '--control', control, '--topic', 'absent', '--domain', '18446744073709551615', '--msg-id', '71'], 0).stdout)
            assert query['domain'] == '18446744073709551615' and not query['ready']
            run(['serve', *options], 3)
            os.mkdir(directory + '/second')
            collision = options.copy()
            collision[1] = directory + '/second/control.sock'
            run(['serve', *collision], 3)
            # 内核只读目录，不依赖 root 能否绕过 chmod。
            unwritable = options.copy()
            unwritable[1] = '/proc/dzipc-cli-test/control.sock'
            run(['serve', *unwritable], 3)
        finally:
            gateway.terminate()
            gateway.wait(timeout=5)
    print(json.dumps({'passed': len(cases), 'cases': cases}, ensure_ascii=False))


if __name__ == '__main__':
    main()
