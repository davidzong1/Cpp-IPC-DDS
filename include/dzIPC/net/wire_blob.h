#pragma once
#include "dzIPC/net/wire_protocol.h"
#include <memory>

class IpcMsgBase;
namespace dzIPC::net
{
// 不可变、独立拥有字节；capacity 恰为 W(size)，共享控制块另计元数据。
class WireBlob
{
  public:
    ByteView view() const noexcept
    {
        return {storage_.get(), size_};
    }
    std::size_t size() const noexcept
    {
        return size_;
    }
    std::size_t capacity() const noexcept
    {
        return capacity_;
    }
    Encoding encoding() const noexcept
    {
        return encoding_;
    }
    std::uint32_t msg_id() const noexcept
    {
        return msg_id_;
    }
    std::uint32_t schema_hash() const noexcept
    {
        return schema_hash_;
    }
    explicit operator bool() const noexcept
    {
        return storage_ != nullptr;
    }

  private:
    friend class WireEncoder;
    std::shared_ptr<const std::uint8_t[]> storage_;
    std::size_t size_ = 0, capacity_ = 0;
    Encoding encoding_ = Encoding::Tlv;
    std::uint32_t msg_id_ = 0, schema_hash_ = 0;
};
class WireEncoder
{
  public:
    static ProtocolStatus adopt(std::unique_ptr<std::uint8_t[]> storage, std::size_t size,
                                std::size_t capacity, Encoding, std::uint32_t msg_id,
                                std::uint32_t schema_hash, WireBlob &out);
    static ProtocolStatus copy(ByteView, Encoding, std::uint32_t msg_id, std::uint32_t schema_hash,
                               WireBlob &out);
    static ProtocolStatus encode(IpcMsgBase &, bool prefer_dzflat, WireBlob &out);
    static ProtocolStatus encode_prebuilt(ByteView segment, std::uint32_t msg_id,
                                          std::uint32_t schema_hash, WireBlob &out);
};
ProtocolStatus validate_blob(ByteView, Encoding, std::uint32_t msg_id, std::uint32_t schema_hash);
} // namespace dzIPC::net
