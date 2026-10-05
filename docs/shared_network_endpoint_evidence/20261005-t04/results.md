# T04：网关独占启动与本机会话

基线 `dae9ca0`，实现与本记录同一次提交。完成 T04；业务数据面仍未就绪，公共工厂保持 legacy。

## 实现

- `serve` 前台运行，SIGINT/SIGTERM 有序退出；`status --control PATH --json` 查询真实控制会话。
- Unix SEQPACKET 使用逐层无符号链接目录、0700 目录、0600 持有期 flock 和 socket。
  SIGSTOP 不移交锁；获锁后只清理本 UID、明确拒绝连接的旧 socket；析构核验 inode。
- HELLO 核验 UID、locality、进程启动标识和时钟命名空间；WELCOME 分配随机 epoch、会话和初始信用。
- 同进程千引用共用一个健康连接，初始化不持全局锁握手；不同控制路径不能同时建立健康实例。
  PID 检查先于继承锁；旧离线实例不能迁移身份或信用。
- 单接收循环按 request_id 和预期响应类型唤醒请求；有限输出、会话、请求历史与可靠等待表。
  重复请求复用缓存，相同 ID 内容冲突拒绝；累计通知允许重复/倒退，拒绝不可比较的计数对。
- 可靠等待表覆盖早到结果、身份匹配、超时和网关失联，以及第 5 节两腿聚合。

## 验证

```sh
cmake -S . -B build-shared-net
cmake --build build-shared-net --target test_shared_net_gateway_lock test_shared_net_client_lifecycle --parallel 4
ctest --test-dir build-shared-net -L shared_net --output-on-failure
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-shared-net-sanitize -R 'test_shared_net_(gateway_lock|client_lifecycle)$' --output-on-failure
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
```

- 新增 14 条 GTest：进程级锁/SIGSTOP/SIGKILL 后重启、目录/锁/socket 权限、符号链接、替换 inode、UDP 启动失败清理；
  千句柄复用、32 并发查询、信用和请求重放、零初始信用、错误 locality/clock、附带 FD 归还、早到结果、
  128 等待者断连、旧对象隔离、fork、慢读者、累计通知及路由版本、错误回复和请求超时。
- 共享网络 CTest **10/10**，ASan/UBSan 新增目标 **2/2**（包含全部 14 条新用例），后端 OFF **2/2**。
- 初轮随机选端口遇到占用，夹具已改为实际探测空闲的整组端口；没有修改系统网络配置。
- 假网关夹具的 future.get 后析构 wait 曾触发 no_state，已修复，最终验证如附带日志。

## 边界

status 明确输出 `ControlReady`、`data_plane_ready=false`。ATTACH_TX、业务注册、发现和网络消息处理由后续任务接入。
本卡信用仅为预授予和协议验证，不代表 T05/T10 两类占用账本已完成。
所有测试使用独占临时目录与自己的进程；未清理全局 SHM、未终止其他进程。
