#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "dzIPC/common/hash.h"

namespace dzIPC::common {
// 不含消息类型：作用域负责通道隔离，msg_id 仍由原协议独立校验。
enum class ScopeKind : std::uint32_t { PubSub = 1, Service = 2 };
inline std::string channel_scope_key(const std::string& topic, std::uint64_t domain, ScopeKind kind) {
    return "DZSC2:" + std::to_string(static_cast<std::uint32_t>(kind)) + ":" +
           std::to_string(domain) + ":" + std::to_string(topic.size()) + ":" + topic;
}
inline std::array<std::uint8_t, 32> channel_scope_token(const std::string& topic, std::uint64_t domain, ScopeKind kind) {
    std::array<std::uint8_t, 32> out{};
    auto put = [&](unsigned offset, std::uint64_t value, unsigned bytes) {
        for(unsigned i=0;i<bytes;++i) out[offset+bytes-1-i]=static_cast<std::uint8_t>(value>>(i*8));
    };
    put(0,0x445A5332,4);put(4,static_cast<std::uint32_t>(kind),4);put(8,domain,8);
    const auto key=channel_scope_key(topic,domain,kind);
    put(16,fnv1a64(key),8);put(24,fnv1a64("identity:"+key),8);
    return out;
}
inline std::string channel_scope_suffix(const std::string& topic, std::uint64_t domain, ScopeKind kind) {
    const auto token=channel_scope_token(topic,domain,kind);
    constexpr char digits[]="0123456789abcdef";
    std::string out;out.reserve(32);
    for(unsigned i=16;i<32;++i){out+=digits[token[i]>>4];out+=digits[token[i]&15];}
    return out;
}
// 寻址使用 avalanche 混合，避免连续话题编号的 FNV 取模呈现结构性聚集。
inline std::uint64_t scope_address_mix(std::uint64_t h) {
    h^=h>>30;h*=0xbf58476d1ce4e5b9ULL;
    h^=h>>27;h*=0x94d049bb133111ebULL;
    return h^(h>>31);
}
inline std::uint16_t socket_scope_port(const std::string& topic, std::uint64_t domain, ScopeKind kind) {
    // 每个作用域使用连续五端口，避免其他作用域的基址与 ACK/握手偏移部分重叠。
    // 避开 Linux 默认临时端口 32768..60999；不用整段高端口空间。
    constexpr std::uint64_t slots=(32767-UDP_DISCOVERY_BASE_PORT+1)/(kUdpPortOffsetMax+1);
    return static_cast<std::uint16_t>(UDP_DISCOVERY_BASE_PORT+
        scope_address_mix(fnv1a64(channel_scope_key(topic,domain,kind)))%slots*(kUdpPortOffsetMax+1));
}
inline std::string socket_scope_address(const std::string& topic, std::uint64_t domain, ScopeKind kind) {
    const auto hash=scope_address_mix(fnv1a64("group:"+channel_scope_key(topic,domain,kind)));
    return "239.255."+std::to_string((hash>>8)&255)+"."+std::to_string(hash&255);
}
} // namespace dzIPC::common
