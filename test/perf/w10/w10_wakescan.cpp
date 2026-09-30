/* W10 标定：控制面 `ShmControlScheduler` 的**空闲唤醒率 vs 在册项数**。
 *
 * 用途：判定门槛 3 修订式 R-1（架构负责人 t23/D-15 采纳）里的
 *   `Σ_在册控制项 (1000 / heartbeat_ms)`
 * 这一项的量纲正确性 —— 它假设"每项每周期各唤醒一次"。
 *
 * 实测结论（见 artifacts/perf/20260929-r25-W10/gate3/gate3_idle.txt §C）：
 *   entries=1    → 100.1/s（= 1 × 1000/10ms，比值 1.0）
 *   entries=10   → 100.0/s（比值 0.1）
 *   entries=100  → 100.2/s（比值 0.0）
 *   entries=1000 → 1906.1/s（比值 0.0）
 * ⇒ 调度器把**同一批到期项合并到一次 tick**，因此唤醒率**不是**项数线性项；
 *   R-1 公式在项数 >1 时**严重高估**（1000 项高估 ~52×）。这是 R-1 需要更正的依据。
 *
 * 用法：w10_wakescan <entries> <window_s> <heartbeat_ms>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "dzIPC/threepools/shm_control_scheduler.h"

struct Idle : dzIPC::shm_control::SubControlState
{
    void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override {}
    const char* debug_name() const noexcept override { return "idle"; }
};

int main(int argc, char** argv)
{
    const int n = argc > 1 ? std::atoi(argv[1]) : 100;
    const double win = argc > 2 ? std::atof(argv[2]) : 10.0;
    const long hb = argc > 3 ? std::atol(argv[3]) : 10;

    auto& sch = dzIPC::shm_control::ShmControlScheduler::instance();
    dzIPC::shm_control::ControlTiming tm;
    tm.sub_heartbeat = std::chrono::milliseconds{hb};
    std::vector<std::shared_ptr<Idle>> st;
    std::vector<dzIPC::shm_control::ShmControlScheduler::EntryId> ids;
    for (int i = 0; i < n; ++i)
    {
        auto s = std::make_shared<Idle>();
        st.push_back(s);
        ids.push_back(sch.register_subscriber(s, tm));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto a = sch.stats();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = sch.stats();
    const double wakes = static_cast<double>(b.tick_count - a.tick_count) / win;
    std::printf("entries=%-5d heartbeat=%ldms window=%.1fs tick_wakes=%.1f/s  "
                "公式项数×(1000/hb)=%.1f/s  比值=%.2f  tick_max_us=%.1f overruns=%llu\n",
                n, hb, win, wakes, n * 1000.0 / hb, wakes / (n * 1000.0 / hb),
                static_cast<double>(b.tick_duration_max_ns) / 1000.0,
                static_cast<unsigned long long>(b.tick_overrun_count));
    std::fflush(stdout);
    std::_Exit(0);
}
