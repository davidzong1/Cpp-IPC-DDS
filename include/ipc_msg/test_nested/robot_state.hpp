#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include "ipc_msg/test_nested/pose.hpp"
namespace dzIPC::Msg {
class RobotState : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    RobotState() = default;
    ~RobotState() = default;


    /* 成员变量 */

    std::string name;    /* name */
    dzIPC::Msg::Pose current_pose;    /* current_pose */
    std::vector<dzIPC::Msg::Pose> pose_history;    /* pose_history */
    std::string note;    /* note */

        /* 序列化函数 */
        ipc::buffer serialize() override
        {
        int32_t name_size = int32_t(name.size()) ;
        ipc::buffer current_pose_serialized = current_pose.serialize();
        int32_t current_pose_size = int32_t(current_pose_serialized.size());
        int32_t pose_history_count = int32_t(pose_history.size());
        std::vector<ipc::buffer> pose_history_serialized;
        std::vector<int32_t> pose_history_sizes;
        pose_history_serialized.reserve(pose_history_count);
        pose_history_sizes.reserve(pose_history_count);
        int32_t pose_history_total_size_ = 0;
        for (auto& nested_msg : pose_history) {
            pose_history_serialized.emplace_back(nested_msg.serialize());
            int32_t nested_size = int32_t(pose_history_serialized.back().size());
            pose_history_sizes.emplace_back(nested_size);
            pose_history_total_size_ += int32_t(sizeof(int32_t)) + nested_size;
        }
        int32_t note_size = int32_t(note.size()) ;

        // 计算总缓冲区大小
        size_t total_size_ = 0;
        total_size_ += sizeof(int32_t) + 4;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(name_size) + name_size;
        total_size_ += sizeof(int32_t) + 12;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(current_pose_size) + current_pose_size;
        total_size_ += sizeof(int32_t) + 12;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(pose_history_count) + pose_history_total_size_;
        total_size_ += sizeof(int32_t) + 4;
        total_size_ += sizeof(uint8_t);
        total_size_ += sizeof(note_size) + note_size;

        // 一次性分配缓冲区
        ipc::buffer buffer = std::move(this->serialize_data_cut(uint32_t(total_size_)));
        uint32_t offset = 0;
        uint16_t page = 1;

        // 序列化 name
        int32_t name_name_size = 4;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&name_name_size), page, offset, sizeof(name_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("name"), page, offset, name_name_size);
        uint8_t name_type = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&name_type), page, offset, sizeof(name_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&name_size), page, offset, sizeof(name_size));          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(name.data()), page, offset, name_size);

        // 序列化 current_pose
        int32_t current_pose_name_size = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&current_pose_name_size), page, offset, sizeof(current_pose_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("current_pose"), page, offset, current_pose_name_size);
        uint8_t current_pose_type = 25;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&current_pose_type), page, offset, sizeof(current_pose_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&current_pose_size), page, offset, sizeof(current_pose_size));
        if (current_pose_size > 0) {
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), static_cast<const uint8_t *>(current_pose_serialized.data()), page, offset, uint32_t(current_pose_size));
        }

        // 序列化 pose_history
        int32_t pose_history_name_size = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&pose_history_name_size), page, offset, sizeof(pose_history_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("pose_history"), page, offset, pose_history_name_size);
        uint8_t pose_history_type = 26;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&pose_history_type), page, offset, sizeof(pose_history_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&pose_history_count), page, offset, sizeof(pose_history_count));
        for (int32_t i = 0; i < pose_history_count; ++i) {
            int32_t nested_size = pose_history_sizes[i];
            this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&nested_size), page, offset, sizeof(nested_size));
            if (nested_size > 0) {
                this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), static_cast<const uint8_t *>(pose_history_serialized[i].data()), page, offset, uint32_t(nested_size));
            }
        }

        // 序列化 note
        int32_t note_name_size = 4;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&note_name_size), page, offset, sizeof(note_name_size));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>("note"), page, offset, note_name_size);
        uint8_t note_type = 12;
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&note_type), page, offset, sizeof(note_type));
        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&note_size), page, offset, sizeof(note_size));          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(note.data()), page, offset, note_size);

        this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);
        return buffer;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        // 反序列化 name
        int32_t name_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&name_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(name_name_size));
        offset += name_name_size;
        uint8_t name_type;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&name_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(name_type));
        (void)name_type;
        int32_t name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(name_size));
        name.resize(name_size );
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(name.data()), static_cast<const uint8_t *>(buffer.data()), offset, name_size);

        // 反序列化 current_pose
        int32_t current_pose_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&current_pose_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(current_pose_name_size));
        offset += current_pose_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t current_pose_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&current_pose_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(current_pose_size));
        ipc::buffer current_pose_buffer(new uint8_t[current_pose_size], current_pose_size, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
        if (current_pose_size > 0) {
            this->adapt_memcpy_tods(static_cast<uint8_t *>(current_pose_buffer.data()), static_cast<const uint8_t *>(buffer.data()), offset, uint32_t(current_pose_size));
        }
        current_pose.deserialize(current_pose_buffer);

        // 反序列化 pose_history
        int32_t pose_history_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&pose_history_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(pose_history_name_size));
        offset += pose_history_name_size;
        offset += sizeof(uint8_t); // 跳过类型标识
        int32_t pose_history_count;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&pose_history_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(pose_history_count));
        pose_history.clear();
        pose_history.resize(pose_history_count);
        for (int32_t i = 0; i < pose_history_count; ++i) {
            int32_t nested_size;
            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&nested_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(nested_size));
            ipc::buffer nested_buffer(new uint8_t[nested_size], nested_size, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
            if (nested_size > 0) {
                this->adapt_memcpy_tods(static_cast<uint8_t *>(nested_buffer.data()), static_cast<const uint8_t *>(buffer.data()), offset, uint32_t(nested_size));
            }
            pose_history[i].deserialize(nested_buffer);
        }

        // 反序列化 note
        int32_t note_name_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&note_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(note_name_size));
        offset += note_name_size;
        uint8_t note_type;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&note_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(note_type));
        (void)note_type;
        int32_t note_size;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&note_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(note_size));
        note.resize(note_size );
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(note.data()), static_cast<const uint8_t *>(buffer.data()), offset, note_size);

    }

        /* 克隆函数 */
        RobotState* clone() const override
        {
            return new RobotState(*this);
        }

};
} // namespace dzIPC::Msg
