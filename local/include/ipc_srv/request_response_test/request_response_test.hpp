#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC::Srv {

// 请求类
class RequestResponseTestRequest : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    RequestResponseTestRequest() = default;
    ~RequestResponseTestRequest() = default;

    /* 成员变量 */
    std::vector<double> request;    /* request */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t request_count = int32_t(request.size());
        int32_t request_size = int32_t(request_count * sizeof(double));

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 7;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(request_count) + request_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 request
        int32_t request_name_size = 7;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&request_name_size), page, offset, sizeof(request_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("request"), page, offset, request_name_size);
        uint8_t request_type = 23;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&request_type), page, offset, sizeof(request_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&request_count), page, offset, sizeof(request_count));
        if (request_count > 0) {
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(request.data()), page, offset, request_size);
        }

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 request
        int32_t request_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&request_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(request_name_size));
        offset += request_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t request_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&request_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(request_count));
        request.resize(request_count);
        if (request_count > 0) {
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(request.data()), static_cast<const uint8_t *>(buffer.data()), offset, request_count * sizeof(double));
        }

    }

        /* 克隆函数 */
        RequestResponseTestRequest* clone() const override
        {
            return new RequestResponseTestRequest(*this);
        }
};

// 响应类
class RequestResponseTestResponse : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    RequestResponseTestResponse() = default;
    ~RequestResponseTestResponse() = default;

    /* 成员变量 */
    std::vector<double> response;    /* response */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t response_count = int32_t(response.size());
        int32_t response_size = int32_t(response_count * sizeof(double));

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 8;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(response_count) + response_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 response
        int32_t response_name_size = 8;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&response_name_size), page, offset, sizeof(response_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("response"), page, offset, response_name_size);
        uint8_t response_type = 23;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&response_type), page, offset, sizeof(response_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&response_count), page, offset, sizeof(response_count));
        if (response_count > 0) {
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(response.data()), page, offset, response_size);
        }

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 response
        int32_t response_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&response_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(response_name_size));
        offset += response_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t response_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&response_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(response_count));
        response.resize(response_count);
        if (response_count > 0) {
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(response.data()), static_cast<const uint8_t *>(buffer.data()), offset, response_count * sizeof(double));
        }

    }

        /* 克隆函数 */
        RequestResponseTestResponse* clone() const override
        {
            return new RequestResponseTestResponse(*this);
        }
};

} // namespace dzIPC::Srv
