# dzipc_log

`dzipc_log` 是进程级通信记录器。它在启用后创建一个后台线程，把当前进程通过 dzIPC 工厂创建的发布、订阅、服务请求和服务响应事件放入内存队列，并在 `StopDzipcLog()` 或 `StartShutdownMonitor()` 处理退出时写入 ROS1 bag v2.0 文件。

## 使用

```cpp
#include "dzIPC/dzipc.h"

// 基本用法：单文件
dzIPC::logger::StartDzipcLog("/tmp/session.bag", 256, 100000);
// 或者使用默认参数 ~/.dzipc/log, 512, max
dzIPC::logger::StartDzipcLog();
// 创建并使用 PublisherIPCPtrMake/SubscriberIPCPtrMake/
// ServerIPCPtrMake/ClientIPCPtrMake
dzIPC::logger::StopDzipcLog();
```

`StartDzipcLog(path, max_memory_mb, max_queue_size)`：已经运行时返回 `false`。未启动时通信路径不执行日志序列化。队列满时丢弃新事件，通信本身不会被阻塞。`StopDzipcLog()` 幂等，并等待后台线程排空队列后关闭文件。

如果 `path` 以 `.bag` 结尾，首个文件使用该精确路径；轮转产生的后续文件使用去掉 `.bag` 的前缀加时间戳和序号。如果 `path` 不以 `.bag` 结尾，则作为目录前缀生成时间戳文件名。

日志 hook 位于公共 pimpl wrapper，因此普通路径和 Nodelet 发布/服务调用都能被捕获。为避免日志序列化修改正在通信的对象，启用日志时会额外执行一次 `clone()` 和 `serialize()`；这会抵消 Nodelet 的部分零拷贝收益。

## 自动轮转

四种独立阈值可触发 **bag 轮转**：当前文件完成 finalize（写入 index/connection/chunk-info、更新 header、关闭），然后打开新的带时间戳的 bag 文件。每个轮转出的 bag 均独立有效。

| 阈值 | 来源 | 触发条件 |
|------|------|----------|
| `max_memory_mb` | `StartDzipcLog` 参数 2 | chunk 数据量 ≥ 限制(默认512M) |
| `max_queue_size` | `StartDzipcLog` 参数 3 | 队列深度 ≥ 限制，启用 1.5× 增广容量（默认最大化） |
| `max_file_size_mb` | `RotationOptions::max_file_size_mb` | 预计文件字节数 ≥ 限制 (严格控制文件大小，默认关闭) |
| `max_duration_sec` | `RotationOptions::max_duration_sec` | bag 打开时间 ≥ 限制（通过 `cv_.wait_until` 在空闲时触发，默认关闭） |
M
轮转由 writer 线程处理。文件 close/open 在**不持有**队列 mutex 的情况下执行，因此 `RecordEvent()` 保持非阻塞。轮转期间 `RecordEvent` 使用 `max_queue_size * 3/2` 的硬上限。

### 迟滞保护

队列触发轮转后，armed 标志被消费。若队列积压仍 ≥ `max(1, max_queue_size/2)`，`rearm_blocked` 标志阻止立即重新 arm。writer 排空到低水位以下后解除阻塞，下次再跨越 `max_queue_size` 时重新 arm。这防止了连续轮转风暴。

### 文件名

path 以 `.bag` 结尾时，首个文件使用精确路径，后续轮转文件以去掉 `.bag` 的前缀加时间戳和序号：

```
events.bag                                             ← 首包（精确路径）
eventsdzipc_log_20260802T143058.456_1.bag              ← 轮转 1
eventsdzipc_log_20260802T143105.789_2.bag              ← 轮转 2
```

path 不以 `.bag` 结尾时，作为目录前缀，首包也带时间戳：

```
{prefix}dzipc_log_20260802T143052.123.bag              ← 首包（时间戳命名）
{prefix}dzipc_log_20260802T143058.456_1.bag            ← 轮转 1
{prefix}dzipc_log_20260802T143105.789_2.bag            ← 轮转 2
```

单调递增序号防止同一毫秒内的文件名冲突。

### 轮转 API

