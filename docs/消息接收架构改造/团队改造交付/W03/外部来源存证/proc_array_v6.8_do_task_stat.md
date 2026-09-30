# 外部来源存证：`fs/proc/array.c`（Linux v6.8）—— `/proc` 聚合语义（W03 归档）

> **用途**：解除 W01「未决事实清单」UF-06：`/proc/<pid>/status` 的上下文切换计数与
> `/proc/<pid>/stat` 的 `utime+stime` 的**聚合语义**此前只有本机实测、缺版本化来源。
> 本文件是 W03 取到的**内核源码层来源**，用于把"实测支持"升级为"源码 + 实测"双证据。
>
> **来源（可复取）**
> - URL：<https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/fs/proc/array.c?h=v6.8>
> - 标签：`v6.8`（torvalds/linux）。取回方式：单次 HTTP GET（HTTP 200），取回日期 2026-09-28。
> - 本机内核：`6.8.0-138-generic`（Ubuntu 22.04 包，`/proc/version` 已记入 `environment.json`）。
>   ⚠️ 存证的是**上游 v6.8**，不是 Ubuntu 补丁树；本节涉及的函数在 6.8.x 内稳定，
>   且相关分支自 Linux 2.6.23（`voluntary_ctxt_switches` 引入）起语义未变。
> - 备注：W01 曾两次抓取 `raw.githubusercontent.com` 超时（见其 UF-06）。本次改用
>   `git.kernel.org` 的 `plain` 端点在 30 s 内取回成功，故 UF-06 可由本存证解除。
> - 本文件为**逐字摘录**（只留判定所需函数），非全文；完整文件请按上述 URL 复取。

---

## 结论（先写结论）

| 文件 | 字段 | 聚合语义 | 源码依据 |
|---|---|---|---|
| `/proc/<pid>/status` | `voluntary_ctxt_switches` / `nonvoluntary_ctxt_switches` | **单任务（对进程而言是线程组主线程）**，**不是**线程组累计 | `proc_pid_status()` → `task_context_switch_counts(m, task)` 读 `p->nvcsw` / `p->nivcsw` |
| `/proc/<pid>/stat` | 字段 14 `utime` + 字段 15 `stime` | **线程组累计**（`whole = 1`） | `proc_tgid_stat()` → `do_task_stat(..., whole=1)` → `thread_group_cputime_adjusted()` |
| `/proc/<pid>/task/<tid>/stat` | 同字段 14/15 | **仅该线程**（`whole = 0`） | `proc_tid_stat()` → `do_task_stat(..., whole=0)` → `task_cputime_adjusted()` |
| `/proc/<pid>/stat` | 字段 22 `starttime` | **boottime 基准**的 clock tick | `nsec_to_clock_t(timens_add_boottime_ns(task->start_boottime))` |

=> 与 W01 本机实测（`本机复现/csw_cpu_scope_probe.output.txt`）**完全一致**：
`A=1 ≪ B=15171`（status 不是线程组累计）、`C=205 ≈ D=206`（stat 是线程组累计）。
W03 因此采用：**逐 TID 聚合为唯一全量口径**（或 `wait4` rusage），
`/proc/<pid>/status` 只作 `main_thread_only_*_reference` 参考值输出，绝不作为全进程合计。

---

## 摘录 1：`/proc/<pid>/status` 的上下文切换计数 = 单任务

`proc_pid_status()` 是 `/proc/<pid>/status` 的 read handler；它把**单个** `task` 传给
`task_context_switch_counts()`：

```c
static inline void task_context_switch_counts(struct seq_file *m,
						struct task_struct *p)
{
	seq_put_decimal_ull(m, "voluntary_ctxt_switches:\t", p->nvcsw);
	seq_put_decimal_ull(m, "\nnonvoluntary_ctxt_switches:\t", p->nivcsw);
	seq_putc(m, '\n');
}
```

```c
int proc_pid_status(struct seq_file *m, struct pid_namespace *ns,
			struct pid *pid, struct task_struct *task)
{
	...
	task_sig(m, task);
	task_cap(m, task);
	task_seccomp(m, task);
	task_cpus_allowed(m, task);
	cpuset_task_status_allowed(m, task);
	task_context_switch_counts(m, task);   /* <== 单任务, 无 __for_each_thread 求和 */
	arch_proc_pid_thread_features(m, task);
	return 0;
}
```

**判读**：这里**没有**任何 `for_each_thread` / `thread_group_*` 求和。
对多线程进程，`/proc/<pid>/status` 取的是 pid 对应那一个 `task_struct`（即线程组主线程）
自己的 `nvcsw/nivcsw`。因此把它当"全进程上下文切换次数"必然严重低估 ——
这正是方案 §1 表格中「两秒仅 10 次上下文切换」以及 UF-03 的根因。

## 摘录 2：`do_task_stat` 的 `whole` 分支 —— entry vs thread

```c
static int do_task_stat(struct seq_file *m, struct pid_namespace *ns,
			struct pid *pid, struct task_struct *task, int whole)
{
	...
	if (whole) {
		thread_group_cputime_adjusted(task, &utime, &stime);
	} else {
		task_cputime_adjusted(task, &utime, &stime);
		min_flt = task->min_flt;
		maj_flt = task->maj_flt;
		gtime = task_gtime(task);
	}
	...
	seq_put_decimal_ull(m, " ", nsec_to_clock_t(utime));
	seq_put_decimal_ull(m, " ", nsec_to_clock_t(stime));
	seq_put_decimal_ll(m, " ", nsec_to_clock_t(cutime));
	seq_put_decimal_ll(m, " ", nsec_to_clock_t(cstime));
	...
	seq_put_decimal_ll(m, " ", num_threads);
	seq_put_decimal_ull(m, " ", 0);
	seq_put_decimal_ull(m, " ", start_time);
```

