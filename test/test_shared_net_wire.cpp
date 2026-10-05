#include "dzIPC/common/channel_scope.h"
#include "dzIPC/net/wire_protocol.h"
#include "shared_net/test_vectors.h"
#include "gtest/gtest.h"
#include <algorithm>

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
