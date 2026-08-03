#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

from send_image_demo import ROOT_DIR, load_dzipc, pick_ipc_type


DEMO_DIR = Path(__file__).resolve().parent
DEFAULT_ROBOT_DIR = DEMO_DIR / "robots"
DEFAULT_CONFIG = DEMO_DIR / "robot_state_config.json"
DEFAULT_URDF = DEFAULT_ROBOT_DIR / "tiny_2dof_arm.urdf"
DEFAULT_URDF_TOPIC = "/demo/robot_urdf"

DOWNLOAD_CANDIDATES = [
    {
        "url": "https://raw.githubusercontent.com/ros/urdf_tutorial/ros2/urdf/06-flexible.urdf",
        "name": "urdf_tutorial_06_flexible.urdf",
        "source": "ros/urdf_tutorial 06-flexible.urdf",
    },
    {
        "url": "https://raw.githubusercontent.com/ros/urdf_tutorial/master/urdf/06-flexible.urdf",
        "name": "urdf_tutorial_06_flexible.urdf",
        "source": "ros/urdf_tutorial 06-flexible.urdf",
    },
]

FALLBACK_URDF = """<?xml version="1.0"?>
<robot name="tiny_2dof_arm">
  <link name="base_link">
    <visual>
      <origin xyz="0 0 0.04"/>
      <geometry><box size="0.42 0.32 0.08"/></geometry>
      <material name="base"><color rgba="0.18 0.23 0.27 1"/></material>
    </visual>
  </link>
  <link name="shoulder_link">
    <visual>
      <origin xyz="0.35 0 0"/>
      <geometry><box size="0.70 0.10 0.10"/></geometry>
      <material name="link_a"><color rgba="0.12 0.52 0.72 1"/></material>
    </visual>
  </link>
  <link name="forearm_link">
    <visual>
      <origin xyz="0.25 0 0"/>
      <geometry><box size="0.50 0.08 0.08"/></geometry>
      <material name="link_b"><color rgba="0.90 0.55 0.14 1"/></material>
    </visual>
  </link>
  <link name="tool0">
    <visual>
      <origin xyz="0.08 0 0"/>
      <geometry><box size="0.16 0.06 0.06"/></geometry>
      <material name="tool"><color rgba="0.30 0.72 0.36 1"/></material>
    </visual>
  </link>
  <joint name="shoulder_pan" type="revolute">
    <parent link="base_link"/>
    <child link="shoulder_link"/>
    <origin xyz="0 0 0.12" rpy="0 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-2.6" upper="2.6" effort="10" velocity="1.5"/>
  </joint>
  <joint name="elbow_lift" type="revolute">
    <parent link="shoulder_link"/>
    <child link="forearm_link"/>
    <origin xyz="0.70 0 0" rpy="0 0 0"/>
    <axis xyz="0 1 0"/>
    <limit lower="-1.8" upper="1.8" effort="10" velocity="1.5"/>
  </joint>
  <joint name="tool_fixed" type="fixed">
    <parent link="forearm_link"/>
    <child link="tool0"/>
    <origin xyz="0.50 0 0" rpy="0 0 0"/>
  </joint>
</robot>
"""


@dataclass(frozen=True)
class JointSpec:
    name: str
    kind: str
    lower: float
    upper: float


@dataclass(frozen=True)
class RobotAsset:
    path: Path
    text: str
    source: str


@dataclass(frozen=True)
class JointWave:
    spec: JointSpec
    center: float
    amplitude: float
    frequency: float
    phase: float

    def value(self, elapsed_s: float) -> float:
        value = self.center + self.amplitude * math.sin(
            self.frequency * elapsed_s + self.phase
        )
        return max(self.spec.lower, min(self.spec.upper, value))


def resolve_path(path: Path | str) -> Path:
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = (ROOT_DIR / resolved).resolve()
    return resolved


def path_for_config(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT_DIR).as_posix()
    except ValueError:
        return str(path.resolve())


