#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import random
import time

from send_image_demo import load_dzipc, pick_ipc_type


def print_usage(args: argparse.Namespace) -> None:
    fps = 1.0 / args.period if args.period > 0 else 0.0
    print("[pointcloud-demo] Visualizer usage:")
    print("  1. Start the web bridge:")
    print(
        "     python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765"
    )
    print("  2. Open http://127.0.0.1:8765")
    print("  3. Click '+ Add Display' and use:")
    print(f"     Topic     : {args.topic}")
    print("     Type      : PointCloud")
    print(f"     Domain    : {args.domain}")
    print(f"     Transport : {args.transport}")
    print(f"     FPS       : {fps:.1f}")
    print(f"     Points    : {args.points}")
    if args.extra:
        print(f"     Extra     : {args.extra}")
    print("  4. Run this publisher:")
    print(
        "     python3 tool/visualizer/demo/send_point_cloud_demo.py "
        f"--topic {args.topic} --points {args.points} "
        f"--domain-id {args.domain} --transport {args.transport} "
        f"--colors {args.colors}"
    )
    print()


def height_color(ipc, z: float, z_min: float, z_max: float):
    color = ipc.StdColor()
    norm = max(0.0, min(1.0, (z - z_min) / max(1e-6, z_max - z_min)))
    color.r = 0.15 + 0.85 * norm
    color.g = 0.35 + 0.45 * (1.0 - abs(norm - 0.5) * 2.0)
    color.b = 1.0 - 0.75 * norm
    color.a = 1.0
    return color


def random_color(ipc, rng: random.Random):
    color = ipc.StdColor()
    color.r = rng.random()
    color.g = rng.random()
    color.b = rng.random()
    color.a = 1.0
    return color


def random_cube_point(rng: random.Random, span: float, center_x: float):
    return (
        center_x + rng.uniform(-span, span),
        rng.uniform(-span, span),
        rng.uniform(-span * 0.35, span * 0.35),
    )


def random_sphere_point(rng: random.Random, span: float, center_x: float):
    theta = rng.uniform(0.0, math.tau)
    phi = math.acos(rng.uniform(-1.0, 1.0))
    radius = span * (rng.random() ** (1.0 / 3.0))
    return (
        center_x + radius * math.sin(phi) * math.cos(theta),
        radius * math.sin(phi) * math.sin(theta),
        radius * math.cos(phi),
    )


def random_wall_point(rng: random.Random, span: float, center_x: float):
    wall = rng.randrange(4)
    if wall == 0:
        return center_x - span, rng.uniform(-span, span), rng.uniform(-1.0, span * 0.8)
    if wall == 1:
        return center_x + span, rng.uniform(-span, span), rng.uniform(-1.0, span * 0.8)
    if wall == 2:
        return center_x + rng.uniform(-span, span), -span, rng.uniform(-1.0, span * 0.8)
    return center_x + rng.uniform(-span, span), span, rng.uniform(-1.0, span * 0.8)


def build_point_cloud(ipc, args: argparse.Namespace, seq: int):
    seed = random.randrange(1 << 30) if args.seed == 0 else args.seed + seq
    rng = random.Random(seed)
    msg = ipc.StdPointCloud()
    msg.header = ipc.StdHeader()
    msg.header.frame_id = args.frame_id
    msg.header.stamp = time.time()
    msg.channel_names = []
    msg.channels = []

    center_x = seq * args.drift
    mode_fn = {
        "cube": random_cube_point,
        "sphere": random_sphere_point,
        "walls": random_wall_point,
    }[args.mode]
    points = []
    colors = []
    for _ in range(args.points):
        x, y, z = mode_fn(rng, args.range, center_x)
        point = ipc.StdVector3d()
        point.data = [x, y, z]
        points.append(point)
        if args.colors == "height":
            colors.append(height_color(ipc, z, args.z_min, args.z_max))
        elif args.colors == "random":
            colors.append(random_color(ipc, rng))
    msg.points = points
    msg.colors = colors
    msg.channel_names = ["height"] if args.colors == "height" else []
    return msg


def publish_point_clouds(args: argparse.Namespace) -> None:
    print_usage(args)
    ipc = load_dzipc()
    args.report_every = max(1, args.report_every)

    template = ipc.StdPointCloud()
    topic_data = ipc.make_topic_data(template)
    pub = ipc.PublisherIPCPtrMake(
        topic_data,
        args.topic,
        args.domain,
        pick_ipc_type(ipc, args.transport),
        args.verbose,
    )
    pub.InitChannel(args.extra)

    print(
        "[pointcloud-demo] publishing StdPointCloud "
        f"topic={args.topic} points={args.points} mode={args.mode} "
        f"colors={args.colors} publish_mode={args.publish_mode} "
        f"range={args.range} drift={args.drift}"
    )
    print("[pointcloud-demo] press Ctrl+C to stop")

    seq = 0
    start_time = time.monotonic()
    next_publish = time.monotonic()
    try:
        while args.count <= 0 or seq < args.count:
            now = time.monotonic()
            if now < next_publish:
                time.sleep(next_publish - now)
            publish_start = time.monotonic()
            msg = build_point_cloud(ipc, args, seq)
            generic = msg.to_generic()
            if args.publish_mode == "best-effort":
                ok = pub.publish_best_effort(generic)
            else:
                ok = pub.publish(generic)
            elapsed = time.monotonic() - start_time
            if seq % args.report_every == 0:
                actual_fps = (seq + 1) / max(1e-6, elapsed)
                publish_ms = (time.monotonic() - publish_start) * 1000.0
                print(
                    f"[pointcloud-demo] seq={seq} ok={ok} fps={actual_fps:.1f} "
                    f"publish_ms={publish_ms:.1f} stamp={msg.header.stamp:.6f} "
                    f"points={len(msg.points)} colors={len(msg.colors)}",
                    flush=True,
                )
            seq += 1
            if args.count > 0 and seq >= args.count:
                break
            next_publish += args.period
            if publish_start - next_publish > args.period:
                next_publish = publish_start + args.period
    except KeyboardInterrupt:
        print("\n[pointcloud-demo] stopped")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Publish random StdPointCloud data for dzIPC visualizer stress tests"
    )
    parser.add_argument("--topic", default="/demo/random_point_cloud")
    parser.add_argument("--domain", "--domain-id", dest="domain", type=int, default=0)
    parser.add_argument("--transport", choices=["socket", "shm"], default="shm")
    parser.add_argument("--extra", default="", help="InitChannel(extra_info)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--period", type=float, default=0.05)
    parser.add_argument("--count", type=int, default=0, help="0 means infinite")
    parser.add_argument("--points", type=int, default=100000)
    parser.add_argument("--range", type=float, default=20.0)
    parser.add_argument("--z-min", type=float, default=-2.0)
    parser.add_argument("--z-max", type=float, default=4.0)
    parser.add_argument("--drift", type=float, default=0.08)
    parser.add_argument("--seed", type=int, default=1, help="0 means nondeterministic")
    parser.add_argument("--frame-id", default="map")
    parser.add_argument("--mode", choices=["cube", "sphere", "walls"], default="walls")
    parser.add_argument("--colors", choices=["none", "height", "random"], default="height")
    parser.add_argument(
        "--publish-mode", choices=["normal", "best-effort"], default="best-effort"
    )
    parser.add_argument("--report-every", type=int, default=30)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        publish_point_clouds(args)
    except Exception as exc:
        print(f"[pointcloud-demo] ERROR: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
