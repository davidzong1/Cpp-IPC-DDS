#pragma once

#include <cstdint>
#include <string>
#include "libipc/export.h"
#define UDP_DISCOVERY_BASE_PORT 11'451

namespace dzIPC::common {
IPC_EXPORT uint64_t fnv1a64(const std::string& data);

/* topic + domain 的 UDP 端口基址。
 *
 * 契约: **永远返回基址区间内的一个值, 不抛异常**(旧实现在越界时抛
 * runtime_error, 而它的调用点全是 socket 传输层的构造函数与嗅探器 —— 抛出来
 * 就是整个进程崩在"建连接"这一步, 且调用方无从降级)。越界输入被**折回**区间。
 *
 * 兼容性: 对修复前**能算出结果**的输入逐位不变(见下方 kUdpPortWindow 注释),
 * 所以新旧版本进程仍然落在同一个端口上 —— 只有修复前必然崩掉的输入拿到新端口。 */
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

/* 端口**基址**的合法区间: [UDP_DISCOVERY_BASE_PORT, kUdpPortBaseMax], 共 kUdpPortWindow 个。
 * 上界要让出 kUdpPortOffsetMax: 一个 topic 占的是 base .. base+kUdpPortOffsetMax 一整段,
 * 只管基址不越界是不够的 —— 基址贴着 65535 时 ACK 通道会静默回绕到低端口, 撞上别的 topic。
 *
 * kUdpPortWindow 同时是 udp_discovery_port_calculate() 的取模模数, 这不是巧合:
 * 端口值域 = base + domain_id × (fnv(topic) % 10000), 而 10000 × 2147483647 ≫ 54081,
 * 所以越界是**常态**而非边界情况(实测 domain=6 就有 9.9% 的 topic 名越界, domain=32 是 83%,
 * domain≥541 几乎全军覆没)。取模之所以能当"修复而不改映射"用, 是因为
 * 旧实现抛异常的条件恰好是 offset > kUdpPortWindow-1 —— 换句话讲, **凡旧版本能算出结果的
 * 输入, 其 offset 都 < kUdpPortWindow, 取模对它们是恒等变换**。逐位不变, 不是近似不变。 */
inline constexpr uint16_t kUdpPortBaseMax = static_cast<uint16_t>(65535 - kUdpPortOffsetMax);
inline constexpr uint16_t kUdpPortWindow = static_cast<uint16_t>(kUdpPortBaseMax - UDP_DISCOVERY_BASE_PORT + 1);
}   // namespace dzIPC::common