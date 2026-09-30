// W03 校准负载 —— "已知线程活动"的可控产生器。
//
// 用途：验证 W03 采集器能把**已知**的线程活动正确计入（验收项「已知线程活动可被
// 正确计入」），并按 §4 W03 第 6 条给出 启动峰值 / 稳定活跃 / 空闲回落 三个相位：
//   · 启动峰值: 线程数拉到 peak（注册/建连阶段的形态）
//   · 稳定活跃: 回落到 steady 个周期工作线程
//   · 空闲回落: 工作线程全部退出，只剩 1 条周期任务线程（心跳/tick 形态）
//
// 本负载**不是**数据面基准，也不产生任何传输路径证据：它只用于校准测量层。
// 相位标记以 CLOCK_MONOTONIC 纳秒写出，采集器按标记切相位：
//   W03_PHASE <name> <monotonic_ns>
// 期望线程数（ground truth）：
//   W03_KNOWN <name> threads=<n>   (n 含主线程与周期任务线程)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/monotonic_clock.h"

using dzIPC::measure::monotonic_now_ns;

namespace {

long arg_long(int argc, char** argv, const char* key, long def)
{
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], key, klen) && argv[i][klen] == '=' && argv[i][klen + 1])
            return std::strtol(argv[i] + klen + 1, nullptr, 10);
        if (!std::strcmp(argv[i], key) && i + 1 < argc) return std::strtol(argv[i + 1], nullptr, 10);
    }
    return def;
}

double arg_double(int argc, char** argv, const char* key, double def)
{
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], key, klen) && argv[i][klen] == '=' && argv[i][klen + 1])
            return std::atof(argv[i] + klen + 1);
        if (!std::strcmp(argv[i], key) && i + 1 < argc) return std::atof(argv[i + 1]);
    }
    return def;
}

void marker(const char* name)
{
    std::printf("W03_PHASE %s %llu\n", name,
                static_cast<unsigned long long>(monotonic_now_ns()));
    std::fflush(stdout);
}

void known(const char* name, long threads)
{
    std::printf("W03_KNOWN %s threads=%ld\n", name, threads);
    std::fflush(stdout);
}

}   // namespace

int main(int argc, char** argv)
{
    const long peak = arg_long(argc, argv, "--threads-peak", 16);
    const long steady = arg_long(argc, argv, "--threads-steady", 6);
    const long periodic_hz = arg_long(argc, argv, "--periodic-hz", 50);
    const double startup_s = arg_double(argc, argv, "--startup-s", 4.0);
    const double steady_s = arg_double(argc, argv, "--steady-s", 6.0);
    const double idle_s = arg_double(argc, argv, "--idle-s", 55.0);
    /* 计数导出闭环: 本负载用**真实的** W03 计数注册表按周期任务节拍灌入诊断计数
     * (scan_rounds / scanned_routes_total / scan_time_ns_total / ready_observed /
     * deferred_depth_*), 退出时导出 counters.json, 供采集器 --counters-json 合并。
     * ⚠️ 这里**不**伪造任何数据路径计数(TLV/DZFlat 等保持 0): 本负载不是数据面实验。 */
    std::string export_counters;
    long sim_routes = 100;
    const std::size_t exlen = std::strlen("--export-counters");
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--export-counters=", exlen + 1))
            export_counters = argv[i] + exlen + 1;
        else if (!std::strcmp(argv[i], "--export-counters") && i + 1 < argc)
            export_counters = argv[i + 1];
        else if (!std::strncmp(argv[i], "--sim-routes=", 13))
            sim_routes = std::strtol(argv[i] + 13, nullptr, 10);
    }
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(true);
    const long total_workers = peak > steady ? peak : steady;

    std::printf("W03_WORKLOAD pid=%d peak=%ld steady=%ld periodic_hz=%ld\n",
                static_cast<int>(::getpid()), peak, steady, periodic_hz);
    std::printf("W03_WORKLOAD startup_s=%.3f steady_s=%.3f idle_s=%.3f\n",
                startup_s, steady_s, idle_s);
    std::fflush(stdout);

    std::atomic<bool> stop{false};
    std::atomic<long> limit{peak};
    std::atomic<unsigned long long> ticks{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(total_workers));
    for (long i = 0; i < total_workers; ++i) {
        workers.emplace_back([&, i]() {
            while (!stop.load()) {
                if (i >= limit.load()) break;
                ticks.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    /* 周期任务线程：整个生命周期都在跑（对应控制面 tick / heartbeat 形态）。 */
    std::thread periodic([&]() {
        const auto period = std::chrono::microseconds(periodic_hz > 0 ? 1000000 / periodic_hz : 20000);
        while (!stop.load()) {
            ticks.fetch_add(1);
            {
                dzIPC::measure::ScanRoundScope scan(static_cast<std::size_t>(sim_routes > 0 ? sim_routes : 1),
                                                    1u);
                scan.set_ready(true);
            }
            std::this_thread::sleep_for(period);
        }
    });

    /* 相位 1：启动峰值 */
    marker("startup_peak");
    known("startup_peak", peak + 2);   // 主线程 + peak 工作线程 + 1 周期任务线程
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(startup_s * 1000)));

    /* 相位 2：稳定活跃 */
    limit.store(steady);
    marker("steady_active");
    known("steady_active", steady + 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(steady_s * 1000)));

    /* 相位 3：空闲回落（工作线程全部退出，只留周期任务线程） */
    limit.store(0);
    marker("idle_fallback");
    known("idle_fallback", 2);         // 主线程 + 1 周期任务线程
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(idle_s * 1000)));

    stop.store(true);
    limit.store(0);
    for (auto& t : workers) t.join();
    periodic.join();
    std::printf("W03_WORKLOAD ticks=%llu\n", static_cast<unsigned long long>(ticks.load()));
    if (!export_counters.empty()) {
        const bool ok = dzIPC::measure::CounterRegistry::instance().write_json_file(export_counters);
        std::printf("W03_WORKLOAD counters_export=%s ok=%d scan_rounds=%llu\n",
                    export_counters.c_str(), (int)ok,
                    static_cast<unsigned long long>(
                        dzIPC::measure::CounterRegistry::instance().get(
                            dzIPC::measure::CounterId::scan_rounds)));
    }
    marker("shutdown");
    std::printf("W03_WORKLOAD_DONE\n");
    return 0;
}
