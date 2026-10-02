# DzFlat 每话题每尺寸档十块池：执行结果

已实施每个底层话题通道、每个尺寸档 10 块的独立 DzFlat 载荷池。A、B、预构造段以及 SHM RPC 请求/响应均接入；RPC 两个方向按各自通道隔离。同话题的多个发布句柄共享这 10 块，多订阅者共享消息并由最后持有者归还。普通 libipc loan、SHM/TLV 继续用原 40 块共享池，Socket 不变。

方案与中断续跑进度见[执行记录](topic_pool_execution.md)，性能详见[速度分析](topic_pool_speed_analysis.md)和[压力分析](topic_pool_stress_analysis.md)。池隔离降低了千话题容量争抢，但压力尾延迟上升，不能作为低延迟优化发布。

测试日期：2026-10-02。机器为 Intel Core i9-14900KF、32 个逻辑 CPU、约 62 GiB 可见内存，Release / O3，CPU affinity 为 0–31，不固定到特定核心。宿主共享内存为 32 GiB，本次未改挂载。测试在独立的 4 GiB tmpfs 中串行执行；构建与资源探针不与正式性能轮并行。宿主仍有其他进程活动，三轮只能说明本机当前条件下的趋势。

旧基线为 Git `e3b17db3a3ac35a3a73497d16c22e02032e64456` 的 40 块共享池；新版本为本次每话题每尺寸档 10 块独立池。比较使用相同测试器、32 个接收 worker、4 个发布线程配置；不重测 CycloneDDS + iceoryx，不能据此更新与 DDS 的排名。

## 正确性与回归

17 组回归最终全部通过，其中新增话题池回归包含 12 个用例；16 格全字节 pub/sub 与 RPC 通信全部通过。首次失败和修正均保留在执行记录，未扩大池容量或放宽交付正确性断言。

| 回归程序 | 17 组回归首次退出码 | 修正后退出码 |
|---|---|---|
| test_topic_chunk_pool | 0 | 无需重跑 |
| test_loan | 0 | 无需重跑 |
| test_chunk_capacity_backpressure | 0 | 无需重跑 |
| test_chunk_hold | 0 | 无需重跑 |
| test_dzflat_transport | 1 | 0 |
| test_w08_dzflat_ab | 0 | 无需重跑 |
| test_adopt_loan_quota | 1 | 0 |
| test_recv_worker | 0 | 无需重跑 |
| test_recv_fragment_isolation | 0 | 无需重跑 |
| test_lifecycle_contract | 0 | 无需重跑 |
| test_recv_wait_set | 0 | 无需重跑 |
| test_shm_route_session | 0 | 无需重跑 |
| test_dzflat_rx | 0 | 无需重跑 |
| test_shm_sniffer_control_name | 0 | 无需重跑 |
| test_dzflat_fallback_semantics | 0 | 无需重跑 |
| test_dzflat_sercli | 0 | 无需重跑 |
| test_shm_ser_backpressure | 0 | 无需重跑 |

新回归覆盖：精确十块、归还循环、话题/prefix/尺寸隔离、同话题多句柄共享、旧池仍 40、实际段容量、旧新池交替收取、广播最后持有者、消息晚于路由释放、sniffer、独立子进程映射、并发首次借样、非法 ID 与超大请求。

| 全字节路径 | 载荷 B | 状态 | 成功发送 | 接收或成功响应 |
|---|---|---|---|---|
| pubsub/shm | 8 | ok | 876148 | 876148 |
| pubsub/shm | 1024 | ok | 601701 | 601701 |
| pubsub/shm | 1048576 | ok | 2463 | 2463 |
| pubsub/a | 8 | ok | 1062212 | 1062212 |
| pubsub/a | 1024 | ok | 696191 | 696191 |
| pubsub/a | 1048576 | ok | 2948 | 2948 |
| pubsub/b | 8 | ok | 1034607 | 1034607 |
| pubsub/b | 1024 | ok | 697339 | 697339 |
| pubsub/b | 1048576 | ok | 2973 | 2973 |
| pubsub/prebuilt | 8 | ok | 983708 | 983708 |
| pubsub/prebuilt | 1024 | ok | 680536 | 680536 |
| pubsub/prebuilt | 1048576 | ok | 2823 | 2823 |
| rpc/shm | 8 | ok | 472726 | 472726 |
| rpc/shm | 1048576 | ok | 12570 | 12570 |
| rpc/a | 8 | ok | 456559 | 456559 |
| rpc/a | 1048576 | ok | 20925 | 20925 |

