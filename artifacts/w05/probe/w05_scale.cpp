/* W05 验收探针（非交付物；证据用）—— SHM pub/sub 控制面线程数与有效收发随规模的变化。
 *
 * 判据（对应 t6 验收）：
 *   · 默认路径：`threads_after_create` 随话题数**不变**（进程级 O(1) 控制面线程）；
 *   · 兼容回退（DZIPC_SHM_CONTROL_SCHEDULER=1）：线程数随话题数线性增长（对照臂）；
 *   · 千路下 attached == sent == received == n（有效收发，不是"线程少了"）。
 *
 * 编译（在仓库根）：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_scale.cpp \
 *       -o artifacts/w05/scratch-probe/w05_scale -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 *
 * ⚠️ 探针纪律（本文件踩过的坑，写在这里免得下游重踩）：
 *   · **不得在 pub/sub 对象仍存活时 clear_storage()** —— 那会在头库与基线库上
 *     **同样**崩溃（实测 3/3 SIGSEGV，`ipc::sync::mutex::lock ← waiter::wake ←
 *     chan_impl::disconnect`），属测试脚手架误用，与本工作包无关；
 *   · pub/sub 的**声明顺序**决定析构顺序（C++ 逆序析构）。本探针让所有对象活到
 *     清理阶段之前，再统一 destruction + clear，避免析构与清段竞争。
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_image.hpp"

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMsgId = 91;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!std::strcmp(argv[i], key))
        {
            return std::strtol(argv[i + 1], nullptr, 10);
        }
    }
    return def;
}

static std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr)
    {
        return 0;
    }
    std::size_t n = 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n;
}

static std::size_t rss_kb()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("VmRSS:", 0) == 0)
        {
            return std::strtoul(line.c_str() + 6, nullptr, 10);
        }
    }
    return 0;
}

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

int main(int argc, char** argv)
{
    const long n = arg_long(argc, argv, "--n", 100);
    const long verify = arg_long(argc, argv, "--verify", 1);
    /* --subs 0 ⇒ 只建发布者（数据面无收包线程）。于是"线程数 - 基线"就是**控制面
     * 线程数**本身：这正是 t6 验收要的那个量（"控制线程数为进程级固定数量"），
     * 不会被数据面线程掩盖。 */
    const long with_subs = arg_long(argc, argv, "--subs", 1);
    const long with_pubs = arg_long(argc, argv, "--pubs", 1);
    const long hold_ms = arg_long(argc, argv, "--hold-ms", 500);
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 91));
    const bool compat = [] {
        const char* v = std::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    std::printf("probe=w05_scale scale=%ld domain=%d arm=%s pubs=%ld subs=%ld\n", n, domain,
                compat ? "compat" : "scheduler", with_pubs, with_subs);
    std::printf("threads_t0=%zu\n", thread_count());

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(n));
    using PubPtr = std::unique_ptr<dzIPC::shm::shm_pub_ipc>;
    using SubPtr = std::unique_ptr<dzIPC::shm::shm_sub_ipc>;
    std::vector<PubPtr> pubs;
    std::vector<SubPtr> subs;
    pubs.reserve(static_cast<std::size_t>(n));
    subs.reserve(static_cast<std::size_t>(n));

    const auto t0 = Clock::now();
    for (long i = 0; i < n; ++i)
    {
        names.push_back("w05scale_" + std::to_string(domain) + "_" + std::to_string(i));
        if (with_pubs)
        {
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(domain), false));
            pubs.back()->InitChannel("w05");
        }
        if (with_subs)
        {
            subs.emplace_back(
                new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(domain), 8, false));
            subs.back()->InitChannel("w05");
        }
    }
    const double create_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("create_total_ms=%.1f threads_after_create=%zu rss_kb=%zu\n", create_ms, thread_count(), rss_kb());

    /* 握手：每个话题的 peer_count 到位（用控制面只读采样，独立于产品内部状态）。 */
    long attached = 0;
    /* 只有发布端时控制面不会进入 Ready（owner 才 set_ready）⇒ 不等待 attach。 */
    const long attach_target = (with_subs && with_pubs) ? n : 0;
    const auto dl = Clock::now() + std::chrono::seconds(60);
    while (attach_target > 0 && Clock::now() < dl && attached < attach_target)
    {
        attached = 0;
        for (long i = 0; i < n; ++i)
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(domain)))
                && cp.peer_count() >= 1)
            {
                ++attached;
            }
        }
        if (attached < n)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::printf("attached=%ld/%ld threads_after_attach=%zu rss_kb=%zu\n", attached, n, thread_count(), rss_kb());

    /* 有效收发：每话题各发一条，各自的订阅者必须收到（不是"随便有一条到了"）。 */
    long sent = 0, got = 0;
    if (verify && with_subs)
    {
        for (long i = 0; i < n; ++i)
        {
            auto img = std::make_shared<dzIPC::Msg::StdImage>();
            img->set_msg_id(kMsgId);
            img->width = 2;
            img->height = 2;
            img->step = 6;
            img->encoding = "rgb8";
            img->data.assign(12, static_cast<std::uint8_t>(i & 0xFF));
            if (pubs[static_cast<std::size_t>(i)]->publish(img))
            {
                ++sent;
            }
        }
        const auto dl2 = Clock::now() + std::chrono::seconds(60);
        std::vector<bool> ok(static_cast<std::size_t>(n), false);
        while (Clock::now() < dl2 && got < n)
        {
            got = 0;
            for (long i = 0; i < n; ++i)
            {
                if (ok[static_cast<std::size_t>(i)])
                {
                    ++got;
                    continue;
                }
                auto sink = td();
                if (subs[static_cast<std::size_t>(i)]->try_get_clone(sink))
                {
                    ok[static_cast<std::size_t>(i)] = true;
                    ++got;
                }
            }
            if (got < n)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::printf("sent=%ld received=%ld/%ld\n", sent, got, n);
    }

    const std::size_t th_peak = thread_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    std::printf("threads_peak=%zu threads_idle=%zu\n", th_peak, thread_count());
    auto st = dzIPC::shm_control::ShmControlScheduler::instance().stats();
    std::printf("sched_entries=%zu tick_count=%llu tick_last_ns=%lld tick_max_ns=%lld overruns=%llu "
                "deferred=%llu exceptions=%llu\n",
                dzIPC::shm_control::ShmControlScheduler::instance().entry_count(),
                static_cast<unsigned long long>(st.tick_count), static_cast<long long>(st.tick_duration_last_ns),
                static_cast<long long>(st.tick_duration_max_ns),
                static_cast<unsigned long long>(st.tick_overrun_count),
                static_cast<unsigned long long>(st.tick_deferred_count),
                static_cast<unsigned long long>(st.callback_exception_count));

    const auto t1 = Clock::now();
    pubs.clear();
    subs.clear();
    const double destroy_ms = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("destroy_total_ms=%.1f threads_after_destroy=%zu entries_after_destroy=%zu\n", destroy_ms,
                thread_count(), dzIPC::shm_control::ShmControlScheduler::instance().entry_count());

    /* 全部对象已析构之后才清段（见文件头纪律）。 */
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