```cpp
// 新增重载：RotationOptions 控制文件大小和时间轮转
dzIPC::logger::RotationOptions opts;
opts.max_file_size_mb  = 1024;   // 超过 1 GiB 轮转
opts.max_duration_sec  = 3600;   // 每小时轮转一次

dzIPC::logger::StartDzipcLog("/tmp/events", 256, 100000, opts);

// 原三参数接口仍可用（不启用文件大小和时间轮转）
dzIPC::logger::StartDzipcLog("/tmp/events.bag", 256, 100000);
// 等价的默认值形式：
dzIPC::logger::StartDzipcLog("/tmp/events.bag");  // 512 MiB 内存，无限制队列
```

## 文件格式与兼容边界

文件以 `#ROSBAG V2.0\n` 开头，使用无压缩（`compression=none`）chunk，并包含：

- 4096 字节文件头记录（`op=0x03`）以及可回写的 `index_pos`、连接数和 chunk 数；
- `dzipc_log/TransportPacket` connection（`op=0x07`）；
- chunk（`op=0x05`）、chunk 内 MessageData（`op=0x02`）和每 chunk 的 IndexData（`op=0x04`）；
- 文件尾部 connection/chunk-info 索引区（`op=0x06`）。

| Op code | 记录类型 |
|---------|----------|
| `0x02`  | Message Data（嵌入 chunk 内） |
| `0x03`  | Bag Header |
| `0x04`  | Index Data |
| `0x05`  | Chunk |
| `0x06`  | Chunk Info |
| `0x07`  | Connection |

TransportPacket 的 payload 是 dzIPC `IpcMsgBase::serialize()` 得到的 opaque bytes，消息定义固定为 `dzipc_log/TransportPacket`。因此 bag 的结构可由 ROS1 bag 读取器扫描，但原始 dzIPC 消息不是 ROS 原生类型；要用 `rosbag play` 还原业务消息，需要在订阅端按项目消息定义解码 `/dzipc_events`。当前库不依赖 ROS runtime，也不提供 lz4/bz2 压缩。

## 退出与安全性

`StartShutdownMonitor()` 的监控线程在清理 IPC 实例前调用 `StopDzipcLog()`。信号处理器只设置退出标志，不执行文件 I/O；这样避免在异步信号上下文中获取 mutex 或写文件。建议在程序正常结束前显式调用 `StopDzipcLog()`，以便获得确定的 flush 时机。

`StopDzipcLog()` 始终会 join writer 线程并尝试 finalize 当前 bag。轮转中新文件打开失败时设置 `running_=false` 并排空剩余队列——不会引发 `std::terminate`。

## 测试

`test/test_dzipc_log.cpp` 覆盖：

| # | 测试 | 验证内容 |
|---|------|----------|
| 1 | NotRecordingByDefault | 默认不记录 |
| 2 | StartStopIdempotent | 重复启动失败，重复停止安全 |
| 3 | PublishGeneratesNonEmptyBag | 发布产生非空文件 |
| 4 | HeaderContainsRosbagV2 | `#ROSBAG V2.0\n` magic |
| 5 | TransportPacketKeyFields | 二进制中 topic、类型标记和 op 字段 |
| 6 | ServiceRequestResponseRecords | 请求/响应被捕获 |
| 7 | StopIdempotentAfterStop | 多次 stop 安全 |
| 8 | EndpointMetaWrittenOnStart | IpcInfoPool 元数据记录 |
| 9 | NodeletEnabledPublishIsRecorded | Nodelet 路径被捕获 |
| T0 | OldApiSingleBag | 三参数 API + 精确路径可用 |
| T1 | MemoryBudgetRotates | `max_memory_mb` 触发轮转 ≥2 bag |
| T2 | FileSizeTriggersRotation | `max_file_size_mb` 触发轮转 ≥2 bag |
| T3 | DurationTriggersRotation | 空闲超时触发轮转 ≥2 bag |
| T4 | QueueDepthTriggersRotation | `max_queue_size` 触发 + 增广容量 |
| T5 | HysteresisPreventsChurn | 迟滞防连续轮转 |
| T6 | MaxQueueOneEdgeCase | `max_queue=1` 边界条件不卡死 |
| T7 | CrossBagNoLossNoDup | 跨 bag 唯一事件不丢不重，每个 bag 通过结构验证 |
