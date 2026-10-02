# 修复执行结果与验收状态

日期：2026-10-02。对应 [执行方案](execution_plan.md)，逐轮结果见 [修复账本](repair_ledger.md)。

**已修复两个独立的 SHM 接收正确性缺陷；Socket 结束边界仅局部修复，旧分片协议的大包完整性仍未闭环。O1 未达到性能门槛，已撤回，最终产品保留正确性修复。整体方案尚未通过最终验收。**

## 产品修改与因果证据

| 项 | 根因与修改 | 确定性验证 | 边界 |
|---|---|---|---|
| C1 | 空读后将最新 sequence 当作已消费，吞掉并发发布；稳定空读仅确认接收前快照，变化则继续处理 | 旧版反例 10/10 失败；空读后发布、稳定检查后发布、回零三类各 1000/1000 通过 | 预算、停止、注销、异常不冒充空读；保留无进展退避 |
| C2 | thread_local 分片缓存只按消息 ID 索引；改为连接独占，并互斥访问/断开清理 | 交错话题同 ID、断开邻路两类旧版各 10/10 失败；修复后各 1000/1000 通过 | 进程本地对象改变，共享 wire/公开 API 未改；锁不跨队列等待，不进入共享 chunk 快路径 |
| C4 | final HB 后继续等页会混入下一条；立即丢弃当前不完整组装 | 旧版 10/10 混页；修复后该反例 1000/1000 通过 | 其他首片/通知丢失仍能混页，不能称彻底修复 |


修改文件：src/dzIPC/threepools/recv_worker.cc、src/libipc/ipc.cpp、src/dzIPC/common/data_rev.cc。新增失败用例及工装只位于 test 下。

C1 的 A/B、8 B/1 KiB、w1/w8、10 秒 × 10 轮共 80 格全部通过。该早期进展结果用于验证 C1；最终构建/后续长测按下表和账本单独记录。

## 最终版本单元回归

| 测试程序 | 退出码 | 通过测试数 |
|---|---|---|
| test_recv_worker | 0 | 19 |
| test_recv_fragment_isolation | 0 | 2 |
| test_lifecycle_contract | 0 | 10 |
| test_recv_wait_set | 0 | 9 |
| test_shm_route_session | 0 | 15 |
| test_w08_dzflat_ab | 0 | 3 |
| test_dzflat_rx | 0 | 8 |
| test_socket_reliable_crc | 0 | 5 |
| test_socket_ser_concurrency | 0 | 3 |
| test_socket_recv_worker | 0 | 7 |
| test_socket_wait_set | 0 | 9 |
| test_socket_readable | 0 | 5 |


旧 C2 回归中的 W08 曾因拒绝覆盖已有证据退出 1；独立目录重跑全 3 测试通过。交付 repair_units.py 已为每次 W08 创建新目录，最终上表保留真正执行的退出码。

## O1 五轮单变量筛选

基线与候选均包含 C1、C2、C4。唯一产品差异是是否删除 wait_once 末尾重复 collect_pending；相同 CPU 集合，每格 10 秒，固定种子交错串行。下表以独立轮中位数呈现，所有 50 格均完整交付。

| 后端 | 输入档 | 版本 | 完整轮 | 均值 µs | p99 µs | 收 CPU 核 | 成功发送 msg/s |
|---|---|---|---|---|---|---|---|
| a | r500k | baseline | 5 | 2.312 | 4.433 | 1.868 | 500,000.036 |
| a | r500k | candidate | 5 | 2.321 | 4.028 | 1.862 | 500,000.023 |
| b | r500k | baseline | 5 | 2.357 | 6.653 | 1.878 | 500,000.028 |
| b | r500k | candidate | 5 | 2.314 | 4.424 | 1.859 | 500,000.033 |
| a | w8 | baseline | 5 | 6.805 | 8.065 | 1.993 | 1,111,686.962 |
| a | w8 | candidate | 5 | 5.941 | 7.799 | 1.992 | 1,175,683.806 |
| b | w8 | baseline | 5 | 7.238 | 8.667 | 1.992 | 1,059,361.288 |
| b | w8 | candidate | 5 | 6.187 | 8.183 | 1.992 | 1,170,612.733 |
| shm | million | baseline | 5 | 25.662 | 93.465 | 11.578 | 1,000,009.271 |
| shm | million | candidate | 5 | 24.644 | 88.746 | 11.845 | 1,000,007.822 |


千话题扫描约 84 → 56 次/消息，但 CPU 中位数 11.58 → 11.85 核，未达下降 30% 目标；8 B 的相同 50 万输入均值也未下降 15%。饱和档有改善，但未达到方案的吞吐 +15% 或均值 -20% 门槛。因此撤回 O1，不能将扫描次数减少直接宣传为 CPU 优化。O2–O5 尚无足够归因证据，不默认加入产品。

