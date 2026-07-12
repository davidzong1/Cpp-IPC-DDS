from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple


def _jsonable(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): _jsonable(val) for key, val in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    return value


def normalize_robot_models(raw: Any) -> List[Dict[str, Any]]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise ValueError("robot_models must be an array")
    models: List[Dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ValueError("robot_models entries must be objects")
        urdf = str(item.get("urdf") or item.get("urdf_text") or item.get("urdfText") or "").strip()
        if not urdf:
            continue
        name = str(item.get("name") or item.get("file_name") or f"robot_{index + 1}").strip()
        model_id = str(item.get("id") or name or f"robot_{index + 1}").strip()
        models.append(
            {
                **{str(key): _jsonable(value) for key, value in item.items()},
                "id": model_id,
                "name": name or model_id,
                "file_name": str(item.get("file_name") or item.get("filename") or "").strip(),
                "urdf": urdf,
                "visible": item.get("visible") is not False,
            }
        )
    return models


def robot_model_to_display(model: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "display_type": "Robot",
        "id": str(model.get("id") or model.get("name") or "robot").strip(),
        "name": str(model.get("name") or model.get("id") or "robot").strip(),
        "urdf_path": str(model.get("file_name") or model.get("urdf_path") or "").strip(),
        "urdf": str(model.get("urdf") or "").strip(),
        "visible": model.get("visible") is not False,
        "fixed_frame": str(model.get("fixed_frame") or "world"),
    }


def normalize_robot_displays(raw: Any) -> List[Dict[str, Any]]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise ValueError("robot_displays must be an array")
    displays: List[Dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ValueError("robot_displays entries must be objects")
        source_fields = [str(key) for key in item.keys()]
        display = robot_model_to_display(item)
        display["id"] = str(item.get("id") or display["id"] or f"robot_{index + 1}").strip()
        display["name"] = str(item.get("name") or display["name"] or display["id"]).strip()
        display["urdf_path"] = str(
            item.get("urdf_path")
            or item.get("urdfPath")
            or item.get("file_name")
            or item.get("filename")
            or display["urdf_path"]
            or ""
        ).strip()
        display["visible"] = item.get("visible") is not False
        if "fixed_frame" in item:
            display["fixed_frame"] = str(item.get("fixed_frame") or "world")
        display["_source_fields"] = source_fields
        displays.append(display)
    return displays


def normalize_robot_state_displays(raw: Any) -> List[Dict[str, Any]]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise ValueError("robot_state_displays must be an array")
    displays: List[Dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ValueError("robot_state_displays entries must be objects")
        source_fields = [str(key) for key in item.keys()]
        name = str(item.get("name") or item.get("topic") or f"robot_state_{index + 1}").strip()
        robot_id = str(
            item.get("robot_id")
            or item.get("target_robot_id")
            or item.get("robot_display")
            or item.get("robotDisplay")
            or ""
        ).strip()
        displays.append(
            {
                **{str(key): _jsonable(value) for key, value in item.items()},
                "display_type": "RobotState",
                "id": str(item.get("id") or name).strip(),
                "name": name,
                "topic": str(item.get("topic") or name).strip(),
                "robot_id": robot_id,
                "target_robot_id": robot_id,
                "msg_type": str(item.get("msg_type") or "RobotState").strip(),
                "domain": int(item.get("domain", 0)),
                "queue": int(item.get("queue", 10)),
                "transport": str(item.get("transport") or "socket"),
                "poll": float(item.get("poll", 0.03)),
                "extra": str(item.get("extra") or ""),
                "visible": item.get("visible") is not False,
                "_source_fields": source_fields,
            }
        )
    return displays


def parse_robot_display_bundle(raw: Any) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    if not isinstance(raw, dict):
        raise ValueError("display bundle must be an object")
    return (
        normalize_robot_displays(raw.get("robot_displays", [])),
        normalize_robot_state_displays(raw.get("robot_state_displays", [])),
    )

from .kin.kinematics import RobotKinematics  # noqa: F401
@dataclass
class RobotStateDisplay:
    id: str
    name: str = ""
    topic: str = ""
    robot_id: str = ""
    msg_type: str = "RobotState"
    domain: int = 0
    queue: int = 10
    transport: str = "socket"
    poll: float = 0.03
    extra: str = ""
    visible: bool = True
    robot_display: Optional["RobotDisplay"] = None
    joint_state_records: List[Any] = field(default_factory=list)
    latest_joint_state: Optional[Any] = None
    active: Optional[bool] = None
    source_fields: Set[str] = field(default_factory=set, repr=False)
    robotkin: Optional[RobotKinematics] = None

    @property
    def target_robot_id(self) -> str:
        return self.robot_id

    @target_robot_id.setter
    def target_robot_id(self, value: str) -> None:
        self.robot_id = value

    @classmethod
    def from_config(cls, raw: Dict[str, Any], defaults: Optional[Dict[str, Any]] = None) -> "RobotStateDisplay":
        defaults = defaults or {}
        topic = str(raw.get("topic") or raw.get("name") or "").strip()
        name = str(raw.get("name") or topic or "robot_state").strip()
        robot_id = str(
            raw.get("robot_id")
            or raw.get("target_robot_id")
            or raw.get("robot_display")
            or raw.get("robotDisplay")
            or ""
        ).strip()
        return cls(
            id=str(raw.get("id") or name).strip(),
            name=name,
            topic=topic,
            robot_id=robot_id,
            msg_type=str(raw.get("msg_type") or raw.get("type") or "RobotState").strip(),
            domain=int(raw.get("domain", defaults.get("domain", 0))),
            queue=int(raw.get("queue", defaults.get("queue", 10))),
            transport=str(raw.get("transport", defaults.get("transport", "socket"))),
            poll=float(raw.get("poll", defaults.get("poll", 0.03))),
            extra=str(raw.get("extra", defaults.get("extra", ""))),
            visible=raw.get("visible") is not False,
            active=bool(raw["active"]) if "active" in raw else None,
            source_fields=set(raw.get("_source_fields") or raw.keys()),
        )

    def bind_robot_display(self, robot_display: Optional["RobotDisplay"]) -> None:
        self.robot_display = robot_display

    def handle_joint_state(self, joint_state: Any) -> None:
        self.joint_state_records.append(joint_state)
        self.latest_joint_state = joint_state
        

    on_joint_state = handle_joint_state

    def handle_joint_state_sample(self, data: Any) -> Dict[str, Any]:
        if isinstance(data, dict):
            joint_state = data.get("joint_state", data)
        else:
            joint_state = getattr(data, "joint_state", data)
        self.handle_joint_state(joint_state)
        if isinstance(joint_state, dict):
            return {"ok": True, "target_robot_id": self.robot_id, **joint_state}
        return {"ok": True, "target_robot_id": self.robot_id, "joint_state": joint_state}

    def to_config(self) -> Dict[str, Any]:
        config = {
            "display_type": "RobotState",
            "id": self.id,
            "name": self.name,
            "topic": self.topic,
            "robot_id": self.robot_id,
            "target_robot_id": self.robot_id,
            "msg_type": self.msg_type,
        }
        optional_values = {
            "domain": self.domain,
            "queue": self.queue,
            "transport": self.transport,
            "poll": self.poll,
            "extra": self.extra,
            "visible": self.visible,
            "active": self.active,
        }
        for key, value in optional_values.items():
            if key in self.source_fields:
                config[key] = value
        return config

    def to_topic_config(self) -> Dict[str, Any]:
        return {
            "topic": self.topic,
            "msg_type": "RobotState",
            "domain": self.domain,
            "queue": self.queue,
            "transport": self.transport,
            "poll": self.poll,
            "extra": self.extra,
        }


@dataclass
class RobotDisplay:
    id: str
    name: str = "Robot"
    urdf_path: str = ""
    urdf_text: str = ""
    visible: bool = True
    fixed_frame: str = "world"
    robot_states: List[RobotStateDisplay] = field(default_factory=list)
    robot_state: Optional[RobotStateDisplay] = None
    joint_state_records: List[Any] = field(default_factory=list)
    source_fields: Set[str] = field(default_factory=set, repr=False)

    @property
    def robot_state_displays(self) -> List[RobotStateDisplay]:
        return self.robot_states

    @robot_state_displays.setter
    def robot_state_displays(self, value: List[RobotStateDisplay]) -> None:
        self.robot_states = list(value)
        self.robot_state = self.robot_states[0] if self.robot_states else None

    @classmethod
    def from_config(cls, raw: Dict[str, Any]) -> "RobotDisplay":
        name = str(raw.get("name") or raw.get("id") or "robot").strip()
        urdf_text = str(raw.get("urdf") or raw.get("urdf_text") or raw.get("urdfText") or "").strip()
        return cls(
            id=str(raw.get("id") or name).strip(),
            name=name,
            urdf_path=str(raw.get("urdf_path") or raw.get("file_name") or raw.get("filename") or "").strip(),
            urdf_text=urdf_text,
            visible=raw.get("visible") is not False,
            fixed_frame=str(raw.get("fixed_frame") or "world"),
            source_fields=set(raw.get("_source_fields") or raw.keys()),
        )

    @property
    def urdf(self) -> str:
        return self.urdf_text

    def set_urdf_path(
        self,
        urdf_path: str,
        root_dir: Optional[Path] = None,
        base_dir: Optional[Path] = None,
    ) -> Dict[str, Any]:
        if root_dir is None:
            root_dir = base_dir
        path = Path(urdf_path).expanduser()
        if not path.is_absolute() and root_dir is not None:
            path = (root_dir / path).resolve()
        self.urdf_path = str(path)
        if path.exists():
            self.urdf_text = path.read_text(encoding="utf-8")
            return {"ok": True, "path": self.urdf_path, "urdf_text": self.urdf_text}
        return {"ok": False, "path": self.urdf_path, "message": f"URDF path does not exist: {path}"}

    def handle_urdf_path_request(self, urdf_path: str, root_dir: Optional[Path] = None) -> Dict[str, Any]:
        return self.set_urdf_path(urdf_path, root_dir)

    def bind_robot_state(self, robot_state: Optional[RobotStateDisplay]) -> None:
        self.robot_state = robot_state
        if robot_state is None:
            return
        robot_state.robot_id = self.id
        robot_state.robot_display = self
        if robot_state not in self.robot_states:
            self.robot_states.append(robot_state)

    attach_robot_state_display = bind_robot_state

    def handle_joint_state(self, joint_state: Any) -> None:
        self.joint_state_records.append(joint_state)
        for robot_state in self.robot_states:
            robot_state.handle_joint_state(joint_state)

    on_joint_state = handle_joint_state

    def handle_joint_state_sample(self, joint_state: Any) -> Dict[str, Any]:
        self.handle_joint_state(joint_state)
        return {
            "ok": True,
            "robot_id": self.id,
            "robot_state_ids": [robot_state.id for robot_state in self.robot_states],
        }

    def to_config(self) -> Dict[str, Any]:
        config = {
            "display_type": "Robot",
            "id": self.id,
            "name": self.name,
            "urdf_path": self.urdf_path,
            "urdf": self.urdf_text,
        }
        if "visible" in self.source_fields:
            config["visible"] = self.visible
        if "fixed_frame" in self.source_fields:
            config["fixed_frame"] = self.fixed_frame
        if self.robot_state is not None:
            config["robot_state"] = self.robot_state.id
        return config

    def to_legacy_robot_model(self) -> Dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "file_name": Path(self.urdf_path).name if self.urdf_path else "",
            "urdf": self.urdf_text,
            "visible": self.visible,
            "fixed_frame": self.fixed_frame,
        }


__all__ = [
    "RobotDisplay",
    "RobotStateDisplay",
    "normalize_robot_models",
    "normalize_robot_displays",
    "normalize_robot_state_displays",
    "parse_robot_display_bundle",
    "robot_model_to_display",
]
