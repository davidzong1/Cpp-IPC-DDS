from __future__ import annotations

from enum import Enum
from typing import Callable, List, Tuple

class IPCType(Enum):
    Shm: int
    Socket: int

IPC_SHM: IPCType
IPC_SOCKET: IPCType

class IpcMsgBase:
    def set_msg_id(self, msg_id: int) -> None: ...
    def total_size(self) -> int: ...
    def total_page_cnt(self) -> int: ...

class TopicData:
    def __init__(self, topic: IpcMsgBase, msg_id: int = 0) -> None: ...
    def topic(self) -> IpcMsgBase: ...
    def update(self, other: IpcMsgBase) -> None: ...
    def swap(self, other: IpcMsgBase) -> IpcMsgBase: ...

class ServiceData:
    def __init__(self, request: IpcMsgBase, response: IpcMsgBase, msg_id: int = 0) -> None: ...
    def request(self) -> IpcMsgBase: ...
    def response(self) -> IpcMsgBase: ...

class ServerIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: ServiceData) -> None: ...
    def reset_callback(self, callback: Callable[[ServiceData], None]) -> None: ...

class ClientIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: ServiceData) -> None: ...
    def send_request(self, request: ServiceData, rev_tm: int = ...) -> bool: ...

class PublisherIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: TopicData) -> None: ...
    def publish(self, msg: IpcMsgBase) -> bool: ...
    def has_subscribed(self) -> bool: ...

class SubscriberIPC:
    def InitChannel(self, extra_info: str = "") -> None: ...
    def reset_message(self, msg: TopicData) -> None: ...
    def get(self, msg: TopicData) -> TopicData: ...
    def try_get(self, msg: TopicData) -> Tuple[bool, TopicData]: ...

# AUTO_GENERATED_MSG_SRV_STUBS_BEGIN
class ComplexMessage(IpcMsgBase):
    status: bool
    tiny_int: int
    tiny_uint: int
    small_int: int
    small_uint: int
    normal_int: int
    normal_uint: int
    big_int: int
    big_uint: int
    single_precision: float
    double_precision: float
    message: str
    status_array: List[bool]
    tiny_int_array: List[int]
    tiny_uint_array: List[int]
    small_int_array: List[int]
    small_uint_array: List[int]
    normal_int_array: List[int]
    normal_uint_array: List[int]
    big_int_array: List[int]
    big_uint_array: List[int]
    single_precision_array: List[float]
    double_precision_array: List[float]
    message_array: List[str]

class TestMsg(IpcMsgBase):
    data1: List[float]
    data2: List[int]
    data3: List[str]
    data4: bool

class RequestResponseTestRequest(IpcMsgBase):
    request: List[float]

class RequestResponseTestResponse(IpcMsgBase):
    response: List[float]

def message_types() -> List[str]: ...
def create_message(type_name: str) -> IpcMsgBase: ...

def service_types() -> List[str]: ...
def create_service_request(service_name: str) -> IpcMsgBase: ...
def create_service_response(service_name: str) -> IpcMsgBase: ...

# AUTO_GENERATED_MSG_SRV_STUBS_END

def make_topic_data(msg: IpcMsgBase, msg_id: int = 0) -> TopicData: ...

def make_service_data(
    request: IpcMsgBase,
    response: IpcMsgBase,
    msg_id: int = 0,
) -> ServiceData: ...

def ServerIPCPtrMake(
    topic_name: str,
    msg: ServiceData,
    callback: Callable[[ServiceData], None],
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> ServerIPC: ...

def ClientIPCPtrMake(
    topic_name: str,
    msg: ServiceData,
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> ClientIPC: ...

def PublisherIPCPtrMake(
    msg: TopicData,
    topic_name: str,
    domain_id: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> PublisherIPC: ...

def SubscriberIPCPtrMake(
    msg: TopicData,
    topic_name: str,
    domain_id: int,
    queue_size: int,
    ipc_type: IPCType,
    verbose: bool = False,
) -> SubscriberIPC: ...


def StartShutdownMonitor() -> None: ...

def RequestShutdown() -> None: ...

def IsShutdownRequested() -> bool: ...
