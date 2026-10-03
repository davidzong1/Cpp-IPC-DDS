# 混合发布订阅改造结果

日期：2026-10-03

本轮将 `IPC_SOCKET` 发布订阅改为混合路径：本机优先使用 SHM，存在跨机或 `IPC_SOCKET_ONLY` 订阅者时使用 UDP；`IPC_SHM` 保持纯 SHM，`IPC_SOCKET_ONLY` 保持纯 UDP。混合端点在信息池中只登记一条逻辑记录，内部 SHM/UDP 腿不重复显示。

## 已完成

- 增加按话题和 domain 的混合发现公告，使用共享内存实例身份区分本机 SHM 可达性，并用租约清理退出订阅者。
- UDP 混合帧保留 32 字节作用域，并加入来源共享内存身份、发布实例和序号；本机已由 SHM 投递的网络副本在接收端抑制，避免重复交付。
- `IPC_SOCKET` 普通发布接口优先走 SHM，借样/嗅探和可靠发布仍走 UDP；不支持 DzFlat 的消息保留序列化路径。
- `dzlist`、`topic_cat` 和 Python 观测路径按逻辑混合端点工作，不同时读取两条腿。
- SocketOnly 订阅者也加入混合发现，因此混合发布者能在晚加入、退出租约和纯 UDP 订阅场景下正确切换网络发送。
- 明确底层 `ipc::route` 是单发布者、多订阅者模型；多发布源场景由一个混合 SHM 发布者配合 `IPC_SOCKET_ONLY` 发布者完成，后者通过 UDP 到达混合订阅者。

## 验证

`/tmp/cppipc-hybrid-units/build/bin/test_hybrid_pubsub` 在沙箱外运行，7/7 通过：

1. 默认 `IPC_SOCKET` 使用 SHM 且只登记一个逻辑端点。
2. SHM 与 SocketOnly 同时存在时无重复交付。
3. SocketOnly 发布者与混合订阅者互通。
4. 晚加入 SocketOnly 订阅者触发 UDP，租约过期后停止发送。
5. 阻塞读取可由任一传输腿唤醒。
6. domain 隔离及多发布源（混合 SHM + SocketOnly UDP）通过。
7. `topic_cat` 选择逻辑混合端点并只读取一次。

其它验证：`test_topic_cat_select` 15/15 通过；`test_ipc_info_pool_version` 2/2 通过；相关目标成功构建；Python 工具 `py_compile` 通过；`git diff --check` 通过。

## 限制

容器组播在部分旧 UDP 集成用例中受 `EPERM` 限制，因此不能把这些用例作为网络权限完整环境下的证据。当前混合专用集成测试已在获授权的沙箱外命令中通过。跨机统一接收转发节点未在本轮实现。
