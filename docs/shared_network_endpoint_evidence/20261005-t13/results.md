# T13 工具、诊断和安装

基于 32c2e2d。公共逻辑发布/订阅预留诊断池槽位，失败返回 RegistryFull；底层 SHM 腿和网关 bridge 不重复登记。dzipc_list 显示模式、域、epoch，长名称展示标记可能截断。专用 QoS/绑核参数明确拒绝，默认值与公共工厂保持一致。

QUERY_STATE kind=1/2 和网关 `status --topic --domain --msg-id [--peer-id]` 返回完整名称、64 位域、目录版本、角色、route epoch 和核验/就绪状态。默认汇总有界，不塞全部话题。增加实际 socket 缓冲、资源占用/峰值、重组/提交、错误、命令积压、发送在途预算与配置上限。应用 `diagnostics_json()` 提供独立本机/网络提交结果、部分提交、信用等待、出站容量。stop 返回后活跃资源可查询为零，必要历史单独计数。

topic_cat 在 shared_v1 下直接构造公共 Subscriber，无需本机已有发布登记；支持域、单次读取与超时。TLV 输出字段，未知 DZFlat schema 输出完整段十六进制与 CRC。安装工具使用相对 RPATH；[使用说明](../../shared_network_endpoint_usage.md) 覆盖启用、错误码、语义与按实例回滚。

验证：

- 新增 GTest 3/3：状态与未知 peer、逻辑池不重复、非默认对象调度拒绝；含应用本机提交诊断断言。
- CLI 驱动 11/11：帮助、未知命令/参数/数值、离线、配置、汇总/UINT64_MAX 域查询、锁冲突、端口冲突、只读目录。
- topic_cat 本机与隔离远端各 1 次 4096B，CRC 与发布源一致，工具 UDP=0；列表恰好显示两条逻辑端点。
- 全共享 CTest 35/35，ASan/UBSan 3/3。
- `cmake --install build-shared-net --prefix /tmp/dzipc-shared-install`；仅用安装 include/lib 独立编译 shared_net_probe，安装二进制运行上述 CLI 和 topic_cat 驱动均通过，未依赖 LD_LIBRARY_PATH。日志同目录。

回归中发现并修正：旧 bridge 测试只数 ShmPub，现按公共发布角色数 ShmPub/SocketPub；新封装原默认 priority=20 与既有公共工厂 LowPriority=0 不一致，现统一使用枚举默认值。未改变旧模式调度。

**未完成验收项**：第 14.2 节仍未逐项提供每话题排队分位数、全链路各阶段计时和所有独立错误/触顶计数。已有 `invalid_packets` 等聚合项不能充当所有细分项；本节点记作核心实现完成、指标待补，不记为全项通过。性能测试仍需 T14，物理跨机验收尚未具备环境。
