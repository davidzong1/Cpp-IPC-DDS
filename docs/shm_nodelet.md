# dzIPC 同进程快速路径（Nodelet）

> **状态**：实现完成并通过集成验证（2026-08-01）。
> **范围**：SHM pub/sub、Socket pub/sub、SHM ser/cli。Socket ser/cli 暂无 nodelet 路径。

---

## 1. 概述

受 ROS1 nodelet 启发，当通信双方位于**同一进程**中时，快速路径可消除序列化和传输层开销。不再走完整的 `serialize() → 传输层 → deserialize()` 管道，而是直接传递 `shared_ptr`。

| 传输层 | 快速路径 | 判定机制 | 传递方式 |
|--------|----------|----------|----------|
| **SHM pub/sub** | ✅ 已实现 | `LocalPubSubRegistry` + `recv_count()` | clone-once → fanout |
| **Socket pub/sub** | ✅ 已实现 | `LocalPubSubRegistry` + `IpcInfoPool` 快照 | clone-once → fanout |
| **SHM ser/cli** | ✅ 已实现 | `LocalPubSubRegistry`（server 唯一） | 每请求 reply queue |
| **Socket ser/cli** | ❌ 无 | — | — |

所有快速路径由**统一进程级开关**控制，默认**关闭**。

---

## 2. 统一进程级开关

```cpp
#include "dzIPC/dzipc.h"

// 默认 false。开启后各传输在条件满足时尝试快速路径。
dzIPC::EnableNodelet(true);

// 查询当前状态（atomic，线程安全，可在任意线程/时刻调用）
bool on = dzIPC::IsNodeletEnabled();
```

| 属性 | 值 |
|------|-----|
| **实现** | `dzIPC/common/nodelet_config.h` / `nodelet_config.cc`；`dzipc.h` 通过 include 重新导出 |
| **默认值** | `false` — 向后兼容，现有代码行为不变 |
| **线程安全** | `std::atomic<bool>`，acquire/release 语义，可在任意时刻切换 |
| **回退 warning** | 开关开启但条件不满足时，每实例每原因输出一次 warning，避免刷屏 |
| **作用域** | 进程级；所有传输读取同一开关 |

---

## 3. SHM pub/sub 快速路径

### 3.1 机制

`shm_pub_ipc::publish_best_effort()` 在 `IsNodeletEnabled()` 为 true 时：

1. 从 `LocalPubSubRegistry` 获取本地订阅者队列快照（key：`(topic, domain, msg_id, ChannelKind::ShmPubSub)`）
2. 从 `publisher_->recv_count()` 获取 SHM 连接的接收者总数
3. 判定条件：`snapshot 非空` 且 `shm_recv == snapshot.size()`（全部 SHM 接收者均在本地注册表中）
4. K=3 稳定性门槛：连续 3 次发布观察到相同拓扑后激活
5. 激活后：`msg->clone()` 一次，同一 `shared_ptr` 扇入所有本地订阅者队列

### 3.2 发布流程

```
publish(msg)
  │
  ├─ IF !IsNodeletEnabled():
  │     publish_for_sniffer(msg)              // 开关关闭，走传统 SHM 路径
  │     return
  │
  ├─ snapshot = registry.subscriber_snapshot(KEY)
  ├─ shm_recv = publisher_->recv_count()
  │
  ├─ IF !snapshot.empty() AND shm_recv == snapshot.size():
  │     consecutive_match++
  │     IF consecutive_match >= K (=3):
  │         cloned = msg->clone()
  │         FOR EACH queue IN snapshot:
  │             queue->push(cloned)
  │         return true
  │
  └─ ELSE:
        consecutive_match = 0
        emit_one_shot_warning(reason)          // 每实例每原因一次，永不重复
        publish_for_sniffer(msg)
```

### 3.3 K=3 稳定性门槛

连续 3 次发布观察到相同拓扑后才激活。任何拓扑变更（订阅者加入/离开、recv 计数变化）立即重置计数器。

### 3.4 `shared_mutex` 锁策略

- **快照**：`shared_lock` — 多个并发发布可并行获取快照
- **注册/注销**：`unique_lock` — 独占访问
- **队列推送**：锁外部 — `CircularQueue::push()` 无锁 CAS

---

## 4. Socket pub/sub 快速路径

### 4.1 机制

`socket_pub_ipc::publish_best_effort()` 在 `IsNodeletEnabled()` 为 true 时：

1. 从 `LocalPubSubRegistry` 获取本地订阅者队列快照
2. 从 `IpcInfoPool` 获取全局 SocketSub 条目计数（跨进程可见）
3. 判定条件：`snapshot 非空` 且 `IpcInfoPool SocketSub 总数 == snapshot.size()`（全部已知订阅者均在本地）
4. K=3 稳定性门槛同上
5. 激活后：`msg->clone()` 一次，fanout 到所有本地订阅者队列，**完全跳过 UDP 发送**

### 4.2 IpcInfoPool 判定边界（重要限制）

