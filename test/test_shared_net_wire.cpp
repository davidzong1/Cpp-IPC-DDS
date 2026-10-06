#include "dzIPC/common/channel_scope.h"
#include "dzIPC/net/wire_protocol.h"
#include "shared_net/test_vectors.h"
#include "gtest/gtest.h"
#include <algorithm>
#include "../src/dzIPC/net/byte_codec.h"

using namespace dzIPC::net;
using shared_net_test::vector;
TEST(SharedNetWire, CrcAndCanonicalRoute)
{
    EXPECT_EQ(crc32c(ByteView("123456789", 9)), 0xe3069283u);
    EXPECT_EQ(crc32c({}), 0u);
    RouteKey key{dzIPC::common::channel_scope_token("/golden/topic", 0x0102030405060708ull,
                                                    dzIPC::common::ScopeKind::PubSub),
                 0x11223344};
    auto canonical = route_bytes(key);
    EXPECT_EQ(Bytes(canonical.begin(), canonical.end()), vector("route_key"));
    auto other = key;
    other.msg_id = 0;
    EXPECT_NE(route_hash(key), route_hash(other));
}
TEST(SharedNetWire, EveryHeaderFieldMatchesIndependentGolden)
{
    for (const auto *name : {"dzmx_data", "dzmx_ack", "dzmx_nack", "dzmx_reject"})
    {
        auto b = vector(name);
        WireHeader h;
        ByteView payload;
        ASSERT_TRUE(decode_packet(ByteView(b), h, payload)) << name;
        EXPECT_EQ(h.route.msg_id, 0x11223344u);
        EXPECT_EQ(h.source_epoch, 0x1112131415161718ull);
        EXPECT_EQ(h.target_epoch, 0x2122232425262728ull);
        EXPECT_EQ(h.sequence, 0x3132333435363738ull);
        EXPECT_EQ(h.receiver_route_epoch, 0x4142434445464748ull);
        EXPECT_EQ(h.message_size, 1025u);
        EXPECT_EQ(h.fragment_count, 2u);
        EXPECT_EQ(h.schema_hash, 0u);
        EXPECT_EQ(h.message_crc, crc32c(ByteView(Bytes(1025, 0x7b))));
        EXPECT_EQ(h.encoding, Encoding::Tlv);
        EXPECT_EQ(h.delivery, Delivery::Reliable);
        for (unsigned i = 0; i < 16; ++i)
        {
            EXPECT_EQ(h.source_id[i], i);
            EXPECT_EQ(h.publisher_id[i], i + 16);
            EXPECT_EQ(h.target_id[i], i + 32);
        }
        const auto key = route_bytes(h.route);
        EXPECT_EQ(Bytes(key.begin(), key.end()), vector("route_key"));
        Bytes encoded;
        ASSERT_TRUE(encode_packet(h, payload, encoded));
        EXPECT_EQ(encoded, b);
    }
}
TEST(SharedNetWire, TruncationCorruptionAndUnknownFieldsRejected)
{
    const auto valid = vector("dzmx_data");
    WireHeader h;
    ByteView payload;
    for (std::size_t n = 0; n < valid.size(); ++n)
        EXPECT_FALSE(decode_packet(ByteView(valid.data(), n), h, payload));
    for (std::size_t i = 0; i < valid.size(); ++i)
    {
        auto b = valid;
        b[i] ^= 1;
        EXPECT_FALSE(decode_packet(ByteView(b), h, payload)) << i;
    }
    EXPECT_FALSE(decode_packet(ByteView(nullptr, 161), h, payload));
    EXPECT_FALSE(decode_packet(ByteView(valid), h, payload, 1024));
}
TEST(SharedNetWire, InvalidFragmentAndNackSemanticsRejectedBeforeEncode)
{
    auto valid = vector("dzmx_data");
    WireHeader h;
    ByteView payload;
    ASSERT_TRUE(decode_packet(ByteView(valid), h, payload));
    Bytes b{91};
    h.fragment_index = UINT32_MAX;
    EXPECT_FALSE(encode_packet(h, payload, b));
    EXPECT_EQ(b, Bytes{91});
    h.fragment_index = 1;
    h.fragment_count = UINT32_MAX;
    EXPECT_FALSE(encode_packet(h, payload, b));
    h.fragment_count = 2;
    h.kind = PacketKind::Nack;
    h.fragment_index = 0;
    for (const auto &nack : {Bytes{0, 0, 0, 0, 0, 0, 0, 0}, Bytes{0, 0, 0, 2}, Bytes{0, 0, 0}})
        EXPECT_FALSE(encode_packet(h, ByteView(nack), b));
    h.kind = PacketKind::Ack;
    h.delivery = Delivery::BestEffort;
    EXPECT_FALSE(encode_packet(h, {}, b));
    h.delivery = Delivery::Reliable;
    h.route.scope[7] = 2;
    EXPECT_FALSE(encode_packet(h, {}, b));
}
TEST(SharedNetWire, DiscoveryAndCatalogGolden)
{
    auto b = vector("dzgd_hello");
    DiscoveryHello hello;
    ASSERT_TRUE(decode_hello(ByteView(b), hello));
    EXPECT_EQ(hello.gateway_epoch, 0x1112131415161718ull);
    EXPECT_EQ(hello.data_base_port, 24000);
    EXPECT_EQ(hello.data_shards, 4);
    EXPECT_EQ(hello.control_port, 24004);
    EXPECT_EQ(hello.snapshot_version, 7u);
    EXPECT_EQ(hello.max_message_bytes, 16777216u);
    Bytes encoded;
    ASSERT_TRUE(encode_hello(hello, encoded));
    EXPECT_EQ(encoded, b);
    for (std::size_t n = 0; n < 64; ++n)
        EXPECT_FALSE(decode_hello(ByteView(b.data(), n), hello));
    for (const auto *name : {"dzgc_request", "dzgc_page", "dzgc_empty_page"})
    {
        b = vector(name);
        CatalogHeader h;
        ByteView p;
        ASSERT_TRUE(decode_catalog(ByteView(b), h, p));
        EXPECT_EQ(h.source_epoch, 0x1112131415161718ull);
        EXPECT_EQ(h.target_epoch, 0x2122232425262728ull);
        EXPECT_EQ(h.snapshot_version, 7u);
        ASSERT_TRUE(encode_catalog(h, p, encoded));
        EXPECT_EQ(encoded, b);
        for (std::size_t n = 0; n < b.size(); ++n)
            EXPECT_FALSE(decode_catalog(ByteView(b.data(), n), h, p));
        if (h.kind == CatalogKind::Page)
        {
            h.page_count = 8193;
            EXPECT_FALSE(encode_catalog(h, p, encoded));
        }
    }
}
TEST(SharedNetWire, DirectoryRolesFullNamesAndStrictOrder)
{
    for (const auto *name : {"directory_empty", "directory_pub", "directory_sub", "directory_both"})
    {
        auto b = vector(name);
        std::vector<RouteDescriptor> routes;
        ASSERT_TRUE(decode_directory(ByteView(b), routes));
        Bytes encoded;
        ASSERT_TRUE(encode_directory(routes, encoded));
        EXPECT_EQ(encoded, b);
        b.push_back(0);
        EXPECT_FALSE(decode_directory(ByteView(b), routes));
        if (!routes.empty())
        {
            auto d = routes.front();
            d.topic += "collision";
            EXPECT_EQ(encode_descriptor(d, false, encoded).code, ProtocolCode::IdentityMismatch);
            d = routes.front();
            d.role_flags = 1;
            d.receiver_route_epoch = 1;
            EXPECT_FALSE(encode_descriptor(d, false, encoded));
            routes.push_back(routes.front());
            EXPECT_FALSE(encode_directory(routes, encoded));
        }
    }
    Bytes too_many{255, 255, 255, 255};
    std::vector<RouteDescriptor> routes;
    EXPECT_FALSE(decode_directory(ByteView(too_many), routes));
    RouteDescriptor empty_name;
    empty_name.key.scope =
        dzIPC::common::channel_scope_token("", 0, dzIPC::common::ScopeKind::PubSub);
    empty_name.role_flags = 1;
    Bytes encoded;
    ASSERT_TRUE(encode_directory({empty_name}, encoded));
    ASSERT_TRUE(decode_directory(ByteView(encoded), routes));
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_TRUE(routes[0].topic.empty());
}

