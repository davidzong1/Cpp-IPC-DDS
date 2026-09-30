/* W06 规模验收探针（证据用，非交付源）：N 个独立话题 pub/sub 有效收发 + 接收路径可分性。
 *
 * 判据（与任务书验收条一一对应）：
 *   registered_count   —— 本进程在册订阅数（= 成功 InitChannel 的 sub 数）
 *   valid_rx_count     —— 逐话题**各自**收到自己那条消息的数（不是"有一条到了"）
 *   fallback_count     —— seam 上 kRecvPathCompat 且原因码 ∈ {backend_unavailable,
 *                          wait_set_full, invalid_token, pool_start_failed, busy, duplicate,
 *                          stopped, invalid_route} 的次数（⛔ 不含 forced/fork/<｜place▁holder▁no▁162｜>no_route = 路径选择）
 *   worker_path_count  —— seam 上 kRecvPathWorker 的次数
 *   预算回显            —— RecvWorkerPool::budget()（§10.4：实际生效值，不是"我以为传了"）
 * 用法：w06_scale --n 1000 --domain 200 [--msg-bytes 64]
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 91;

static struct SeamTally
{
    std::mutex m;
    long worker_path{0};
    long compat_total{0};
    long fallback{0};          /* 真回退（口径见文件头） */
    long path_choice{0};       /* forced / fork / no_route */
    std::vector<int> reasons;
    void on(const dzIPC::detail::SeamEvent& ev)
    {
        std::lock_guard<std::mutex> lock(m);
        if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker)
        {
            ++worker_path;
            return;
        }
        if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat)
        {
            return;
        }
        ++compat_total;
        reasons.push_back(static_cast<int>(ev.size));
        switch (static_cast<dzIPC::detail::RecvPathReason>(ev.size))
        {
        case dzIPC::detail::RecvPathReason::kForcedCompatEnv:
        case dzIPC::detail::RecvPathReason::kForkChild:
        case dzIPC::detail::RecvPathReason::kNoRoute:
            ++path_choice;
            break;
        default:
            ++fallback;
            break;
        }
    }
} g_seam;

static void w06_seam_hook(const dzIPC::detail::SeamEvent& ev) noexcept { g_seam.on(ev); }

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], key) == 0)
        {
            return std::strtol(argv[i + 1], nullptr, 10);
        }
    }
    return def;
}

