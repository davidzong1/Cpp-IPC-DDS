/* 复核 W10 工装 w10_pick_socket_topics() 的端口排除是否真的生效。
 * 逐字复刻 test/perf/w10/w10_matrix.cpp:1176-1190 的解析循环，只加计数器。 */
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include "dzIPC/common/hash.h"

int main()
{
    std::set<int> used;
    std::ifstream f("/proc/net/udp");
    std::string line;
    std::getline(f, line);                       // 表头
    int rows = 0, parsed_nonzero = 0;
    while (std::getline(f, line))
    {
        std::istringstream is(line);
        std::string addr;
        if (!(is >> addr)) continue;
        ++rows;
        const auto colon = addr.find(':');
        if (colon == std::string::npos) continue;
        const int p = static_cast<int>(std::strtol(addr.c_str() + colon + 1, nullptr, 16));
        if (p != 0) ++parsed_nonzero;
        used.insert(p);
    }
    std::printf("rows=%d parsed_nonzero=%d used_set_size=%zu\n", rows, parsed_nonzero, used.size());
    std::printf("first_token_sample=\"%s\"\n", [&] { std::ifstream g("/proc/net/udp"); std::string l; std::getline(g, l);
        std::getline(g, l); std::istringstream is(l); std::string a; is >> a; return a.c_str(); }());
    std::printf("used={");
    for (int v : used) std::printf("%d ", v);
    std::printf("}\n");

    /* 真实占用端口数（正确解析：跳过 slot 号，取 local_address 的第 2 段） */
    std::set<int> real;
    std::ifstream g("/proc/net/udp");
    std::getline(g, line);
    while (std::getline(g, line))
    {
        std::istringstream is(line);
        std::string slot, local;
        if (!(is >> slot >> local)) continue;
        const auto c = local.find(':');
        if (c == std::string::npos) continue;
        real.insert(static_cast<int>(std::strtol(local.c_str() + c + 1, nullptr, 16)));
    }
    std::printf("real_occupied=%zu  real_has_48650=%d\n", real.size(), (int)real.count(48650));
    const int base38 = static_cast<int>(dzIPC::common::udp_discovery_port_calculate("w10_socket_independent_3103_38", 3103));
    std::printf("topic38_base=%d  picker_would_exclude_it=%d\n", base38, (int)(used.count(base38) != 0));
    return 0;
}
