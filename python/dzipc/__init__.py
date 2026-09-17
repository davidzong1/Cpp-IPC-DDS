"""
dzIPC Python 包 — 将编译后的 C++ 模块与动态消息封装层组合。

架构：
  _dzipc_core.so   — C++ pybind11 绑定（IPC 基础设施 + GenericMessage）
  gen_msgs/        — 自动生成的 Python 消息封装（纯 Python，无需编译）
  gen_srv/         — 自动生成的 Python 服务封装（纯 Python，无需编译）
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


from . import dzflat as _dzflat   # noqa: E402  (不依赖 _dzipc_core, 不会形成环)


def publish_dzflat(pub, msg, msg_id=0) -> bool:
    """发布一条消息: 优先走 DZFlat 平坦段, 不可用则回退普通 TLV 发布。

    段由 `dzipc.dzflat.pack()` 按生成的 schema 写好 —— 发布端因此不再需要 TLV 组装与
    1460+12 页尾分段, 而接收端照旧借样/按 schema 直读(见 docs/dzflat_shm.md §9.6)。

    回退是常态而不是错误, 以下任一条都会退化成 TLV 送达(既不抛异常, 也不静默丢消息):
        开关未开(EnableDzFlat) / 类型没有 schema / 段头不合法 /
        段头 msg_id 与本话题模板不符 / 无接收方 / chunk 池耗尽 /
        nodelet 拓扑(SHM 与 UDP 各自的本地快速路径) / 该传输没有平坦腿

    返回 True 表示本次确实以平坦段发出; False 表示走了 TLV。
    """
    seg = _dzflat.pack(msg, msg_id=msg_id)
    if seg is not None and pub.publish_prebuilt_segment(seg):
        return True
    pub.publish(msg.to_generic() if hasattr(msg, "to_generic") else msg)
    return False


# AUTO_GENERATED_MSG_SRV_IMPORTS_BEGIN
from .gen_msgs import (
    RobotState,
    StdColor,
    StdDouble,
    StdFloat,
    StdHeader,
    StdImage,
    StdMarker,
    StdMatrix,
    StdMatrix3d,
    StdPath,
    StdPointCloud,
    StdPose,
    StdQuaternion,
    StdRawMessage,
    StdString,
    StdTf,
    StdVector,
    StdVector3d,
    ComplexMessage,
    TestMsg,
    Pose,
    RobotState,
)
from .gen_srv import (
    RequestResponseTestRequest,
    RequestResponseTestResponse,
    StdBoolRequest,
    StdBoolResponse,
    StdDoubleRequest,
    StdDoubleResponse,
    StdFloatRequest,
    StdFloatResponse,
    StdIntRequest,
    StdIntResponse,
    StdStringRequest,
    StdStringResponse,
)
# AUTO_GENERATED_MSG_SRV_IMPORTS_END

# AUTO_GENERATED_MSG_SRV_FUNCS_BEGIN
from .gen_msgs import message_types, create_message
from .gen_srv import service_types, create_service_request, create_service_response
# AUTO_GENERATED_MSG_SRV_FUNCS_END
