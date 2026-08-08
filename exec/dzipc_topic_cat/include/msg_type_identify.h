#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC {
#define TAIL_MSG_SIZE 12         // total cnt(2 bytes)+ now page(2 bytes)+total_size(4byte) + dz_ipc_msg_id(4 bytes)
#define IPC_MSG_MAX_SIZE 1'460   // 1472-12
#define IPC_MSG_

enum MsgType : uint8_t {
    MSG_BOOL = 1,
    MSG_INT8 = 2,
    MSG_UINT8 = 3,
    MSG_INT16 = 4,
    MSG_UINT16 = 5,
    MSG_INT32 = 6,
    MSG_UINT32 = 7,
    MSG_INT64 = 8,
    MSG_UINT64 = 9,
    MSG_FLOAT32 = 10,
    MSG_FLOAT64 = 11,
    MSG_STRING = 12,
    MSG_BOOL_ARRAY = 13,
    MSG_INT8_ARRAY = 14,
    MSG_UINT8_ARRAY = 15,
    MSG_INT16_ARRAY = 16,
    MSG_UINT16_ARRAY = 17,
    MSG_INT32_ARRAY = 18,
    MSG_UINT32_ARRAY = 19,
    MSG_INT64_ARRAY = 20,
    MSG_UINT64_ARRAY = 21,
    MSG_FLOAT32_ARRAY = 22,
    MSG_FLOAT64_ARRAY = 23,
    MSG_STRING_ARRAY = 24,
    MSG_NESTED = 25,
    MSG_NESTED_ARRAY = 26,
};

template<typename T>
inline T read_value_from_buffer(const uint8_t* buffer, uint32_t& offset)
{
    T value{};
    uint8_t* value_bytes = reinterpret_cast<uint8_t*>(&value);
    uint32_t data_len = sizeof(T);
    uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
    uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;
    uint32_t cut_cnt = ((pure_data_offset + data_len) / IPC_MSG_MAX_SIZE) - (pure_data_offset / IPC_MSG_MAX_SIZE);
    uint32_t has_copy_size = 0;
    for (int i = 0; i < cut_cnt; ++i)
    {
        uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                      data_len - has_copy_size);
        std::memcpy(value_bytes + has_copy_size, buffer + offset, copy_size);

        offset += copy_size + TAIL_MSG_SIZE;   // 跨过数据长度外，还要跨过那 12 个尾部特征字节
        pure_data_offset += copy_size;
        has_copy_size += copy_size;
    }
    long remaining_size = data_len - has_copy_size;
    if (remaining_size > 0)
    {
        std::memcpy(value_bytes + has_copy_size, buffer + offset, remaining_size);
        offset += remaining_size;
    }
    return value;
}

inline std::string read_string_from_buffer(const uint8_t* buffer, uint32_t& offset)
{
    int32_t str_size = read_value_from_buffer<int32_t>(buffer, offset);
    std::string str;
    if (str_size > 0)
    {
        str.resize(str_size);
        uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
        uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;
        uint32_t cut_cnt = ((pure_data_offset + str_size) / IPC_MSG_MAX_SIZE) - (pure_data_offset / IPC_MSG_MAX_SIZE);
        uint32_t has_copy_size = 0;
        for (int i = 0; i < cut_cnt; ++i)
        {
            uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                          static_cast<uint32_t>(str_size) - has_copy_size);
            std::memcpy(&str[0] + has_copy_size, buffer + offset, copy_size);

            offset += copy_size + TAIL_MSG_SIZE;   // 跨过数据长度外，还要跨过那 12 个尾部特征字节
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        long remaining_size = str_size - has_copy_size;
        if (remaining_size > 0)
        {
            std::memcpy(&str[0] + has_copy_size, buffer + offset, remaining_size);
            offset += remaining_size;
        }
    }
    return str;
}

