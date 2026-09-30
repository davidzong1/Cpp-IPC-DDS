/* 后端错误最小用例：LD_PRELOAD 注入 add=false ⇒ 必须**显式失败**（backend_unavailable /
 * fallback_backend_unavailable），⛔ 不得静默降级也不得忙等。
 * 用法：probe_backend [--expect-unavailable 0|1] [--out <f>] */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fstream>
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"
using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;
static std::atomic<long> g_compat{0}, g_compat_bk{0}, g_worker{0};
static void hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker) { g_worker.fetch_add(1); return; }
    if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat) return;
    g_compat.fetch_add(1);
    if (static_cast<dzIPC::detail::RecvPathReason>(ev.size) == dzIPC::detail::RecvPathReason::kBackendUnavailable)
        g_compat_bk.fetch_add(1);
}
static long arg_long(int c, char** v, const char* k, long d) { for (int i=1;i+1<c;++i) if (!std::strcmp(v[i],k)) return strtol(v[i+1],nullptr,10); return d; }
static const char* arg_str(int c, char** v, const char* k, const char* d) { for (int i=1;i+1<c;++i) if (!std::strcmp(v[i],k)) return v[i+1]; return d; }
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long dom = arg_long(argc, argv, "--domain", 8900);
    const std::string out = arg_str(argc, argv, "--out", "");
    dzIPC::detail::SetSeamHook(&hook);
    auto s = dzIPC::measure::CounterRegistry::instance().snapshot();
    auto g = [&](dzIPC::measure::CounterId id) { return (unsigned long long)s.get(id); };
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); };
    const bool av = dzIPC::threepools::RecvWorkerPool::backend_available();
    const char* bn = dzIPC::threepools::RecvWorkerPool::backend_name();
    /* 走真实产品路径：建一条订阅 ⇒ 期望它走**显式回退**而不是静默降级 */
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    const long N = 3;
    long ok_rx = 0;
    for (long i = 0; i < N; ++i)
    {
        const std::string nm = "bk_" + std::to_string(dom) + "_" + std::to_string(i);
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), nm, (size_t)dom, false));
        pubs.back()->InitChannel("bk");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), nm, (size_t)dom, 64, false));
        subs.back()->InitChannel("bk");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (long i = 0; i < N; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>(); m->set_msg_id(kMsgId); m->str = "X" + std::to_string(i);
        (void)pubs[(size_t)i]->publish(m);
    }
    for (long k = 0; k < 25; ++k)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        long got = 0;
        for (long i = 0; i < N; ++i) { auto sink = td(); if (subs[(size_t)i]->try_get_clone(sink)) ++got; }
        if (got == N) { ok_rx = got; break; }
        ok_rx = got;
    }
    const auto s2 = dzIPC::measure::CounterRegistry::instance().snapshot();
    auto g2 = [&](dzIPC::measure::CounterId id) { return (unsigned long long)s2.get(id); };
    printf("backend_available=%d backend_name=%s\n", (int)av, bn);
    printf("seam worker=%ld compat=%ld compat_backend_unavailable=%ld\n", g_worker.load(), g_compat.load(), g_compat_bk.load());
    printf("counters fallback_total=%llu fallback_backend_unavailable=%llu registration_attempts=%llu registration_ok=%llu\n",
           g2(dzIPC::measure::CounterId::fallback_total) - g(dzIPC::measure::CounterId::fallback_total),
           g2(dzIPC::measure::CounterId::fallback_backend_unavailable) - g(dzIPC::measure::CounterId::fallback_backend_unavailable),
           g2(dzIPC::measure::CounterId::registration_attempts) - g(dzIPC::measure::CounterId::registration_attempts),
           g2(dzIPC::measure::CounterId::registration_ok) - g(dzIPC::measure::CounterId::registration_ok));
    printf("rx=%ld/%ld pool_route_count=%zu pool_backend_available=%d\n", ok_rx, N,
           dzIPC::threepools::RecvWorkerPool::instance().route_count(),
           (int)dzIPC::threepools::RecvWorkerPool::instance().backend_available());
    if (!out.empty())
    {
        std::ofstream f(out);
        f << "{\n \"backend_available\": " << (av?"true":"false") << ",\n \"backend_name\": \"" << bn << "\",\n"
          << " \"compat_events\": " << g_compat.load() << ",\n \"compat_backend_unavailable\": " << g_compat_bk.load() << ",\n"
          << " \"worker_events\": " << g_worker.load() << ",\n \"fallback_total\": "
          << (g2(dzIPC::measure::CounterId::fallback_total) - g(dzIPC::measure::CounterId::fallback_total)) << ",\n"
          << " \"fallback_backend_unavailable\": "
          << (g2(dzIPC::measure::CounterId::fallback_backend_unavailable) - g(dzIPC::measure::CounterId::fallback_backend_unavailable)) << ",\n"
          << " \"rx\": " << ok_rx << ",\n \"expected\": " << N << "\n}\n";
    }
    dzIPC::detail::SetSeamHook(nullptr);
    for (long i = 0; i < N; ++i)
    {
        const std::string nm = "bk_" + std::to_string(dom) + "_" + std::to_string(i);
        ipc::route::clear_storage(shm_topic_segment_name(nm, (size_t)dom).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, (size_t)dom).c_str());
    }
    printf("PROBE_BACKEND_DONE\n");
    return 0;
}
