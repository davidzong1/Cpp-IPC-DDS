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
except Exception as exc:
    print(f"[ERROR] Cannot import dzipc: {exc}", file=sys.stderr)
    sys.exit(1)


def echo_topic(topic: str, domain: int = 1, queue: int = 10, freq: int = 10) -> None:
    # 构造模板消息，与 ipc_demo.py 中 run_sub 完全一致
    msg_template = ipc.Supervisor()
    topic_data = ipc.make_topic_data(msg_template)

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
            ok, out = sub.try_get(topic_data)
            if not ok:
                time.sleep(period)
                continue

            msg = out.topic() if out is not None else topic_data.topic()

            # 打印核心字段
            update_time = getattr(msg, "update_time", "N/A")
            additional_info = getattr(msg, "additional_info", "{}")

            # 尝试把 additional_info 解析为 JSON 以美化显示
            try:
                info_parsed = json.loads(additional_info)
                info_str = json.dumps(info_parsed, indent=2, ensure_ascii=False)
            except (json.JSONDecodeError, TypeError):
                info_str = additional_info

            print("---")
            print(f"update_time: {update_time}")
            print(f"additional_info:\n{info_str}")
    except KeyboardInterrupt:
        print("\n[ECHO] done.")


def main() -> None:
    parser = argparse.ArgumentParser(description="dzIPC topic echo")
    parser.add_argument("--topic", required=True, help="Topic name")
    parser.add_argument("--domain", type=int, default=1)
    parser.add_argument("--queue", type=int, default=10)
    parser.add_argument("--freq", type=int, default=10, help="Poll frequency in Hz")
    args = parser.parse_args()
    echo_topic(args.topic, args.domain, args.queue, args.freq)


if __name__ == "__main__":
    main()
