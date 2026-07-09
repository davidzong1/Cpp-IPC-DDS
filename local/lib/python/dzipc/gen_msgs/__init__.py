"""自动生成的类型注册表 — 每次修改 .msg/.srv 后由 batch_msg_srv_generator.py 更新"""
from __future__ import annotations
from typing import List

# 导入所有封装类
from .supervisor import Supervisor
from .complex_message import ComplexMessage
from .test_msg import TestMsg
from .train_iter_info import TrainIterInfo
from .request_response_test import RequestResponseTestRequest, RequestResponseTestResponse
from .reward_cfg import RewardCfgRequest, RewardCfgResponse

# 暴露到 dzipc 模块命名空间
__all__ = [
    "Supervisor",
    "ComplexMessage",
    "TestMsg",
    "TrainIterInfo",
    "RequestResponseTestRequest",
    "RequestResponseTestResponse",
    "RewardCfgRequest",
    "RewardCfgResponse",
]


def message_types() -> List[str]:
    return [
        "Supervisor",
        "ComplexMessage",
        "TestMsg",
        "TrainIterInfo",
    ]


def create_message(type_name: str):
    """创建消息实例并返回其 GenericMessage（已序列化字段）"""
    from dzipc._dzipc_core import GenericMessage  # noqa: F401
    if type_name == "Supervisor":
        return Supervisor().to_generic()
    if type_name == "ComplexMessage":
        return ComplexMessage().to_generic()
    if type_name == "TestMsg":
        return TestMsg().to_generic()
    if type_name == "TrainIterInfo":
        return TrainIterInfo().to_generic()
    raise ValueError("Unknown message type: " + type_name)


def service_types() -> List[str]:
    return [
        "RequestResponseTest",
        "RewardCfg",
    ]


def create_service_request(service_name: str):
    """创建服务请求实例并返回其 GenericMessage"""
    if service_name == "RequestResponseTest":
        return RequestResponseTestRequest().to_generic()
    if service_name == "RewardCfg":
        return RewardCfgRequest().to_generic()
    raise ValueError("Unknown service type: " + service_name)


def create_service_response(service_name: str):
    """创建服务响应实例并返回其 GenericMessage"""
    if service_name == "RequestResponseTest":
        return RequestResponseTestResponse().to_generic()
    if service_name == "RewardCfg":
        return RewardCfgResponse().to_generic()
    raise ValueError("Unknown service type: " + service_name)