## 1000 话题资源实测

每话题同时持有 10 块，第 11 次必须返回池满；随后全部归还并验证重新借出。只触及请求载荷首尾页，没有全量填充大档。逻辑容量来自共享文件 st_size，实际占用来自 st_blocks × 512；不把 VmSize 当物理内存。下表均为全部 1000 话题总和。

| 每次请求 B | 池数量 | 池逻辑 MiB | 池实际 MiB | 含路由全部段数 | 全部段实际 MiB | 进程RSS MiB | 进程VmSize MiB |
|---|---|---|---|---|---|---|---|
| 64 | 1000 | 19.581 | 23.438 | 12000 | 70.312 | 120.035 | 161.945 |
| 1024 | 1000 | 19.581 | 23.438 | 12000 | 70.312 | 120.027 | 161.945 |
| 1048577 | 1000 | 20009.815 | 82.031 | 12000 | 128.906 | 178.621 | 20150.227 |

2 MiB 档共 19.5408 GiB 逻辑池容量；首尾触页时池实际为 82.031 MiB。该实验验证一万个借样可同时成立，不是 19.54 GiB 全量写入或 32 GiB 满载稳定性测试。64 B 和 1024 B 的原始 loan 请求都落到 1 KiB 档；真实 DzFlat 还需加消息头再选档。

本平台单池文件大小为 `52 + 10 × align_up(尺寸档容量 + 16, 1024)` 字节，52 包括池头和段尾引用计数。池按需创建；若 1000 话题每个都曾用到 1–64 KiB 的全部 64 档及 128 KiB–2 MiB 的 5 个大档，合计约 **58.34 GiB 逻辑容量**，超过 32 GiB。因此本方案不能理解为“所有话题所有尺寸同时预分配也必定够用”。

归还借样仅归还块，不清除已触页内容。实测销毁全部路由后仍保留 1000 个池：池句柄由进程级缓存持有，进程退出时释放；动态创建大量不同话题/尺寸档会累计映射与物理页。当前没有新增全局 32 GiB 配额或页预留器。

## 使用边界

- 默认 view 上限与池容量同为 10；取出后持有的 Sample、未发布借样也占池。慢消费者可能把十块全部占满，A 回退、B 借样失败是保留的行为。
- `loan_t` 布局保留，新本地池标识映射为线上负 ID -2…-11。新收端可读旧共享池与新话题池，旧收端会拒绝新池 ID；新池通信要求双方升级。未增加能力协商，不能承诺混合版本交付。公开 chan_impl 旧符号未删除，新增三个模板实例的 loan_topic；内部 STL 模板符号有变化，这不是完整 ABI 兼容性认证。
- 池名包含完整底层通道名并受操作系统名字长度限制。新增命名开销会缩短可用话题名上限；本次未引入散列缩名。以 Linux 的 255 字节段名限制为准，部署长话题名需要连同 prefix、版本、尺寸和容量后缀检查。
- 后续若要降低压力延迟，应另测相同成功输入下的分段耗时、应用取样等待和 worker 排队；本次不调整默认 32-worker 或借样池容量。

## 构建与运行复现

所有工作目录放仓库外。先将上述旧 Git 的 include、src、test/transport_comparison 快照放入工作目录的 baseline-source，再分别从 baseline-source/test/transport_comparison 与当前 test/transport_comparison 配置 CMake 至 baseline/build、final/build。二者均执行 Release 构建。

```bash
# 工作目录、RouDi 路径与配置需替换为本机值；配置可使用既有测试配置。
python3 -B test/transport_comparison/repair_units.py --work <工作目录>/units --targets <报告所列17个目标>

# 每阶段在 isolated.sh 内执行；脚本使用同一 flock 保证串行。
bash test/transport_comparison/isolated.sh <RouDi> <配置> <RouDi日志> \
  python3 -B test/transport_comparison/topic_pool_runs.py --work <工作目录> --stage units
bash test/transport_comparison/isolated.sh <RouDi> <配置> <RouDi日志> \
  python3 -B test/transport_comparison/topic_pool_runs.py --work <工作目录> --stage quick --tag topic10-v2
# 同样运行 --stage compare、--stage long。配置或二进制变化时使用新 tag。
python3 -B test/transport_comparison/topic_pool_resources.py --work <工作目录> --roudi <RouDi> --config <配置>
python3 -B test/transport_comparison/topic_pool_report.py --work <工作目录> --tag topic10-v2
```

