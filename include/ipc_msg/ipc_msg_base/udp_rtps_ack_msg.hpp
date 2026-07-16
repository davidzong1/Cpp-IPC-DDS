#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

class IpcRtpsNackMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsNackMsgId = 0x44'5A'4E'4B;   // "DZNK"
    static constexpr std::size_t kMaxMissingPages = 256;

    IpcRtpsNackMsg() { set_msg_id(kRtpsNackMsgId); }

    ~IpcRtpsNackMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t receiver_id{0};
    uint32_t sequence{0};
    std::vector<uint16_t> missing_pages;

    bool check_nk_id(const ipc::buffer& data) const { return check_id(data, kRtpsNackMsgId); }

    ipc::buffer serialize() override
    {
        const uint16_t miss_cnt = static_cast<uint16_t>(std::min(missing_pages.size(), kMaxMissingPages));
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(sequence) + sizeof(miss_cnt)
                                     + static_cast<uint32_t>(miss_cnt * sizeof(uint16_t));

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&receiver_id), page,
                         offset, sizeof(receiver_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&miss_cnt), page, offset,
                         sizeof(miss_cnt));

        for (uint16_t i = 0; i < miss_cnt; ++i)
        {
            const uint16_t page_idx = missing_pages[i];
            adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_idx), page,
                             offset, sizeof(page_idx));
        }

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        uint16_t miss_cnt = 0;

        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(sequence));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&miss_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(miss_cnt));

        const uint16_t clamp_cnt = static_cast<uint16_t>(std::min<std::size_t>(miss_cnt, kMaxMissingPages));
        missing_pages.clear();
        missing_pages.reserve(clamp_cnt);

        for (uint16_t i = 0; i < clamp_cnt; ++i)
        {
            uint16_t page_idx = 0;
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_idx), static_cast<const uint8_t*>(buffer.data()), offset,
                              sizeof(page_idx));
            missing_pages.push_back(page_idx);
        }
    }

    IpcRtpsNackMsg* clone() const override { return new IpcRtpsNackMsg(*this); }
};

class IpcRtpsAckMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsAckMsgId = 0x44'5A'41'4B;   // "DZAK"

    IpcRtpsAckMsg() { set_msg_id(kRtpsAckMsgId); }

    ~IpcRtpsAckMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t receiver_id{0};
    uint32_t sequence{0};
    uint8_t integrity_flags{0};
    uint32_t payload_crc32c{0};

    bool check_ak_id(const ipc::buffer& data) const { return check_id(data, kRtpsAckMsgId); }

    ipc::buffer serialize() override
    {
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(sequence) + sizeof(integrity_flags) + sizeof(payload_crc32c);

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&receiver_id), page,
                         offset, sizeof(receiver_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&integrity_flags), page,
                         offset, sizeof(integrity_flags));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&payload_crc32c), page,
                         offset, sizeof(payload_crc32c));

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;

        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));

        // Sequence field: read if buffer large enough; back-compat with old-format ACK.
        const std::size_t min_size_for_seq = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id)
                                             + sizeof(receiver_id) + sizeof(sequence) + sizeof(integrity_flags)
                                             + sizeof(payload_crc32c) + 12;
        if (buffer.size() >= min_size_for_seq)
        {
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                              sizeof(sequence));
        }

        // Integrity fields: back-compat with old-format ACK that lacks integrity_flags + payload_crc32c.
        const std::size_t min_size_for_crc = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id)
                                             + sizeof(receiver_id) + sizeof(sequence) + sizeof(integrity_flags)
                                             + sizeof(payload_crc32c) + 12;
        if (buffer.size() >= min_size_for_crc)
        {
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&integrity_flags), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(integrity_flags));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&payload_crc32c), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(payload_crc32c));
        }
        else if (buffer.size() >= min_size_for_seq + sizeof(integrity_flags) + sizeof(payload_crc32c))
        {
            // ACK without sequence but with integrity: skip sequence, read integrity.
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&integrity_flags), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(integrity_flags));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&payload_crc32c), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(payload_crc32c));
        }
    }

    IpcRtpsAckMsg* clone() const override { return new IpcRtpsAckMsg(*this); }
};