inline ipc::buffer read_raw_buffer_from_buffer(const uint8_t* buffer, uint32_t& offset, const int32_t data_size)
{
    ipc::buffer nested_buffer(new uint8_t[data_size], data_size,
                              [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
    if (data_size <= 0)
    {
        return nested_buffer;
    }

    uint8_t* value_ptr = static_cast<uint8_t*>(nested_buffer.data());
    uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
    uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;
    uint32_t cut_cnt = ((pure_data_offset + data_size) / IPC_MSG_MAX_SIZE) - (pure_data_offset / IPC_MSG_MAX_SIZE);
    uint32_t has_copy_size = 0;
    for (uint32_t i = 0; i < cut_cnt; ++i)
    {
        uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                      static_cast<uint32_t>(data_size) - has_copy_size);
        std::memcpy(value_ptr + has_copy_size, buffer + offset, copy_size);

        offset += copy_size + TAIL_MSG_SIZE;
        pure_data_offset += copy_size;
        has_copy_size += copy_size;
    }

    long remaining_size = data_size - has_copy_size;
    if (remaining_size > 0)
    {
        std::memcpy(value_ptr + has_copy_size, buffer + offset, remaining_size);
        offset += remaining_size;
    }
    return nested_buffer;
}

inline std::string indent_lines(const std::string& input, const std::string& indent)
{
    std::stringstream source(input);
    std::stringstream result;
    std::string line;
    while (std::getline(source, line))
    {
        result << indent << line << "\n";
    }
    return result.str();
}

template<typename T>
inline std::vector<T> read_array_from_buffer(const uint8_t* buffer, uint32_t& offset, const int32_t element_count)
{
    // 如果是 bool 类型，需要特殊处理，因为 std::vector<bool> 是位优化的
    if constexpr (std::is_same_v<T, bool>)
    {
        std::vector<uint8_t> temp_array(element_count);
        int32_t total_size = element_count * sizeof(uint8_t);
        uint8_t* value_ptr = temp_array.data();

        uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
        uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;
        uint32_t cut_cnt = ((pure_data_offset + total_size) / IPC_MSG_MAX_SIZE) - (pure_data_offset / IPC_MSG_MAX_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i)
        {
            uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                          static_cast<uint32_t>(total_size) - has_copy_size);
            std::memcpy(value_ptr + has_copy_size, buffer + offset, copy_size);

            offset += copy_size + TAIL_MSG_SIZE;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        long remaining_size = total_size - has_copy_size;
        if (remaining_size > 0)
        {
            std::memcpy(value_ptr + has_copy_size, buffer + offset, remaining_size);
            offset += remaining_size;
        }

        std::vector<bool> array(element_count);
        for (int32_t i = 0; i < element_count; ++i)
        {
            array[i] = (temp_array[i] != 0);
        }
        return array;
    }
    else
    {
        int32_t total_size = element_count * sizeof(T);
        std::vector<T> array(element_count);
        uint8_t* value_ptr = reinterpret_cast<uint8_t*>(array.data());

        uint32_t passed_tails = offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
        uint32_t pure_data_offset = offset - passed_tails * TAIL_MSG_SIZE;
        uint32_t cut_cnt = ((pure_data_offset + total_size) / IPC_MSG_MAX_SIZE) - (pure_data_offset / IPC_MSG_MAX_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i)
        {
            uint32_t copy_size = std::min(IPC_MSG_MAX_SIZE - (pure_data_offset % IPC_MSG_MAX_SIZE),
                                          static_cast<uint32_t>(total_size) - has_copy_size);
            std::memcpy(value_ptr + has_copy_size, buffer + offset, copy_size);

            offset += copy_size + TAIL_MSG_SIZE;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        long remaining_size = total_size - has_copy_size;
        if (remaining_size > 0)
        {
            std::memcpy(value_ptr + has_copy_size, buffer + offset, remaining_size);
            offset += remaining_size;
        }
        return array;
    }
}

inline std::string msg_to_string(ipc::buffer& raw_data)
{
    if (raw_data.empty())
    {
        return std::string("");
    }
    const uint8_t* buffer = static_cast<const uint8_t*>(raw_data.data());
    uint32_t size = static_cast<uint32_t>(raw_data.size());
    std::stringstream ss;
    uint32_t offset = 0;
    uint32_t page = 0;
    while (true)
    {
        // 读取尾部特征字节，判断是否还有下一页数据
        if (offset + TAIL_MSG_SIZE >= size)
        {
            break;
        }
        else if (offset % 1'472 == IPC_MSG_MAX_SIZE)
        {
            page++;
            offset += TAIL_MSG_SIZE;   // 跳过尾部特征字节
        }
        std::string name = read_string_from_buffer(buffer, offset);
        uint8_t type = read_value_from_buffer<uint8_t>(buffer, offset);
        switch (type)
        {
        case MsgType::MSG_BOOL:
            {
                bool value = read_value_from_buffer<uint8_t>(buffer, offset) == 1;
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_INT8:
            {
                int8_t value = read_value_from_buffer<int8_t>(buffer, offset);
                ss << name << ": " << static_cast<int>(value) << "\n";
            }
            break;
        case MsgType::MSG_UINT8:
            {
                uint8_t value = read_value_from_buffer<uint8_t>(buffer, offset);
                ss << name << ": " << static_cast<unsigned int>(value) << "\n";
            }
            break;
        case MsgType::MSG_INT16:
            {
                int16_t value = read_value_from_buffer<int16_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_UINT16:
            {
                uint16_t value = read_value_from_buffer<uint16_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_INT32:
            {
                int32_t value = read_value_from_buffer<int32_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_UINT32:
            {
                uint32_t value = read_value_from_buffer<uint32_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_INT64:
            {
                int64_t value = read_value_from_buffer<int64_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_UINT64:
            {
                uint64_t value = read_value_from_buffer<uint64_t>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_FLOAT32:
            {
                float value = read_value_from_buffer<float>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_FLOAT64:
            {
                double value = read_value_from_buffer<double>(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_STRING:
            {
                std::string value = read_string_from_buffer(buffer, offset);
                ss << name << ": " << value << "\n";
            }
            break;
        case MsgType::MSG_BOOL_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<bool> value = read_array_from_buffer<bool>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_INT8_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<int8_t> value = read_array_from_buffer<int8_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << static_cast<int>(value[i]);
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_UINT8_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<uint8_t> value = read_array_from_buffer<uint8_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << static_cast<unsigned int>(value[i]);
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_INT16_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<int16_t> value = read_array_from_buffer<int16_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_UINT16_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<uint16_t> value = read_array_from_buffer<uint16_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_INT32_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<int32_t> value = read_array_from_buffer<int32_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_UINT32_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<uint32_t> value = read_array_from_buffer<uint32_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_INT64_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<int64_t> value = read_array_from_buffer<int64_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_UINT64_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<uint64_t> value = read_array_from_buffer<uint64_t>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_FLOAT32_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<float> value = read_array_from_buffer<float>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_FLOAT64_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<double> value = read_array_from_buffer<double>(buffer, offset, count);
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << value[i];
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_STRING_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                std::vector<std::string> value;
                value.reserve(count);
                for (int32_t i = 0; i < count; ++i)
                {
                    value.push_back(read_string_from_buffer(buffer, offset));
                }
                ss << name << ": [ ";
                for (size_t i = 0; i < value.size(); ++i)
                {
                    ss << "\"" << value[i] << "\"";
                    if (i != value.size() - 1)
                    {
                        ss << ", ";
                    }
                }
                ss << " ]\n";
            }
            break;
        case MsgType::MSG_NESTED:
            {
                int32_t nested_size = read_value_from_buffer<int32_t>(buffer, offset);
                ipc::buffer nested_buffer = read_raw_buffer_from_buffer(buffer, offset, nested_size);
                ss << name << ":\n";
                ss << indent_lines(msg_to_string(nested_buffer), "  ");
            }
            break;
        case MsgType::MSG_NESTED_ARRAY:
            {
                int32_t count = read_value_from_buffer<int32_t>(buffer, offset);
                ss << name << ": [\n";
                for (int32_t i = 0; i < count; ++i)
                {
                    int32_t nested_size = read_value_from_buffer<int32_t>(buffer, offset);
                    ipc::buffer nested_buffer = read_raw_buffer_from_buffer(buffer, offset, nested_size);
                    ss << "  -\n";
                    ss << indent_lines(msg_to_string(nested_buffer), "    ");
                }
                ss << "]\n";
            }
            break;
        default:
            throw std::runtime_error("Unsupported message type" + std::to_string(type) + " for field: " + name);
        }
    }
    return ss.str();
}

#undef TAIL_MSG_SIZE
#undef IPC_MSG_MAX_SIZE
}   // namespace dzIPC
