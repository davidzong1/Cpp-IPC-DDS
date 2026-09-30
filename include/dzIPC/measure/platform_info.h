#pragma once
/* W03 测量口径 · 运行环境与拓扑快照
 * ============================================================================
 * 交付依据：§12「运行前必须保存 git status --short、重新配置和编译命令、实际环境
 * 变量、CPU 拓扑、加载库路径及配置哈希」；§6.2「受控组记录 governor、P/E 核类型、
 * SMT、亲和性、NUMA、系统负载及温度/频率条件」。
 *
 * 这些字段全部进 `environment.json` / `topology.json`，是判断"两组实验是否可比"的
 * 前提。缺项写 null 并在 verdict 标注，不允许省略后假定可比。
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include "dzIPC/measure/proc_sampler.h"

namespace dzIPC {
namespace measure {

struct RlimitInfo
{
    long long soft{-1};
    long long hard{-1};
    bool      unlimited{false};
};

struct PlatformInfo
{
    std::string   hostname;
    std::string   kernel_release;
    std::string   kernel_version;
    std::string   machine;
    std::string   cpu_model;
    int           online_cpus{0};
    int           configured_cpus{0};
    bool          smt_on{false};
    long          clock_ticks_per_sec{100};
    long          page_size{4096};
    bool          numa_available{false};
    int           numa_nodes{0};
    std::vector<std::string> cpu_governors;       ///< 每 CPU 一个（去重后仍按 CPU 列出）
    std::vector<std::string> cpu_governors_unique;
    bool          hypervisor{false};
    std::string   virtualization;
    double        loadavg_1{0.0};
    double        loadavg_5{0.0};
    double        loadavg_15{0.0};
    double        mem_total_kb{0.0};
    double        mem_available_kb{0.0};
    RlimitInfo    nofile;
    RlimitInfo    rss;
    long          argv_max{0};
    long          open_max_configured{0};
    std::string   cgroup_controllers;
};

inline long lround_to_long(double v) { return static_cast<long>(v + 0.5); }

inline PlatformInfo collect_platform_info()
{
    PlatformInfo p;

    utsname u {};
    if (::uname(&u) == 0) {
        p.hostname = u.nodename;
        p.kernel_release = u.release;
        p.kernel_version = u.version;
        p.machine = u.machine;
    }
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && p.hostname.empty()) p.hostname = buf;

    p.online_cpus = static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
    p.configured_cpus = static_cast<int>(::sysconf(_SC_NPROCESSORS_CONF));
    p.clock_ticks_per_sec = ::sysconf(_SC_CLK_TCK);
    p.page_size = ::sysconf(_SC_PAGESIZE);
    p.argv_max = ::sysconf(_SC_ARG_MAX);
    p.open_max_configured = ::sysconf(_SC_OPEN_MAX);

    /* /proc/cpuinfo: model name + siblings vs cpu cores（判断 SMT） */
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        int siblings = 0, cores = 0, processors = 0;
        while (std::getline(f, line)) {
            const std::size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string key = line.substr(0, c);
            std::string val = line.substr(c + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            if (key == "model name" && p.cpu_model.empty()) p.cpu_model = val;
            else if (key == "siblings") siblings = std::atoi(val.c_str());
            else if (key == "cpu cores") cores = std::atoi(val.c_str());
            else if (key == "processor") ++processors;
            else if (key == "flags" && val.find("hypervisor") != std::string::npos) p.hypervisor = true;
        }
        if (siblings > 0 && cores > 0) p.smt_on = (siblings > cores);
        if (p.configured_cpus == 0) p.configured_cpus = processors;
    }

    /* cpufreq governor（可能不存在；缺失即空，报告标"未确认"） */
    for (int i = 0; i < p.online_cpus; ++i) {
        bool ok = false;
        std::string g = read_text_file("/sys/devices/system/cpu/cpu" + std::to_string(i)
                                      + "/cpufreq/scaling_governor", &ok);
        while (!g.empty() && (g.back() == '\n' || g.back() == '\r')) g.pop_back();
        if (!ok) g = "unavailable";
        p.cpu_governors.push_back(g);
        bool dup = false;
        for (const std::string& e : p.cpu_governors_unique) if (e == g) { dup = true; break; }
        if (!dup) p.cpu_governors_unique.push_back(g);
    }

    /* NUMA */
    {
        std::ifstream f("/sys/devices/system/node/possible");
        std::string s;
        if (f && (f >> s)) {
            p.numa_available = true;
            std::size_t dash = s.find('-');
            if (dash != std::string::npos)
                p.numa_nodes = std::atoi(s.substr(dash + 1).c_str()) + 1;
            else
                p.numa_nodes = 1;
        }
    }

    /* loadavg */
    {
        std::ifstream f("/proc/loadavg");
        if (f) f >> p.loadavg_1 >> p.loadavg_5 >> p.loadavg_15;
    }

    /* meminfo */
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            const std::size_t c = line.find(':');
            if (c == std::string::npos) continue;
            const std::string key = line.substr(0, c);
            double v = std::atof(line.c_str() + c + 1);
            if (key == "MemTotal") p.mem_total_kb = v;
            else if (key == "MemAvailable") p.mem_available_kb = v;
        }
    }

    /* rlimits */
    struct rlimit rl {};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        p.nofile.unlimited = (rl.rlim_cur == RLIM_INFINITY);
        p.nofile.soft = p.nofile.unlimited ? -1 : static_cast<long long>(rl.rlim_cur);
        p.nofile.hard = (rl.rlim_max == RLIM_INFINITY) ? -1 : static_cast<long long>(rl.rlim_max);
    }
    if (::getrlimit(RLIMIT_RSS, &rl) == 0) {
        p.rss.unlimited = (rl.rlim_cur == RLIM_INFINITY);
        p.rss.soft = p.rss.unlimited ? -1 : static_cast<long long>(rl.rlim_cur);
    }

    /* cgroup */
    {
        std::ifstream f("/proc/self/cgroup");
        std::string line, joined;
        while (std::getline(f, line)) { if (!joined.empty()) joined += ';'; joined += line; }
        p.cgroup_controllers = joined;
    }

    /* 虚拟化提示 */
    {
        std::string v = read_text_file("/sys/class/dmi/id/product_name");
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        p.virtualization = v;
    }
    return p;
}

