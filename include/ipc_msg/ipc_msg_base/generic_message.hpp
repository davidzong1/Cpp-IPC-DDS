#pragma once
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace dzIPC {

/// 消息字段类型枚举，与 MsgType 一致
enum FieldType : uint8_t {
    FT_BOOL = 1,
    FT_INT8 = 2,
    FT_UINT8 = 3,
    FT_INT16 = 4,
    FT_UINT16 = 5,
    FT_INT32 = 6,
    FT_UINT32 = 7,
    FT_INT64 = 8,
    FT_UINT64 = 9,
    FT_FLOAT32 = 10,
    FT_FLOAT64 = 11,
    FT_STRING = 12,
    FT_BOOL_ARRAY = 13,
    FT_INT8_ARRAY = 14,
    FT_UINT8_ARRAY = 15,
    FT_INT16_ARRAY = 16,
    FT_UINT16_ARRAY = 17,
    FT_INT32_ARRAY = 18,
    FT_UINT32_ARRAY = 19,
    FT_INT64_ARRAY = 20,
    FT_UINT64_ARRAY = 21,
    FT_FLOAT32_ARRAY = 22,
    FT_FLOAT64_ARRAY = 23,
    FT_STRING_ARRAY = 24,
    FT_NESTED = 25,
    FT_NESTED_ARRAY = 26,
};

/// 动态消息类 — 运行时定义字段，序列化格式与生成的消息头文件完全兼容。
/// 添加到 libipc.so 后，新增 msg/srv 类型无需重新编译任何 C++ 代码。
class GenericMessage : public IpcMsgBase
{
public:
    GenericMessage() = default;
    ~GenericMessage() = default;

    // ---- 字段管理 ----

    /// 清空所有字段
    void clear()
    {
        fields_.clear();
        dzflat_seg_.clear();
        dzflat_seg_hash_ = 0;
    }

    /// 返回字段数量
    size_t field_count() const { return fields_.size(); }

    /// 获取第 i 个字段名
    const std::string& field_name(size_t i) const { return fields_.at(i).name; }

    /// 获取第 i 个字段类型
    uint8_t field_type(size_t i) const { return fields_.at(i).type; }

    // ---- 标量 setters ----
    void set_bool(const std::string& name, bool val);
    void set_int8(const std::string& name, int8_t val);
    void set_uint8(const std::string& name, uint8_t val);
    void set_int16(const std::string& name, int16_t val);
    void set_uint16(const std::string& name, uint16_t val);
    void set_int32(const std::string& name, int32_t val);
    void set_uint32(const std::string& name, uint32_t val);
    void set_int64(const std::string& name, int64_t val);
    void set_uint64(const std::string& name, uint64_t val);
    void set_float32(const std::string& name, float val);
    void set_float64(const std::string& name, double val);
    void set_string(const std::string& name, const std::string& val);
    void set_nested(const std::string& name, const GenericMessage& val);

    // ---- 标量 getters ----
    bool        get_bool(const std::string& name) const;
    int8_t      get_int8(const std::string& name) const;
    uint8_t     get_uint8(const std::string& name) const;
    int16_t     get_int16(const std::string& name) const;
    uint16_t    get_uint16(const std::string& name) const;
    int32_t     get_int32(const std::string& name) const;
    uint32_t    get_uint32(const std::string& name) const;
    int64_t     get_int64(const std::string& name) const;
    uint64_t    get_uint64(const std::string& name) const;
    float       get_float32(const std::string& name) const;
    double      get_float64(const std::string& name) const;
    std::string get_string(const std::string& name) const;
    GenericMessage get_nested(const std::string& name) const;

    // ---- 数组 setters ----
    void set_bool_array(const std::string& name, const std::vector<uint8_t>& arr);
    void set_int8_array(const std::string& name, const std::vector<int8_t>& arr);
    void set_uint8_array(const std::string& name, const std::vector<uint8_t>& arr);
    void set_int16_array(const std::string& name, const std::vector<int16_t>& arr);
    void set_uint16_array(const std::string& name, const std::vector<uint16_t>& arr);
    void set_int32_array(const std::string& name, const std::vector<int32_t>& arr);
    void set_uint32_array(const std::string& name, const std::vector<uint32_t>& arr);
    void set_int64_array(const std::string& name, const std::vector<int64_t>& arr);
    void set_uint64_array(const std::string& name, const std::vector<uint64_t>& arr);
    void set_float32_array(const std::string& name, const std::vector<float>& arr);
    void set_float64_array(const std::string& name, const std::vector<double>& arr);
    void set_string_array(const std::string& name, const std::vector<std::string>& arr);
    void set_nested_array(const std::string& name, const std::vector<GenericMessage>& arr);

    // ---- 数组 getters ----
    std::vector<uint8_t>  get_bool_array(const std::string& name) const;
    std::vector<int8_t>   get_int8_array(const std::string& name) const;
    std::vector<uint8_t>  get_uint8_array(const std::string& name) const;
    std::vector<int16_t>  get_int16_array(const std::string& name) const;
    std::vector<uint16_t> get_uint16_array(const std::string& name) const;
    std::vector<int32_t>  get_int32_array(const std::string& name) const;
    std::vector<uint32_t> get_uint32_array(const std::string& name) const;
    std::vector<int64_t>  get_int64_array(const std::string& name) const;
    std::vector<uint64_t> get_uint64_array(const std::string& name) const;
    std::vector<float>    get_float32_array(const std::string& name) const;
    std::vector<double>   get_float64_array(const std::string& name) const;
    std::vector<std::string> get_string_array(const std::string& name) const;
    std::vector<GenericMessage> get_nested_array(const std::string& name) const;

    // ---- IpcMsgBase 接口 ----
    ipc::buffer serialize() override;
    void deserialize(const ipc::buffer& buffer) override;

    /* ------------------------------------------------------- DZFlat 段直通
     *
     * GenericMessage 是**自描述 TLV** 的通用走查器 —— 它靠 wire 里的字段名工作。
     * DZFlat 是定长布局, wire 里没有字段名, 所以本类型在 C++ 侧**无法**把它解析成
     * fields_: 缺 schema。而 schema 只存在于 generator 产出的物件里, 且没有任何 TU
     * 会编译那些生成头文件(Python 进程尤其如此), 所以 C++ 侧拿不到。
     *
     * 因此这里只做直通: 原样留存段字节 + schema 指纹, 由**持有 schema 的一侧**去解码。
     * 落地形态是 Python: generator 另外发射一份 Python schema
     * (python/dzipc/gen_msgs/_dzflat_schema.py), 由 python/dzipc/dzflat.py 解码。
     * 这样 Python 侧还顺带拿到了 TLV 路径给不了的东西 —— 大数组可以用 memoryview
     * 零拷贝读, 而不是 get_uint8_array() 那样返回一个百万元素的 Python list。
     *
     * 不这样做的后果不是"退化", 而是**静默丢消息**: 订阅循环在 dzflat_read 返回 false
     * 时会 continue(见 shm_pub_sub_ipc.cc 的双 wire 分派)。
     *
     * 注意: 持有段的 GenericMessage 是只读直通体, fields_ 为空 —— 两者互斥。
     * 它不能被 serialize() 回 TLV(同样缺 schema), 只能整段转发。
     */
    bool dzflat_read(const void* seg, size_t size) override
    {
        if (!dzflat::looks_like_dzflat(seg, size))
        {
            return false;
        }
        dzflat::SegHeader h{};
        std::memcpy(&h, seg, sizeof(h));
        const auto* p = static_cast<const uint8_t*>(seg);
        dzflat_seg_.assign(p, p + h.total_size);
        dzflat_seg_hash_ = h.schema_hash;
        fields_.clear();   /* 与 TLV 字段互斥 */
        return true;
    }

    /// 是否持有一个 DZFlat 段(而非 TLV 字段)。
    bool has_dzflat() const noexcept { return !dzflat_seg_.empty(); }

    /// 所持段的 schema 指纹; 未持有时为 0。用于在 Python 侧查 schema 注册表。
    uint32_t dzflat_seg_schema_hash() const noexcept { return dzflat_seg_hash_; }

    /// 所持段的原始字节。生命周期与本对象绑定。
    const std::vector<uint8_t>& dzflat_seg() const noexcept { return dzflat_seg_; }
    GenericMessage* clone() const override { return new GenericMessage(*this); }

private:
    struct FieldEntry {
        std::string name;
        uint8_t type{0};
        std::vector<uint8_t> data;
    };

    std::vector<FieldEntry> fields_;
    /* DZFlat 直通载荷。非空即表示本对象持有一个 DZFlat 段, 此时 fields_ 为空。 */
    std::vector<uint8_t> dzflat_seg_;
    uint32_t dzflat_seg_hash_ = 0;

    // 按名称查找字段（返回索引），未找到返回 -1
    int find_field(const std::string& name) const;

    // 辅助：将标量写入 data 缓冲
    template<typename T>
    static void append_raw(std::vector<uint8_t>& dst, const T& val) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        dst.insert(dst.end(), p, p + sizeof(T));
    }

    // 辅助：从 data 缓冲读出标量
    template<typename T>
    static T read_raw(const std::vector<uint8_t>& src, size_t offset = 0) {
        T val{};
        if (offset + sizeof(T) <= src.size()) {
            std::memcpy(&val, src.data() + offset, sizeof(T));
        }
        return val;
    }

    // 辅助：set 标量字段
    template<typename T>
    void set_scalar(const std::string& name, uint8_t type_id, const T& val) {
        int idx = find_field(name);
        if (idx < 0) {
            idx = static_cast<int>(fields_.size());
            fields_.emplace_back();
        }
        auto& f = fields_[idx];
        f.name = name;
        f.type = type_id;
        f.data.clear();
        append_raw(f.data, val);
    }

    // 辅助：set 字符串字段
    void set_string_impl(const std::string& name, uint8_t type_id, const std::string& val) {
        int idx = find_field(name);
        if (idx < 0) {
            idx = static_cast<int>(fields_.size());
            fields_.emplace_back();
        }
        auto& f = fields_[idx];
        f.name = name;
        f.type = type_id;
        f.data.clear();
        int32_t sz = static_cast<int32_t>(val.size());
        append_raw(f.data, sz);
        f.data.insert(f.data.end(),
                      reinterpret_cast<const uint8_t*>(val.data()),
                      reinterpret_cast<const uint8_t*>(val.data()) + val.size());
    }

    // 辅助：set 嵌套消息字段。data 保存为 int32_t size + serialized bytes。
    void set_nested_impl(const std::string& name, uint8_t type_id, const GenericMessage& val) {
        int idx = find_field(name);
        if (idx < 0) {
            idx = static_cast<int>(fields_.size());
            fields_.emplace_back();
        }
        auto& f = fields_[idx];
        f.name = name;
        f.type = type_id;
        f.data.clear();
        ipc::buffer serialized = const_cast<GenericMessage&>(val).serialize();
        int32_t sz = static_cast<int32_t>(serialized.size());
        append_raw(f.data, sz);
        if (sz > 0) {
            const uint8_t* p = static_cast<const uint8_t*>(serialized.data());
            f.data.insert(f.data.end(), p, p + sz);
        }
    }

    // 辅助：set 基本类型数组
    template<typename T>
    void set_array_impl(const std::string& name, uint8_t type_id,
                        const T* elements, int32_t count) {
        int idx = find_field(name);
        if (idx < 0) {
            idx = static_cast<int>(fields_.size());
            fields_.emplace_back();
        }
        auto& f = fields_[idx];
        f.name = name;
        f.type = type_id;
        f.data.clear();
        append_raw(f.data, count);
        if (count > 0) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(elements);
            f.data.insert(f.data.end(), p, p + count * sizeof(T));
        }
    }

    // 辅助：get 标量字段
    template<typename T>
    T get_scalar(const std::string& name) const {
        int idx = find_field(name);
        if (idx < 0) throw std::runtime_error("Field not found: " + name);
        const auto& f = fields_[idx];
        if (f.data.size() < sizeof(T))
            throw std::runtime_error("Field data too short: " + name);
        return read_raw<T>(f.data, 0);
    }

    // 辅助：get 数组字段
    template<typename T>
    std::vector<T> get_array_impl(const std::string& name) const {
        int idx = find_field(name);
        if (idx < 0) throw std::runtime_error("Field not found: " + name);
        const auto& f = fields_[idx];
        if (f.data.size() < sizeof(int32_t))
            throw std::runtime_error("Array field data too short: " + name);
        int32_t count = read_raw<int32_t>(f.data, 0);
        std::vector<T> result(count);
        if (count > 0) {
            size_t data_offset = sizeof(int32_t);
            size_t expected = data_offset + count * sizeof(T);
            if (f.data.size() < expected)
                throw std::runtime_error("Array field data truncated: " + name);
            std::memcpy(result.data(), f.data.data() + data_offset, count * sizeof(T));
        }
        return result;
    }

