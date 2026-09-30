/* t32 独立复算（决定性）：唤醒率的**发生器**是什么 —— 每次 tick 派发几项、每周期几组。
 * 计数：tick_count（= 唤醒率分子）、callback_count（实际派发项数）。
 * ⇒ items_per_tick = callbacks/ticks 直接给出「同批合并」的批量因子。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include "dzIPC/threepools/shm_control_scheduler.h"
struct Idle : dzIPC::shm_control::SubControlState {
  std::atomic<std::uint64_t> n{0};
  void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override { n.fetch_add(1, std::memory_order_relaxed); }
  const char* debug_name() const noexcept override { return "idle"; } };
int main(int argc, char** argv)
{
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double spread_ms = argc > 2 ? std::atof(argv[2]) : 0.0;
    const double win = argc > 3 ? std::atof(argv[3]) : 15.0;
    auto& sch = dzIPC::shm_control::ShmControlScheduler::instance();
    dzIPC::shm_control::ControlTiming tm; tm.sub_heartbeat = std::chrono::milliseconds{10};
    std::vector<std::shared_ptr<Idle>> st;
    const auto r0 = dzIPC::shm_control::ControlClock::now();
    for (int i = 0; i < n; ++i) {
        auto s = std::make_shared<Idle>(); st.push_back(s);
        (void)sch.register_subscriber(s, tm);
        if (spread_ms > 0.0)
            std::this_thread::sleep_until(r0 + std::chrono::microseconds((long long)(spread_ms*1000.0*(i+1)/n)));
    }
    const double reg_ms = std::chrono::duration<double,std::milli>(dzIPC::shm_control::ControlClock::now()-r0).count();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto a = sch.stats();
    std::uint64_t c0 = 0; for (auto& s : st) c0 += s->n.load();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = sch.stats();
    std::uint64_t c1 = 0; for (auto& s : st) c1 += s->n.load();
    const double ticks = (double)(b.tick_count - a.tick_count) / win;
    const double cbs   = (double)(c1 - c0) / win;
    std::printf("n=%-5d 散布=%-6.1fms 注册耗时=%-7.2fms | tick=%9.1f/s callbacks=%10.1f/s 项/批=%7.2f | 每周期批数=%6.2f (period=10ms) | 公式Σ=%9.1f/s 比值=%.4f\n",
        n, spread_ms, reg_ms, ticks, cbs, (ticks>0?cbs/ticks:0.0), ticks/100.0, n*100.0, ticks/(n*100.0));
    std::fflush(stdout);
    std::_Exit(0);
}
