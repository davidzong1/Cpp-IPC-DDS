#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "libipc/buffer.h"

struct ipc_tail_msg
{
    uint16_t page_cnt = 0;
    uint16_t now_page = 0;
    uint32_t total_size = 0;
    uint32_t dz_ipc_msg_id = 0;
};

class IpcMsgBase : public std::enable_shared_from_this<IpcMsgBase>
{
    struct size_info
    {
        uint16_t page_cnt;
        uint32_t total_size;
    };

public:
    IpcMsgBase() = default;
    ~IpcMsgBase() = default;

    void set_msg_id(const uint32_t id) { dz_ipc_msg_id = id; }

    bool check_id(const ipc::buffer& data, uint32_t expected_id) const
    {
        if (data.size() < 12)
        {
            return false;
        }
        const uint8_t* id = reinterpret_cast<const uint8_t*>(data.data()) + data.size() - 4;
        uint32_t msg_id = (static_cast<uint32_t>(id[0]) << 24) | (static_cast<uint32_t>(id[1]) << 16)
                          | (static_cast<uint32_t>(id[2]) << 8) | (static_cast<uint32_t>(id[3]));
        return msg_id == expected_id;
    };

    /* 计算序列化后消息类型标识符 */
    bool check_id(const ipc::buffer& data)
    {
        if (data.size() < 12)
        {
            return false;
        }
        const uint8_t* id = reinterpret_cast<const uint8_t*>(data.data()) + data.size() - 4;
        uint32_t msg_id = (static_cast<uint32_t>(id[0]) << 24) | (static_cast<uint32_t>(id[1]) << 16)
                          | (static_cast<uint32_t>(id[2]) << 8) | (static_cast<uint32_t>(id[3]));
        return msg_id == dz_ipc_msg_id;
    };

    /* 序列化函数 */
    virtual ipc::buffer serialize() { return ipc::buffer(); };

    /* 反序列化函数 */
    virtual void deserialize(const ipc::buffer& data) {};

    /* 克隆函数 */
    virtual IpcMsgBase* clone() const { return new IpcMsgBase(*this); };

protected:
#define TAIL_MSG_SIZE 12         // total cnt(2 bytes)+ now page(2 bytes)+total_size(4byte) + dz_ipc_msg_id(4 bytes)
#define IPC_MSG_MAX_SIZE 1'460   // 1472-12

    size_info correct_total_size(uint32_t total_data_len) const
    {
        uint16_t total_page = static_cast<uint16_t>(total_data_len / IPC_MSG_MAX_SIZE) + 1;
        return size_info{total_page, total_data_len + total_page * TAIL_MSG_SIZE};
    }

    void add_tail_msg(uint8_t* dst, uint16_t& page) const
    {
        dst[0] = static_cast<uint8_t>(this->_total_page_cnt
                                      >> 8);   // page start from 1, 0 is reserved for uncut message
        dst[1] = static_cast<uint8_t>(this->_total_page_cnt & 0xFF);   // complete flag, 0 for incomplete, 1 for complete
        dst[2] = static_cast<uint8_t>(page >> 8);                 // page start from 1, 0 is reserved for uncut message
        dst[3] = static_cast<uint8_t>(page & 0xFF);               // complete flag, 0 for incomplete, 1 for complete
        dst[4] = static_cast<uint8_t>(this->_total_size >> 24);   // 4 bytes for total size
        dst[5] = static_cast<uint8_t>((this->_total_size >> 16) & 0xFF);     // 4 bytes for total size
        dst[6] = static_cast<uint8_t>((this->_total_size >> 8) & 0xFF);      // 4 bytes for total size
        dst[7] = static_cast<uint8_t>(this->_total_size & 0xFF);             // 4 bytes for total size
        dst[8] = static_cast<uint8_t>(this->dz_ipc_msg_id >> 24);            // 4 bytes for msg id
        dst[9] = static_cast<uint8_t>((this->dz_ipc_msg_id >> 16) & 0xFF);   // 4 bytes for msg id
        dst[10] = static_cast<uint8_t>((this->dz_ipc_msg_id >> 8) & 0xFF);   // 4 bytes for msg id
        dst[11] = static_cast<uint8_t>(this->dz_ipc_msg_id & 0xFF);          // 4 bytes for msg id
    }

