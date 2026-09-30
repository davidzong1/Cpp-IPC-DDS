# 本机复现（W01 新增实验）

> 本目录承载 W01 为**判定"上下文切换 / 空闲 CPU 口径"**而新增的一条可复现实验。
> 它是本次纠偏中唯一**新产生的数据**；除此之外 W01 不改产品代码、不重跑基准、不覆盖任何原始样本。

## 1. 为什么需要它

主报告初版把两处读数并列使用：

| 侧 | 读数 | 探针 | 取值来源 |
|---|---|---|---|
| 改造前基线库（477 路） | `ctx_vol=19325`、`cpu_cores=1.124` | `build_baseline/bin/baseline_probe` | 日志**自报** `ctx_scope=sum(/proc/self/task/<tid>/status) 全线程聚合` |
| 改造后 worker（477 路） | `ctx_vol=10`、`cpu_cores=0.0050` | `test/perf/out/20260927_t6_probes/t6scale` | `/proc/self/status`（ctx）与 `/proc/self/stat`（CPU） |

⇒ 于是产生两个必须回答的问题：
1. `/proc/self/status` 的 `voluntary_ctxt_switches` 是不是"全线程组累计"？
2. `/proc/self/stat` 的 `utime+stime` 是不是"全线程组累计"？

只有第 2 问答"是"、第 1 问答"不是"，才能解释"CPU 可比、ctx 不可比"，从而把初版的"225×"准确地劈成两半。

## 2. 实验设计

`csw_cpu_scope_probe.c`：

- 起 **8 条 `usleep(1 ms)` 保活线程**（制造大量自愿上下文切换）+ **1 条忙转线程**（制造 CPU）；
- 所有线程在**整个 2 s 采样窗口内保活**，窗口结束后才置 `g_stop` 退出 ⇒ 避免线程退出导致"遍历 `/proc/self/task`"的累计失真；
- 窗口首尾各读一次，同时收集**四种**读数：
  - A：`/proc/self/status` 的 `voluntary_ctxt_switches`（调用线程自身）
  - B：遍历 `/proc/self/task/<tid>/status` 求和（全线程组）
  - C：`/proc/self/stat` 的 `utime+stime`（调用者视角）
  - D：遍历 `/proc/self/task/<tid>/stat` 求 `utime+stime` 之和（全线程组）

## 3. 结果（`csw_cpu_scope_probe.output.txt`）

```
window=2.0s  payload: 8x usleep(1ms) keep-alive threads + 1x busy spin thread
CLK_TCK=100
A  /proc/self/status voluntary delta       = 1
B  task-group       voluntary delta       = 15171
C  /proc/self/stat   utime+stime delta     = 205 ticks (1.0250 core over 2s)
D  task-group        utime+stime delta     = 206 ticks (1.0300 core over 2s)
judgement: A << B  => self/status ctxt switch is NOT thread-group aggregate
judgement: C~=D (C/D = 0.995) => self/stat CPU IS thread-group aggregate
```

**判读**

| 结论 | 依据 | 对主报告的作用 |
|---|---|---|
| `/proc/self/status` 的 ctxt switch **不是**全线程组累计（差 4 个数量级） | A=1 vs B=15171 | 把"19325 → 10"判为**口径不一致**，结论删除（UF-03） |
| `/proc/self/stat` 的 `utime+stime` **是**全线程组累计 | C=205 vs D=206（比值 0.995） | 保留"空闲 CPU 1.124 核 → 0.0050 核（≈225×）"为**同口径**结论 |

复现比例：`B/A ≈ 15000×`、`C/D ≈ 0.995`。两次独立运行（本探针的早期简化版曾得 A=1 / B=15192）结论一致。

## 4. 构建与运行

```bash
cd /home/zwc/cpp_ipc_dds
gcc -O2 -o /tmp/csw_cpu_scope_probe \
  "docs/消息接收架构改造/团队改造交付/W01/本机复现/csw_cpu_scope_probe.c" -lpthread
/tmp/csw_cpu_scope_probe | tee /tmp/csw_cpu_scope_probe.output.txt
```

> ⚠️ **可移植性提示**：源码注释里若出现 `/proc/self/task/*/status` 会被 C 编译器当成注释结束符（`*/`）。本文件已改用 `/proc/self/task/<tid>/status` 写法；修改时请勿改回。

## 5. 本实验的边界（不要过度外推）

1. 只证明**字段口径**，**不**给出"某个具体进程的上下文切换次数"。要报告真实空闲切换数，仍需在目标进程上按统一口径重测（UF-03 解除条件）。
2. **未**取得内核源码形式的机制来源（`torvalds/linux` 抓取超时）⇒ 见 UF-06，机制"为什么"不进入任何结论。
3. 本机 `CLK_TCK=100`，故 tick 粒度 10 ms；结论只用"量级关系"（A≪B、C≈D），不依赖 tick 精度。
