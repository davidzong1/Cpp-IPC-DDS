#!/usr/bin/env python3
from __future__ import annotations

import argparse
from ctypes import c_double
import sys
import time
from typing import List
import threading
try:
    import dzipc as ipc
except Exception as exc:
    print("[ERROR] Cannot import dzipc：", exc)
    print("[HINT] Please first build the pybind module and add the directory where the .so file is located to the PYTHONPATH environment variable")
    sys.exit(1)
def pub_thread():
    msg_template = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg_template)
    pub = ipc.PublisherIPCPtrMake(
        topic_data,
        "test_python_api",
        0,
        ipc.IPC_SOCKET,
        True,
    )
    pub.InitChannel()
    count:int=0
    print("Start publishing...")

    while(True):
        msg = ipc.TestMsg()
        msg.data1 = [float(i) for i in range(1000)]  # 大数据：1000 个double
        msg.data2 = []
        msg.data3 = []
        msg.data4 = True

        ok = pub.publish(msg)
        count+=1
        if count%10==0:
            print(f"[PUB] Published: data1={msg.data1}, data2={msg.data2}, data3={msg.data3}, data4={msg.data4}")
        time.sleep(0.1)
        
if __name__ == "__main__":
    # 启动发布线程
    threading.Thread(target=pub_thread, daemon=True).start()

    # 主线程订阅消息
    msg_template = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg_template)
    sub = ipc.SubscriberIPCPtrMake(
        topic_data,
        "test_python_api",
        0,
        10,
        ipc.IPC_SOCKET,
        True,
    )
    sub.InitChannel()

    print("Start subscribing...")
    print("please running ")
    recv_count: int = 0
    poll_count: int = 0
    # 注意：不要用阻塞型 sub.get()。当前 pybind 绑定没有 py::gil_scoped_release，
    # 阻塞调用会一直持有 GIL，同进程里的 pub_thread 永远拿不到 GIL，造成整个进程死锁。
    # 改用非阻塞 try_get() + time.sleep()（time.sleep 会释放 GIL，让发布线程得以运行）。
    while True:
        topic_data = ipc.make_topic_data(ipc.TestMsg())
        ok, topic_data = sub.try_get(topic_data)
        poll_count += 1
        if not ok:
            # 没收到就让出 GIL，给发布线程跑的机会
            time.sleep(0.01)
            if poll_count % 500 == 0:
                print(f"[SUB] still waiting... polls={poll_count}, recv={recv_count}")
            continue
        msg = topic_data.topic()
        recv_count += 1
        if recv_count % 10 == 0:
            print(f"[SUB] Received#{recv_count}: data1={msg.data1}, data2={msg.data2}, "
                  f"data3={msg.data3}, data4={msg.data4}")