```c
int proc_tid_stat(struct seq_file *m, struct pid_namespace *ns,
			struct pid *pid, struct task_struct *task)
{
	return do_task_stat(m, ns, pid, task, 0);   /* /proc/<pid>/task/<tid>/stat: 单线程 */
}

int proc_tgid_stat(struct seq_file *m, struct pid_namespace *ns,
			struct pid *pid, struct task_struct *task)
{
	return do_task_stat(m, ns, pid, task, 1);   /* /proc/<pid>/stat: whole=1 线程组累计 */
}
```

**判读**：
1. `/proc/<pid>/stat` 的 `utime/stime` 走 `thread_group_cputime_adjusted()` ⇒ **线程组累计**；
   所以 W03 的 CPU 口径可以直接用 `/proc/<pid>/stat`（与 W01 的 C≈D 一致）。
2. `/proc/<pid>/task/<tid>/stat` 走 `task_cputime_adjusted()` ⇒ **单线程**；
   所以逐 TID 求和可以用来反证/替代；W03 在每次运行都输出这个对照
   （`cpu.json` 的 `cpu_scope_check`）。
3. `num_threads` 也在同一 handler 内输出（`get_nr_threads(task)`），与
   `/proc/<pid>/task` 目录项数互为交叉检查。

### 补充说明（防误读，W01 提出）：`do_task_stat` 里的 `__for_each_thread` **不含** utime/stime

同一函数里**确实**存在一个 `__for_each_thread` 循环，但它只累加

```c
		if (whole) {
			struct task_struct *t;

			min_flt = sig->min_flt;
			maj_flt = sig->maj_flt;
			gtime = sig->gtime;

			rcu_read_lock();
			__for_each_thread(sig, t) {
				min_flt += t->min_flt;
				maj_flt += t->maj_flt;
				gtime += task_gtime(t);
			}
			rcu_read_unlock();
		}
```

即 **`min_flt` / `maj_flt` / `gtime`** 三项（缺页与 guest 时间），**不涉及 `utime` / `stime`**。
`utime`/`stime` 的去向完全由紧随其后的

```c
	if (whole) {
		thread_group_cputime_adjusted(task, &utime, &stime);
	} else {
		task_cputime_adjusted(task, &utime, &stime);
		...
	}
```

决定。因此该循环**不是**"`/proc/<pid>/stat` 不是线程组累计"的反例——若不点明这一点，
后来者看到 `__for_each_thread` 容易误判。W01 已在 `W01/未决事实清单.md` 的 UF-06 条目写入同一澄清。

## 摘录 3：字段 22 `starttime` 的时基是 boottime

```c
	/* scale priority and nice values from timeslices to -20..20 */
	/* to make it look like a "normal" Unix priority/nice value  */
	priority = task_prio(task);
	nice = task_nice(task);

	/* apply timens offset for boottime and convert nsec -> ticks */
	start_time =
		nsec_to_clock_t(timens_add_boottime_ns(task->start_boottime));
```

**判读**：`starttime` 由 `task->start_boottime` 转 tick，属 **CLOCK_BOOTTIME** 时基。
`/proc/uptime` 同为 boottime 时基 ⇒ W03 用 `starttime > 窗口起点(=uptime×CLK_TCK)`
判定"线程是否在窗口开始后才创建"在**时基上自洽**（`ProcessSampler::attach()` /
`read_thread_starttime_ticks()`）。本机 `CLK_TCK=100`（tick 粒度 10 ms），
因此同 tick 边界按"新生"处理并在 `tids_starttime_boundary` 登记有界误差。

---

## 未决/边界（不得过度外推）

1. 存证的是**上游 v6.8** 源码，不是本机 Ubuntu 6.8.0-138-generic 的补丁树；
   上述四个函数不属 Ubuntu 常用 patch 面，但严格说"本机二进制 == 上游源码"未逐字节验证，
   该点保留为**未完全核实**。判定不受影响：本机实测（W01 探针 + W03 的
   `cpu_scope_check` / gtest）与源码语义一致。
2. 本存证**只**回答"聚合语义"（what），不回答"某个具体进程的空闲切换有多少"（how many）。
   真实数字仍须按 W03 口径在目标进程上采（见 `W03_测量口径与路径观测.md` §上下文切换）。
3. 本存证不解 UF-07（CycloneDDS 是否真零拷贝）、UF-14（iceoryx 探针字段回显）、
   UF-10（p99 未受控）等其它未决事实。

---

## 交叉核验与关闭状态（2026-09-28 追加，只追加）

- W01（文档负责人）**独立复取同一 URL**（HTTP 200，23234 B）并逐段读源码，三条结论与本存证一致；
  已据本存证把 `W01/未决事实清单.md` 的 **UF-06 标为「✅ 已解除」**（引用本路径与 t4 attempt
  `4a9d66ec-…`），并把 `W01/证据索引.md` 的 **E-42 由 E-X 升为 E-C**。
- 本存证在 W03 文档中对应 **§3.7**，状态为**已关闭（双证据 + 残余限制登记）**（队长裁决 D-5 裁决 1）。
- 残余限制（保留为已知限制）：本存证为**上游 v6.8**，非本机 `6.8.0-138-generic` 补丁树，未逐字节核对。
- W03 第三份实测：`artifacts/perf/20260928-r03-W03-ctxscope/`（把 W01 探针作为被观测进程，
  主线程参考 7 / 逐 TID 聚合 22107 / rusage 22800）。
