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
        uint64_t hash_value = static_cast<uint16_t>(dzIPC::common::fnv1a64(topic_name) % 10000);
        uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + domain_id * hash_value;
        /* 一个 topic 实际占用 base .. base+kUdpPortOffsetMax 这一段, 所以校验
         * 上界的是段尾而不是段首 —— 否则基址刚好落在 65535 附近时, ACK 通道会
         * 静默回绕到低端口, 撞上别的 topic。 */
        if (limited_port + dzIPC::common::kUdpPortOffsetMax > 65535)
        {
            throw std::runtime_error("Calculated port number exceeds the maximum allowed value of 65535. Please choose a different topic name or domain ID.");
        }
        return static_cast<uint16_t>(limited_port); // 确保端口号在有效范围内
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