IpcInfoPool 仅能发现通过 dzIPC `ScopedRegistration` 注册的订阅者（`socket_sub_ipc::InitChannel` 中注册）。以下消费者**不可检测**：

- 原生 UDP 监听器（非 dzIPC 客户端）
- 被动嗅探器
- raw socket 消费者
- 任何读取 UDP 流的外部工具

当开关开启且判定全本地时，快速路径**完全绕过 UDP 发送**——上述不可检测的消费者**收不到消息**。对调试/监控场景，保持开关关闭（默认），或对需要可见的消息使用 `publish_for_sniffer()` 强制走 UDP 路径。

### 4.3 发布流程

```
publish(msg)
  │
  ├─ IF !IsNodeletEnabled():
  │     chunk_send_ex(msg->serialize())         // UDP 路径
  │     return
  │
  ├─ snapshot = registry.subscriber_snapshot(KEY)
  ├─ total_subs = IpcInfoPool::count(SocketSub, topic, domain)
  │
  ├─ IF !snapshot.empty() AND total_subs > 0 AND total_subs == snapshot.size():
  │     consecutive_match++
  │     IF consecutive_match >= K (=3):
  │         cloned = msg->clone()
  │         FOR EACH queue IN snapshot:
  │             queue->push(cloned)
  │         return true
  │
  └─ ELSE:
        consecutive_match = 0
        emit_one_shot_warning(reason)
        chunk_send_ex(msg->serialize())         // 回退 UDP
```

---

## 5. SHM ser/cli 快速路径

### 5.1 机制

`shm_cli_ipc::send_request()` 在 `IsNodeletEnabled()` 为 true 时：

1. 从 `LocalPubSubRegistry` 查找同 `ChannelKey` 的队列
2. 判定条件：`snapshot.size() == 1`（恰好一个本地 server）
3. K=3 稳定性门槛：`server_count == 1` 连续 3 次确认后激活
4. 激活后：
   - 创建每请求的 `reply_queue`（容量 1，生命周期限于本次调用）
   - 将请求 (clone) + reply_queue 封装为 envelope 推入 server 队列
   - 在 `reply_queue` 上阻塞等待（带超时），超时返回 false，**不回退 SHM**（server 可能正在异步处理，双重发送会导致重复执行回调）
5. server 侧：`response_thread_func()` 从自己的请求队列读取、执行回调、将响应写入 reply_queue

### 5.2 关键设计点

- **每请求 reply queue**：避免多个客户端共享队列导致的响应路由错误
- **不回退 SHM**：超时后不重试 SHM，防止 server 双重执行
- **no_local_server** warning：请求时若无本地 server 注册，输出一次性 warning
- **server_count > 1** warning：注册表异常（多余一个 server 队列），输出一次性 warning

---

## 6. Socket ser/cli

Socket ser/cli 当前**无 nodelet 快速路径**。`send_request()` 始终走标准 socket 路径（`chunk_send`/`chunk_rev_server`）。`IsNodeletEnabled()` 在此传输层无影响。

---

## 7. API 覆盖

| 方法 | SHM pub/sub | Socket pub/sub | SHM ser/cli | Socket ser/cli |
|------|-------------|----------------|-------------|----------------|
| `publish()` | ✅ 快速（K=3 后） | ✅ 快速（K=3 后） | — | — |
| `publish_best_effort()` | ✅ 快速（K=3 后） | ✅ 快速（K=3 后） | — | — |
| `publish_blocking()` | **从不** | **从不** | — | — |
| `publish_for_sniffer()` | **从不** | **从不** | — | — |
| `send_request()` | — | — | ✅ 快速（K=3 后） | ❌ 仅标准路径 |

- **blocking**：始终走传统路径 — 超时语义依赖传输层流控制
- **sniffer**：始终走传统路径 — 必须写入传输层以便被动嗅探器可观测

---

## 8. 回退与警告

所有传输层在无法走快速路径时自动回退正常通信路径。warning 遵循统一规则：

| 规则 | 说明 |
|------|------|
| **每实例一次** | 每个实例按原因记录已输出的 warning，生命周期内不复位 |
| **并发安全** | SHM pub/sub 使用 atomic 位掩码；SHM ser/cli 使用 atomic bool；Socket pub/sub 状态由 fast-path mutex 保护 |
| **格式** | `"nodelet requested but unavailable; falling back ... (reason)"` |
| **原因分类** | 无本地订阅者、混合本地/远程、IpcInfoPool 不可用、server 不存在、注册表异常等 |
| **不刷屏** | 同一实例同一原因只在生命周期内打印一次 |

---

## 9. 只读约束

与 ROS1 nodelet 的 `boost::shared_ptr<const Msg>` 不同，dzIPC 使用 `std::shared_ptr<IpcMsgBase>`（可变的）。单次 `clone()` 后同一 `shared_ptr` 扇出到所有本地订阅者——任何订阅者调用非 const 方法会损坏所有共享该指针的订阅者数据。快速路径订阅者必须将消息视为只读，通过文档和约定执行。

