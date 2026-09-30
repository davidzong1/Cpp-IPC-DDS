/* D-22 碰撞面复算（**正确** 解析 /proc/net/udp：第 2 列 local_address）。
 * 输出：dzIPC 端口窗口 [11451,65531] 与本机已占 UDP 端口的重叠、ip_local_port_range
 * 与该窗口的重叠、以及 domain=3103 的 1000 路里有多少条落进"已被占用"的端口段。
 * ⛔ 只读探针，不改任何产品行为。 */
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "dzIPC/common/hash.h"

int main(int argc, char** argv)
{
    const long dom = (argc > 1) ? std::atol(argv[1]) : 3103;
    const int n = (argc > 2) ? std::atoi(argv[2]) : 1000;

    std::set<int> occ;
    {
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
            occ.insert(static_cast<int>(std::strtol(local.c_str() + c + 1, nullptr, 16)));
        }
    }
    std::set<int> occ6;
    {
        std::ifstream f("/proc/net/udp6");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line))
        {
            std::istringstream is(line);
            std::string slot, local;
            if (!(is >> slot >> local)) continue;
            const auto c = local.rfind(':');
            if (c == std::string::npos) continue;
            occ6.insert(static_cast<int>(std::strtol(local.c_str() + c + 1, nullptr, 16)));
        }
    }
    const int lo = UDP_DISCOVERY_BASE_PORT;   /* 宏(11'451): ⛔ 不能加 dzIPC::common:: 前缀 */
    const int hi = 11451 + dzIPC::common::kUdpPortWindow - 1;
    int occ_in_window = 0;
    for (int p : occ) if (p >= lo && p <= hi) ++occ_in_window;

    int lpr_lo = 0, lpr_hi = 0;
    {
        std::ifstream f("/proc/sys/net/ipv4/ip_local_port_range");
        f >> lpr_lo >> lpr_hi;
    }
    const long win_slots = dzIPC::common::kUdpPortWindow;
    const int ov_lo = (lpr_lo > lo) ? lpr_lo : lo;
    const int ov_hi = (lpr_hi < hi) ? lpr_hi : hi;
    const long ov = (ov_hi >= ov_lo) ? (ov_hi - ov_lo + 1) : 0;

    std::printf("dzipc_window=[%d,%d] slots=%ld\n", lo, hi, win_slots);
    std::printf("ip_local_port_range=[%d,%d]\n", lpr_lo, lpr_hi);
    std::printf("overlap=[%d,%d] slots=%ld  overlap_ratio=%.4f\n", ov_lo, ov_hi, ov,
                win_slots > 0 ? static_cast<double>(ov) / static_cast<double>(win_slots) : 0.0);
    std::printf("occupied_udp4=%zu occupied_udp6=%zu occupied_in_window=%d (%.4f of window)\n", occ.size(), occ6.size(),
                occ_in_window, win_slots > 0 ? static_cast<double>(occ_in_window) / static_cast<double>(win_slots) : 0.0);

    /* domain=3103 默认命名下的 1000 路：逐条算 base_port，检查 [base,base+4] 是否与已占端口相交。 */
    int clash_routes = 0, clash_slots = 0;
    std::vector<std::string> clashed;
    for (int i = 0; i < n; ++i)
    {
        const std::string name = "w10_socket_independent_" + std::to_string(dom) + "_" + std::to_string(i);
        const int base = dzIPC::common::udp_discovery_port_calculate(name, static_cast<int>(dom));
        bool hit = false;
        for (int o = 0; o < 5; ++o)
        {
            if (occ.count(base + o) || occ6.count(base + o))
            {
                hit = true;
                ++clash_slots;
            }
        }
        if (hit)
        {
            ++clash_routes;
            clashed.push_back(std::to_string(i) + ":" + std::to_string(base));
        }
    }
    std::printf("domain=%ld routes=%d clash_routes=%d clash_ratio=%.4f clash_slots=%d\n", dom, n, clash_routes,
                n > 0 ? static_cast<double>(clash_routes) / n : 0.0, clash_slots);
    std::printf("clashed=");
    for (auto& s : clashed) std::printf("%s ", s.c_str());
    std::printf("\n");
    return 0;
}
