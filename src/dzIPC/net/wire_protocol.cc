#include "dzIPC/net/wire_protocol.h"
#include "byte_codec.h"
#include "dzIPC/common/channel_scope.h"

namespace dzIPC::net
{
using namespace codec;
namespace
{
const std::array<std::uint32_t, 256> &crc_table()
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (unsigned i = 0; i < result.size(); ++i)
        {
            auto x = i;
            for (unsigned j = 0; j < 8; ++j)
                x = (x >> 1) ^ ((x & 1) ? 0x82f63b78U : 0);
            result[i] = x;
        }
        return result;
    }();
    return table;
}
ProtocolStatus validate_packet(const WireHeader &h, ByteView p, std::uint32_t max_message)
{
    if (!valid(p) || p.size > kFragmentBytes)
        return error(ProtocolCode::BadLength);
    if (!valid_scope(h.route.scope) || !nonzero(h.source_id) || !nonzero(h.target_id) ||
        !nonzero(h.publisher_id) || !h.source_epoch || !h.target_epoch || !h.sequence ||
        !h.receiver_route_epoch ||
        (h.encoding != Encoding::Tlv && h.encoding != Encoding::DzFlat) ||
        (h.delivery != Delivery::BestEffort && h.delivery != Delivery::Reliable) ||
        (h.encoding == Encoding::Tlv && h.schema_hash != 0) ||
        (h.encoding == Encoding::DzFlat && h.schema_hash == 0))
        return error(ProtocolCode::BadField);
    if (!h.message_size || h.message_size > max_message || h.message_size > kMaxMessageBytes)
        return error(ProtocolCode::TooLarge);
    if (h.fragment_count != (std::uint64_t(h.message_size) + 1023) / 1024)
        return error(ProtocolCode::BadField);
    if (h.kind == PacketKind::Data)
    {
        if (h.fragment_index >= h.fragment_count)
            return error(ProtocolCode::BadField);
        const auto offset = std::uint64_t(h.fragment_index) * 1024;
        if (p.size != std::min<std::uint64_t>(1024, h.message_size - offset))
            return error(ProtocolCode::BadLength);
    }
    else
    {
        if (h.fragment_index || h.delivery != Delivery::Reliable)
            return error(ProtocolCode::BadField);
        switch (h.kind)
        {
        case PacketKind::Ack:
            if (p.size)
                return error(ProtocolCode::BadLength);
            break;
        case PacketKind::Nack:
            if (!p.size || p.size % 4)
                return error(ProtocolCode::BadLength);
            for (std::size_t i = 0; i < p.size; i += 4)
                if (get(p.data + i, 4) >= h.fragment_count ||
                    (i && get(p.data + i, 4) <= get(p.data + i - 4, 4)))
                    return error(ProtocolCode::BadField);
            break;
        case PacketKind::Reject:
            if (p.size != 4)
                return error(ProtocolCode::BadLength);
            if (get(p.data, 4) < 1 || get(p.data, 4) > 7)
                return error(ProtocolCode::BadField);
            break;
        default:
            return error(ProtocolCode::BadField);
        }
    }
    return {};
}
ProtocolStatus validate_hello(const DiscoveryHello &h)
{
    if (!nonzero(h.gateway_id) || !h.gateway_epoch || !h.data_base_port || h.data_shards < 1 ||
        h.data_shards > 16 || std::uint32_t(h.data_base_port) + h.data_shards - 1 > 65535 ||
        !h.control_port ||
        (h.control_port >= h.data_base_port && h.control_port - h.data_base_port < h.data_shards) ||
        !h.max_message_bytes || h.max_message_bytes > kMaxMessageBytes)
        return error(ProtocolCode::BadField);
    return {};
}
ProtocolStatus validate_catalog(const CatalogHeader &h, ByteView p)
{
    if (!valid(p) || p.size > 1024)
        return error(ProtocolCode::BadLength);
    if (!nonzero(h.source_id) || !nonzero(h.target_id) || !h.source_epoch || !h.target_epoch)
        return error(ProtocolCode::BadField);
    if (h.kind == CatalogKind::Request)
    {
        if (p.size || h.page_index || h.page_count || h.body_crc)
            return error(ProtocolCode::BadField);
    }
    else if (h.kind == CatalogKind::Page)
    {
        if (!h.page_count || h.page_count > 8192 || h.page_index >= h.page_count)
            return error(ProtocolCode::BadField);
        if (!p.size || (h.page_index + 1 < h.page_count && p.size != 1024))
            return error(ProtocolCode::BadLength);
    }
    else
        return error(ProtocolCode::BadField);
    return {};
}
ProtocolStatus validate_descriptor(const RouteDescriptor &d, bool registration)
{
    if (d.topic.size() > 1024 || d.topic.find('\0') != std::string::npos ||
        !valid_scope(d.key.scope))
        return error(ProtocolCode::BadField);
    const auto scope = common::channel_scope_token(d.topic, get(d.key.scope.data() + 8, 8),
                                                   common::ScopeKind::PubSub);
    if (scope != d.key.scope)
        return error(ProtocolCode::IdentityMismatch);
    if (registration ? (d.role_flags || d.receiver_route_epoch)
                     : (d.role_flags < 1 || d.role_flags > 3 ||
                        bool(d.role_flags & 2) != bool(d.receiver_route_epoch)))
        return error(ProtocolCode::BadField);
    return {};
}
} // namespace
std::uint32_t codec::packet_crc(ByteView b, std::size_t zero_offset) noexcept
{
    if (!valid(b))
        return 0;
    const auto &table = crc_table();
    std::uint32_t c = 0xffffffff;
    for (std::size_t i = 0; i < b.size; ++i)
    {
        const auto x = i >= zero_offset && i - zero_offset < 4 ? 0 : b.data[i];
        c = table[(c ^ x) & 255] ^ (c >> 8);
    }
    return c ^ 0xffffffff;
}
std::uint32_t crc32c(ByteView b) noexcept
{
    return packet_crc(b, SIZE_MAX);
}
bool nonzero(const Identity &id) noexcept
{
    return !zeros(id.data(), id.size());
}
bool valid_scope(const Scope &s) noexcept
{
    return std::memcmp(s.data(), "DZS2", 4) == 0 && get(s.data() + 4, 4) == 1;
}
std::array<std::uint8_t, 36> route_bytes(const RouteKey &key) noexcept
{
    std::array<std::uint8_t, 36> result{};
    std::copy(key.scope.begin(), key.scope.end(), result.begin());
    put(result.data() + 32, key.msg_id, 4);
    return result;
}
std::uint64_t route_hash(const RouteKey &key) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (auto b : route_bytes(key))
    {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}
