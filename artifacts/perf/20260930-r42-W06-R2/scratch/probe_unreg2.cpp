/* 复现 case 3 的确切序列：先断发布者，再注销订阅；全程每 100 ms 采样一次计数。 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"
using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;
static std::shared_ptr<dzIPC::TopicData> td(){ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }
static void sample(const char* tag, Clock::time_point t0)
{
    const auto s = dzIPC::threepools::RecvWorkerPool::instance().stats();
    printf("  [%s] t=%.0fms wakeups=%llu timeouts=%llu routes=%zu recv_once=%llu\n", tag,
           std::chrono::duration<double,std::milli>(Clock::now()-t0).count(),
           (unsigned long long)s.wait_wakeups, (unsigned long long)s.wait_timeouts, s.route_count,
           (unsigned long long)s.recv_once_calls);
}
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long dom = argc > 1 ? strtol(argv[1], nullptr, 10) : 8995;
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    dzIPC::threepools::RecvBudget b{}; b.wait_timeout = std::chrono::milliseconds{5000};
    b.idle_keep_alive = std::chrono::milliseconds{60000};
    printf("start=%d\n", (int)pool.start(2, b));
    const std::string nm = "r42_unreg2_" + std::to_string(dom);
    auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), nm, (size_t)dom, false); pub->InitChannel("u");
    auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), nm, (size_t)dom, 64, false); sub->InitChannel("u");
    for (int i=0;i<300 && pool.route_count()==0;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto t0 = Clock::now();
    sample("before", t0);
    printf("--- 断发布者 ---\n"); t0 = Clock::now();
    pub.reset();
    sample("after pub.reset", t0);
    for (int i=0;i<6;++i){ std::this_thread::sleep_for(std::chrono::milliseconds(250)); sample("settle", t0); }
    printf("--- 注销订阅 ---\n"); t0 = Clock::now();
    const auto t1 = Clock::now();
    sub.reset();
    const double d = std::chrono::duration<double,std::milli>(Clock::now()-t1).count();
    printf("sub.reset() returned in %.2f ms; routes=%zu\n", d, pool.route_count());
    sample("after sub.reset", t0);
    for (int i=0;i<8;++i){ std::this_thread::sleep_for(std::chrono::milliseconds(250)); sample("settle2", t0); }
    pub.reset();
    ipc::route::clear_storage(shm_topic_segment_name(nm,(size_t)dom).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(nm,(size_t)dom).c_str());
    printf("PROBE2_DONE\n");
    _exit(0);
}
