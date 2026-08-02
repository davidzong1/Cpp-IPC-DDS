# Cpp-IPC+： C++ IPC Library Like **DDS**

## A high-performance inter-process communication library using shared memory and UDP on Linux/Windows

- 🌟🌟🌟The communication method is similar to DDS
- Compilers with C++17 support are recommended (msvc-2017/gcc-7/clang-4)
- No other dependencies except STL.
- Only lock-free or lightweight spin-lock is used.
- Circular array is used as the underline data structure.

---

## 🌟Addition

- 增加类似与 dds 的话题通信模式以及 srv 通信模式，例程参考`test/test_dzipc.cpp`和`test/test_complex_msg.cpp`
- 支持自动生成 msg 和 srv 头文件
- 执行`scripts/install.sh`自动更新相关话题文件
- 增加 ros2 构建选项
- 增加python调用api
- 增加可手动开启的 Nodelet 同进程快速路径，支持 SHM/UDP pub-sub 与 SHM ser-cli
- 增加进程级 `dzipc_log`，可把 pub/sub 与 srv/cli 通信记录为 ROS1 bag v2.0 文件

---

## 🚀 Nodelet（同进程快速路径）

Nodelet 用于优化同一进程内的通信。当拓扑满足条件时，消息通过 `shared_ptr` 直接传递，跳过序列化和 SHM/UDP 传输。该功能为进程级可选优化，**默认关闭**，现有 publisher、subscriber、server 和 client 的创建方式无需修改。

| 通信模式 | Nodelet 支持 |
|----------|--------------|
| SHM pub/sub | 已支持 |
| UDP（`IPC_SOCKET`）pub/sub | 已支持 |
| SHM ser/cli | 已支持 |
| Socket ser/cli | 暂不支持，始终使用普通 Socket 通信 |

通过顶层头文件手动控制：

```cpp
#include "dzIPC/dzipc.h"

// 默认值为 false。建议在创建通信对象前统一设置。
dzIPC::EnableNodelet(true);

// publisher/subscriber/server/client 仍按原有方式创建和使用。
bool enabled = dzIPC::IsNodeletEnabled();

// 可在运行时关闭，后续通信立即恢复普通传输路径。
dzIPC::EnableNodelet(false);
```

`EnableNodelet(true)` 只表示允许尝试快速路径，并非强制启用。只有通信端位于同一进程、已知对端全部为本地端，并连续 3 次观察到稳定拓扑时才会启用；SHM ser/cli 还要求只有一个本地 server。任一条件不满足时会自动回退到普通 SHM/UDP 通信，并按实例和原因输出一次 warning，避免重复刷屏。

调试与 sniffer 注意事项：

- `publish_for_sniffer()` 和 `publish_blocking()` 始终使用真实传输层，不受 Nodelet 快速路径影响。
- UDP Nodelet 生效时会跳过 UDP 发送，原生 UDP 监听器或被动 sniffer 无法收到这类普通 `publish()` 消息。
- 需要完整抓取通信数据时，应保持 Nodelet 关闭（默认状态）；也可以对指定消息调用 `publisher->publish_for_sniffer(msg)` 强制写入传输层。

完整的判定机制、回退策略、测试结果和已知限制见 [Nodelet 设计文档](docs/shm_nodelet.md)。

---

## dzipc_log（进程级通信日志）

`dzipc_log` 默认关闭。启用后会自动记录由 dzIPC 公共工厂创建的 publisher、subscriber、server 和 client 通信，包括 Nodelet 快速路径，并在停止或退出时写入单个 `.bag` 文件：

```cpp
#include "dzIPC/dzipc.h"

dzIPC::logger::StartDzipcLog("/tmp/session.bag");
// 正常创建并使用 pub/sub、srv/cli
dzIPC::logger::StopDzipcLog();
```

也可通过 `StartDzipcLog(path, max_memory_mb, max_queue_size)` 设置 chunk 内存预算和待处理事件上限。队列满时日志事件会被丢弃，但通信不会被阻塞。输出采用 ROS1 bag v2.0、`compression=none`，业务 payload 以 `dzipc_log/TransportPacket` 中的 opaque bytes 保存；它不是 ROS 原生业务消息，需要按 dzIPC 消息定义解码。

完整 API、退出 flush 语义、Nodelet 性能影响和 bag 兼容边界见 [dzipc_log 文档](docs/dzipc_log.md)。

---

## Usage

#### Install

直接执行`scripts/install.sh`或文件安装


#### C++ Interface

与 dds 使用方法类似，在 msg 和 srv 文件夹下创建消息文件，然后运行`scripts/install.sh`编译安装后在自己项目上调用头文件即可，格式参考`test/test_dzipc.cpp`。

#### Python Interface

进入`python`文件夹后输入`pip install .`进行安装后在脚本中`import dzipc`即可，与 C++ 用法类似，参考`python/ipc_demo.py`

#### TUI界面

可以使用`tui.py`可视化实现脚本管理


#### Tools(ubuntu下直接安装至`/usr/bin`中)

`dzipc_list`：查看当前本地所有**Topic**
- `-w`：持续查看(可选)

`dzipc_topic_cat`:
- `-t`：**Topic**名字(必选) 
- `-s`：服务模式还是话题模式通信(必选) **(True为service，False为Publish)**

---

#### Update msg and srv

可以使用`scripts/install.sh`脚本进行更新(只能更新默认目录)，也可以使用`scripts/update_msg_srv.sh`进行更新(可以传入外部路径)使用方法：
```shell
# 指定路径（累加到已有配置）
scripts/update_msg_srv.sh --msg <PATH1> --srv <PATH2>
scripts/update_msg_srv.sh --msg <PATH1> --msg <PATH2>   # 累加，不覆盖

# 使用已保存配置更新
scripts/update_msg_srv.sh

# 清除配置
scripts/update_msg_srv.sh --reset

# 帮助
scripts/update_msg_srv.sh --help
```

## Test (TODO)

# Reference

🌟[Cpp-IPC](https://github.com/mutouyun/cpp-ipc)
