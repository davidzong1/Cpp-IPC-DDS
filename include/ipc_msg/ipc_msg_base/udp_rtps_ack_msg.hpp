#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

class IpcRtpsNackMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsNackMsgId = 0x44'5A'4E'4B;   // "DZNK"
    static constexpr std::size_t kMaxMissingPages = 256;

    IpcRtpsNackMsg() { set_msg_id(kRtpsNackMsgId); }

    ~IpcRtpsNackMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t receiver_id{0};
    uint32_t sequence{0};
    std::vector<uint16_t> missing_pages;

    bool check_nk_id(const ipc::buffer& data) const { return check_id(data, kRtpsNackMsgId); }

    ipc::buffer serialize() override
    {
        const uint16_t miss_cnt = static_cast<uint16_t>(std::min(missing_pages.size(), kMaxMissingPages));
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(sequence) + sizeof(miss_cnt)
                                     + static_cast<uint32_t>(miss_cnt * sizeof(uint16_t));

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&receiver_id), page,
                         offset, sizeof(receiver_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&miss_cnt), page, offset,
                         sizeof(miss_cnt));

        for (uint16_t i = 0; i < miss_cnt; ++i)
        {
            const uint16_t page_idx = missing_pages[i];
            adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_idx), page,
                             offset, sizeof(page_idx));
        }

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;
        uint16_t miss_cnt = 0;

        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(sequence));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&miss_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(miss_cnt));

        const uint16_t clamp_cnt = static_cast<uint16_t>(std::min<std::size_t>(miss_cnt, kMaxMissingPages));
        missing_pages.clear();
        missing_pages.reserve(clamp_cnt);

        for (uint16_t i = 0; i < clamp_cnt; ++i)
        {
            uint16_t page_idx = 0;
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_idx), static_cast<const uint8_t*>(buffer.data()), offset,
                              sizeof(page_idx));
            missing_pages.push_back(page_idx);
        }
    }

    IpcRtpsNackMsg* clone() const override { return new IpcRtpsNackMsg(*this); }
};

/* NACK 的位图编码变体。
 *
 * ---- 为什么需要 ----
 * IpcRtpsNackMsg 每片 2 字节显式列举, 且硬上限 256 片。而 §7.1 实测的丢包形态是
 * "缓冲溢出型突发", 单个空洞就长达 255 片 —— 恰好顶满上限。1 MB = 713 片的消息
 * 若丢了 400 片, 显式列表一轮只能报前 256 片, 剩下的要等下一轮, 每轮都要付一个
 * round_wait_ms(713 片时 200 ms)。
 *
 * 位图把"报 N 片"的成本从 2N 字节降到 span/8 字节:
 *   255 片连续空洞: 510 B -> 32 B
 *   713 片全缺:     显式列表只能报前 256 片(512 B, 剩余 457 片要等下一轮),
 *                   位图 90 B 一帧报完
 *
 * ---- 为什么用独立 msg_id 而不是在 DZNK 里加一个格式位 ----
 * 旧版发送端的 IpcRtpsNackMsg::deserialize 会把格式位当成 miss_cnt 的高字节, 读出
 * 一个巨大的数, clamp 到 256 之后仍会从一个只有几十字节的 buffer 里读 512 字节 ——
 * adapt_memcpy_tods 不做边界检查, 那是一次真实的越界读。
 * 独立 msg_id 让旧版发送端的 check_id 直接失配, 安全地忽略掉整帧, 退回"没收到
 * NACK"的超时路径。代价只是它这一轮不重传, 不会内存越界。
 *
 * 因此新接收端**不能**无条件发这个格式: 对方是旧版时发了等于没发, Reliable 会从
 * "能重传"退化成"必然超时"。能力协商走 Heartbeat 的 kFlagBitmapNack 位, 收不到
 * 该标志就老老实实用显式列表。
 *
 * ---- 位语义 ----
 * bit i (i 从 0 起) 置 1 表示页号 base_page + i 缺失, 与 RTPS SequenceNumberSet
 * 的 "bit set = requested" 一致。base_page 是**第一个缺失的**页号, 不是最后一个
 * 收到的 —— 这是 RTPS 规范 8.3.5.5 最容易搞反的地方, 这里沿用同一约定。 */
class IpcRtpsNackBitmapMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsNackBitmapMsgId = 0x44'5A'4E'42;   // "DZNB"

    /* 单页上限。IpcMsgBase 的 correct_total_size 在 total_data_len >= 1460 时就会
     * 切成两页, 而分片后的 NACK 到达发送端会被逐个 datagram 处理, 每片都过
     * check_id(尾部 msg_id 相同, 都能通过)然后 deserialize 出半截垃圾。
     * 所以这个上限是正确性要求, 不是优化。 */
    static constexpr std::size_t kMaxWireBytes = 1'459;
    static constexpr std::size_t kFixedFieldBytes = 2 + 4 + 4 + 4 + 4 + 2 + 2;   // 22
    static constexpr std::size_t kMaxBitmapBytes = kMaxWireBytes - kFixedFieldBytes;   // 1437
    static constexpr std::size_t kMaxBitmapBits = kMaxBitmapBytes * 8;                 // 11496

    IpcRtpsNackBitmapMsg() { set_msg_id(kRtpsNackBitmapMsgId); }

    ~IpcRtpsNackBitmapMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t receiver_id{0};
    uint32_t sequence{0};
    uint16_t base_page{0};   // bit 0 对应的页号
    /* 有效位数。**线上存的是位图字节数, 不是这个值** —— 接收端按 map_bytes * 8
     * 重建, 所以往返之后 bit_cnt 会向上取整到字节边界(300 -> 304)。多出来的填充位
     * 恒为 0, is_missing 返回 false, 不影响语义。存字节数而非位数是因为反序列化
     * 必须先知道要读多少字节才能做边界夹取。 */
    uint16_t bit_cnt{0};
    std::vector<uint8_t> bitmap;

    bool check_nb_id(const ipc::buffer& data) const { return check_id(data, kRtpsNackBitmapMsgId); }

    /* 开一个覆盖 [base, base + bits) 的空窗口。bits 超过单页容量时截断 —— 剩下的
     * 缺片由下一轮 NACK 补报, 这与显式列表撞上 256 上限时的行为一致。 */
    void reset_window(uint16_t base, std::size_t bits)
    {
        base_page = base;
        bit_cnt = static_cast<uint16_t>(std::min<std::size_t>(bits, kMaxBitmapBits));
        bitmap.assign((static_cast<std::size_t>(bit_cnt) + 7) / 8, 0);
    }

    void set_missing(uint16_t page)
    {
        if (page < base_page)
        {
            return;
        }
        const std::size_t idx = static_cast<std::size_t>(page) - base_page;
        if (idx >= bit_cnt)
        {
            return;
        }
        bitmap[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7));
    }

    bool is_missing(uint16_t page) const
    {
        if (page < base_page)
        {
            return false;
        }
        const std::size_t idx = static_cast<std::size_t>(page) - base_page;
        if (idx >= bit_cnt || (idx >> 3) >= bitmap.size())
        {
            return false;
        }
        return (bitmap[idx >> 3] & static_cast<uint8_t>(1u << (idx & 7))) != 0;
    }

    ipc::buffer serialize() override
    {
        const uint16_t bits = static_cast<uint16_t>(std::min<std::size_t>(bit_cnt, kMaxBitmapBits));
        const std::size_t want_bytes = (static_cast<std::size_t>(bits) + 7) / 8;
        const uint16_t map_bytes = static_cast<uint16_t>(std::min(want_bytes, bitmap.size()));

        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(sequence) + sizeof(base_page) + sizeof(bit_cnt) + map_bytes;

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&receiver_id), page,
                         offset, sizeof(receiver_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&base_page), page, offset,
                         sizeof(base_page));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&map_bytes), page, offset,
                         sizeof(map_bytes));
        if (map_bytes > 0)
        {
            adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), bitmap.data(), page, offset, map_bytes);
        }

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        /* 与 IpcRtpsAckMsg 不同, 这是新增帧, 不存在需要兼容的旧格式 —— 长度不足
         * 就是不认识的东西, 整帧拒绝好过读出半截垃圾字段。 */
        if (buffer.size() < kFixedFieldBytes + 12)
        {
            return;
        }

        uint32_t offset = 0;
        uint16_t map_bytes = 0;

        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(sequence));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&base_page), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(base_page));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&map_bytes), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(map_bytes));

        /* 位图长度取自线上字段, 必须夹在实际可读字节内。adapt_memcpy_tods 不做
         * 边界检查, 少了这一步, 一个声称 map_bytes=60000 的畸形帧就能读穿堆。 */
        const std::size_t avail = buffer.size() - kFixedFieldBytes - 12;
        const std::size_t take = std::min<std::size_t>(std::min<std::size_t>(map_bytes, avail), kMaxBitmapBytes);

        bitmap.assign(take, 0);
        if (take > 0)
        {
            adapt_memcpy_tods(bitmap.data(), static_cast<const uint8_t*>(buffer.data()), offset,
                              static_cast<uint32_t>(take));
        }
        bit_cnt = static_cast<uint16_t>(take * 8);
    }

    IpcRtpsNackBitmapMsg* clone() const override { return new IpcRtpsNackBitmapMsg(*this); }
};

class IpcRtpsAckMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsAckMsgId = 0x44'5A'41'4B;   // "DZAK"

    IpcRtpsAckMsg() { set_msg_id(kRtpsAckMsgId); }

    ~IpcRtpsAckMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t receiver_id{0};
    uint32_t sequence{0};
    uint8_t integrity_flags{0};
    uint32_t payload_crc32c{0};

    bool check_ak_id(const ipc::buffer& data) const { return check_id(data, kRtpsAckMsgId); }

    ipc::buffer serialize() override
    {
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(receiver_id)
                                     + sizeof(sequence) + sizeof(integrity_flags) + sizeof(payload_crc32c);

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&receiver_id), page,
                         offset, sizeof(receiver_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&integrity_flags), page,
                         offset, sizeof(integrity_flags));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&payload_crc32c), page,
                         offset, sizeof(payload_crc32c));

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        uint32_t offset = 0;

        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&receiver_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(receiver_id));

        // Sequence field: read if buffer large enough; back-compat with old-format ACK.
        const std::size_t min_size_for_seq = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id)
                                             + sizeof(receiver_id) + sizeof(sequence) + sizeof(integrity_flags)
                                             + sizeof(payload_crc32c) + 12;
        if (buffer.size() >= min_size_for_seq)
        {
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                              sizeof(sequence));
        }

        // Integrity fields: back-compat with old-format ACK that lacks integrity_flags + payload_crc32c.
        const std::size_t min_size_for_crc = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id)
                                             + sizeof(receiver_id) + sizeof(sequence) + sizeof(integrity_flags)
                                             + sizeof(payload_crc32c) + 12;
        if (buffer.size() >= min_size_for_crc)
        {
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&integrity_flags), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(integrity_flags));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&payload_crc32c), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(payload_crc32c));
        }
        else if (buffer.size() >= min_size_for_seq + sizeof(integrity_flags) + sizeof(payload_crc32c))
        {
            // ACK without sequence but with integrity: skip sequence, read integrity.
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&integrity_flags), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(integrity_flags));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&payload_crc32c), static_cast<const uint8_t*>(buffer.data()),
                              offset, sizeof(payload_crc32c));
        }
    }

    IpcRtpsAckMsg* clone() const override { return new IpcRtpsAckMsg(*this); }
};

/* Heartbeat —— 发送端发完分片后在**数据通道**上的主动通告。
 *
 * 两个作用, 第二个是硬需求而非优化:
 *
 * 1) 让接收端立刻知道权威分片总数, 从而把"靠超时猜"换成"按通告判":
 *    收全 -> 立即 ACK; 缺片且对端 Reliable -> 立即 NACK; 缺片且对端 BestEffort
 *    -> 立即放弃。改动前接收端要空等 round_wait_ms(20-200ms) × 若干轮。
 *
 * 2) **携带 sequence**。12 字节 tail 里没有 sequence 字段, 接收端在
 *    wait_first_data_chunk 里构造 chunk_meta 时只能填 {page_cnt, total_size,
 *    msg_id}, sequence 恒为 0; 而发送端的 meta.sequence 来自逐条自增的计数器。
 *    于是 chunk_send_ex 的 `ack_msg.sequence == meta.sequence` 校验只有进程发出
 *    的**第一条**消息(计数器恰为 0)能通过, 之后永远对不上。
 *    Heartbeat 是线格式里唯一能把 sequence 送到接收端的载体 —— 没有它, 光做
 *    端点分离也修不好 Reliable。
 *
 * 载荷 21 B + 12 B tail = 33 B, 恒为单页, 不会自身分片。 */
