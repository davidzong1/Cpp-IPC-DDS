#pragma once
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/logger/dzipc_log.h"
#include "dzIPC/server_ipc.h"
#include "dzIPC/topic_ipc.h"
#include "libipc/export.h"

namespace dzIPC {

/***********************************************************************************/
/***********************************************************************************/
/************************************功能函数***************************************/
/***********************************************************************************/
/***********************************************************************************/
constexpr IPCType IPC_SHM = IPCType::Shm;         // 共享内存通信模式
constexpr IPCType IPC_SOCKET = IPCType::Socket;   // UDP通信模式

#define ENABLENODELET EnableNodelet(true); // 启用进程内快速路径
#define DISABLENODELET EnableNodelet(false); // 禁用进程内快速路径
/* 全大写不是风格偏好, 是必须的: 宏名一旦与函数 EnableDzFlat 同名, 对象式宏会把
 * 调用点 dzIPC::EnableDzFlat(false) 展开成 dzIPC::EnableDzFlat(true);(false); ——
 * 两条合法语句, 编译器不报错, 而实参被**静默反转**。而 EnableDzFlat(false) 恰好是
 * 灰度升级的第一步(docs/dzflat_shm.md §3.5), 反转的后果是把 DZFlat 段推给未升级的
 * 订阅方, 表现为静默丢消息。 */
#define ENABLEDZFLAT EnableDzFlat(true); // 启用DZFlat平坦布局(shm专用)
#define DISABLEDZFLAT EnableDzFlat(false); // 禁用DZFlat平坦布局

using msgPtr = std::shared_ptr<IpcMsgBase>;   // 基类消息智能指针类型定义，用于接收数据

/***********************************************************************************/
/***********************************************************************************/
/**********************************服务-客户通信*************************************/
/***********************************************************************************/
/***********************************************************************************/
using ServerCallBackFun = std::function<void(std::shared_ptr<ServiceData>&)>;   // 服务端回调函数类型定义
using ServerDataPtr = std::shared_ptr<ServiceData>;                             // 服务数据智能指针类型定义
using ServerIPCPtr = std::shared_ptr<dzIPC::pimpl::server_ipc_impl>;            // 服务端通信类智能指针类型定义
using ClientIPCPtr = std::shared_ptr<dzIPC::pimpl::client_ipc_impl>;            // 客户端通信类智能指针类型定义
/* 服务客户通信-服务端类智能指针 */
IPC_EXPORT ServerIPCPtr ServerIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                         ServerCallBackFun callback, size_t domain_id, IPCType ipc_type,
                                         bool verbose = false, bool enable_thread_qos = Qos::NotUseQos, int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);
/* 服务客户通信-客户端类智能指针 */
IPC_EXPORT ClientIPCPtr ClientIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                         size_t domain_id, IPCType ipc_type, bool verbose = false, bool enable_thread_qos = Qos::NotUseQos, int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);

/* 服务数据智能指针创建函数定义 */
template<typename T, typename U,
         typename = std::enable_if_t<std::is_base_of_v<IpcMsgBase, T> && std::is_base_of_v<IpcMsgBase, U>>>
inline std::shared_ptr<ServiceData> ServerDataPtrMake(int msg_id = 0)
{
    return std::make_shared<ServiceData>(std::make_shared<T>(),   // 输入数据类型为 T
                                         std::make_shared<U>(),   // 输出数据类型为 U
                                         msg_id);
}

/***********************************************************************************/
/***********************************************************************************/
/**********************************话题-订阅通信*************************************/
/***********************************************************************************/
/***********************************************************************************/
using TopicDataPtr = std::shared_ptr<TopicData>;                               // 话题数据智能指针类型定义
using PublisherIPCPtr = std::shared_ptr<dzIPC::pimpl::publisher_ipc_impl>;     // 发布者类智能指针类型定义
using SubscriberIPCPtr = std::shared_ptr<dzIPC::pimpl::subscriber_ipc_impl>;   // 订阅者类智能指针类型定义
/* 发布订阅通信-发布者类智能指针 */
IPC_EXPORT PublisherIPCPtr PublisherIPCPtrMake(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                               size_t domain_id, IPCType ipc_type, bool verbose = false, bool enable_thread_qos = Qos::NotUseQos,
                                               int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);
/* 发布订阅通信-订阅者类智能指针 */
IPC_EXPORT SubscriberIPCPtr SubscriberIPCPtrMake(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                                 size_t domain_id, const size_t queue_size, IPCType ipc_type,
                                                 bool verbose = false, bool enable_thread_qos = Qos::NotUseQos,
                                                 int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);

/* 话题数据智能指针创建函数定义 */
template<typename T = IpcMsgBase, typename = std::enable_if_t<std::is_base_of<IpcMsgBase, T>::value>>
inline TopicDataPtr TopicDataPtrMake(int msg_id = 0)
{
    return std::make_shared<TopicData>(std::make_shared<T>(), msg_id);
}

/***********************************************************************************/
/***********************************************************************************/
/************************************测试使用***************************************/
/***********************************************************************************/
/***********************************************************************************/
// 启动退出监控线程，捕获 Ctrl+C 后释放全局实例容器并退出进程
IPC_EXPORT void StartShutdownMonitor();
// 允许外部主动触发退出流程
IPC_EXPORT void RequestShutdown();
// 查询是否已经请求退出
IPC_EXPORT bool IsShutdownRequested();

}
