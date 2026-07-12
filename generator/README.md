# ROS2 风格消息生成器

这个工具集可以将 ROS2 风格的`.msg`文件转换为包含序列化/反序列化功能的 C++头文件。

## 文件说明

- `msg_generator.py` - 单个消息文件生成器
- `batch_msg_srv_generator.py` - 批量处理 `.msg/.srv`，并自动更新 `python/interface.cc` 的 pybind 绑定块

## Python 快速配置（自定义消息）

当你新增或修改 `msg/**/*.msg`、`srv/**/*.srv` 后，只需要执行：

```bash
python3 tools/batch_msg_srv_generator.py
```

脚本会自动完成三件事：

1. 重新生成 `include/ipc_msg/**` 消息头文件
2. 重新生成 `include/ipc_srv/**` 服务头文件
3. 自动刷新 `python/interface.cc` 中以下两段：
	- `AUTO_GENERATED_MSG_SRV_INCLUDES`
	- `AUTO_GENERATED_MSG_SRV_BINDINGS`

Python 侧可直接使用自动工厂接口进行配置驱动调用：

```python
import dzipc_cpp as ipc

print(ipc.message_types())
msg = ipc.create_message("TestMsg")

print(ipc.service_types())
req = ipc.create_service_request("RequestResponseTest")
res = ipc.create_service_response("RequestResponseTest")
```

## 支持的数据类型

### 基本类型

- `bool` → `bool`
- `int8` → `int8_t`
- `uint8` → `uint8_t`
- `int16` → `int16_t`
- `uint16` → `uint16_t`
- `int32` → `int32_t`
- `uint32` → `uint32_t`
- `int64` → `int64_t`
- `uint64` → `uint64_t`
- `float32` → `float`
- `float64` → `double`
- `string` → `std::string`

### 数组类型

- 所有基本类型的数组，如 `int32[]` → `std::vector<int32_t>`
- 字符串数组 `string[]` → `std::vector<std::string>`
- 固定大小数组，如 `int32[10]` → `std::array<int32_t, 10>`

## 使用方法

### 单个文件处理

```bash
python3 tools/msg_generator.py <msg文件路径>
```

示例：

```bash
python3 tools/msg_generator.py msg/unitree_dds_ipc/unitree_dds_ipc.msg
```

### 批量处理

```bash
python3 tools/batch_msg_generator.py <输入目录> [输出目录]
```

示例：

```bash
# 在原目录生成
python3 tools/batch_msg_generator.py msg/

# 指定输出目录
python3 tools/batch_msg_generator.py msg/ generated/
```

## 消息文件格式

`.msg` 文件采用 ROS2 标准格式，每行一个字段：

```
# 示例消息定义
bool status
int32 count
float64[] values
string[] names
```

注释以 `#` 开头。

## 生成的 C++类

生成的类继承自 `shm_ipc_datastruct` 并包含：

- 所有消息字段作为公共成员变量
- `serialize()` 函数：将对象序列化为字节流
- `deserialize()` 函数：从字节流恢复对象

### 使用示例

```cpp
#include "generated_message.hpp"

// 创建消息对象
my_message_raw msg;
msg.status = true;
msg.count = 42;
msg.values = {1.0, 2.0, 3.0};
msg.names = {"hello", "world"};

// 序列化
std::vector<char> data = msg.serialize();

// 反序列化
my_message_raw restored;
restored.deserialize(data);
```

## 编译要求

- C++14 或更高版本
- 需要包含基类 `shm_ipc_datastruct.h`

## 测试

项目包含测试程序验证序列化/反序列化功能：

```bash
cd test
g++ -std=c++14 -I../ test_generated_msg.cpp -o test_generated_msg
./test_generated_msg
```

## 故障排除

### 编译错误

1. 确保包含路径正确
2. 检查基类文件是否存在
3. 使用 C++14 或更高标准

### 路径问题

生成的头文件使用相对路径引用基类，确保项目结构正确。

## 限制

当前版本不支持：

- 嵌套消息类型
- 常量定义
- 服务定义

## 扩展

要添加新的数据类型支持，修改 `msg_generator.py` 中的 `TYPE_MAPPING` 字典。
