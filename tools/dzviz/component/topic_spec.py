from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Dict

MIN_POLL_INTERVAL_S = 0.005


def normalize_poll_interval(value: Any, fallback: float = 0.03) -> float:
    try:
        poll = float(value)
    except (TypeError, ValueError):
        poll = fallback
    if not math.isfinite(poll):
        poll = fallback
    return max(MIN_POLL_INTERVAL_S, poll)


@dataclass
class TopicSpec:
    topic: str
    msg_type: str
    domain: int
    queue: int
    transport: str
    poll: float
    extra: str = ""
    verbose: bool = False

    @classmethod
    def from_config(cls, raw: Dict[str, Any], defaults: Dict[str, Any]) -> "TopicSpec":
        topic = str(raw.get("topic") or raw.get("name") or "").strip()
        msg_type = str(raw.get("msg_type") or raw.get("type") or "").strip()
        if not topic or not msg_type:
            raise ValueError("topic and msg_type are required")
        transport = str(
            raw.get("transport", defaults.get("transport", "socket"))
        ).strip()
        if transport not in {"shm", "socket"}:
            raise ValueError("transport must be shm or socket")
        return cls(
            topic=topic,
            msg_type=msg_type,
            domain=int(raw.get("domain", defaults.get("domain", 1))),
            queue=int(raw.get("queue", defaults.get("queue", 10))),
            transport=transport,
            poll=normalize_poll_interval(raw.get("poll", defaults.get("poll", 0.03))),
            extra=str(raw.get("extra", defaults.get("extra", ""))),
            verbose=bool(raw.get("verbose", defaults.get("verbose", False))),
        )

    def to_config(self) -> Dict[str, Any]:
        return {
            "topic": self.topic,
            "msg_type": self.msg_type,
            "domain": self.domain,
            "queue": self.queue,
            "transport": self.transport,
            "poll": self.poll,
            "extra": self.extra,
            "verbose": self.verbose,
        }

    def to_meta(self, active: bool = True) -> Dict[str, Any]:
        meta = self.to_config()
        meta.update({"name": self.topic, "type": self.msg_type, "active": active})
        return meta


__all__ = ["TopicSpec", "MIN_POLL_INTERVAL_S", "normalize_poll_interval"]