报告脚本针对本次证据布局；新执行若无修正重跑，可省略 revised-tests/results.json。

## 证据指纹

本次性能二进制、库及运行器 SHA-256 如下；报告生成时核对当前二进制与运行归档一致。源码交付指纹见下表。

最后整理注释与行尾后已重新构建，comparison 与 libipc.so 均与正式测试版本逐字节一致。\n\n| 对象 | SHA-256 |
|---|---|
| baseline/comparison | c97f23248b642737d01e9e4d6a7efe86f434da68d54035098a6f2cb691270b9a |
| baseline/lib/libipc.so | d24eb7c4d09a1370de1882a75b85b2207202d6fe1f49f5212ab968ffdfbf8aae |
| final/comparison | 7ad5043ade55668a2e8b7423d0688c1e613267ed7e30a74853a0bb8d40f798ed |
| final/lib/libipc.so | 3446992abb2f7f0dc106ff821231441e1ebccbb1da0844a0a66a0c6a99dccafb |
| script | 4656a29b988644b997ee18ace719898e53786a8a28f0462efdb26084938f5597 |
| runner | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db |

| 交付源码 | SHA-256 |
|---|---|
| include/libipc/def.h | d6817ba540892a7124ec693ccf22a3b766d7746fe85751d42facdc6a56ca3f2e |
| include/libipc/ipc.h | 7c5d6efe79abe2698f2d4e829de8c9fc990c97124e98bc632904e92a3b1c8a7c |
| src/libipc/ipc.cpp | 6c260d6f69b41e176d6a154c09377b78d5d4b504306f89103c45d00414283f14 |
| src/libipc/memory/resource.h | 1f1848047f5688d687936033fce837309e505e58b5b526b67a115609449c378c |
| src/libipc/utility/id_pool.h | 5a0a33735757464af750c3b25bd170f998078f3c9f9c9a9a89d0653e7829c53d |
| src/libipc/sniffer.cpp | c7753a7e7a6862e9c99e52b7f5e87a5f8f9303c01099115d760f4468224e53cf |
| include/dzIPC/shm_pub_sub_ipc.h | 2c5f93cc5809a3f3d4616dd293deabd3b9736e3f83db2555b37f1ecbf81c1d41 |
| src/dzIPC/shm_pub_sub_ipc.cc | 1555587bdaaaf9529f4bc3312d9bdebca392e32cc93dd69878c7fb352dbc1e9c |
| src/dzIPC/shm_ser_cli_ipc.cc | 70248d3525cf71d5ee127b666f872d66d4665c5d42115baed871a893a982c982 |
| src/dzIPC/common/nodelet_config.cc | d685b03b696ca1b3e4e1183189807704d0b4d548a836456099074d26b0b202d8 |
| include/dzIPC/common/nodelet_config.h | 0e55fbc76bcda871db97151ff45870d582148a6edcbcbba6234d79dda421d1ae |
| include/dzIPC/common/loaned_message.h | fd11414977def76cab52d26876201715b5f9b515d599a8d4175c99276e2e460f |
| test/test_topic_chunk_pool.cpp | c2fb9f5fca374491517e78d72ac012d5dbeb230f6c12a732be2cce2ab8407f68 |
| test/test_adopt_loan_quota.cpp | a0a2388c011ca26886b62f0740a07ab5ab1831075030c53fb4e445da57fbc97a |
| test/test_dzflat_transport.cpp | 55571fd33a244a8953311c907f0a27813558b58af1f5bbda1c3b2e59ef19dd6d |

全部正式性能轮错误消息计数合计：0；重复数合计：0。状态、失败、回退及延迟已逐轮归档到两份分析。构建存在原有 /proc 路径 snprintf 截断警告；编辑器部分 C++ 诊断仍引用旧安装头，真实 CMake 构建及回归结果作为验证依据。
