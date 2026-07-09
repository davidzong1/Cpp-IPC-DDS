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
    std::vector<uint16_t> missing_pages;

    bool check_nk_id(const ipc::buffer& data) const { return check_id(data, kRtpsNackMsgId); }

    ipc::buffer serialize() override
    {
        const uint16_t miss_cnt = static_cast<uint16_t>(std::min(missing_pages.size(), kMaxMissingPages));
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(miss_cnt) + static_cast<uint32_t>(miss_cnt * sizeof(uint16_t));

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

        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));
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

    bool check_ak_id(const ipc::buffer& data) const { return check_id(data, kRtpsAckMsgId); }

    ipc::buffer serialize() override
    {
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id);

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

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;

        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));
    }

    IpcRtpsAckMsg* clone() const override { return new IpcRtpsAckMsg(*this); }
};
