"""
dzIPC Python 包 — 将编译后的 C++ 模块与动态消息封装层组合。

架构：
  _dzipc_core.so   — C++ pybind11 绑定（IPC 基础设施 + GenericMessage）
  gen_msgs/        — 自动生成的 Python 消息封装（纯 Python，无需编译）
"""

# 从编译后的 C++ 模块导入所有符号
from ._dzipc_core import *  # noqa: F401, F403

# 保存原始 C++ 函数引用
_make_topic_data = make_topic_data
_make_service_data = make_service_data


def _to_generic(obj):
    """将 Python 封装对象或 GenericMessage 统一转为 GenericMessage"""
    if isinstance(obj, GenericMessage):
        return obj
    if hasattr(obj, 'to_generic'):
        return obj.to_generic()
    return obj


def make_topic_data(msg, msg_id=0):
    """创建 TopicData —— 自动将 Python 封装对象转为 GenericMessage"""
    return _make_topic_data(_to_generic(msg), msg_id)


def make_service_data(request, response, msg_id=0):
    """创建 ServiceData —— 自动将 Python 封装对象转为 GenericMessage"""
    return _make_service_data(_to_generic(request), _to_generic(response), msg_id)


# AUTO_GENERATED_MSG_SRV_IMPORTS_BEGIN
from .gen_msgs import (
    Supervisor,
    ComplexMessage,
    TestMsg,
    TrainIterInfo,
    RequestResponseTestRequest,
    RequestResponseTestResponse,
    RewardCfgRequest,
    RewardCfgResponse,
)
# AUTO_GENERATED_MSG_SRV_IMPORTS_END

# AUTO_GENERATED_MSG_SRV_FUNCS_BEGIN
from .gen_msgs import (message_types, service_types,
    create_message, create_service_request, create_service_response)
# AUTO_GENERATED_MSG_SRV_FUNCS_END