public:
    // ---- 从 buffer 中按页读取（用于 deserialize 内部实现） ----
    // 这些 inline helpers 直接内联在 serialization/deserialization 方法中使用

private:
    // 跳过页尾标记（反序列化时使用）
    static inline void skip_tail_if_needed(uint32_t& offset) {
        constexpr uint32_t PAGE_SIZE = 1460;
        constexpr uint32_t TAIL = 12;
        if (offset >= PAGE_SIZE && (offset - PAGE_SIZE) % (PAGE_SIZE + TAIL) < PAGE_SIZE) {
            // offset 恰好落在数据结束处，下一页之前需要跳过 tail
        }
        // 检查是否需要跳过 tail：offset % (1460+12) >= 1460
        uint32_t pos_in_block = offset % (PAGE_SIZE + TAIL);
        if (pos_in_block >= PAGE_SIZE) {
            offset += (PAGE_SIZE + TAIL) - pos_in_block; // 跳到下一页数据开始
        }
    }

    // 从 buffer 中按页读数据（对应 adapt_memcpy_tods 的逻辑）
    template<typename T>
    static T read_paged(const uint8_t* buf, uint32_t buf_size, uint32_t& offset) {
        constexpr uint32_t PAGE_SIZE = 1460;
        constexpr uint32_t TAIL = 12;
        T val{};
        uint8_t* dst = reinterpret_cast<uint8_t*>(&val);
        uint32_t data_len = sizeof(T);
        uint32_t passed_tails = offset / (PAGE_SIZE + TAIL);
        uint32_t pure_data_offset = offset - passed_tails * TAIL;
        uint32_t cut_cnt = ((pure_data_offset + data_len) / PAGE_SIZE)
                         - (pure_data_offset / PAGE_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i) {
            uint32_t copy_size = std::min(PAGE_SIZE - (pure_data_offset % PAGE_SIZE),
                                          data_len - has_copy_size);
            if (offset + copy_size > buf_size) break;
            std::memcpy(dst + has_copy_size, buf + offset, copy_size);
            offset += copy_size + TAIL;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        uint32_t remaining = data_len - has_copy_size;
        if (remaining > 0 && offset + remaining <= buf_size) {
            std::memcpy(dst + has_copy_size, buf + offset, remaining);
            offset += remaining;
        }
        return val;
    }

    // 从 buffer 按页读字符串
    static std::string read_string_paged(const uint8_t* buf, uint32_t buf_size, uint32_t& offset,
                                         int32_t str_size) {
        std::string result;
        if (str_size <= 0) return result;
        result.resize(str_size);
        constexpr uint32_t PAGE_SIZE = 1460;
        constexpr uint32_t TAIL = 12;
        uint32_t passed_tails = offset / (PAGE_SIZE + TAIL);
        uint32_t pure_data_offset = offset - passed_tails * TAIL;
        uint32_t data_len = static_cast<uint32_t>(str_size);
        uint32_t cut_cnt = ((pure_data_offset + data_len) / PAGE_SIZE)
                         - (pure_data_offset / PAGE_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i) {
            uint32_t copy_size = std::min(PAGE_SIZE - (pure_data_offset % PAGE_SIZE),
                                          data_len - has_copy_size);
            if (offset + copy_size > buf_size) break;
            std::memcpy(&result[0] + has_copy_size, buf + offset, copy_size);
            offset += copy_size + TAIL;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        uint32_t remaining = data_len - has_copy_size;
        if (remaining > 0 && offset + remaining <= buf_size) {
            std::memcpy(&result[0] + has_copy_size, buf + offset, remaining);
            offset += remaining;
        }
        return result;
    }

    static std::vector<uint8_t> read_bytes_paged(const uint8_t* buf, uint32_t buf_size, uint32_t& offset,
                                                 int32_t data_size) {
        std::vector<uint8_t> result;
        if (data_size <= 0) return result;
        result.resize(data_size);
        constexpr uint32_t PAGE_SIZE = 1460;
        constexpr uint32_t TAIL = 12;
        uint32_t passed_tails = offset / (PAGE_SIZE + TAIL);
        uint32_t pure_data_offset = offset - passed_tails * TAIL;
        uint32_t data_len = static_cast<uint32_t>(data_size);
        uint32_t cut_cnt = ((pure_data_offset + data_len) / PAGE_SIZE)
                         - (pure_data_offset / PAGE_SIZE);
        uint32_t has_copy_size = 0;
        for (uint32_t i = 0; i < cut_cnt; ++i) {
            uint32_t copy_size = std::min(PAGE_SIZE - (pure_data_offset % PAGE_SIZE),
                                          data_len - has_copy_size);
            if (offset + copy_size > buf_size) break;
            std::memcpy(result.data() + has_copy_size, buf + offset, copy_size);
            offset += copy_size + TAIL;
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        uint32_t remaining = data_len - has_copy_size;
        if (remaining > 0 && offset + remaining <= buf_size) {
            std::memcpy(result.data() + has_copy_size, buf + offset, remaining);
            offset += remaining;
        }
        return result;
    }
};

// ============ inline 实现 ============

inline int GenericMessage::find_field(const std::string& name) const {
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// ---- 标量 setters ----

inline void GenericMessage::set_bool(const std::string& name, bool val) {
    uint8_t v = val ? 1 : 0;
    set_scalar(name, FT_BOOL, v);
}
inline void GenericMessage::set_int8(const std::string& name, int8_t val) {
    set_scalar(name, FT_INT8, val);
}
inline void GenericMessage::set_uint8(const std::string& name, uint8_t val) {
    set_scalar(name, FT_UINT8, val);
}
inline void GenericMessage::set_int16(const std::string& name, int16_t val) {
    set_scalar(name, FT_INT16, val);
}
inline void GenericMessage::set_uint16(const std::string& name, uint16_t val) {
    set_scalar(name, FT_UINT16, val);
}
inline void GenericMessage::set_int32(const std::string& name, int32_t val) {
    set_scalar(name, FT_INT32, val);
}
inline void GenericMessage::set_uint32(const std::string& name, uint32_t val) {
    set_scalar(name, FT_UINT32, val);
}
inline void GenericMessage::set_int64(const std::string& name, int64_t val) {
    set_scalar(name, FT_INT64, val);
}
inline void GenericMessage::set_uint64(const std::string& name, uint64_t val) {
    set_scalar(name, FT_UINT64, val);
}
inline void GenericMessage::set_float32(const std::string& name, float val) {
    set_scalar(name, FT_FLOAT32, val);
}
inline void GenericMessage::set_float64(const std::string& name, double val) {
    set_scalar(name, FT_FLOAT64, val);
}
inline void GenericMessage::set_string(const std::string& name, const std::string& val) {
    set_string_impl(name, FT_STRING, val);
}
inline void GenericMessage::set_nested(const std::string& name, const GenericMessage& val) {
    set_nested_impl(name, FT_NESTED, val);
}

// ---- 标量 getters ----

inline bool GenericMessage::get_bool(const std::string& name) const {
    return get_scalar<uint8_t>(name) != 0;
}
inline int8_t GenericMessage::get_int8(const std::string& name) const {
    return get_scalar<int8_t>(name);
}
inline uint8_t GenericMessage::get_uint8(const std::string& name) const {
    return get_scalar<uint8_t>(name);
}
inline int16_t GenericMessage::get_int16(const std::string& name) const {
    return get_scalar<int16_t>(name);
}
inline uint16_t GenericMessage::get_uint16(const std::string& name) const {
    return get_scalar<uint16_t>(name);
}
inline int32_t GenericMessage::get_int32(const std::string& name) const {
    return get_scalar<int32_t>(name);
}
inline uint32_t GenericMessage::get_uint32(const std::string& name) const {
    return get_scalar<uint32_t>(name);
}
inline int64_t GenericMessage::get_int64(const std::string& name) const {
    return get_scalar<int64_t>(name);
}
inline uint64_t GenericMessage::get_uint64(const std::string& name) const {
    return get_scalar<uint64_t>(name);
}
inline float GenericMessage::get_float32(const std::string& name) const {
    return get_scalar<float>(name);
}
inline double GenericMessage::get_float64(const std::string& name) const {
    return get_scalar<double>(name);
}
inline std::string GenericMessage::get_string(const std::string& name) const {
    int idx = find_field(name);
    if (idx < 0) throw std::runtime_error("Field not found: " + name);
    const auto& f = fields_[idx];
    if (f.data.size() < sizeof(int32_t))
        throw std::runtime_error("String field data too short: " + name);
    int32_t sz = read_raw<int32_t>(f.data, 0);
    if (sz < 0) return {};
    return std::string(reinterpret_cast<const char*>(f.data.data() + sizeof(int32_t)),
                       static_cast<size_t>(sz));
}
inline GenericMessage GenericMessage::get_nested(const std::string& name) const {
    int idx = find_field(name);
    if (idx < 0) throw std::runtime_error("Field not found: " + name);
    const auto& f = fields_[idx];
    if (f.data.size() < sizeof(int32_t))
        throw std::runtime_error("Nested field data too short: " + name);
    int32_t sz = read_raw<int32_t>(f.data, 0);
    if (sz < 0 || f.data.size() < sizeof(int32_t) + static_cast<size_t>(sz))
        throw std::runtime_error("Nested field data truncated: " + name);
    auto* copy = new uint8_t[sz];
    if (sz > 0) {
        std::memcpy(copy, f.data.data() + sizeof(int32_t), static_cast<size_t>(sz));
    }
    ipc::buffer buf(copy, static_cast<size_t>(sz), [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
    GenericMessage nested;
    nested.deserialize(buf);
    return nested;
}

// ---- 数组 setters (bool 用 uint8_t 容器传) ----

inline void GenericMessage::set_bool_array(const std::string& name, const std::vector<uint8_t>& arr) {
    set_array_impl(name, FT_BOOL_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_int8_array(const std::string& name, const std::vector<int8_t>& arr) {
    set_array_impl(name, FT_INT8_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_uint8_array(const std::string& name, const std::vector<uint8_t>& arr) {
    set_array_impl(name, FT_UINT8_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_int16_array(const std::string& name, const std::vector<int16_t>& arr) {
    set_array_impl(name, FT_INT16_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_uint16_array(const std::string& name, const std::vector<uint16_t>& arr) {
    set_array_impl(name, FT_UINT16_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_int32_array(const std::string& name, const std::vector<int32_t>& arr) {
    set_array_impl(name, FT_INT32_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_uint32_array(const std::string& name, const std::vector<uint32_t>& arr) {
    set_array_impl(name, FT_UINT32_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_int64_array(const std::string& name, const std::vector<int64_t>& arr) {
    set_array_impl(name, FT_INT64_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_uint64_array(const std::string& name, const std::vector<uint64_t>& arr) {
    set_array_impl(name, FT_UINT64_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_float32_array(const std::string& name, const std::vector<float>& arr) {
    set_array_impl(name, FT_FLOAT32_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_float64_array(const std::string& name, const std::vector<double>& arr) {
    set_array_impl(name, FT_FLOAT64_ARRAY, arr.data(), static_cast<int32_t>(arr.size()));
}
inline void GenericMessage::set_string_array(const std::string& name, const std::vector<std::string>& arr) {
    int idx = find_field(name);
    if (idx < 0) {
        idx = static_cast<int>(fields_.size());
        fields_.emplace_back();
    }
    auto& f = fields_[idx];
    f.name = name;
    f.type = FT_STRING_ARRAY;
    f.data.clear();
    int32_t count = static_cast<int32_t>(arr.size());
    append_raw(f.data, count);
    for (const auto& s : arr) {
        int32_t sz = static_cast<int32_t>(s.size());
        append_raw(f.data, sz);
        f.data.insert(f.data.end(),
                      reinterpret_cast<const uint8_t*>(s.data()),
                      reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    }
}
inline void GenericMessage::set_nested_array(const std::string& name, const std::vector<GenericMessage>& arr) {
    int idx = find_field(name);
    if (idx < 0) {
        idx = static_cast<int>(fields_.size());
        fields_.emplace_back();
    }
    auto& f = fields_[idx];
    f.name = name;
    f.type = FT_NESTED_ARRAY;
    f.data.clear();
    int32_t count = static_cast<int32_t>(arr.size());
    append_raw(f.data, count);
    for (const auto& nested : arr) {
        ipc::buffer serialized = const_cast<GenericMessage&>(nested).serialize();
        int32_t sz = static_cast<int32_t>(serialized.size());
        append_raw(f.data, sz);
        if (sz > 0) {
            const uint8_t* p = static_cast<const uint8_t*>(serialized.data());
            f.data.insert(f.data.end(), p, p + sz);
        }
    }
}

// ---- 数组 getters ----

inline std::vector<uint8_t> GenericMessage::get_bool_array(const std::string& name) const {
    return get_array_impl<uint8_t>(name);
}
inline std::vector<int8_t> GenericMessage::get_int8_array(const std::string& name) const {
    return get_array_impl<int8_t>(name);
}
inline std::vector<uint8_t> GenericMessage::get_uint8_array(const std::string& name) const {
    return get_array_impl<uint8_t>(name);
}
inline std::vector<int16_t> GenericMessage::get_int16_array(const std::string& name) const {
    return get_array_impl<int16_t>(name);
}
inline std::vector<uint16_t> GenericMessage::get_uint16_array(const std::string& name) const {
    return get_array_impl<uint16_t>(name);
}
inline std::vector<int32_t> GenericMessage::get_int32_array(const std::string& name) const {
    return get_array_impl<int32_t>(name);
}
inline std::vector<uint32_t> GenericMessage::get_uint32_array(const std::string& name) const {
    return get_array_impl<uint32_t>(name);
}
inline std::vector<int64_t> GenericMessage::get_int64_array(const std::string& name) const {
    return get_array_impl<int64_t>(name);
}
inline std::vector<uint64_t> GenericMessage::get_uint64_array(const std::string& name) const {
    return get_array_impl<uint64_t>(name);
}
inline std::vector<float> GenericMessage::get_float32_array(const std::string& name) const {
    return get_array_impl<float>(name);
}
inline std::vector<double> GenericMessage::get_float64_array(const std::string& name) const {
    return get_array_impl<double>(name);
}
inline std::vector<std::string> GenericMessage::get_string_array(const std::string& name) const {
    int idx = find_field(name);
    if (idx < 0) throw std::runtime_error("Field not found: " + name);
    const auto& f = fields_[idx];
    if (f.data.size() < sizeof(int32_t))
        throw std::runtime_error("String array field data too short: " + name);
    int32_t count = read_raw<int32_t>(f.data, 0);
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(count));
    size_t off = sizeof(int32_t);
    for (int32_t i = 0; i < count; ++i) {
        if (off + sizeof(int32_t) > f.data.size()) break;
        int32_t sz = read_raw<int32_t>(f.data, off);
        off += sizeof(int32_t);
        if (off + sz > f.data.size()) break;
        result.emplace_back(reinterpret_cast<const char*>(f.data.data() + off),
                            static_cast<size_t>(sz));
        off += sz;
    }
    return result;
}
inline std::vector<GenericMessage> GenericMessage::get_nested_array(const std::string& name) const {
    int idx = find_field(name);
    if (idx < 0) throw std::runtime_error("Field not found: " + name);
    const auto& f = fields_[idx];
    if (f.data.size() < sizeof(int32_t))
        throw std::runtime_error("Nested array field data too short: " + name);
    int32_t count = read_raw<int32_t>(f.data, 0);
    std::vector<GenericMessage> result;
    result.reserve(static_cast<size_t>(count));
    size_t off = sizeof(int32_t);
    for (int32_t i = 0; i < count; ++i) {
        if (off + sizeof(int32_t) > f.data.size()) break;
        int32_t sz = read_raw<int32_t>(f.data, off);
        off += sizeof(int32_t);
        if (sz < 0 || off + static_cast<size_t>(sz) > f.data.size()) break;
        auto* copy = new uint8_t[sz];
        if (sz > 0) {
            std::memcpy(copy, f.data.data() + off, static_cast<size_t>(sz));
        }
        ipc::buffer buf(copy, static_cast<size_t>(sz),
                        [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
        GenericMessage nested;
        nested.deserialize(buf);
        result.emplace_back(std::move(nested));
        off += static_cast<size_t>(sz);
    }
    return result;
}

// ============ 序列化 ============

inline ipc::buffer GenericMessage::serialize() {
    if (fields_.empty()) {
        // 返回空缓冲
        return serialize_data_cut(0);
    }

    // 计算原始数据总大小
    uint32_t total_data_size = 0;
    for (const auto& f : fields_) {
        total_data_size += 4;                     // int32_t name_size
        total_data_size += static_cast<uint32_t>(f.name.size()); // name bytes
        total_data_size += 1;                     // uint8_t type
        total_data_size += static_cast<uint32_t>(f.data.size()); // value data
    }

    ipc::buffer buf = std::move(serialize_data_cut(total_data_size));
    uint32_t offset = 0;
    uint16_t page = 1;

    for (const auto& f : fields_) {
        int32_t name_size = static_cast<int32_t>(f.name.size());
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&name_size),
                         page, offset, sizeof(name_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(f.name.data()),
                         page, offset, static_cast<uint32_t>(f.name.size()));
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&f.type),
                         page, offset, sizeof(f.type));
        if (!f.data.empty()) {
            adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                             reinterpret_cast<const uint8_t*>(f.data.data()),
                             page, offset, static_cast<uint32_t>(f.data.size()));
        }
    }

    add_tail_msg(static_cast<uint8_t*>(buf.data()) + offset, page);
    return buf;
}

// ============ 反序列化 ============

inline void GenericMessage::deserialize(const ipc::buffer& buffer) {
    fields_.clear();
    if (buffer.empty() || buffer.size() < 12) return;

    const uint8_t* buf = static_cast<const uint8_t*>(buffer.data());
    uint32_t buf_size = static_cast<uint32_t>(buffer.size());
    uint32_t offset = 0;

    deserialize_data_cut(buf_size);

    constexpr uint32_t PAGE_SIZE = 1460;
    constexpr uint32_t TAIL = 12;

    while (offset + TAIL < buf_size) {
        // 检查是否在页尾边界（需要跳过 tail）
        uint32_t pos_in_block = offset % (PAGE_SIZE + TAIL);
        if (pos_in_block >= PAGE_SIZE) {
            offset += (PAGE_SIZE + TAIL) - pos_in_block;
            continue;
        }

        // 还有多少数据页可以读？检查剩余数据大小
        if (offset + 4 > buf_size) break;

        // 读取字段名长度
        int32_t name_size = read_paged<int32_t>(buf, buf_size, offset);
        if (name_size <= 0 || offset + static_cast<uint32_t>(name_size) > buf_size) break;

        // 读取字段名
        FieldEntry field;
        field.name = read_string_paged(buf, buf_size, offset, name_size);

        // 读取类型
        if (offset + 1 > buf_size) break;
        field.type = read_paged<uint8_t>(buf, buf_size, offset);

        // 根据类型读取值数据
        switch (field.type) {
        case FT_BOOL: {
            if (offset + 1 > buf_size) return;
            uint8_t v = read_paged<uint8_t>(buf, buf_size, offset);
            field.data.push_back(v);
            break;
        }
        case FT_INT8: {
            if (offset + 1 > buf_size) return;
            int8_t v = read_paged<int8_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_UINT8: {
            if (offset + 1 > buf_size) return;
            uint8_t v = read_paged<uint8_t>(buf, buf_size, offset);
            field.data.push_back(v);
            break;
        }
        case FT_INT16: {
            if (offset + 2 > buf_size + TAIL) return;
            int16_t v = read_paged<int16_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_UINT16: {
            if (offset + 2 > buf_size + TAIL) return;
            uint16_t v = read_paged<uint16_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_INT32: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t v = read_paged<int32_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_UINT32: {
            if (offset + 4 > buf_size + TAIL) return;
            uint32_t v = read_paged<uint32_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_INT64: {
            if (offset + 8 > buf_size + TAIL) return;
            int64_t v = read_paged<int64_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_UINT64: {
            if (offset + 8 > buf_size + TAIL) return;
            uint64_t v = read_paged<uint64_t>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_FLOAT32: {
            if (offset + 4 > buf_size + TAIL) return;
            float v = read_paged<float>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_FLOAT64: {
            if (offset + 8 > buf_size + TAIL) return;
            double v = read_paged<double>(buf, buf_size, offset);
            field.data.resize(sizeof(v));
            std::memcpy(field.data.data(), &v, sizeof(v));
            break;
        }
        case FT_STRING: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t str_size = read_paged<int32_t>(buf, buf_size, offset);
            if (str_size < 0) return;
            std::string s = read_string_paged(buf, buf_size, offset, str_size);
            int32_t out_sz = static_cast<int32_t>(s.size());
            field.data.resize(sizeof(int32_t) + s.size());
            std::memcpy(field.data.data(), &out_sz, sizeof(int32_t));
            std::memcpy(field.data.data() + sizeof(int32_t), s.data(), s.size());
            break;
        }
        case FT_NESTED: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t nested_size = read_paged<int32_t>(buf, buf_size, offset);
            if (nested_size < 0) return;
            std::vector<uint8_t> nested_bytes = read_bytes_paged(buf, buf_size, offset, nested_size);
            int32_t out_sz = static_cast<int32_t>(nested_bytes.size());
            field.data.resize(sizeof(int32_t) + nested_bytes.size());
            std::memcpy(field.data.data(), &out_sz, sizeof(int32_t));
            if (!nested_bytes.empty()) {
                std::memcpy(field.data.data() + sizeof(int32_t), nested_bytes.data(), nested_bytes.size());
            }
            break;
        }
        case FT_BOOL_ARRAY:
        case FT_INT8_ARRAY:
        case FT_UINT8_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            field.data.resize(sizeof(int32_t) + count);
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + 1 > buf_size) return;
                uint8_t b = read_paged<uint8_t>(buf, buf_size, offset);
                field.data[sizeof(int32_t) + i] = b;
            }
            break;
        }
        case FT_INT16_ARRAY:
        case FT_UINT16_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            size_t elem_sz = (field.type == FT_INT16_ARRAY) ? sizeof(int16_t) : sizeof(uint16_t);
            field.data.resize(sizeof(int32_t) + count * elem_sz);
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + elem_sz > buf_size + TAIL) return;
                if (field.type == FT_INT16_ARRAY) {
                    int16_t v = read_paged<int16_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                } else {
                    uint16_t v = read_paged<uint16_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                }
            }
            break;
        }
        case FT_INT32_ARRAY:
        case FT_UINT32_ARRAY:
        case FT_FLOAT32_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            size_t elem_sz = 4;
            field.data.resize(sizeof(int32_t) + count * elem_sz);
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + elem_sz > buf_size + TAIL) return;
                if (field.type == FT_INT32_ARRAY) {
                    int32_t v = read_paged<int32_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                } else if (field.type == FT_UINT32_ARRAY) {
                    uint32_t v = read_paged<uint32_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                } else {
                    float v = read_paged<float>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                }
            }
            break;
        }
        case FT_INT64_ARRAY:
        case FT_UINT64_ARRAY:
        case FT_FLOAT64_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            size_t elem_sz = 8;
            field.data.resize(sizeof(int32_t) + count * elem_sz);
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + elem_sz > buf_size + TAIL) return;
                if (field.type == FT_INT64_ARRAY) {
                    int64_t v = read_paged<int64_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                } else if (field.type == FT_UINT64_ARRAY) {
                    uint64_t v = read_paged<uint64_t>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                } else {
                    double v = read_paged<double>(buf, buf_size, offset);
                    std::memcpy(field.data.data() + sizeof(int32_t) + i * elem_sz, &v, elem_sz);
                }
            }
            break;
        }
        case FT_STRING_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            field.data.resize(sizeof(int32_t));
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + 4 > buf_size + TAIL) return;
                int32_t str_size = read_paged<int32_t>(buf, buf_size, offset);
                if (str_size < 0) return;
                std::string s = read_string_paged(buf, buf_size, offset, str_size);
                int32_t out_sz = static_cast<int32_t>(s.size());
                size_t old_sz = field.data.size();
                field.data.resize(old_sz + sizeof(int32_t) + s.size());
                std::memcpy(field.data.data() + old_sz, &out_sz, sizeof(int32_t));
                std::memcpy(field.data.data() + old_sz + sizeof(int32_t), s.data(), s.size());
            }
            break;
        }
        case FT_NESTED_ARRAY: {
            if (offset + 4 > buf_size + TAIL) return;
            int32_t count = read_paged<int32_t>(buf, buf_size, offset);
            if (count < 0) return;
            field.data.resize(sizeof(int32_t));
            std::memcpy(field.data.data(), &count, sizeof(int32_t));
            for (int32_t i = 0; i < count; ++i) {
                if (offset + 4 > buf_size + TAIL) return;
                int32_t nested_size = read_paged<int32_t>(buf, buf_size, offset);
                if (nested_size < 0) return;
                std::vector<uint8_t> nested_bytes = read_bytes_paged(buf, buf_size, offset, nested_size);
                int32_t out_sz = static_cast<int32_t>(nested_bytes.size());
                size_t old_sz = field.data.size();
                field.data.resize(old_sz + sizeof(int32_t) + nested_bytes.size());
                std::memcpy(field.data.data() + old_sz, &out_sz, sizeof(int32_t));
                if (!nested_bytes.empty()) {
                    std::memcpy(field.data.data() + old_sz + sizeof(int32_t),
                                nested_bytes.data(), nested_bytes.size());
                }
            }
            break;
        }
        default:
            // 未知类型，停止解析
            return;
        }

        fields_.push_back(std::move(field));
    }
}

} // namespace dzIPC
