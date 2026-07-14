# dzipc_pub

向 dzIPC topic 发布消息的命令行工具。工具会自动从 IpcInfoPool 中查找 subscriber，解析其声明的消息类型对应的 `.msg` 文件来构建消息 schema，并填充默认值。可通过 `-f` 覆盖指定字段的值。

## 快速上手

``` bash
# 向 my_topic 发布消息，覆盖 x 和 y 字段，每秒 2 条，共发 10 条
./dzipc_pub -t my_topic -f x:int32:42 -f y:float64:3.14 --freq 2 --count 10

# 持续发布，每秒 1 条（默认），直到 Ctrl+C
./dzipc_pub -t my_topic -f name:string:hello
```

## 参数说明

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--topic` | `-t` | string | **必填** | 要发布到的 topic 名称 |
| `--field` | `-f` | string | — | 字段覆盖，格式 `name:type:value`，可重复使用 |
| `--msg-dir` | — | string | `msg` | `.msg` 文件所在目录路径 |
| `--mode` | `-m` | string | `auto` | 传输方式：`auto`（自动）、`shm`、`socket` |
| `--domain` | `-d` | int | 自动检测 | Domain ID，-1 表示自动从 subscriber 获取 |
| `--msg_id` | — | int | 自动检测 | 消息类型 ID，-1 表示自动从 subscriber 获取 |
| `--freq` | — | int | `1` | 发布频率（Hz），必须 > 0 |
| `--count` | `-c` | int | `0` | 发布条数，0 表示无限发布直至 Ctrl+C |

## 使用示例

### 覆盖多个字段

``` bash
./dzipc_pub -t robot_state -f x:int32:100 -f y:int32:200 -f name:string:robot_01
```

### 强制使用 shm 传输

``` bash
./dzipc_pub -t my_topic --mode shm -d 1 --freq 5
```

### 强制使用 socket 传输

``` bash
./dzipc_pub -t my_topic --mode socket -d 0
```

### 手动指定 msg_id

``` bash
./dzipc_pub -t my_topic --msg_id 100 -f name:string:hello
```

### 发布固定条数后自动退出

``` bash
./dzipc_pub -t my_topic -f value:float64:3.14 --count 50 --freq 10
```

### 指定自定义 msg 目录

``` bash
./dzipc_pub -t my_topic -f data:int32:42 --msg-dir /path/to/custom/msg
```

## 支持的字段类型

`-f name:type:value` 中的 type 支持以下类型：

| 类型 | 示例 |
|------|------|
| `bool` | `-f flag:bool:true` |
| `int8` | `-f a:int8:-128` |
| `uint8` | `-f b:uint8:255` |
| `int16` | `-f c:int16:-32768` |
| `uint16` | `-f d:uint16:65535` |
| `int32` | `-f e:int32:42` |
| `uint32` | `-f f:uint32:100` |
| `int64` | `-f g:int64:-9999999999` |
| `uint64` | `-f h:uint64:9999999999` |
| `float32` | `-f i:float32:3.14` |
| `float64` | `-f j:float64:2.718281828` |
| `string` | `-f k:string:hello_world` |

> **注意**：数组类型（如 `int32[]`）和嵌套消息类型（nested）需通过 `.msg` 文件定义 schema，暂不支持通过 CLI `-f` 直接覆盖。

## 工作原理

1. **查找 subscriber**：从 IpcInfoPool 中查找目标 topic 的活跃 subscriber，获取其声明的 `type_name`、`domain_id` 和传输方式（shm/socket）。
2. **解析 schema**：在 `--msg-dir` 目录下递归查找与 `type_name` 匹配的 `.msg` 文件，解析字段定义。
3. **构建消息**：根据 schema 创建消息实例并填充默认值（数值类型为 0，字符串为空，bool 为 false）。
4. **应用覆盖**：将 `-f` 指定的字段值覆盖到消息上。
5. **发布**：按 `--freq` 频率循环发布，每 10 条自动检查 subscriber 是否仍然存活，若 subscriber 全部下线则自动停止。
