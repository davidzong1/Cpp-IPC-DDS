/**
 * @file    dzipc_perf_benchmark.cpp
 * @brief   dzIPC 通信性能测试程序
 *
 * 覆盖 4 种通信组合:
 *   - pub-sub over SHM      (IPC_SHM,    PublisherIPC / SubscriberIPC)
 *   - pub-sub over SOCKET   (IPC_SOCKET, PublisherIPC / SubscriberIPC)
 *   - ser-cli over SHM      (IPC_SHM,    ServerIPC / ClientIPC)
 *   - ser-cli over SOCKET   (IPC_SOCKET, ServerIPC / ClientIPC)
 *
 * 通信 API 用法与 test/test_dzipc.cpp 保持一致。
 *
 * 测试方式(对齐 docs/ipc_performance_optimization_report.md 的 benchmark 计划):
 *   - 真实跨进程: 每个用例 fork()+exec() 出一个全新的对端进程, 不用同进程线程,
 *     也不启用 Nodelet 同进程快速路径, 保证测的是真实 IPC 链路
 *   - 每个用例先 warmup, 再固定时长测量
 *   - payload 扫描: 64B ... 1MB
 *   - 两类测量:
 *       lat  : 固定速率(默认 1kHz)发送, 得到无排队干扰的时延分布
 *       tput : 全速发送, 得到吞吐上限与丢包率
 *     ser-cli 为同步请求-响应, 单次运行同时给出 RTT 分布与 req/s
 *   - 指标: 吞吐(msg/s, MB/s)、时延 min/mean/p50/p90/p99/p999/max、抖动(标准差)、
 *           丢包率、CPU 占用(核数)
 *   - 记录完整硬件与系统配置
 *   - 输出 CSV / JSON / 时延样本, 供 scripts/plot_dzipc_perf.py 绘制性能曲线
 *
 * 时延定义:
 *   pub-sub : 单向时延。发布端把 steady_clock 时间戳打进消息, 订阅端取出后与本地
 *             时钟作差。收发进程在同一台机器、同一个 CLOCK_MONOTONIC 上, 可直接比较。
 *   ser-cli : 往返时延(RTT), 由客户端本地测量, 不依赖时钟同步。
 *
 * 用法:
 *   ./dzipc_perf_benchmark [选项]
 *     --out=DIR           结果输出目录 (默认 ./perf_results/<时间戳>)
 *     --payloads=A,B,C    payload 字节数列表
 *                         (默认 64,256,1024,4096,16384,65536,262144,1048576)
 *     --duration=S        每个用例测量时长, 秒 (默认 3)
 *     --warmup=S          每个用例预热时长, 秒 (默认 0.5)
 *     --lat-rate=HZ       时延用例的固定发送速率 (默认 1000)
 *     --cases=LIST        选择用例, 逗号分隔:
 *                         pubsub_shm,pubsub_socket,sercli_shm,sercli_socket (默认全部)
 *     --skip-tput         跳过 pub-sub 全速吞吐用例
 *     --pin=A,B           把发送端/接收端进程分别绑到 CPU A 和 B
 *     --queue=N           订阅端队列深度 (默认 1024)
 *     --domain=N          domain id (默认 88)
 *     --help
 *
 *   以下参数供程序内部 fork+exec 子进程使用, 不需要手工传:
 *     --role=sub|srv  --ctl=NAME  --topic=NAME  --payload=N  --transport=shm|socket
 */

#include "dzIPC/common/hash.h"
#include "dzIPC/common/data_rev.h"
#include "dzIPC/dzipc.h"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono;
using dzIPC::IPC_SHM;
using dzIPC::IPC_SOCKET;

using Clock = steady_clock;

constexpr int kMsgId = 4242;      // pub-sub 模板消息 id
constexpr int kSrvMsgId = 4243;   // ser-cli 模板消息 id
constexpr std::size_t kMaxSamples = 1u << 21;   // 最多保存 2Mi 条时延样本(约 16MiB)

inline std::uint64_t now_ns()
{
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(Clock::now().time_since_epoch()).count());
}

/* ------------------------------------------------------------------------ */
/* 小工具                                                                    */
/* ------------------------------------------------------------------------ */

std::string read_file_text(const std::string& path, std::size_t max_bytes = 65536)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return std::string();
    }
    std::string out;
    out.resize(max_bytes);
    f.read(&out[0], static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    {
        --e;
    }
    return s.substr(b, e - b);
}

std::string read_first_line(const std::string& path)
{
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line))
    {
        return trim(line);
    }
    return std::string();
}

/* 从 "key : value" 形式的 /proc 文本里取第一个匹配值 */
std::string proc_field(const std::string& text, const std::string& key)
{
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line))
    {
        const std::size_t pos = line.find(':');
        if (pos == std::string::npos)
        {
            continue;
        }
        if (trim(line.substr(0, pos)) == key)
        {
            return trim(line.substr(pos + 1));
        }
    }
    return std::string();
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep))
    {
        cur = trim(cur);
        if (!cur.empty())
        {
            out.push_back(cur);
        }
    }
    return out;
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
        }
    }
    return out;
}

bool make_dir_p(const std::string& path)
{
    for (std::size_t i = 1; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            const std::string cur = path.substr(0, i);
            if (!cur.empty() && ::mkdir(cur.c_str(), 0775) != 0 && errno != EEXIST)
            {
                return false;
            }
        }
    }
    return true;
}

std::string timestamp_string()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
    return std::string(buf);
}

std::string iso_time_string()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S%z", &tm_buf);
    return std::string(buf);
}

void pin_to_cpu(int cpu)
{
    if (cpu < 0)
    {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0)
    {
        std::cerr << "[warn] sched_setaffinity(cpu=" << cpu << ") failed\n";
    }
}

/* 本进程消耗的 CPU 时间 (user + sys) */
std::uint64_t self_cpu_ns()
{
    struct rusage ru;
    if (::getrusage(RUSAGE_SELF, &ru) != 0)
    {
        return 0;
    }
    const std::uint64_t u = static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1000000000ull
                            + static_cast<std::uint64_t>(ru.ru_utime.tv_usec) * 1000ull;
    const std::uint64_t s = static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1000000000ull
                            + static_cast<std::uint64_t>(ru.ru_stime.tv_usec) * 1000ull;
    return u + s;
}

/* 清理上一轮遗留的共享内存段, 避免用例之间互相污染 */
void purge_shm(const std::string& tag)
{
    const std::string cmd = "rm -f /dev/shm/*" + tag + "* /dev/shm/__IPC_SHM__*" + tag + "* 2>/dev/null";
    if (std::system(cmd.c_str()) != 0)
    {
        /* 无匹配文件时 rm 返回非 0, 忽略 */
    }
}

/* ------------------------------------------------------------------------ */
/* 硬件与系统配置采集                                                        */
/* ------------------------------------------------------------------------ */

using KV = std::vector<std::pair<std::string, std::string>>;

std::string cpu_cache_summary()
{
    std::ostringstream os;
    bool first = true;
    for (int i = 0; i < 8; ++i)
    {
        const std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(i);
        const std::string level = read_first_line(base + "/level");
        const std::string type = read_first_line(base + "/type");
        const std::string size = read_first_line(base + "/size");
        if (level.empty() || size.empty())
        {
            continue;
        }
        if (!first)
        {
            os << ", ";
        }
        first = false;
        os << "L" << level;
        if (type == "Instruction")
        {
            os << "i";
        }
        else if (type == "Data")
        {
            os << "d";
        }
        os << "=" << size;
    }
    return os.str();
}

std::string cpu_flag_summary(const std::string& cpuinfo)
{
    const std::string hay = " " + proc_field(cpuinfo, "flags") + " ";
    static const char* interesting[] = {"sse4_2",       "avx",         "avx2",         "avx512f",
                                        "constant_tsc", "nonstop_tsc", "tsc_reliable", "rdtscp"};
    std::ostringstream os;
    bool first = true;
    for (const char* f : interesting)
    {
        if (hay.find(std::string(" ") + f + " ") != std::string::npos)
        {
            if (!first)
            {
                os << ",";
            }
            first = false;
            os << f;
        }
    }
    return os.str();
}

/* 统计 (physical id, core id) 组合数 = 物理核心数 */
std::size_t count_physical_cores(const std::string& cpuinfo)
{
    std::vector<std::string> seen;
    std::istringstream is(cpuinfo);
    std::string line;
    std::string phys;
    std::string core;
    auto flush = [&]() {
        if (!phys.empty() && !core.empty())
        {
            const std::string key = phys + "/" + core;
            if (std::find(seen.begin(), seen.end(), key) == seen.end())
            {
                seen.push_back(key);
            }
        }
        phys.clear();
        core.clear();
    };
    while (std::getline(is, line))
    {
        const std::size_t pos = line.find(':');
        if (pos == std::string::npos)
        {
            flush();
            continue;
        }
        const std::string k = trim(line.substr(0, pos));
        const std::string v = trim(line.substr(pos + 1));
        if (k == "physical id")
        {
            phys = v;
        }
        else if (k == "core id")
        {
            core = v;
        }
    }
    flush();
    return seen.size();
}

