# perf 调度、空闲态与复制对照

本轮完成 19 个诊断窗口及离线审计，进一步定位等待与复制成本；**没有新的生产延迟修复，也没有新的 54 窗口验收，历史三个失败仍开放**。用户已要求等待另一位 Agent 完成后再采样，本批结束后只做离线解析、审计和归档；不能以短暂无编译进程代替完成确认。

## 采样条件与完整性

沿用 `fix/writer-boundaries-latency` 的身份修复库，生产代码未再修改。benchmark 仅在诊断开启时在线程启动处取得一次 `reader_tid`，用于关联同一接收线程的调度事件。四批 manifest 分别冻结源码提交、工具及二进制 SHA256、实际命令、真实加载库、CPU 允许集合和前后系统状态：

|批次|源码提交|窗口|发布 / 接收|用途|
|---|---|---:|---:|---|
|[smoke](smoke/manifest.json)|9cd1c7cf|1|300 / 300|3 秒冒烟，时钟及过滤器验证|
|[paired](paired/manifest.json)|88139a51|8|8000 / 132000|1 SUB/4KiB 与 32 SUB/64B，perf OFF/ON 逆序配对|
|[paired32-recheck](paired32-recheck/manifest.json)|87b5198c|4|4000 / 128000|32 SUB/64B 复核|
|[copy-affinity](copy-affinity/manifest.json)|5999e699|6|6000 / 6000|1 SUB/1MiB，P/P、P/E、E/P 两轮逆序|

共 18300 次发布、266300 次接收，零观察丢失、重复、损坏。13 个 perf ON 窗口的 136300 条接收样本全部匹配完整链，唤醒发起者均匹配发布主线程；其余 130000 条明确标记 `perf_disabled`。未发现 perf LOST 记录或未知解码行。smoke 的早期解码未加 `--show-lost-events`，已对原 perf.data **离线**补查，输出与原解码逐字节一致，见 [补查记录](smoke-lost-check.json)。以上完整性包含受干扰窗口，不能据此认定其性能有效。

仅 perf 提权，业务通过 `setpriv` 降回 UID/GID 1000。所有命令保留降权参数；copy 六窗还逐秒记录了业务实际 UID/GID，均为 1000，前面批次不具备这一附加观测。未保存 sudo 凭据，未修改 sysctl、电源、空闲态或正式验收条件。

除 3 秒冒烟外，每窗 100Hz、预热 2 秒、正式 10 秒；shared_v1、同一生产库，接收和发布 CPU 诊断始终开启。perf OFF 仅关闭 perf，仍有应用诊断开销。小消息两批允许 CPU0～31，不绑核；复制实验固定 CPU 只作诊断。

## 受干扰窗口

按正式消息首尾的 CLOCK_MONOTONIC 区间核对逐秒进程记录，以下三窗存在另一工作区 `build-shared-net` 编译重叠：

|窗口|处置|
|---|---|
|paired/r1-sub32-perf1|保留原始数据；该轮 OFF/ON 配对不作性能结论|
|paired/r2-sub32-perf0|保留原始数据；该轮 OFF/ON 配对不作性能结论|
|paired32-recheck/r2-sub32-perf0|保留原始数据；该轮 OFF/ON 配对不作性能结论|

其余 16 窗未观察到已知编译/压测进程重叠。copy 六窗每窗前额外检查连续空闲 20 秒。检查按秒且只匹配已知进程名，可能漏过短任务及其他负载，不能称为系统完全独占；也不能用它判断另一位 Agent 已完成。详细进程与时间见 [audit.json](audit.json)。

## 调度阶段定位

同消息序号和 reader TID，统一 CLOCK_MONOTONIC，逐条关联：

```text
notify_begin → sched_waking → sched_wakeup → sched_switch(next=reader) → wait_end
```

`sched_waking→sched_wakeup` 包含内核唤醒处理、可能的跨核投递和目标 CPU 激活；`sched_wakeup→sched_switch` 才是进入可运行状态后到首次获得 CPU 的区间。`scheduled→wait_end` 包含等待返回前的执行，也可能包含再次抢占。对缺失事件保留缺失原因，不补零。

下表每列均为**同窗按端到端最慢 1% 选出的同一组样本的算术均值**，单位 µs，不能理解成各阶段 p99。1 SUB 每窗取 10 条；32 SUB 每窗取 320 条，分别来自 236/205 个发布序号，并非 320 个独立发布实验。