def iter_urdf_files(root: Path) -> Iterable[Path]:
    skip_dirs = {".git", "__pycache__", "build", "cmake-build-debug"}
    for current, dirs, files in root.walk():
        dirs[:] = [
            name
            for name in dirs
            if name not in skip_dirs and not name.startswith(".")
        ]
        for name in files:
            if name.lower().endswith(".urdf"):
                yield current / name


def looks_like_urdf(text: str) -> bool:
    if "<robot" not in text:
        return False
    try:
        ET.fromstring(text)
    except ET.ParseError:
        return False
    return True


def first_existing_urdf(args: argparse.Namespace) -> Optional[RobotAsset]:
    if args.urdf:
        path = resolve_path(args.urdf)
        if not path.exists():
            raise FileNotFoundError(f"URDF path does not exist: {path}")
        text = path.read_text(encoding="utf-8")
        if not looks_like_urdf(text):
            raise ValueError(f"not a valid URDF XML file: {path}")
        return RobotAsset(path=path, text=text, source="--urdf")

    for root in (resolve_path(args.resource_dir), ROOT_DIR):
        if not root.exists():
            continue
        for path in iter_urdf_files(root):
            text = path.read_text(encoding="utf-8", errors="replace")
            if looks_like_urdf(text):
                return RobotAsset(path=path, text=text, source="existing repository URDF")
    return None


def download_urdf(args: argparse.Namespace) -> Optional[RobotAsset]:
    if args.no_download:
        return None
    target_dir = resolve_path(args.resource_dir)
    target_dir.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []
    for candidate in DOWNLOAD_CANDIDATES:
        target = target_dir / candidate["name"]
        try:
            request = urllib.request.Request(
                candidate["url"],
                headers={"User-Agent": "dzipc-visualizer-robot-state-demo"},
            )
            with urllib.request.urlopen(request, timeout=args.download_timeout) as response:
                text = response.read().decode("utf-8")
            if not looks_like_urdf(text):
                raise ValueError("downloaded content is not URDF XML")
            target.write_text(text, encoding="utf-8")
            return RobotAsset(path=target, text=text, source=candidate["source"])
        except (OSError, UnicodeDecodeError, urllib.error.URLError, ValueError) as exc:
            errors.append(f"{candidate['url']}: {exc}")
    if errors:
        print("[robot-state-demo] URDF download failed; using offline fallback")
        for error in errors:
            print(f"[robot-state-demo]   {error}")
    return None


def fallback_urdf(args: argparse.Namespace) -> RobotAsset:
    target_dir = resolve_path(args.resource_dir)
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / DEFAULT_URDF.name
    target.write_text(FALLBACK_URDF, encoding="utf-8")
    return RobotAsset(path=target, text=FALLBACK_URDF, source="offline fallback")


def prepare_robot_asset(args: argparse.Namespace) -> RobotAsset:
    if not args.force_download:
        existing = first_existing_urdf(args)
        if existing is not None:
            return existing
    downloaded = download_urdf(args)
    if downloaded is not None:
        return downloaded
    return fallback_urdf(args)


def parse_robot_name(urdf_text: str, fallback: str) -> str:
    try:
        root = ET.fromstring(urdf_text)
    except ET.ParseError:
        return fallback
    return root.attrib.get("name") or fallback


def parse_joint_specs(urdf_text: str) -> list[JointSpec]:
    root = ET.fromstring(urdf_text)
    joints: list[JointSpec] = []
    for joint in root.findall("joint"):
        kind = joint.attrib.get("type", "fixed")
        if kind == "fixed":
            continue
        name = joint.attrib.get("name", "").strip()
        if not name:
            continue
        lower = -math.pi
        upper = math.pi
        if kind == "prismatic":
            lower, upper = -0.25, 0.25
        limit = joint.find("limit")
        if limit is not None:
            try:
                lower = float(limit.attrib.get("lower", lower))
                upper = float(limit.attrib.get("upper", upper))
            except ValueError:
                lower, upper = -math.pi, math.pi
        if kind == "continuous" or not math.isfinite(lower) or not math.isfinite(upper):
            lower, upper = -math.pi, math.pi
        if upper <= lower:
            lower, upper = -math.pi, math.pi
        joints.append(JointSpec(name=name, kind=kind, lower=lower, upper=upper))
    return joints


