from __future__ import annotations

import math
import random
import threading
import time
from typing import Any, Callable, Optional


class DemoPublisher(threading.Thread):
    def __init__(
        self,
        hub: Any,
        period_s: float,
        time_ms: Optional[Callable[[], int]] = None,
    ) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.period_s = period_s
        self.stop_event = threading.Event()
        self.x = 0.0
        self.y = 0.0
        self.seq = 0
        self._time_ms = time_ms or (lambda: int(time.time() * 1000))

    def run(self) -> None:
        while not self.stop_event.is_set():
            self.seq += 1
            self.x += random.uniform(-0.08, 0.16)
            self.y += random.uniform(-0.10, 0.10)
            data = {
                "name": "demo_robot",
                "current_pose": {
                    "x": round(self.x, 4),
                    "y": round(self.y, 4),
                    "z": 0.0,
                    "frame_id": "map",
                },
                "battery": round(72.0 + 8.0 * math.sin(self.seq / 20.0), 3),
                "temperature": round(36.0 + 3.0 * math.sin(self.seq / 13.0), 3),
                "state": "RUNNING" if self.seq % 80 < 65 else "IDLE",
            }
            self.hub.publish(
                {
                    "kind": "sample",
                    "topic": "demo_robot_state",
                    "msg_type": "RobotState",
                    "transport": "demo",
                    "domain": 0,
                    "timestamp_ms": self._time_ms(),
                    "data": data,
                }
            )
            time.sleep(self.period_s)


__all__ = ["DemoPublisher"]
