#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
namespace dzIPC::Msg {
class TestMsg : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    TestMsg() = default;
    ~TestMsg() = default;


    /* 成员变量 */

    std::vector<double> data1;    /* data1 */
    std::vector<int32_t> data2;    /* data2 */
    std::vector<std::string> data3;    /* data3 */
    bool data4;    /* data4 */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t data1_count = int32_t(data1.size());
        int32_t data1_size = int32_t(data1_count * sizeof(double));
        int32_t data2_count = int32_t(data2.size());
        int32_t data2_size = int32_t(data2_count * sizeof(int32_t));
        int32_t data3_count = int32_t(data3.size());
        int32_t data3_total_size_ = 0;
        for (const auto& str : data3) {
            data3_total_size_ += int32_t(sizeof(int32_t) + str.size()) ;
        }
        int32_t data4_size = int32_t(sizeof(data4));

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 5;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(data1_count) + data1_size;
        total_size_ += sizeof(int32_t) + 5;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(data2_count) + data2_size;
        total_size_ += sizeof(int32_t) + 5;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(data3_count) + data3_total_size_;
        total_size_ += sizeof(int32_t) + 5;
        total_size_ += sizeof(uint8_t);
        total_size_ += data4_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 data1
        int32_t data1_name_size = 5;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data1_name_size), page, offset, sizeof(data1_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("data1"), page, offset, data1_name_size);
        uint8_t data1_type = 23;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data1_type), page, offset, sizeof(data1_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data1_count), page, offset, sizeof(data1_count));
        if (data1_count > 0) {
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(data1.data()), page, offset, data1_size);
        }

        // 序列化 data2
        int32_t data2_name_size = 5;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data2_name_size), page, offset, sizeof(data2_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("data2"), page, offset, data2_name_size);
        uint8_t data2_type = 18;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data2_type), page, offset, sizeof(data2_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data2_count), page, offset, sizeof(data2_count));
        if (data2_count > 0) {
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(data2.data()), page, offset, data2_size);
        }

        // 序列化 data3
        int32_t data3_name_size = 5;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data3_name_size), page, offset, sizeof(data3_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("data3"), page, offset, data3_name_size);
        uint8_t data3_type = 24;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data3_type), page, offset, sizeof(data3_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data3_count), page, offset, sizeof(data3_count));
        for (const auto& str : data3) {
            int32_t str_size = int32_t(str.size());
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&str_size), page, offset, sizeof(str_size));
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(str.data()), page, offset, str_size);
        }

        // 序列化 data4
        int32_t data4_name_size = 5;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data4_name_size), page, offset, sizeof(data4_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("data4"), page, offset, data4_name_size);
        uint8_t data4_type = 1;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data4_type), page, offset, sizeof(data4_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&data4), page, offset, sizeof(data4));

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 data1
        int32_t data1_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data1_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data1_name_size));
        offset += data1_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t data1_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data1_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data1_count));
        data1.resize(data1_count);
        if (data1_count > 0) {
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(data1.data()), static_cast<const uint8_t *>(buffer.data()), offset, data1_count * sizeof(double));
        }

        // 反序列化 data2
        int32_t data2_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data2_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data2_name_size));
        offset += data2_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t data2_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data2_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data2_count));
        data2.resize(data2_count);
        if (data2_count > 0) {
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(data2.data()), static_cast<const uint8_t *>(buffer.data()), offset, data2_count * sizeof(int32_t));
        }

        // 反序列化 data3
        int32_t data3_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data3_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data3_name_size));
        offset += data3_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t data3_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data3_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data3_count));
        data3.clear();
        data3.reserve(data3_count);
        for (int32_t i = 0; i < data3_count; ++i) {
            int32_t str_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&str_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(str_size));
            std::string str(str_size, '\0');
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(str.data()), static_cast<const uint8_t *>(buffer.data()), offset, str_size);
            data3.emplace_back(std::move(str));
        }

        // 反序列化 data4
        int32_t data4_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data4_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data4_name_size));
        offset += data4_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&data4), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(data4));

    }

        /* 克隆函数 */
        TestMsg* clone() const override
        {
            return new TestMsg(*this);
        }

};
} // namespace dzIPC::Msg
