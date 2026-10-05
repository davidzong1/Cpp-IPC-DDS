#!/usr/bin/env python3
"""在调用者独占的 IPC/mount 命名空间验证 Python 公共工厂。"""
import argparse
import gc
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--gateway', required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))
    with tempfile.TemporaryDirectory(prefix='dzipc-python-') as directory:
        control = directory + '/control.sock'
        os.environ.update(DZIPC_NET_BACKEND='shared_v1', DZIPC_SHM_MPMC='1',
                          DZIPC_GATEWAY_CONTROL=control)
        gateway = subprocess.Popen([args.gateway, 'serve', '--control', control,
                                    '--listen-ip', '127.0.0.1', '--interface', 'lo'],
                                   stdout=subprocess.DEVNULL)
        try:
            end = time.monotonic() + 5
            while not os.path.exists(control):
                assert gateway.poll() is None and time.monotonic() < end
                time.sleep(.01)
            import dzipc as ipc
            from dzipc import dzflat
            ipc.EnableDzFlat(True)
            td = ipc.make_topic_data(ipc.GenericMessage(), 71)
            sub = ipc.SubscriberIPCPtrMake(td, 'shared_python_' + str(os.getpid()),
                                          0, 8, ipc.IPC_SOCKET, False)
            sub.InitChannel()
            pub = ipc.PublisherIPCPtrMake(td, 'shared_python_' + str(os.getpid()),
                                         0, ipc.IPC_SOCKET, False)
            pub.InitChannel()
            message = ipc.StdImage()
            message.width = 32
            message.height = 8
            message.data = [i % 256 for i in range(256)]
            segment = dzflat.pack(message, msg_id=71)
            assert segment is not None and pub.publish_prebuilt_segment(segment)

            def receive():
                end = time.monotonic() + 3
                while time.monotonic() < end:
                    ok, out = sub.try_get_clone(td)
                    if ok:
                        return out.topic()
                    time.sleep(.001)
                raise AssertionError('Python 接收超时')

            received = receive()
            assert received.has_dzflat()
            assert bytes(received.dzflat_bytes()) == bytes(segment)
            assert not sub.try_get_clone(td)[0]
            assert not pub.publish_prebuilt_segment(bytes(48))
            ipc.EnableDzFlat(False)
            generic = message.to_generic()
            generic.set_msg_id(71)
            assert pub.publish(generic)
            received = receive()
            assert not received.has_dzflat()
            assert not sub.try_get_clone(td)[0]
            del pub, sub
            gc.collect()
            print(json.dumps({'python_cases': 2, 'passed': 2,
                              'prebuilt_bytes': len(segment)}))
        finally:
            gateway.terminate()
            try:
                gateway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait()


if __name__ == '__main__':
    main()
