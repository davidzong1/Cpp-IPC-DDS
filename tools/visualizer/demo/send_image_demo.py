#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import importlib.machinery
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

ROOT_DIR = Path(__file__).resolve().parents[3]
DEMO_DIR = Path(__file__).resolve().parent
DEFAULT_IMAGE = DEMO_DIR / "images" / "DepthFigure.jpeg"


def compatible_dzipc_roots() -> list[Path]:
    candidates = [
        ROOT_DIR / "local" / "lib" / "python",
        ROOT_DIR / "python",
    ]
    roots: list[Path] = []
    for root in candidates:
        package_dir = root / "dzipc"
        if not package_dir.is_dir():
            continue
        if any(
            (package_dir / f"_dzipc_core{suffix}").exists()
            for suffix in importlib.machinery.EXTENSION_SUFFIXES
        ):
            roots.append(root)
    return roots


def add_dzipc_paths() -> None:
    candidate_roots = [
        ROOT_DIR / "local" / "lib" / "python",
        ROOT_DIR / "python",
    ]
    candidate_strings = {str(path) for path in candidate_roots}
    sys.path[:] = [path for path in sys.path if path not in candidate_strings]

    roots = compatible_dzipc_roots()
    if not roots:
        roots = [root for root in candidate_roots if (root / "dzipc").is_dir()]
    for root in reversed(roots):
        sys.path.insert(0, str(root))


def preload_local_libipc() -> None:
    local_libipc = ROOT_DIR / "local" / "lib" / "libipc.so"
    if local_libipc.exists():
        ctypes.CDLL(str(local_libipc), mode=ctypes.RTLD_GLOBAL)


def load_dzipc():
    add_dzipc_paths()
    preload_local_libipc()
    try:
        import dzipc as ipc  # type: ignore
    except Exception as exc:
        print(f"[image-demo] ERROR: cannot import dzipc: {exc}")
        print(f"[image-demo] Python executable: {sys.executable}")
        print(f"[image-demo] Python version: {sys.version.split()[0]}")
        print("[image-demo] Compatible dzipc search paths:")
        for root in compatible_dzipc_roots():
            print(f"  {root}")
        print(
            "[image-demo] Build/install the Python package first, then rerun this demo."
        )
        print("[image-demo] Example:")
        print("  ./scripts/install.sh")
        print("  python3 tool/visualizer/demo/send_image_demo.py")
        sys.exit(1)
    return ipc


def pick_ipc_type(ipc, transport: str):
    return ipc.IPC_SHM if transport == "shm" else ipc.IPC_SOCKET


def jpeg_size(data: bytes) -> Optional[Tuple[int, int]]:
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        return None
    i = 2
    sof_markers = {
        0xC0,
        0xC1,
        0xC2,
        0xC3,
        0xC5,
        0xC6,
        0xC7,
        0xC9,
        0xCA,
        0xCB,
        0xCD,
        0xCE,
        0xCF,
    }
    while i + 4 < len(data):
        while i < len(data) and data[i] == 0xFF:
            i += 1
        if i >= len(data):
            return None
        marker = data[i]
        i += 1
        if marker in (0xD8, 0xD9):
            continue
        if i + 2 > len(data):
            return None
        seg_len = int.from_bytes(data[i : i + 2], "big")
        if seg_len < 2 or i + seg_len > len(data):
            return None
        if marker in sof_markers and seg_len >= 7:
            height = int.from_bytes(data[i + 3 : i + 5], "big")
            width = int.from_bytes(data[i + 5 : i + 7], "big")
            return width, height
        i += seg_len
    return None


def png_size(data: bytes) -> Optional[Tuple[int, int]]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def infer_compressed_image(
    path: Path, data: bytes
) -> Tuple[str, Optional[Tuple[int, int]]]:
    suffix = path.suffix.lower()
    if suffix in {".jpg", ".jpeg"} or data[:2] == b"\xff\xd8":
        return "jpeg", jpeg_size(data)
    if suffix == ".png" or data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png", png_size(data)
    return "bytes", None


def make_rgb8_gradient(width: int, height: int) -> bytes:
    data = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 3
            data[offset] = int(255 * x / max(1, width - 1))
            data[offset + 1] = int(255 * y / max(1, height - 1))
            data[offset + 2] = 180
    return bytes(data)


def step_for_encoding(width: int, encoding: str, data_len: int) -> int:
    pixel_bytes = {
        "mono8": 1,
        "rgb8": 3,
        "bgr8": 3,
        "rgba8": 4,
        "bgra8": 4,
        "mono16": 2,
        "16uc1": 2,
        "32fc1": 4,
    }.get(encoding.lower())
    return width * pixel_bytes if pixel_bytes is not None else data_len