TEST(SharedNetWire, AcceleratedAndPortableCrcMatchIndependentBitwiseReference) {
    const auto reference = [](ByteView bytes, std::size_t zero) {
        std::uint32_t c = 0xffffffff;
        for (std::size_t i = 0; i < bytes.size; ++i) {
            c ^= i >= zero && i - zero < 4 ? 0 : bytes.data[i];
            for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ (c & 1 ? 0x82f63b78u : 0);
        }
        return c ^ 0xffffffff;
    };
    Bytes storage(4096 + 16); std::uint32_t seed = 19;
    for (auto& byte : storage) { seed = seed * 1664525u + 1013904223u; byte = seed >> 24; }
    for (std::size_t alignment = 0; alignment < 8; ++alignment)
        for (const std::size_t size : {0u,1u,3u,4u,7u,8u,9u,15u,16u,31u,32u,159u,160u,1024u,1184u,4096u}) {
            ByteView bytes(storage.data() + alignment, size);
            for (const std::size_t zero : {std::size_t{0}, std::size_t{5}, std::size_t{7}, size ? size - 1 : 0, size, SIZE_MAX}) {
                const auto expected = reference(bytes, zero);
                EXPECT_EQ(codec::packet_crc(bytes, zero), expected) << alignment << '/' << size << '/' << zero;
                EXPECT_EQ(codec::packet_crc_portable(bytes, zero), expected) << alignment << '/' << size << '/' << zero;
            }
        }
    EXPECT_EQ(codec::packet_crc({nullptr, 7}, 0), 0u);
    EXPECT_EQ(codec::packet_crc_portable({}, SIZE_MAX), 0u);
}

