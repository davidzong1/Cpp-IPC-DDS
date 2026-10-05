#include "dzIPC/net/wire_blob.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "gtest/gtest.h"
#include <cstring>

using namespace dzIPC::net;
namespace
{
class CountingMessage : public dzIPC::GenericMessage
{
  public:
    unsigned serializations = 0;
    ipc::buffer serialize() override
    {
        ++serializations;
        return GenericMessage::serialize();
    }
};
Bytes segment()
{
    Bytes b(40);
    dzflat::SegHeader h{dzflat::kMagic, 0xabcdef01, 32, 8, 40, 1, 0, 71, 0};
    std::memcpy(b.data(), &h, sizeof(h));
    b[32] = 0x5a;
    return b;
}
} // namespace
TEST(SharedNetBlob, MultiPageTlvIsByteExactAndOwned)
{
    CountingMessage msg;
    msg.set_msg_id(71);
    msg.set_string("payload", std::string(5000, 'z'));
    auto expected = msg.serialize();
    msg.serializations = 0;
    // 原 serialize 的第一页 now_page 是 2；不得按新网络分片规则拒绝或修正。
    EXPECT_EQ(static_cast<const std::uint8_t *>(expected.data())[ipc::wire_packet_size - 9], 2u);
    WireBlob blob;
    ASSERT_TRUE(WireEncoder::encode(msg, false, blob));
    EXPECT_EQ(msg.serializations, 1u);
    ASSERT_EQ(blob.size(), expected.size());
    EXPECT_EQ(std::memcmp(blob.view().data, expected.data(), blob.size()), 0);
    EXPECT_EQ(blob.capacity(), ((blob.size() + 63) / 64) * 64);
    EXPECT_EQ(blob.encoding(), Encoding::Tlv);
    msg.clear();
    EXPECT_EQ(std::memcmp(blob.view().data, expected.data(), blob.size()), 0);
    Bytes corrupt(blob.view().data, blob.view().data + blob.size());
    corrupt.back() ^= 1;
    WireBlob rejected;
    EXPECT_FALSE(WireEncoder::copy(ByteView(corrupt), Encoding::Tlv, 71, 0, rejected));
    EXPECT_FALSE(rejected);
}
TEST(SharedNetBlob, PrebuiltDropsOnlyCapacityPaddingAndPreservesHeader)
{
    auto b = segment();
    const auto expected = b;
    b.resize(1024, 0xcc);
    WireBlob blob;
    ASSERT_TRUE(WireEncoder::encode_prebuilt(ByteView(b), 71, 0xabcdef01, blob));
    EXPECT_EQ(blob.size(), 40u);
    EXPECT_EQ(blob.capacity(), 64u);
    EXPECT_EQ(Bytes(blob.view().data, blob.view().data + blob.size()), expected);
    EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b), 72, 0, blob));
    EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b), 71, 0x1234, blob));
    b[12] = 0xff;
    EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b), 71, 0, blob));
}
TEST(SharedNetBlob, GenericFlatNeverSerializesAsTlv)
{
    auto b = segment();
    CountingMessage msg;
    msg.set_msg_id(71);
    ASSERT_TRUE(msg.dzflat_read(b.data(), b.size()));
    WireBlob blob;
    ASSERT_TRUE(WireEncoder::encode(msg, false, blob));
    EXPECT_EQ(msg.serializations, 0u);
    EXPECT_EQ(blob.encoding(), Encoding::DzFlat);
    EXPECT_EQ(Bytes(blob.view().data, blob.view().data + blob.size()), b);
}
TEST(SharedNetBlob, MalformedFlatAndNullRejected)
{
    auto b = segment();
    WireBlob blob;
    for (std::size_t n = 0; n < b.size(); ++n)
        EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b.data(), n), 71, 0, blob));
    EXPECT_FALSE(WireEncoder::copy(ByteView(nullptr, 40), Encoding::DzFlat, 71, 0, blob));
    b[22] = 1;
    EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b), 71, 0, blob));
    b = segment();
    b[20] = 2;
    EXPECT_EQ(WireEncoder::encode_prebuilt(ByteView(b), 71, 0, blob).code,
              ProtocolCode::BadVersion);
    b = segment();
    b[28] = 1;
    EXPECT_FALSE(WireEncoder::encode_prebuilt(ByteView(b), 71, 0, blob));
}
