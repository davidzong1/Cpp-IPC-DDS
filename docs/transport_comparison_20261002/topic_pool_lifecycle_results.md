# 话题独享池身份与生命周期修复报告

日期：2026-10-02。按本轮拍板执行；此前速度/资源报告保留为历史版本，不代表当前布局。

## 交付行为

- pub/sub 创建时用完整 `topic_name_.c_str()` 调用 `fnv1a64()`，将身份写入池头。池键同时包含 prefix 的散列；注册项保存完整 prefix/name 并校验，身份不匹配时拒绝打开及删除。RPC 请求、响应通道各自独立管理。
- `InitChannel` 先处理无人使用的旧资源，再取得话题租约；已有其他发布者、订阅者、未发布借样或收到的 Sample 使用时复用原池，不重置、不删除。载荷池仍在首次使用该尺寸档时按需创建，初始化只建租约注册项。
- 话题析构释放自身引用。最后一个进程租约与借样引用释放后，解除映射并删除本话题全部尺寸池、注册项及通道队列/等待资源。`ipc::route::clear_storage` 同样遵守活跃保护，不能强删活跃池或队列。
- 所有 SHM 载荷池强制每话题每尺寸档 10 块。删除 `large_msg_cache`、原 40 块池、双池 ID 分支及 `loan_topic` 选择入口；统一 `loan`，普通 send/sniffer/RPC 使用相同话题池与尺寸规则。小消息原有内联队列路径不因此强制分配 chunk。
- 原始 `loan_t` 副本共同保活；最后副本析构自动归还未发布块。发布/归还只能终结一次；收到的 Sample 可以比话题长寿。

## 修复中发现并处理的问题

1. 池删除与队列删除曾分处锁内外，有机会删除并发新建的队列。现两者在同一个目录级锁内完成。
2. 高层测试最初出现最后借样释放后仍剩一个池。原因是 `SubState → msg_queue → 驱逐回调 → SubState` 强引用环，现回调改为弱引用。相同失败用例修复后通过。
3. 崩溃后只有池被清理会留下队列 writer 状态。现无人租约时连同队列和等待资源一起清理。
4. 旧测试仍按全机共享池路径和容量取样。容量、隔离、耗尽日志及池外部读取工具均迁移到十块身份池；未用放宽成功条件的方式接受失败。

## 验证结果

Linux 私有 user/mount/IPC namespace，独立 4 GiB tmpfs，Release 构建。未修改宿主共享内存挂载，也未停止或修改宿主 RouDi。最终 18 组、137 个单测全部通过。

| 测试程序 | 用例数 | 结果 |
|---|---:|---|
| `test_topic_chunk_pool` | 25 | 通过 |
| `test_loan` | 10 | 通过 |
| `test_dzflat_transport` | 10 | 通过 |
| `test_chunk_capacity_backpressure` | 8 | 通过 |
| `test_adopt_loan_quota` | 5 | 通过 |
| `test_dzflat_sercli` | 4 | 通过 |
| `test_shm_ser_backpressure` | 2 | 通过 |
| `test_shm_sniffer_control_name` | 3 | 通过 |
| `test_lifecycle_contract` | 10 | 通过 |
| `test_shm_route_session` | 15 | 通过 |
| `test_dzflat_fallback_semantics` | 6 | 通过 |
| `test_shm_sub_dtor_gate` | 2 | 通过 |
| `test_chunk_hold` | 3 | 通过 |
| `test_lap_safety` | 3 | 通过 |
| `test_pool_exhaust_observability` | 2 | 通过 |
| `test_recv_worker` | 19 | 通过 |
| `test_recv_fragment_isolation` | 2 | 通过 |
| `test_dzflat_rx` | 8 | 通过 |

关键覆盖：按需建池、20 次多尺寸建销毁、跨话题隔离、同话题复用、跨进程最后使用者删除、异常退出后重新初始化、注册身份拒绝、普通 send 与 loan 同档共用容量、长话题名/domain、sniffer、借样副本重复归还防护、Sample 晚释放。高层 pub/sub 重复 InitChannel 三次可收发；话题析构后外部 LoanedMessage 仍有效，最后释放后池数归零。

### 传输验证

16 格全载荷字节验证全部通过：pub/sub 的 SHM、A、B、prebuilt × 8 B/1 KiB/1 MiB 共 12 格，RPC 的 SHM/A × 8 B/1 MiB 共 4 格；每格 3 秒。此处用于验证路径与数据正确性，不构成全尺寸速度复测。

### 千话题资源

每个话题同时借满 10 块，首尾触页，额外一次借样必须报耗尽；归还后重新借出成功。三档各独立运行，所有话题销毁后池文件和本次话题相关段均为 0。

| 请求字节数 | 1000 池逻辑字节 | 池实际分配字节 | 全部相关段实际分配字节 | 销毁后池数 |
|---:|---:|---:|---:|---:|
| 64 | 20516000 | 24576000 | 77824000 | 0 |
| 1024 | 20516000 | 24576000 | 77824000 | 0 |
| 1048577 | 20981796000 | 86016000 | 139264000 | 0 |

