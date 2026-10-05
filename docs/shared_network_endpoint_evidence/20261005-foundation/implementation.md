# 首批实施记录：T01～T03

实施依据为用户批准的完整方案，源码基线 `f066a82`。本批完成配置、纯协议、载荷和裸 UDP
基础；T04～T15 尚未实施。T00 的能力补证已完成一部分，规模、延迟与跨机结果仍待采集。

## 实现

- **T01**：严格解析 `legacy/shared_v1`、首次网络入口采样、前置能力错误码、地址/端口/网卡/
  Unix 路径验证、双信用容量与元数据配额。支持 64 位 Linux ON/OFF 构建，配置解析在 OFF
  下仍链接。模式错误在创建原始 pimpl 前抛出，避免失败构造泄漏。修复 CMake 覆盖显式构建类型。
- **T02**：DZMX 160B、DZGD 64B、DZGC 84B、DZLC v2 40B、DZTX v2 112B；规范 RouteKey、
  完整名称指纹校验、PUB/SUB 角色、CRC32C、所有本机消息 body 边界、发送结果与两类累计信用。
  独立 Python 生成 [参考字节向量](../../../test/shared_net/golden_vectors.json)，C++ 对拍。
  WireBlob 私有不可变容量固定为 `W(n)`；TLV 保留全部历史页尾，GenericMessage 平坦段优先原样保留。
- **T03**：非阻塞 UDP、实际源地址、截断/空包/WouldBlock/Fatal 区分、sendmmsg/recvmmsg、
  部分发送前缀计数、固定 K+2 个 socket、显式网卡发现组播及 TTL=1。IO 循环复用既有
  SocketWaitSet 的注册代次，先摘除再关闭；按报文、字节和时间预算轮转；批量已读后缀有界保留。
  处理回调可注销端点；回调异常也会保留尚未处理的批量后缀。

`dzipc_gateway check-config` 已可运行；`serve/status` 明确返回 NotImplemented。
默认仍为 legacy；共享网络对象尚不进入业务数据面。

## 验证

| 构建/检查 | 结果 |
|---|---|
| RelWithDebInfo / 共享后端 ON | 8/8 CTest 目标通过；41 个 GTest 用例 + 1 个 Python 向量检查；0 跳过 |
| 共享后端 OFF | 2/2 CTest 目标通过：配置 10 例、SHM 能力 3 例；0 跳过 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 协议、blob、本机协议、端点、IO 共 5/5 目标通过；0 跳过 |
| 旧路径聚焦回归 | test_shm 8、test_channel_scope 11、test_shm_sniffer_control_name 3，共 22/22 通过 |
| 工具构建 | dzipc_gateway、dzipc_list、dzipc_topic_cat 成功 |
| UDP 实测 | K=1/4/16 均为 K+2；1000 RouteKey 不增端点；100 个不同目标共用同一 socket |
| 故障注入 | 截断包、第二包广播权限失败形成部分发送、停止唤醒、FD 复用、慢回调、回调注销/异常 |

最终日志与二进制/源码 SHA-256 见本目录的 `validation_manifest.json`。
入库文本日志仅移除行尾空格/制表符；清单同时记录原始输出与入库文件的 SHA-256。
旧完整回归的 8 个已知失败未在本批解决，详见 [基线](baseline.md)。本批没有重跑无关全套 CTest。

复现命令（仓库根目录）：

```bash
cmake -S . -B build-shared-net \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_DEBUG_INFO=OFF \
  -DLIBIPC_BUILD_TESTS=ON -DLIBIPC_BUILD_PYTHON=OFF \
  -DUPDATA_MSG_SRV_GENERATOR=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-shared-net --target \
  dzipc_gateway dzipc_list dzipc_topic_cat \
  test_shared_net_config test_shared_net_wire test_shared_net_blob \
  test_shared_net_local_protocol test_shared_net_endpoint \
  test_shared_net_io_lifecycle test_shared_net_shm_capacity \
  test_shm test_channel_scope test_shm_sniffer_control_name --parallel 4
ctest --test-dir build-shared-net -L shared_net -N
ctest --test-dir build-shared-net -L shared_net --output-on-failure
build-shared-net/bin/test_shm
build-shared-net/bin/test_channel_scope
build-shared-net/bin/test_shm_sniffer_control_name

cmake -S . -B build-shared-net-off \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_DEBUG_INFO=OFF \
  -DLIBIPC_BUILD_TESTS=ON -DLIBIPC_BUILD_PYTHON=OFF \
  -DUPDATA_MSG_SRV_GENERATOR=OFF -DDZIPC_BUILD_SHARED_NET=OFF
cmake --build build-shared-net-off --target \
  test_shared_net_config test_shared_net_shm_capacity --parallel 4
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure

cmake -S . -B build-shared-net-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_DEBUG_INFO=OFF \
  -DLIBIPC_BUILD_TESTS=ON -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-shared-net-sanitize --target \
  test_shared_net_wire test_shared_net_blob test_shared_net_local_protocol \
  test_shared_net_endpoint test_shared_net_io_lifecycle --parallel 4
ctest --test-dir build-shared-net-sanitize \
  --tests-regex '^test_shared_net_(wire|blob|local_protocol|endpoint|io_lifecycle)$' \
  --output-on-failure
```

## 失败与修正

保留首次失败输出，不调整断言或增加等待掩盖问题：

- 配置测试的整数列表类型不一致、能力探针调用了不存在的 loan 方法，按真实 API 修正。
- 出站段名前缀长度手工算错，改用字符串字面量长度，并检查名称内身份与会话头一致。
- 旧 TLV 跨页先递增 now_page，不能按新网络分片序号校验；增加明确的字节保留断言。
- IO 回调可改变就绪队列长度，复核时补充空队列检查与测试。

## 接续边界

下一张卡为 **T04**：实例目录权限、flock、Unix SOCK_SEQPACKET 会话、身份与时钟域验证、
每进程运行时、请求分发和断连唤醒。仅有协议编解码不代表会话鉴权已实现。
T05 还必须验证出站 route 的 force_push 原子接管与两类信用守恒，T06 才实现重组/去重。
仅有数据 shard 计算不代表错误分片的重组隔离已经集成；该行为需在 T06/T10 再验证。
STATE 的 JSON 业务内容由后续状态层解释，目前协议层校验其长度与 UTF-8。
当前没有真实非 Linux、异端序 DZFlat 或跨机性能证据，不能宣称延迟优化已经达标。

当前会话未提供 `member_report_result`、`member_read_shared`、`member_send_message` 或
`/compact` 可调用能力；未宣称已进行团队回报/压缩。
