# 勘误：本目录 `idle.json` 的 `idle_fallback.duration_s` 为无符号下溢值

> 本文件是**只追加**的勘误说明，不改动本目录内任何既有文件（依据《团队改造方案》§12
> 「结果目录采用只追加策略；重跑生成新目录，不能覆盖旧日志或只替换汇总表」）。
> 修正版结果见 **`artifacts/perf/20260928-r04-W03-idlewindow/`**（run_id `20260928-r04-W03`）。
> 发现与修复记录：队长裁决 D-5；W03 文档 §11.1。

## 1. 缺陷

本目录 `idle.json` 中：

```json
"idle_fallback": { "duration_s": 18446744073.459354, ... }
```

`18446744073.459354 = (2^64 - 1) / 1e9` —— 无符号下溢（`a - b` 且 `a < b`）的典型值。
同一文件里 `startup_peak`（4.510605 s）与 `steady_active`（14.775025 s）恰好正确，掩盖了该缺陷。

**其它字段不受影响**：相位线程数/CPU/ctx 均值、`thread_fallback_ratio`、`idle_fallback_observed`、
`periodic_task_coverage`、`window_sufficient`、`total_window_s`、`cpu.json` 的 `cpu_integrity` /
`cpu_scope_check`、`counters.json` 的合并结果均与修正版一致（对比 r04 可逐项核对）。

## 2. 根因

1. 采集器 `tools/measure/w03_collector.cpp` 按"每采样时刻"分组时使用了**字符串键**
   `std::to_string(ns) + "|" + phase`；`std::map<std::string, ...>` 的迭代序是**字典序**。
   本机 uptime ≈ 9.9e3 s ⇒ ns 值在同一轮运行中从 **13 位跨到 14 位**，字典序与数值序不一致。
   本目录实测：`idle_fallback` 各键中字典序**首键** `10000125064509` 在时间上**晚于**
   字典序**末键** `9999874868855`，于是喂给观测器的采样顺序首末颠倒。
2. `PhaseObserver::compute()` 用 `v.back().monotonic_ns - v.front().monotonic_ns` 求相位时长，
   依赖插入顺序；首元素晚于末元素时即下溢。

## 3. 修复（已入库）

| # | 文件 | 修改 |
|---|---|---|
| 1 | `tools/measure/w03_collector.cpp` | 分组键改为 `std::map<std::pair<std::uint64_t, std::string>, ...>`（先按 ns **数值**比较） |
| 2 | `include/dzIPC/measure/idle_observer.h` | 相位时长改为顺序无关的 `max_ns - min_ns`（库不再依赖调用方插入顺序，结构上不可能再下溢） |
| 3 | `test/test_w03_measurement.cpp` | 新增回归 `PhaseDurationIsOrderIndependentAndCannotUnderflow`（乱序插入 + 跨 13/14 位边界，断言 `duration_s == max-min` 且 `< 1e6`） |

## 4. 修正后的正确值（来自 r04，同一负载与参数）

| 相位 | 样本 | duration_s（r04） | 样本数 × 250 ms 自洽性 |
|---|---|---|---|
| startup_peak | 19 | 4.512 | 19 × 0.25 = 4.75（含边界）✓ |
| steady_active | 41 | 14.778 | 41 × 0.25 = 10.25（含相位切换抖动） |
| idle_fallback | 220 | **54.790** | 220 × 0.25 = 55.0 ✓ |

三相时长之和 74.08 s 与实际总窗口 69.82 s 的差值来自相位边界采样的重叠与过渡（负载按标记切换
相位，边界处同一时刻只归一个相位），不改变"窗口 ≥ 60 s"的判定。

## 5. 复现修正版

```bash
cd /home/zwc/cpp_ipc_dds
tools/measure/build.sh
./build/w03/w03_collector evidence \
  --out-dir=artifacts/perf/20260928-r04-W03-idlewindow --run-id=20260928-r04-W03 \
  --window-s=90 --required-window-s=60 --sample-ms=250 \
  --periodic-ticks=3500 --periodic-source="w03_thread_workload:periodic tick 50Hz" \
  --expect-threads=18 --transport=calibration-known-threads \
  --build-dir=$PWD/build --diagnostics=on \
  "--job=w03_thread_workload,other,$PWD/build/w03/w03_thread_workload,--threads-peak=16,--threads-steady=6,--periodic-hz=50,--startup-s=5,--steady-s=10,--idle-s=55,--sim-routes=100,--export-counters=$PWD/artifacts/perf/20260928-r04-W03-idlewindow/counters_from_workload.json" \
  --counters-json=$PWD/artifacts/perf/20260928-r04-W03-idlewindow/counters_from_workload.json,w03_thread_workload
```

**引用约定**：需要空闲相位时长时请引用 **r04**；引用 r01 时只能引用其上表列出的未受影响的字段，
不得引用本目录的 `idle_fallback.duration_s`。
