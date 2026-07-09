#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC::Srv {

// 请求类
class RewardCfgRequest : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    RewardCfgRequest() = default;
    ~RewardCfgRequest() = default;

    /* 成员变量 */
    std::vector<std::string> reward_cfg_names;/* reward_cfg_names */
    std::vector<float> reward_cfg_values;/* reward_cfg_values */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
            int32_t reward_cfg_names_count = int32_t(reward_cfg_names.size());
        int32_t reward_cfg_names_total_size_ = 0;
        for (const auto& str : reward_cfg_names) {
                reward_cfg_names_total_size_ += int32_t(sizeof(int32_t) + str.size()) ;
        }
            int32_t reward_cfg_values_count = int32_t(reward_cfg_values.size());
            int32_t reward_cfg_values_size = int32_t(reward_cfg_values_count * sizeof(float));

            // 计算总缓冲区大小
            size_t total_size_ = 0;
            total_size_ += sizeof(int32_t) + 16;
            total_size_ += sizeof(uint8_t);
            total_size_ += sizeof(reward_cfg_names_count) + reward_cfg_names_total_size_;
            total_size_ += sizeof(int32_t) + 17;
            total_size_ += sizeof(uint8_t);
            total_size_ += sizeof(reward_cfg_values_count) + reward_cfg_values_size;

            // 一次性分配缓冲区
            ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
            uint32_t offset = 0;
            uint16_t page = 1;

            // 序列化 reward_cfg_names
            int32_t reward_cfg_names_name_size = 16;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_names_name_size), page, offset, sizeof(reward_cfg_names_name_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("reward_cfg_names"), page, offset, reward_cfg_names_name_size);
            uint8_t reward_cfg_names_type = 24;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_names_type), page, offset, sizeof(reward_cfg_names_type));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_names_count), page, offset, sizeof(reward_cfg_names_count));
            for (const auto& str : reward_cfg_names) {
                int32_t str_size = int32_t(str.size());
                this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&str_size), page, offset, sizeof(str_size));
                this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(str.data()), page, offset, str_size);
            }

            // 序列化 reward_cfg_values
            int32_t reward_cfg_values_name_size = 17;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_values_name_size), page, offset, sizeof(reward_cfg_values_name_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("reward_cfg_values"), page, offset, reward_cfg_values_name_size);
            uint8_t reward_cfg_values_type = 22;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_values_type), page, offset, sizeof(reward_cfg_values_type));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&reward_cfg_values_count), page, offset, sizeof(reward_cfg_values_count));
            if (reward_cfg_values_count > 0) {
                this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(reward_cfg_values.data()), page, offset, reward_cfg_values_size);
            }

            this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
            return buffer;
        }

        /* 反序列化函数 */
        void deserialize(const ipc::buffer& buffer) override
        {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
            // 反序列化 reward_cfg_names
            int32_t reward_cfg_names_name_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&reward_cfg_names_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(reward_cfg_names_name_size));
            offset += reward_cfg_names_name_size;
            offset += sizeof(uint8_t); // 跳过类型标识
            int32_t reward_cfg_names_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&reward_cfg_names_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(reward_cfg_names_size));
            reward_cfg_names.resize(reward_cfg_names_size );
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(reward_cfg_names.data()), static_cast<const uint8_t *>(buffer.data()), offset, reward_cfg_names_size);

            // 反序列化 reward_cfg_values
            int32_t reward_cfg_values_name_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&reward_cfg_values_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(reward_cfg_values_name_size));
            offset += reward_cfg_values_name_size;
            offset += sizeof(uint8_t); // 跳过类型标识
            int32_t reward_cfg_values_count;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&reward_cfg_values_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(reward_cfg_values_count));
            reward_cfg_values.resize(reward_cfg_values_count);
            if (reward_cfg_values_count > 0) {
                this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(reward_cfg_values.data()), static_cast<const uint8_t *>(buffer.data()), offset, reward_cfg_values_count * sizeof(float));
            }

    }

    /* 克隆函数 */
    RewardCfgRequest* clone() const override
    {
        return new RewardCfgRequest(*this);
    }
};

// 响应类
class RewardCfgResponse : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    RewardCfgResponse() = default;
    ~RewardCfgResponse() = default;

    /* 成员变量 */
    bool success;/* success */
    std::string message;/* message */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
            int32_t success_size = int32_t(sizeof(success));
            int32_t message_size = int32_t(message.size()) ;

            // 计算总缓冲区大小
            size_t total_size_ = 0;
            total_size_ += sizeof(int32_t) + 7;
            total_size_ += sizeof(uint8_t);
            total_size_ += success_size;
            total_size_ += sizeof(int32_t) + 7;
            total_size_ += sizeof(uint8_t);
            total_size_ += sizeof(message_size) + message_size;

            // 一次性分配缓冲区
            ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
            uint32_t offset = 0;
            uint16_t page = 1;

            // 序列化 success (bool类型特殊处理)
            int32_t success_name_size = 7;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&success_name_size), page, offset, sizeof(success_name_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("success"), page, offset, success_name_size);
            uint8_t success_type = 1;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&success_type), page, offset, sizeof(success_type));
            uint8_t success_byte = success ? 1 : 0;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&success_byte), page, offset, sizeof(success_byte));

            // 序列化 message
            int32_t message_name_size = 7;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&message_name_size), page, offset, sizeof(message_name_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("message"), page, offset, message_name_size);
            uint8_t message_type = 12;
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&message_type), page, offset, sizeof(message_type));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&message_size), page, offset, sizeof(message_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(message.data()), page, offset, message_size);

            this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
            return buffer;
        }

        /* 反序列化函数 */
        void deserialize(const ipc::buffer& buffer) override
        {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
            // 反序列化 success (bool类型特殊处理)
            int32_t success_name_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&success_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(success_name_size));
            offset += success_name_size;
            offset += sizeof(uint8_t); // 跳过类型标识
            uint8_t success_byte;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&success_byte), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(success_byte));
            success = (success_byte != 0);

            // 反序列化 message
            int32_t message_name_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&message_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(message_name_size));
            offset += message_name_size;
            offset += sizeof(uint8_t); // 跳过类型标识
            int32_t message_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&message_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(message_size));
            message.resize(message_size );
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(message.data()), static_cast<const uint8_t *>(buffer.data()), offset, message_size);

    }

    /* 克隆函数 */
    RewardCfgResponse* clone() const override
    {
        return new RewardCfgResponse(*this);
    }
};

} // namespace dzIPC::Srv