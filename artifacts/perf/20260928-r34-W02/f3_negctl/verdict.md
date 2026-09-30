# W02 统一跨进程基准 · 判定（verdict）

> run_id: `r30negctl_backlog`  
> source_revision: `e800ccc496ac710b711c9346709e86a148c41241`（工作区含未提交改动，原文见 `manifest.json:working_tree_diff`）  
> 二进制 sha256: `7d77172381ad3ce44ee94dcf783dabe023637125070cfc979e3c94903f4ee616`；libipc sha256: `cf209393773d51ee4762dd95bfd1b04f873ec54aa8f61cf5a7ed92be2990fa46`（`manifest.json:binary_sha256`）  
> 时基: CLOCK_MONOTONIC，vdso_ns_per_call=12.1792 syscall_ns_per_call=82.7608 vdso_in_use=true（`manifest.json:clock_cost_ns_per_call`）

## 1. 机器判定摘要

```
process_model : cross-process (pub/sub 均为 fork+exec 的新映像)
config_hash   : 3be9041c4dacfc12
payload_shapes: 1024B(实际 1021B + 头 32B)
cases         : 1 通过 0 / 未通过 1
```

## 2. 逐用例判定（每行引用原始文件）

> 判定来源：本节所有「判定」列**只读** `run_one_case()` 里唯一一处判定逻辑写下的 `failure_reasons`/gate 布尔；⛔ 本渲染不重算任何判据（纪律：同一事实只有一处判定逻辑）。

| case | 路径 | 组 | 载荷 | 等待 | 计划/尝试/成功/失败 | 接收 | 丢失 | 重复 | 乱序 | 校验失败 | 迟发 | 积压(周期) | 结构异常 | 段头矛盾 | gate(late/backlog/blocked/abn/hdr) | dzflat/回退 | 文件 | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `r0_tlv_full_blocking_1024B_1000Hz` | tlv | full-read | 1021B | blocking | 1000/1000/1000/0 | 1000 | 0 | 0 | 0 | 0 | 1 | 0.114 | 0 | 0 | 1/1/0/0/0 | 0/1803 | `samples/r0_tlv_full_blocking_1024B_1000Hz.samples.csv` | **未通过** |

（gate 列 = 「该 gate 判失败的次数」，0 = 通过；阈值见 `manifest.failure_thresholds`。所列为**判定结果**，不是原始量 —— 原始量在同行的迟发/积压/结构异常/段头矛盾列。）

## 3. 计时边界（禁止混用结束点，§10.7）

| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |
|---|---|---|---|---|
| `r0_tlv_full_blocking_1024B_1000Hz` | 3685/18135 | 10324/77626 | 25/204 | 15713/85327 |

说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。

## 4. 失败/未通过原因（逐条，不静默排除）

- `r0_tlv_full_blocking_1024B_1000Hz`:
  - 迟发超出阈值: late=1 (0.1000%) > 阈值[abs<=0 且 rate<=0.0000%]（阈值见 manifest.failure_thresholds；仅落列不判定=静默排除，故进判定）
  - 发送积压超出阈值: backlog_max=114085 ns = 0.114 个周期 > 阈值 0.000 （阈值见 manifest.failure_thresholds）

## 5. 跨进程身份与正常退出

- `r0_tlv_full_blocking_1024B_1000Hz` identity: parent_pid=4 pub_pid=23 sub_pid=22 | pub_ready_ns=150810247745344 in [150810244754009,150810265327848] | sub_ready_ns=150810247817965 in [150810244671941,150810265327690] | pub_starttime_ticks=15081024 sub_starttime_ticks=15081024 | pub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0 | sub_lib=/home/zwc/cpp_ipc_dds/build/lib/libipc.so.1.3.0

（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例在 §4 里显式列为失败原因，不当通过。）

## 6. 未确认项

- **A/B 可分性只在发布侧成立**：实测 `dzflat-a` 与 `dzflat-b` 两档的消费者侧`via_view`/`via_object` **完全相同**（各 13500 / 0，10 轮 full 档合计）⇒ `via_view` **不是** B 档的区分判据（早前文档把它写成 B 档专属判据，已更正）。可分性证据在**发布侧**：`wire_bytes_source`（A=`对象 dzflat_size()` / B=`B 借样 chunk 容量`）与实际路径条数计数。
- **失败量阈值是本基准自定的预算**（`manifest.failure_thresholds`）：迟发默认「次数>5 **且** 比率>0.5%」才判失败。它是**可判**而非**无条件失败**；引用时必须连阈值一起引。
- `r0_tlv_full_blocking_1024B_1000Hz`: 定速档存在迟发，最大积压 114085 ns（迟发 1 次）—— 该轮延迟含排队成分

