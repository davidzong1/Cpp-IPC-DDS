from __future__ import annotations

import gc
import glob
import os
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

from .point_clouds import DEFAULT_POINT_CLOUD_SAMPLE_ENCODER
from .tf import DEFAULT_TF_SAMPLE_ENCODER

POINT_CLOUD_ENCODER = DEFAULT_POINT_CLOUD_SAMPLE_ENCODER
TF_ENCODER = DEFAULT_TF_SAMPLE_ENCODER
SUBSCRIBER_FIRST_SAMPLE_TIMEOUT_S = 3.0
SUBSCRIBER_RETRY_BACKOFF_MAX_S = 5.0
SHM_IDLE_RECONNECT_MIN_S = 2.0
SHM_IDLE_RECONNECT_POLL_MULTIPLIER = 50.0


class DzipcSubscriber(threading.Thread):
    def __init__(
        self,
        hub: Any,
        spec: Any,
        cooldown_remain: float = 0.0,
        ipc_loader: Optional[Callable[[], Any]] = None,
        msg_resolver: Optional[Callable[[Any, str], Tuple[str, Any]]] = None,
        image_encoder: Optional[Callable[[Any], Dict[str, Any]]] = None,
        tf_encoder: Any = None,
        data_serializer: Optional[Callable[[Any, int], Any]] = None,
        field_getter: Optional[Callable[[Any, str, Any], Any]] = None,
        time_ms: Optional[Callable[[], int]] = None,
    ) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.spec = spec
        self.stop_event = threading.Event()
        self.sample_seq = 0
        self._cooldown_remain = cooldown_remain
        self._ipc_loader = ipc_loader
        self._msg_resolver = msg_resolver
        self._image_encoder = image_encoder
        self._tf_encoder = tf_encoder
        self._data_serializer = data_serializer
        self._field_getter = field_getter
        self._time_ms = time_ms or (lambda: int(time.time() * 1000))

    @staticmethod
    def _sanitize_for_shm(topic: str) -> str:
        """Mimic C++ sanitize_topic_name() character-level sanitisation.

        Replaces every character that is NOT alphanumeric, '_', '-', or '.'
        with '_', matching the C++ implementation in name_operator.cc.
        This does NOT add the "dz_ipc_" prefix or "_topic" suffix.
        """
        result: List[str] = []
        for ch in topic:
            if ch.isalnum() or ch in ('_', '-', '.'):
                result.append(ch)
            else:
                result.append('_')
        return ''.join(result)

    @staticmethod
    def _clean_shm_for_topic(topic: str) -> None:
        """Remove stale SHM connection files for *topic*.

        This is a last-resort offline cleanup helper.  Normal display
        removal must not call it because display removal does not own the
        publisher's live SHM segments.  We match against the
        C++-sanitised topic name to cover control-plane, queue, waiter,
        and counter SHM segments when no publisher is running.

        IMPORTANT: Do NOT call this while a publisher is still running on
        the same topic — that would delete the publisher's live SHM files,
        creating a split-brain where publisher writes to old (unlinked)
        memory and the new subscriber reads from fresh (empty) files.
        """
        sanitised = DzipcSubscriber._sanitize_for_shm(topic)
        for pat in (
            f"/dev/shm/__IPC_SHM__*__dz_ipc__{sanitised}*",
            f"/dev/shm/__IPC_SHM__*__dz_ipc_{sanitised}*",
        ):
            for path in glob.glob(pat):
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def _retry_delay(self, attempt: int) -> float:
        return min(SUBSCRIBER_RETRY_BACKOFF_MAX_S, 0.5 * (2 ** min(attempt, 4)))

    def _idle_reconnect_timeout(self) -> float:
        if self.spec.transport != "shm":
            return 0.0
        return max(
            SHM_IDLE_RECONNECT_MIN_S,
            self.spec.poll * SHM_IDLE_RECONNECT_POLL_MULTIPLIER,
        )

    def _cleanup_session(
        self,
        sub: Any,
        topic_data: Any,
        template: Any,
        msg_cls: Any,
        ipc: Any,
    ) -> Tuple[None, None, None, None, None]:
        del sub
        del topic_data
        del template
        del msg_cls
        del ipc
        for _ in range(3):
            gc.collect()
        return None, None, None, None, None

    def _process_sample(
        self, sub: Any, topic_data: Any, out: Any,
        msg_cls: Any, resolved_msg_type: str,
    ) -> None:
        """Decode and publish one sample from the subscriber."""
        msg_obj = out.topic() if out is not None else topic_data.topic()
        if (
            resolved_msg_type != "RobotState"
            and hasattr(msg_cls, "from_generic")
            and hasattr(msg_obj, "field_count")
        ):
            msg_obj = msg_cls.from_generic(msg_obj)
        self.sample_seq = (self.sample_seq + 1) & 0xFFFFFFFF
        event: Dict[str, Any] = {
            "kind": "sample",
            "topic": self.spec.topic,
            "msg_type": self.spec.msg_type,
            "resolved_msg_type": resolved_msg_type,
            "transport": self.spec.transport,
            "domain": self.spec.domain,
            "queue": self.spec.queue,
            "extra": self.spec.extra,
            "timestamp_ms": self._time_ms(),
        }
        if resolved_msg_type == "StdPointCloud":
            event.update(POINT_CLOUD_ENCODER.encode_event_fields(msg_obj, self.sample_seq))
        elif resolved_msg_type == "StdTF":
            if self._tf_encoder is not None:
                event.update(self._tf_encoder.encode_event_fields(msg_obj, self.sample_seq))
            elif self._data_serializer is not None:
                event["data"] = self._data_serializer(msg_obj, 0)
            else:
                event["data"] = str(msg_obj)
        elif resolved_msg_type == "StdImage":
            if self._image_encoder is None or self._field_getter is None:
                event["data"] = "<image-encoder-not-available>"
            else:
                metadata = self._image_encoder(msg_obj)
                event.update({
                    "data": {
                        "header": self._data_serializer(self._field_getter(msg_obj, "header", {}), 0) if self._data_serializer else {},
                        "width": metadata["width"],
                        "height": metadata["height"],
                        "encoding": metadata["image_encoding"],
                        "step": metadata["step"],
                        "data_length": metadata["data_length"],
                    },
                    "binary": metadata,
                })
        else:
            if self._data_serializer is not None:
                event["data"] = self._data_serializer(msg_obj, 0)
            else:
                event["data"] = str(msg_obj)
        self.hub.publish(event)

    def run(self) -> None:
        # Honour the cooldown period (deferred here from add_topic()
        # to keep the asyncio event loop responsive).
        if self._cooldown_remain > 0:
            time.sleep(self._cooldown_remain)
            gc.collect()

        now_ms_fn = self._time_ms

        attempt = 0
        while not self.stop_event.is_set():
            if self.stop_event.is_set():
                return
            sub = None
            ipc = None
            topic_data = None
            template = None
            msg_cls = None
            resolved_msg_type = ""
            try:
                if self._ipc_loader is None:
                    raise RuntimeError("ipc_loader is required for DzipcSubscriber")
                ipc = self._ipc_loader()
                if self._msg_resolver is None:
                    raise RuntimeError("msg_resolver is required for DzipcSubscriber")
                resolved_msg_type, msg_cls = self._msg_resolver(
                    ipc, self.spec.msg_type
                )
                template = msg_cls()
                topic_data = ipc.make_topic_data(template)
                ipc_type = (
                    ipc.IPC_SHM
                    if self.spec.transport == "shm"
                    else ipc.IPC_SOCKET
                )
                sub = ipc.SubscriberIPCPtrMake(
                    topic_data,
                    self.spec.topic,
                    self.spec.domain,
                    self.spec.queue,
                    ipc_type,
                    self.spec.verbose,
                )
                sub.InitChannel(self.spec.extra)
            except Exception as exc:
                self.hub.publish({
                    "kind": "error", "topic": self.spec.topic,
                    "message": f"init failed (attempt {attempt + 1}): {exc}",
                    "timestamp_ms": now_ms_fn(),
                })
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(self._retry_delay(attempt)):
                    return
                attempt += 1
                continue

            # Wait for the first sample to confirm the channel is healthy.
            first_sample_deadline = (
                time.monotonic() + SUBSCRIBER_FIRST_SAMPLE_TIMEOUT_S
            )
            got_data = False
            while time.monotonic() < first_sample_deadline:
                if self.stop_event.is_set():
                    sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                        sub, topic_data, template, msg_cls, ipc
                    )
                    return
                try:
                    ok, out = sub.try_get_clone(topic_data)
                    if ok:
                        got_data = True
                        self._process_sample(
                            sub, topic_data, out, msg_cls,
                            resolved_msg_type,
                        )
                        break
                    if self.stop_event.wait(self.spec.poll):
                        sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                            sub, topic_data, template, msg_cls, ipc
                        )
                        return
                except Exception as exc:
                    self.hub.publish({
                        "kind": "error", "topic": self.spec.topic,
                        "message": f"read failed during health check: {exc}",
                        "timestamp_ms": now_ms_fn(),
                    })
                    if self.stop_event.wait(self.spec.poll):
                        sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                            sub, topic_data, template, msg_cls, ipc
                        )
                        return

            if not got_data:
                self.hub.publish({
                    "kind": "error", "topic": self.spec.topic,
                    "message": f"no data received (attempt {attempt + 1}), retrying",
                    "timestamp_ms": now_ms_fn(),
                })
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(max(2.0, self._retry_delay(attempt))):
                    return
                attempt += 1
                continue

            # --- healthy main loop ---
            attempt = 0
            last_sample_time = time.monotonic()
            idle_reconnect_timeout = self._idle_reconnect_timeout()
            try:
                while not self.stop_event.is_set():
                    try:
                        ok, out = sub.try_get_clone(topic_data)
                        if not ok:
                            if (
                                idle_reconnect_timeout > 0
                                and time.monotonic() - last_sample_time
                                > idle_reconnect_timeout
                            ):
                                self.hub.publish({
                                    "kind": "error",
                                    "topic": self.spec.topic,
                                    "message": (
                                        "no samples received for "
                                        f"{idle_reconnect_timeout:.1f}s; "
                                        "rebuilding subscriber"
                                    ),
                                    "timestamp_ms": now_ms_fn(),
                                })
                                break
                            if self.stop_event.wait(self.spec.poll):
                                return
                            continue
                        last_sample_time = time.monotonic()
                        self._process_sample(
                            sub, topic_data, out, msg_cls,
                            resolved_msg_type,
                        )
                    except Exception as exc:
                        self.hub.publish({
                            "kind": "error", "topic": self.spec.topic,
                            "message": str(exc), "timestamp_ms": now_ms_fn(),
                        })
                        if self.stop_event.wait(
                            timeout=max(self.spec.poll, 0.2)
                        ):
                            return
            finally:
                sub, topic_data, template, msg_cls, ipc = self._cleanup_session(
                    sub, topic_data, template, msg_cls, ipc
                )
                if self.stop_event.wait(0.3):
                    return


__all__ = [
    "DzipcSubscriber",
    "SUBSCRIBER_FIRST_SAMPLE_TIMEOUT_S",
    "SUBSCRIBER_RETRY_BACKOFF_MAX_S",
    "SHM_IDLE_RECONNECT_MIN_S",
    "SHM_IDLE_RECONNECT_POLL_MULTIPLIER",
]
