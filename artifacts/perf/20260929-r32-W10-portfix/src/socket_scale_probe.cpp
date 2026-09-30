/* D-22 验收探针：socket 拓扑 N 个**独立**话题的「逐 route 有效收发」台账。
 *
 * 为什么另建而不复用 test/perf/w10/w10_matrix.cpp：那份工装的 `w10_pick_socket_topics()`
 * 解析 /proc/net/udp 时取的是**行首 slot 号**（"145:"）而不是 local_address 列，
 * 于是"排除已占端口"实际只排除了端口 0 —— 复现件见 ../03_picker_parse_check.txt。
 * 本探针用正确解析（第 2 列 local_address 的冒号后 16 进制）做端口段避让，因此它能
 * 把「修复后的有界失败」与「端口不再碰撞时千路能否真的收发」两件事**分开**验证。
 *
 * 两种取名模式：
 *   --names=naive   照 w10_matrix 的原始命名（w10_socket_independent_<dom>_<i>），保留碰撞
 *   --names=scanned 端口段 [base, base+4] 互不重叠且避开当前已占端口（正确解析）
 *
 * 输出：每 route 一行台账 + 汇总；缺一路或漏一包 ⇒ verdict=FAIL。
 * 用法: socket_scale_probe <n> <domain> <msgs> <names:naive|scanned> */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/hash.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

namespace {
constexpr int kMsgId = 77;

int thread_count()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("Threads:", 0) == 0) return std::atoi(line.c_str() + 8);
    return -1;
}

long fd_count(long* maxfd = nullptr)
{
    long n = 0, mx = -1;
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        ++n;
        const long v = std::atol(e->d_name);
        if (v > mx) mx = v;
    }
    ::closedir(d);
    if (maxfd) *maxfd = mx;
    return n;
}

/* 正确解析：第 1 段是 slot 号，第 2 段才是 local_address（"HEXADDR:HEXPORT"）。 */
std::set<int> occupied_udp_ports()
{
    std::set<int> used;
    std::ifstream f("/proc/net/udp");
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line))
    {
        std::istringstream is(line);
        std::string slot, local;
        if (!(is >> slot >> local)) continue;
        const auto c = local.find(':');
        if (c == std::string::npos) continue;
        used.insert(static_cast<int>(std::strtol(local.c_str() + c + 1, nullptr, 16)));
    }
    return used;
}

std::vector<std::string> pick_names(long dom, long want, const char* mode)
{
    std::vector<std::string> out;
    if (std::strcmp(mode, "naive") == 0)
    {
        for (long i = 0; i < want; ++i)
            out.push_back("w10_socket_independent_" + std::to_string(dom) + "_" + std::to_string(i));
        return out;
    }
    std::set<int> used = occupied_udp_ports();
    for (long i = 0; static_cast<long>(out.size()) < want && i < 2000000; ++i)
    {
        const std::string name = "w10_socket_independent_" + std::to_string(dom) + "_" + std::to_string(i);
        const int base = dzIPC::common::udp_discovery_port_calculate(name, static_cast<int>(dom));
        bool ok = true;
        for (int o = 0; o < 5 && ok; ++o)
            if (used.count(base + o) != 0) ok = false;
        if (!ok) continue;
        for (int o = 0; o < 5; ++o) used.insert(base + o);
        out.push_back(name);
    }
    return out;
}

std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}
}  // namespace