## 真实验证当前覆盖

| 前缀 | 总格 | 通过 | 不达标/失败 | 不支持 |
|---|---|---|---|---|
| final-quick | 62 | 62 | 0 | 0 |
| final-idle | 4 | 4 | 0 | 0 |
| final-short | 15 | 15 | 0 | 0 |
| final-long | 3 | 3 | 0 | 0 |
| final-capacity | 6 | 0 | 6 | 0 |
| dds-original-capacity | 1 | 0 | 1 | 0 |
| dds-scaled-short | 5 | 5 | 0 | 0 |
| dds-scaled-long | 3 | 3 | 0 | 0 |
| final-input | 90 | 90 | 0 | 0 |
| final-control | 120 | 120 | 0 | 0 |
| probe- | 162 | 162 | 0 | 0 |
| final-full | 0 | 0 | 0 | 0 |
| final-bytes | 0 | 0 | 0 | 0 |


0 格表示尚未执行，不是通过。RPC 的 B/Prebuilt 原生接口仍为 N/A。全量速度与全部尺寸正确性矩阵保留在 repair_campaign.py 的 full 阶段；C4 未闭环时，不将代表格冒充全量验收。

## 千话题交付、CPU 与容量

| case_id | 话题 | 计划 | 成功发 | 收 | 最少/最多每话题 | p99 µs | 收 CPU 核 | 主线程 CPU s | 队列淘汰 | 状态 |
|---|---|---|---|---|---|---|---|---|---|---|
| cpu-w16-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 75.273 | 11.401 | 9.888 | 0 | ok |
| cpu-w16-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 112.005 | 11.268 | 9.906 | 0 | ok |
| cpu-w16-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 50.281 | 11.376 | 9.902 | 0 | ok |
| cpu-w32-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 84.928 | 11.488 | 9.964 | 0 | ok |
| cpu-w32-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 106.613 | 11.705 | 9.968 | 0 | ok |
| cpu-w32-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 82.675 | 11.981 | 9.971 | 0 | ok |
| cpu-w64-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 81.761 | 10.137 | 9.982 | 0 | ok |
| cpu-w64-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 89.531 | 10.686 | 9.982 | 0 | ok |
| cpu-w64-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 88.551 | 10.720 | 9.982 | 0 | ok |
| dds-original-capacity-stress-stress-stress-dds-iox-64-n1000-r1 | 1000 | — | — | — | —/— | — | — | — | — | failed |
| final-capacity-stress-stress-stress-a-64-n1000-r1 | 1000 | 10000000 | 9883217 | 9883217 | 9771/9955 | 142.868 | 10.989 | 9.948 | 0 | degraded |
| final-capacity-stress-stress-stress-a-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 130.878 | 11.279 | 9.948 | 0 | degraded |
| final-capacity-stress-stress-stress-a-64-n1000-r3 | 1000 | 10000000 | 9983456 | 9983456 | 9943/10000 | 103.424 | 11.265 | 9.952 | 0 | degraded |
| final-capacity-stress-stress-stress-b-64-n1000-r1 | 1000 | 10000000 | 9172606 | 9172606 | 8953/9589 | 91.213 | 9.929 | 9.934 | 0 | degraded |
| final-capacity-stress-stress-stress-b-64-n1000-r2 | 1000 | 10000000 | 9191621 | 9191621 | 8933/9611 | 89.893 | 9.959 | 9.944 | 0 | degraded |
| final-capacity-stress-stress-stress-b-64-n1000-r3 | 1000 | 10000000 | 9331549 | 9331549 | 9062/9759 | 77.426 | 10.220 | 9.941 | 0 | degraded |
| final-long-stress-stress-stress-shm-64-n1000-r1 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 85.898 | 11.561 | 59.775 | 0 | ok |
| final-long-stress-stress-stress-shm-64-n1000-r2 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 97.723 | 11.561 | 59.777 | 0 | ok |
| final-long-stress-stress-stress-shm-64-n1000-r3 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 96.830 | 12.055 | 59.834 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1-r1 | 1 | 10000 | 10000 | 10000 | 10000/10000 | 56.460 | 1.013 | 9.999 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1-r2 | 1 | 10000 | 10000 | 10000 | 10000/10000 | 90.851 | 1.011 | 9.999 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1-r3 | 1 | 10000 | 10000 | 10000 | 10000/10000 | 86.819 | 1.015 | 9.999 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1-r4 | 1 | 10000 | 10000 | 10000 | 10000/10000 | 95.845 | 1.014 | 9.999 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1-r5 | 1 | 10000 | 10000 | 10000 | 10000/10000 | 36.828 | 1.012 | 9.999 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n100-r1 | 100 | 1000000 | 1000000 | 1000000 | 10000/10000 | 41.235 | 1.841 | 9.987 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n100-r2 | 100 | 1000000 | 1000000 | 1000000 | 10000/10000 | 41.401 | 1.861 | 9.989 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n100-r3 | 100 | 1000000 | 1000000 | 1000000 | 10000/10000 | 42.599 | 1.887 | 9.987 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n100-r4 | 100 | 1000000 | 1000000 | 1000000 | 10000/10000 | 38.572 | 1.862 | 9.989 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n100-r5 | 100 | 1000000 | 1000000 | 1000000 | 10000/10000 | 33.134 | 1.788 | 9.991 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 94.648 | 11.480 | 9.965 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 70.376 | 11.654 | 9.974 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 73.852 | 11.826 | 9.973 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1000-r4 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 70.763 | 11.577 | 9.972 | 0 | ok |
| final-short-stress-stress-stress-shm-64-n1000-r5 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 73.423 | 11.694 | 9.971 | 0 | ok |
| workers-w32-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 89.358 | 11.820 | 9.978 | 0 | ok |
| workers-w32-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 68.358 | 11.510 | 9.973 | 0 | ok |
| workers-w32-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 94.210 | 12.218 | 9.971 | 0 | ok |
| workers-w32-stress-shm-64-n1000-r4 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 80.254 | 12.374 | 9.977 | 0 | ok |
| workers-w32-stress-shm-64-n1000-r5 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 84.520 | 12.199 | 9.975 | 0 | ok |
| workers-w64-stress-shm-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 81.180 | 10.642 | 9.982 | 0 | ok |
| workers-w64-stress-shm-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 45.457 | 10.418 | 9.986 | 0 | ok |
| workers-w64-stress-shm-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 69.275 | 11.249 | 9.984 | 0 | ok |
| workers-w64-stress-shm-64-n1000-r4 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 66.556 | 10.701 | 9.983 | 0 | ok |
| workers-w64-stress-shm-64-n1000-r5 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 96.058 | 10.505 | 9.986 | 0 | ok |
| cpu-w32-stress-dds-iox-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 465.614 | 0.886 | 0.040 | 0 | ok |
| cpu-w32-stress-dds-iox-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 7,135.278 | 0.911 | 0.038 | 0 | ok |
| cpu-w32-stress-dds-iox-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 3,686.912 | 0.895 | 0.036 | 0 | ok |
| dds-scaled-long-stress-stress-stress-dds-iox-64-n1000-r1 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 5,830.482 | 0.896 | 0.229 | 0 | ok |
| dds-scaled-long-stress-stress-stress-dds-iox-64-n1000-r2 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 3,304.807 | 0.890 | 0.205 | 0 | ok |
| dds-scaled-long-stress-stress-stress-dds-iox-64-n1000-r3 | 1000 | 60000000 | 60000000 | 60000000 | 60000/60000 | 4,300.132 | 0.886 | 0.230 | 0 | ok |
| dds-scaled-short-stress-stress-stress-dds-iox-64-n1000-r1 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 8,198.692 | 0.905 | 0.040 | 0 | ok |
| dds-scaled-short-stress-stress-stress-dds-iox-64-n1000-r2 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 602.612 | 0.882 | 0.037 | 0 | ok |
| dds-scaled-short-stress-stress-stress-dds-iox-64-n1000-r3 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 3,835.240 | 0.899 | 0.036 | 0 | ok |
| dds-scaled-short-stress-stress-stress-dds-iox-64-n1000-r4 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 8,546.665 | 0.880 | 0.036 | 0 | ok |
| dds-scaled-short-stress-stress-stress-dds-iox-64-n1000-r5 | 1000 | 10000000 | 10000000 | 10000000 | 10000/10000 | 534.757 | 0.877 | 0.039 | 0 | ok |