ProtocolStatus encode_packet(const WireHeader &h, ByteView p, Bytes &out)
{
    auto s = validate_packet(h, p, kMaxMessageBytes);
    if (!s)
        return s;
    Bytes b(160 + p.size);
    auto *d = b.data();
    std::memcpy(d, "DZMX", 4);
    d[4] = 1;
    d[5] = static_cast<std::uint8_t>(h.kind);
    put(d + 8, 160, 2);
    put(d + 10, p.size, 2);
    std::copy(h.route.scope.begin() + 4, h.route.scope.end(), d + 12);
    std::copy(h.source_id.begin(), h.source_id.end(), d + 40);
    put(d + 56, h.source_epoch, 8);
    std::copy(h.publisher_id.begin(), h.publisher_id.end(), d + 64);
    put(d + 80, h.sequence, 8);
    std::copy(h.target_id.begin(), h.target_id.end(), d + 88);
    put(d + 104, h.target_epoch, 8);
    put(d + 112, h.message_size, 4);
    put(d + 116, h.fragment_index, 4);
    put(d + 120, h.fragment_count, 4);
    put(d + 124, h.message_crc, 4);
    put(d + 128, h.route.msg_id, 4);
    put(d + 132, h.schema_hash, 4);
    put(d + 136, h.receiver_route_epoch, 8);
    d[148] = static_cast<std::uint8_t>(h.encoding);
    d[149] = static_cast<std::uint8_t>(h.delivery);
    if (p.size)
        std::memcpy(d + 160, p.data, p.size);
    put(d + 144, packet_crc(ByteView(b), 144), 4);
    out.swap(b);
    return {};
}
ProtocolStatus decode_packet(ByteView b, WireHeader &out, ByteView &payload,
                             std::uint32_t max_message)
{
    if (!valid(b) || b.size < 160 || b.size > 1184)
        return error(ProtocolCode::BadLength);
    const auto *d = b.data;
    if (std::memcmp(d, "DZMX", 4))
        return error(ProtocolCode::BadMagic);
    if (d[4] != 1)
        return error(ProtocolCode::BadVersion);
    if (get(d + 8, 2) != 160 || get(d + 10, 2) != b.size - 160)
        return error(ProtocolCode::BadLength);
    if (get(d + 6, 2) || !zeros(d + 150, 10))
        return error(ProtocolCode::BadField);
    if (packet_crc(b, 144) != get(d + 144, 4))
        return error(ProtocolCode::BadCrc);
    WireHeader h;
    h.kind = static_cast<PacketKind>(d[5]);
    std::memcpy(h.route.scope.data(), "DZS2", 4);
    std::copy_n(d + 12, 28, h.route.scope.begin() + 4);
    copy(h.source_id, d + 40);
    h.source_epoch = get(d + 56, 8);
    copy(h.publisher_id, d + 64);
    h.sequence = get(d + 80, 8);
    copy(h.target_id, d + 88);
    h.target_epoch = get(d + 104, 8);
    h.message_size = get(d + 112, 4);
    h.fragment_index = get(d + 116, 4);
    h.fragment_count = get(d + 120, 4);
    h.message_crc = get(d + 124, 4);
    h.route.msg_id = get(d + 128, 4);
    h.schema_hash = get(d + 132, 4);
    h.receiver_route_epoch = get(d + 136, 8);
    h.encoding = static_cast<Encoding>(d[148]);
    h.delivery = static_cast<Delivery>(d[149]);
    const ByteView p(d + 160, b.size - 160);
    auto s = validate_packet(h, p, max_message);
    if (!s)
        return s;
    out = h;
    payload = p;
    return {};
}
ProtocolStatus encode_hello(const DiscoveryHello &h, Bytes &out)
{
    auto s = validate_hello(h);
    if (!s)
        return s;
    Bytes b(64);
    auto *d = b.data();
    std::memcpy(d, "DZGD", 4);
    d[4] = d[5] = 1;
    put(d + 6, 64, 2);
    std::copy(h.gateway_id.begin(), h.gateway_id.end(), d + 8);
    put(d + 24, h.gateway_epoch, 8);
    put(d + 32, h.data_base_port, 2);
    put(d + 34, h.data_shards, 2);
    put(d + 36, h.control_port, 2);
    put(d + 38, 1, 2);
    put(d + 40, h.snapshot_version, 8);
    put(d + 48, h.max_message_bytes, 4);
    put(d + 52, packet_crc(ByteView(b), 52), 4);
    out.swap(b);
    return {};
}
ProtocolStatus decode_hello(ByteView b, DiscoveryHello &out)
{
    if (!valid(b) || b.size != 64)
        return error(ProtocolCode::BadLength);
    const auto *d = b.data;
    if (std::memcmp(d, "DZGD", 4))
        return error(ProtocolCode::BadMagic);
    if (d[4] != 1)
        return error(ProtocolCode::BadVersion);
    if (d[5] != 1 || get(d + 6, 2) != 64 || get(d + 38, 2) != 1 || !zeros(d + 56, 8))
        return error(ProtocolCode::BadField);
    if (packet_crc(b, 52) != get(d + 52, 4))
        return error(ProtocolCode::BadCrc);
    DiscoveryHello h;
    copy(h.gateway_id, d + 8);
    h.gateway_epoch = get(d + 24, 8);
    h.data_base_port = get(d + 32, 2);
    h.data_shards = get(d + 34, 2);
    h.control_port = get(d + 36, 2);
    h.snapshot_version = get(d + 40, 8);
    h.max_message_bytes = get(d + 48, 4);
    auto s = validate_hello(h);
    if (s)
        out = h;
    return s;
}
ProtocolStatus encode_catalog(const CatalogHeader &h, ByteView p, Bytes &out)
{
    auto s = validate_catalog(h, p);
    if (!s)
        return s;
    Bytes b(84 + p.size);
    auto *d = b.data();
    std::memcpy(d, "DZGC", 4);
    d[4] = 1;
    d[5] = static_cast<std::uint8_t>(h.kind);
    put(d + 6, 84, 2);
    put(d + 8, p.size, 2);
    std::copy(h.source_id.begin(), h.source_id.end(), d + 12);
    put(d + 28, h.source_epoch, 8);
    std::copy(h.target_id.begin(), h.target_id.end(), d + 36);
    put(d + 52, h.target_epoch, 8);
    put(d + 60, h.snapshot_version, 8);
    put(d + 68, h.page_index, 4);
    put(d + 72, h.page_count, 4);
    put(d + 76, h.body_crc, 4);
    if (p.size)
        std::memcpy(d + 84, p.data, p.size);
    put(d + 80, packet_crc(ByteView(b), 80), 4);
    out.swap(b);
    return {};
}
ProtocolStatus decode_catalog(ByteView b, CatalogHeader &out, ByteView &payload)
{
    if (!valid(b) || b.size < 84 || b.size > 1108)
        return error(ProtocolCode::BadLength);
    const auto *d = b.data;
    if (std::memcmp(d, "DZGC", 4))
        return error(ProtocolCode::BadMagic);
    if (d[4] != 1)
        return error(ProtocolCode::BadVersion);
    if (get(d + 6, 2) != 84 || get(d + 8, 2) != b.size - 84)
        return error(ProtocolCode::BadLength);
    if (get(d + 10, 2))
        return error(ProtocolCode::BadField);
    if (packet_crc(b, 80) != get(d + 80, 4))
        return error(ProtocolCode::BadCrc);
    CatalogHeader h;
    h.kind = static_cast<CatalogKind>(d[5]);
    copy(h.source_id, d + 12);
    h.source_epoch = get(d + 28, 8);
    copy(h.target_id, d + 36);
    h.target_epoch = get(d + 52, 8);
    h.snapshot_version = get(d + 60, 8);
    h.page_index = get(d + 68, 4);
    h.page_count = get(d + 72, 4);
    h.body_crc = get(d + 76, 4);
    const ByteView p(d + 84, b.size - 84);
    auto s = validate_catalog(h, p);
    if (!s)
        return s;
    out = h;
    payload = p;
    return {};
}
ProtocolStatus encode_descriptor(const RouteDescriptor &d, bool registration, Bytes &out)
{
    auto s = validate_descriptor(d, registration);
    if (!s)
        return s;
    Bytes b(52 + d.topic.size());
    std::copy(d.key.scope.begin(), d.key.scope.end(), b.begin());
    put(b.data() + 32, d.key.msg_id, 4);
    put(b.data() + 36, d.schema_hash, 4);
    put(b.data() + 40, d.receiver_route_epoch, 8);
    put(b.data() + 48, d.topic.size(), 2);
    put(b.data() + 50, d.role_flags, 2);
    std::copy(d.topic.begin(), d.topic.end(), b.begin() + 52);
    out.swap(b);
    return {};
}
ProtocolStatus decode_descriptor(ByteView b, bool registration, RouteDescriptor &out)
{
    if (!valid(b) || b.size < 52 || b.size != 52 + get(b.data + 48, 2))
        return error(ProtocolCode::BadLength);
    RouteDescriptor d;
    copy(d.key.scope, b.data);
    d.key.msg_id = get(b.data + 32, 4);
    d.schema_hash = get(b.data + 36, 4);
    d.receiver_route_epoch = get(b.data + 40, 8);
    d.role_flags = get(b.data + 50, 2);
    if (b.size > 1076)
        return error(ProtocolCode::TooLarge);
    d.topic.assign(reinterpret_cast<const char *>(b.data + 52), b.size - 52);
    auto s = validate_descriptor(d, registration);
    if (s)
        out = std::move(d);
    return s;
}
ProtocolStatus encode_directory(const std::vector<RouteDescriptor> &routes, Bytes &out)
{
    if (routes.size() > 4096)
        return error(ProtocolCode::TooLarge);
    Bytes b;
    append(b, routes.size(), 4);
    for (std::size_t i = 0; i < routes.size(); ++i)
    {
        if (i && !(routes[i - 1].key < routes[i].key))
            return error(ProtocolCode::BadField);
        Bytes entry;
        auto s = encode_descriptor(routes[i], false, entry);
        if (!s)
            return s;
        b.insert(b.end(), entry.begin(), entry.end());
    }
    out.swap(b);
    return {};
}
ProtocolStatus decode_directory(ByteView b, std::vector<RouteDescriptor> &out,
                                std::uint32_t max_routes)
{
    if (!valid(b) || b.size < 4 || b.size > 8 * 1024 * 1024)
        return error(ProtocolCode::BadLength);
    const auto count = get(b.data, 4);
    if (count > max_routes || count > 4096 || count > (b.size - 4) / 52)
        return error(ProtocolCode::TooLarge);
    std::vector<RouteDescriptor> routes;
    routes.reserve(count);
    std::size_t pos = 4;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        if (b.size - pos < 52)
            return error(ProtocolCode::BadLength);
        const auto length = 52 + get(b.data + pos + 48, 2);
        if (length > b.size - pos)
            return error(ProtocolCode::BadLength);
        RouteDescriptor d;
        auto s = decode_descriptor(ByteView(b.data + pos, length), false, d);
        if (!s)
            return s;
        if (!routes.empty() && !(routes.back().key < d.key))
            return error(ProtocolCode::BadField);
        routes.push_back(std::move(d));
        pos += length;
    }
    if (pos != b.size)
        return error(ProtocolCode::BadLength);
    out.swap(routes);
    return {};
}
} // namespace dzIPC::net
