# dzipc_list

列出当前所有活跃的 dzIPC 条目（topic / service 的发布端和订阅端信息）。支持一次性快照和持续刷新两种模式，可按 kind 或 topic 名称过滤。

## 快速上手

``` bash
# 一次性列出所有活跃条目
./dzipc_list

# 持续刷新，类似 top
./dzipc_list -w
```

## 参数说明

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--watch` | `-w` | flag | 关闭 | 持续刷新模式，类似 `top`，Ctrl+C 退出 |
| `--interval` | `-i` | int | `500` | 刷新间隔（毫秒），最小 100ms |
| `--kind` | `-k` | string | — | 按 kind 过滤，不指定则显示所有类型 |
| `--topic` | `-t` | string | — | 按 topic 名称子串过滤 |
| `--verbose` | `-v` | flag | 关闭 | 显示 extra / timestamps 等详细字段 |
| `--reset` | — | flag | — | 清空共享内存池（需所有 dzIPC 进程已退出） |
| `--help` | `-h` | flag | — | 显示帮助信息 |

## 使用示例

### 一次性列出所有条目

``` bash
./dzipc_list
```

输出示例：

```
ID  KIND           PID     AGE     TOPIC                           TYPE                           EXTRA
0   shm_pub        12345   5s      robot_state                     RobotState                     -
1   shm_sub        12346   3s      robot_state                     RobotState                     -
2   socket_pub     12347   10s     sensor_data                     SensorData                     -

Total: 3 entries
```

### 持续刷新（类似 top）

``` bash
./dzipc_list -w
```

### 自定义刷新间隔为 1 秒

``` bash
./dzipc_list -w -i 1000
```

### 只显示 shm_pub 类型的条目

``` bash
./dzipc_list -k shm_pub
```

### 按 topic 名称子串过滤

``` bash
./dzipc_list -t robot
```

### 组合过滤 + 详细模式

``` bash
./dzipc_list -k socket_sub -t sensor -v
```

### 清空共享内存池

``` bash
./dzipc_list --reset
```

> **注意**：`--reset` 会清空整个 IpcInfoPool 共享内存，执行前需确保所有 dzIPC 进程已退出，否则可能导致数据不一致。

## 支持的 kind 类型

| kind | 说明 |
|------|------|
| `shm_pub` | 共享内存发布端 |
| `shm_sub` | 共享内存订阅端 |
| `shm_server` | 共享内存服务端 |
| `shm_client` | 共享内存客户端 |
| `socket_pub` | Socket 发布端 |
| `socket_sub` | Socket 订阅端 |
| `socket_server` | Socket 服务端 |
| `socket_client` | Socket 客户端 |

## 输出列说明

| 列名 | 说明 |
|------|------|
| ID | 条目在共享内存池中的 slot 编号 |
| KIND | 条目类型（shm_pub / socket_sub 等） |
| PID | 进程 ID |
| AGE | 条目自注册以来的存活时间 |
| TOPIC | topic 或 service 名称 |
| TYPE | 消息类型名称（如 `RobotState`） |
| EXTRA | 额外信息（`-v` 模式下显示更多详情） |
