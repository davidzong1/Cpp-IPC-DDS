#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>       // std::numeric_limits
#include <new>
#include <utility>

namespace ipc {

// types

using byte_t = std::uint8_t;

/// \brief Index of a chunk inside a CHUNK_INFO shared segment; -1 is invalid.
/// Declared here (rather than only in the internal id_pool.h) so the public
/// loan API in ipc.h can name it.
using storage_id_t = std::int32_t;

template <std::size_t N>
struct uint;

template <> struct uint<8 > { using type = std::uint8_t ; };
template <> struct uint<16> { using type = std::uint16_t; };
template <> struct uint<32> { using type = std::uint32_t; };
template <> struct uint<64> { using type = std::uint64_t; };

template <std::size_t N>
using uint_t = typename uint<N>::type;

// constants

enum : std::uint32_t {
    invalid_value   = (std::numeric_limits<std::uint32_t>::max)(),
    default_timeout = 100, // ms
};

enum : std::size_t {
    data_length     = 64,
    sniffer_ring_slots = 256,
    sniffer_payload_limit = data_length * sniffer_ring_slots,
    large_msg_limit = data_length,
    large_msg_align = 1024,
    /* 每尺寸档 chunk 池容量。40 = 10(钉上限, 对齐 ROS 2 默认 QoS depth=10)
     * × 4(满钉订阅者余量) —— dzIPC::ViewQueueCap() 即取本值 / 4。
     * 调整时同步检查: id_pool::max_count = min(本值, uint8 上限 255);
     * 段名编码了本值(ipc.cpp get_info), 容量不同的段天然隔离不混挂;
     * UF-007/UF-011 判据的 4×kCap 魔数由本值导出, 不得写死。 */
    large_msg_cache = 40,
};

enum class relat { // multiplicity of the relationship
    single,
    multi
};

enum class trans { // transmission
    unicast,
    broadcast
};

// producer-consumer policy flag

template <relat Rp, relat Rc, trans Ts>
struct wr {};

template <typename WR>
struct relat_trait;

template <relat Rp, relat Rc, trans Ts>
struct relat_trait<wr<Rp, Rc, Ts>> {
    constexpr static bool is_multi_producer = (Rp == relat::multi);
    constexpr static bool is_multi_consumer = (Rc == relat::multi);
    constexpr static bool is_broadcast      = (Ts == trans::broadcast);
};

template <template <typename> class Policy, typename Flag>
struct relat_trait<Policy<Flag>> : relat_trait<Flag> {};

// the prefix tag of a channel
struct prefix {
    char const *str;
};

} // namespace ipc