static size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return 0;
    size_t n = 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long n = arg_long(argc, argv, "--n", 100);
    const long payload = arg_long(argc, argv, "--msg-bytes", 64);
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 200));

    /* seam 钩子是**普通函数指针**（无捕获）⇒ 计数对象用文件级静态指针。 */
    static SeamTally tally;
    dzIPC::detail::SetSeamHook(&w06_seam_hook);

    auto td = [] {
        return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
    };
    std::vector<std::string> names;
    using PubPtr = std::unique_ptr<dzIPC::shm::shm_pub_ipc>;
    using SubPtr = std::unique_ptr<dzIPC::shm::shm_sub_ipc>;
    std::vector<PubPtr> pubs;
    std::vector<SubPtr> subs;
    names.reserve(static_cast<std::size_t>(n));
    pubs.reserve(static_cast<std::size_t>(n));
    subs.reserve(static_cast<std::size_t>(n));

    const auto t0 = Clock::now();
    for (long i = 0; i < n; ++i)
    {
        names.push_back("w06scale_" + std::to_string(domain) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(domain), false));
        pubs.back()->InitChannel("w06");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(domain), 8, false));
        subs.back()->InitChannel("w06");
    }
    const double create_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    /* 先等每个话题的控制面 peer 到位（订阅者握手完成）再发：否则发布落在"还没连上"
     * 的窗口里，测出来的就不是接收池的能力。 */
    long registered = 0;
    {
        const auto adl = Clock::now() + std::chrono::seconds(120);
        while (Clock::now() < adl && registered < n)
        {
            registered = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)],
                                                   static_cast<std::size_t>(domain)))
                    && cp.peer_count() >= 1)
                {
                    ++registered;
                }
            }
            if (registered < n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    std::printf("registered_count=%ld/%ld threads_after_create=%zu create_ms=%.1f\n", registered, n,
                thread_count(), create_ms);

    long sent = 0;
    for (long i = 0; i < n; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str.assign(static_cast<std::size_t>(payload), static_cast<char>('a' + (i % 26)));
        if (pubs[static_cast<std::size_t>(i)]->publish(m)) ++sent;
    }

    std::vector<char> ok(static_cast<std::size_t>(n), 0);
    long valid = 0;
    const auto dl = Clock::now() + std::chrono::seconds(120);
    while (Clock::now() < dl && valid < n)
    {
        valid = 0;
        for (long i = 0; i < n; ++i)
        {
            if (ok[static_cast<std::size_t>(i)]) { ++valid; continue; }
            auto sink = td();
            if (subs[static_cast<std::size_t>(i)]->try_get_clone(sink))
            {
                auto s = sink->topic()->msgcast<dzIPC::Msg::StdString>();
                if (s && s->str.size() == static_cast<std::size_t>(payload)
                    && s->str[0] == static_cast<char>('a' + (i % 26)))
                {
                    ok[static_cast<std::size_t>(i)] = 1;
                    ++valid;
                }
            }
        }
        if (valid < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("valid_rx_count=%ld/%ld sent=%ld payload=%ld\n", valid, n, sent, payload);

    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const auto st = pool.stats();
    const auto& b = pool.budget();
    std::printf("pool_started=%d workers=%zu route_count=%zu msgs=%llu wait_wakeups=%llu wait_timeouts=%llu "
                "idle_exits=%llu restarts=%llu recv_once_calls=%llu recv_once_max_ns=%llu over_budget=%llu "
                "recv_errors=%llu\n",
                static_cast<int>(pool.running()), pool.worker_count(), st.route_count,
                static_cast<unsigned long long>(st.messages_received),
                static_cast<unsigned long long>(st.wait_wakeups),
                static_cast<unsigned long long>(st.wait_timeouts),
                static_cast<unsigned long long>(st.idle_exits),
                static_cast<unsigned long long>(st.thread_restarts),
                static_cast<unsigned long long>(st.recv_once_calls),
                static_cast<unsigned long long>(st.recv_once_max_ns),
                static_cast<unsigned long long>(st.recv_once_over_budget),
                static_cast<unsigned long long>(st.recv_errors));
    std::printf("budget msgs=%zu bytes=%zu time_us=%lld wait_ms=%lld idle_ms=%lld\n", b.max_messages_per_route,
                b.max_bytes_per_route, static_cast<long long>(b.max_processing_time_per_route.count()),
                static_cast<long long>(b.wait_timeout.count()), static_cast<long long>(b.idle_keep_alive.count()));

    long registered_after = 0;
    for (long i = 0; i < n; ++i)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(domain)))
            && cp.peer_count() >= 1)
        {
            ++registered_after;
        }
    }
    std::printf("registered_after_rx=%ld/%ld\n", registered_after, n);

    {
        const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
        const auto v = [&](dzIPC::measure::CounterId id) {
            return static_cast<unsigned long long>(c.get(id));
        };
        std::printf("counters fallback_total=%llu backend_unavailable=%llu capacity_full=%llu wait_set_full=%llu "
                    "wait_token_invalid=%llu registration_attempts=%llu registration_ok=%llu "
                    "registration_failed=%llu registration_busy=%llu registration_duplicate=%llu "
                    "registration_stopped=%llu registration_invalid_token=%llu registration_invalid_route=%llu\n",
                    v(dzIPC::measure::CounterId::fallback_total),
                    v(dzIPC::measure::CounterId::fallback_backend_unavailable),
                    v(dzIPC::measure::CounterId::fallback_capacity_full),
                    v(dzIPC::measure::CounterId::wait_set_full),
                    v(dzIPC::measure::CounterId::wait_token_invalid),
                    v(dzIPC::measure::CounterId::registration_attempts),
                    v(dzIPC::measure::CounterId::registration_ok),
                    v(dzIPC::measure::CounterId::registration_failed),
                    v(dzIPC::measure::CounterId::registration_busy),
                    v(dzIPC::measure::CounterId::registration_duplicate),
                    v(dzIPC::measure::CounterId::registration_stopped),
                    v(dzIPC::measure::CounterId::registration_invalid_token),
                    v(dzIPC::measure::CounterId::registration_invalid_route));
    }
    {
        std::lock_guard<std::mutex> lock(g_seam.m);
        std::printf("worker_path_count=%ld compat_total=%ld fallback_count=%ld path_choice=%ld\n",
                    g_seam.worker_path, g_seam.compat_total, g_seam.fallback, g_seam.path_choice);
        std::printf("reasons=");
        for (int r : g_seam.reasons) std::printf("%d,", r);
        std::printf("\n");
    }

    const std::size_t routes_before_destroy = pool.route_count();
    pubs.clear();
    subs.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("routes_before_destroy=%zu routes_after_destroy=%zu threads_after_destroy=%zu\n", routes_before_destroy,
                pool.route_count(), thread_count());

    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W06_SCALE_DONE\n");
    return (valid == n && sent == n) ? 0 : 1;
}
