#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "ipc_msg/ipc_msg_base/dzflat.h"
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
    virtual ~IpcMsgBase() = default;

    void set_msg_id(const uint32_t id) { dz_ipc_msg_id = id; }

    uint32_t msg_id() const noexcept { return dz_ipc_msg_id; }

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

    /* ---------------------------------------------------------------------
     * DZFlat 平坦布局接口(SHM 专属旁路, 设计见 docs/dzflat_shm.md)
     *
     * 默认实现表示"本类型不支持 DZFlat"(schema hash 为 0), 于是发布/订阅两侧都会
     * 走既有 TLV 路径。generator 为每个消息类型发射覆写; 手写的消息类型(如 RTPS
     * 控制帧)不覆写即自动留在 TLV 上, 无需改动。
     *
     * 用 msg_id 之外再加 schema hash 的原因: TLV 靠字段名自描述, 增删字段仍能解析;
     * DZFlat 是定长布局, 结构一变就必须拒收, 否则静默错解(docs §3.5)。
     * ------------------------------------------------------------------- */

    /// 本类型的 DZFlat 结构指纹; 0 表示不支持 DZFlat。
    virtual uint32_t dzflat_schema_hash() const noexcept { return 0; }

    bool dzflat_supported() const noexcept { return dzflat_schema_hash() != 0; }

    /// 编码本消息所需的字节数上界(含段头)。不支持时返回 0。
    virtual uint32_t dzflat_size() const { return 0; }

    /// 把本消息按 DZFlat 布局写进 seg(容量 cap)。cap 须 >= dzflat_size()。
    virtual bool dzflat_write(void* seg, uint32_t cap) const { return false; }

    /// 从 DZFlat 段读回本消息(拷回 owning struct)。段不可信, 校验失败返回 false。
    virtual bool dzflat_read(const void* seg, size_t size) { return false; }

    /// DZFlat 段头里的 msg_id 是否与本消息类型一致(对应 TLV 的 check_id)。
    bool check_dzflat_id(const ipc::buffer& data) const
    {
        uint32_t id = 0;
        if (!dzflat_peek_msg_id(data.data(), data.size(), id))
        {
            return false;
        }
        return id == dz_ipc_msg_id;
    }

    /* 上一次 deserialize 是否读越界过。见 docs/dzflat_known_issues.md 第 3 条。
     *
     * adapt_memcpy_tods 的偏移完全由**缓冲自身的内容**算出(字段名长度、数组元素个数都
     * 是从 wire 里读的), 所以一个被截断或错乱的缓冲能把偏移推到任意远处; 而它原本连
     * buffer 大小都拿不到, 根本无法拦 —— 实测能让 deserialize 越界读到 SIGSEGV。
     *
     * 现在越界会被拦下并置位, 调用方据此**丢弃整条消息**, 把"崩进程"降级成"丢一条"。
     * 拦法不需要改任何调用点: 生成的 deserialize 第一句就是
     * deserialize_data_cut(buffer.size()), 它把 _total_size 设成了缓冲长度。 */
    bool deserialize_ok() const noexcept { return !_deser_overflow; }

    /// 不解析负载, 只从段头取 msg_id。非 DZFlat 段返回 false。
    static bool dzflat_peek_msg_id(const void* seg, size_t size, uint32_t& out)
    {
        if (!dzflat::looks_like_dzflat(seg, size))
        {
            return false;
        }
        dzflat::SegHeader h{};
        std::memcpy(&h, seg, sizeof(h));
        out = h.msg_id;
        return true;
    }

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

    /* src 上 [at, at+len) 是否落在缓冲内。_total_size 在 deserialize 期间等于缓冲长度
     * (由 deserialize_data_cut 设置)。 */
    bool tods_in_range(uint32_t at, uint32_t len) const noexcept
    {
        return (at <= _total_size) && (len <= _total_size - at);
    }

    /* wire 上读来的"元素个数"在拿去 resize / reserve / 构造字符串之前必须先过这道闸。
     *
     * 与 adapt_memcpy_tods 的边界检查**不是重复**, 两者拦的是不同的东西:
     *   本函数           拦**分配**。resize 是先分配再拷, 所以它跑在那道检查之前 ——
     *                    实测把 count 篡改成 0x20000000 能让 vector<double> 提交 4.3GB
     *                    (resize 会值初始化, 是真写下去的), 内存紧张时抛 bad_alloc /
     *                    length_error, 而这条路径上无人捕获 → 进程 abort;
     *   adapt_memcpy_tods 拦**读取**, 保证不会读到映射之外。
     *
     * 判据取"count × 单元素最小字节数 ≤ 缓冲剩余长度"。这是必要条件而非精确界(跨页字段
     * 的物理跨度还要加上页尾字节, 精确校验仍由 adapt_memcpy_tods 逐段做), 但足以把荒谬
     * 的 count 挡在分配之前 —— 这里要的就是这个。 */
    bool tods_count_ok(uint32_t at, std::int64_t count, std::size_t min_elem_size) const noexcept
    {
        if (count < 0)
        {
            _deser_overflow = true;
            return false;
        }
        const std::uint64_t need =
            static_cast<std::uint64_t>(count) * (min_elem_size ? min_elem_size : 1);
        if (need > 0xFFFFFFFFull || !tods_in_range(at, static_cast<uint32_t>(need)))
        {
            _deser_overflow = true;
            return false;
        }
        return true;
    }

    void adapt_memcpy_tods(uint8_t* dst, const uint8_t* src, uint32_t& offset, const uint32_t local_data_len) const
    {
        /* 越界即止: 置位、把目标**清零**、一个字节都不从源拷。调用方据 deserialize_ok()
         * 丢整条消息。
         *
         * 为什么必须清零而不是原样返回: 生成的 deserialize 是
         *     int32_t n;                       // 未初始化
         *     adapt_memcpy_tods(&n, ...);      // 越界 → 不拷
         *     vec.resize(n);                   // ← 用的是未初始化的栈垃圾
         * 光"不拷"会把越界读换成**用未初始化值做无界分配**: resize(垃圾) 抛
         * std::length_error / bad_alloc, 而这条路径上没有任何人捕获 —— 进程直接 abort。
         * 清零之后 n 恒为 0, resize(0) / reserve(0) / std::string(0,'\0') 全都安全。
         *
         * 检查必须**逐段**做而不是只在入口算一次总跨度: 跨页字段的实际读取会在每段之后
         * 额外跨过 TAIL_MSG_SIZE 个页尾字节, 所以物理跨度大于 local_data_len, 入口处的
         * 单次检查拦不住最后几段。 */
        if (dst == nullptr)
        {
            _deser_overflow = true;
            return;
        }
        if (src == nullptr)
        {
            _deser_overflow = true;
            std::memset(dst, 0, local_data_len);
            return;
        }
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
            if (!tods_in_range(offset, copy_size))
            {
                /* 清掉还没写到的尾部, 让调用方读到的是确定的 0 而非栈垃圾。 */
                _deser_overflow = true;
                std::memset(dst + has_copy_size, 0, local_data_len - has_copy_size);
                return;
            }
            std::memcpy(dst + has_copy_size, src + offset, copy_size);

            offset += copy_size + TAIL_MSG_SIZE;   // 跨过数据长度外，还要跨过那 12 个尾部特征字节
            pure_data_offset += copy_size;
            has_copy_size += copy_size;
        }
        long remaining_size = local_data_len - has_copy_size;
        if (remaining_size > 0)
        {
            if (!tods_in_range(offset, static_cast<uint32_t>(remaining_size)))
            {
                _deser_overflow = true;
                std::memset(dst + has_copy_size, 0, static_cast<std::size_t>(remaining_size));
                return;
            }
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
        _deser_overflow = false;   /* 每次反序列化重新开始计 */
        /* 2 bytes for each cut use as page and complete flag */
        uint16_t total_page_cnt = static_cast<uint16_t>(total_data_len / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE)) + 1;
        this->_total_size = total_data_len;
        this->_total_page_cnt = total_page_cnt;
    }

    /* 物理偏移 -> 纯逻辑数据偏移(剥离开交错在流中的页尾字节)。仅当
     * physical_offset 落在数据位置(而非页尾内)时成立——生成的反序列化代码
     * 总是在字段边界调用。 */
    uint32_t to_pure_offset(uint32_t physical_offset) const
    {
        uint32_t passed_tails = physical_offset / (IPC_MSG_MAX_SIZE + TAIL_MSG_SIZE);
        return physical_offset - passed_tails * TAIL_MSG_SIZE;
    }

    /* 从物理偏移 physical_offset 起 len 字节是否在缓冲区内连续(没有被页尾
     * 打断, 且字段末尾不正好卡在页边界——卡边界会触发一条紧随其后的页
     * 尾, 别名路径无法跳过它)。 */
    bool region_is_contiguous(uint32_t physical_offset, uint32_t len) const
    {
        uint32_t pure = to_pure_offset(physical_offset);
        return pure % IPC_MSG_MAX_SIZE + len < IPC_MSG_MAX_SIZE;
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
    /* 本轮 deserialize 是否读越界过(见 deserialize_ok)。mutable: adapt_memcpy_tods
     * 是 const 成员, 而它是唯一能发现越界的地方。 */
    mutable bool _deser_overflow = false;
    mutable uint32_t dz_ipc_msg_id = 0;   // 消息类型标识符，可用于区分不同消息类型
};