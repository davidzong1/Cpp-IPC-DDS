#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
namespace dzIPC::Msg {
class Supervisor : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    Supervisor() = default;
    ~Supervisor() = default;


    /* 成员变量 */

    std::string update_time;    /* update_time */
    std::string additional_info;    /* additional_info */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t update_time_size = int32_t(update_time.size()) ;
        int32_t additional_info_size = int32_t(additional_info.size()) ;

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 11;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(update_time_size) + update_time_size;
        total_size_ += sizeof(int32_t) + 15;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(additional_info_size) + additional_info_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 update_time
        int32_t update_time_name_size = 11;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&update_time_name_size), page, offset, sizeof(update_time_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("update_time"), page, offset, update_time_name_size);
        uint8_t update_time_type = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&update_time_type), page, offset, sizeof(update_time_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&update_time_size), page, offset, sizeof(update_time_size));          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(update_time.data()), page, offset, update_time_size);

        // 序列化 additional_info
        int32_t additional_info_name_size = 15;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&additional_info_name_size), page, offset, sizeof(additional_info_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("additional_info"), page, offset, additional_info_name_size);
        uint8_t additional_info_type = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&additional_info_type), page, offset, sizeof(additional_info_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&additional_info_size), page, offset, sizeof(additional_info_size));          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(additional_info.data()), page, offset, additional_info_size);

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 update_time
        int32_t update_time_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&update_time_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(update_time_name_size));
        offset += update_time_name_size;
        uint8_t update_time_type;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&update_time_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(update_time_type));
        (void)update_time_type;
        int32_t update_time_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&update_time_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(update_time_size));
        update_time.resize(update_time_size );
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(update_time.data()), static_cast<const uint8_t *>(buffer.data()), offset, update_time_size);

        // 反序列化 additional_info
        int32_t additional_info_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&additional_info_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(additional_info_name_size));
        offset += additional_info_name_size;
        uint8_t additional_info_type;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&additional_info_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(additional_info_type));
        (void)additional_info_type;
        int32_t additional_info_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&additional_info_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(additional_info_size));
        additional_info.resize(additional_info_size );
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(additional_info.data()), static_cast<const uint8_t *>(buffer.data()), offset, additional_info_size);

    }

        /* 克隆函数 */
        Supervisor* clone() const override
        {
            return new Supervisor(*this);
        }

};
} // namespace dzIPC::Msg