class IpcRtpsHeartbeatMsg : public IpcMsgBase
{
public:
    static constexpr uint32_t kRtpsHeartbeatMsgId = 0x44'5A'48'42;   // "DZHB"

    /* flags */
    static constexpr uint8_t kFlagReliable = 0x01;   // 发送端会响应 NACK 重传
    static constexpr uint8_t kFlagFinal = 0x02;      // 发送端不会再重传(发完/已放弃)
    /* 发送端认识 DZNB(位图 NACK)。接收端只有看到这一位才可以发位图格式 ——
     * 旧版发送端的 check_id 会直接丢弃 DZNB, 那一轮就等于没发 NACK。 */
    static constexpr uint8_t kFlagBitmapNack = 0x04;

    static constexpr std::size_t kWireSize = 2 + 4 + 4 + 4 + 4 + 1 + 2 + 12;

    IpcRtpsHeartbeatMsg() { set_msg_id(kRtpsHeartbeatMsgId); }

    ~IpcRtpsHeartbeatMsg() = default;

    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t data_msg_id{0};
    uint32_t sender_id{0};
    uint32_t sequence{0};
    uint8_t flags{0};
    uint16_t round{0};   // 第几次通告, 0 = 数据发送前的前导

    bool check_hb_id(const ipc::buffer& data) const { return check_id(data, kRtpsHeartbeatMsgId); }

    ipc::buffer serialize() override
    {
        const uint32_t total_size_ = sizeof(page_cnt) + sizeof(total_size) + sizeof(data_msg_id) + sizeof(sender_id)
                                     + sizeof(sequence) + sizeof(flags) + sizeof(round);

        ipc::buffer data = serialize_data_cut(total_size_);
        uint32_t offset = 0;
        uint16_t page = 1;

        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&page_cnt), page, offset,
                         sizeof(page_cnt));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&total_size), page,
                         offset, sizeof(total_size));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&data_msg_id), page,
                         offset, sizeof(data_msg_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sender_id), page, offset,
                         sizeof(sender_id));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&sequence), page, offset,
                         sizeof(sequence));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&flags), page, offset,
                         sizeof(flags));
        adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&round), page, offset,
                         sizeof(round));

        add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    void deserialize(const ipc::buffer& buffer) override
    {
        /* 新消息, 不需要 IpcRtpsAckMsg 那样的向后兼容尺寸试探: 长度不对就是不认识
         * 的东西, 直接拒绝, 避免读出垃圾字段。 */
        if (buffer.size() != kWireSize)
        {
            return;
        }

        uint32_t offset = 0;
        deserialize_data_cut(uint32_t(buffer.size()));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&page_cnt), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(page_cnt));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&total_size), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(total_size));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&data_msg_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(data_msg_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sender_id), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(sender_id));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&sequence), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(sequence));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&flags), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(flags));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&round), static_cast<const uint8_t*>(buffer.data()), offset,
                          sizeof(round));
    }

    IpcRtpsHeartbeatMsg* clone() const override { return new IpcRtpsHeartbeatMsg(*this); }
};

/* dzIPC 的控制帧与数据帧共用数据通道, 靠 msg_id 区分。0x445A**** ("DZ..")
 * 这一段保留给控制帧, 用户消息不得占用。
 *
 * 数据接收路径靠 msg_ptr->check_id() 天然过滤掉它们, 但**不按 msg_id 过滤的**
 * chunk_rev_sniff 必须显式调用本函数排除, 否则 33 字节的 heartbeat 会被当成
 * 一条合法的单页消息。 */
inline bool is_rtps_control_frame(uint32_t msg_id)
{
    return msg_id == IpcRtpsHeartbeatMsg::kRtpsHeartbeatMsgId || msg_id == IpcRtpsAckMsg::kRtpsAckMsgId
           || msg_id == IpcRtpsNackMsg::kRtpsNackMsgId || msg_id == IpcRtpsNackBitmapMsg::kRtpsNackBitmapMsgId;
}
