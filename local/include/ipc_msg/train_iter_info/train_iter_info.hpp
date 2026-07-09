#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
namespace dzIPC::Msg {
class TrainIterInfo : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    TrainIterInfo() = default;
    ~TrainIterInfo() = default;


    /* 成员变量 */

    int32_t iter_cnt;    /* iter_cnt */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t iter_cnt_size = int32_t(sizeof(iter_cnt));

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 8;
        total_size_ += sizeof(uint8_t);
        total_size_ += iter_cnt_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 iter_cnt
        int32_t iter_cnt_name_size = 8;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&iter_cnt_name_size), page, offset, sizeof(iter_cnt_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("iter_cnt"), page, offset, iter_cnt_name_size);
        uint8_t iter_cnt_type = 6;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&iter_cnt_type), page, offset, sizeof(iter_cnt_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&iter_cnt), page, offset, sizeof(iter_cnt));

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 iter_cnt
        int32_t iter_cnt_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&iter_cnt_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(iter_cnt_name_size));
        offset += iter_cnt_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&iter_cnt), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(iter_cnt));

    }

        /* 克隆函数 */
        TrainIterInfo* clone() const override
        {
            return new TrainIterInfo(*this);
        }

};
} // namespace dzIPC::Msg
