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
        dzflat_enabled: bool = True,
    ) -> None:
        super().__init__(daemon=True)
        self.hub = hub
        self.spec = spec
        self.stop_event = threading.Event()
        self.sample_seq = 0
        self._cooldown_remain = cooldown_remain
        self._dzflat_enabled = bool(dzflat_enabled)
        self._dzflat_wire_logged = False
        self._ipc_loader = ipc_loader
        self._msg_resolver = msg_resolver
        self._image_encoder = image_encoder
        self._tf_encoder = tf_encoder
        self._data_serializer = data_serializer
        self._field_getter = field_getter
        self._time_ms = time_ms or (lambda: int(time.time() * 1000))

    @staticmethod
    def _sanitize_for_shm(topic: str) -> str:
        """C++ sanitize_topic_name() 的 Python 转写 —— **逐字节**, 与 C++ 一致。

        规则(src/dzIPC/common/name_operator.cc:13): 只保留 ASCII 字母数字与 '_' '-' '.',
        其余一律 '_', 且按**字节**判断:

            const bool is_alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
                                  || (ch >= 'a' && ch <= 'z');
            name.push_back(is_alnum || ch == '_' || ch == '-' || ch == '.' ? ch : '_');

        ⛔ 所以这里**不能**用 str.isalnum() —— 它认 Unicode(如 '１２３'、'²'、'四'),
        C++ 不认。实测 '/中文': 旧写法给出 '_中文', C++ 给出 '_______'(7 个字节各一个 '_'),
        段名就此错开且**不报错**(控制面 open 是 create|open, 会按错名建一个空壳)。

        errors='surrogateescape' 与 Python 解码 argv 的策略成对: 把命令行传来的、可能不是
        合法 UTF-8 的原始字节原样取回, 与 C++ 从 argv 拿到的字节串一致。
        返回必然是纯 ASCII(保留的字节全在 ASCII 区间)。

        本方法**不**加 "dz_ipc_d<domain>_" 前缀与 "_topic" 后缀 —— 那是
        _segment_name_for_topic() 的事。
        """
        raw = topic.encode("utf-8", "surrogateescape")
        out = bytearray()
        for b in raw:
            if (0x30 <= b <= 0x39) or (0x41 <= b <= 0x5A) or (0x61 <= b <= 0x7A) \
                    or b in (0x5F, 0x2D, 0x2E):   # '_' '-' '.'
                out.append(b)
            else:
                out.append(0x5F)                  # '_'
        return out.decode("ascii")

    @staticmethod
    def _segment_name_for_topic(topic: str, domain: int = 0) -> str:
        """数据段名 —— C++ shm_topic_segment_name() 的 Python 转写。

        唯一出处 include/dzIPC/common/name_operator.h(实现在 name_operator.cc:25):

            "dz_ipc_d" + std::to_string(domain_id) + "_" + sanitize_topic_name(topic) + "_topic"

        domain 参与命名, 漏掉它 SHM 上就没有 domain 隔离(docs/shm_defect_fixes.md 第 1 条)。
        """
        return ("dz_ipc_d" + str(domain) + "_"
                + DzipcSubscriber._sanitize_for_shm(topic) + "_topic")

    @staticmethod
    def _shm_globs_for_topic(topic: str, domain: int = 0) -> List[str]:
        """该 topic+domain 在 /dev/shm 里**全部**落盘文件名(glob 形式)。

        实测(2026-09-15, python3.10 起真实 SHM 发布端后逐个核对)传输层为
        dz_ipc_d0__<san>_topic 建出的文件是:

            __IPC_SHM__QU_CONN__<seg>__<queue_size>__<max_receivers>
            __IPC_SHM__AC_CONN__<seg>
            __IPC_SHM__{CC,RD,WT}_CONN__<seg>_WAITER_{COND,LOCK,STATE}_
            <seg>_control2                                   ← 控制面, **裸文件**

        即: 数据/等待者通道带 libipc 的 __IPC_SHM__ 前缀, 控制面不带。所以需要
        "带前缀的三条 + 裸的一条"。段名以 '__' 或 '_WAITER_' 为界收尾, 因此下面每条都在
        段名后立刻收窄 —— 裸 '...<seg>*' 会误伤名字更长的邻居 topic(实测
        dz_ipc_d0__x_topic 的 pattern 会匹配 dz_ipc_d0__x_topic_extra_topic 的通道)。

        ⚠️ 旧写法是 f"...__dz_ipc__{san}*" / f"...__dz_ipc_{san}*" —— 既缺 `d<domain>_`
        也缺 `_topic`, 对**任何**真实文件都不匹配, 于是这个"清理"函数一直是**静默空转**。
        """
        seg = DzipcSubscriber._segment_name_for_topic(topic, domain)
        return [
            f"/dev/shm/__IPC_SHM__*__{seg}",              # AC_CONN(无后缀)
            f"/dev/shm/__IPC_SHM__*__{seg}__*",           # QU_CONN(带 queue/receiver 后缀)
            f"/dev/shm/__IPC_SHM__*__{seg}_WAITER_*",     # CC/RD/WT 的等待者三元组
            f"/dev/shm/{seg}_control2",                   # 控制面(裸文件, 无前缀)
        ]

    @staticmethod
    def _clean_shm_for_topic(topic: str, domain: int = 0) -> None:
        """Remove stale SHM connection files for *topic* (on *domain*).

        This is a last-resort offline cleanup helper.  Normal display
        removal must not call it because display removal does not own the
        publisher's live SHM segments.

        IMPORTANT: Do NOT call this while a publisher is still running on
        the same topic — that would delete the publisher's live SHM files,
        creating a split-brain where publisher writes to old (unlinked)
        memory and the new subscriber reads from fresh (empty) files.

        ⚠️ domain 是段名的一部分, 必须由调用方给出(默认 0 只为兼容旧调用点):
        给错 domain 不会报错, 只会按**另一个** domain 的段名去找 —— 找不到就静默什么
        都不删(安全), 但若那个 domain 恰好有活在跑, 就会删掉它的段。dzviz 的
        TopicSpec.from_config() 默认值是 1(topic_spec.py:45), 而 dzplot 默认 0,
        调用时务必传 spec.domain。
        """
        for pat in DzipcSubscriber._shm_globs_for_topic(topic, domain):
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
        """Decode and publish one sample from the subscriber.

        DZFlat 话题(SHM + 开关开启): 传输层把借样段收进 GenericMessage
        (dzflat_adopt, 零拷贝), 此时 fields_ 为空 —— 必须走 from_generic() 的
        DZFlat 分支(python/dzipc/dzflat.py 按 schema 解码)才能拿到字段, 否则是
        静默空数据。RobotState 一并统一走 from_generic: 它的 joint_state 在
        note(JSON string)里, 下游 _unwrap_joint_state 本就支持从 note 解出,
        这里只负责把两种 wire 统一成封装对象(TLV 分支行为不变)。
        """
        msg_obj = out.topic() if out is not None else topic_data.topic()
        if hasattr(msg_cls, "from_generic") and hasattr(msg_obj, "field_count"):
            if not self._dzflat_wire_logged:
                self._dzflat_wire_logged = True
                try:
                    if msg_obj.has_dzflat():
                        print(
                            f"[dzviz] DZFlat wire detected on {self.spec.topic} "
                            f"(borrowed={msg_obj.dzflat_is_borrowed()}); "
                            "decoding via dzipc.dzflat schema",
                            flush=True,
                        )
                except Exception:
                    pass
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
                # DZFlat 借样开关: SHM 话题默认启用(见 dzflat_shm.md)。开关是
                # 发布端进程级原子量; 本进程内发布(若有)与订阅判别共用它。
                # 幂等且无竞态危害 —— 多话题并发重复 set 同值等价于一次。socket
                # 话题不触碰它(DZFlat 是 SHM 专属旁路, socket 默认配置不受影响)。
                if (
                    self._dzflat_enabled
                    and self.spec.transport == "shm"
                    and hasattr(ipc, "EnableDzFlat")
                ):
                    try:
                        ipc.EnableDzFlat(True)
                    except Exception as exc:
                        print(
                            f"[dzviz] EnableDzFlat(True) failed on {self.spec.topic}: {exc}",
                            flush=True,
                        )
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
