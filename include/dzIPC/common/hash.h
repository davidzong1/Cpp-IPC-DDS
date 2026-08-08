#pragma once

#include <cstdint>
#include <string>
#include "libipc/export.h"
#define UDP_DISCOVERY_BASE_PORT 11'451

namespace dzIPC::common {
IPC_EXPORT uint64_t fnv1a64(const std::string& data);
IPC_EXPORT uint16_t udp_discovery_port_calculate(const std::string& topic_name, int domain_id);
IPC_EXPORT std::string udp_discovery_addr_calculate(const std::string& topic_name);

/* 一个 topic 占用的端口偏移。基址由 udp_discovery_port_calculate 给出。
 *
 * 0/1/2 是历史分配, 不能动 —— dzipc_topic_cat 这类外部嗅探工具按 0/+1 监听,
 * 改了它们会直接失聪。3/4 是端点分离新增的 ACK 回传通道。
 *
 * 为什么 ACK 要单独占端口: 数据 socket 加入了组播组且 IP_MULTICAST_LOOP 必须
 * 保持 1(否则同机其他进程收不到), 于是发送端自己的分片会全部回绕进它的接收
 * 队列, 把对端的 ACK 挤到几百个分片之后。ACK 走独立端口 + 发送端数据 socket
 * 不入组, 两者合起来才能让确认及时到达。 */
inline constexpr uint16_t kUdpPortOffsetData = 0;        // pub-sub 数据 / ser-cli 请求
inline constexpr uint16_t kUdpPortOffsetResponse = 1;    // ser-cli 响应
inline constexpr uint16_t kUdpPortOffsetHandshake = 2;   // ser-cli 握手
inline constexpr uint16_t kUdpPortOffsetAckData = 3;     // 对 offset 0 的 ACK/NACK
inline constexpr uint16_t kUdpPortOffsetAckResponse = 4; // 对 offset 1 的 ACK/NACK
inline constexpr uint16_t kUdpPortOffsetMax = kUdpPortOffsetAckResponse;
}   // namespace dzIPC::common