def build_image_payload(
    args: argparse.Namespace,
) -> Tuple[bytes, int, int, str, int, str]:
    if args.synthetic_rgb8:
        width = args.width or 640
        height = args.height or 360
        data = make_rgb8_gradient(width, height)
        return data, width, height, "rgb8", width * 3, "synthetic rgb8 gradient"

    image_path = Path(args.image).expanduser()
    if not image_path.is_absolute():
        image_path = (ROOT_DIR / image_path).resolve()
    if not image_path.exists():
        width = args.width or 640
        height = args.height or 360
        data = make_rgb8_gradient(width, height)
        return (
            data,
            width,
            height,
            "rgb8",
            width * 3,
            f"synthetic rgb8 gradient; missing {image_path}",
        )
    data = image_path.read_bytes()
    inferred_encoding, inferred_size = infer_compressed_image(image_path, data)
    width = args.width
    height = args.height
    if inferred_size is not None:
        width = width or inferred_size[0]
        height = height or inferred_size[1]
    if not width or not height:
        raise ValueError(
            f"cannot detect image size for {image_path}; pass --width and --height"
        )
    encoding = args.encoding or inferred_encoding
    step = step_for_encoding(width, encoding, len(data))
    return data, width, height, encoding, step, str(image_path)


def print_usage(args: argparse.Namespace) -> None:
    fps = 1.0 / args.period if args.period > 0 else 0.0
    print("[image-demo] Visualizer usage:")
    print("  1. Start the web bridge:")
    print(
        "     python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765"
    )
    print("  2. Open http://127.0.0.1:8765")
    print("  3. Click '+ Add Display' and use:")
    print(f"     Topic     : {args.topic}")
    print("     Type      : Image")
    print(f"     Domain    : {args.domain}")
    print(f"     Transport : {args.transport}")
    print(f"     FPS       : {fps:.1f}")
    if args.extra:
        print(f"     Extra     : {args.extra}")
    print("  4. Run this publisher:")
    print(
        "     python3 tool/visualizer/demo/send_image_demo.py "
        f"--topic {args.topic} --domain-id {args.domain} --transport {args.transport}"
    )
    print()


def publish_images(args: argparse.Namespace) -> None:
    print_usage(args)
    ipc = load_dzipc()
    data, width, height, encoding, step, source = build_image_payload(args)

    template = ipc.StdImage()
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
        "[image-demo] publishing StdImage "
        f"topic={args.topic} size={width}x{height} encoding={encoding} "
        f"bytes={len(data)} source={source}"
    )
    print("[image-demo] press Ctrl+C to stop")

    seq = 0
    data_list = list(data)
    next_publish = time.monotonic()
    try:
        while args.count <= 0 or seq < args.count:
            now = time.monotonic()
            if now < next_publish:
                time.sleep(next_publish - now)
            publish_start = time.monotonic()
            msg = ipc.StdImage()
            msg.header = ipc.StdHeader()
            msg.header.frame_id = args.frame_id
            msg.header.stamp = time.time()
            msg.height = height
            msg.width = width
            msg.encoding = encoding
            msg.step = step
            msg.data = data_list

            ok = pub.publish(msg.to_generic())
            print(
                f"[image-demo] seq={seq} ok={ok} stamp={msg.header.stamp:.6f} "
                f"{width}x{height} {encoding} data={len(data)} bytes",
                flush=True,
            )
            seq += 1
            if args.count > 0 and seq >= args.count:
                break
            next_publish += args.period
            if publish_start - next_publish > args.period:
                next_publish = publish_start + args.period
    except KeyboardInterrupt:
        print("\n[image-demo] stopped")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Publish StdImage demo data for dzIPC visualizer"
    )
    parser.add_argument("--topic", default="/demo/depth_image", help="dzIPC topic name")
    parser.add_argument("--domain", "--domain-id", dest="domain", type=int, default=0)
    parser.add_argument("--transport", choices=["socket", "shm"], default="shm")
    parser.add_argument("--extra", default="", help="InitChannel(extra_info)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--period", type=float, default=1.0 / 60.0, help="publish interval in seconds"
    )
    parser.add_argument(
        "--count", type=int, default=0, help="number of frames; 0 means infinite"
    )
    parser.add_argument("--frame-id", default="demo_camera")
    parser.add_argument("--image", default=str(DEFAULT_IMAGE.relative_to(ROOT_DIR)))
    parser.add_argument(
        "--encoding",
        default="",
        help="override encoding, default infers jpeg/png/bytes",
    )
    parser.add_argument(
        "--width", type=int, default=0, help="override or provide image width"
    )
    parser.add_argument(
        "--height", type=int, default=0, help="override or provide image height"
    )
    parser.add_argument(
        "--synthetic-rgb8",
        action="store_true",
        help="publish a generated raw rgb8 gradient instead of reading --image",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        publish_images(args)
    except Exception as exc:
        print(f"[image-demo] ERROR: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
