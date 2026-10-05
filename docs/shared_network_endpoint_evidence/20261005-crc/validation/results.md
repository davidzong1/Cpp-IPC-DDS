# O05 CRC32C 验证

优化前库是2b90e28正式窗口使用的libipc，单独复制到/tmp/dzipc-crc-before-2b90e28并校验SHA256一致。同一个CRC probe通过LD_LIBRARY_PATH分别加载前后库；不修改源网关配置、不改校验多项式/初末异或或协议CRC字段。

- 完整共享CTest36/36；Wire协议7/7，包括独立逐bit算法核对硬件派发与软件回退、8种对齐、短/边界/分片尺寸和跨8字节清零字段。
- ASan/UBSan（含泄漏）wire/reassembly/metrics 3/3。
- 真实双向UDP及DATA/ACK丢失、重复/反序故障通过，见fault-mixed/result.json及实际注入报告。
- Python2/2、安装CLI17/17、安装回滚3/3。

SSE4.2仅存在于带target属性的独立函数；运行时检测不支持即走原表算法。未全局打开CPU专用指令集。软件回退可直接被测试，无需假定当前机器没有SSE4.2。硬件以外平台没有实机执行证据，不能据此声称跨平台编译/性能验收完成。

微基准仅为CRC调用耗时；网络端到端复测另固定热流70Hz（1MiB）、冷流100Hz（64B），前后版本各同/不同shard三轮30秒，原始有序统计样本在latency_samples_ns（按数值排序，非时间顺序）。不将CRC微基准提升倍率直接写作端到端提升。
