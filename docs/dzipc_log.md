# dzipc_log

`dzipc_log` 是进程级通信记录器。它在启用后创建一个后台线程，把当前进程通过 dzIPC 工厂创建的发布、订阅、服务请求和服务响应事件放入内存队列，并在 `StopDzipcLog()` 或 `StartShutdownMonitor()` 处理退出时写入一个 ROS1 bag v2.0 文件。

## 使用

```cpp
#include "dzIPC/dzipc.h"

dzIPC::logger::StartDzipcLog("/tmp/session.bag", 256, 100000);
// 创建并使用 PublisherIPCPtrMake/SubscriberIPCPtrMake/
// ServerIPCPtrMake/ClientIPCPtrMake
dzIPC::logger::StopDzipcLog();
```

`StartDzipcLog(path, max_memory_mb, max_queue_size)` 会覆盖同名文件；已经运行时返回 `false`。未启动时通信路径不执行日志序列化。队列满时丢弃新事件，通信本身不会被阻塞。`StopDzipcLog()` 幂等，并等待后台线程排空队列后关闭文件。

日志 hook 位于公共 pimpl wrapper，因此普通路径和 Nodelet 发布/服务调用都能被捕获。为避免日志序列化修改正在通信的对象，启用日志时会额外执行一次 `clone()` 和 `serialize()`；这会抵消 Nodelet 的部分零拷贝收益。外部 `dzipc_topic_cat` 仍然只能观察实际 SHM/UDP 传输，Nodelet 绕过传输层的消息只有库内 logger 能看到。

## 文件格式与兼容边界

文件以 `#ROSBAG V2.0\n` 开头，使用无压缩（`compression=none`）chunk，并包含：

- 4096 字节文件头记录（`op=0x03`）以及可回写的 `index_pos`、连接数和 chunk 数；
- `dzipc_log/TransportPacket` connection（`op=0x07`）；
- chunk（`op=0x05`）、chunk 内 MessageData（`op=0x02`）和每 chunk 的 IndexData（`op=0x04`）；
- 文件尾部 connection/chunk-info 索引区（`op=0x06`）。

TransportPacket 的 payload 是 dzIPC `IpcMsgBase::serialize()` 得到的 opaque bytes，消息定义固定为 `dzipc_log/TransportPacket`。因此 bag 的结构可由 ROS1 bag 读取器扫描，但原始 dzIPC 消息不是 ROS 原生类型；要用 `rosbag play` 还原业务消息，需要在订阅端按项目消息定义解码 `/dzipc_events`。当前库不依赖 ROS runtime，也不提供 lz4/bz2 压缩。

## 退出与安全性

`StartShutdownMonitor()` 的监控线程在清理 IPC 实例前调用 `StopDzipcLog()`。信号处理器只设置退出标志，不执行文件 I/O；这样避免在异步信号上下文中获取 mutex 或写文件。建议在程序正常结束前显式调用 `StopDzipcLog()`，以便获得确定的 flush 时机。

## 验证

`test/test_dzipc_log.cpp` 覆盖启动/停止幂等、空 bag、发布、服务请求/响应、IpcInfoPool 元信息和 ROS bag magic。没有 ROS 安装时，仍可检查 magic、record header、op 字段和 `index_pos`；有 ROS 环境时可使用 `rosbag info` 做进一步校验。