A 回退到 TLV 与 B 借样失败必须单列。各档共享块池数量不变；成功发送全收不代表达到计划输入。DDS 扩容只修改端点/通知器容量，必须使用同版本整套重建依赖；原安装的创建失败不能作为百万吞吐成绩。

## 计时与工装变化

延迟仍从准备本条载荷之前到应用取得，RPC 从准备请求到 success 响应。B 的载荷写入计入准备阶段；publish 主要提交描述符，所以大包发布耗时变化较小。未改变正常速度档 memset 工作量，全字节正确性档使用固定种子、随序号及位置变化的非均匀内容，二者分别报告。

发送停止后先公布逐话题成功数和最终序号，接收确认排空；无进展有界退出。逐话题位图核对重复/缺口，实际计划与成功数、队列淘汰和 wire 拒绝原因分列。限速多发 1 条的旧工装已修正，旧初筛不可用于性能结论。

静默档要求前一条确认送达后再等待 100 ms，单条在途，目标 1000 条，硬时长保留 20% 余量。没有启动下一条业务消息帮助唤醒前一条。

## 未闭环项与下一步

Socket 旧数据片没有逐消息 sequence；同类型同长度的 A 首片与 B 后续页无法区分。彻底修复需逐数据报携带版本、发布实例、消息序号及长度/偏移/校验，并明确两端共同升级范围。此前已询问该范围，未收到答复前保持现有协议，不擅自实施 wire 升级。

