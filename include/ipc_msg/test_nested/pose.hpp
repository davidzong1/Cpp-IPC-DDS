#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
namespace dzIPC::Msg {
class Pose : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    Pose() = default;
    ~Pose() = default;


    /* 成员变量 */

    double x;    /* x */
    double y;    /* y */
    double z;    /* z */
    std::string frame_id;    /* frame_id */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t x_size = int32_t(sizeof(x));
        int32_t y_size = int32_t(sizeof(y));
        int32_t z_size = int32_t(sizeof(z));
        int32_t frame_id_size = int32_t(frame_id.size()) ;

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 1;
        total_size_ += sizeof(uint8_t);
        total_size_ += x_size;
        total_size_ += sizeof(int32_t) + 1;
        total_size_ += sizeof(uint8_t);
        total_size_ += y_size;
        total_size_ += sizeof(int32_t) + 1;
        total_size_ += sizeof(uint8_t);
        total_size_ += z_size;
        total_size_ += sizeof(int32_t) + 8;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(frame_id_size) + frame_id_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 x
        int32_t x_name_size = 1;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&x_name_size), page, offset, sizeof(x_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("x"), page, offset, x_name_size);
        uint8_t x_type = 11;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&x_type), page, offset, sizeof(x_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&x), page, offset, sizeof(x));

        // 序列化 y
        int32_t y_name_size = 1;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&y_name_size), page, offset, sizeof(y_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("y"), page, offset, y_name_size);
        uint8_t y_type = 11;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&y_type), page, offset, sizeof(y_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&y), page, offset, sizeof(y));

        // 序列化 z
        int32_t z_name_size = 1;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&z_name_size), page, offset, sizeof(z_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("z"), page, offset, z_name_size);
        uint8_t z_type = 11;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&z_type), page, offset, sizeof(z_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&z), page, offset, sizeof(z));

        // 序列化 frame_id
        int32_t frame_id_name_size = 8;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&frame_id_name_size), page, offset, sizeof(frame_id_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("frame_id"), page, offset, frame_id_name_size);
        uint8_t frame_id_type = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&frame_id_type), page, offset, sizeof(frame_id_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&frame_id_size), page, offset, sizeof(frame_id_size));          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(frame_id.data()), page, offset, frame_id_size);

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 x
        int32_t x_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&x_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(x_name_size));
        offset += x_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&x), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(x));

        // 反序列化 y
        int32_t y_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&y_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(y_name_size));
        offset += y_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&y), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(y));

        // 反序列化 z
        int32_t z_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&z_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(z_name_size));
        offset += z_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&z), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(z));

        // 反序列化 frame_id
        int32_t frame_id_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&frame_id_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(frame_id_name_size));
        offset += frame_id_name_size;
        uint8_t frame_id_type;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&frame_id_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(frame_id_type));
        (void)frame_id_type;
        int32_t frame_id_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&frame_id_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(frame_id_size));
        frame_id.resize(frame_id_size );
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(frame_id.data()), static_cast<const uint8_t *>(buffer.data()), offset, frame_id_size);

    }

        /* 克隆函数 */
        Pose* clone() const override
        {
            return new Pose(*this);
        }

};
} // namespace dzIPC::Msg
