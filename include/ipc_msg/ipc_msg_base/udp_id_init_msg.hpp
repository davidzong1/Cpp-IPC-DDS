#pragma once
/* ⛔ 本行是 2026-09 补上的: 本文件是本目录里**唯一**没有 include guard 的头
 * (其它三个都是 #pragma once)。它过去只被"每个 TU 恰好包含一次"侥幸掩盖 ——
 * 直到 exec/dzipc_topic_cat/include/handshake_probe.h 与测试同时包含它, 同一个
 * TU 里出现两次定义, 立刻报 redefinition of class IpcPubSubIdInitMsg。
 * 加 guard 是纯修复: 不改任何字段/布局/ABI。 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

class IpcPubSubIdInitMsg : public IpcMsgBase
{
public:
    /* 构造函数和析构函数 */
    IpcPubSubIdInitMsg() = default;
    ~IpcPubSubIdInitMsg() = default;
    bool host_flag{false};
    bool cli_flag{false};
    bool run_status{true};

    /* ---- ser-cli 路径切换的裁定状态(T2 §3 D2) ----
     *
     * 为什么加在第 4 个载荷字节: 前 3 个字节是既有语义, 偏移不动, 所以在一次
     * 灰度期里"新进程发旧进程"与"旧进程发新进程"两个方向都不会错位 —— 旧解码器
     * 只读 [0..2], 多出来的第 4 字节它根本不看; 新解码器遇到旧帧(纯载荷 3 字节)
     * 时保留默认值 Unknown。Unknown 的含义就是"按 socket 走", 也就是**默认值本身
     * 就是安全方向**(T2 §3 D2 要求的向后兼容正是这一条)。
     *
     * ⛔ 不能用 check_host/check_cli/check_run_status 的裸 data_ptr 访问方式读它:
     * 那三个函数是给"数据一定够长"的调用点用的, 旧帧没有第 4 个载荷字节。
     * 判别方法见 has_path_state —— **不能**用 buffer.size()。 */
    enum class PathState : uint8_t {
        Unknown = 0,   // 旧版本对端 / 尚未提议 ⇒ 双方都留在 socket
        ProposeShm = 1,
        ConfirmShm = 2,
        ConfirmSocket = 3,
        WithdrawToSocket = 4
    };
    PathState path_state{PathState::Unknown};

    bool check_host(ipc::buffer& raw_data)
    {
        uint8_t* data_ptr = reinterpret_cast<uint8_t*>(raw_data.data());
        return data_ptr[0] == 1;
    }

    bool check_cli(ipc::buffer& raw_data)
    {
        uint8_t* data_ptr = reinterpret_cast<uint8_t*>(raw_data.data());
        return data_ptr[1] == 1;
    }

    bool check_run_status(ipc::buffer& raw_data)
    {
        uint8_t* data_ptr = reinterpret_cast<uint8_t*>(raw_data.data());
        return data_ptr[2] == 1;
    }

    /* 帧里是否带了 path_state 字段(旧帧为 false)。
     *
     * ⛔ 判据**不能**用 raw_data.size() >= 4: 序列化会在载荷后再追加 12 字节页尾,
     * 所以老版本那 3 字节载荷发出来总共是 15 字节, size() 恒 >= 4 —— 这样判会让
     * 新解码器去读第 4 个字节, 而那里其实是**页尾的第一个字节**, 读出来是任意值。
     * 也就是说"向后兼容"会变成一个假声明: 旧进程的帧会被解成一个随机的 path_state。
     *
     * 正确判据是"纯载荷长度": 尾部 12 字节里的 total_size 是整个缓冲长度(含页尾),
     * 减去 page_cnt*12 才是载荷。3 字节的老帧 = 15-12 = 3, 新帧 = 16-12 = 4。
     * 判 >= 4 等价于"这个帧里确实写了第 4 个字段"。 */
    static uint32_t pure_payload_size(const ipc::buffer& raw_data)
    {
        if (raw_data.size() < 12)
        {
            return 0;
        }
        const auto* p = static_cast<const uint8_t*>(raw_data.data());
        const size_t n = raw_data.size();
        const uint32_t page_cnt = (static_cast<uint32_t>(p[n - 12]) << 8) | static_cast<uint32_t>(p[n - 11]);
        const uint32_t total_size = (static_cast<uint32_t>(p[n - 8]) << 24) | (static_cast<uint32_t>(p[n - 7]) << 16)
                                    | (static_cast<uint32_t>(p[n - 6]) << 8) | static_cast<uint32_t>(p[n - 5]);
        const uint32_t tails = page_cnt * 12u;
        if (total_size < tails || total_size != static_cast<uint32_t>(n))
        {
            return 0;   // 尾部自相矛盾 => 按"没有新字段"处理(安全方向)
        }
        return total_size - tails;
    }
    static bool has_path_state(const ipc::buffer& raw_data) { return pure_payload_size(raw_data) >= 4; }
    static PathState peek_path_state(const ipc::buffer& raw_data)
    {
        if (!has_path_state(raw_data))
        {
            return PathState::Unknown;
        }
        return static_cast<PathState>(reinterpret_cast<const uint8_t*>(raw_data.data())[3]);
    }

    /* 序列化函数 */
    ipc::buffer serialize() override
    {
        size_t total_size_ = sizeof(bool) * 4;
        ipc::buffer data = std::move(this->serialize_data_cut(total_size_));
        uint32_t offset = 0;
        uint16_t page = 1;
        uint8_t host_flag_cache = host_flag ? 1 : 0;
        uint8_t cli_flag_cache = cli_flag ? 1 : 0;
        uint8_t run_status_cache = run_status ? 1 : 0;
        uint8_t path_state_cache = static_cast<uint8_t>(path_state);
        this->adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&host_flag_cache),
                               page, offset, 1);
        this->adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&cli_flag_cache),
                               page, offset, 1);
        this->adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&run_status_cache),
                               page, offset, 1);
        this->adapt_memcpy_tos(static_cast<uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(&path_state_cache),
                               page, offset, 1);
        this->add_tail_msg(static_cast<uint8_t*>(data.data()) + offset, page);
        return data;
    }

    /* 反序列化函数 */
    void deserialize(const ipc::buffer& buffer) override
    {
        /* 必须先告诉基类"缓冲有多长": adapt_memcpy_tods 的越界判据用的就是
         * _total_size, 而它的默认值是 0 —— 不设的话每个字段都会被判越界、清零,
         * deserialize 会静默地什么都不解出来。生成的 deserialize 第一句就是这句,
         * 这里的手写实现原先漏了它(该消息在真实路径上只走 check_* 裸访问, 所以
         * 一直没暴露)。 */
        this->deserialize_data_cut(static_cast<uint32_t>(buffer.size()));
        uint32_t offset = 0;
        uint8_t host_flag_cache;
        uint8_t cli_flag_cache;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&host_flag_cache),
                                static_cast<const uint8_t*>(buffer.data()), offset, 1);
        host_flag = host_flag_cache == 1 ? true : false;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&cli_flag_cache), static_cast<const uint8_t*>(buffer.data()),
                                offset, 1);
        cli_flag = cli_flag_cache == 1 ? true : false;
        uint8_t run_status_cache;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&run_status_cache),
                                static_cast<const uint8_t*>(buffer.data()), offset, 1);
        run_status = run_status_cache == 1 ? true : false;
        /* 旧帧(3 字节)保持 Unknown: 不是错误, 是 D2 约定的安全默认。 */
        if (!has_path_state(buffer))
        {
            path_state = PathState::Unknown;
            return;
        }
        uint8_t path_state_cache;
        this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&path_state_cache),
                                static_cast<const uint8_t*>(buffer.data()), offset, 1);
        path_state = static_cast<PathState>(path_state_cache);
    }

    IpcPubSubIdInitMsg* clone() const override { return new IpcPubSubIdInitMsg(*this); }
};