C3 的代表 RPC 已验证；不得据此归因全部历史超时或声称完整尺寸回归通过。O1 已按门槛撤回，小消息与千话题性能目标尚未达成。内核 perf_event_paranoid=4 拒绝采样，未修改系统设置，也不把队列驻留合并区间伪称为纯内核唤醒时间。

AGENTS.md 指定的团队 MCP 工具未在本会话开放，未执行 member_report_result 或 /compact，不声称已经团队回报。

## 复现方式

所有构建和中间结果放在仓库外；下例 task_work 取新的绝对路径。正常产品由 transport_comparison/CMakeLists.txt 构建，避免顶层关闭 Python 时引用缺失安装目标的问题。

```bash
python3 -B test/transport_comparison/repair_units.py --work "$task_work/units" --run
cmake -S test/transport_comparison -B "$task_work/final/build"
cmake --build "$task_work/final/build" -j6
python3 -B test/latency_breakdown/instrument.py "$task_work/trace-final"
cmake -S test/latency_breakdown -B "$task_work/trace-final/build"
cmake --build "$task_work/trace-final/build" -j6
bash test/transport_comparison/prepare_scaled_deps.sh "$task_work/scaled-deps"
cmake -S test/transport_comparison -B "$task_work/scaled-final/build" -DDDS_ROOT="$task_work/scaled-deps/prefix"
cmake --build "$task_work/scaled-final/build" -j6
python3 -B test/transport_comparison/repair_campaign.py --work "$task_work" --phase correctness
python3 -B test/transport_comparison/repair_campaign.py --work "$task_work" --phase comparison
# 全量阶段必须单独核对 Socket 未闭环状态，失败记录不可删除。
python3 -B test/transport_comparison/repair_campaign.py --work "$task_work" --phase full
python3 -B test/transport_comparison/repair_report.py --work "$task_work"
```

每阶段非零退出码表示存在未通过的格，不能用最后一条命令覆盖前面的失败。复现默认保留中间证据以便续跑；最终验收完成后先汇总 Markdown，再清理本任务临时产物，禁止停止宿主 RouDi。

## 环境与构建指纹

Git 基点：1d52fba1b25e5aad9de13a4abf742ceb7d786277。Linux zwc-leju 6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC Fri Aug  7 13:43:15 UTC  x86_64 x86_64 x86_64 GNU/Linux。CPU 集合：pid 3359458 的当前亲和力列表：0-31。

| 构建 | 程序 SHA-256 | 库 SHA-256 |
|---|---|---|
| final | 39f01e64462532ab6fe6dce540b9f434bab12e71ec4a5a9b3509f7e6c5e8fec3 | d24eb7c4d09a1370de1882a75b85b2207202d6fe1f49f5212ab968ffdfbf8aae |
| scaled-final | 4b9b2f1cffc2ac6fcdf0f54c9d84f470ea564bc405818cad569c9db4d4814a5a | d24eb7c4d09a1370de1882a75b85b2207202d6fe1f49f5212ab968ffdfbf8aae |
| trace-final | 7503f3924b18ac3e2f63cd371610968802ceb09e6e84e65487e78c91ce5ac2cb | ff1fa42323e2c08d788115e7c5a59a529e7f7e12518573bc3ae748181c05fb8a |
| baseline-final | aeb914b08f2671a8ff2e09a6188908be0be8c543a75cd156e33cfb5746cb620e | d24eb7c4d09a1370de1882a75b85b2207202d6fe1f49f5212ab968ffdfbf8aae |
| c2-fixed | 2371af4dc6a8aed9baf36c966facc79a5f0d21341d414a09d30a4cd3ac4cc96d | 11a5e05fbd0e6f0aaba6ebd8aa76b4fe32246bab9f3e1c4560bca02c482e82e2 |


## 运行配置记录

