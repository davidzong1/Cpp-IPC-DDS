# dzipc_pub

向 dzIPC topic 发布消息的命令行工具（对标 `rostopic pub`）。工具会自动从 IpcInfoPool 中查找 subscriber，解析其声明的消息类型对应的 `.msg` 文件来构建消息 schema 并填充默认值；也可用 `--type` 显式指定类型在无订阅者时发布。字段值可通过 `-f` 覆盖，类型自动从 schema 推断。

## 快速上手

``` bash
# 向 my_topic 发布消息，覆盖 x 和 y 字段，每秒 2 条，共发 10 条
./dzipc_pub -t my_topic -f x=42 -f y=3.14 -r 2 -c 10

# 只发一条（等订阅者握手完成后发出）并退出，类似 rostopic pub -1
./dzipc_pub -t my_topic -f name=hello --once

# 持续发布，每秒 1 条（默认），直到 Ctrl+C
./dzipc_pub -t my_topic -f name=hello

# 无订阅者时显式指定类型发布
./dzipc_pub -t my_topic -T StdVector -f x=1.0 -d 0
```

## 参数说明

| 参数 | 简写 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| `--topic` | `-t` | string | **必填** | 要发布到的 topic 名称 |
| `--type` | `-T` | string | 自动检测 | 消息类型名（PascalCase）；无订阅者时必须指定 |
| `--field` | `-f` | string | — | 字段覆盖，`name=value` 或旧式 `name:type:value`，可重复 |
| `--file` | — | string | — | 从文件读取字段覆盖（每行一条，`#` 注释） |
| `--msg-dir` | — | string | `msg` | `.msg` 文件所在目录路径 |
| `--mode` | `-m` | string | `auto` | 传输方式：`auto`（跟随订阅者）、`shm`、`socket` |
| `--domain` | `-d` | int | 自动检测 | Domain ID，-1 表示自动从 subscriber 获取 |
| `--msg_id` | — | int | `0` | 消息 ID；订阅者会丢弃 msg_id 不匹配的消息，需与其一致 |
| `--rate` | `-r` | double | `1.0` | 发布频率（Hz），支持小数如 `0.5` |
| `--count` | `-c` | int | `0` | 发布条数，0 表示无限发布直至 Ctrl+C |
| `--once` | `-1` | flag | — | 只发布一条后退出（等价于 `-c 1`） |
| `--wait` | `-w` | double | `5.0` | 发布前等待订阅者握手的秒数（仅 shm；0 关闭） |

## 字段覆盖语法

类型自动从 `.msg` schema 查得，无需手写：

| 场景 | 示例 |
|------|------|
| 标量 | `-f x=42`、`-f ratio=3.14`、`-f ok=true` |
| 字符串（可含空格） | `-f label=hello world` |
| 数组 | `-f samples=[1,2,3]` 或 `-f samples=1,2,3` |
| 字符串数组 | `-f tags=[alpha,beta]` |
| 清空数组 | `-f samples=[]` |
| 嵌套字段（点路径） | `-f position.x=1.5`、`-f pose.header.frame=map` |
| 旧式带类型（会校验与 schema 一致） | `-f x:int32:42` |

校验规则：字段名不存在、类型不符、数值越界（如 `int8` 传 129）均直接报错退出，不会把打错的字段静默发出去。

> **注意**：嵌套消息数组（`pose[]`）暂不支持通过 CLI 覆盖，保持空数组发布。

## 工作原理

1. **查找 subscriber**：从 IpcInfoPool 中查找目标 topic 的活跃 subscriber，获取其声明的 `type_name`、`domain_id` 和传输方式（shm/socket）。若无订阅者且给定 `--type` 则按参数发布。
2. **解析 schema**：在 `--msg-dir` 目录下递归查找与类型匹配的 `.msg` 文件；嵌套类型递归解析。
3. **构建消息**：按 schema 填默认值（数值 0、字符串空、bool false、数组空、嵌套递归构建）。
4. **应用覆盖**：将 `-f` / `--file` 指定的值覆盖到消息上（严格校验）。
5. **建立通道**：创建 publisher 并 `InitChannel()`；shm 模式下等待订阅者握手完成（`--wait`），避免首条丢失。
6. **回显与发布**：打印将要发出的完整消息内容，然后按 `--rate` 循环发布；每 2 秒检查一次订阅者是否仍然存活（通过自动发现建立的会话），全部下线则停止。