    void adapt_memcpy_tos(uint8_t* dst, const uint8_t* src, uint16_t& page, uint32_t& offset,
                          const uint32_t local_data_len)
    {
        // 计算不包含之前追加的尾部长度的 纯数据逻辑偏移量,由于page最小为1，因此纯数据偏移量初始值为0
        uint32_t pure_data_offset = offset - ((page - 1) * TAIL_MSG_SIZE);
        uint32_t cut_cnt = ((pure_data_offset + local_data_len) / IPC_MSG_MAX_SIZE)
                           - (pure_data_offset / IPC_MSG_MAX_SIZE);
        uint32_t has_copy_size = 0;

        for (uint32_t i = 0; i < cut_cnt; ++i)
        {
            // 按照正确的纯逻辑偏移去算：当前此数据页还剩下的真正写入容量
            uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                          local_data_len - has_copy_size);

            std::memcpy(dst + offset, src + has_copy_size, copy_size);
            offset += copy_size;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;

            page++;
            this->add_tail_msg(dst + offset, page);
            offset += TAIL_MSG_SIZE;
        }

        long remaining_size = local_data_len - has_copy_size;
        if (remaining_size > 0)
        {
            std::memcpy(dst + offset, src + has_copy_size, remaining_size);
            offset += remaining_size;
        }
    }

    void adapt_memcpy_tods(uint8_t* dst, const uint8_t* src, uint32_t& offset, const uint32_t local_data_len) const
    {
        // 反推已经跨过了多少个页附加控制信息，从而计算出纯逻辑数据偏移
        uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
        uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;

        uint32_t cut_cnt = ((pure_data_offset + local_data_len) / IPC_MSG_MAX_SIZE)
                           - (pure_data_offset / IPC_MSG_MAX_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i)
        {
            uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                          local_data_len - has_copy_size);
            std::memcpy(dst + has_copy_size, src + offset, copy_size);

            offset += copy_size + TAIL_MSG_SIZE;   // 跨过数据长度外，还要跨过那 12 个尾部特征字节
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        long remaining_size = local_data_len - has_copy_size;
        if (remaining_size > 0)
        {
            std::memcpy(dst + has_copy_size, src + offset, remaining_size);
            offset += remaining_size;
        }
    }

    ipc::buffer serialize_data_cut(uint32_t total_data_len)
    {
        /* 2 bytes for each cut use as page and complete flag */
        size_info size_info = correct_total_size(total_data_len);
        this->_total_size = size_info.total_size;
        this->_total_page_cnt = size_info.page_cnt;

        // 2. 移除 std::move，直接返回临时对象
        return ipc::buffer(new uint8_t[this->_total_size], this->_total_size,
                           [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
    }

    void deserialize_data_cut(const uint32_t total_data_len)
    {
        /* 2 bytes for each cut use as page and complete flag */
        uint16_t total_page_cnt = static_cast<uint16_t>(total_data_len / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE)) + 1;
        this->_total_size = total_data_len;
        this->_total_page_cnt = total_page_cnt;
    }

public:
    const uint32_t total_size() { return this->_total_size; }

    const uint16_t total_page_cnt() { return this->_total_page_cnt; }

    const ipc_tail_msg get_tail_msg(ipc::buffer& data) const
    {
        ipc_tail_msg tail_msg;
        size_t data_len = data.size();
        const uint8_t* data_ptr = static_cast<const uint8_t*>(data.data());
        if (data_len > 0)
        {
            tail_msg.page_cnt = static_cast<uint16_t>(data_ptr[data_len - 12]) << 8
                                | static_cast<uint16_t>(data_ptr[data_len - 11]);
            tail_msg.now_page = static_cast<uint16_t>(data_ptr[data_len - 10]) << 8
                                | static_cast<uint16_t>(data_ptr[data_len - 9]);
            tail_msg.total_size = static_cast<uint32_t>(data_ptr[data_len - 8]) << 24
                                  | static_cast<uint32_t>(data_ptr[data_len - 7]) << 16
                                  | static_cast<uint32_t>(data_ptr[data_len - 6]) << 8
                                  | static_cast<uint32_t>(data_ptr[data_len - 5]);
            tail_msg.dz_ipc_msg_id = static_cast<uint32_t>(data_ptr[data_len - 4]) << 24
                                     | static_cast<uint32_t>(data_ptr[data_len - 3]) << 16
                                     | static_cast<uint32_t>(data_ptr[data_len - 2]) << 8
                                     | static_cast<uint32_t>(data_ptr[data_len - 1]);
        }
        return tail_msg;
    }

#undef IPC_MSG_MAX_SIZE
#undef TAIL_MSG_SIZE

    template<typename T = IpcMsgBase, typename = std::enable_if_t<std::is_base_of<IpcMsgBase, T>::value>>
    std::shared_ptr<T> msgcast()
    {
        return std::static_pointer_cast<T>(shared_from_this());
    }

protected:
    mutable uint32_t _total_size = 0;
    mutable uint16_t _total_page_cnt = 0;
    mutable uint32_t dz_ipc_msg_id = 0;   // 消息类型标识符，可用于区分不同消息类型
};