| manifest | SHA-256 | 当时运行器 SHA-256 | 当时矩阵脚本 SHA-256 |
|---|---|---|---|
| audited/repair-progress-manifest.json | d63e912d6beb50000404cbce09650efaa7623515908f414bdee210487404f287 | fd0d49f98c4a147f1eb50e6b092a7c38262398534cba3de1a2502da9dfba58cd | 71589670c16ae7395f5089e03ba0b2c27eaefa30b3f94b2d7e04b4b93905065e |
| audited/repair-quick-manifest.json | 3d6bda51517c13e06dc6a42554e5e53ae952dc25c7de9adde944845ac3f603c0 | fd0d49f98c4a147f1eb50e6b092a7c38262398534cba3de1a2502da9dfba58cd | 71589670c16ae7395f5089e03ba0b2c27eaefa30b3f94b2d7e04b4b93905065e |
| final/cpu-profile-manifest.json | b690123dbd06df6b037b886c32eef26b3b58561b8f312bd1d7c64268f4c9670f | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | d3f77d5aa90b27bf4499f7d485a0e5b08357eaad86c34aac07972eb858fd9c0c |
| final/dds-original-capacity-stress-manifest.json | 0934582ab3cb06a77b4b64e943024a858835fbb5a7a87f1ddd6c07947a8541df | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-capacity-stress-manifest.json | ed8e38455e5fa7ab2e7a744052808c8e994a6d4da2c6ede3b8da891394943757 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-control-control-manifest.json | a7d3c67555fb074817b299cde447a7798493f34822428bef771c97c2155db20b | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-idle-idle-manifest.json | 45e28eece0fa4bbff7083bc2488601be8feabc06a2f998ed46446d6e7c05fda8 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-input-baseline-manifest.json | 467f988e8d64d47b15d970de4c626ecffe646b0b61928947d866671aa01a2afc | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-long-stress-manifest.json | 557c609aaa3ea2fe1b8d6312694f25f5d2a2d3f9bab01f2c10dff5717b960d01 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-quick-quick-manifest.json | a475b7187e3e0135b71758ebe063855990e1fa74a0e9aa276a662b8b8802b0de | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/final-short-stress-manifest.json | c28f32b0df2f73902c4a8e325b44c16ebe2ddfa92144225f236df85cc060e2f0 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| final/worker-compare-manifest.json | 782deca8773f61936ec351cf5ca0ac27244ed74ae8d2b97e3250300eb6fb9f67 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | fd4d739ea6285831e9838276a193c714a40b3f5d508a4d20e3f6923239285019 |
| paired/o1-screen-manifest.json | 863271f13f1cdfc5eb91e680de4bab3a066bc8fa1bc9b5a28fc66c3f549c38e4 | fd0d49f98c4a147f1eb50e6b092a7c38262398534cba3de1a2502da9dfba58cd | 4b7333af3e3688aacd00d77a7d7756a5929399feee5b994a05c1964f4f199362 |
| paired-final/o1-c2-manifest.json | 752a294e70aaf85c32029fd0418405687cc5c05375342436722938e45dfdfb9e | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 4b7333af3e3688aacd00d77a7d7756a5929399feee5b994a05c1964f4f199362 |
| probe-final/probe-manifest.json | 6b2b286d372e1d6a5a1981afc3bf37e05bfe179cbf187812c15e778a62d75568 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 1c6a3b32051d7f79e579667d76870d97b34c398d0857185332c9f9f47f606a8f |
| scaled-final/cpu-profile-manifest.json | a1722d02c670ac174c262d07123e8d092579cf46bf464d80f7ec9d319a920242 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | d3f77d5aa90b27bf4499f7d485a0e5b08357eaad86c34aac07972eb858fd9c0c |
| scaled-final/dds-scaled-long-stress-manifest.json | b8ff866b751e25944c5060d493820fb6814d4f03e910c52b7559dfe61a889761 | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| scaled-final/dds-scaled-short-stress-manifest.json | fc462f6823fb0087cda86028fa48958401118e90dfa5a8643a8dad94d437831c | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |


