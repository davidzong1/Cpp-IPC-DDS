# ⚠️ 已作废（superseded）：初版「loan 借样死悬挂上界不成立」探针

**状态：结论已撤回（2026-09-28，队长 D-16 第 3 条）。**

本目录的 `w09_probe_loan_dead_hold_{child,parent}.cpp` 与 `.out.txt` 是**初版**探针。
它们两端使用**完全相同的 route 名**（`w09leak / w09leak_topic`），因此观测到的
`PARENT_CAN_LOAN=0` 实际来自 **`loan()` 的单生产端 `ready_sending()` flag 被暴死进程永久占用**
（`src/libipc/ipc.cpp:1818-1822` 在任何 chunk 借用**之前**判定；单生产端下是
`atomic_flag.test_and_set()`，见 `src/libipc/queue.h:143-146` + `src/libipc/circ/elem_array.h:62-68`），
**与 chunk 池容量无关**。

**更正后的复现件（请用这一组）**：

| 文件 | 用途 |
|---|---|
| `t19_leak_child_unpublished.cpp` | 子进程借 N 块不发布后 `_exit`（可指定 route 名） |
| `t19_leak_parent_diff_route.cpp` | 父进程用**不同 route 名**测同档剩余块数 |
| `t19_leak_child_clean_exit.cpp` | 对照：正常返回（跑析构 ⇒ 释放 sender flag） |
| `t19_leak_experiments.out.txt` | 两条缺陷的原始输出 |

**结论**（详见 `../容量与背压_交付.md` §7.3 与 `../回退计数与三种语义分离.md` §8）：
1. 未发布借样的 chunk 占用 = `40 − N`（逐点符合，朴素上界**成立**）；
2. **单生产端 sender flag 暴死泄漏**（同 route 名 ⇒ 后续生产端永久无法借样；`send()` 不受影响）。

保留本目录文件仅为**审计留痕**（方案 §12「只追加、不覆盖」），⛔ 不得再被引用为结论依据。