def make_waves(joints: list[JointSpec], seed: int) -> list[JointWave]:
    rng = random.Random(seed)
    waves: list[JointWave] = []
    for index, joint in enumerate(joints):
        span = joint.upper - joint.lower
        center = (joint.lower + joint.upper) * 0.5
        amplitude = min(span * 0.42, 1.15 if joint.kind != "prismatic" else 0.18)
        waves.append(
            JointWave(
                spec=joint,
                center=center,
                amplitude=amplitude,
                frequency=rng.uniform(0.35, 0.95) * (1.0 + index * 0.08),
                phase=rng.uniform(0.0, math.tau),
            )
        )
    return waves


def joint_positions(waves: list[JointWave], elapsed_s: float) -> list[float]:
    return [wave.value(elapsed_s) for wave in waves]


def base_pose(elapsed_s: float) -> tuple[float, float, float]:
    return (
        0.10 * math.sin(0.23 * elapsed_s),
        0.08 * math.sin(0.19 * elapsed_s + 0.7),
        0.0,
    )


def build_config(args: argparse.Namespace, asset: RobotAsset, robot_name: str) -> dict:
    poll = min(max(args.period, 0.005), 0.25)
    topic_config = {
        "topic": args.topic,
        "msg_type": "RobotState",
        "domain": args.domain,
        "queue": args.queue,
        "transport": args.transport,
        "poll": poll,
        "extra": args.extra,
        "verbose": bool(args.verbose),
    }
    urdf_topic_config = {
        "topic": args.urdf_topic,
        "msg_type": "RawMessage",
        "domain": args.domain,
        "queue": args.queue,
        "transport": args.transport,
        "poll": poll,
        "extra": args.extra,
        "verbose": bool(args.verbose),
    }
    return {
        "domain": args.domain,
        "transport": args.transport,
        "queue": args.queue,
        "poll": poll,
        "extra": args.extra,
        "verbose": bool(args.verbose),
        "topics": [topic_config, urdf_topic_config],
        "robot_displays": [
            {
                "id": args.robot_id,
                "name": robot_name,
                "urdf_path": path_for_config(asset.path),
                "urdf": asset.text,
                "fixed_frame": args.frame_id,
                "visible": True,
            }
        ],
        "robot_state_displays": [
            {
                "id": args.state_id,
                "name": args.state_id,
                "topic": args.topic,
                "robot_id": args.robot_id,
                "target_robot_id": args.robot_id,
                "msg_type": "RobotState",
                "domain": args.domain,
                "queue": args.queue,
                "transport": args.transport,
                "poll": poll,
                "extra": args.extra,
                "visible": True,
            }
        ],
    }


def write_config(
    args: argparse.Namespace, asset: RobotAsset, robot_name: str
) -> Optional[Path]:
    if args.no_config:
        return None
    path = resolve_path(args.config_out)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(build_config(args, asset, robot_name), indent=2, ensure_ascii=False)
        + "\n",
        encoding="utf-8",
    )
    return path


def make_pose(ipc, x: float, y: float, z: float, frame_id: str):
    pose = ipc.Pose()
    pose.x = float(x)
    pose.y = float(y)
    pose.z = float(z)
    pose.frame_id = frame_id
    return pose


