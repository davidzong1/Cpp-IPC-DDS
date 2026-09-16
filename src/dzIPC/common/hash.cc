#include "dzIPC/common/hash.h"
#include <iostream>
namespace dzIPC::common
{
    // 64-bit FNV-1a constants
    constexpr uint64_t kFnvOffsetBasis64 = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime64 = 1099511628211ull;
    uint64_t fnv1a64(const std::string &data)
    {
        uint64_t hash = kFnvOffsetBasis64;
        for (unsigned char c : data)
        {
            hash ^= static_cast<uint64_t>(c);
            hash *= kFnvPrime64;
        }
        return hash;
    }

    /******************************************************************************************************/
    /******************************************************************************************************/
    /******************************************************************************************************/
    uint16_t udp_discovery_port_calculate(const std::string &topic_name, int domain_id)
    {
        /* 10000 是端口公式的一半, 不是可调参数: 改它等于改**每一个** topic 的端口,
         * 新旧版本进程会各自绑到不同端口上并且都"成功"(组播不受影响), 表现为静默互不通。 */
        constexpr uint64_t kTopicPortSpan = 10000;

        const uint64_t hash_value = dzIPC::common::fnv1a64(topic_name) % kTopicPortSpan;
        const uint64_t offset = static_cast<uint64_t>(domain_id) * hash_value;
        const uint64_t base = UDP_DISCOVERY_BASE_PORT;
        const uint64_t window = dzIPC::common::kUdpPortWindow;

        /* 修复前这里是 throw std::runtime_error, 于是 domain_id ≥ 6 就已经有 topic
         * 名直接打死调用进程(实测 domain=6 为 9.9%, domain=32 为 83%, domain≥541 近 100%),
         * 而调用点全是 socket 传输层构造函数 —— 抛出来没有任何一处能降级处理。
         *
         * 判据是"越界就折回", 而折回用的取模**只在越界时起作用**: 旧实现抛异常的条件
         * 恰好是 offset ≥ window, 所以凡旧版本能返回结果的输入, 这里 offset < window,
         * `offset % window == offset` —— 逐位不变(见 hash.h 的 kUdpPortWindow 注释)。 */
        const uint64_t folded = offset < window ? offset : offset % window;
        /* base + folded ≤ base + window - 1 == 65535 - kUdpPortOffsetMax, 加上
         * kUdpPortOffsetMax 的 ACK 段也仍在 65535 内 —— 段尾校验由这里保证, 不再抛。 */
        return static_cast<uint16_t>(base + folded);
    }
    /******************************************************************************************************/
    /******************************************************************************************************/
    /******************************************************************************************************/
    std::string udp_discovery_addr_calculate(const std::string &topic_name)
    {
        std::string ip_prefix = "239.255.";
        std::string ip_mid = topic_name + "_mid";
        std::string ip_end = topic_name + "_end";
        uint64_t mid_hash_value = dzIPC::common::fnv1a64(ip_mid) % 254;
        uint64_t end_hash_value = dzIPC::common::fnv1a64(ip_end) % 254;
        return ip_prefix + std::to_string(static_cast<uint64_t>(mid_hash_value)) + "." + std::to_string(static_cast<uint64_t>(end_hash_value));
    }

}