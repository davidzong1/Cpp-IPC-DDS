#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>       // std::numeric_limits
#include <new>
#include <utility>

namespace ipc {
// 1472 字节 UDP 预算中预留 32 字节作用域头；分片及 TLV 页共用此常量。
inline constexpr std::size_t wire_packet_size = 1408;


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
    // 所有 SHM 载荷强制使用每话题、每尺寸档 10 块池。
    topic_msg_cache = 10,
};

namespace detail {
// V5 队列 storage descriptor。序号隔离同一 ID 的不同借出，代次隔离不同段。
struct storage_token {
    storage_id_t wire_id = -1;
    std::uint64_t ticket = 0, generation = 0;
};
constexpr bool valid_storage(storage_id_t id) noexcept {
    return id >= 0 && id < static_cast<storage_id_t>(topic_msg_cache);
}
constexpr storage_id_t storage_to_wire(storage_id_t id) noexcept {
    return valid_storage(id) ? -2 - id : -1;
}
constexpr storage_id_t storage_from_wire(storage_id_t id) noexcept {
    return id <= -2 && id >= -1 - static_cast<storage_id_t>(topic_msg_cache) ? -2 - id : -1;
}
} // namespace detail

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