KV collect_system_info()
{
    KV kv;
    const std::string cpuinfo = read_file_text("/proc/cpuinfo", 262144);
    const std::string meminfo = read_file_text("/proc/meminfo");

    char host[256] = {0};
    ::gethostname(host, sizeof(host) - 1);

    struct utsname un;
    std::string os_name;
    std::string kernel;
    std::string arch;
    if (::uname(&un) == 0)
    {
        os_name = un.sysname;
        kernel = un.release;
        arch = un.machine;
    }

    kv.emplace_back("test_time", iso_time_string());
    kv.emplace_back("hostname", host);
    kv.emplace_back("os", os_name);
    kv.emplace_back("kernel", kernel);
    kv.emplace_back("arch", arch);

    std::string distro;
    {
        const std::string rel = read_file_text("/etc/os-release");
        const std::size_t p = rel.find("PRETTY_NAME=");
        if (p != std::string::npos)
        {
            const std::size_t b = rel.find('"', p);
            const std::size_t e = (b == std::string::npos) ? std::string::npos : rel.find('"', b + 1);
            if (b != std::string::npos && e != std::string::npos)
            {
                distro = rel.substr(b + 1, e - b - 1);
            }
        }
    }
    kv.emplace_back("distro", distro);

    kv.emplace_back("cpu_model", proc_field(cpuinfo, "model name"));
    kv.emplace_back("cpu_vendor", proc_field(cpuinfo, "vendor_id"));
    kv.emplace_back("cpu_logical_cores", std::to_string(::sysconf(_SC_NPROCESSORS_ONLN)));
    kv.emplace_back("cpu_physical_cores", std::to_string(count_physical_cores(cpuinfo)));
    kv.emplace_back("cpu_mhz_now", proc_field(cpuinfo, "cpu MHz"));

    const std::string maxkhz = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!maxkhz.empty())
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(0) << std::strtod(maxkhz.c_str(), nullptr) / 1000.0;
        kv.emplace_back("cpu_mhz_max", os.str());
    }
    kv.emplace_back("cpu_governor", read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"));
    kv.emplace_back("cpu_cache", cpu_cache_summary());
    kv.emplace_back("cpu_flags", cpu_flag_summary(cpuinfo));

    kv.emplace_back("mem_total", proc_field(meminfo, "MemTotal"));
    kv.emplace_back("mem_available", proc_field(meminfo, "MemAvailable"));
    kv.emplace_back("hugepages_thp", read_first_line("/sys/kernel/mm/transparent_hugepage/enabled"));

    struct statvfs vfs;
    if (::statvfs("/dev/shm", &vfs) == 0)
    {
        const double gb =
            static_cast<double>(vfs.f_blocks) * static_cast<double>(vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        std::ostringstream os;
        os << std::fixed << std::setprecision(2) << gb << " GiB";
        kv.emplace_back("dev_shm_size", os.str());
    }

    kv.emplace_back("net_rmem_max", read_first_line("/proc/sys/net/core/rmem_max"));
    kv.emplace_back("net_wmem_max", read_first_line("/proc/sys/net/core/wmem_max"));
    kv.emplace_back("net_rmem_default", read_first_line("/proc/sys/net/core/rmem_default"));
    kv.emplace_back("net_netdev_backlog", read_first_line("/proc/sys/net/core/netdev_max_backlog"));

    const std::string cmdline = read_first_line("/proc/cmdline");
    kv.emplace_back("kernel_cmdline", cmdline);
    kv.emplace_back("isolcpus", cmdline.find("isolcpus") != std::string::npos ? "yes" : "no");

    {
        /* 直接读 /proc/loadavg, 避免依赖 getloadavg 的特性宏 */
        const std::string la = read_first_line("/proc/loadavg");
        const std::size_t sp = la.find(' ');
        kv.emplace_back("loadavg_1min_at_start", sp == std::string::npos ? la : la.substr(0, sp));
    }

#if defined(__VERSION__)
    kv.emplace_back("compiler", __VERSION__);
#else
    kv.emplace_back("compiler", "unknown");
#endif
#if defined(NDEBUG)
    kv.emplace_back("build_type", "release (NDEBUG, -O2)");
#else
    kv.emplace_back("build_type", "debug (assertions on)");
#endif
    kv.emplace_back("cxx_standard", std::to_string(__cplusplus));
    kv.emplace_back("nodelet_fast_path", dzIPC::IsNodeletEnabled() ? "enabled" : "disabled");
    kv.emplace_back("process_model", "multi-process (fork+exec), 收发分属不同进程");

    return kv;
}

/* ------------------------------------------------------------------------ */
/* 统计                                                                      */
/* ------------------------------------------------------------------------ */

struct LatStats
{
    double min_us = 0, max_us = 0, mean_us = 0, stddev_us = 0;
    double p50_us = 0, p90_us = 0, p99_us = 0, p999_us = 0;
    std::size_t count = 0;
};

/* samples 必须已排序; 返回值单位 us */
double percentile_sorted(const std::vector<std::uint64_t>& s, double q)
{
    if (s.empty())
    {
        return 0.0;
    }
    const double idx = q * static_cast<double>(s.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return (static_cast<double>(s[lo]) * (1.0 - frac) + static_cast<double>(s[hi]) * frac) / 1000.0;
}

LatStats compute_stats(std::vector<std::uint64_t>& samples)
{
    LatStats st;
    if (samples.empty())
    {
        return st;
    }
    std::sort(samples.begin(), samples.end());
    st.count = samples.size();
    st.min_us = static_cast<double>(samples.front()) / 1000.0;
    st.max_us = static_cast<double>(samples.back()) / 1000.0;

    long double sum = 0;
    for (std::uint64_t v : samples)
    {
        sum += static_cast<long double>(v);
    }
    const long double mean = sum / static_cast<long double>(samples.size());
    long double var = 0;
    for (std::uint64_t v : samples)
    {
        const long double d = static_cast<long double>(v) - mean;
        var += d * d;
    }
    var /= static_cast<long double>(samples.size());

    st.mean_us = static_cast<double>(mean) / 1000.0;
    st.stddev_us = std::sqrt(static_cast<double>(var)) / 1000.0;
    st.p50_us = percentile_sorted(samples, 0.50);
    st.p90_us = percentile_sorted(samples, 0.90);
    st.p99_us = percentile_sorted(samples, 0.99);
    st.p999_us = percentile_sorted(samples, 0.999);
    return st;
}

/* ------------------------------------------------------------------------ */
/* 用例结果                                                                  */
/* ------------------------------------------------------------------------ */

struct CaseResult
{
    std::string case_id;
    std::string pattern;     // pubsub / sercli
    std::string transport;   // shm / socket
    std::string test_kind;   // lat / tput / rtt
    std::size_t payload_bytes = 0;
    std::size_t bytes_per_op = 0;
    double target_rate_hz = 0;   // 0 = 全速
    double duration_sec = 0;
    std::uint64_t sent = 0;
    std::uint64_t recv = 0;
    std::uint64_t failed = 0;
    double cpu_cores = 0;
    bool samples_truncated = false;
    bool ok = false;
    std::string note;
    LatStats lat;
    std::vector<std::uint64_t> samples;

    /* 阶段 0/1: 分片诊断与节流设置。frag_valid = false 表示本用例未采集。 */
    bool frag_valid = false;
    std::uint64_t frag_gap_hist[7]{};
    std::uint64_t frag_msgs_complete = 0;
    std::uint64_t frag_msgs_incomplete = 0;
    std::uint64_t frag_expected = 0;
    std::uint64_t frag_missing = 0;
    std::uint64_t frag_gaps_total = 0;
    std::uint64_t frag_gap_max = 0;
    /* NACK 编码选择。两个 sent 都为 0 时无法区分"零丢包"与"位图从不触发",
     * 所以判读时要结合 frag_missing 看。 */
    std::uint64_t nack_explicit_sent = 0;
    std::uint64_t nack_bitmap_sent = 0;
    std::uint64_t nack_explicit_truncated = 0;
    std::uint64_t nack_bitmap_truncated = 0;
    std::size_t rate_limit_bps = 0;

    /* 分片级丢包率。与 loss_pct() 的区别: 后者是消息级, 前者是分片级。
     * 若丢包独立, 两者应满足 msg_ok = (1 - frag_loss)^page_cnt。 */
    double frag_loss_pct() const
    {
        return frag_expected ? 100.0 * static_cast<double>(frag_missing) / static_cast<double>(frag_expected) : 0.0;
    }
    /* 平均空洞长度: ≈1 说明随机丢包, 显著 >1 说明突发丢包。 */
    double mean_gap_len() const
    {
        return frag_gaps_total ? static_cast<double>(frag_missing) / static_cast<double>(frag_gaps_total) : 0.0;
    }

    std::uint64_t lost() const { return sent > recv ? sent - recv : 0; }
    double loss_pct() const
    {
        return sent ? 100.0 * static_cast<double>(lost()) / static_cast<double>(sent) : 0.0;
    }
    double throughput_msg_s() const
    {
        return duration_sec > 0 ? static_cast<double>(recv) / duration_sec : 0.0;
    }
    double throughput_mb_s() const
    {
        return throughput_msg_s() * static_cast<double>(bytes_per_op) / (1024.0 * 1024.0);
    }
};

/* ------------------------------------------------------------------------ */
/* 跨进程控制块 (POSIX 共享内存, fork+exec 后仍可按名字打开)                  */
/* ------------------------------------------------------------------------ */

struct SharedCtl
{
    std::atomic<std::int32_t> peer_ready;      // 对端进程已就绪
    std::atomic<std::int32_t> peer_failed;     // 对端初始化失败
    std::atomic<std::int32_t> peer_got_traffic;   // 对端已实际收到探测帧(端到端连通性)
    std::atomic<std::int32_t> phase;           // 0=warmup 1=measure 2=stop
    std::atomic<std::uint64_t> recv_measured;  // 测量期收到的消息数
    std::atomic<std::uint64_t> sample_n;
    std::atomic<std::uint64_t> child_cpu_ns;
    std::atomic<std::int32_t> truncated;

    /* 阶段 0: 订阅端子进程回传的分片空洞统计。
     * 位图只在库内部的重组栈帧上存在, 只能由订阅进程读出后经此回传。 */
    std::atomic<std::uint64_t> frag_gap_hist[7];
    std::atomic<std::uint64_t> frag_msgs_complete;
    std::atomic<std::uint64_t> frag_msgs_incomplete;
    std::atomic<std::uint64_t> frag_expected;
    std::atomic<std::uint64_t> frag_missing;
    std::atomic<std::uint64_t> frag_gaps_total;
    std::atomic<std::uint64_t> frag_gap_max;
    std::atomic<std::uint64_t> nack_explicit_sent;
    std::atomic<std::uint64_t> nack_bitmap_sent;
    std::atomic<std::uint64_t> nack_explicit_truncated;
    std::atomic<std::uint64_t> nack_bitmap_truncated;

    std::uint64_t samples[kMaxSamples];
};

/* 父进程: 创建并映射; 子进程: 按名字打开并映射 */
SharedCtl* ctl_map(const std::string& name, bool create)
{
    const int flags = create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    const int fd = ::shm_open(name.c_str(), flags, 0600);
    if (fd < 0)
    {
        std::cerr << "[fatal] shm_open(" << name << ") failed: " << std::strerror(errno) << "\n";
        return nullptr;
    }
    if (create && ::ftruncate(fd, static_cast<off_t>(sizeof(SharedCtl))) != 0)
    {
        std::cerr << "[fatal] ftruncate failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return nullptr;
    }
    void* p = ::mmap(nullptr, sizeof(SharedCtl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED)
    {
        std::cerr << "[fatal] mmap SharedCtl failed: " << std::strerror(errno) << "\n";
        return nullptr;
    }

    if (create)
    {
        /* 默认初始化(不加括号)以构造 atomic 成员, 同时避免触碰 16MiB 样本数组 */
        SharedCtl* c = new (p) SharedCtl;
        c->peer_ready.store(0);
        c->peer_failed.store(0);
        c->peer_got_traffic.store(0);
        c->phase.store(0);
        c->recv_measured.store(0);
        c->sample_n.store(0);
        c->child_cpu_ns.store(0);
        c->truncated.store(0);
        for (int i = 0; i < 7; ++i)
        {
            c->frag_gap_hist[i].store(0);
        }
        c->frag_msgs_complete.store(0);
        c->frag_msgs_incomplete.store(0);
        c->frag_expected.store(0);
        c->frag_missing.store(0);
        c->frag_gaps_total.store(0);
        c->frag_gap_max.store(0);
        c->nack_explicit_sent.store(0);
        c->nack_bitmap_sent.store(0);
        c->nack_explicit_truncated.store(0);
        c->nack_bitmap_truncated.store(0);
        return c;
    }
    return static_cast<SharedCtl*>(p);
}

void ctl_unmap(SharedCtl* c)
{
    if (c)
    {
        ::munmap(c, sizeof(SharedCtl));
    }
}

/* ------------------------------------------------------------------------ */
/* 配置                                                                      */
/* ------------------------------------------------------------------------ */

struct Config
{
    std::string out_dir;
    std::vector<std::size_t> payloads{64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};
    double duration = 3.0;
    double warmup = 0.5;
    double lat_rate = 1000.0;
    std::vector<std::string> cases{"pubsub_shm", "pubsub_socket", "sercli_shm", "sercli_socket"};
    bool skip_tput = false;
    int pin_tx = -1;
    int pin_rx = -1;
    std::size_t queue_size = 1024;
    /* domain_id 必须小: UDP 端口 = 11451 + domain_id * (hash(topic) % 10000), 需 <= 65535。
     * domain_id = 1 时对任意 topic 名都安全; 更大的值会让相当比例的 topic 名算出越界端口。 */
    std::size_t domain = 1;
    /* 发送语义: 默认 publish_blocking(有界等待)。
     * publish()/best-effort 在 SHM 上是 tm=0 的 no_member_try_send, 队列一满就 force_push。
     * force_push 已不再强制断开慢订阅者(见 src/libipc/prod_cons.h), 但覆写与 pop() 的
     * 拷贝之间仍无互斥, 订阅端仍可能读到撕裂数据。测吞吐上限用 blocking 更可信。 */
    bool best_effort = false;
    std::uint64_t publish_timeout_ms = 100;

    /* 阶段 1: 发送节流上限, B/s, 0 = 不限速(默认, 与改动前行为一致)。
     * 仅对 socket 多分片消息有意义。用于验证"大包丢包是接收缓冲溢出"这一假设。 */
    std::size_t rate_limit_bps = 0;
    /* 阶段 0: 采集分片空洞长度直方图, 区分随机丢包与突发丢包。 */
    bool frag_stats = false;

    /* 仅子进程使用 */
    std::string role;
    std::string ctl_name;
    std::string topic;
    std::string transport;
    std::size_t payload = 0;

    bool has_case(const std::string& c) const
    {
        return std::find(cases.begin(), cases.end(), c) != cases.end();
    }
};

dzIPC::IPCType type_of(const std::string& transport)
{
    return transport == "shm" ? IPC_SHM : IPC_SOCKET;
}

/* UDP 端口由 topic 名与 domain_id 计算, 越界时库会直接抛异常。
 * 在拉起子进程之前先算一遍, 把崩溃变成一条清晰的用例失败信息。 */
bool udp_port_ok(const std::string& topic, std::size_t domain, std::string& err)
{
    try
    {
        const uint16_t port = dzIPC::common::udp_discovery_port_calculate(topic, static_cast<int>(domain));
        (void)port;
        return true;
    }
    catch (const std::exception& e)
    {
        err = std::string("UDP 端口计算越界 (domain_id=") + std::to_string(domain) + ", topic=" + topic
              + "): " + e.what();
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* payload 构造                                                              */
/* ------------------------------------------------------------------------ */

/* TestMsg 布局: data2 固定 4 个 int32 承载 seq 与时间戳, 其余用 data1 填到目标大小 */
std::size_t payload_doubles(std::size_t payload_bytes)
{
    constexpr std::size_t kFixed = 4 * sizeof(std::int32_t) + sizeof(bool);
    if (payload_bytes <= kFixed + sizeof(double))
    {
        return 1;
    }
    return (payload_bytes - kFixed) / sizeof(double);
}

std::shared_ptr<dzIPC::Msg::TestMsg> make_topic_payload(std::size_t payload_bytes)
{
    auto m = std::make_shared<dzIPC::Msg::TestMsg>();
    m->data1.assign(payload_doubles(payload_bytes), 1.0);
    m->data2.assign(4, 0);
    m->data4 = true;
    return m;
}

/* ------------------------------------------------------------------------ */
/* 子进程: 拉起对端                                                          */
/* ------------------------------------------------------------------------ */

pid_t spawn_peer(const std::vector<std::string>& args)
{
    const pid_t pid = ::fork();
    if (pid != 0)
    {
        return pid;   // 父进程(或 fork 失败)
    }

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("dzipc_perf_benchmark"));
    for (const auto& a : args)
    {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    /* exec 一个全新的进程映像: 不继承父进程里 dzIPC 的任何全局状态或线程 */
    ::execv("/proc/self/exe", argv.data());
    std::perror("execv(/proc/self/exe)");
    ::_exit(127);
}

/* 子进程角色: pub-sub 订阅端 */
int run_role_subscriber(const Config& cfg)
{
    SharedCtl* ctl = ctl_map(cfg.ctl_name, false);
    if (ctl == nullptr)
    {
        return 1;
    }
    pin_to_cpu(cfg.pin_rx);

    /* 阶段 0: 分片空洞采集必须在订阅进程里开启 —— 重组位图是库内部状态。
     * 只对 socket 有意义: SHM 传输没有分片。 */
    if (cfg.frag_stats && cfg.transport != "shm")
    {
        dzIPC::socket::set_fragment_loss_tracking(true);
    }

    /* 订阅端不发包, 设这个值纯粹是给 recv_chunk_common 推算组装窗口用的
     * (见 rate_limited_transit_ms)。 */
    if (cfg.rate_limit_bps > 0 && cfg.transport != "shm")
    {
        dzIPC::socket::set_socket_rate_limit_bps(cfg.rate_limit_bps);
    }

    dzIPC::SubscriberIPCPtr sub;
    try
    {
        auto tpl = std::make_shared<dzIPC::TopicData>(make_topic_payload(cfg.payload), kMsgId);
        sub = dzIPC::SubscriberIPCPtrMake(tpl, cfg.topic, cfg.domain, cfg.queue_size, type_of(cfg.transport),
                                          false);
        sub->InitChannel("perf_bench");
    }
    catch (const std::exception& e)
    {
        std::cerr << "[sub] init failed: " << e.what() << "\n";
        ctl->peer_failed.store(1, std::memory_order_release);
        ctl_unmap(ctl);
        return 1;
    }

    auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(kMsgId);
    ctl->peer_ready.store(1, std::memory_order_release);

    std::uint64_t local_recv = 0;
    try
    {
        while (ctl->phase.load(std::memory_order_acquire) != 2)
        {
            if (!sub->try_get(rcv))
            {
                continue;
            }
            auto d = rcv->topic()->msgcast<dzIPC::Msg::TestMsg>();
            if (!d || d->data2.size() < 4)
            {
                continue;
            }
            /* 收到任意一帧(含预热/探测帧)即向发布端确认链路已通。
             * socket 传输没有反向发现通道, 只能靠这个端到端信号。 */
            ctl->peer_got_traffic.store(1, std::memory_order_release);
            const std::uint64_t hi = static_cast<std::uint32_t>(d->data2[1]);
            const std::uint64_t lo = static_cast<std::uint32_t>(d->data2[2]);
            const std::uint64_t ts = (hi << 32) | lo;
            if (ts == 0)
            {
                continue;   // 预热消息, 不计入统计
            }
            const std::uint64_t rx = now_ns();
            ++local_recv;
            const std::uint64_t idx = ctl->sample_n.fetch_add(1, std::memory_order_relaxed);
            if (idx < kMaxSamples)
            {
                ctl->samples[idx] = (rx > ts) ? (rx - ts) : 0;
            }
            else
            {
                ctl->truncated.store(1, std::memory_order_relaxed);
            }
        }
    }
    catch (const std::exception& e)
    {
        /* 发布端 force_push 覆写正在读的槽位时, 反序列化会读到撕裂数据并抛异常。
         * 记录已收到的数量, 让父进程能给出有意义的结果而不是整体崩溃。 */
        std::cerr << "[sub] receive exception (SHM ring slot likely overwritten): " << e.what() << "\n";
        ctl->peer_failed.store(2, std::memory_order_release);
    }

    ctl->recv_measured.store(local_recv, std::memory_order_release);
    ctl->child_cpu_ns.store(self_cpu_ns(), std::memory_order_release);

    if (cfg.frag_stats && cfg.transport != "shm")
    {
        const auto fs = dzIPC::socket::get_fragment_loss_stats();
        for (std::size_t i = 0; i < dzIPC::socket::FragmentLossStats::kGapBuckets; ++i)
        {
            ctl->frag_gap_hist[i].store(fs.gap_hist[i], std::memory_order_relaxed);
        }
        ctl->frag_msgs_complete.store(fs.messages_complete, std::memory_order_relaxed);
        ctl->frag_msgs_incomplete.store(fs.messages_incomplete, std::memory_order_relaxed);
        ctl->frag_expected.store(fs.fragments_expected, std::memory_order_relaxed);
        ctl->frag_missing.store(fs.fragments_missing, std::memory_order_relaxed);
        ctl->frag_gaps_total.store(fs.gaps_total, std::memory_order_relaxed);
        ctl->nack_explicit_sent.store(fs.nack_explicit_sent, std::memory_order_relaxed);
        ctl->nack_bitmap_sent.store(fs.nack_bitmap_sent, std::memory_order_relaxed);
        ctl->nack_explicit_truncated.store(fs.nack_explicit_truncated, std::memory_order_relaxed);
        ctl->nack_bitmap_truncated.store(fs.nack_bitmap_truncated, std::memory_order_relaxed);
        /* gap_max 用 release 收尾, 与父进程读侧的 acquire 配对, 保证上面这些
         * relaxed 写在父进程看到 gap_max 时都已可见。新增字段必须写在它之前。 */
        ctl->frag_gap_max.store(fs.gap_max, std::memory_order_release);
    }

    sub.reset();
    ctl_unmap(ctl);
    return 0;
}

/* 子进程角色: ser-cli 服务端 */
int run_role_server(const Config& cfg)
{
    SharedCtl* ctl = ctl_map(cfg.ctl_name, false);
    if (ctl == nullptr)
    {
        return 1;
    }
    pin_to_cpu(cfg.pin_rx);

    /* 阶段 1: 服务端发送响应时同样限速 —— ser-cli 两个方向都会发大包。 */
    if (cfg.rate_limit_bps > 0 && cfg.transport != "shm")
    {
        dzIPC::socket::set_socket_rate_limit_bps(cfg.rate_limit_bps);
    }

    dzIPC::ServerIPCPtr server;
    try
    {
        dzIPC::ServerDataPtr msg =
            dzIPC::ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest,
                                     dzIPC::Srv::RequestResponseTestResponse>(kSrvMsgId);

        server = dzIPC::ServerIPCPtrMake(
            cfg.topic, msg,
            [](dzIPC::ServerDataPtr& m)
            {
                auto req = m->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
                auto res = m->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
                /* 回显等长响应, 使请求/响应两个方向的负载对称 */
                res->response.assign(req->request.begin(), req->request.end());
            },
            cfg.domain, type_of(cfg.transport), false);
        server->InitChannel("perf_bench");
    }
    catch (const std::exception& e)
    {
        std::cerr << "[srv] init failed: " << e.what() << "\n";
        ctl->peer_failed.store(1, std::memory_order_release);
        ctl_unmap(ctl);
        return 1;
    }

    ctl->peer_ready.store(1, std::memory_order_release);

    while (ctl->phase.load(std::memory_order_acquire) != 2)
    {
        std::this_thread::sleep_for(milliseconds(2));
    }

    ctl->child_cpu_ns.store(self_cpu_ns(), std::memory_order_release);
    server.reset();
    ctl_unmap(ctl);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* pub-sub 用例                                                              */
/* ------------------------------------------------------------------------ */

CaseResult run_pubsub_case(const Config& cfg, const std::string& transport, std::size_t payload_bytes, bool max_rate)
{
    const dzIPC::IPCType type = type_of(transport);

    CaseResult r;
    r.pattern = "pubsub";
    r.transport = transport;
    r.test_kind = max_rate ? "tput" : "lat";
    r.payload_bytes = payload_bytes;
    r.bytes_per_op = payload_bytes;
    r.target_rate_hz = max_rate ? 0.0 : cfg.lat_rate;
    r.case_id = r.pattern + "_" + r.transport + "_" + std::to_string(payload_bytes) + "B_" + r.test_kind;

    const std::string topic =
        "perfbench_" + transport + "_" + std::to_string(payload_bytes) + "_" + (max_rate ? "t" : "l");
    purge_shm(topic);

    /* socket 路径的 UDP 端口由 topic 名与 domain_id 推算, 越界会在构造时抛异常 */
    if (type == IPC_SOCKET)
    {
        std::string err;
        if (!udp_port_ok(topic, cfg.domain, err))
        {
            r.note = err;
            return r;
        }
    }

    const std::string ctl_name = "/dzipc_perf_" + std::to_string(::getpid()) + "_" + r.case_id;
    ::shm_unlink(ctl_name.c_str());   // 清掉可能的残留
    SharedCtl* ctl = ctl_map(ctl_name, true);
    if (ctl == nullptr)
    {
        r.note = "shm_open failed";
        return r;
    }

    auto cleanup = [&]() {
        ctl_unmap(ctl);
        ::shm_unlink(ctl_name.c_str());
        purge_shm(topic);
    };

    std::vector<std::string> sub_args{"--role=sub",
                                      "--ctl=" + ctl_name,
                                      "--topic=" + topic,
                                      "--transport=" + transport,
                                      "--payload=" + std::to_string(payload_bytes),
                                      "--queue=" + std::to_string(cfg.queue_size),
                                      "--domain=" + std::to_string(cfg.domain),
                                      "--pin=" + std::to_string(cfg.pin_tx) + "," + std::to_string(cfg.pin_rx)};
    if (cfg.frag_stats)
    {
        sub_args.push_back("--frag-stats");
    }
    /* 订阅端只收不发, 看似不需要限速配置 —— 但 recv_chunk_common 要用限速值
     * 推算"这条消息在线上至少要跑多久", 才能把组装窗口放宽到覆盖传输时间。
     * 不传的话订阅端算出 transit=0, 沿用 tm=50ms 的窗口, 而 1 MB @ 20 MB/s
     * 传输就要 50.05ms —— 每条消息都在收全的前一刻超时, 表现为一条都收不到。 */
    if (cfg.rate_limit_bps > 0 && transport != "shm")
    {
        sub_args.push_back("--rate-limit=" + std::to_string(cfg.rate_limit_bps));
    }
    const pid_t pid = spawn_peer(sub_args);
    if (pid < 0)
    {
        r.note = "fork failed";
        cleanup();
        return r;
    }

    /* ---------------- 父进程: 发布端 ---------------- */
    pin_to_cpu(cfg.pin_tx);

    /* 阶段 1: 节流在发布进程生效。设在 InitChannel 之前, 避免中途改速率。 */
    if (cfg.rate_limit_bps > 0 && transport != "shm")
    {
        dzIPC::socket::set_socket_rate_limit_bps(cfg.rate_limit_bps);
    }

    {
        const auto dl = Clock::now() + seconds(30);
        while (!ctl->peer_ready.load(std::memory_order_acquire)
               && !ctl->peer_failed.load(std::memory_order_acquire) && Clock::now() < dl)
        {
            std::this_thread::sleep_for(milliseconds(5));
        }
    }
    if (ctl->peer_failed.load(std::memory_order_acquire))
    {
        r.note = "subscriber init failed (see [sub] error above)";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }
    if (!ctl->peer_ready.load(std::memory_order_acquire))
    {
        r.note = "subscriber not ready (timeout)";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }

    dzIPC::PublisherIPCPtr pub;
    try
    {
        auto tpl = std::make_shared<dzIPC::TopicData>(make_topic_payload(payload_bytes), kMsgId);
        pub = dzIPC::PublisherIPCPtrMake(tpl, topic, cfg.domain, type, false);
        pub->InitChannel("perf_bench");
    }
    catch (const std::exception& e)
    {
        r.note = std::string("publisher init failed: ") + e.what();
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }

    auto pmsg = std::make_shared<dzIPC::TopicData>(make_topic_payload(payload_bytes), kMsgId);
    auto pd = pmsg->topic()->msgcast<dzIPC::Msg::TestMsg>();

    /* 发送入口, 按传输方式选择语义 —— 两种传输的 publish_blocking 不是一回事:
     *  - SHM:    publish_blocking = 对环形队列有界等待, 超时返回 false。用它是为了
     *            避开 best-effort 在队列满时 force_push 覆写订阅者正在读的槽位。
     *  - SOCKET: publish_blocking = Reliable + CRC32C, 每次发送都要等订阅端 ACK。
     *            chunk_send_ex() 里免等 ACK 的快路径只对 "BestEffort 且单包" 生效
     *            (data_rev.cc:652), Reliable 一律落进 ACK 等待循环, 拿不到 ACK 就
     *            FailedTimeout。socket 没有共享环形队列, 也就没有 force_push 问题;
     *            用 Reliable 既测错了对象(测的是 ACK 往返而非链路单向时延), 又和
     *            test_dzipc.cpp 走的路径不一致。所以 socket 固定 publish()/BestEffort。
     * --best-effort 可强制两种传输都用 publish()。 */
    const bool use_blocking = !cfg.best_effort ;
    auto do_publish = [&]() -> bool {
        if (use_blocking)
        {
            return pub->publish_blocking(pmsg->topic(), cfg.publish_timeout_ms);
        }
        return pub->publish(pmsg->topic());
    };

    /* ---- 端到端连通性握手 ----
     * 不能用 pub->has_subscribed(): socket_pub_ipc::subscribed_ 原本只声明不赋值
     * (恒 false), 现在虽已由 discovery_loop() 从 IpcInfoPool 回填, 但那只覆盖走
     * dzIPC 注册的订阅者。改为持续发探测帧(时间戳置 0, 订阅端不计入统计), 直到
     * 订阅端通过共享控制块回报收到为止 —— 该判据对两种传输都成立, 且证明的是
     * 数据面真的通了, 比任何控制面计数都强。*/
    std::uint64_t probe_ok = 0;
    std::uint64_t probe_fail = 0;
    {
        /* 修正 3: 握手探测期间不限速。
         *
         * 探测帧和数据帧一样是全尺寸 payload, 1 MB @ 20 MB/s 单帧就要 50ms 传输,
         * 而探测循环每 10ms 发一帧 —— 发送速率远超限速允许的值, 令牌桶把每次
         * publish 拖成阻塞, 订阅端在探测窗口内一帧都组装不完, 握手必然超时。
         * (实测: probes sent ok=325 failed=0 但订阅端 peer_got_traffic 始终为 0。)
         *
         * 握手测的是"链路通不通", 不是稳态吞吐, 没有限速的必要;
         * 限速在握手成功后、预热开始前恢复。 */
        const bool throttled = cfg.rate_limit_bps > 0 && transport != "shm";
        if (throttled)
        {
            dzIPC::socket::set_socket_rate_limit_bps(0);
        }

        std::int32_t probe_seq = -1;
        const auto dl = Clock::now() + seconds(type == IPC_SHM ? 10 : 20);
        while (!ctl->peer_got_traffic.load(std::memory_order_acquire)
               && !ctl->peer_failed.load(std::memory_order_acquire) && Clock::now() < dl)
        {
            pd->data2[0] = probe_seq--;
            pd->data2[1] = 0;
            pd->data2[2] = 0;
            if (do_publish())
            {
                ++probe_ok;
            }
            else
            {
                ++probe_fail;
            }
            std::this_thread::sleep_for(milliseconds(10));
        }

        if (throttled)
        {
            dzIPC::socket::set_socket_rate_limit_bps(cfg.rate_limit_bps);
        }
    }
    if (!ctl->peer_got_traffic.load(std::memory_order_acquire))
    {
        /* 把探测帧的发送结果带出来, 区分"发不出去"和"发出去了但对端收不到" */
        r.note = "handshake timeout: subscriber received no probe frame (probes sent ok="
                 + std::to_string(probe_ok) + " failed=" + std::to_string(probe_fail) + "; "
                 + (probe_ok == 0 ? "publisher sent nothing -> check the send path"
                                  : "publisher sent but peer got nothing -> check multicast "
                                    "delivery / subscriber receive path")
                 + ")";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        pub.reset();
        cleanup();
        return r;
    }
    std::this_thread::sleep_for(milliseconds(type == IPC_SHM ? 100 : 300));

    /* ---- 预热: 时间戳置 0, 订阅端会跳过 ---- */
    {
        std::int32_t wseq = 0;
        const auto wu_end = Clock::now() + duration<double>(cfg.warmup);
        while (Clock::now() < wu_end)
        {
            pd->data2[0] = wseq++;
            pd->data2[1] = 0;
            pd->data2[2] = 0;
            pd->data2[3] = 0;
            do_publish();
            std::this_thread::sleep_for(microseconds(200));
        }
        std::this_thread::sleep_for(milliseconds(150));   // 等订阅端排空预热消息
    }

    /* ---- 测量 ---- */
    const std::uint64_t cpu_before = self_cpu_ns();
    ctl->phase.store(1, std::memory_order_release);

    const auto t0 = Clock::now();
    const auto deadline = t0 + duration<double>(cfg.duration);
    const auto period = max_rate ? nanoseconds(0)
                                 : nanoseconds(static_cast<std::int64_t>(1e9 / std::max(1.0, cfg.lat_rate)));
    auto next_send = t0;
    std::int32_t seq = 1;

    while (Clock::now() < deadline)
    {
        if (!max_rate)
        {
            /* 速率控制: 距离下一拍远则 sleep, 近则自旋, 保证节拍稳定 */
            for (;;)
            {
                const auto now = Clock::now();
                if (now >= next_send)
                {
                    break;
                }
                const auto left = next_send - now;
                if (left > microseconds(300))
                {
                    std::this_thread::sleep_for(left - microseconds(200));
                }
            }
            next_send += period;
        }

        const std::uint64_t ts = now_ns();
        pd->data2[0] = seq++;
        pd->data2[1] = static_cast<std::int32_t>(static_cast<std::uint32_t>(ts >> 32));
        pd->data2[2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(ts & 0xFFFFFFFFull));
        pd->data2[3] = 0;
        if (do_publish())
        {
            ++r.sent;
        }
        else
        {
            ++r.failed;   // 队列满且等待超时, 消息未进入传输层
        }
    }
    const double elapsed = duration<double>(Clock::now() - t0).count();
    const std::uint64_t cpu_after = self_cpu_ns();

    /* 让在途消息落地后再让订阅端退出 */
    std::this_thread::sleep_for(milliseconds(type == IPC_SHM ? 250 : 500));
    ctl->phase.store(2, std::memory_order_release);
    ::waitpid(pid, nullptr, 0);

    r.duration_sec = elapsed;
    r.recv = ctl->recv_measured.load(std::memory_order_acquire);
    r.samples_truncated = ctl->truncated.load(std::memory_order_acquire) != 0;

    const std::uint64_t n = std::min<std::uint64_t>(ctl->sample_n.load(std::memory_order_acquire), kMaxSamples);
    r.samples.assign(ctl->samples, ctl->samples + n);
    r.lat = compute_stats(r.samples);

    r.rate_limit_bps = (transport != "shm") ? cfg.rate_limit_bps : 0;
    if (cfg.frag_stats && transport != "shm")
    {
        r.frag_gap_max = ctl->frag_gap_max.load(std::memory_order_acquire);
        for (int i = 0; i < 7; ++i)
        {
            r.frag_gap_hist[i] = ctl->frag_gap_hist[i].load(std::memory_order_relaxed);
        }
        r.frag_msgs_complete = ctl->frag_msgs_complete.load(std::memory_order_relaxed);
        r.frag_msgs_incomplete = ctl->frag_msgs_incomplete.load(std::memory_order_relaxed);
        r.frag_expected = ctl->frag_expected.load(std::memory_order_relaxed);
        r.frag_missing = ctl->frag_missing.load(std::memory_order_relaxed);
        r.frag_gaps_total = ctl->frag_gaps_total.load(std::memory_order_relaxed);
        r.nack_explicit_sent = ctl->nack_explicit_sent.load(std::memory_order_relaxed);
        r.nack_bitmap_sent = ctl->nack_bitmap_sent.load(std::memory_order_relaxed);
        r.nack_explicit_truncated = ctl->nack_explicit_truncated.load(std::memory_order_relaxed);
        r.nack_bitmap_truncated = ctl->nack_bitmap_truncated.load(std::memory_order_relaxed);
        /* 单分片消息不进入统计路径(page_cnt == 1 直接返回), 因此 expected == 0
         * 说明本用例的 payload 没有触发分片, 不是采集失败。 */
        r.frag_valid = r.frag_expected > 0;
    }

    const std::uint64_t total_cpu = (cpu_after - cpu_before) + ctl->child_cpu_ns.load(std::memory_order_acquire);
    r.cpu_cores = elapsed > 0 ? static_cast<double>(total_cpu) / 1e9 / elapsed : 0.0;
    r.ok = r.recv > 0;
    if (!r.ok)
    {
        r.note = "no message received";
    }
    if (ctl->peer_failed.load(std::memory_order_acquire) == 2)
    {
        r.note = "subscriber deserialization exception: SHM slot overwritten by force_push (torn data)";
        r.ok = false;
    }

    pub.reset();
    cleanup();
    return r;
}

/* ------------------------------------------------------------------------ */
/* ser-cli 用例                                                              */
/* ------------------------------------------------------------------------ */

CaseResult run_sercli_case(const Config& cfg, const std::string& transport, std::size_t payload_bytes)
{
    const dzIPC::IPCType type = type_of(transport);

    CaseResult r;
    r.pattern = "sercli";
    r.transport = transport;
    r.test_kind = "rtt";
    r.payload_bytes = payload_bytes;
    r.bytes_per_op = payload_bytes * 2;   // 一次调用在两个方向各搬运一次 payload
    r.target_rate_hz = 0.0;               // 同步调用, 背靠背
    r.case_id = r.pattern + "_" + r.transport + "_" + std::to_string(payload_bytes) + "B_rtt";

    const std::string service = "perfsrv_" + transport + "_" + std::to_string(payload_bytes);
    purge_shm(service);

    if (type == IPC_SOCKET)
    {
        std::string err;
        if (!udp_port_ok(service, cfg.domain, err))
        {
            r.note = err;
            return r;
        }
    }

    const std::string ctl_name = "/dzipc_perf_" + std::to_string(::getpid()) + "_" + r.case_id;
    ::shm_unlink(ctl_name.c_str());
    SharedCtl* ctl = ctl_map(ctl_name, true);
    if (ctl == nullptr)
    {
        r.note = "shm_open failed";
        return r;
    }

    auto cleanup = [&]() {
        ctl_unmap(ctl);
        ::shm_unlink(ctl_name.c_str());
        purge_shm(service);
    };

    std::vector<std::string> srv_args{"--role=srv",
                                      "--ctl=" + ctl_name,
                                      "--topic=" + service,
                                      "--transport=" + transport,
                                      "--payload=" + std::to_string(payload_bytes),
                                      "--domain=" + std::to_string(cfg.domain),
                                      "--pin=" + std::to_string(cfg.pin_tx) + "," + std::to_string(cfg.pin_rx)};
    /* 服务端也要限速: ser-cli 双向都会发大包, 只限客户端等于只治了一半。 */
    if (cfg.rate_limit_bps > 0 && transport != "shm")
    {
        srv_args.push_back("--rate-limit=" + std::to_string(cfg.rate_limit_bps));
    }
    const pid_t pid = spawn_peer(srv_args);
    if (pid < 0)
    {
        r.note = "fork failed";
        cleanup();
        return r;
    }

    /* ---------------- 父进程: 客户端 ---------------- */
    pin_to_cpu(cfg.pin_tx);

    /* 阶段 0/1: ser-cli 的客户端自己也收响应, 统计直接在本进程采集, 不走 SharedCtl。 */
    if (cfg.frag_stats && transport != "shm")
    {
        dzIPC::socket::reset_fragment_loss_stats();
        dzIPC::socket::set_fragment_loss_tracking(true);
    }
    if (cfg.rate_limit_bps > 0 && transport != "shm")
    {
        dzIPC::socket::set_socket_rate_limit_bps(cfg.rate_limit_bps);
    }
    {
        const auto dl = Clock::now() + seconds(30);
        while (!ctl->peer_ready.load(std::memory_order_acquire)
               && !ctl->peer_failed.load(std::memory_order_acquire) && Clock::now() < dl)
        {
            std::this_thread::sleep_for(milliseconds(5));
        }
    }
    if (ctl->peer_failed.load(std::memory_order_acquire))
    {
        r.note = "server init failed (see [srv] error above)";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }
    if (!ctl->peer_ready.load(std::memory_order_acquire))
    {
        r.note = "server not ready (timeout)";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }

    dzIPC::ServerDataPtr msg =
        dzIPC::ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            kSrvMsgId);
    dzIPC::ClientIPCPtr client;
    try
    {
        client = dzIPC::ClientIPCPtrMake(service, msg, cfg.domain, type, false);
        client->InitChannel("perf_bench");
    }
    catch (const std::exception& e)
    {
        r.note = std::string("client init failed: ") + e.what();
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return r;
    }

    {
        const auto dl = Clock::now() + seconds(type == IPC_SHM ? 10 : 20);
        while (!client->handshake_completed() && Clock::now() < dl)
        {
            std::this_thread::sleep_for(milliseconds(10));
        }
    }
    if (!client->handshake_completed())
    {
        r.note = "client handshake timeout";
        ctl->phase.store(2);
        ::waitpid(pid, nullptr, 0);
        client.reset();
        cleanup();
        return r;
    }

    /* send_request 只读取 request, 只覆写 response, 因此请求负载填一次即可 */
    const std::size_t nd = std::max<std::size_t>(1, payload_bytes / sizeof(double));
    auto req = msg->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
    req->request.assign(nd, 1.5);

    constexpr std::uint64_t kRecvTimeoutMs = 2000;

    /* ---- 预热 ---- */
    {
        const auto wu_end = Clock::now() + duration<double>(cfg.warmup);
        while (Clock::now() < wu_end)
        {
            client->send_request(msg, kRecvTimeoutMs);
        }
    }

    /* ---- 测量 ---- */
    const std::uint64_t cpu_before = self_cpu_ns();
    ctl->phase.store(1, std::memory_order_release);

    std::vector<std::uint64_t> lat;
    lat.reserve(1 << 16);

    const auto t0 = Clock::now();
    const auto deadline = t0 + duration<double>(cfg.duration);
    while (Clock::now() < deadline)
    {
        const std::uint64_t a = now_ns();
        const bool ok = client->send_request(msg, kRecvTimeoutMs);
        const std::uint64_t b = now_ns();
        ++r.sent;
        if (!ok)
        {
            ++r.failed;
            continue;
        }
        ++r.recv;
        if (lat.size() < kMaxSamples)
        {
            lat.push_back(b - a);
        }
        else
        {
            r.samples_truncated = true;
        }
    }
    const double elapsed = duration<double>(Clock::now() - t0).count();
    const std::uint64_t cpu_after = self_cpu_ns();

    ctl->phase.store(2, std::memory_order_release);
    ::waitpid(pid, nullptr, 0);

    r.duration_sec = elapsed;
    r.samples = std::move(lat);
    r.lat = compute_stats(r.samples);

    r.rate_limit_bps = (transport != "shm") ? cfg.rate_limit_bps : 0;
    if (cfg.frag_stats && transport != "shm")
    {
        /* 客户端进程内直接读取: 统计的是它收到的"响应"分片。
         * 请求方向的分片丢失由服务端观察, 本用例不采集 —— 但 ser-cli 走
         * Reliable, 丢片会被 NACK 重传补齐, 因此这里的缺片数反映的是
         * 重传也没救回来的部分, 与失败率直接对应。 */
        const auto fs = dzIPC::socket::get_fragment_loss_stats();
        for (std::size_t i = 0; i < dzIPC::socket::FragmentLossStats::kGapBuckets; ++i)
        {
            r.frag_gap_hist[i] = fs.gap_hist[i];
        }
        r.frag_msgs_complete = fs.messages_complete;
        r.frag_msgs_incomplete = fs.messages_incomplete;
        r.frag_expected = fs.fragments_expected;
        r.frag_missing = fs.fragments_missing;
        r.frag_gaps_total = fs.gaps_total;
        r.frag_gap_max = fs.gap_max;
        r.nack_explicit_sent = fs.nack_explicit_sent;
        r.nack_bitmap_sent = fs.nack_bitmap_sent;
        r.nack_explicit_truncated = fs.nack_explicit_truncated;
        r.nack_bitmap_truncated = fs.nack_bitmap_truncated;
        r.frag_valid = r.frag_expected > 0;
        dzIPC::socket::set_fragment_loss_tracking(false);
    }

    const std::uint64_t total_cpu = (cpu_after - cpu_before) + ctl->child_cpu_ns.load(std::memory_order_acquire);
    r.cpu_cores = elapsed > 0 ? static_cast<double>(total_cpu) / 1e9 / elapsed : 0.0;
    r.ok = r.recv > 0;
    if (!r.ok)
    {
        r.note = "no successful request";
    }

    client.reset();
    cleanup();
    return r;
}

/* ------------------------------------------------------------------------ */
/* 输出                                                                      */
/* ------------------------------------------------------------------------ */

void print_case(const CaseResult& r)
{
    std::cout << "  " << std::left << std::setw(32) << r.case_id << std::right << std::fixed;
    if (!r.ok)
    {
        std::cout << "  FAILED: " << r.note << "\n";
        return;
    }
    std::cout << std::setw(11) << std::setprecision(0) << r.throughput_msg_s() << " msg/s"
              << std::setw(9) << std::setprecision(1) << r.throughput_mb_s() << " MB/s"
              << "  p50=" << std::setw(8) << std::setprecision(2) << r.lat.p50_us
              << "  p99=" << std::setw(8) << r.lat.p99_us
              << "  p999=" << std::setw(8) << r.lat.p999_us
              << "  max=" << std::setw(9) << r.lat.max_us << " us"
              << "  丢包=" << std::setw(6) << std::setprecision(2) << r.loss_pct() << "%"
              << "  CPU=" << std::setprecision(2) << r.cpu_cores << "\n";
    if (r.frag_valid)
    {
        std::cout << "    [分片] 期望=" << r.frag_expected << " 缺=" << r.frag_missing
                  << " (丢包=" << std::setprecision(4) << r.frag_loss_pct() << "%)"
                  << "  空洞=" << r.frag_gaps_total << " 平均长度=" << std::setprecision(2) << r.mean_gap_len()
                  << " 最长=" << r.frag_gap_max
                  << "  消息[完整=" << r.frag_msgs_complete << " 丢弃=" << r.frag_msgs_incomplete << "]\n";

        /* 只在真的发过 NACK 时才打印。零丢包时全 0, 打出来纯属噪音 ——
         * 但零丢包与"位图坏了"在这一行上长得一样, 所以判读必须结合上面的缺片数。 */
        if (r.nack_explicit_sent > 0 || r.nack_bitmap_sent > 0)
        {
            std::cout << "    [NACK] 显式列表=" << r.nack_explicit_sent << " 位图=" << r.nack_bitmap_sent;
            if (r.nack_explicit_truncated > 0 || r.nack_bitmap_truncated > 0)
            {
                std::cout << "  截断[列表=" << r.nack_explicit_truncated << " 位图=" << r.nack_bitmap_truncated
                          << "]";
            }
            std::cout << "\n";
        }
    }
}

const char* kCsvHeader =
    "case_id,pattern,transport,test_kind,payload_bytes,bytes_per_op,target_rate_hz,duration_s,"
    "sent,recv,failed,lost,loss_pct,throughput_msg_s,throughput_MB_s,"
    "lat_min_us,lat_mean_us,lat_p50_us,lat_p90_us,lat_p99_us,lat_p999_us,lat_max_us,jitter_stddev_us,"
    "cpu_cores,sample_count,samples_truncated,ok,note,"
    "rate_limit_bps,frag_valid,frag_expected,frag_missing,frag_loss_pct,frag_gaps,mean_gap_len,gap_max,"
    "frag_msgs_ok,frag_msgs_dropped,gap_1,gap_2_5,gap_6_15,gap_16_31,gap_32_63,gap_64_255,gap_256plus,"
    "nack_explicit_sent,nack_bitmap_sent,nack_explicit_truncated,nack_bitmap_truncated";

void write_csv(const std::string& path, const std::vector<CaseResult>& rs)
{
    std::ofstream f(path);
    f << kCsvHeader << "\n" << std::fixed;
    for (const auto& r : rs)
    {
        f << r.case_id << ',' << r.pattern << ',' << r.transport << ',' << r.test_kind << ','
          << r.payload_bytes << ',' << r.bytes_per_op << ',' << std::setprecision(1) << r.target_rate_hz << ','
          << std::setprecision(4) << r.duration_sec << ',' << r.sent << ',' << r.recv << ',' << r.failed << ','
          << r.lost() << ',' << std::setprecision(4) << r.loss_pct() << ',' << std::setprecision(2)
          << r.throughput_msg_s() << ',' << std::setprecision(4) << r.throughput_mb_s() << ',' << r.lat.min_us
          << ',' << r.lat.mean_us << ',' << r.lat.p50_us << ',' << r.lat.p90_us << ',' << r.lat.p99_us << ','
          << r.lat.p999_us << ',' << r.lat.max_us << ',' << r.lat.stddev_us << ',' << r.cpu_cores << ','
          << r.lat.count << ',' << (r.samples_truncated ? 1 : 0) << ',' << (r.ok ? 1 : 0) << ',' << '"' << r.note
          << '"' << ',' << r.rate_limit_bps << ',' << (r.frag_valid ? 1 : 0) << ',' << r.frag_expected << ','
          << r.frag_missing << ',' << std::setprecision(6) << r.frag_loss_pct() << ',' << r.frag_gaps_total << ','
          << std::setprecision(3) << r.mean_gap_len() << ',' << r.frag_gap_max << ',' << r.frag_msgs_complete << ','
          << r.frag_msgs_incomplete;
        for (int i = 0; i < 7; ++i)
        {
            f << ',' << r.frag_gap_hist[i];
        }
        f << ',' << r.nack_explicit_sent << ',' << r.nack_bitmap_sent << ',' << r.nack_explicit_truncated << ','
          << r.nack_bitmap_truncated;
        f << "\n";
    }
}

void write_json(const std::string& path, const KV& sysinfo, const Config& cfg, const std::vector<CaseResult>& rs)
{
    std::ofstream f(path);
    f << std::fixed;
    f << "{\n  \"hardware\": {\n";
    for (std::size_t i = 0; i < sysinfo.size(); ++i)
    {
        f << "    \"" << json_escape(sysinfo[i].first) << "\": \"" << json_escape(sysinfo[i].second) << "\""
          << (i + 1 == sysinfo.size() ? "\n" : ",\n");
    }
    f << "  },\n";

    f << "  \"config\": {\n"
      << "    \"duration_s\": " << std::setprecision(3) << cfg.duration << ",\n"
      << "    \"warmup_s\": " << cfg.warmup << ",\n"
      << "    \"latency_rate_hz\": " << cfg.lat_rate << ",\n"
      << "    \"subscriber_queue_size\": " << cfg.queue_size << ",\n"
      << "    \"domain_id\": " << cfg.domain << ",\n"
      << "    \"publish_mode\": \""
      << (cfg.best_effort ? "publish() best-effort on both transports"
                          : "SHM: publish_blocking / SOCKET: publish() best-effort")
      << "\",\n"
      << "    \"publish_timeout_ms\": " << cfg.publish_timeout_ms << ",\n"
      << "    \"pin_tx_cpu\": " << cfg.pin_tx << ",\n"
      << "    \"pin_rx_cpu\": " << cfg.pin_rx << ",\n"
      << "    \"rate_limit_bps\": " << cfg.rate_limit_bps << ",\n"
      << "    \"fragment_stats\": " << (cfg.frag_stats ? "true" : "false") << "\n"
      << "  },\n";

    f << "  \"results\": [\n";
    for (std::size_t i = 0; i < rs.size(); ++i)
    {
        const auto& r = rs[i];
        f << "    {\n"
          << "      \"case_id\": \"" << r.case_id << "\",\n"
          << "      \"pattern\": \"" << r.pattern << "\",\n"
          << "      \"transport\": \"" << r.transport << "\",\n"
          << "      \"test_kind\": \"" << r.test_kind << "\",\n"
          << "      \"payload_bytes\": " << r.payload_bytes << ",\n"
          << "      \"bytes_per_op\": " << r.bytes_per_op << ",\n"
          << "      \"target_rate_hz\": " << std::setprecision(1) << r.target_rate_hz << ",\n"
          << "      \"duration_s\": " << std::setprecision(4) << r.duration_sec << ",\n"
          << "      \"sent\": " << r.sent << ",\n"
          << "      \"recv\": " << r.recv << ",\n"
          << "      \"failed\": " << r.failed << ",\n"
          << "      \"lost\": " << r.lost() << ",\n"
          << "      \"loss_pct\": " << r.loss_pct() << ",\n"
          << "      \"throughput_msg_s\": " << std::setprecision(2) << r.throughput_msg_s() << ",\n"
          << "      \"throughput_MB_s\": " << std::setprecision(4) << r.throughput_mb_s() << ",\n"
          << "      \"lat_min_us\": " << r.lat.min_us << ",\n"
          << "      \"lat_mean_us\": " << r.lat.mean_us << ",\n"
          << "      \"lat_p50_us\": " << r.lat.p50_us << ",\n"
          << "      \"lat_p90_us\": " << r.lat.p90_us << ",\n"
          << "      \"lat_p99_us\": " << r.lat.p99_us << ",\n"
          << "      \"lat_p999_us\": " << r.lat.p999_us << ",\n"
          << "      \"lat_max_us\": " << r.lat.max_us << ",\n"
          << "      \"jitter_stddev_us\": " << r.lat.stddev_us << ",\n"
          << "      \"cpu_cores\": " << r.cpu_cores << ",\n"
          << "      \"sample_count\": " << r.lat.count << ",\n"
          << "      \"samples_truncated\": " << (r.samples_truncated ? "true" : "false") << ",\n"
          << "      \"ok\": " << (r.ok ? "true" : "false") << ",\n"
          << "      \"note\": \"" << json_escape(r.note) << "\",\n"
          << "      \"rate_limit_bps\": " << r.rate_limit_bps << ",\n"
          << "      \"fragment_stats\": ";
        if (!r.frag_valid)
        {
            /* null 而非零值对象: 区分"没采集"与"采集到零丢包"。 */
            f << "null\n";
        }
        else
        {
            f << "{\n"
              << "        \"fragments_expected\": " << r.frag_expected << ",\n"
              << "        \"fragments_missing\": " << r.frag_missing << ",\n"
              << "        \"fragment_loss_pct\": " << std::setprecision(6) << r.frag_loss_pct() << ",\n"
              << "        \"gaps_total\": " << r.frag_gaps_total << ",\n"
              << "        \"mean_gap_len\": " << std::setprecision(3) << r.mean_gap_len() << ",\n"
              << "        \"gap_max\": " << r.frag_gap_max << ",\n"
              << "        \"messages_complete\": " << r.frag_msgs_complete << ",\n"
              << "        \"messages_dropped\": " << r.frag_msgs_incomplete << ",\n"
              << "        \"gap_histogram\": {\n"
              << "          \"1\": " << r.frag_gap_hist[0] << ",\n"
              << "          \"2-5\": " << r.frag_gap_hist[1] << ",\n"
              << "          \"6-15\": " << r.frag_gap_hist[2] << ",\n"
              << "          \"16-31\": " << r.frag_gap_hist[3] << ",\n"
              << "          \"32-63\": " << r.frag_gap_hist[4] << ",\n"
              << "          \"64-255\": " << r.frag_gap_hist[5] << ",\n"
              << "          \"256+\": " << r.frag_gap_hist[6] << "\n"
              << "        },\n"
              << "        \"nack_encoding\": {\n"
              << "          \"explicit_sent\": " << r.nack_explicit_sent << ",\n"
              << "          \"bitmap_sent\": " << r.nack_bitmap_sent << ",\n"
              << "          \"explicit_truncated\": " << r.nack_explicit_truncated << ",\n"
              << "          \"bitmap_truncated\": " << r.nack_bitmap_truncated << "\n"
              << "        }\n"
              << "      }\n";
        }
        f << "    }" << (i + 1 == rs.size() ? "\n" : ",\n");
    }
    f << "  ]\n}\n";
}

void write_hardware_txt(const std::string& path, const KV& sysinfo)
{
    std::ofstream f(path);
    f << "dzIPC performance benchmark - hardware and system configuration\n";
    f << "======================================================================\n";
    for (const auto& kv : sysinfo)
    {
        f << std::left << std::setw(24) << kv.first << " : " << kv.second << "\n";
    }
}

/* 时延样本导出: 已排序, 最多下采样到 20000 点, 足够画 CDF 且文件不会过大 */
void write_samples(const std::string& dir, const CaseResult& r)
{
    if (r.samples.empty())
    {
        return;
    }
    constexpr std::size_t kMaxOut = 20000;
    const std::size_t n = r.samples.size();
    const std::size_t step = (n + kMaxOut - 1) / kMaxOut;

    std::ofstream f(dir + "/" + r.case_id + ".csv");
    f << "latency_us\n" << std::fixed << std::setprecision(4);
    for (std::size_t i = 0; i < n; i += step)
    {
        f << static_cast<double>(r.samples[i]) / 1000.0 << "\n";
    }
}

/* ------------------------------------------------------------------------ */
/* 参数解析                                                                  */
/* ------------------------------------------------------------------------ */

void print_help()
{
    std::cout << "dzIPC communication performance benchmark\n"
                 "usage: dzipc_perf_benchmark [options]\n"
                 "  --out=DIR         result directory (default ./perf_results/<timestamp>)\n"
                 "  --payloads=LIST   payload sizes in bytes, comma separated\n"
                 "  --duration=S      measurement duration per case (default 3)\n"
                 "  --warmup=S        warmup duration per case (default 0.5)\n"
                 "  --lat-rate=HZ     send rate for latency cases (default 1000)\n"
                 "  --cases=LIST      pubsub_shm,pubsub_socket,sercli_shm,sercli_socket\n"
                 "  --skip-tput       skip the pub-sub max-rate throughput cases\n"
                 "  --best-effort     force publish() best-effort on both transports\n"
                 "                    (overwrites when the queue is full; may yield torn data)\n"
                 "  --pub-timeout=MS  publish_blocking timeout in ms, SHM only (default 100)\n"
                 "  --pin=A,B         pin publisher to core A, subscriber to core B\n"
                 "  --queue=N         subscriber queue depth (default 1024)\n"
                 "  --domain=N        domain id (default 1; larger values can overflow the UDP port)\n"
                 "  --rate-limit=BPS  socket send rate cap in bytes/s, 0 = unlimited (default 0)\n"
                 "                    e.g. 20M caps at 20 MB/s; only affects multi-fragment messages\n"
                 "  --frag-stats      collect fragment gap-length histogram (random vs burst loss)\n";
}

bool parse_args(int argc, char** argv, Config& cfg)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h")
        {
            print_help();
            std::exit(0);
        }
        else if (a.rfind("--out=", 0) == 0)
        {
            cfg.out_dir = a.substr(6);
        }
        else if (a.rfind("--payloads=", 0) == 0)
        {
            cfg.payloads.clear();
            for (const auto& s : split(a.substr(11), ','))
            {
                cfg.payloads.push_back(static_cast<std::size_t>(std::strtoull(s.c_str(), nullptr, 10)));
            }
        }
        else if (a.rfind("--duration=", 0) == 0)
        {
            cfg.duration = std::strtod(a.substr(11).c_str(), nullptr);
        }
        else if (a.rfind("--warmup=", 0) == 0)
        {
            cfg.warmup = std::strtod(a.substr(9).c_str(), nullptr);
        }
        else if (a.rfind("--lat-rate=", 0) == 0)
        {
            cfg.lat_rate = std::strtod(a.substr(11).c_str(), nullptr);
        }
        else if (a.rfind("--cases=", 0) == 0)
        {
            cfg.cases = split(a.substr(8), ',');
        }
        else if (a == "--skip-tput")
        {
            cfg.skip_tput = true;
        }
        else if (a == "--best-effort")
        {
            cfg.best_effort = true;
        }
        else if (a.rfind("--pub-timeout=", 0) == 0)
        {
            cfg.publish_timeout_ms = std::strtoull(a.substr(14).c_str(), nullptr, 10);
        }
        else if (a.rfind("--rate-limit=", 0) == 0)
        {
            /* 支持 K/M/G 后缀: --rate-limit=20M 等价于 20971520 */
            const std::string v = a.substr(13);
            char* end = nullptr;
            double n = std::strtod(v.c_str(), &end);
            if (end != nullptr && *end != '\0')
            {
                switch (*end)
                {
                case 'k':
                case 'K': n *= 1024.0; break;
                case 'm':
                case 'M': n *= 1024.0 * 1024.0; break;
                case 'g':
                case 'G': n *= 1024.0 * 1024.0 * 1024.0; break;
                default: break;
                }
            }
            cfg.rate_limit_bps = static_cast<std::size_t>(n);
        }
        else if (a == "--frag-stats")
        {
            cfg.frag_stats = true;
        }
        else if (a.rfind("--pin=", 0) == 0)
        {
            const auto v = split(a.substr(6), ',');
            if (v.size() == 2)
            {
                cfg.pin_tx = std::atoi(v[0].c_str());
                cfg.pin_rx = std::atoi(v[1].c_str());
            }
        }
        else if (a.rfind("--queue=", 0) == 0)
        {
            cfg.queue_size = static_cast<std::size_t>(std::strtoull(a.substr(8).c_str(), nullptr, 10));
        }
        else if (a.rfind("--domain=", 0) == 0)
        {
            cfg.domain = static_cast<std::size_t>(std::strtoull(a.substr(9).c_str(), nullptr, 10));
        }
        /* --- 内部: 子进程角色参数 --- */
        else if (a.rfind("--role=", 0) == 0)
        {
            cfg.role = a.substr(7);
        }
        else if (a.rfind("--ctl=", 0) == 0)
        {
            cfg.ctl_name = a.substr(6);
        }
        else if (a.rfind("--topic=", 0) == 0)
        {
            cfg.topic = a.substr(8);
        }
        else if (a.rfind("--transport=", 0) == 0)
        {
            cfg.transport = a.substr(12);
        }
        else if (a.rfind("--payload=", 0) == 0)
        {
            cfg.payload = static_cast<std::size_t>(std::strtoull(a.substr(10).c_str(), nullptr, 10));
        }
        else
        {
            std::cerr << "unknown option: " << a << "\n";
            print_help();
            return false;
        }
    }
    return true;
}

}   // namespace

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    Config cfg;
    cfg.out_dir = "perf_results/" + timestamp_string();

    if (!parse_args(argc, argv, cfg))
    {
        return 1;
    }

    /* Nodelet 是同进程快速路径, 性能测试要测真实跨进程 IPC, 明确关闭 */
    dzIPC::EnableNodelet(false);

    /* ---- 子进程分支: 只扮演对端角色, 不产出报告 ---- */
    if (cfg.role == "sub")
    {
        return run_role_subscriber(cfg);
    }
    if (cfg.role == "srv")
    {
        return run_role_server(cfg);
    }

    /* ---- 主进程 ---- */
    if (!make_dir_p(cfg.out_dir) || !make_dir_p(cfg.out_dir + "/samples"))
    {
        std::cerr << "[fatal] cannot create output directory: " << cfg.out_dir << "\n";
        return 1;
    }

    const KV sysinfo = collect_system_info();

    std::cout << "======================================================================\n"
              << " dzIPC 通信性能测试\n"
              << "======================================================================\n";
    for (const auto& kv : sysinfo)
    {
        if (kv.first == "kernel_cmdline")
        {
            continue;   // 太长, 只写文件
        }
        std::cout << "  " << std::left << std::setw(22) << kv.first << ": " << kv.second << "\n";
    }
    std::cout << "----------------------------------------------------------------------\n"
              << "  测量时长/用例        : " << cfg.duration << " s\n"
              << "  预热时长/用例        : " << cfg.warmup << " s\n"
              << "  时延用例发送速率     : " << cfg.lat_rate << " Hz\n"
              << "  payload 扫描         : ";
    for (std::size_t p : cfg.payloads)
    {
        std::cout << p << " ";
    }
    std::cout << "B\n"
              << "  输出目录             : " << cfg.out_dir << "\n"
              << "======================================================================\n\n";

    std::vector<CaseResult> results;

    struct Combo
    {
        const char* key;
        const char* pattern;
        const char* transport;
    };
    const Combo combos[] = {
        {"pubsub_shm", "pubsub", "shm"},
        {"pubsub_socket", "pubsub", "socket"},
        {"sercli_shm", "sercli", "shm"},
        {"sercli_socket", "sercli", "socket"},
    };

    for (const auto& c : combos)
    {
        if (!cfg.has_case(c.key))
        {
            continue;
        }
        std::cout << "---- " << c.key << " ----\n";
        for (std::size_t payload : cfg.payloads)
        {
            if (std::string(c.pattern) == "pubsub")
            {
                CaseResult lat = run_pubsub_case(cfg, c.transport, payload, /*max_rate=*/false);
                print_case(lat);
                write_samples(cfg.out_dir + "/samples", lat);
                results.push_back(std::move(lat));

                if (!cfg.skip_tput)
                {
                    CaseResult tp = run_pubsub_case(cfg, c.transport, payload, /*max_rate=*/true);
                    print_case(tp);
                    write_samples(cfg.out_dir + "/samples", tp);
                    results.push_back(std::move(tp));
                }
            }
            else
            {
                CaseResult rtt = run_sercli_case(cfg, c.transport, payload);
                print_case(rtt);
                write_samples(cfg.out_dir + "/samples", rtt);
                results.push_back(std::move(rtt));
            }
        }
        std::cout << "\n";
    }

    write_csv(cfg.out_dir + "/results.csv", results);
    write_json(cfg.out_dir + "/results.json", sysinfo, cfg, results);
    write_hardware_txt(cfg.out_dir + "/hardware.txt", sysinfo);

    std::size_t failed = 0;
    for (const auto& r : results)
    {
        if (!r.ok)
        {
            ++failed;
        }
    }

    std::cout << "======================================================================\n"
              << "  用例总数 : " << results.size() << "   失败 : " << failed << "\n"
              << "  结果 CSV : " << cfg.out_dir << "/results.csv\n"
              << "  结果 JSON: " << cfg.out_dir << "/results.json\n"
              << "  硬件配置 : " << cfg.out_dir << "/hardware.txt\n"
              << "  时延样本 : " << cfg.out_dir << "/samples/\n"
              << "\n  绘图: python3 scripts/plot_dzipc_perf.py " << cfg.out_dir << "\n"
              << "======================================================================\n";
    return failed == 0 ? 0 : 2;
}
