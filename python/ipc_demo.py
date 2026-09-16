#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time
from typing import List

try:
    import dzipc as ipc
except Exception as exc:
    print("[ERROR] Cannot import dzipc：", exc)
    print("[HINT] Please first build the pybind module and add the directory where the .so file is located to the PYTHONPATH environment variable")
    sys.exit(1)


def pick_ipc_type(transport: str):
    return ipc.IPC_SHM if transport == "shm" else ipc.IPC_SOCKET


def run_pub(args: argparse.Namespace) -> None:
    msg_template = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg_template)
    pub = ipc.PublisherIPCPtrMake(
        topic_data,
        args.topic,
        args.domain,
        pick_ipc_type(args.transport),
        args.verbose,
    )
    pub.InitChannel(args.extra)

    for i in range(args.count):
        msg = ipc.TestMsg()
        msg.data1 = [1.1, 2.2, float(i)]
        msg.data2 = [i, i + 1, i + 2]
        msg.data3 = ["hello", "python", f"seq={i}"]
        if i == args.count - 1:
            msg.data3.append("exit")
        msg.data4 = (i % 2 == 0)

        # publish 收的是 C++ 侧消息对象, 不是 Python 封装类 —— 必须显式 to_generic()。
        # (注意与 make_topic_data 的不对称: 那个会自动转换, 这个不会。直接传封装对象
        #  会得到一条 pybind 的 "Invoked with: ..." 类型错误。)
        ok = pub.publish(msg.to_generic())
        print(f"[PUB] seq={i}, ok={ok}, data3={msg.data3}")
        time.sleep(args.period)


def run_sub(args: argparse.Namespace) -> None:
    # 用模板消息构建 TopicData（与 C++ 用法一致）
    msg_template = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg_template)
    sub = ipc.SubscriberIPCPtrMake(
        topic_data,
        args.topic,
        args.domain,
        args.queue,
        pick_ipc_type(args.transport),
        args.verbose,
    )
    sub.InitChannel(args.extra)

    print("[SUB] 订阅已启动，Ctrl+C 退出")
    try:
        while True:
            ok, out = sub.try_get_clone(topic_data)
            if not ok:
                time.sleep(args.poll)
                continue

            # topic() 返回的是 **C++ 侧对象**，字段不是 Python 属性 —— 必须用生成的
            # 封装类把它转成 Python 对象再读。
            #
            # 原先这里是 `if not hasattr(msg_obj, "data3"): msg_obj = msg_template`
            # 加一串 getattr(..., 默认值)：hasattr 永远为假，于是**每次都退回读模板对象**
            # (空值)，而 getattr 的默认值又把这个错误吞掉 —— 打印出来的一直是空列表,
            # 收不到 exit 也永远不退出。见 docs/dzflat_known_issues.md 第 2 条。
            #
            # from_generic 两种 wire 都认(TLV 与 DZFlat)。
            msg_obj = out.topic() if out is not None else topic_data.topic()
            msg = ipc.TestMsg.from_generic(msg_obj)

            print(
                "[SUB] recv:",
                {
                    "data1": list(msg.data1),
                    "data2": list(msg.data2),
                    "data3": list(msg.data3),
                    "data4": bool(msg.data4),
                },
            )

            if "exit" in list(msg.data3):
                print("[SUB] 收到 exit，退出")
                return
    except KeyboardInterrupt:
        print("\n[SUB] 已退出")


def run_server(args: argparse.Namespace) -> None:
    req_template = ipc.RequestResponseTestRequest()
    res_template = ipc.RequestResponseTestResponse()
    service_data = ipc.make_service_data(req_template, res_template)

    def callback(data):
        req = data.request()
        res = data.response()

        if not hasattr(req, "request"):
            req = req_template
        if not hasattr(res, "response"):
            res = res_template

        req_vec = list(req.request)
        res.response = [x + 1.0 for x in req_vec]
        print(f"[SRV] req={req_vec} -> res={list(res.response)}")

    srv = ipc.ServerIPCPtrMake(
        args.topic,
        service_data,
        callback,
        args.domain,
        pick_ipc_type(args.transport),
        args.verbose,
    )
    srv.InitChannel(args.extra)

    print("[SRV] 服务已启动，Ctrl+C 退出")
    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        print("\n[SRV] 已退出")


def run_client(args: argparse.Namespace) -> None:
    req_template = ipc.RequestResponseTestRequest()
    res_template = ipc.RequestResponseTestResponse()
    service_data = ipc.make_service_data(req_template, res_template)

    cli = ipc.ClientIPCPtrMake(
        args.topic,
        service_data,
        args.domain,
        pick_ipc_type(args.transport),
        args.verbose,
    )
    cli.InitChannel(args.extra)

    for i in range(args.count):
        payload: List[float] = [float(x) for x in range(i + 1)]

        req = service_data.request()
        if not hasattr(req, "request"):
            req = req_template
        req.request = payload

        ok = cli.send_request(service_data, args.timeout)

        res = service_data.response()
        if not hasattr(res, "response"):
            res = res_template

        print(f"[CLI] ok={ok}, req={payload}, res={list(res.response)}")
        time.sleep(args.period)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="dzIPC Python 通信例程")
    sub = parser.add_subparsers(dest="mode", required=True)

    def add_common(p: argparse.ArgumentParser):
        p.add_argument("--topic", required=True, help="topic/service 名")
        p.add_argument("--domain", type=int, default=1)
        p.add_argument("--transport", choices=["shm", "socket"], default="socket")
        p.add_argument("--extra", default="", help="InitChannel(extra_info)")
        p.add_argument("--verbose", action="store_true")

    p_pub = sub.add_parser("pub", help="发布者")
    add_common(p_pub)
    p_pub.add_argument("--count", type=int, default=10)
    p_pub.add_argument("--period", type=float, default=0.2)

    p_sub = sub.add_parser("sub", help="订阅者")
    add_common(p_sub)
    p_sub.add_argument("--queue", type=int, default=10)
    p_sub.add_argument("--poll", type=float, default=0.05)

    p_srv = sub.add_parser("server", help="服务端")
    add_common(p_srv)

    p_cli = sub.add_parser("client", help="客户端")
    add_common(p_cli)
    p_cli.add_argument("--count", type=int, default=5)
    p_cli.add_argument("--period", type=float, default=0.2)
    p_cli.add_argument("--timeout", type=int, default=2_000_000_000, help="send_request rev_tm")

    return parser


def main() -> None:
    args = build_parser().parse_args()
    if args.mode == "pub":
        run_pub(args)
    elif args.mode == "sub":
        run_sub(args)
    elif args.mode == "server":
        run_server(args)
    elif args.mode == "client":
        run_client(args)
    else:
        raise ValueError(f"unknown mode: {args.mode}")


if __name__ == "__main__":
    main()
