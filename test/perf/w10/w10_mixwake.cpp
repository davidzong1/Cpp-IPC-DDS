/* W10 标定：与产品**同形**的混合周期唤醒率（N 个订阅项 + N 个发布项）。
 *
 * 用途：判定门槛 3 修订式 R-1（架构负责人 t23/D-15）里
 *   `Σ_在册控制项 (1000 / heartbeat_ms)`
 * 这一项的**量纲正确性与量级**。产品的实际注册是 1 订阅项/话题（10 ms）+ 1 发布项/话题（50 ms）。
 *
 * 实测（artifacts/perf/20260929-r25-W10/gate3/gate3_idle.txt §C）：
 *   sub=1000 + pub=1000（entries=2000）⇒ 实测 **3352.6–4603.0 /s**，而 R-1 公式给出
 *   `1000×100 + 1000×20 = 120000 /s` ⇒ **高估 13–36×**。
 * 原因：调度器把**同一批到期项合并到一次 tick**，唤醒率由"到期分组的分散度"决定，
 * 而不是项数的线性求和；Σ 只是理论上界。
 *
 * 用法：w10_mixwake <n_pairs> <window_s>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "dzIPC/threepools/shm_control_scheduler.h"

struct IdleSub : dzIPC::shm_control::SubControlState
{
    void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override {}
    const char* debug_name() const noexcept override { return "idle-sub"; }
};

struct IdlePub : dzIPC::shm_control::PubControlState
{
    void on_pub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override {}
    bool has_peers() const override { return false; }
    void on_pub_stale_scan(dzIPC::shm_control::ControlClock::time_point, std::chrono::nanoseconds) override {}
    const char* debug_name() const noexcept override { return "idle-pub"; }
};

int main(int argc, char** argv)
{
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double win = argc > 2 ? std::atof(argv[2]) : 20.0;
    auto& sch = dzIPC::shm_control::ShmControlScheduler::instance();
    std::vector<std::shared_ptr<IdleSub>> subs;
    std::vector<std::shared_ptr<IdlePub>> pubs;
    for (int i = 0; i < n; ++i)
    {
        auto s = std::make_shared<IdleSub>();
        subs.push_back(s);
        sch.register_subscriber(s);   /* 默认 10 ms */
        auto p = std::make_shared<IdlePub>();
        pubs.push_back(p);
        sch.register_publisher(p);    /* 默认 50 ms */
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto a = sch.stats();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = sch.stats();
    const double wakes = static_cast<double>(b.tick_count - a.tick_count) / win;
    const double formula = n * 1000.0 / 10 + n * 1000.0 / 50;
    std::printf("sub=%d(10ms)+pub=%d(50ms) entries=%zu wakes=%.1f/s  公式Σ=%.1f/s  比值=%.3f  tick_max_us=%.1f overruns=%llu\n",
                n, n, sch.entry_count(), wakes, formula, wakes / formula,
                static_cast<double>(b.tick_duration_max_ns) / 1000.0,
                static_cast<unsigned long long>(b.tick_overrun_count));
    std::fflush(stdout);
    std::_Exit(0);
}
