# 共享网络实施基线（2026-10-05）

源码基线为 `f066a82`，MPMC 已交付；完整方案保留用户批准的工作区修订。
本目录记录 T00/T01 的实际检查。共享网络尚不能运行，性能收益尚未验证。

## 构建和结果

- 独立目录 `build-shared-net`，`RelWithDebInfo`、`ENABLE_DEBUG_INFO=OFF`、Python OFF。
- 修复顶层 CMake 强制覆盖构建类型，实际编译命令与硬件见 [环境指纹](baseline_environment.json)。
- `cmake --build build-shared-net --target test_shared_net_config dzipc_gateway --parallel 4`：退出码 0。
- `ctest --test-dir build-shared-net -L shared_net --output-on-failure`：T01 当时登记 1 项，1/1 通过；GTest 10 个用例，无跳过。
- `build-shared-net-off` 使用同样配置、追加 `-DDZIPC_BUILD_SHARED_NET=OFF`；构建与配置测试通过（1/1），不构建网关。
- 不支持平台通过纯函数验证 UnsupportedPlatform 的优先级；未作真实非 Linux 构建。
- 默认旧路径 `test_shm`（8）、`test_channel_scope`（11）、`test_shm_sniffer_control_name`（3）共 22 用例通过，无跳过。
- 新增 `test_shared_net_shm_capacity` 3/3 通过，无跳过；独立 PID 前缀，只清理自己创建的段。
- `dzipc_gateway check-config --listen-ip 127.0.0.1 --interface lo --control /tmp/dzipc-check/control.sock`：退出码 0，没有 bind 或启动网关。
- 首次构建暴露测试整数列表类型不一致、探针借样 API 拼写错误，已修正；保留首次失败日志，未删除断言。

## MPMC 能力与限制

| 项目 | 已确认事实 |
|---|---|
| 数据布局 | MPMC V2；旧 route 物理命名隔离 |
| 发布者 | 每业务话题 32 个 PublisherRegistry 槽；网关注入端也占一个 |
| 接收者 | 32 位连接位图，最多 32 个连接 |
| 环容量 | 256 槽；底层 publish_loan 存在 force_push 分支，T05 仍须验证两腿接管边界 |
| 诊断池 | 全机 4096 项；不是每会话 4096；topic 数组 128B，不能用于恢复完整路由名 |
| 普通 join/leave | 已交付测试覆盖同话题 survivor 继续发送、跨进程加入、协调者退出；证据见 MPMC 方案第 9 节 |
| 无接收者 loan | 返回 no_receiver，未提交 |
| 未发布 loan | 关闭发布端后仍钉住池，释放 loan 后归还 |
| 已接收样本 | 关闭发布/接收端后仍可读取；样本生命周期保留映射 |
| 16 MiB 业务 loan | 实际容量 16777216B |
| 16 MiB + 112B 出站记录 | 请求 16777328B，实际 loan 33554432B |
| 对应出站池映射 | 实测 335556608B；当前每档 10 个 chunk，不是 40；映射与 RSS/tmpfs/在途容量分开统计 |

容量原始结果见 [探针输出](shared-net-shm-capacity.txt)。旧测试注释仍有 40 chunk 描述，
本轮以真实 `pool_alloc.cpp` 的 C10 和运行测量为准，不据旧注释扩大额度。

## 未完成证据

T00 保持部分完成：1/100/1000 话题资源、小包/大包本机延迟、8/32 进程长期压力、
真实跨机单目标/多目标性能尚未采集。现有单机测试不能替代这些验收。
历史完整 CTest 为 28/36，以下 8 个旧基线失败继续保留：
`test_w05_stale_slot_gate`、`test_w05_stale_slot_gate_arm`、`test_wakeup_artifact`、
`test_shm_i5_pop_buffer`、`test_shm_sub_dtor_gate`、`test_shm_ready_transition`、
`test_w03_measurement`、`test_dzflat_fallback_semantics`。

T01 已实现配置与构建骨架：首版配额可以下调、不能超过方案默认硬上限；控制速率硬上限
1000000 包/秒、突发上限 65536 包；跨线程队列固定默认 4096 项/8 MiB，最少按每项 64B 校验。
服务/状态命令明确返回 NotImplemented；默认 legacy 不变，尚未接入新数据面。
团队 MCP 回报与 `/compact` 当前无可调用工具，未声称已执行。
