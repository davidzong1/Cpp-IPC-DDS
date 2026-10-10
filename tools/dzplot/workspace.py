"""dzplot 工作区：复用 3D 显示组件，并连接曲线与场景的数据源。"""
from __future__ import annotations

from typing import Any, Dict
import threading

from tools.dzplot import visualizer as viz


def load_workspace_ipc() -> Any:
    # 与 dzplot 的嗅探/回放一致，显式 PYTHONPATH 和已安装绑定优先。
    try:
        import dzipc
    except ImportError as exc:
        local_library = viz.ROOT_DIR / "local" / "lib" / "libipc.so"
        if "libipc.so" not in str(exc) or not local_library.is_file():
            raise RuntimeError("无法加载 dzIPC 绑定，请使用与绑定版本匹配的 Python 解释器。") from exc
        # 仅在动态库未找到时补充仓库安装产物；正常的显式环境不被覆盖。
        viz.ctypes.CDLL(str(local_library), mode=viz.ctypes.RTLD_GLOBAL)
        import dzipc
    except Exception as exc:
        raise RuntimeError("无法加载 dzIPC 绑定，请使用与绑定版本匹配的 Python 解释器。") from exc
    return dzipc


class WorkspaceVizHub(viz.WebHub):
    """3D 订阅样本供曲线使用；工作区显示复用已有回放/嗅探样本。"""

    def __init__(self, plot_hub: Any, defaults: Dict[str, Any], config_path: Any) -> None:
        self.plot_hub = plot_hub
        self.source_displays: Dict[str, Any] = {}
        self._sample_ids = 0
        self._sample_lock = threading.Lock()
        self._decode_warnings = set()
        self.ipc_loader = load_workspace_ipc
        super().__init__(defaults, config_path)

    def publish(self, event: Dict[str, Any]) -> None:
        if event.get("kind") == "sample" and not event.get("workspace_sample"):
            self.plot_hub.event_queue.put({
                "source": "visualizer",
                "topic": event["topic"],
                "msg_type": event.get("msg_type", ""),
                "timestamp_ns": event.get("timestamp_ms", viz.now_ms()) * 1_000_000,
                "fields": {**self.plot_fields(event.get("data", {})),
                           **{key: event.get("binary", {})[key]
                              for key in ("point_count", "byte_length")
                              if key in event.get("binary", {})}},
                "transport": event.get("transport", ""),
                "domain": event.get("domain", 0),
            })
        super().publish(event)

    @classmethod
    def plot_fields(cls, value: Any, depth: int = 0) -> Any:
        # 图像和点云的完整数据留在 3D 通路；曲线只需有界的字段预览。
        if depth > 8:
            return None
        if isinstance(value, dict):
            return {key: cls.plot_fields(item, depth + 1)
                    for key, item in value.items()
                    if key not in {"data_b64", "points_b64", "colors_b64"}}
        if isinstance(value, (list, tuple)):
            return [cls.plot_fields(item, depth + 1) for item in value[:100]]
        return value

    def observe_plot_sample(self, sample: Dict[str, Any]) -> None:
        if sample.pop("_viz_encoded", False):
            return
        topic = sample.get("topic", "")
        if sample.get("source") == "visualizer" or topic not in self.source_displays:
            return
        spec = self.source_displays[topic]
        self.publish({
            "kind": "sample", "workspace_sample": True,
            "topic": topic, "msg_type": spec.msg_type,
            "transport": sample.get("source", "workspace"), "domain": spec.domain,
            "timestamp_ms": sample.get("timestamp_ns", 0) / 1_000_000 or viz.now_ms(),
            "data": sample.get("fields", {}),
        })

    def decode_source_sample(self, sample: Dict[str, Any], raw: Any = None,
                             message: Any = None) -> None:
        """仅为已选 3D 显示解码完整载荷，曲线仍发送有界字段预览。"""
        topic = sample.get("topic", "")
        spec = self.source_displays.get(topic)
        if spec is None:
            return
        sample["_viz_encoded"] = True
        try:
            fields = sample.get("fields", {})
            display_type = spec.msg_type
            resolved_type = viz.VISUALIZER_MESSAGE_ALIASES.get(display_type, display_type)
            if resolved_type in set(viz.VISUALIZER_MESSAGE_ALIASES.values()):
                wire_type = sample.get("msg_type") or resolved_type
                wire_type = viz.VISUALIZER_MESSAGE_ALIASES.get(wire_type, wire_type)
                if raw is not None or (message is not None and hasattr(message, "field_count")):
                    ipc = load_workspace_ipc()
                    if message is None and raw is not None:
                        from dzipc import dzflat
                        from dzipc.gen_msgs import _dzflat_schema  # noqa: F401
                        if dzflat.looks_like_dzflat(raw):
                            message = dzflat.decode(raw, zero_copy=False)
                            if message is None:
                                raise ValueError("3D 数据的 DZFlat schema 未找到")
                        else:
                            message = ipc.create_message(wire_type)
                            message.deserialize_bytes(bytes(raw))
                    cls = getattr(ipc, wire_type, None)
                    if message is not None and hasattr(message, "field_count") and cls and hasattr(cls, "from_generic"):
                        message = cls.from_generic(message)
                fields = message if message is not None else fields
            with self._sample_lock:
                self._sample_ids = (self._sample_ids + 1) % (1 << 32)
                sample_id = self._sample_ids
            event = {"kind": "sample", "workspace_sample": True,
                     "topic": topic, "msg_type": spec.msg_type,
                     "transport": sample.get("source", "workspace"), "domain": spec.domain,
                     "timestamp_ms": sample.get("timestamp_ns", 0) / 1_000_000 or viz.now_ms()}
            if resolved_type == "StdPointCloud":
                event.update(viz.POINT_CLOUD_ENCODER.encode_event_fields(fields, sample_id))
            elif resolved_type == "StdImage":
                event["binary"] = viz.encode_image_data(fields)
                event["data"] = {key: value for key, value in event["binary"].items()
                                 if key not in {"data_b64", "encoding_type"}}
            elif resolved_type == "StdTF":
                event.update(viz.DEFAULT_TF_SAMPLE_ENCODER.encode_event_fields(fields, sample_id))
            else:
                event["data"] = viz.to_jsonable(fields)
            self.publish(event)
        except Exception as exc:
            if topic not in self._decode_warnings:
                self._decode_warnings.add(topic)
                self.publish({"kind": "error", "topic": topic,
                              "message": f"无法解码 3D 工作区数据：{exc}", "timestamp_ms": viz.now_ms()})

    def add_source_display(self, raw: Dict[str, Any]) -> Any:
        spec = viz.TopicSpec.from_config(raw, self.defaults)
        if not spec.topic:
            raise ValueError("请输入工作区话题名称")
        if spec.topic in self.subscribers:
            super().remove_topic(spec.topic)
        self.source_displays[spec.topic] = spec
        meta = {**spec.to_meta(active=True), "source": "workspace", "visualize": True}
        self.topics[spec.topic] = meta
        self.publish({"kind": "topic_added", "topic": meta, "timestamp_ms": viz.now_ms()})
        return spec

    def add_topic(self, spec: Any) -> None:
        self.source_displays.pop(spec.topic, None)
        super().add_topic(spec)

    def remove_topic(self, topic: str) -> None:
        self.source_displays.pop(topic, None)
        super().remove_topic(topic)

    def add_robot_state_display(self, raw: Dict[str, Any]) -> Any:
        if raw.get("source") != "workspace":
            return super().add_robot_state_display(raw)
        state = viz.RobotStateDisplay.from_config(raw, self.defaults)
        robot = self.robot_displays.get(state.target_robot_id)
        if not state.topic or robot is None:
            raise ValueError("机器人状态显示需要话题和已存在的机器人模型")
        self.robot_state_displays[state.id] = state
        robot.attach_robot_state_display(state)
        self.add_source_display(state.to_topic_config())
        self.topics[state.topic].update(display_type="RobotState", robot_id=robot.id,
                                        display_id=state.id)
        self.publish({"kind": "robot_state_added", "robot_state": {
            **state.to_config(), "source": "workspace"}, "timestamp_ms": viz.now_ms()})
        return state

    def remove_robot_state_display(self, state_id: str) -> None:
        state = self.robot_state_displays.get(state_id)
        if state and state.topic in self.source_displays:
            self.remove_topic(state.topic)
        super().remove_robot_state_display(state_id)

    def export_config(self) -> Dict[str, Any]:
        config = super().export_config()
        config["topics"].extend({**spec.to_config(), "source": "workspace"}
                                for spec in self.source_displays.values())
        return config

    def robot_state_display_configs(self) -> Any:
        items = super().robot_state_display_configs()
        for item in items:
            if item.get("topic") in self.source_displays:
                item["source"] = "workspace"
        return items

    def apply_config(self, config: Dict[str, Any], replace: bool = True) -> None:
        if replace:
            for topic in list(self.source_displays):
                self.remove_topic(topic)
        live_topics = [item for item in config.get("topics", [])
                       if item.get("source") != "workspace"]
        super().apply_config({**config, "topics": live_topics}, replace)
        for item in config.get("topics", []):
            if item.get("source") == "workspace":
                self.add_source_display(item)

    def handle_command(self, command: Dict[str, Any]) -> Dict[str, Any]:
        if command.get("action") == "add_source_display":
            spec = self.add_source_display(command.get("topic", {}))
            return {"kind": "ack", "action": "add_source_display", "ok": True,
                    "topic": self.topics[spec.topic]}
        return super().handle_command(command)