| 最终源码/脚本 | SHA-256 |
|---|---|
| src/dzIPC/common/data_rev.cc | a382f7dab1568164c7b33e37d79bd63675c394ad463e5f28fdfceb75af88c3e8 |
| src/dzIPC/threepools/recv_worker.cc | 65a149e30519ea968c327765cb9dba8fbed63fdf52418ff341189b97dc22c2de |
| src/libipc/ipc.cpp | 01012465a7771bea9542c941e108e7e26a11de00651c8a9a501362360b860f05 |
| test/test_recv_fragment_isolation.cpp | f531fb82af0a06db32f9cd0aac03458de3c96f89fa284ffe84fd4798ddbf6e5c |
| test/test_recv_worker.cpp | 0c35e1debc1da8f6c90cffaf1c4305d6beeb759c64810868531b026eea9f1eff |
| test/test_socket_reliable_crc.cpp | d2b9db611db6059bac7d03a8a1ca18b684ae069b12c5362f3e9a73acd5cfd779 |
| test/transport_comparison/audit.py | c09aea788afdcbe655693f82f2b7f9267453239c76efbdf731dcb31f2d67984d |
| test/transport_comparison/comparison.cpp | 664dcca3549f5f9633098302e443d97e994edd6652d712c719d96706fcb68c65 |
| test/transport_comparison/generate_types.py | 730ce23d40d6650adc39d8b0c0b2831868f778e9b9466722fe107873ff134efe |
| test/transport_comparison/prepare_roudi.py | 7d57be86cbba2f552472da2e20e1e9205a2df63a07abb5fc7cbef4c20ff7c505 |
| test/transport_comparison/repair_campaign.py | 353831fe490d1096934f77dbd2ef69bab7a9425b4db79fb39446fe0ed9466501 |
| test/transport_comparison/repair_compare.py | 4b7333af3e3688aacd00d77a7d7756a5929399feee5b994a05c1964f4f199362 |
| test/transport_comparison/repair_cpu_profile.py | d3f77d5aa90b27bf4499f7d485a0e5b08357eaad86c34aac07972eb858fd9c0c |
| test/transport_comparison/repair_probe.py | 1c6a3b32051d7f79e579667d76870d97b34c398d0857185332c9f9f47f606a8f |
| test/transport_comparison/repair_report.py | 9fa7069b21f946f6bdcfcaae98cd788bc5d34f216265d4e48c0df306ebafbadf |
| test/transport_comparison/repair_runs.py | 5a27d0c6e727d0dd48639ee684e37f33f82084c39e73165b15705b1344e7ca8e |
| test/transport_comparison/repair_units.py | be89744ec25591e2f26998a3fbd43e3eeeb2b86afcb278856c3da899b8bd52da |
| test/transport_comparison/repair_worker_compare.py | fd4d739ea6285831e9838276a193c714a40b3f5d508a4d20e3f6923239285019 |
| test/transport_comparison/report.py | fd900724553b7cc86907c6e338d37ced7e6011b79b43ed63dad65fe4f8d88317 |
| test/transport_comparison/run.py | a2611ef8c2fa917fda4385c75ab9837d49437207afa34ae01d5f198d2c9fa1db |
| test/transport_comparison/test_orchestrator.py | 69c2f4763c1321cacf54fa18cbd8e759e4abf4ed6f1aec110dbfded757170204 |


## 静默后单条

| case_id | 计划 | 成功发 | 收 | p50 µs | p99 µs | 最大 µs | 停止推进 | 状态 |
|---|---|---|---|---|---|---|---|---|
| final-idle-idle-idle100-pubsub-a-1024-n1-r1 | 1000 | 1000 | 1000 | 26.911 | 194.763 | 224.896 | 0 | ok |
| final-idle-idle-idle100-pubsub-a-8-n1-r1 | 1000 | 1000 | 1000 | 25.411 | 164.671 | 220.112 | 0 | ok |
| final-idle-idle-idle100-pubsub-b-1024-n1-r1 | 1000 | 1000 | 1000 | 47.704 | 209.242 | 295.354 | 0 | ok |
| final-idle-idle-idle100-pubsub-b-8-n1-r1 | 1000 | 1000 | 1000 | 29.016 | 217.743 | 381.208 | 0 | ok |


现有频率策略：powersave。未修改调度策略，CPU 0–31 包含 P/E 核与 SMT，线程可以迁移；跨轮波动如实保留。

RouDi 测试池配置（容量字节 / 块数）：128/10000、1024/5000、16384/1000、131072/200、1048576/50、2097152/50。最后一档覆盖 1 MiB 应用数据加协议开销。


## 测量窗口内 CPU 归因

独立诊断档每 100 ms 读取本轮子进程 /proc；采样轮开始时检查共享控制文件的 start/end 窗口。每线程用首末样本的差值除以自身观察秒数，再汇总核当量。首尾约 100 ms 未覆盖；线程依次读取，末轮个别读数可能超出终点一次遍历的耗时，并非严格同步截点。计数精度为系统时钟节拍，采样器会产生额外 CPU 与调度干扰。主线程与其余线程单列，其余线程不能全部当作 worker。观察到的 CPU 集合不是迁移次数。