TEST(SharedNetWire, V2PacketHelloCatalogAndDirectoryAreExplicit)
{
    auto v1 = vector("dzmx_data");
    WireHeader h;
    ByteView payload;
    ASSERT_TRUE(decode_packet(ByteView(v1), h, payload));
    h.data_source_endpoint_epoch = 0x5152535455565758ull;
    h.data_target_endpoint_epoch = 0x6162636465666768ull;
    Bytes encoded;
    ASSERT_TRUE(encode_packet_v2(h, payload, encoded));
    EXPECT_EQ(encoded.size(), kWireHeaderV2Size + payload.size);
    EXPECT_EQ(encoded.size(), 177u);
    WireHeader decoded;
    ByteView decoded_payload;
    ASSERT_TRUE(decode_packet_v2(ByteView(encoded), decoded, decoded_payload));
    EXPECT_EQ(decoded.data_source_endpoint_epoch, h.data_source_endpoint_epoch);
    EXPECT_EQ(decoded.data_target_endpoint_epoch, h.data_target_endpoint_epoch);
    EXPECT_EQ(Bytes(decoded_payload.data, decoded_payload.data + decoded_payload.size),
              Bytes(payload.data, payload.data + payload.size));
    EXPECT_EQ(decode_packet(ByteView(encoded), decoded, decoded_payload).code, ProtocolCode::BadVersion);
    EXPECT_FALSE(decode_packet_v2(ByteView(v1), decoded, decoded_payload));

    DiscoveryHelloV2 hello;
    hello.gateway_id = h.source_id;
    hello.gateway_epoch = h.source_epoch;
    hello.snapshot_version = 7;
    hello.control_port = 24004;
    Bytes hello_bytes;
    ASSERT_TRUE(encode_hello_v2(hello, hello_bytes));
    EXPECT_EQ(hello_bytes[4], 2);
    EXPECT_EQ(hello_bytes[32], 0);
    EXPECT_EQ(hello_bytes[34], 0);
    DiscoveryHelloV2 hello_roundtrip;
    ASSERT_TRUE(decode_hello_v2(ByteView(hello_bytes), hello_roundtrip));
    EXPECT_EQ(hello_roundtrip.capabilities, 3);
    DiscoveryHello legacy_hello;
    EXPECT_EQ(decode_hello(ByteView(hello_bytes), legacy_hello).code, ProtocolCode::BadVersion);

    CatalogHeader catalog;
    catalog.kind = CatalogKind::Page;
    catalog.source_id = h.source_id;
    catalog.target_id = h.target_id;
    catalog.source_epoch = h.source_epoch;
    catalog.target_epoch = h.target_epoch;
    catalog.snapshot_version = 7;
    catalog.page_index = 0;
    catalog.page_count = 1;
    Bytes catalog_bytes;
    ASSERT_TRUE(encode_catalog_v2(catalog, ByteView(v1.data(), 100), catalog_bytes));
    CatalogHeader catalog_roundtrip;
    ByteView catalog_payload;
    ASSERT_TRUE(decode_catalog_v2(ByteView(catalog_bytes), catalog_roundtrip, catalog_payload));
    EXPECT_EQ(catalog_roundtrip.snapshot_version, 7u);
    EXPECT_EQ(decode_catalog(ByteView(catalog_bytes), catalog_roundtrip, catalog_payload).code,
              ProtocolCode::BadVersion);

    RouteDescriptor route;
    route.topic = "/v2/topic";
    route.key.scope = dzIPC::common::channel_scope_token(route.topic, 0, dzIPC::common::ScopeKind::PubSub);
    route.key.msg_id = 71;
    route.role_flags = 3;
    route.receiver_route_epoch = 9;
    route.data_port = 31001;
    route.endpoint_epoch = 17;
    route.endpoint_flags = 1;
    Bytes directory;
    ASSERT_TRUE(encode_directory_v2({route}, directory));
    EXPECT_EQ(directory.size(), 4u + 64u + route.topic.size());
    std::vector<RouteDescriptor> routes;
    ASSERT_TRUE(decode_directory_v2(ByteView(directory), routes));
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_EQ(routes.front().data_port, 31001u);
    EXPECT_EQ(routes.front().endpoint_epoch, 17u);
    EXPECT_EQ(routes.front().endpoint_flags, 1u);
    auto malformed = directory;
    malformed[54] = 2;
    EXPECT_FALSE(decode_directory_v2(ByteView(malformed), routes));

    auto pooled = route; pooled.endpoint_flags = 0; pooled.data_port = 31001; pooled.endpoint_epoch = 21;
    auto pooled_peer = pooled; pooled_peer.key.msg_id = 72; pooled_peer.data_port = 31001;
    ASSERT_TRUE(encode_directory_v2({pooled, pooled_peer}, directory));
    pooled_peer.endpoint_epoch = 22;
    EXPECT_FALSE(encode_directory_v2({pooled, pooled_peer}, directory));
    pooled_peer = pooled; pooled_peer.key.msg_id = 72; pooled_peer.data_port = 31002;
    EXPECT_FALSE(encode_directory_v2({pooled, pooled_peer}, directory));

    auto dedicated = route; dedicated.endpoint_flags = 1; dedicated.data_port = 31001; dedicated.endpoint_epoch = 31;
    auto dedicated_peer = dedicated; dedicated_peer.key.msg_id = 72; dedicated_peer.endpoint_epoch = 32;
    EXPECT_FALSE(encode_directory_v2({dedicated, dedicated_peer}, directory));
    dedicated_peer.data_port = 31002; dedicated_peer.endpoint_epoch = 31;
    EXPECT_FALSE(encode_directory_v2({dedicated, dedicated_peer}, directory));

    pooled_peer = pooled; pooled_peer.key.msg_id = 72; pooled_peer.data_port = 31002; pooled_peer.endpoint_epoch = 22;
    ASSERT_TRUE(encode_directory_v2({pooled, pooled_peer}, directory));
    const auto second = 4 + 64 + route.topic.size();
    directory[second + 52] = static_cast<std::uint8_t>(pooled.data_port >> 8);
    directory[second + 53] = static_cast<std::uint8_t>(pooled.data_port);
    EXPECT_FALSE(decode_directory_v2(ByteView(directory), routes));
}
