#include "socket_sniffer.h"
#include <string>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"

namespace dzIPC {

socket_sniffer::~socket_sniffer()
{
    stop_.store(true, std::memory_order_release);
    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }
}

void socket_sniffer::create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id)
{
    stop_.store(false, std::memory_order_release);
    this->ser_or_topic_ = ser_or_topic;
    this->msg_id_ = msg_id;
    std::string ip_hash = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    uint16_t port_hash = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    req_ = std::make_unique<ipc::socket::UDPNode>();
    req_->create(topic_name.c_str(), ip_hash.c_str(), port_hash);
    while (!req_->connect() && !stop_.load(std::memory_order_acquire))
    {
        std::fprintf(stderr, "\033[31m[%s sniffer] failed to connect req socket on %s:%u, retry in 1s...\033[0m\n",
                     topic_name.c_str(), ip_hash.c_str(), static_cast<unsigned>(port_hash));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (ser_or_topic_)
    {
        res_ = std::make_unique<ipc::socket::UDPNode>();
        res_->create(topic_name.c_str(), ip_hash.c_str(), port_hash + 1);
        while (!res_->connect() && !stop_.load(std::memory_order_acquire))
        {
            std::fprintf(stderr, "\033[31m[%s sniffer] failed to connect res socket on %s:%u, retry in 1s...\033[0m\n",
                         topic_name.c_str(), ip_hash.c_str(), static_cast<unsigned>(port_hash + 1));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    /* 可选: 在 ser-cli 握手通道(base+2)上再挂一条**只读**监听。
     *
     * ⛔ 只对 ser-cli 开 —— pub/sub 根本没有这条通道(整个仓库只有 socket_ser_ipc::
     *    server_handshake / socket_cli_ipc 建 +2), 开在那里只会白占一个端口。
     * ⛔ 只试一次、不重试: 现有的 req_/res_ 挂了会每 1 秒红字重试, 那是**必需**通道;
     *    观测是**附加**能力, 拿不到就降级, 绝不能让嗅探器本身不可用。 */
    if (watch_handshake_ && ser_or_topic_)
    {
        if (hs_probe_.open(topic_name, domain_id))
        {
            std::fprintf(stdout, "\033[32m[%s sniffer] handshake watch opened (read-only) on %s:%u\033[0m\n",
                         topic_name.c_str(), ip_hash.c_str(), static_cast<unsigned>(hs_probe_.snapshot().port));
        }
        else
        {
            std::fprintf(stderr, "\033[33m[%s sniffer] handshake watch unavailable, continuing without it\033[0m\n",
                         topic_name.c_str());
        }
    }
    ready.store(true, std::memory_order_release);
    recv_thread_ = std::thread(
        [this]()
        {
            while (!stop_.load(std::memory_order_acquire))
            {
                /* 握手观测先排空: 它用的是 receive_nowait(不阻塞), 且每轮有上限,
                 * 所以**不会**给下面的数据面接收加任何等待时间。 */
                hs_probe_.poll();
                sniffer_info got = recv_inner(50);
                // Only refresh the cache when we actually received something, so a
                // timeout in this iteration doesn't clobber a payload that the
                // consumer hasn't picked up yet.
                if (got.request.size() > 0 || got.response.size() > 0)
                {
                    auto msg_cache_cache = std::make_unique<sniffer_info>(std::move(got));
                    std::lock_guard<std::mutex> lock(msg_mutex);
                    msg_cache = std::move(msg_cache_cache);
                }
                std::this_thread::yield();
            }
        });
}

bool socket_sniffer::check_msg_id(const ipc::buffer& buf) const
{
    if (buf.size() < 12)
    {
        return false;
    }
    const uint8_t* id = reinterpret_cast<const uint8_t*>(buf.data()) + buf.size() - 4;
    uint32_t msg_id = (static_cast<uint32_t>(id[0]) << 24) | (static_cast<uint32_t>(id[1]) << 16)
                      | (static_cast<uint32_t>(id[2]) << 8) | (static_cast<uint32_t>(id[3]));
    return msg_id == this->msg_id_;
}

sniffer_info socket_sniffer::try_recv() noexcept
{
    if (!ready)
    {
        return sniffer_info{ipc::buffer{}, ipc::buffer{}};
    }
    std::shared_ptr<sniffer_info> info = nullptr;
    {
        std::lock_guard<std::mutex> lock(msg_mutex);
        info = std::move(msg_cache);
    }
    sniffer_info out = info ? std::move(*info) : sniffer_info{ipc::buffer{}, ipc::buffer{}};
    /* 观测字段**每次都现场取**, 不跟着 msg_cache 走。
     *
     * 原因正是这条通道存在的理由: 切到 SHM 之后 socket 数据面被双侧停掉, msg_cache
     * 永远为空 —— 若把观测挂在缓存上, 恰好在最需要看它的场景里(切换之后)什么都看不到。
     * 取快照在探针内部加了锁并吞掉异常, 所以本函数仍是 noexcept 安全的。 */
    out.hs = hs_probe_.snapshot();
    return out;
}

sniffer_info socket_sniffer::recv_inner(std::uint64_t timeout_ms) noexcept
{
    if (ser_or_topic_)
    {
        ipc::buffer req_cache, res_cache;
        try
        {
            req_cache = dzIPC::socket::chunk_rev_sniff(*req_, timeout_ms);
            res_cache = dzIPC::socket::chunk_rev_sniff(*res_, timeout_ms);
            if (!check_msg_id(req_cache))
            {
                req_cache = ipc::buffer{};
            }
            if (!check_msg_id(res_cache))
            {
                res_cache = ipc::buffer{};
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
        return sniffer_info{std::move(req_cache), std::move(res_cache)};
    }
    else
    {
        try
        {
            ipc::buffer req_cache = dzIPC::socket::chunk_rev_sniff(*req_, timeout_ms);
            if (!check_msg_id(req_cache))
            {
                req_cache = ipc::buffer{};
            }
            return sniffer_info{std::move(req_cache), ipc::buffer{}};
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
    }
}
}   // namespace dzIPC