|阶段|1 SUB/4KiB 第1轮|1 SUB/4KiB 第2轮|32 SUB/64B 复核第1轮|32 SUB/64B 复核第2轮|
|---|---:|---:|---:|---:|
|端到端|113.226|129.711|210.884|240.682|
|notify→waking|2.904|0.781|26.823|26.983|
|waking→wakeup|64.954|80.027|95.049|143.774|
|wakeup→scheduled|3.188|6.988|16.405|19.039|
|scheduled→wait_end|9.948|12.304|16.613|11.876|
|notify→wait_end，以上四段之和|80.994|100.100|154.890|201.672|

这四个无已知重叠窗口都指向**成为可运行状态之前的唤醒激活区间主导长尾**。32 SUB 的 notify→waking 也明显增加，符合一次广播依次唤醒多个等待者的待查方向；本轮没有内核调用栈，不能指定某一把锁或通知实现为原因。原 paired 第2轮的 32 SUB perf ON 窗也未观察到重叠，其最慢 1% 的 waking→wakeup 为 140.413µs，wakeup→scheduled 为 14.000µs；因对应 OFF 受干扰，仍不作配对收益结论。

这主要是**长尾**证据，不能直接解释原 1 SUB/4KiB 的 **p50** 失败。4KiB 两轮全部样本的 notify→wait_end 均值仅为 11.589/9.971µs，而端到端均值为 31.543/23.472µs；通知之前和等待返回之后的已有分段仍须一并核对。

### 空闲态关联的边界

本轮四批、所有 CPU 的实际快照均是 state0=POLL、state1=C1_ACPI、state2=C2_ACPI、state3=C3_ACPI，公布延迟分别为 0/1/127/1048µs，均启用。审计核对了批次和每窗前后名称、公布延迟、禁用标志、调频驱动/governor/最大频率未变化；**实际运行频率并未固定**。

空闲态使用 `sched_wakeup.target_cpu` 对应 CPU 在 waking 时的状态，避免把迁移前位置当作最终唤醒 CPU。在上述最慢 1% 中，有匹配 idle exit 的子集为 10/10、10/10、309/320、320/320 条；waking→idle_exit 均值分别为 55.294、61.620、82.607、122.968µs。分母不同时不能直接与整组均值相减或算贡献率。

全部样本按 waking 时空闲态分组，state3 的 waking→wakeup 均值也高于 state1：4KiB 两轮为 8.711 对 2.407、15.685 对 1.621µs；32 SUB 复核为 13.882 对 2.738、21.614 对 2.457µs。这支持继续调查空闲退出与跨核唤醒，但不是将整个激活区间归因于 C-state 的因果证明。公布的 1048µs 不能替代逐样本实测。

采样 manifest 没有记录当时的 `cpuidle/current_driver`；[离线整理时的只读快照](post-sampling-cpu.json)显示 `intel_idle`，同时核型 PMU 集合为 P 核 CPU0～15、E 核 CPU16～31。空闲态名称带 ACPI 不等于驱动为 `acpi_idle`，也不能据驱动名称将 state3 改称 C6。后续采样应同时冻结驱动名称和状态映射。

## perf 观测影响

以下只列完整且未观察到编译重叠的配对，均为端到端 µs。第1轮顺序 OFF→ON，第2轮 ON→OFF。

|场景 / 轮次|OFF 均值 / p50 / p99|ON 均值 / p50 / p99|
|---|---:|---:|
|1 SUB/4KiB 第1轮|23.198 / 21.913 / 56.252|31.543 / 22.580 / 83.856|
|1 SUB/4KiB 第2轮|30.080 / 23.297 / 126.604|23.472 / 13.601 / 116.276|
|32 SUB/64B 复核第1轮|43.967 / 29.635 / 183.883|55.976 / 50.290 / 170.746|

影响方向不稳定，且两窗间核位置、频率和空闲历史可能变化，不能减去固定 perf 开销后当作真实延迟。32 SUB 复核第2轮 ON 为 48.177 / 35.803 / 218.347µs，OFF 受干扰，不补入完整配对。

## 1MiB 复制与核位置

PP=PUB0/SUB4，PE=PUB0/SUB16，EP=PUB16/SUB4，网关始终 CPU2。第一轮 PP→PE→EP，第二轮 EP→PE→PP，均开启 perf。六窗未观察到编译/压测重叠，发布复制段起止 CPU 均符合指定位置；起止相同本身不能排除中间迁移，但这里发布线程允许集合仅有指定单核。