64 B 与 1 KiB 都落在相同档；1,048,577 B 请求上取整为 2 MiB 容量档，因此 1000×10 块池逻辑约 19.54 GiB。上表实际分配来自 `st_blocks × 512`；大档只触首尾页，不能将该实际分配量解释为全载荷写满后的内存消耗。多尺寸档同时启用要累计计算，本实现不提供 32 GiB 全局配额或预留保证。

固定 `__IPC_POOL_CATALOG_V2` 是零字节全局生命周期锁文件，故意保留，不能 unlink 以免锁 inode 分裂；它不属于载荷池且不占载荷页。注册项及每话题池已随最终释放删除。池外进程堆缓存与 RSS 不保证立即回到启动值。

### 千话题短压力验证

A/B 各独立运行一轮 10 秒，64 B、1000 话题、每话题 1000 msg/s，4 发布线程、32 接收 worker、窗口 8。每条路径计划/成功发送/窗口内接收均为 10,000,000，发送失败、坏消息、重复、缺口均为 0；1000 话题各收到 10,000 条。

| 路径 | 发送调用均值 μs | 接收延迟均值 μs | 接收 P50 μs | 接收 P99 μs |
|---|---:|---:|---:|---:|
| A | 2.335 | 58.551 | 31.583 | 340.974 |
| B | 1.714 | 48.631 | 30.619 | 292.739 |

延迟沿用 comparison 的发送端消息时间戳到接收端观测时间口径，不能解释为“写入完成后开始计时”。压力模式校验序号/交付守恒，完整载荷逐字节验证由上述 16 格承担。单轮短测只证明此负载下运行正常；没有重跑 CycloneDDS+iceoryx 或旧版本，不据此判断相对性能改善。

池归因工具迁移后验证：2 话题、queue=2、1000 条发布上限、10 次采样，聚合占用 L 始终为 4，坏快照/坏链为 0。当前未开启队列插桩 (`instr=0`)，因此不声称完成 Q 与 L 的双通道校验。

## 兼容性与边界

- 库版本 1.4.0，SONAME `libipc.so.4`；`loan_t` 布局、DzIPC 类布局及队列协议发生变化（队列 V4、池 V2），必须重新构建并升级通信双方。已删除旧池选择 API，不提供与旧共享池混跑的兼容承诺，不清理正在运行的旧版共享段。
- 生命周期协调当前实现并验证于 Linux（flock、POSIX SHM、/dev/shm）；未验证 Windows。
- 正常释放最后使用者时自动删除；进程异常退出由内核释放租约，残留池在下次 InitChannel/clear_storage 回收，没有后台即时清扫器。非空但身份不可验证的注册残段保守拒绝，不自动覆盖。
- 活跃保护不改变 route 单生产者拓扑；其他发布者可保护/复用池，不意味着新增多生产者发布能力。
- 目录锁只用于创建、首次映射和销毁，逐消息不取目录锁；新 loan 生命周期仍有 shared_ptr 与终结互斥开销。耗尽日志计数沿用按操作/尺寸档聚合、进程内首报和节流规则，不等于每话题精确耗尽计数。
- 编辑器诊断有旧 `/usr/local/include` 头文件缓存，仍报 loan_t/storage_id_t 不存在；真实 CMake 构建通过。pool_alloc.cpp 编辑器诊断无错误，diff 空白检查通过。

## 复现

生命周期、回归与资源（全程私有共享内存，日志在仓库外）：

```bash
python3 -B test/transport_comparison/topic_pool_lifecycle.py --work /var/tmp/cppipc-pool-lifecycle-new
```

传输程序沿用 `test/transport_comparison/CMakeLists.txt` 构建到上述工作目录 `final/build`；在私有 namespace 中运行 `topic_pool_runs.py --stage quick --tag lifecycle-final` 和 `run.py stress --backends a,b --topics 1000 --rounds 1 --seconds 10`，并指定相应 `--work`（前者用工作目录，后者用其 final 子目录）。环境需启用 unshare 和用户 namespace。

最终仅保留修改源码、测试脚本与 Markdown 文档；本轮临时构建、JSON、原始日志及二进制在摘录结果后清理。历史交付文件未删除。

## 验证产物摘要

- 单测动态库 SHA256：`8af161db528ed2df0af65482239fedce06c3b59e768455865e46b2e5366112de`
- 传输动态库 SHA256：`8af161db528ed2df0af65482239fedce06c3b59e768455865e46b2e5366112de`
- comparison SHA256：`486e9499f6500a86c343f80347099fc5a56d77de968f38dbadc80e33a6e6f1a1`
- 单测结果清单 SHA256：`8ad53d50c1f04be02de56b0a1edae38e31f2ebe7a93e8017e6aa71036f53eb11`
- 16 格结果 SHA256：`de1f5043567158f06b2778ba5aa9d058a9206b3b35474b5191d8964540c64eea`

源码修改基线：`cd16d88172a155dabea1be939532ae103dc63563`。本轮改动留在工作区，未提交。
