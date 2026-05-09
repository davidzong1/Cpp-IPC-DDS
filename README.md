# Cpp-IPC+： C++ IPC Library Like **DDS**

## A high-performance inter-process communication library using shared memory and UDP on Linux/Windows

- 🌟🌟🌟The communication method is similar to DDS
- Compilers with C++17 support are recommended (msvc-2017/gcc-7/clang-4)
- No other dependencies except STL.
- Only lock-free or lightweight spin-lock is used.
- Circular array is used as the underline data structure.

---

## 🌟Adition

- 增加类似与 dds 的话题通信模式以及 srv 通信模式，例程参考`test/test_dzipc.cpp`和`test/test_complex_msg.cpp`
- 支持自动生成 msg 和 srv 头文件
- 执行`install.sh`自动更新相关话题文件
- 增加 ros2 构建选项
- 增加python调用api

---

## Usage

#### Install

直接执行`install.sh`或文件安装


#### C++ Interface

与 dds 使用方法类似，在 msg 和 srv 文件夹下创建消息文件，然后运行`install.sh`编译安装后在自己项目上调用头文件即可，格式参考`test/test_dzipc.cpp`。

#### Python Interface

进入`python`文件夹后输入`pip install .`进行安装后在脚本中`import dzipc`即可，与 C++ 用法类似，参考`python/ipc_demo.py`

#### Tools(ubuntu下直接安装至`/usr/bin`中)

`dzipc_list`：查看当前本地所有**Topic**
- `-w`：持续查看(可选)

`dzipc_topic_cat`:
- `-t`：**Topic**名字(必选) 
- `-s`：服务模式还是话题模式通信(必选) **(True为service，False为Publish)**

---

## Test (TODO)

# Reference

🌟[Cpp-IPC](https://github.com/mutouyun/cpp-ipc)