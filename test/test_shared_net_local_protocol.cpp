#include "dzIPC/net/local_protocol.h"
#include "shared_net/test_vectors.h"
#include "gtest/gtest.h"
#include <cstdio>
using namespace dzIPC::net;
using shared_net_test::vector;

TEST(SharedNetLocal, EveryV2KindMatchesIndependentGolden)
{
    for (unsigned kind = 1; kind <= 24; ++kind)
    {
        if (kind == 15 || kind == 16)
            continue;
        char name[16];
        std::snprintf(name, sizeof(name), "dzlc_%02u", kind);
        auto b = vector(name);
        LocalHeader h;
        ByteView body;
        ASSERT_TRUE(decode_local(ByteView(b), h, body)) << kind;
        EXPECT_EQ(static_cast<unsigned>(h.kind), kind);
        EXPECT_EQ(h.request_id, kind == 21 || kind == 22 ? 0 : 0x6162636465666768ull);
        EXPECT_EQ(h.session_id, kind == 1 ? 0 : 0x5152535455565758ull);
        EXPECT_EQ(h.gateway_epoch, kind == 1 ? 0 : 0x1112131415161718ull);
        Bytes encoded;
        ASSERT_TRUE(encode_local(h, body, encoded));
        EXPECT_EQ(encoded, b);
        for (std::size_t n = 0; n < b.size(); ++n)
            EXPECT_FALSE(decode_local(ByteView(b.data(), n), h, body)) << kind << ':' << n;
        b.push_back(0);
        EXPECT_FALSE(decode_local(ByteView(b), h, body));
    }
}
TEST(SharedNetLocal, OldBeginVersionsAndUnknownFlagsRejected)
{
    const auto b = vector("dzlc_03");
    LocalHeader h;
    ByteView body;
    for (auto kind : {0, 15, 16, 25})
    {
        auto bad = b;
        bad[7] = kind;
        EXPECT_FALSE(decode_local(ByteView(bad), h, body));
    }
    auto bad = b;
    bad[5] = 1;
    EXPECT_EQ(decode_local(ByteView(bad), h, body).code, ProtocolCode::BadVersion);
    bad = b;
    bad[15] = 1;
    EXPECT_FALSE(decode_local(ByteView(bad), h, body));
    bad = b;
    std::fill(bad.begin() + 24, bad.begin() + 32, 0);
    EXPECT_FALSE(decode_local(ByteView(bad), h, body));
}
TEST(SharedNetLocal, WelcomeAndTwoDistinctCumulativeCounters)
{
    auto b = vector("dzlc_02");
    LocalHeader h;
    ByteView body;
    ASSERT_TRUE(decode_local(ByteView(b), h, body));
    WelcomeBody w;
    ASSERT_TRUE(decode_welcome(body, w));
    EXPECT_EQ(w.outbox_limit_bytes, 33554432u);
    EXPECT_EQ(w.outbox_record_limit, 256u);
    EXPECT_EQ(w.granted_bytes, 1048576u);
    EXPECT_EQ(w.granted_records, 16u);
    Bytes encoded;
    ASSERT_TRUE(encode_welcome(w, encoded));
    EXPECT_EQ(encoded, Bytes(body.data, body.data + body.size));
    EXPECT_EQ(w.tx_name, outbox_name(w.locality, h.gateway_epoch, h.session_id));
    auto wrong_session = h;
    ++wrong_session.session_id;
    Bytes wrong_packet;
    EXPECT_FALSE(encode_local(wrong_session, ByteView(encoded), wrong_packet));
    w.granted_bytes = w.granted_records = 0;
    EXPECT_TRUE(encode_welcome(w, encoded));
    w.outbox_limit_bytes = 16777328;
    EXPECT_FALSE(encode_welcome(w, encoded));
    b = vector("dzlc_21");
    ASSERT_TRUE(decode_local(ByteView(b), h, body));
    CreditCounters progress;
    ASSERT_TRUE(decode_tx_progress(body, progress));
    EXPECT_EQ(progress.records, 9u);
    EXPECT_EQ(progress.bytes, 33554432u);
    EXPECT_EQ(encode_tx_progress(progress), Bytes(body.data, body.data + body.size));
    b = vector("dzlc_24");
    ASSERT_TRUE(decode_local(ByteView(b), h, body));
    CreditCounters grant;
    ASSERT_TRUE(decode_credit_grant(body, grant));
    EXPECT_EQ(grant.bytes, 67108864u);
    EXPECT_EQ(grant.records, 1024u);
    EXPECT_EQ(encode_credit_grant(grant), Bytes(body.data, body.data + body.size));
    h.request_id = 0;
    EXPECT_TRUE(encode_local(h, body, encoded)); // 主动补充也合法
}
TEST(SharedNetLocal, RouteStateAndRemoteResultFlags)
{
    auto b = vector("dzlc_22");
    LocalHeader h;
    ByteView body;
    ASSERT_TRUE(decode_local(ByteView(b), h, body));
    RouteStateBody r;
    ASSERT_TRUE(decode_route_state(body, r));
    EXPECT_EQ(r.state_version, 7u);
    EXPECT_TRUE(r.synchronized);
    EXPECT_EQ(r.remote_ready_count, 2u);
    Bytes encoded;
    ASSERT_TRUE(encode_route_state(r, encoded));
    EXPECT_EQ(encoded, Bytes(body.data, body.data + body.size));
    b = vector("dzlc_17");
    ASSERT_TRUE(decode_local(ByteView(b), h, body));
    SendResultBody result;
    ASSERT_TRUE(decode_send_result(body, result));
    EXPECT_EQ(result.result, SendResultCode::TimedOut);
    EXPECT_TRUE(result.possible_remote_delivery);
    EXPECT_EQ(result.target_count, 2u);
    EXPECT_EQ(result.acked_count, 0u);
    ASSERT_TRUE(encode_send_result(result, encoded));
    EXPECT_EQ(encoded, Bytes(body.data, body.data + body.size));
    encoded[31] |= 1;
    EXPECT_FALSE(decode_send_result(ByteView(encoded), result));
    result.acked_count = 3;
    EXPECT_FALSE(encode_send_result(result, encoded));
}
TEST(SharedNetLocal, OutboxV2DeadlineAndCapacityPadding)
{
    for (const auto *name : {"dztx_reliable", "dztx_best_effort"})
    {
        auto b = vector(name);
        OutboxHeader h;
        ByteView blob;
        ASSERT_TRUE(decode_outbox(ByteView(b), h, blob));
        EXPECT_EQ(blob.size, 1025u);
        EXPECT_EQ(h.gateway_epoch, 0x1112131415161718ull);
        EXPECT_EQ(h.session_id, 0x5152535455565758ull);
        EXPECT_EQ(h.sequence, 0x3132333435363738ull);
        EXPECT_EQ(h.route.msg_id, 0x11223344u);
        EXPECT_EQ(h.request_id, h.delivery == Delivery::Reliable ? 0x6162636465666768ull : 0);
        EXPECT_EQ(h.deadline_monotonic_ns, h.delivery == Delivery::Reliable ? 987654321u : 0);
        Bytes encoded;
        ASSERT_TRUE(encode_outbox(h, blob, encoded));
        EXPECT_EQ(encoded, b);
        for (std::size_t n = 0; n < b.size(); ++n)
            EXPECT_FALSE(decode_outbox(ByteView(b.data(), n), h, blob));
        b.resize(2048, 0xa5);
        ASSERT_TRUE(decode_outbox(ByteView(b), h, blob));
        EXPECT_EQ(blob.size, 1025u);
        h.request_id = h.delivery == Delivery::Reliable ? 0 : 1;
        EXPECT_FALSE(encode_outbox(h, blob, encoded));
        b[7] = 96;
        EXPECT_FALSE(decode_outbox(ByteView(b), h, blob));
        b[7] = 112;
        b[5] = 1;
        EXPECT_FALSE(decode_outbox(ByteView(b), h, blob));
    }
}
TEST(SharedNetLocal, InvalidBodyBoundsAndUtf8)
{
    LocalHeader h{LocalKind::Error, 0, 1, 1};
    Bytes encoded;
    EXPECT_FALSE(encode_local(h, ByteView(Bytes{0, 0, 0, 1, 0, 2, 0xc0, 0x80}), encoded));
    h.kind = LocalKind::QueryState;
    h.request_id = 1;
    EXPECT_TRUE(encode_local(h, ByteView(Bytes{0}), encoded));
    EXPECT_FALSE(encode_local(h, ByteView(Bytes{1}), encoded));
    h.kind = LocalKind::CreditRequest;
    EXPECT_FALSE(encode_local(h, ByteView(Bytes(8)), encoded));
    EXPECT_FALSE(encode_local(h, ByteView(nullptr, 8), encoded));
}
TEST(SharedNetLocal, LargeStateResponseSupportsScaledRouteStatus)
{
    LocalHeader h{LocalKind::State, 1, 1, 1};
    Bytes body(4 + 900000, 'a');
    const auto length = static_cast<std::uint32_t>(body.size() - 4);
    body[0] = static_cast<std::uint8_t>(length >> 24);
    body[1] = static_cast<std::uint8_t>(length >> 16);
    body[2] = static_cast<std::uint8_t>(length >> 8);
    body[3] = static_cast<std::uint8_t>(length);
    Bytes encoded;
    ASSERT_TRUE(encode_local(h, ByteView(body), encoded));
    EXPECT_EQ(encoded.size(), 40u + body.size());
    LocalHeader decoded;
    ByteView decoded_body;
    ASSERT_TRUE(decode_local(ByteView(encoded), decoded, decoded_body));
    EXPECT_EQ(decoded.kind, LocalKind::State);
    EXPECT_EQ(decoded_body.size, body.size());
}