def build_robot_state(
    ipc,
    args: argparse.Namespace,
    seq: int,
    elapsed_s: float,
    waves: list[JointWave],
    history: list,
):
    positions = joint_positions(waves, elapsed_s)
    x, y, z = base_pose(elapsed_s)
    pose = make_pose(ipc, x, y, z, args.frame_id)
    history.append(pose)
    del history[: max(0, len(history) - args.history)]

    msg = ipc.RobotState()
    msg.name = args.robot_id
    msg.current_pose = pose
    msg.pose_history = list(history)
    msg.note = json.dumps(
        {
            "seq": seq,
            "stamp": time.time(),
            "joint_state": {
                "name": [wave.spec.name for wave in waves],
                "position": positions,
            },
            "base_position": [x, y, z],
            "base_orientation": [0.0, 0.0, 0.0, 1.0],
            "frame_id": args.frame_id,
        },
        separators=(",", ":"),
    )
    return msg


def build_urdf_message(ipc, asset: RobotAsset):
    msg = ipc.StdRawMessage()
    msg.format = "urdf+xml"
    msg.payload = asset.text
    return msg


def print_usage(
    args: argparse.Namespace,
    asset: RobotAsset,
    config_path: Optional[Path],
    joints: list[JointSpec],
) -> None:
    fps = 1.0 / args.period if args.period > 0 else 0.0
    print("[robot-state-demo] Visualizer usage:")
    print("  1. Start the web bridge with the generated config:")
    if config_path is not None:
        print(
            "     python3 tool/visualizer/dzipc_web_bridge.py "
            f"--host 127.0.0.1 --port 8765 --config {path_for_config(config_path)} --load-config"
        )
    else:
        print(
            "     python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765"
        )
    print("  2. Open http://127.0.0.1:8765")
    print("  3. Run this publisher:")
    print(
        "     python3 tool/visualizer/demo/send_robot_state.py "
        f"--topic {args.topic} --domain-id {args.domain} --transport {args.transport}"
    )
    print()
    print(
        "[robot-state-demo] robot asset "
        f"path={path_for_config(asset.path)} source={asset.source} joints={len(joints)} fps={fps:.1f}"
    )
    print(f"[robot-state-demo] URDF side topic: {args.urdf_topic} (RawMessage)")
    if config_path is not None:
        print(f"[robot-state-demo] config written to {path_for_config(config_path)}")


