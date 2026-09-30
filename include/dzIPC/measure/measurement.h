#pragma once
/* W03 测量口径与路径观测 · 总入口
 * ============================================================================
 * 头文件-only 的测量层，不修改任何既有实现文件、不引入平台宏到既有公共头。
 * 使用方式（模块热路径只依赖 counters.h 的宏）：
 *
 *   #include "dzIPC/measure/measurement.h"
 *   using namespace dzIPC::measure;
 *
 *   // 数据路径计数（每成功交付一条消息）
 *   if (path_is_dzflat_b) DZIPC_MEASURE_INC(CounterId::dzflat_b_messages);
 *   // 回退
 *   DZIPC_MEASURE_INC(CounterId::fallback_total);
 *   DZIPC_MEASURE_INC(CounterId::fallback_backend_unavailable);
 *   // 诊断（仅在 diagnostics_enabled() 时累加；采集器会写明开关状态）
 *   { ScanRoundScope scan(route_count, deferred_size);  ... ; scan.set_ready(ready); }
 *
 *   // 进程退出前导出
 *   CounterRegistry::instance().write_json_file("counters.json");
 *
 * 采集器（tools/measure/w03_collector.cpp）在**外部**观测被测进程：
 *   · rusage 模式：fork+wait4，得到覆盖整个线程组生命周期的 CPU 与上下文切换；
 *   · /proc 采样模式：逐 TID 增量，处理新生/退出线程并标注完整性；
 *   · 相位统计：startup_peak / steady_active / idle_fallback，窗口 >= 60 s；
 *   · 路径证据：证据不足的路径在 verdict 中写「未确认」。
 */
#include "dzIPC/measure/monotonic_clock.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/proc_sampler.h"
#include "dzIPC/measure/platform_info.h"
#include "dzIPC/measure/idle_observer.h"
#include "dzIPC/measure/path_evidence.h"
#include "dzIPC/measure/field_schema.h"
