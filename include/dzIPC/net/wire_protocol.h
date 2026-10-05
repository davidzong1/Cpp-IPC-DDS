#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dzIPC::net
{
using Bytes = std::vector<std::uint8_t>;
using Identity = std::array<std::uint8_t, 16>;
using Scope = std::array<std::uint8_t, 32>;
struct ByteView
{
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    ByteView() = default;
    ByteView(const void *p, std::size_t n) : data(static_cast<const std::uint8_t *>(p)), size(n)
    {
    }
    explicit ByteView(const Bytes &b) : data(b.data()), size(b.size())
    {
    }
};
enum class ProtocolCode
{
    Ok,
    BadLength,
    BadMagic,
    BadVersion,
    BadField,
    BadCrc,
    IdentityMismatch,
    TooLarge
};
struct ProtocolStatus
{
    ProtocolCode code = ProtocolCode::Ok;
    explicit operator bool() const noexcept
    {
        return code == ProtocolCode::Ok;
    }
};
inline constexpr std::size_t kWireHeaderSize = 160, kFragmentBytes = 1024;
inline constexpr std::uint32_t kMaxMessageBytes = 16 * 1024 * 1024;
enum class Encoding : std::uint8_t
{
    Tlv = 1,
    DzFlat = 2
};
enum class Delivery : std::uint8_t
{
    BestEffort = 0,
    Reliable = 1
};
enum class PacketKind : std::uint8_t
{
    Data = 1,
    Ack = 2,
    Nack = 3,
    Reject = 4
};
struct RouteKey
{
    Scope scope{};
    std::uint32_t msg_id = 0;
    bool operator==(const RouteKey &b) const noexcept
    {
        return scope == b.scope && msg_id == b.msg_id;
    }
    bool operator!=(const RouteKey &b) const noexcept
    {
        return !(*this == b);
    }
    bool operator<(const RouteKey &b) const noexcept
    {
        return scope < b.scope || (scope == b.scope && msg_id < b.msg_id);
    }
};
std::array<std::uint8_t, 36> route_bytes(const RouteKey &key) noexcept;
bool valid_scope(const Scope &scope) noexcept;
bool nonzero(const Identity &id) noexcept;
std::uint64_t route_hash(const RouteKey &key) noexcept;
std::uint32_t crc32c(ByteView bytes) noexcept;

struct WireHeader
{
    PacketKind kind = PacketKind::Data;
    RouteKey route;
    Identity source_id{}, publisher_id{}, target_id{};
    std::uint64_t source_epoch = 0, sequence = 0, target_epoch = 0;
    std::uint32_t message_size = 0, fragment_index = 0, fragment_count = 0;
    std::uint32_t message_crc = 0, schema_hash = 0;
    std::uint64_t receiver_route_epoch = 0;
    Encoding encoding = Encoding::Tlv;
    Delivery delivery = Delivery::BestEffort;
};
// 解码 view 借用输入缓冲，不能跨其生命周期；失败不改写输出。
ProtocolStatus encode_packet(const WireHeader &, ByteView payload, Bytes &out);
ProtocolStatus decode_packet(ByteView packet, WireHeader &out, ByteView &payload,
                             std::uint32_t max_message = kMaxMessageBytes);

struct DiscoveryHello
{
    Identity gateway_id{};
    std::uint64_t gateway_epoch = 0, snapshot_version = 0;
    std::uint16_t data_base_port = 24000, data_shards = 4, control_port = 24004;
    std::uint32_t max_message_bytes = kMaxMessageBytes;
};
ProtocolStatus encode_hello(const DiscoveryHello &, Bytes &out);
ProtocolStatus decode_hello(ByteView packet, DiscoveryHello &out);
enum class CatalogKind : std::uint8_t
{
    Request = 1,
    Page = 2
};
struct CatalogHeader
{
    CatalogKind kind = CatalogKind::Request;
    Identity source_id{}, target_id{};
    std::uint64_t source_epoch = 0, target_epoch = 0, snapshot_version = 0;
    std::uint32_t page_index = 0, page_count = 0, body_crc = 0;
};
ProtocolStatus encode_catalog(const CatalogHeader &, ByteView payload, Bytes &out);
ProtocolStatus decode_catalog(ByteView packet, CatalogHeader &out, ByteView &payload);

struct RouteDescriptor
{
    RouteKey key;
    std::uint32_t schema_hash = 0;
    std::uint64_t receiver_route_epoch = 0;
    std::uint16_t role_flags = 0;
    std::string topic;
};
// registration=true 时 role/epoch 均须为零；目录需角色 1/2/3。
ProtocolStatus encode_descriptor(const RouteDescriptor &, bool registration, Bytes &out);
ProtocolStatus decode_descriptor(ByteView bytes, bool registration, RouteDescriptor &out);
ProtocolStatus encode_directory(const std::vector<RouteDescriptor> &, Bytes &out);
ProtocolStatus decode_directory(ByteView bytes, std::vector<RouteDescriptor> &out,
                                std::uint32_t max_routes = 4096);
} // namespace dzIPC::net
