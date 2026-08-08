# dzipc_topic_cat

嗅探 dzIPC topic 或 service 的实时消息，类似网络工具 `tcpdump` 或 `wireshark`，以只读方式接入已有通道，不影响正常通信。

- **Topic 模式**：嗅探发布/订阅的消息内容，实时显示每条消息的字段值。
- **Service 模式**：嗅探请求/响应（Request/Response），同时显示两端的报文内容。

## 快速上手

``` bash
# 嗅探 topic 消息
./dzipc_topic_cat -t my_topic -s false

# 嗅探 service 请求/响应
./dzipc_topic_cat -t my_service -s true
```

## 参数说明

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--topic` | `-t` | string | **必填** | 要嗅探的 topic 或 service 名称 |
| `--ser_or_topic` | `-s` | bool | **必填** | `true` = service 模式，`false` = topic 模式 |
| `--msg_id` | `-m` | int | `0` | 按消息 ID 过滤（0 表示不过滤，接收所有 msg_id） |
| `--freq` | `-f` | int | `20` | 接收/刷新频率（Hz），0 表示使用默认 20Hz |

## 使用示例

### 嗅探 topic 消息

``` bash
./dzipc_topic_cat -t robot_state -s false
```

输出示例：

```
=====================================================

Topic: robot_state                    Type: Topic(SHM)               Msg ID: 0

------------------------------------------------------
Message:
x: 42
y: 100
name: robot_01
```

### 嗅探 service 请求/响应

``` bash
./dzipc_topic_cat -t calc_service -s true
```

输出示例：

```
=====================================================

Topic: calc_service                   Type: Service(SOCKET)          Msg ID: 0

------------------------------------------------------
Request:
a: 10
b: 20
Response:
result: 30
```

### 只接收指定 msg_id 的消息

``` bash
./dzipc_topic_cat -t my_topic -s false -m 100
```

### 降低刷新频率以减少 CPU 占用

``` bash
./dzipc_topic_cat -t my_topic -s false -f 5
```

### 提高刷新频率以获得更低延迟

``` bash
./dzipc_topic_cat -t my_topic -s false -f 50
```

## 工作原理

1. **自动发现**：从 IpcInfoPool 中查找目标 topic/service 的活跃发布端/服务端，自动识别传输方式（SHM 或 SOCKET）。
2. **等待连接**：如果目标 topic/service 尚未发布，工具会显示等待提示（黄色文字），持续轮询直到发布端上线。
3. **实时接收**：建立只读嗅探连接后，按指定频率接收并显示消息内容。消息内容为空时保留上一次的缓存显示，避免闪烁。
4. **自动重连**：发布端下线或传输方式变更时，工具自动断开并重新进入等待状态。

## 注意事项

- 嗅探连接为只读，不会影响原始 topic/service 的通信。
- 消息内容通过 `msg_to_string` 反射序列化，显示效果取决于消息类型的反射支持。
- 按 Ctrl+C 可随时退出。