int main(int argc, char** argv)
{
    const long n = (argc > 1) ? std::atol(argv[1]) : 100;
    const long dom = (argc > 2) ? std::atol(argv[2]) : 3103;
    const int msgs = (argc > 3) ? std::atoi(argv[3]) : 1;
    const char* names_mode = (argc > 4) ? argv[4] : "scanned";

    const auto names = pick_names(dom, n, names_mode);
    std::printf("probe=socket_scale n=%ld domain=%ld msgs=%d names=%s picked=%zu threaded=%s\n", n, dom, msgs,
                names_mode, names.size(), (thread_count() > 0) ? "yes" : "no");
    std::fflush(stdout);
    if (static_cast<long>(names.size()) < n)
    {
        std::printf("PICK_SHORT picked=%zu want=%ld\n", names.size(), n);
        return 3;
    }

    std::vector<std::unique_ptr<dzIPC::socket::socket_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    pubs.reserve(names.size());
    subs.reserve(names.size());

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        subs.emplace_back(new dzIPC::socket::socket_sub_ipc(td(), names[i], static_cast<std::size_t>(dom), 64, false));
        subs.back()->InitChannel("d22");
    }
    const auto subs_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        pubs.emplace_back(new dzIPC::socket::socket_pub_ipc(td(), names[i], static_cast<std::size_t>(dom), false));
        pubs.back()->InitChannel("d22");
    }
    /* 分开计时：订阅侧每路 ~2 fd + 收包 worker；发布侧每路一条 discovery_loop
     * （50 ms 轮询 IpcInfoPool）。两者混在一起无法判断规模成本的归属。 */
    const auto pubs_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
        - subs_ms;
    const auto create_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    long maxfd = -1;
    const long fds = fd_count(&maxfd);
    std::printf("create_ms=%lld subs_ms=%lld pubs_ms=%lld fds_open=%ld max_fd=%ld threads=%d\n",
                (long long)create_ms, (long long)subs_ms, (long long)pubs_ms, fds, maxfd, thread_count());
    std::fflush(stdout);

    /* 订阅者发现通道是 IpcInfoPool 50 ms 轮询；给足窗口（与 w10_matrix 的 400 ms 同口径）。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    /* 逐 route 发 + 逐 route 收：台账落在每个 route 自己的计数上。 */
    std::vector<int> rx(names.size(), 0);
    std::vector<char> got(names.size(), 0);
    std::vector<long> first_us(names.size(), -1);
    int tx_ok = 0;
    for (int k = 0; k < msgs; ++k)
    {
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "route" + std::to_string(i) + "#" + std::to_string(k);
            if (pubs[i]->publish_best_effort(m)) ++tx_ok;
        }
    }
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        long total = 0;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            auto sink = td();
            while (subs[i]->try_get_clone(sink))
            {
                ++rx[i];
                if (!got[i])
                {
                    got[i] = 1;
                    first_us[i] = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                }
            }
            total += rx[i];
            sink.reset();
        }
        if (total >= static_cast<long>(names.size()) * msgs) break;
        if (std::chrono::steady_clock::now() >= drain_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    long routes_ok = 0, routes_lost = 0;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const bool ok = (rx[i] >= msgs);
        if (ok) ++routes_ok; else ++routes_lost;
        std::printf("LEDGER route=%zu topic=%s base_port=%d rx=%d/%d %s\n", i, names[i].c_str(),
                    dzIPC::common::udp_discovery_port_calculate(names[i], static_cast<int>(dom)), rx[i], msgs,
                    ok ? "OK" : "LOST");
    }
    const auto total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("SUMMARY n=%zu tx_ok=%d tx_expected=%zu routes_ok=%ld routes_lost=%ld total_ms=%lld\n", names.size(),
                tx_ok, names.size() * static_cast<std::size_t>(msgs), routes_ok, routes_lost, (long long)total_ms);
    std::printf("SOCKET_SCALE_DONE verdict=%s\n", (routes_lost == 0 && tx_ok == static_cast<int>(names.size() * static_cast<std::size_t>(msgs))) ? "PASS" : "FAIL");
    std::fflush(stdout);
    /* ⛔ 不析构：千路析构要跑 2000 ms×N 的静默等待，且本探针只取数。
     * 段/端口随进程退出由内核回收（共享段属 SHM 侧，本探针不建）。 */
    ::fflush(nullptr);
    _exit(routes_lost == 0 ? 0 : 1);
}
