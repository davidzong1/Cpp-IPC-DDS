# verdict（W05 / t6）—— 自评 S3

| 判据 | 结论 | 证据 |
|---|---|---|
| 模块已接入（S2） | ✅ | `src/dzIPC/shm_pub_sub_ipc.cc` 的 `PubHeartbeatState`/`SubHandshakeState` + `RegistrationToken` 接入点 |
| 单项验证（S3） | ✅ | 千路线程数 1002（1+N+1）=O(1)、兼容臂 3001=1+N+2N；稳态 `tick_overrun` 增量 0/5 轮；churn 后 `entry_count` 归零；控制面回归全绿（除 §6 既有缺陷） |
| 1000 发布/订阅组合下控制线程数固定 | ✅ | `compose_sched_n1000.log` 1002；`compose_compat_n1000.log` 3001 |
| 重连/死亡检测/注销无回调泄漏 | ✅ | `churn_n200_r3.log`（entry_count 每轮回 0、callback_exception=0）；`test_shm_sub_dtor_gate` 2 OK；`test_shm_ready_transition` 3 OK |
| 已有 `test_shm_control_scheduler` 等回归全绿 | ⚠️ 部分 | 8 项中 7 项全绿；`test_recv_worker` 有 SEGFAULT，**已反事实归因为既有缺陷**（W05 开/关 21/24 vs 23/24），见交付 §6 |
| 重配+重编后 A/B 计数仍可分 | ✅ | `test_w08_dzflat_ab --gtest_filter='*SeparableByConfig*'` PASSED；四路径调用点全在 |

## 未自证项（⛔ 不自我升级）
- **独立验收（S4）**：归架构负责人经 t14/R1。
- **正式性能结论**：`tick_duration_max_ns` 的**启动突发**（2.8~57.5 ms）只在本包做了定性与分段读数；随 route 数变化的曲线由 W11 正式采集。
- **准入 5（W04-F2 顺序）**：本阶段判定"不适用"（route 未进 worker 池），该判定交评审独立核（交付 §9 第 2 条）。
- **W06 义务**：`remove_route`→`begin_rebuild`→`add_route` 的插入由 t7 完成，本包不代做。