| 构建 | case_id | 状态 | 采到接收线程 | 用户态核 | 系统态核 | 系统态 % | 自愿切换/s | 非自愿切换/s | p99 µs | 采样器 CPU s | 采样异常 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| final | cpu-w16-stress-shm-64-n1000-r1 | ok | 18 | 4.595 | 6.806 | 59.697 | 274,415.531 | 651.189 | 75.273 | 0.311 | 0 |
| final | cpu-w16-stress-shm-64-n1000-r2 | ok | 18 | 5.017 | 6.237 | 55.420 | 270,715.965 | 572.266 | 112.005 | 0.356 | 0 |
| final | cpu-w16-stress-shm-64-n1000-r3 | ok | 18 | 5.099 | 6.278 | 55.182 | 295,319.115 | 672.605 | 50.281 | 0.337 | 0 |
| final | cpu-w32-stress-shm-64-n1000-r1 | ok | 34 | 5.391 | 6.102 | 53.095 | 693,303.163 | 7,322.742 | 84.928 | 0.457 | 0 |
| final | cpu-w32-stress-shm-64-n1000-r2 | ok | 34 | 5.480 | 6.229 | 53.201 | 706,034.244 | 4,915.308 | 106.613 | 0.429 | 0 |
| final | cpu-w32-stress-shm-64-n1000-r3 | ok | 34 | 5.502 | 6.485 | 54.101 | 667,555.012 | 2,195.112 | 82.675 | 0.422 | 0 |
| final | cpu-w64-stress-shm-64-n1000-r1 | ok | 66 | 5.250 | 4.872 | 48.129 | 891,017.185 | 8,335.769 | 81.761 | 0.726 | 0 |
| final | cpu-w64-stress-shm-64-n1000-r2 | ok | 66 | 5.508 | 5.125 | 48.198 | 865,355.998 | 3,397.458 | 89.531 | 0.639 | 0 |
| final | cpu-w64-stress-shm-64-n1000-r3 | ok | 66 | 5.725 | 4.982 | 46.528 | 877,518.320 | 3,268.827 | 88.551 | 0.605 | 0 |
| scaled-final | cpu-w32-stress-dds-iox-64-n1000-r1 | ok | 10 | 0.882 | 0.003 | 0.342 | 1,900.913 | 5.353 | 465.614 | 0.217 | 0 |
| scaled-final | cpu-w32-stress-dds-iox-64-n1000-r2 | ok | 10 | 0.908 | 0.002 | 0.222 | 1,772.407 | 11.617 | 7,135.278 | 0.213 | 0 |
| scaled-final | cpu-w32-stress-dds-iox-64-n1000-r3 | ok | 10 | 0.892 | 0.002 | 0.227 | 1,895.113 | 5.468 | 3,686.912 | 0.195 | 0 |


## 无采样 worker 配置对照

同一最终产品、1000 话题 × 1000 msg/s、64 B、4 个发布线程、每格 10 秒；32/64 worker 按固定种子交错，CPU 集合不变。全部结果（包括失败）见账本，表内性能只汇总通过轮。

| worker | 通过/总轮 | CPU 核中位数 | 最小核 | 最大核 | 均值 µs | p99 µs | 扫描/消息 |
|---|---|---|---|---|---|---|---|
| 32 | 5/5 | 12.199 | 11.510 | 12.374 | 25.682 | 84.520 | 84.104 |
| 64 | 5/5 | 10.642 | 10.418 | 11.249 | 22.247 | 69.275 | 46.946 |

| 配对轮 | 64 相对 32 CPU 变化 % | p99 变化 % |
|---|---|---|
| 1 | -9.966 | -9.152 |
| 2 | -9.491 | -33.502 |
| 3 | -7.933 | -26.467 |
| 4 | -13.516 | -17.068 |
| 5 | -13.889 | 13.651 |



## CPU 证据的含义与下一步

测量窗口内 CPP 32-worker 的三轮中位数为用户态约 5.48 核、系统态约 6.23 核；DDS 扩容档约 0.89 核用户态、0.002 核系统态。CPP 接收主线程约占 1 核，DDS 主线程约 0.004 核。DDS 的“0 线程”是应用不额外创建接收 worker，采样仍看到 10 个接收进程线程，不能称整个进程零线程。

CPP 自愿上下文切换在 16/32/64 worker 下分别约 27.4/69.3/87.8 万次/s；DDS 约 1895 次/s。64-worker 的切换次数更多，系统态 CPU 却较低，因此不能把每次上下文切换视为固定成本。worker 数同时改变每个等待集合的大小、调度并行度和消息聚合机会，本实验只能确认配置效果，不能单独证明其中哪个因素致因。

据此，优先级应是分别测量 waitv 调用频率、EAGAIN/真正等待比例与等待集合长度，再检验就绪消息的聚合处理。用户态分配/共享统计仍可能显著，但不能把接近一半的系统态时间归因于 Sample 构造。O2 缓冲复用也不能提前承诺降低系统态开销；需单变量测量，保留 token 生命周期、并发注销和空闲阻塞。

