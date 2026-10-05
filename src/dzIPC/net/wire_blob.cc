#include "dzIPC/net/wire_blob.h"
#include "byte_codec.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC::net
{
using namespace codec;
ProtocolStatus validate_blob(ByteView b, Encoding encoding, std::uint32_t msg_id,
                             std::uint32_t schema_hash)
{
    if (!valid(b) || !b.size || b.size > kMaxMessageBytes)
        return error(ProtocolCode::BadLength);
    if (encoding == Encoding::Tlv)
    {
        if (schema_hash)
            return error(ProtocolCode::BadField);
        constexpr auto page_size = ipc::wire_packet_size;
        const auto count = (b.size + page_size - 1) / page_size;
        for (std::size_t page = 0; page < count; ++page)
        {
            const auto length = std::min(page_size, b.size - page * page_size);
            if (length < 12)
                return error(ProtocolCode::BadLength);
            const auto *tail = b.data + page * page_size + length - 12;
            // 历史 serialize() 在跨页时先递增 now_page；网络旧路径还会改写它。
            // 新协议保留原值，不把该字段当作外层分片号或改写序列化输出。
            if (get(tail, 2) != count || get(tail + 4, 4) != b.size || get(tail + 8, 4) != msg_id)
                return error(ProtocolCode::BadField);
        }
        return {};
    }
    if (encoding != Encoding::DzFlat)
        return error(ProtocolCode::BadField);
    if (b.size < sizeof(dzflat::SegHeader))
        return error(ProtocolCode::BadLength);
    dzflat::SegHeader h{};
    std::memcpy(&h, b.data, sizeof(h));
    if (h.magic != dzflat::kMagic)
        return error(ProtocolCode::BadMagic);
    if (h.layout_ver != dzflat::kLayoutVer)
        return error(ProtocolCode::BadVersion);
    if (h.total_size != b.size || h.root_off != sizeof(h) || h.root_size > b.size - sizeof(h))
        return error(ProtocolCode::BadLength);
    if (!schema_hash || h.schema_hash != schema_hash || h.msg_id != msg_id || h.flags || h.reserved)
        return error(ProtocolCode::BadField);
    return {};
}
ProtocolStatus WireEncoder::copy(ByteView b, Encoding encoding, std::uint32_t id,
                                 std::uint32_t schema, WireBlob &out)
{
    auto s = validate_blob(b, encoding, id, schema);
    if (!s)
        return s;
    WireBlob result;
    result.size_ = b.size;
    result.capacity_ = ((b.size + 63) / 64) * 64;
    std::unique_ptr<std::uint8_t[]> storage(new std::uint8_t[result.capacity_]{});
    std::memcpy(storage.get(), b.data, b.size);
    result.storage_ = std::move(storage);
    result.encoding_ = encoding;
    result.msg_id_ = id;
    result.schema_hash_ = schema;
    out = std::move(result);
    return {};
}
ProtocolStatus WireEncoder::encode_prebuilt(ByteView b, std::uint32_t id, std::uint32_t schema,
                                            WireBlob &out)
{
    if (!valid(b) || b.size < sizeof(dzflat::SegHeader))
        return error(ProtocolCode::BadLength);
    dzflat::SegHeader h{};
    std::memcpy(&h, b.data, sizeof(h));
    if (h.total_size > b.size)
        return error(ProtocolCode::BadLength);
    return copy(ByteView(b.data, h.total_size), Encoding::DzFlat, id,
                schema ? schema : h.schema_hash, out);
}
ProtocolStatus WireEncoder::encode(IpcMsgBase &msg, bool prefer_dzflat, WireBlob &out)
{
    // GenericMessage 的段无法转回 TLV，必须优先保留原段。
    if (const auto *generic = dynamic_cast<const GenericMessage *>(&msg);
        generic && generic->has_dzflat())
        return encode_prebuilt(ByteView(generic->dzflat_data(), generic->dzflat_len()),
                               msg.msg_id(), generic->dzflat_seg_schema_hash(), out);
    if (prefer_dzflat && msg.dzflat_supported())
    {
        const auto size = msg.dzflat_size();
        if (!size || size > kMaxMessageBytes)
            return error(ProtocolCode::TooLarge);
        Bytes b(size);
        if (!msg.dzflat_write(b.data(), size))
            return error(ProtocolCode::BadField);
        return encode_prebuilt(ByteView(b), msg.msg_id(), msg.dzflat_schema_hash(), out);
    }
    auto serialized = msg.serialize();
    return copy(ByteView(serialized.data(), serialized.size()), Encoding::Tlv, msg.msg_id(), 0,
                out);
}
} // namespace dzIPC::net