---

## 10. 修改文件

| 文件 | 变更 |
|------|------|
| `include/dzIPC/dzipc.h` | include `nodelet_config.h`，重新导出统一开关 |
| `include/dzIPC/common/nodelet_config.h` | **新文件** — 统一开关声明 |
| `src/dzIPC/common/nodelet_config.cc` | **新文件** — 统一开关实现（atomic，默认 false） |
| `include/dzIPC/common/local_pub_sub_registry.h` | **新文件** — 进程本地注册表；key 为 `(topic, domain, msg_id, ChannelKind)` 四元组，隔离不同传输类型 |
| `src/dzIPC/common/local_pub_sub_registry.cc` | **新文件** — 注册表实现 |
| `include/ipc_msg/ipc_msg_base/ipc_msg_base.hpp` | `msg_id()` getter |
| `include/dzIPC/shm_pub_sub_ipc.h` | `msg_queue_` → `shared_ptr`；快速路径状态成员；`nodelet_warned_` atomic |
| `src/dzIPC/shm_pub_sub_ipc.cc` | `publish_best_effort()` 快速路径门控 + K=3 + warning |
| `include/dzIPC/socket_pub_sub_ipc.h` | 快速路径状态成员；warning 标志 |
| `src/dzIPC/socket_pub_sub_ipc.cc` | `publish_best_effort()` 快速路径 + IpcInfoPool 判定 + warning |
| `include/dzIPC/shm_ser_cli_ipc.h` | 快速路径状态成员；warning 标志 |
| `src/dzIPC/shm_ser_cli_ipc.cc` | `send_request()` 快速路径 + 每请求 reply queue（含内部类型 `FastPathRequestEnvelope`）+ K=3 + warning |
| `test/test_shm_nodelet.cpp` | 13 个测试（含开关、拓扑与 ChannelKind 隔离） |
| `test/test_socket_nodelet.cpp` | 11 个测试（UDP 快速路径、强制传输、隔离与远端回退） |
| `test/test_shm_ser_cli_nodelet.cpp` | 5 个测试（服务快速路径、默认关闭与异常回退） |
| `test/test_nodelet_switch.cpp` | 8 个统一开关与 warning 行为测试 |
| `docs/shm_nodelet.md` | 本文档 |

---

## 11. 验证结果

使用独立构建目录 `/tmp/cpp_ipc_dds_nodelet_build`，关闭 Python、demo 与消息生成器后完成构建和运行期验收：

| 测试目标 | 结果 |
|----------|------|
| `test_shm_nodelet` | 13/13 通过 |
| `test_socket_nodelet` | 11/11 通过；UDP 负责人另做 3 轮重复，共 33/33 通过 |
| `test_shm_ser_cli_nodelet` | 5/5 通过 |
| `test_nodelet_switch` | 8/8 通过 |
| `test_dzipc_shm` | 9/9 通过 |
| `test_dzipc_socket` | 2/2 通过 |
| `test_socket_reliable_crc` | 1/1 通过，2 项依环境跳过 |
| 安装/API | 安装成功；安装树的 `dzipc.h` 可编译；动态库导出 `EnableNodelet` 与 `IsNodeletEnabled` |

构建过程无新增编译 warning，`git diff --check -- include src test docs` 通过。

---

## 12. 风险与缓解

| 风险 | 严重程度 | 缓解 |
|------|----------|------|
| 可变 shared_ptr 数据损坏 | 中 | 文档化只读契约 |
| snapshot 与传输层计数之间的 TOCTOU | 中 | K=3 稳定性抑制波动；新加入的远程对等方可能错过单条消息但在下一次发布时恢复 |
| Socket IpcInfoPool 不可检测原生监听器 | 中 | 默认关闭；文档说明限制；需显式 `publish_for_sniffer()` 保证 UDP 可见性 |
| 跨进程崩溃后计数残留 | 低 | 传输层最终检测到死亡对等方；短暂窗口期内优雅回退 |
| SHM ser/cli 每请求 reply queue 分配 | 低 | 容量 1 的队列，开销极小；仅在快速路径激活时分配 |
| `nodelet_warned_` 位永不复位 | 低 | 有意设计 — 每实例每原因严格一次；如需重新启用 warning，销毁重建实例 |

---

## 13. 总结

同进程快速路径是**按传输层增量实现的优化**，由**统一进程级开关**控制：

- **默认关闭** — 向后兼容，sniffer 始终可见，无意外行为变更
- **显式开启** — `dzIPC::EnableNodelet(true)` 后各传输在条件满足时自动走快速路径
- **优雅回退** — 条件不满足时回退传统路径，按实例/原因输出一次 warning
- **K=3 门槛** — 连续 3 次稳定拓扑观察后才激活，抑制波动
- **单次 clone 扇出** — SHM/Socket pub/sub 共享同一模式
- **SHM ser/cli** — 每请求 reply queue，要求 server 唯一
- **Socket ser/cli** — 暂无快速路径