这轮不新增持续自旋、不削减应用校验、不删除同步来换取数字。64-worker 对照未达到 CPU 下降 30% 的门槛，保留默认 32-worker；将其视为诊断结果。CPP 更低的千话题 p99 与 DDS 更低的 CPU 同时报告，不能只选一个指标宣称全面领先。


## 本轮分段结论

已完成 162 个 U/O/T 格，其中 162 格通过；开启抽样的 54 格共关联 1,397,736 个样本，未关联 0，时序异常 0。

| 路径 | 输入 msg/s | 轮 | 抽样总均值 µs | 投递前→worker µs | worker 后取包 µs | 取包→应用 µs | 其中队列交接 µs | 处理前占比 % |
|---|---|---|---|---|---|---|---|---|
| a | 各自饱和 w8 | 3 | 6.256 | 4.985 | 0.560 | 0.443 | 0.189 | 79.556 |
| a | 500000 | 3 | 2.461 | 1.085 | 0.651 | 0.452 | 0.186 | 44.212 |
| b | 各自饱和 w8 | 3 | 6.516 | 5.226 | 0.589 | 0.437 | 0.189 | 79.312 |
| b | 500000 | 3 | 2.522 | 1.138 | 0.653 | 0.448 | 0.188 | 45.112 |

各列是独立轮均值的中位数，不能逐列相加；占比先在同轮计算再取中位数。A/B 饱和档的主要延迟仍在 worker 开始处理前，但该段包括队列驻留、前序服务和通知调度，不能全部称为 futex 唤醒。相同 50 万输入时该段明显缩短，说明输入负载必须受控。应用队列交接约 0.19 µs，不是饱和延迟的最大分段。

U/O/T 的速度差包含探针扰动和跨轮波动，正式速度使用 U；抽样阶段成本使用 T。DDS 的发布入口→take 包含其自己的发布与接收阶段，不能当作 CPP 的同名 worker 段。相同输入差距较小，也不能据此认为高话题数 CPU 已解决。


## 本阶段交付核对与清理记录

- 已归档 771 格实验记录，其中最终构建相关 505 格；失败与不支持项未删除。阶段退出码、配置、源码/二进制指纹保留在 Markdown 账本中。
- 最终产品三处修改和 comparison.cpp 的指纹与已测最终构建一致。17 个 Python 脚本语法检查、文档相对链接检查和 `git diff --check` 通过。
- VS Code 的 C++ 诊断仍解析到 `/usr/local/include` 的旧安装头，出现缺少现有类型/成员的报错；不能称编辑器诊断全绿。已使用实际单元构建的编译参数，对当前三处产品文件分别执行 `-fsyntax-only`，全部通过；此前真实构建和 95 个相关单元用例通过。未为消除旧头诊断而改动产品接口。
- 已确认没有本次测试活动进程，并删除 `/var/tmp/cppipc-repair-20261002-gwguoxrl`（约 577 MiB），包括构建、源码副本、JSON/CSV、日志和探针数据。私有测试 IPC 随隔离环境退出回收；宿主 RouDi PID 1207 保持运行。
- 仓库新增内容仅为报告文档、测试源码与脚本；没有保留中间计算文件。后续复现需重新构建，历史报告中的临时路径用于来源说明。
- 这次交付是有明确未完成项的阶段结果：C4 未闭环、完整 18 尺寸矩阵未执行、性能门槛未达到。不得将清理完成解释为整体验收通过。

补充诊断的复现命令（先按前文构建，`task_work` 使用新的仓库外绝对路径）：

```bash
task_roudi=/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi
python3 -B test/transport_comparison/prepare_roudi.py "$task_work/final/roudi.toml"
bash test/transport_comparison/isolated.sh "$task_roudi" "$task_work/final/roudi.toml" "$task_work/cpu-roudi.log" python3 -B test/transport_comparison/repair_cpu_profile.py --work "$task_work/final" --workers 16 32 64 --rounds 3 --seconds 10
bash test/transport_comparison/isolated.sh "$task_roudi" "$task_work/final/roudi.toml" "$task_work/worker-roudi.log" python3 -B test/transport_comparison/repair_worker_compare.py --work "$task_work/final" --workers 32 64 --rounds 5 --seconds 10
bash test/transport_comparison/isolated.sh "$task_work/scaled-deps/prefix/bin/iox-roudi" "$task_work/scaled-final/roudi.toml" "$task_work/dds-cpu-roudi.log" python3 -B test/transport_comparison/repair_cpu_profile.py --work "$task_work/scaled-final" --backend dds-iox --workers 32 --rounds 3 --seconds 10
```

采样复现会受到机器、调度和轮次波动影响；报告中的固定叙述对应本次已执行实验，新数据必须重新核对结论。生成报告脚本不会替用户删除新运行的中间证据。
