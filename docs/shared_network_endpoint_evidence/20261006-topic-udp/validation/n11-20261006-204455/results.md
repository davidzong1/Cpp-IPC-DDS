# N11 验证结果

日期：2026-10-06。基线提交：`7aba239c`（N10 队列拆分候选已回退）。本节点只修改了 `test/test_shared_net_config.cpp`：Linux 资源审计用例同时要求 `DZIPC_TEST_SHARED_NET_BUILT`，因此 `DZIPC_BUILD_SHARED_NET=OFF` 时不会链接 shared-net 专属符号。

## 构建

以下构建均退出码 0：

```text
cmake --build build-shared-net --parallel 4
cmake --build build-shared-net-off --parallel 4
cmake --build build-shared-net-sanitize --parallel 4
```

`build-shared-net-sanitize` 为 ASan + UBSan Debug 构建。

## 回归结果

原始日志保存在本目录：

| 配置 | 聚焦集 | shared_net 全量 |
|---|---:|---:|
| 普通 | 13/13 通过 | 38/39 通过 |
| `DZIPC_BUILD_SHARED_NET=OFF` | 14/14 通过（含 `test_shared_net_config`） | 未运行全量 shared_net |
| ASan + UBSan | 12/13 通过 | 36/39 通过 |

普通构建聚焦集覆盖 MPMC、DZFlat、生命周期和信息池；OFF 聚焦集还包含 shared-net 配置测试。普通 shared_net 全量唯一失败为 `test_shared_net_v2_per_topic_port_fallback`，ASan + UBSan 全量失败为 `test_shared_net_end_to_end`、`test_shared_net_v2_per_topic_port_fallback` 和 `test_shared_net_v2_exclusive_worker`。这些失败都发生在 1 MiB BestEffort 场景：发送端报告成功，但接收端只得到约 500～630 KiB，随后重组超时；状态中的 `wrong_shard`、CRC、非法包和发送错误均为 0。sanitizer 全量没有新增 ASan/UBSan 报告。

sanitizer 聚焦集唯一失败为 `test_lifecycle_contract` 的 `GenerationRebuildRequiresRemoveRouteFirstOrCrashStall`。该测试故意不先 `remove_route` 就 rebuild，ASan 在 `recv_wait_set`/`RecvWorker` 读已失效对象时报告 SEGV；这是已有的 R-01 负向生命周期契约。普通构建同一用例通过，其他 12 个聚焦用例通过。

## Legacy 回退

从 `build-shared-net` 安装到临时目录后，仅使用安装目录的头文件和库编译 `installed_roundtrip.cc`，执行 `rollback.py`：

```text
passed=3
shared_v1 roundtrip=true
explicit legacy roundtrip=true
unset backend roundtrip=true, backend=legacy
control_connections_after_rollback=0
```

旧模式未连接共享网关控制 socket。物理跨主机验收仍未完成。

完整命令输出见：`ordinary-focused.log`、`off-focused.log`、`sanitize-focused.log`、`ordinary-shared-net-full.log`、`sanitize-shared-net-full.log`、`legacy-rollback.log`。
