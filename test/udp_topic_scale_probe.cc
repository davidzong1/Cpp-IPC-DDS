/* UDP 话题规模探针: N 个 topic 下组播地址与端口的碰撞数
 *
 * 背景(docs/shm_defect_fixes.md 第 3 条、dds_interface_roadmap.md 拦路石 3):
 * 组地址是 239.255.<fnv(t+"_mid")%254>.<fnv(t+"_end")%254>, 只有 254×254 个可用组;
 * 端口是 11451 + domain_id * (fnv(t)%10000), 默认 domain_id=0 时对**所有** topic 都
 * 退化成 11451。于是默认配置下**组是唯一的隔离维度**。
 *
 * 两个 topic 撞到同一个 (组, 端口) 就构成串扰: 各自的订阅者会收到对方的流量。
 * 若 msg_id 又相同(默认就是 0), check_id 放行 → 按错误类型反序列化。
 *
 * 本探针**直接调生产函数**(dzIPC/common/hash.h 的两个导出函数), 不重写一份哈希 ——
 * 重写会与真实行为偏离, 那样的数字没有决策价值。
 *
 * 自带 main, 不是 gtest。
 */
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dzIPC/common/hash.h"

namespace {

struct Counts
{
    std::size_t n = 0;
    std::size_t distinct_groups = 0;
    std::size_t distinct_group_port = 0;
    /* 有多少个 topic 与**至少一个**别的 topic 共享 (组,端口) */
    std::size_t topics_in_group_collision = 0;
    std::size_t topics_in_groupport_collision = 0;
    /* 组碰撞里最坏的那一组有几个 topic */
    std::size_t worst_group_share = 0;
    std::size_t worst_groupport_share = 0;
};

Counts measure(const std::vector<std::string>& topics, int domain_id)
{
    Counts c;
    c.n = topics.size();
    std::map<std::string, std::size_t> by_group;
    std::map<std::pair<std::string, uint16_t>, std::size_t> by_group_port;

    for (const auto& t : topics)
    {
        const std::string g = dzIPC::common::udp_discovery_addr_calculate(t);
        uint16_t p = 0;
        try
        {
            p = dzIPC::common::udp_discovery_port_calculate(t, domain_id);
        }
        catch (const std::exception&)
        {
            continue;   /* 端口越界的 topic 走不到传输层, 不计入 */
        }
        ++by_group[g];
        ++by_group_port[{g, p}];
    }

    c.distinct_groups = by_group.size();
    c.distinct_group_port = by_group_port.size();
    for (const auto& kv : by_group)
    {
        if (kv.second > 1)
        {
            c.topics_in_group_collision += kv.second;
            c.worst_group_share = std::max(c.worst_group_share, kv.second);
        }
    }
    for (const auto& kv : by_group_port)
    {
        if (kv.second > 1)
        {
            c.topics_in_groupport_collision += kv.second;
            c.worst_groupport_share = std::max(c.worst_groupport_share, kv.second);
        }
    }
    return c;
}

void report(const char* label, const Counts& c, int domain_id)
{
    const std::size_t gcol = c.topics_in_group_collision;
    const std::size_t gpcol = c.topics_in_groupport_collision;
    std::printf("\n--- %s (domain=%d, %zu topic) ---\n", label, domain_id, c.n);
    std::printf("  不同组数            : %zu  (空组空间 254*254 = 64516)\n", c.distinct_groups);
    std::printf("  不同(组,端口)数     : %zu\n", c.distinct_group_port);
    std::printf("  组碰撞: 卷入 %zu 个 topic (%.2f%%), 最挤的组含 %zu 个\n", gcol,
                100.0 * double(gcol) / double(c.n), c.worst_group_share);
    std::printf("  (组,端口)碰撞: 卷入 %zu 个 topic (%.2f%%), 最挤含 %zu 个\n", gpcol,
                100.0 * double(gpcol) / double(c.n), c.worst_groupport_share);
}

std::vector<std::string> gen_sequential(std::size_t n)
{
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) v.push_back("/topic_" + std::to_string(i));
    return v;
}

/* ROS 风格的分层命名 —— 这是真实部署里最常见的形态 */
std::vector<std::string> gen_ros_like(std::size_t n)
{
    static const char* kSub[] = {"camera", "lidar", "imu", "gnss", "radar", "sonar",
                                 "thermal", "depth", "fisheye", "microphone"};
    static const char* kField[] = {"image", "points", "info", "status", "raw",
                                   "compressed", "meta", "diag"};
    std::vector<std::string> v;
    v.reserve(n);
    std::size_t i = 0;
    for (std::size_t s = 0; v.size() < n; ++s)
    {
        for (const char* sub : kSub)
        {
            for (const char* f : kField)
            {
                if (v.size() >= n) break;
                v.push_back("/robot_" + std::to_string(s) + "/" + sub + "/" + f);
            }
        }
        (void)i;
    }
    return v;
}

}   // namespace

int main(int argc, char** argv)
{
    std::size_t n = 8192;
    if (argc > 1) n = std::stoul(argv[1]);

    std::printf("UDP 话题规模探针 — %zu 个 topic\n", n);
    std::printf("组地址 = 239.255.<fnv(t+_mid)%%254>.<fnv(t+_end)%%254>\n");
    std::printf("端口   = 11451 + domain_id * (fnv(t)%%10000)\n");

    const auto seq = gen_sequential(n);
    const auto ros = gen_ros_like(n);

    for (int dom : {0, 1, 7})
    {
        report("顺序命名", measure(seq, dom), dom);
        report("ROS 风格命名", measure(ros, dom), dom);
    }

    /* 额外: 换个更\"人写\"的命名看是否更糟 */
    std::vector<std::string> human;
    human.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        human.push_back("/ns" + std::to_string(i % 64) + "/node" + std::to_string(i / 64)
                        + "/data");
    }
    report("少量 ns × 多 node", measure(human, 0), 0);
    return 0;
}
