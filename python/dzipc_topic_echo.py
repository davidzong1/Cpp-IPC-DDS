#!/usr/bin/env python3
"""Minimal topic-echo tool for dzIPC — like ``ros2 topic echo``.

Usage:
    python dzipc_topic_echo.py --topic supervisor_status
    python dzipc_topic_echo.py --topic supervisor_status --domain 1 --freq 10
"""

from __future__ import annotations

import argparse
import json
import sys
import time

try:
    import dzipc as ipc
    from dzipc import dzflat
    from dzipc._dzipc_core import GenericMessage
except Exception as exc:
    print(f"[ERROR] Cannot import dzipc: {exc}", file=sys.stderr)
    sys.exit(1)


def echo_topic(topic: str, domain: int = 1, queue: int = 10, freq: int = 10,
               msg_id: int = 0) -> None:
    # 用 GenericMessage 作模板 —— 通用 echo 不该绑定某个具体类型。
    #
    # 以前这里是 ipc.Supervisor(), 于是只有那一种形状的消息能被"识别"; 而且
    # out.topic() 返回的是 C++ 侧对象, getattr(msg, "additional_info") 根本取不到,
    # 所以旧实现对**所有**类型都只会打印 N/A。GenericMessage 两种 wire 都能承接:
    # TLV 由它自带的字段走查解析, DZFlat 则原样留存段字节交给 dzflat.dump 按 schema
    # 解码(定长布局的 wire 里没有字段名, 只有查 schema 才知道偏移对应哪个字段)。
    msg_template = GenericMessage()
    topic_data = ipc.make_topic_data(msg_template, msg_id)

    sub = ipc.SubscriberIPCPtrMake(
        topic_data,
        topic,
        domain,
        queue,
        ipc.IPC_SHM,
        verbose=False,
    )
    sub.InitChannel()

    period = 1.0 / max(freq, 1)
    print(f"[ECHO] listening on topic='{topic}' domain={domain} freq={freq}Hz\n")
    try:
        while True:
            ok, out = sub.try_get_clone(topic_data)
            if not ok:
                time.sleep(period)
                continue

            msg = out.topic() if out is not None else topic_data.topic()

            # 通用字段转储 —— 同一份代码覆盖 TLV 与 DZFlat 两种 wire。
            #
            # 以前这里只打印 update_time / additional_info 两个字段, 也就是只能echo
            # 那一种消息形状; 其他类型看到的永远是 "N/A"。改用 dzflat.dump 之后, 任何
            # 由 generator 产出的类型都能完整显示, 而 DZFlat 消息尤其需要它 —— 定长
            # 布局的 wire 里没有字段名, 只有查 schema 才知道每个偏移是什么。
            print("---")
            print(dzflat.dump(msg))

            # 兼容旧用法: 这两个字段若存在, 仍单独美化一遍
            additional_info = getattr(msg, "additional_info", None)
            if additional_info:
                try:
                    print("additional_info(parsed):\n"
                          + json.dumps(json.loads(additional_info), indent=2,
                                       ensure_ascii=False))
                except (json.JSONDecodeError, TypeError):
                    pass
    except KeyboardInterrupt:
        print("\n[ECHO] done.")


def main() -> None:
    parser = argparse.ArgumentParser(description="dzIPC topic echo")
    parser.add_argument("--topic", required=True, help="Topic name")
    parser.add_argument("--domain", type=int, default=1)
    parser.add_argument("--queue", type=int, default=10)
    parser.add_argument("--freq", type=int, default=10, help="Poll frequency in Hz")
    parser.add_argument("--msg-id", type=int, default=0,
                        help="话题的 msg_id, 须与发布端一致(不匹配则一条都收不到)")
    args = parser.parse_args()
    echo_topic(args.topic, args.domain, args.queue, args.freq, args.msg_id)


if __name__ == "__main__":
    main()
