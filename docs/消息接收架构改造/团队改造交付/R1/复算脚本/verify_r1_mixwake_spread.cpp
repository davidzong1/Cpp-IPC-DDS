/* t32 独立复算（决定性 2）：产品同形（N sub@10ms + N pub@50ms）下，
 * 唤醒率 wakes/s 与**注册散布**的关系。用于判定「能否把唤醒率冻结为常数」。
 * 用法: mixspread <n_pairs> <reg_total_ms> <window_s>   （reg_total_ms=0 ⇒ 尽快注册）
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include "dzIPC/threepools/shm_control_scheduler.h"
struct IdleSub : dzIPC::shm_control::SubControlState {
  std::atomic<std::uint64_t> n{0};
  void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override { n.fetch_add(1, std::memory_order_relaxed); }
  const char* debug_name() const noexcept override { return "s"; } };
struct IdlePub : dzIPC::shm_control::PubControlState {
  std::atomic<std::uint64_t> n{0};
  void on_pub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override { n.fetch_add(1, std::memory_order_relaxed); }
  bool has_peers() const override { return false; }
  void on_pub_stale_scan(dzIPC::shm_control::ControlClock::time_point, std::chrono::nanoseconds) override {}
  const char* debug_name() const noexcept override { return "p"; } };
int main(int argc, char** argv)
{
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double reg_ms = argc > 2 ? std::atof(argv[2]) : 0.0;
    const double win = argc > 3 ? std::atof(argv[3]) : 20.0;
    auto& sch = dzIPC::shm_control::ShmControlScheduler::instance();
    std::vector<std::shared_ptr<IdleSub>> subs; std::vector<std::shared_ptr<IdlePub>> pubs;
    const auto r0 = dzIPC::shm_control::ControlClock::now();
    for (int i = 0; i < n; ++i)
    {
        auto s = std::make_shared<IdleSub>(); subs.push_back(s); (void)sch.register_subscriber(s);
        auto p = std::make_shared<IdlePub>(); pubs.push_back(p); (void)sch.register_publisher(p);
        if (reg_ms > 0.0)
            std::this_thread::sleep_until(r0 + std::chrono::microseconds((long long)(reg_ms*1000.0*(i+1)/n)));
    }
    const double actual_reg_ms = std::chrono::duration<double,std::milli>(dzIPC::shm_control::ControlClock::now()-r0).count();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto a = sch.stats();
    std::uint64_t c0=0; for (auto&s:subs) c0+=s->n.load(); for (auto&p:pubs) c0+=p->n.load();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = sch.stats();
    std::uint64_t c1=0; for (auto&s:subs) c1+=s->n.load(); for (auto&p:pubs) c1+=p->n.load();
    const double wakes=(double)(b.tick_count-a.tick_count)/win, cbs=(double)(c1-c0)/win;
    std::printf("pairs=%-5d 目标注册=%-7.1fms 实测注册=%-8.2fms | wakes=%9.1f/s callbacks=%10.1f/s 项/批=%6.2f | 公式Σ=%.0f/s 高估=%.1f×\n",
        n, reg_ms, actual_reg_ms, wakes, cbs, (wakes>0?cbs/wakes:0.0), n*100.0+n*20.0, (n*120.0)/wakes);
    std::fflush(stdout);
    std::_Exit(0);
}