|窗口|复制墙钟均值 µs|复制 CPU 均值 µs|端到端均值 / p50 / p99 µs|
|---|---:|---:|---:|
|第1轮 PP|49.046|49.379|76.018 / 51.480 / 170.546|
|第1轮 PE|42.722|43.048|71.261 / 51.970 / 195.685|
|第1轮 EP|71.734|72.296|99.497 / 79.925 / 173.637|
|第2轮 EP|83.248|84.274|119.485 / 119.901 / 182.341|
|第2轮 PE|46.404|46.713|77.313 / 52.772 / 205.058|
|第2轮 PP|48.259|48.585|69.175 / 50.515 / 145.646|

PUB 在 CPU16 时复制真实 CPU 成本更高，两轮均成立；不能将差异全部解释成线程被抢占。CPU 与墙钟计时区间存在钩子及读钟边界差异，因此 CPU 均值略高不是负等待时间。核型、频率、缓存状态尚未单独控制，不能泛化成固定 P/E 性能比，也不能用绑核结果替换原验收。

大消息仍有其他尾部来源：例如第1轮 PP 最慢 1% 的端到端均值 544.067µs，其中 wakeup→scheduled 均值 443.897µs；第1轮 EP 为 434.691µs / 269.059µs。**小消息的唤醒激活结论不能套用于所有场景**。本批过滤器没有记录所有竞争线程的完整运行历史，无法继续确定是谁占用了这些核。

## 下一轮执行顺序

1. 等待另一位 Agent 明确完成，冻结本分支及其环境状态，再启动新增采样。确认完成后仍保留进程活动审计；受到干扰的配对整体标记，不挑选有利窗口。
2. 先补齐 32 SUB 的第二组完整 OFF/ON 逆序对照；记录采样时 cpuidle 驱动、状态映射和核型。不修改原 CPU0～31、电源及门槛。
3. 将 4KiB 全体样本及 p50 附近样本与现有发布/等待/交接分段关联，区分中位数问题和长尾问题。若继续定位 waking→wakeup，在单独诊断窗口按可用内核符号采集 futex 唤醒、跨核唤醒投递及处理栈，并核对其额外开销；不能从当前 tracepoint 直接推断具体锁。
4. 32 SUB 比较每个接收者在同一广播中的唤醒次序、核迁移与激活延迟，再评估事件驱动的通知/交接候选。保持广播、取消、重建和 I5，不通过忙轮询或改变电源策略制造通过。
5. 1MiB 保留标准 memcpy，对发布侧已有分段继续核对实际执行核、复制次数、地址/容量及池复用；有可重复热点后才引入代码候选。固定核只用于诊断。
6. 候选通过生命周期、完整性与相关回归后，才执行原 f066a82 基线、30 秒、3轮×9格×两模式的无诊断 54 窗口，按逐轮 p50/p99 原门槛决定三个失败是否关闭。

## 复核入口

- [完整审计](audit.json)：每窗计数、调度匹配、进程重叠、实际 UID 观测及系统设置核对范围。
- [paired 汇总](paired/scheduler-summary.json)、[32 SUB 复核汇总](paired32-recheck/scheduler-summary.json)、[复制汇总](copy-affinity/scheduler-summary.json)。各窗口含 `joined.csv.gz`、原始 pub/sub CSV、events.txt.gz、perf.data.gz、result.json 和日志，全部保留。
- [日志归档映射](archive-manifest.json)：文本日志及 perf header 仅 gzip 无损压缩，原采样 manifest 和原始字节 SHA256 不变；审计会解压验证。原 perf.data/事件/CSV 未重写。
- [解析器单测](parser-tests.log.gz)：4/4，通过缺失事件、迁移目标 CPU、LOST、perf OFF 和重复消息的边界验证；[工装构建日志](benchmark-build.log.gz)。

在本独立工作区、保留采样二进制的条件下，以下均为普通用户离线操作，不会启动 benchmark 或 perf record：

```bash
python3 -m unittest discover -s test/shared_net -p 'test_summarize_perf_scheduler.py' -v
python3 test/shared_net/summarize_perf_scheduler.py docs/shared_network_endpoint_evidence/20261006-writer-boundaries/perf/paired
python3 test/shared_net/audit_perf_scheduler.py docs/shared_network_endpoint_evidence/20261006-writer-boundaries/perf
```

离线解析会重生成派生汇总和 joined CSV；采样 manifest 记录的原始输入不会改变。审计还依赖原提交和本机保留的二进制以核验指纹，不把缺失二进制默认为验证通过。本分支未合并，另一位 Agent 的主工作区和索引未改动。
