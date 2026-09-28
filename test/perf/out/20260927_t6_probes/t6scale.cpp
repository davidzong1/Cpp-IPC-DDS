// t6 scale probe — ABI-valid (current headers). Measures receive-thread count, ctx switches,
// idle CPU and (optional) functional receipt at scale N, in the same units as baseline_probe.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {
constexpr std::uint32_t kMsgId = 37;
using Clock = std::chrono::steady_clock;

long arg_long(int argc, char** argv, const char* key, long def) {
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], key, klen) && argv[i][klen] == '=' && argv[i][klen])
            return std::strtol(argv[i] + klen + 1, nullptr, 10);
        if (!std::strcmp(argv[i], key) && i + 1 < argc) return std::strtol(argv[i + 1], nullptr, 10);
    }
    return def;
}
bool arg_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
struct ProcStat { unsigned long long vol{0}, nonvol{0}, cpu_ticks{0}; };
long ticks_per_sec() { static const long v = sysconf(_SC_CLK_TCK); return v; }
ProcStat proc_stat() {
    ProcStat s;
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0) s.vol = std::strtoull(line.c_str() + 25, nullptr, 10);
        else if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) s.nonvol = std::strtoull(line.c_str() + 28, nullptr, 10);
    }
    std::ifstream h("/proc/self/stat");
    std::string all; std::getline(h, all);
    const auto close = all.rfind(')');
    if (close != std::string::npos) {
        std::istringstream is(all.substr(close + 2));
        std::string t;
        int idx = 1;                     // rest[1] == field 3 (state)
        unsigned long long utime = 0, stime = 0;
        while (is >> t) {
            if (idx == 12) utime = std::strtoull(t.c_str(), nullptr, 10);   // field 14
            else if (idx == 13) stime = std::strtoull(t.c_str(), nullptr, 10);  // field 15
            ++idx;
        }
        s.cpu_ticks = utime + stime;
    }
    return s;
}
std::size_t thread_count() {
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    std::size_t n = 0;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n;
    closedir(d);
    return n;
}
std::size_t fd_count(std::size_t* maxfd) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    std::size_t n = 0, mx = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ++n;
        const long v = std::strtol(e->d_name, nullptr, 10);
        if (v > 0 && static_cast<std::size_t>(v) > mx) mx = static_cast<std::size_t>(v);
    }
    closedir(d);
    if (maxfd) *maxfd = mx;
    return n;
}
}  // namespace

int main(int argc, char** argv) {
    const long scale = arg_long(argc, argv, "--scale", 1);
    const long domain = arg_long(argc, argv, "--domain", 1);
    const long idle = arg_long(argc, argv, "--idle-ms", 2000);
    const long sample = arg_long(argc, argv, "--sample-ms", 200);
    const long send_msgs = arg_long(argc, argv, "--send-msgs", 0);
    const std::string topic = "t6scale";
    std::printf("mode=socket_pub_sub\nscale=%ld\npid=%d\n", scale, (int)getpid());

    const auto t0 = Clock::now();
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    subs.reserve(static_cast<std::size_t>(scale));
    for (long i = 0; i < scale; ++i) {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(
            td, topic, static_cast<std::size_t>(domain), 1024, false));
        subs.back()->InitChannel("scale");
    }
    const double create_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::size_t maxfd = 0; std::size_t fds = fd_count(&maxfd);
    std::printf("create_total_ms=%.3f\ncreate_per_sub_ms=%.3f\nfds_open=%zu\nmax_socket_fd=%zu\n",
                create_ms, create_ms / (scale ? scale : 1), fds, maxfd);

    long sent = 0, received = 0;
    {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::socket::socket_pub_ipc pub{td, topic, static_cast<std::size_t>(domain), false};
        pub.InitChannel("scale");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (send_msgs > 0) {
            for (long i = 0; i < send_msgs; ++i) {
                auto m = std::make_shared<dzIPC::Msg::StdImage>();
                m->set_msg_id(kMsgId);          // 模板 id 必须匹配，否则订阅端按 IdSkipped 拒收
                if (pub.publish(m)) ++sent;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            dzIPC::Sample s;
            while (subs[0]->try_get(s)) ++received;              // 视图队列（DZFlat 段）
            auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
            while (subs[0]->try_get_clone(sink)) ++received;      // 物化队列（TLV）
        }
    }
    std::printf("sent=%ld\nreceived_sub0=%ld\n", sent, received);

    const ProcStat before = proc_stat();
    const auto w0 = Clock::now();
    std::size_t tmin = static_cast<std::size_t>(-1), tmax = 0, tsum = 0, tn = 0;
    std::vector<std::size_t> series;
    for (long e = 0; e < idle; e += sample) {
        const std::size_t t = thread_count();
        series.push_back(t);
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
        tsum += t; ++tn;
        std::this_thread::sleep_for(std::chrono::milliseconds(sample));
    }
    const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
    const ProcStat after = proc_stat();
    const long hz = ticks_per_sec();
    const double cpu = static_cast<double>(after.cpu_ticks - before.cpu_ticks) / (hz > 0 ? hz : 100);
    std::printf("threads_max=%zu\nthreads_min=%zu\nthreads_avg=%.3f\nthreads_per_sub=%.3f\n",
                tmax, tmin == static_cast<std::size_t>(-1) ? 0 : tmin,
                tn ? static_cast<double>(tsum) / tn : 0.0,
                scale ? static_cast<double>(tmax) / scale : 0.0);
    std::printf("ctx_vol=%llu\nctx_nonvol=%llu\nctx_vol_per_s=%.3f\nctx_nonvol_per_s=%.3f\n",
                after.vol - before.vol, after.nonvol - before.nonvol,
                wall > 0 ? (after.vol - before.vol) / wall : 0.0,
                wall > 0 ? (after.nonvol - before.nonvol) / wall : 0.0);
    std::printf("idle_window_s=%.3f\ncpu_cores=%.4f\n", wall, wall > 0 ? cpu / wall : 0.0);
    std::printf("threads_series=");
    for (std::size_t i = 0; i < series.size(); ++i) std::printf("%s%zu", i ? "," : "", series[i]);
    std::printf("\nDONE\n");
    std::fclose(stdout);
    _exit(0);   // skip static destructors: 空闲退出由 OS 回收
}
