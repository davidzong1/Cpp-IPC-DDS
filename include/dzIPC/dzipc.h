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
/* ---- 传输选择: 三个公开常量, 三个互不重叠的含义 ----
 *
 *   IPC_SHM         强制共享内存。同机最省, **但跨不了主机**。
 *   IPC_SOCKET      ser-cli: 自动选路(握手走 UDP 引导通道, 双方两阶段确认后同主机
 *                   切 SHM、跨主机保持 socket); pub/sub: 纯 UDP socket。
 *                   **这是推荐写法** —— 它表达的是"要能跨主机", 而不是"禁止共享内存"。
 *   IPC_SOCKET_ONLY 强制纯 socket, 不做任何自动选路(ser-cli)。对照实验/基线/排查用,
 *                   生产路径不该用它。
 */
constexpr IPCType IPC_SHM = IPCType::Shm;              // 共享内存通信模式(强制)
constexpr IPCType IPC_SOCKET = IPCType::Socket;        // ser-cli 自动选路 / pub-sub 纯 UDP
constexpr IPCType IPC_SOCKET_ONLY = IPCType::SocketOnly;  // 强制纯 socket(对照实验用)

#define ENABLENODELET EnableNodelet(true); // 启用进程内快速路径
#define DISABLENODELET EnableNodelet(false); // 禁用进程内快速路径
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
// UF-009 链式接管(2026-09-18): 安装前保存应用既有处置; 信号到来时回放既有处理器
// (SIG_DFL/SIG_IGN 跳过)、复权(应用处置装回), 并给应用 500ms 宽限自行退出——
// 优雅路径上栈正常展开, IPC 实例析构、SHM 段 unlink、应用收尾恢复执行;
// 超时才走库收尾: 先按注册顺序调用 RegisterShutdownCallBack 回调(signo=实际信号)、
// 扫除本进程 create 建出的段名, 再 std::exit(128+signo) —— 退出码如实上报信号
// (UF-004 子项收口 2026-09-20, 不再恒 0 掩盖)。RequestShutdown() 的库内部退出
// 路径不回放不宽限, 回调照跑(signo=0)、段名照扫, 退出码保持 0(非信号死亡)。
IPC_EXPORT void StartShutdownMonitor();
// 允许外部主动触发退出流程
IPC_EXPORT void RequestShutdown();
// 查询是否已经请求退出
IPC_EXPORT bool IsShutdownRequested();

// UF-004 收尾回调钩子(2026-09-20): 注册库收尾路径(std::exit 前)最早时机调用的回调。
//   · 超时收尾: signo = 实际信号(SIGINT=2/SIGTERM=15); RequestShutdown 内部收尾: signo = 0。
//   · 多回调按注册顺序执行; 每轮按注册时刻的快照跑一遍(回调内再注册不保证本轮被调);
//     回调异常被吞掉 —— 收尾路径不得被打断。
//   · 契约: 回调必须快速返回(阻塞会拖住整个收尾); 监控线程未启动(未创建任何 IPC 对象
//     且未显式 StartShutdownMonitor)时永不触发 —— 此时库不参与退出。
//   · 优雅路径(应用在宽限内自行退出)不经过库收尾, 回调不触发 —— 应用自己的收尾
//     本来就在 main 栈展开里。
using ShutdownCallBackFun = std::function<void(int)>;
IPC_EXPORT void RegisterShutdownCallBack(ShutdownCallBackFun cb);
// 让**库不要**隐式接管进程退出(UF-004 opt-out): 必须在创建第一个 IPC 对象**之前**调用。
//   true  = 此后库**不再隐式安装**: 四个 *IPCPtrMake 这条路不再装 SIGINT/SIGTERM 处理器、
//           不起监控线程, 信号交给应用自己的处置(SIG_DFL 即硬杀), 应用自负退出。
//           注意: 它只承诺"此后不再隐式安装", **不**代表"库当前没接管", 更**不能**撤销
//           已经装上的处理器 / 已经在跑的监控线程。顺序反例: 先**显式**调过
//           StartShutdownMonitor()(它直接走 std::call_once, 不置内部"已启动"标志)再调本
//           函数, 这里同样返回 true, 而处理器与监控线程**已经在跑**, 行为保持"已启动"——
//           这条顺序上返回值与"库是否已接管"无关。要判当前是否已接管, 请自行
//           sigaction(SIGINT, nullptr, &old) 自查, 不要读返回值。
//   false = 确定"太晚": 库已由隐式路径接管且不可撤销; 本次调用**不改变任何行为**
//           (默认路径逐位不变)。
// 只关"四个 *IPCPtrMake 隐式安装"这一条路: 显式 StartShutdownMonitor() 仍然照装。
IPC_EXPORT bool DisableShutdownMonitor() noexcept;

}