inline std::string json_escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char tmp[8];
                std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned>(c));
                o += tmp;
            } else {
                o += c;
            }
        }
    }
    return o;
}

inline std::string platform_info_json(const PlatformInfo& p)
{
    std::ostringstream o;
    o << "{\n";
    o << "  \"hostname\": \"" << json_escape(p.hostname) << "\",\n";
    o << "  \"kernel_release\": \"" << json_escape(p.kernel_release) << "\",\n";
    o << "  \"kernel_version\": \"" << json_escape(p.kernel_version) << "\",\n";
    o << "  \"machine\": \"" << json_escape(p.machine) << "\",\n";
    o << "  \"cpu_model\": \"" << json_escape(p.cpu_model) << "\",\n";
    o << "  \"online_cpus\": " << p.online_cpus << ",\n";
    o << "  \"configured_cpus\": " << p.configured_cpus << ",\n";
    o << "  \"smt_on\": " << (p.smt_on ? "true" : "false") << ",\n";
    o << "  \"clock_ticks_per_sec\": " << p.clock_ticks_per_sec << ",\n";
    o << "  \"page_size\": " << p.page_size << ",\n";
    o << "  \"numa_available\": " << (p.numa_available ? "true" : "false") << ",\n";
    o << "  \"numa_nodes\": " << p.numa_nodes << ",\n";
    o << "  \"hypervisor\": " << (p.hypervisor ? "true" : "false") << ",\n";
    o << "  \"virtualization\": \"" << json_escape(p.virtualization) << "\",\n";
    o << "  \"loadavg\": [" << p.loadavg_1 << ", " << p.loadavg_5 << ", " << p.loadavg_15 << "],\n";
    o << "  \"mem_total_kb\": " << p.mem_total_kb << ",\n";
    o << "  \"mem_available_kb\": " << p.mem_available_kb << ",\n";
    o << "  \"nofile_soft\": " << p.nofile.soft << ",\n";
    o << "  \"nofile_hard\": " << p.nofile.hard << ",\n";
    o << "  \"open_max_configured\": " << p.open_max_configured << ",\n";
    o << "  \"governors_unique\": [";
    for (std::size_t i = 0; i < p.cpu_governors_unique.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << json_escape(p.cpu_governors_unique[i]) << "\"";
    }
    o << "],\n";
    o << "  \"governors_per_cpu\": [";
    for (std::size_t i = 0; i < p.cpu_governors.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << json_escape(p.cpu_governors[i]) << "\"";
    }
    o << "],\n";
    o << "  \"cgroup\": \"" << json_escape(p.cgroup_controllers) << "\"\n";
    o << "}\n";
    return o.str();
}

}   // namespace measure
}   // namespace dzIPC