def publish_robot_states(args: argparse.Namespace) -> None:
    if args.period <= 0:
        raise ValueError("--period must be positive")
    args.report_every = max(1, args.report_every)
    asset = prepare_robot_asset(args)
    robot_name = parse_robot_name(asset.text, args.robot_id)
    joints = parse_joint_specs(asset.text)
    if not joints:
        print("[robot-state-demo] WARNING: URDF has no movable joints; publishing empty joint_state")
    config_path = write_config(args, asset, robot_name)
    print_usage(args, asset, config_path, joints)
    if args.prepare_only:
        return

    ipc = load_dzipc()
    waves = make_waves(joints, args.seed)
    template = ipc.RobotState()
    topic_data = ipc.make_topic_data(template)
    pub = ipc.PublisherIPCPtrMake(
        topic_data,
        args.topic,
        args.domain,
        pick_ipc_type(ipc, args.transport),
        args.verbose,
    )
    pub.InitChannel(args.extra)
    urdf_topic_data = ipc.make_topic_data(ipc.StdRawMessage())
    urdf_pub = ipc.PublisherIPCPtrMake(
        urdf_topic_data,
        args.urdf_topic,
        args.domain,
        pick_ipc_type(ipc, args.transport),
        args.verbose,
    )
    urdf_pub.InitChannel(args.extra)

    print(
        "[robot-state-demo] publishing RobotState "
        f"topic={args.topic} urdf_topic={args.urdf_topic} "
        f"robot_id={args.robot_id} transport={args.transport}"
    )
    print("[robot-state-demo] press Ctrl+C to stop")

    seq = 0
    history: list = []
    start_time = time.monotonic()
    next_publish = start_time
    try:
        while args.count <= 0 or seq < args.count:
            now = time.monotonic()
            if now < next_publish:
                time.sleep(next_publish - now)
            publish_start = time.monotonic()
            elapsed_s = publish_start - start_time
            msg = build_robot_state(ipc, args, seq, elapsed_s, waves, history)
            generic = msg.to_generic()
            if args.publish_mode == "best-effort":
                ok = pub.publish_best_effort(generic)
            else:
                ok = pub.publish(generic)
            urdf_ok = True
            if seq % max(1, args.urdf_every) == 0:
                urdf_msg = build_urdf_message(ipc, asset)
                if args.publish_mode == "best-effort":
                    urdf_ok = urdf_pub.publish_best_effort(urdf_msg.to_generic())
                else:
                    urdf_ok = urdf_pub.publish(urdf_msg.to_generic())
            if seq % args.report_every == 0:
                note = json.loads(msg.note)
                positions = note["joint_state"]["position"]
                sample = ", ".join(f"{value:.3f}" for value in positions[:4])
                if len(positions) > 4:
                    sample += ", ..."
                actual_fps = (seq + 1) / max(1e-6, time.monotonic() - start_time)
                publish_ms = (time.monotonic() - publish_start) * 1000.0
                print(
                    f"[robot-state-demo] seq={seq} ok={ok} urdf_ok={urdf_ok} fps={actual_fps:.1f} "
                    f"publish_ms={publish_ms:.1f} joints=[{sample}]",
                    flush=True,
                )
            seq += 1
            if args.count > 0 and seq >= args.count:
                break
            next_publish += args.period
            if publish_start - next_publish > args.period:
                next_publish = publish_start + args.period
    except KeyboardInterrupt:
        print("\n[robot-state-demo] stopped")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Publish a continuous RobotState trajectory and a matching visualizer Robot config"
    )
    parser.add_argument("--topic", default="/demo/robot_state", help="dzIPC topic name")
    parser.add_argument("--urdf-topic", default=DEFAULT_URDF_TOPIC, help="RawMessage topic carrying URDF XML")
    parser.add_argument("--domain", "--domain-id", dest="domain", type=int, default=0)
    parser.add_argument("--transport", choices=["socket", "shm"], default="shm")
    parser.add_argument(
        "--queue", type=int, default=10, help="subscriber queue size for generated config"
    )
    parser.add_argument("--extra", default="", help="InitChannel(extra_info)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--period", type=float, default=0.03, help="publish interval in seconds")
    parser.add_argument("--count", type=int, default=0, help="number of samples; 0 means infinite")
    parser.add_argument("--report-every", "--report", dest="report_every", type=int, default=30)
    parser.add_argument(
        "--publish-mode", choices=["normal", "best-effort"], default="best-effort"
    )
    parser.add_argument("--frame-id", default="world")
    parser.add_argument("--robot-id", default="demo_robot")
    parser.add_argument("--state-id", default="demo_robot_state")
    parser.add_argument(
        "--history", type=int, default=120, help="pose history samples carried in RobotState"
    )
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--urdf-every", type=int, default=20, help="republish URDF every N RobotState samples")
    parser.add_argument("--urdf", default="", help="explicit URDF file path")
    parser.add_argument("--resource-dir", default=str(DEFAULT_ROBOT_DIR.relative_to(ROOT_DIR)))
    parser.add_argument(
        "--force-download", action="store_true", help="skip existing URDF scan and try GitHub first"
    )
    parser.add_argument(
        "--no-download", action="store_true", help="use existing or fallback URDF without network"
    )
    parser.add_argument("--download-timeout", type=float, default=4.0)
    parser.add_argument("--config-out", default=str(DEFAULT_CONFIG.relative_to(ROOT_DIR)))
    parser.add_argument("--no-config", action="store_true", help="do not write visualizer config")
    parser.add_argument("--prepare-only", action="store_true", help="prepare URDF/config and exit")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        publish_robot_states(args)
    except Exception as exc:
        print(f"[robot-state-demo] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
